/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "t30/t23_vpu_abi.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks, calls, expected_operation;
static OpenIMPT23VpuAbi expected_abi;
static int result_code;
#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint32_t read32(const unsigned char *data, size_t offset)
{ uint32_t value; memcpy(&value, data + offset, sizeof(value)); return value; }
static void write32(unsigned char *data, size_t offset, uint32_t value)
{ memcpy(data + offset, &value, sizeof(value)); }
static uint64_t read64(const unsigned char *data, size_t offset)
{ uint64_t value; memcpy(&value, data + offset, sizeof(value)); return value; }
static void write64(unsigned char *data, size_t offset, uint64_t value)
{ memcpy(data + offset, &value, sizeof(value)); }

int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args; unsigned char *wire;
    unsigned long base = expected_abi == OPENIMP_T23_VPU_OEM80 ?
                         0xc0506300ul : 0xc0586300ul;
    size_t time_offset = expected_abi == OPENIMP_T23_VPU_OEM80 ? 72u : 80u;
    ++calls;
    CHECK(fd == 17);
    CHECK(request == (base | expected_operation));
    va_start(args, request); wire = va_arg(args, unsigned char *); va_end(args);
    if (expected_operation == 7) {
        CHECK(read32(wire, 0) == 0x13100004u);
        CHECK(read32(wire, 4) == 0 && read32(wire, 8) == 0);
        for (size_t i = 12; i < 88; ++i) CHECK(wire[i] == 0);
        write32(wire, 4, 0xfeedabcd);
    } else {
        CHECK(read32(wire, 0) == 0xc001cafeu);
        CHECK(read32(wire, 4) == 0xc002cafeu);
        CHECK(read32(wire, 8) == 2000u);
        CHECK(read32(wire, 16) == 0x02000001u);
        CHECK(read32(wire, 24) == 2u);
        CHECK(read32(wire, 36) == 0x02a90000u);
        CHECK(read32(wire, 40) == UINT32_MAX);
        CHECK(read32(wire, 56) == 0u);
        CHECK(read32(wire, 64) == 540u);
        CHECK(read64(wire, time_offset) == UINT64_C(0x1122334455667788));
        if (expected_abi == OPENIMP_T23_VPU_SDK88) {
            CHECK(read32(wire, 72) == 0xdeadbeefu);
            CHECK(read32(wire, 76) == 0xabcdef01u);
            write32(wire, 72, 0x20000000u);
        }
        write32(wire, 0, 0xc003cafeu);
        write32(wire, 4, 0xc004cafeu);
        write32(wire, 28, 0x301u);
        write32(wire, 32, 12345u);
        write64(wire, time_offset, UINT64_C(0x99aabbccddeeff00));
    }
    errno = result_code < 0 ? EIO : EBUSY;
    return result_code;
}

static void channel_case(OpenIMPT23VpuAbi abi, unsigned int operation, int result)
{
    struct {
        uint64_t before;
        OpenIMPT23VpuNode node;
        uint64_t after;
    } guarded;
    memset(&guarded, 0xa5, sizeof(guarded));
    guarded.before = UINT64_C(0x1020304050607080);
    guarded.after = UINT64_C(0x8070605040302010);
    guarded.node = (OpenIMPT23VpuNode){0};
    guarded.node.clist = 0xc001cafeu; guarded.node.vlist = 0xc002cafeu;
    guarded.node.mdelay = 2000u; guarded.node.vpu_id = 0x02000001;
    guarded.node.workphase = 2; guarded.node.dma_addr = 0x02a90000;
    guarded.node.thread_id = -1; guarded.node.ivdc_mem_line = 540;
    guarded.node.max_bs_act = 0xdeadbeef; guarded.node.reserved = 0xabcdef01;
    guarded.node.time = UINT64_C(0x1122334455667788);
    expected_abi = abi; expected_operation = operation; result_code = result;
    unsigned int previous = calls;
    CHECK(openimp_t23_vpu_channel(17, abi, operation, &guarded.node) == result);
    CHECK(errno == (result < 0 ? EIO : EBUSY));
    CHECK(calls == previous + 1); /* No retry, including EPERM/EIO paths. */
    CHECK(guarded.node.clist == 0xc003cafeu && guarded.node.vlist == 0xc004cafeu);
    CHECK(guarded.node.status == 0x301 && guarded.node.output_len == 12345);
    CHECK(guarded.node.time == UINT64_C(0x99aabbccddeeff00));
    CHECK(guarded.node.max_bs_act == (abi == OPENIMP_T23_VPU_SDK88 ? 0x20000000u : 0));
    CHECK(guarded.node.reserved == (abi == OPENIMP_T23_VPU_SDK88 ? 0xabcdef01u : 0));
    CHECK(guarded.before == UINT64_C(0x1020304050607080));
    CHECK(guarded.after == UINT64_C(0x8070605040302010));
}

