/* SPDX-License-Identifier: MIT */
#include "t23/t23_isp_oem80.h"
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, ioctls, allocated, freed, kernel_registered;
static int failing_stage, fail_delete, fail_dma_free, stage;
static unsigned int active_id, runtime_index[2], activated, enumerations;
static int bad_enum;
static struct { uint32_t phys; unsigned int owner; } owners[64];
static uint32_t next_phys = 0x02a90000;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint16_t id_of(const unsigned char *record)
{ uint16_t id; memcpy(&id, record + 32, 2); return id; }

int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    CHECK(size == 4096 || size == 8192);
    CHECK(!strcmp(tag, "ncubuf") || !strcmp(tag, "wdrbuf"));
    info->phys_addr = next_phys; info->size = (uint32_t)size;
    CHECK(allocated < 64);
    owners[allocated].phys = info->phys_addr; owners[allocated].owner = active_id;
    next_phys += 0x10000; ++allocated;
    return 0;
}

int DMA_FreePhys(uint32_t phys)
{
    CHECK(phys >= 0x02a90000);
    unsigned int i;
    for (i = 0; i < allocated && owners[i].phys != phys; ++i) {}
    CHECK(i < allocated);
    /* The other sensor may remain active; this buffer's owner must be gone. */
    CHECK(!(kernel_registered & (1u << owners[i].owner)));
    if (fail_dma_free) { errno = EIO; return -1; }
    owners[i].phys = 0;
    ++freed;
    return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args; unsigned char *record; uint32_t *words;
    ++ioctls; CHECK(fd == 19);
    va_start(args, request); record = va_arg(args, unsigned char *); va_end(args);
    words = (uint32_t *)record;
    if (request == 0x805456c2ul) {
        CHECK(id_of(record) < 2);
        if (fail_delete) { errno = EBUSY; return -1; }
        kernel_registered &= ~(1u << id_of(record));
        activated &= ~(1u << id_of(record));
        return 0;
    }
    if (request == 0xc0045627ul && words[0] == 0) {
        CHECK(words[1] < 2);
        return 0;
    }
    ++stage;
    if (stage == failing_stage) { errno = EIO; return -1; }
    switch (request) {
    case 0x805456c1ul:
        CHECK(!memcmp(record, "cv2005", 6));
        CHECK(id_of(record) < 2);
        CHECK(words[9] == 1 && words[16] == 0);
        CHECK(words[20] == 123456); /* tail is deliberately not the selector */
        kernel_registered |= 1u << id_of(record);
        runtime_index[id_of(record)] = 0; /* sensor object starts zeroed */
        return 0;
    case 0xc050561aul: {
        uint32_t requested = words[0];
        CHECK(requested <= 2);
        for (unsigned int i = 1; i < 20; ++i) CHECK(words[i] == 0);
        ++enumerations;
        for (unsigned int id = 0; id < 2; ++id) {
            if (!(kernel_registered & (1u << id))) continue;
            runtime_index[id] = id; /* real enum side effect in VIN mode 1 */
            if (requested != runtime_index[id]) continue;
            strcpy((char *)record + 4, id ? "cv2005s1" : "cv2005");
            words[9] = 2;
            if (bad_enum == 1) strcpy((char *)record + 4, "missing-sensor");
            if (bad_enum == 2) memset(record + 4, 'X', 32);
            if (bad_enum == 3) words[0] = 100;
            return 0;
        }
        if (bad_enum == 4) return 0;
        errno = EINVAL; return -1;
    }
    case 0xc00456c7ul:
        CHECK(!strcmp((const char *)record, "/tmp/iq.bin"));
        for (unsigned int i = 64; i < 128; ++i) CHECK(record[i] == 0);
        return 0;
    case 0xc0045627ul:
        CHECK(words[0] == 1 && words[1] < 2);
        CHECK(kernel_registered & (1u << words[1]));
        CHECK(runtime_index[words[1]] == words[1]);
        active_id = words[1];
        activated |= 1u << active_id;
        return 0;
    case 0x800c56d5ul:
    case 0x800c56d7ul:
        CHECK(words[0] < 2 && words[1] == 0 && words[2] == 0);
        words[1] = 0; words[2] = request == 0x800c56d5ul ? 4096 : 8192;
        return 0;
    case 0x800c56d4ul:
    case 0x800c56d6ul:
        CHECK(words[0] < 2 && words[1] >= 0x02a90000);
        CHECK(words[2] == (request == 0x800c56d4ul ? 4096u : 8192u));
        return 0;
    default:
        /* The real driver returns zero for unknown commands. Make the test
         * reject such false success explicitly. */
        CHECK(!"unexpected ioctl");
        return 0;
    }
}

static void make_record(unsigned char record[84], unsigned int id)
{
    uint32_t bus = 1, tail = 123456;
    uint16_t selector = (uint16_t)id;
    memset(record, 0, 84); strcpy((char *)record, id ? "cv2005s1" : "cv2005");
    memcpy(record + 32, &selector, 2); memcpy(record + 36, &bus, 4);
    memcpy(record + 80, &tail, 4);
}

