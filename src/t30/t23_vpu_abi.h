/* SPDX-License-Identifier: MIT */
#ifndef OPENIMP_T23_DRIVER_ABI_H
#define OPENIMP_T23_DRIVER_ABI_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    OPENIMP_T23_VPU_SDK88 = 0,
    OPENIMP_T23_VPU_OEM80 = 1
} OpenIMPT23VpuAbi;

/* Canonical userspace state. The OEM80 wire record omits max_bs_act and
 * reserved, placing time at 0x48 instead of 0x50. No native pointers cross
 * this interface: clist/vlist are opaque 32-bit kernel cookies. */
typedef struct {
    uint32_t clist, vlist, mdelay, channel_id;
    int32_t vpu_id;
    uint32_t codecdir, workphase, status, output_len, dma_addr;
    int32_t thread_id;
    uint32_t cmpx, n_flag, ncu_addr;
    uint32_t frame_type, overflow_cnt, ivdc_mem_line, data_threshold;
    uint32_t max_bs_act, reserved;
    uint64_t time;
} OpenIMPT23VpuNode;

_Static_assert(sizeof(OpenIMPT23VpuNode) == 88, "canonical T23 node size");
_Static_assert(offsetof(OpenIMPT23VpuNode, max_bs_act) == 72, "T23 common prefix");
_Static_assert(offsetof(OpenIMPT23VpuNode, time) == 80, "SDK88 time offset");

/* Select once before opening a channel. Unset/empty defaults to sdk88;
 * unknown values fail with EINVAL. There is no automatic command retry. */
int openimp_t23_vpu_select(OpenIMPT23VpuAbi *abi);
int openimp_t23_vpu_parse(const char *name, OpenIMPT23VpuAbi *abi);
const char *openimp_t23_vpu_name(OpenIMPT23VpuAbi abi);

/* Only REQUEST(0), RELEASE(1) and RUN(2) take a channel record here.
 * ioctl return values, partial copy-back and errno are preserved. */
int openimp_t23_vpu_channel(int fd, OpenIMPT23VpuAbi abi, unsigned int operation,
                            OpenIMPT23VpuNode *node);
/* Command 7 consumes reg_info {address,value,direction}, not channel_node,
 * despite its encoded ioctl size. Direction 0 is read. */
int openimp_t23_vpu_read_reg(int fd, OpenIMPT23VpuAbi abi, uint32_t address,
                             uint32_t *value);

#endif