int main(void)
{
    OpenIMPT23VpuAbi abi; unsigned int previous;
    CHECK(sizeof(OpenIMPT23VpuNode) == 88);
    CHECK(!openimp_t23_vpu_parse(NULL, &abi) && abi == OPENIMP_T23_VPU_SDK88);
    CHECK(!openimp_t23_vpu_parse("", &abi) && abi == OPENIMP_T23_VPU_SDK88);
    CHECK(!openimp_t23_vpu_parse("sdk88", &abi) && abi == OPENIMP_T23_VPU_SDK88);
    CHECK(!openimp_t23_vpu_parse("oem80", &abi) && abi == OPENIMP_T23_VPU_OEM80);
    CHECK(openimp_t23_vpu_parse("auto", &abi) < 0 && errno == EINVAL);
    CHECK(openimp_t23_vpu_parse("80", &abi) < 0);
    CHECK(openimp_t23_vpu_parse("oem80 ", &abi) < 0);
    CHECK(openimp_t23_vpu_parse("sdk88", NULL) < 0);
    CHECK(!setenv("OPENIMP_T23_DRIVER_ABI", "oem80", 1));
    CHECK(!openimp_t23_vpu_select(&abi) && abi == OPENIMP_T23_VPU_OEM80);
    CHECK(!strcmp(openimp_t23_vpu_name(abi), "oem80"));
    CHECK(!unsetenv("OPENIMP_T23_DRIVER_ABI"));
    CHECK(!openimp_t23_vpu_select(&abi) && abi == OPENIMP_T23_VPU_SDK88);
    for (int mode = 0; mode <= 1; ++mode) {
        for (unsigned int operation = 0; operation <= 2; ++operation) {
            channel_case((OpenIMPT23VpuAbi)mode, operation, 0);
            channel_case((OpenIMPT23VpuAbi)mode, operation, -1);
        }
        channel_case((OpenIMPT23VpuAbi)mode, 2, 2);
        expected_abi = (OpenIMPT23VpuAbi)mode; expected_operation = 7;
        uint32_t value = 0;
        result_code = 0;
        CHECK(!openimp_t23_vpu_read_reg(17, expected_abi, 0x13100004, &value));
        CHECK(value == 0xfeedabcdu && errno == EBUSY);
        result_code = -1; value = 42;
        CHECK(openimp_t23_vpu_read_reg(17, expected_abi, 0x13100004, &value) == -1);
        CHECK(value == 42 && errno == EIO);
    }
    previous = calls;
    OpenIMPT23VpuNode node = {0};
    CHECK(openimp_t23_vpu_channel(17, (OpenIMPT23VpuAbi)99, 0, &node) < 0);
    CHECK(openimp_t23_vpu_channel(17, OPENIMP_T23_VPU_OEM80, 12, &node) < 0);
    CHECK(openimp_t23_vpu_channel(17, OPENIMP_T23_VPU_OEM80, 0, NULL) < 0);
    CHECK(openimp_t23_vpu_read_reg(17, (OpenIMPT23VpuAbi)99, 0, &node.clist) < 0);
    CHECK(openimp_t23_vpu_read_reg(17, OPENIMP_T23_VPU_OEM80, 0, NULL) < 0);
    CHECK(calls == previous);
    printf("PASS: %u T23 VPU ABI checks\n", checks);
    return 0;
}
