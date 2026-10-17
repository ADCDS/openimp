/* SPDX-License-Identifier: MIT */
#include "t23_ivdc_oem80.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int openimp_t23_ivdc_kernel_healthy(void)
{
    FILE *file = fopen("/proc/sys/kernel/tainted", "r");
    unsigned long value = 0;
    if (!file) return -1;
    int valid = fscanf(file, "%lu", &value) == 1;
    int closed = fclose(file);
    /* Linux TAINT_DIE (bit 7): resource-state observations after an Oops
     * are insufficient proof of safe DMA reuse. Only reset clears it. */
    if (!valid || closed || (value & (1ul << 7))) { errno = EIO; return -1; }
    return 0;
}

static int read_address(uint32_t *out)
{
    FILE *file = fopen("/proc/jz/isp/isp-ivdc", "r");
    char line[256];
    int result = -1;
    if (!file) return -1;
    errno = 0;
    while (fgets(line, sizeof line, file)) {
        const char *name = "IVDC DDR Addr";
        char *p = line, *end;
        while (isspace((unsigned char)*p)) ++p;
        if (strncmp(p, name, strlen(name))) continue;
        if (!strchr(line, '\n')) break;
        p += strlen(name);
        while (isspace((unsigned char)*p)) ++p;
        if (*p++ != ':') break;
        errno = 0;
        unsigned long address = strtoul(p, &end, 0);
        if (end == p || errno || address > UINT32_MAX) break;
        while (isspace((unsigned char)*end)) ++end;
        if (*end) break;
        *out = (uint32_t)address;
        result = 0;
        break;
    }
    int saved = result ? (errno ? errno : EIO) : 0;
    if (fclose(file) && !result) { saved = errno ? errno : EIO; result = -1; }
    if (result) errno = saved;
    return result;
}

int openimp_t23_ivdc_check_detached(void)
{
    uint32_t address;
    if (openimp_t23_ivdc_kernel_healthy() != 0) return -1;
    if (read_address(&address) != 0) return -1;
    if (address) { errno = EBUSY; return -1; }
    return 0;
}

int openimp_t23_ivdc_verify_attached(OpenIMPT23IvdcOem80 *s)
{
    uint32_t address;
    if (!s) { errno = EINVAL; return -1; }
    if (!s->installed) return 0;
    if (openimp_t23_ivdc_kernel_healthy() != 0) return -1;
    if (read_address(&address) != 0) return -1;
    if (!s->allocated || address != s->dma.phys_addr) { errno = EIO; return -1; }
    fprintf(stderr, "[IVDC OEM80] driver consumed reserved workspace phys=0x%08x\n", address);
    return 0;
}

int openimp_t23_ivdc_create(OpenIMPT23IvdcOem80 *s)
{
    if (!s || s->fd >= 0 || s->allocated || s->installed) {
        errno = EBUSY; return -1;
    }
    if (openimp_t23_ivdc_check_detached() != 0) return -1;
    s->fd = open("/dev/misc-ivdc", O_RDWR | O_CLOEXEC);
    if (s->fd < 0) return -1;
    int mode = -1;
    int result = ioctl(s->fd, 0xc004543ful, &mode);
    if (result != 0 || mode < 0 || mode > 2) {
        int saved = result ? (errno ? errno : EIO) : ENOTSUP;
        close(s->fd); s->fd = -1; errno = saved; return -1;
    }
    s->mode = mode;
    return 0;
}

