/* SPDX-License-Identifier: MIT */
#include "t23_vpu_abi.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

typedef struct {
    uint32_t common[18];
    uint64_t time;
} Oem80Node;

_Static_assert(sizeof(Oem80Node) == 80, "OEM80 wire size");
_Static_assert(offsetof(Oem80Node, time) == 72, "OEM80 time offset");

static int valid_abi(OpenIMPT23VpuAbi abi)
{
    return abi == OPENIMP_T23_VPU_SDK88 || abi == OPENIMP_T23_VPU_OEM80;
}

int openimp_t23_vpu_parse(const char *name, OpenIMPT23VpuAbi *abi)
{
    if (!abi) { errno = EINVAL; return -1; }
    if (!name || !*name || !strcmp(name, "sdk88"))
        *abi = OPENIMP_T23_VPU_SDK88;
    else if (!strcmp(name, "oem80"))
        *abi = OPENIMP_T23_VPU_OEM80;
    else { errno = EINVAL; return -1; }
    return 0;
}

int openimp_t23_vpu_select(OpenIMPT23VpuAbi *abi)
{
    return openimp_t23_vpu_parse(getenv("OPENIMP_T23_DRIVER_ABI"), abi);
}

const char *openimp_t23_vpu_name(OpenIMPT23VpuAbi abi)
{
    return abi == OPENIMP_T23_VPU_OEM80 ? "oem80" :
           abi == OPENIMP_T23_VPU_SDK88 ? "sdk88" : "invalid";
}

static unsigned long command(OpenIMPT23VpuAbi abi, unsigned int operation)
{
    return (abi == OPENIMP_T23_VPU_OEM80 ? 0xc0506300ul : 0xc0586300ul) |
           operation;
}

int openimp_t23_vpu_channel(int fd, OpenIMPT23VpuAbi abi, unsigned int operation,
                            OpenIMPT23VpuNode *node)
{
    if (!valid_abi(abi) || !node || operation > 2) {
        errno = EINVAL;
        return -1;
    }
    if (abi == OPENIMP_T23_VPU_SDK88)
        return ioctl(fd, command(abi, operation), node);
    else {
        Oem80Node wire;
        int result, saved_errno;

        memcpy(wire.common, node, sizeof(wire.common));
        wire.time = node->time;
        result = ioctl(fd, command(abi, operation), &wire);
        saved_errno = errno;
        memcpy(node, wire.common, sizeof(wire.common));
        node->time = wire.time;
        /* These words do not exist in OEM80, and must never alias time. */
        node->max_bs_act = 0;
        node->reserved = 0;
        errno = saved_errno;
        return result;
    }
}

int openimp_t23_vpu_read_reg(int fd, OpenIMPT23VpuAbi abi, uint32_t address,
                             uint32_t *value)
{
    uint32_t payload[22] = {0};
    int result, saved_errno;

    if (!valid_abi(abi) || !value) { errno = EINVAL; return -1; }
    payload[0] = address;
    result = ioctl(fd, command(abi, 7), payload);
    saved_errno = errno;
    if (result == 0)
        *value = payload[1];
    errno = saved_errno;
    return result;
}
