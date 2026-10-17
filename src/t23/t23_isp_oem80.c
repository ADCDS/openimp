/* SPDX-License-Identifier: MIT */
#include "t23_isp_oem80.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

typedef struct { uint32_t enable, selector; } SensorInput;
typedef struct { uint32_t selector, phys, size; } SensorBuffer;
typedef struct {
    uint32_t index;
    char name[32];
    uint32_t type;
    unsigned char reserved[40];
} SensorEnum;
_Static_assert(sizeof(SensorInput) == 8, "OEM sensor-input payload");
_Static_assert(sizeof(SensorBuffer) == 12, "OEM ISP-buffer payload");
_Static_assert(sizeof(SensorEnum) == 80, "OEM input-enumeration payload");

static int selector(const void *record)
{
    uint16_t id;
    if (!record) { errno = EINVAL; return -1; }
    memcpy(&id, (const unsigned char *)record + 32, sizeof(id));
    if (id >= 2 || !memchr(record, 0, 32)) { errno = EINVAL; return -1; }
    return id;
}

static int input(int fd, unsigned int id, unsigned int enable)
{
    SensorInput value = {enable, id};
    /* The command advertises four bytes but this driver copies eight. */
    return ioctl(fd, 0xc0045627ul, &value);
}

static int enumerate_inputs(int fd, const char *name)
{
    int found = 0;
    /* Enumeration assigns runtime sensor indices in this OEM module. Replay
     * the complete traversal, including its terminal EINVAL, before S_INPUT.
     * Only two physical slots belong to this profile; bound broken drivers. */
    for (uint32_t index = 0; index <= 2; ++index) {
        SensorEnum value = {0};
        value.index = index;
        if (ioctl(fd, 0xc050561aul, &value) != 0) {
            if (errno != EINVAL) return -1;
            if (found) return 0;
            errno = ENOENT; return -1;
        }
        if (index == 2) { errno = EOVERFLOW; return -1; }
        if (value.index != index || !memchr(value.name, 0, sizeof(value.name))) {
            errno = EPROTO; return -1;
        }
        if (!strcmp(value.name, name)) found = 1;
    }
    errno = EOVERFLOW; return -1;
}

static int release_dma(IMPDMABufferInfo **buffer)
{
    if (!*buffer) return 0;
    if (DMA_FreePhys((*buffer)->phys_addr) != 0) return -1;
    free(*buffer); *buffer = NULL;
    return 0;
}

int openimp_t23_oem80_isp_close(OpenIMPT23IspOem80 *state)
{
    if (!state) { errno = EINVAL; return -1; }
    if (state->registered) { errno = EBUSY; return -1; }
    for (unsigned int id = 0; id < 2; ++id) {
        if (release_dma(&state->wdr[id]) || release_dma(&state->ncu[id]))
            return -1;
    }
    return 0;
}

int openimp_t23_oem80_sensor_del(OpenIMPT23IspOem80 *state, int fd,
                                const void *record84)
{
    int id = selector(record84);
    if (!state || id < 0) { errno = EINVAL; return -1; }
    if (!(state->registered & (1u << id)) && !state->ncu[id] && !state->wdr[id]) {
        errno = ENOENT;
        return -1;
    }
    if (state->registered & (1u << id)) {
        if (input(fd, (unsigned int)id, 0) != 0 ||
            ioctl(fd, 0x805456c2ul, state->record[id]) != 0)
            return -1;
        state->registered &= ~(1u << id);
    }
    /* Never free memory while the driver may still reference this sensor. */
    if (release_dma(&state->wdr[id]) || release_dma(&state->ncu[id]))
        return -1;
    memset(state->record[id], 0, sizeof(state->record[id]));
    return 0;
}

static int allocate_buffer(int fd, unsigned int id, int wdr,
                            IMPDMABufferInfo **out)
{
    SensorBuffer buffer = {id, 0, 0};
    IMPDMABufferInfo *dma;
    unsigned long get = wdr ? 0x800c56d7ul : 0x800c56d5ul;
    unsigned long set = wdr ? 0x800c56d6ul : 0x800c56d4ul;

    if (ioctl(fd, get, &buffer) != 0) return -1;
    if (!buffer.size && wdr) return 0;
    if (!buffer.size || buffer.size > INT_MAX) { errno = EOVERFLOW; return -1; }
    dma = calloc(1, sizeof(*dma));
    if (!dma) return -1;
    if (DMA_AllocDescriptor(dma, (int)buffer.size, wdr ? "wdrbuf" : "ncubuf") != 0) {
        free(dma);
        return -1;
    }
    *out = dma;
    buffer.selector = id;
    buffer.phys = dma->phys_addr;
    return ioctl(fd, set, &buffer);
}

int openimp_t23_oem80_sensor_add(OpenIMPT23IspOem80 *state, int fd,
                                const void *record84, const char *path128,
                                int wdr)
{
    int id = selector(record84);
    int saved_errno;
    if (!state || id < 0) { errno = EINVAL; return -1; }
    if ((state->registered & (1u << id)) || state->ncu[id] || state->wdr[id]) {
        errno = EBUSY;
        return -1;
    }
    if (ioctl(fd, 0x805456c1ul, record84) != 0) return -1;
    state->registered |= 1u << id;
    memcpy(state->record[id], record84, 84);
    /* The default-path command advertises four bytes but copies 128. Its
     * internal path-slot semantics are not inferred here. Caller zero-pads. */
    if ((path128 && ioctl(fd, 0xc00456c7ul, path128) != 0) ||
        enumerate_inputs(fd, record84) != 0 ||
        input(fd, (unsigned int)id, 1) != 0 ||
        allocate_buffer(fd, (unsigned int)id, 0, &state->ncu[id]) != 0 ||
        (wdr && allocate_buffer(fd, (unsigned int)id, 1, &state->wdr[id]) != 0)) {
        saved_errno = errno ? errno : EIO;
        /* A failed rollback deliberately retains registration and DMA state.
         * Close will refuse it; a later DeleteSensor can retry cleanup. */
        (void)openimp_t23_oem80_sensor_del(state, fd, record84);
        errno = saved_errno;
        return -1;
    }
    return 0;
}