int openimp_t23_ivdc_prepare(OpenIMPT23IvdcOem80 *s, int width, int height)
{
    if (!s || s->fd < 0 || s->allocated || s->installed) {
        errno = EBUSY; return -1;
    }
    if (openimp_t23_ivdc_kernel_healthy() != 0) return -1;
    if (!s->mode) return 0;
    if (width <= 0 || height <= 0) { errno = EINVAL; return -1; }
    uint32_t lines = 0;
    if (ioctl(s->fd, 0xc0045442ul, &lines) != 0) return -1;
    uint64_t stride = ((uint64_t)(unsigned int)width + 255) & ~UINT64_C(255);
    uint64_t bytes = stride * (lines ? (uint64_t)lines : (unsigned int)height);
    /* Check before multiplying; a hostile line count cannot wrap uint64_t. */
    if (bytes > (uint64_t)INT_MAX * (lines ? 2u : 8u) / 3u) {
        errno = EOVERFLOW; return -1;
    }
    bytes = bytes * 3 / (lines ? 2 : 8);
    if (!bytes || bytes > INT_MAX) { errno = EOVERFLOW; return -1; }
    if (openimp_t23_ivdc_check_detached() != 0 || DMA_Ensure_RMEM() != 0) return -1;
    memset(&s->dma, 0, sizeof s->dma);
    if (DMA_AllocDescriptor(&s->dma, (int)bytes, "ivdcwork") != 0) return -1;
    s->allocated = 1;
    /* The driver forms a cached KSEG0 alias. Require a complete page extent
     * in reserved RAM, entirely below the 512 MiB KSEG0 physical boundary. */
    uint64_t extent = (bytes + 4095) & ~UINT64_C(4095);
    if (!(s->dma.flags & 2) || !s->dma.virt_addr || !s->dma.phys_addr || (s->dma.phys_addr & 4095) ||
        s->dma.size < bytes || (uint64_t)s->dma.phys_addr + extent > 0x20000000) {
        int saved = EOVERFLOW;
        (void)openimp_t23_ivdc_release(s);
        errno = saved; return -1;
    }
    if (DMA_RmemFlushCache((void *)(uintptr_t)s->dma.virt_addr, s->dma.size, 0) != 0)
        return -1;
    /* A failed ioctl can still have side effects. Retain until explicit
     * queue cleanup plus address verification and clearing the pending slot. */
    s->installed = 1;
    if (ioctl(s->fd, 0xc0045441ul, &s->dma.phys_addr) != 0) return -1;
    fprintf(stderr, "[IVDC OEM80] reserved workspace phys=0x%08x bytes=%u lines=%u\n",
            s->dma.phys_addr, (unsigned int)bytes, lines);
    return 0;
}

int openimp_t23_ivdc_detach(OpenIMPT23IvdcOem80 *s, int frame_fd)
{
    uint32_t req[5] = {0, 1, 2, 0, 0};
    uint32_t zero = 0;
    if (!s || frame_fd < 0) { errno = EINVAL; return -1; }
    if (openimp_t23_ivdc_kernel_healthy() != 0) return -1;
    if (ioctl(frame_fd, 0xc0145608ul, req) != 0) return -1;
    if (!s->installed) return 0;
    /* REQBUFS(0) hides remote cleanup errors. Verify the driver's consumed
     * address, not its stale size. Never change ownership before this edge. */
    if (openimp_t23_ivdc_check_detached() != 0) return -1;
    /* On a pre-STREAMON failure, the global supplied address may never have
     * been consumed. Its SET handler only copies this word; clearing it now
     * prevents a later queue from adopting memory we are about to release. */
    if (ioctl(s->fd, 0xc0045441ul, &zero) != 0) return -1;
    s->installed = 0;
    fprintf(stderr, "[IVDC OEM80] driver workspace detached and pending address cleared\n");
    return 0;
}

int openimp_t23_ivdc_release(OpenIMPT23IvdcOem80 *s)
{
    if (!s) { errno = EINVAL; return -1; }
    if (s->installed) { errno = EBUSY; return -1; }
    if (s->allocated) {
        if (openimp_t23_ivdc_kernel_healthy() != 0) return -1;
        if ((s->dma.flags & 2) &&
            (!s->dma.virt_addr || DMA_RmemFlushCache((void *)(uintptr_t)s->dma.virt_addr,
                                                   s->dma.size, 0) != 0))
            return -1;
        if (DMA_FreePhys(s->dma.phys_addr) != 0) return -1;
        fprintf(stderr, "[IVDC OEM80] released reserved workspace phys=0x%08x\n", s->dma.phys_addr);
        memset(&s->dma, 0, sizeof s->dma); s->allocated = 0;
    }
    return 0;
}

int openimp_t23_ivdc_destroy(OpenIMPT23IvdcOem80 *s)
{
    if (openimp_t23_ivdc_release(s) != 0) return -1;
    if (s->fd >= 0) {
        int fd = s->fd; s->fd = -1;
        /* Linux close releases the descriptor even if it reports EINTR. */
        if (close(fd) != 0) return -1;
    }
    s->mode = 0;
    return 0;
}