int main(void)
{
    unsigned char record[84]; char path[128] = "/tmp/iq.bin";
    {
        unsigned int id = 0;
        OpenIMPT23IspOem80 state = {0}; make_record(record, id);
        stage = 0;
        CHECK(!openimp_t23_oem80_sensor_add(&state, 19, record, path, 1));
        CHECK(state.registered == (1u << id));
        CHECK(state.ncu[id] && state.wdr[id]);
        CHECK(openimp_t23_oem80_isp_close(&state) < 0 && errno == EBUSY);
        unsigned int before = ioctls;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, path, 0) < 0);
        CHECK(ioctls == before);
        CHECK(!openimp_t23_oem80_sensor_del(&state, 19, record));
        CHECK(!state.registered && !state.ncu[id] && !state.wdr[id]);
        CHECK(!openimp_t23_oem80_isp_close(&state));
    }
    {
        OpenIMPT23IspOem80 state = {0};
        unsigned char secondary[84];
        make_record(record, 0); make_record(secondary, 1); stage = 0;
        CHECK(!openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 0));
        CHECK(!openimp_t23_oem80_sensor_add(&state, 19, secondary, NULL, 0));
        CHECK(state.registered == 3 && state.ncu[0] && state.ncu[1]);
        CHECK(activated == 3 && runtime_index[1] == 1);
        CHECK(enumerations == 7); /* primary:2, then ordered dual:2+3 */
        CHECK(!openimp_t23_oem80_sensor_del(&state, 19, record));
        CHECK(state.registered == 2 && !state.ncu[0] && state.ncu[1]);
        CHECK(openimp_t23_oem80_isp_close(&state) < 0 && errno == EBUSY);
        fail_dma_free = 1;
        CHECK(openimp_t23_oem80_sensor_del(&state, 19, secondary) < 0);
        CHECK(state.registered == 0 && state.ncu[1]);
        CHECK(openimp_t23_oem80_isp_close(&state) < 0);
        fail_dma_free = 0;
        CHECK(!openimp_t23_oem80_isp_close(&state));
        CHECK(!state.ncu[1]);
    }
    for (int fail = 1; fail <= 9; ++fail) {
        OpenIMPT23IspOem80 state = {0}; make_record(record, 0);
        failing_stage = fail; stage = 0;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, path, 1) < 0);
        CHECK(errno == EIO);
        CHECK(!state.registered && kernel_registered == 0);
        CHECK(!state.ncu[0] && !state.wdr[0]);
        CHECK(!openimp_t23_oem80_isp_close(&state));
    }
    {
        OpenIMPT23IspOem80 state = {0}; make_record(record, 0);
        stage = 0; failing_stage = 7; fail_delete = 1;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 1) < 0);
        CHECK(state.registered && state.ncu[0]);
        unsigned int before = freed;
        CHECK(openimp_t23_oem80_isp_close(&state) < 0 && errno == EBUSY);
        CHECK(freed == before);
        failing_stage = 0; fail_delete = 0;
        CHECK(!openimp_t23_oem80_sensor_del(&state, 19, record));
        CHECK(!openimp_t23_oem80_isp_close(&state));
    }
    for (int bad = 1; bad <= 4; ++bad) {
        OpenIMPT23IspOem80 state = {0}; make_record(record, 0);
        stage = failing_stage = 0; bad_enum = bad;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 0) < 0);
        CHECK(!state.registered && !kernel_registered && !state.ncu[0]);
        CHECK(!openimp_t23_oem80_isp_close(&state));
    }
    bad_enum = 0;
    for (int retain = 0; retain < 2; ++retain) {
        OpenIMPT23IspOem80 state = {0}; unsigned char secondary[84];
        make_record(record, 0); make_record(secondary, 1); stage = 0;
        CHECK(!openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 0));
        IMPDMABufferInfo *primary = state.ncu[0];
        stage = 0; failing_stage = 3; fail_delete = retain;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, secondary, NULL, 0) < 0 && errno == EIO);
        CHECK(state.ncu[0] == primary && !state.ncu[1]);
        CHECK(state.registered == (retain ? 3u : 1u));
        CHECK(openimp_t23_oem80_isp_close(&state) < 0);
        failing_stage = fail_delete = 0;
        if (retain) CHECK(!openimp_t23_oem80_sensor_del(&state, 19, secondary));
        CHECK(!openimp_t23_oem80_sensor_del(&state, 19, record));
        CHECK(!openimp_t23_oem80_isp_close(&state));
    }
    { /* OEM traversal cannot find slot 1 without the primary slot present. */
        OpenIMPT23IspOem80 state = {0}; make_record(record, 1); stage = 0;
        CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 0) < 0 && errno == ENOENT);
        CHECK(!state.registered && !kernel_registered);
    }
    make_record(record, 2); unsigned int before = ioctls;
    OpenIMPT23IspOem80 state = {0};
    CHECK(openimp_t23_oem80_sensor_add(&state, 19, record, NULL, 0) < 0);
    CHECK(ioctls == before);
    CHECK(allocated == freed);
    printf("PASS: %u OEM ISP ABI/lifecycle checks\n", checks);
    return 0;
}
