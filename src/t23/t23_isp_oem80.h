/* SPDX-License-Identifier: MIT */
#ifndef OPENIMP_T23_ISP_OEM80_H
#define OPENIMP_T23_ISP_OEM80_H

#include "dma_alloc.h"

/* Private state for the JA-A12 driver's two sensor slots. Public IMP record
 * layouts are unchanged. Records retain the selector at +0x20, not +0x50. */
typedef struct {
    unsigned int registered;
    unsigned char record[2][84];
    IMPDMABufferInfo *ncu[2];
    IMPDMABufferInfo *wdr[2];
} OpenIMPT23IspOem80;

int openimp_t23_oem80_sensor_add(OpenIMPT23IspOem80 *state, int fd,
                                const void *record84, const char *path128,
                                int wdr);
int openimp_t23_oem80_sensor_del(OpenIMPT23IspOem80 *state, int fd,
                                const void *record84);
/* Refuse teardown while registered; retry only safe, detached DMA frees. */
int openimp_t23_oem80_isp_close(OpenIMPT23IspOem80 *state);

#endif
