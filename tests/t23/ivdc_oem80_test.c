/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "t23/t23_ivdc_oem80.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
static unsigned checks, allocs, frees, flushes, closes, requests, sets, commands;
static unsigned mode = 2, lines = 540, wanted_size = 622080;
static uint32_t supplied, consumed, phys = 0x05000000, flags = 2;
static int fail_open, fail_get_mode, fail_get_lines, fail_alloc, fail_flush;
static int fail_publish, fail_clear, fail_queue, fail_free, keep_consumed, fail_proc;
static int rmem = 1, bad_proc, frame_closed;
static const char *proc_override;
static unsigned long kernel_taint;
static char proc[256];

int __wrap_open(const char *path, int options, ...)
{
    CHECK(!strcmp(path, "/dev/misc-ivdc"));
    CHECK(options == (O_RDWR | O_CLOEXEC));
    if (fail_open) { errno = EIO; return -1; }
    return 19;
}
int __wrap_close(int fd) { CHECK(fd == 19); ++closes; return 0; }
FILE *__wrap_fopen(const char *path, const char *access)
{
    CHECK(!strcmp(access, "r"));
    if (!strcmp(path, "/proc/sys/kernel/tainted")) {
        snprintf(proc, sizeof proc, "%lu\n", kernel_taint);
        return fmemopen(proc, strlen(proc), "r");
    }
    CHECK(!strcmp(path, "/proc/jz/isp/isp-ivdc"));
    if (fail_proc) { errno = EACCES; return NULL; }
    if (proc_override) snprintf(proc, sizeof proc, "%s", proc_override);
    else snprintf(proc, sizeof proc, bad_proc ? "no ownership proof\n" :
        " IVDC DDR Addr : 0x%08x\n IVDC DDR Buffer Size : 622080\n", consumed);
    return fmemopen(proc, strlen(proc), "r");
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    ++commands;
    va_list args; va_start(args, request); uint32_t *p = va_arg(args, uint32_t *); va_end(args);
    if (request == 0xc0145608ul) {
        CHECK(fd == 23 && p[0] == 0 && p[1] == 1 && p[2] == 2 && p[3] == 0 && p[4] == 0);
        ++requests;
        if (fail_queue) { errno = EBUSY; return -1; }
        if (!keep_consumed) consumed = 0;
        return 0;
    }
    CHECK(fd == 19);
    switch (request) {
    case 0xc004543ful:
        if (fail_get_mode) { errno = EIO; return -1; }
        *p = mode; return 0;
    case 0xc0045442ul:
        if (fail_get_lines) { errno = EIO; return -1; }
        *p = lines; return 0;
    case 0xc0045441ul:
        ++sets;
        if (*p) {
            CHECK(flushes && allocs > frees);
            supplied = *p; /* Even the error can publish the word. */
            if (fail_publish) { errno = EIO; return -1; }
        } else {
            CHECK(requests && consumed == 0); /* Never flip ownership first. */
            if (fail_clear) { errno = EIO; return -1; }
            supplied = 0;
        }
        return 0;
    default: CHECK(!"unexpected command"); return -1;
    }
}
int DMA_Ensure_RMEM(void) { if (rmem) return 0; errno = ENODEV; return -1; }
int DMA_AllocDescriptor(IMPDMABufferInfo *info, int size, const char *tag)
{
    CHECK((unsigned)size == wanted_size && !strcmp(tag, "ivdcwork"));
    if (fail_alloc) { errno = ENOMEM; return -1; }
    ++allocs; info->phys_addr = phys; info->virt_addr = 0x70000000;
    info->size = (uint32_t)size; info->flags = flags;
    return 0;
}
int DMA_RmemFlushCache(void *address, uint32_t size, int direction)
{
    CHECK((uintptr_t)address == 0x70000000 && size == wanted_size && direction == 0);
    CHECK(consumed == 0); /* Never touch the CPU alias while driver owns it. */
    ++flushes;
    if (fail_flush) { errno = EIO; return -1; }
    return 0;
}
int DMA_FreePhys(uint32_t address)
{
    CHECK(address == phys && supplied == 0 && consumed == 0);
    CHECK(frame_closed || !requests); /* Frame fd closes before post-detach free. */
    if (fail_free) { errno = EIO; return -1; }
    ++frees; return 0;
}
static OpenIMPT23IvdcOem80 create(void)
{
    OpenIMPT23IvdcOem80 s = {.fd = -1};
    frame_closed = requests = 0;
    CHECK(!openimp_t23_ivdc_create(&s));
    return s;
}
static void release_state(OpenIMPT23IvdcOem80 *s)
{
    CHECK(!openimp_t23_ivdc_detach(s, 23));
    CHECK(!s->installed && supplied == 0 && consumed == 0);
    frame_closed = 1;
    CHECK(!openimp_t23_ivdc_release(s));
    CHECK(!s->allocated);
    CHECK(!openimp_t23_ivdc_destroy(s) && s->fd == -1);
}
int main(void)
{
    OpenIMPT23IvdcOem80 s = create();
    CHECK(!openimp_t23_ivdc_prepare(&s, 640, 360));
    CHECK(s.installed && s.allocated && supplied == phys);
    CHECK(openimp_t23_ivdc_verify_attached(&s) < 0);
    consumed = supplied;
    CHECK(!openimp_t23_ivdc_verify_attached(&s));
    consumed += 4096;
    CHECK(openimp_t23_ivdc_verify_attached(&s) < 0 && s.installed);
    consumed = supplied;
    CHECK(openimp_t23_ivdc_release(&s) < 0 && errno == EBUSY);
    CHECK(openimp_t23_ivdc_destroy(&s) < 0 && s.fd == 19);
    unsigned before = allocs;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && allocs == before);
    keep_consumed = 1;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && errno == EBUSY);
    CHECK(s.installed && s.allocated && supplied == phys && frees == 0);
    keep_consumed = 0;
    fail_queue = 1;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && s.installed);
    fail_queue = 0; fail_proc = 1;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && s.installed && supplied == phys);
    fail_proc = 0; bad_proc = 1;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && s.installed);
    bad_proc = 0; fail_clear = 1;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && s.installed && supplied == phys);
    fail_clear = 0;
    CHECK(!openimp_t23_ivdc_detach(&s, 23));
    frame_closed = 1; fail_free = 1;
    CHECK(openimp_t23_ivdc_release(&s) < 0 && !s.installed && s.allocated);
    fail_free = 0; fail_flush = 1;
    CHECK(openimp_t23_ivdc_release(&s) < 0 && s.allocated);
    fail_flush = 0;
    CHECK(!openimp_t23_ivdc_destroy(&s));

    /* Publication failure leaves a possibly supplied but unconsumed pointer. */
    s = create(); fail_publish = 1;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && s.installed);
    CHECK(supplied == phys && consumed == 0);
    fail_publish = 0; release_state(&s);

    /* Checked fallback sizing, alignment and line-count overflow. */
    s = create(); lines = 0; wanted_size = 103680;
    CHECK(!openimp_t23_ivdc_prepare(&s, 640, 360)); release_state(&s);
    s = create(); lines = UINT32_MAX;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && errno == EOVERFLOW);
    CHECK(!s.allocated && !s.installed); CHECK(!openimp_t23_ivdc_destroy(&s));
    lines = 540; wanted_size = 622080;
    for (unsigned i = 0; i < 4; ++i) {
        s = create();
        if (i == 0) rmem = 0;
        if (i == 1) fail_get_lines = 1;
        if (i == 2) fail_alloc = 1;
        if (i == 3) fail_flush = 1;
        CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && !s.installed);
        rmem = 1; fail_get_lines = fail_alloc = fail_flush = 0;
        CHECK(!openimp_t23_ivdc_destroy(&s));
    }
    for (unsigned i = 0; i < 3; ++i) {
        s = create(); frame_closed = 1;
        phys = i == 0 ? 0x20000000 : i == 1 ? 0x1ffff000 : 0x05000001;
        CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && !s.installed);
        CHECK(!openimp_t23_ivdc_destroy(&s));
    }
    phys = 0x05000000; mode = 0; s = create(); before = allocs;
    CHECK(!openimp_t23_ivdc_prepare(&s, 640, 360) && allocs == before);
    CHECK(!openimp_t23_ivdc_destroy(&s));
    mode = 2;
    for (unsigned i = 0; i < 2; ++i) {
        s = (OpenIMPT23IvdcOem80){.fd = -1};
        fail_open = i == 0; fail_get_mode = i == 1;
        CHECK(openimp_t23_ivdc_create(&s) < 0 && s.fd == -1);
        fail_open = fail_get_mode = 0;
    }
    s = (OpenIMPT23IvdcOem80){.fd = -1}; consumed = phys;
    before = allocs;
    CHECK(openimp_t23_ivdc_create(&s) < 0 && s.fd == -1 && allocs == before);
    consumed = 0; s = create(); consumed = phys;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && allocs == before);
    consumed = 0;
    CHECK(!openimp_t23_ivdc_destroy(&s));
    const char *bad[] = {" IVDC DDR Addr : junk\n", " IVDC DDR Addr : 0x100000000\n",
        " IVDC DDR Addr : 0x0 trailing\n", " IVDC DDR Addr : 0x0   "};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        proc_override = bad[i];
        CHECK(openimp_t23_ivdc_check_detached() < 0);
    }
    proc_override = NULL;
    s = create();
    CHECK(openimp_t23_ivdc_prepare(&s, 0, 360) < 0);
    CHECK(openimp_t23_ivdc_prepare(&s, 640, -1) < 0);
    CHECK(!s.allocated); CHECK(!openimp_t23_ivdc_destroy(&s));
    s = create(); flags = 0;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && !s.installed);
    CHECK(!openimp_t23_ivdc_destroy(&s)); flags = 2;
    s = create(); kernel_taint = 1ul << 7; before = commands;
    CHECK(openimp_t23_ivdc_prepare(&s, 640, 360) < 0 && commands == before && !s.allocated);
    kernel_taint = 0; CHECK(!openimp_t23_ivdc_destroy(&s));
    s = create(); CHECK(!openimp_t23_ivdc_prepare(&s, 640, 360));
    consumed = supplied; kernel_taint = 1ul << 7; before = requests;
    CHECK(openimp_t23_ivdc_detach(&s, 23) < 0 && requests == before);
    CHECK(s.installed && s.allocated && supplied == phys);
    CHECK(openimp_t23_ivdc_check_detached() < 0);
    kernel_taint = 0; release_state(&s);
    CHECK(allocs == frees);
    printf("OEM80 IVDC lifecycle: %u checks passed\n", checks);
    return 0;
}
