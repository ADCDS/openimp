/* SPDX-License-Identifier: MIT */
#ifndef OPENIMP_T23_IVDC_OEM80_H
#define OPENIMP_T23_IVDC_OEM80_H
#include "dma_alloc.h"

typedef struct {
    int fd, mode, allocated, installed;
    IMPDMABufferInfo dma;
} OpenIMPT23IvdcOem80;

/* Initialize with .fd = -1. Serialized by the FrameSource lifecycle lock.
 * Only FS0 owns the global workspace. None of these calls changes mode. */
/* Also used before first reserved-arena allocation in a fresh process. */
int openimp_t23_ivdc_kernel_healthy(void);
int openimp_t23_ivdc_check_detached(void);
int openimp_t23_ivdc_create(OpenIMPT23IvdcOem80 *state);
int openimp_t23_ivdc_prepare(OpenIMPT23IvdcOem80 *state, int width, int height);
/* After successful positive REQBUFS, before queuing frames or streaming. */
int openimp_t23_ivdc_verify_attached(OpenIMPT23IvdcOem80 *state);
/* Caller has stopped capture and all dependent channels. Always releases the
 * frame queue first; retain everything on failed/uncertain driver detach. */
int openimp_t23_ivdc_detach(OpenIMPT23IvdcOem80 *state, int frame_fd);
/* Only after detach and frame fd close. Detached free failures are retryable. */
int openimp_t23_ivdc_release(OpenIMPT23IvdcOem80 *state);
int openimp_t23_ivdc_destroy(OpenIMPT23IvdcOem80 *state);
#endif
