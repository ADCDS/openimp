/* SPDX-License-Identifier: MIT
 * Request/release one private channel only. No DMA allocation, ISP/sensor
 * operation, register access, VPU RUN, or media-buffer mapping is issued.
 */
#define _POSIX_C_SOURCE 200809L
#include "t30/t23_vpu_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    OpenIMPT23VpuAbi abi;
    OpenIMPT23VpuNode node = {0};
    int fd, result, saved_errno;
    if (argc != 2 || strcmp(argv[1], "--request-release")) {
        fputs("Usage: OPENIMP_T23_DRIVER_ABI=oem80 t23-vpu-abi-probe --request-release\n", stderr);
        return 2;
    }
    if (openimp_t23_vpu_select(&abi) != 0) {
        fputs("Invalid OPENIMP_T23_DRIVER_ABI\n", stderr); return 2;
    }
    alarm(5);
    fd = open("/dev/soc_vpu", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open soc_vpu"); return 1; }
    node.mdelay = 500; node.thread_id = -1;
    result = openimp_t23_vpu_channel(fd, abi, 0, &node);
    saved_errno = errno;
    printf("abi=%s request=%d errno=%d\n", openimp_t23_vpu_name(abi),
           result, result ? saved_errno : 0);
    if (result != 0) { close(fd); return 1; }
    if (!node.clist) { fputs("No channel cookie returned\n", stderr); close(fd); return 1; }
    node.vlist = 0; node.workphase = 2;
    result = openimp_t23_vpu_channel(fd, abi, 1, &node);
    printf("abi=%s release=%d errno=%d\n", openimp_t23_vpu_name(abi),
           result, result ? errno : 0);
    close(fd); alarm(0);
    return result ? 1 : 0;
}
