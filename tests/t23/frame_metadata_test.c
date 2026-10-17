/* SPDX-License-Identifier: MIT */
/* Exercise the real DQBUF -> VBM publication boundary, without camera DMA. */
#define _GNU_SOURCE
#include "../../src/kernel_interface.c"
#include <assert.h>

static uint32_t completion_kind, completion_overflow = 7;
static int fail_dequeue, fail_queue;
static unsigned int queues, captures;
static uint32_t expected_address = 0x05000000, expected_length = 345600;
int DMA_AllocDescriptor(IMPDMABufferInfo *p, int n, const char *s)
{ (void)p; (void)n; (void)s; assert(!"direct pool must not allocate image storage"); return -1; }
int DMA_PoolAllocDescriptor(int pool, IMPDMABufferInfo *p, int n, const char *s)
{ (void)pool; return DMA_AllocDescriptor(p,n,s); }
int DMA_FreePhys(uint32_t p) { (void)p; assert(!"direct pool owns no image allocation"); return -1; }
int DMA_RmemStats(size_t *u, size_t *s, size_t *l) { (void)u; (void)s; (void)l; return -1; }
void DMA_LogRmem(const char *s) { (void)s; }
int64_t IMP_System_GetTimeStamp(void) { return 123; }
int64_t OpenIMP_P0_NormalizeMonotonicTimeStamp(uint64_t stamp)
{ assert(stamp == 12000034); return 123; }
void openimp_t31_ivs_capture(int chn, const void *frame)
{ assert(chn == 1 && frame); ++captures; }
int __wrap_ioctl(int fd, unsigned long command, ...)
{
    va_list args; va_start(args, command);
    struct v4l2_buf32 *b = va_arg(args, struct v4l2_buf32 *); va_end(args);
    assert(fd == 19 && b->type == 1 && b->memory == 2);
    if (command == VIDIOC_DQBUF) {
        if (fail_dequeue) { errno = EAGAIN; return -1; }
        b->index = 0; b->ts_sec = 12; b->ts_usec = 34;
        b->reserved = completion_kind; b->reserved2 = completion_overflow;
        return 0;
    }
    assert(command == VIDIOC_QBUF && b->index == 0);
    assert(b->m == expected_address && b->length == expected_length);
    ++queues;
    if (fail_queue) { errno = EIO; return -1; }
    return 0;
}

int main(void)
{
    VBMFrame frame = {.chn = 1, .size = 345600, .phys_addr = 0x05000000};
    int queue[1] = {0}; uint8_t userspace[1] = {0};
    VBMPool pool = {.chn = 1, .frames = &frame, .frame_count = 1,
                    .available_queue = queue, .buf_in_userspace = userspace, .oem80 = 1};
    pthread_mutex_init(&pool.queue_mutex, NULL); vbm_instance[1] = &pool;
    assert(!setenv("OPENIMP_T23_DRIVER_ABI", "oem80", 1));
    vbm_last_pull_ms[1] = vbm_now_ms();
    void *out = NULL;
    completion_kind = 0x00010003;
    assert(VBMKernelDequeue(1, 19, &out) == VBM_DQ_HELD);
    assert(!out && !pool.queue_count && !captures && queues == 1 && !userspace[0]);
    uint32_t kind, overflow;
    memcpy(&kind, (unsigned char *)&frame + 0x3f8, 4);
    memcpy(&overflow, (unsigned char *)&frame + 0x3f4, 4);
    assert(kind == completion_kind && overflow == completion_overflow);
    completion_kind = 2; /* Unknown nonordinary completions also fail closed. */
    assert(VBMKernelDequeue(1, 19, &out) == VBM_DQ_HELD && !out);
    assert(!captures && !pool.queue_count && queues == 2);
    unsetenv("OPENIMP_T23_DRIVER_ABI"); /* live pool keeps its owning profile */
    completion_kind = 3;
    assert(VBMKernelDequeue(1, 19, &out) == VBM_DQ_HELD && !out);
    assert(!captures && !pool.queue_count && queues == 3);
    completion_kind = 3; fail_queue = 1;
    assert(VBMKernelDequeue(1, 19, &out) < 0 && userspace[0]);
    assert(!out && !captures && !pool.queue_count && queues == 4);
    fail_queue = 0; completion_kind = 0;
    assert(!VBMKernelDequeue(1, 19, &out));
    assert(out == &frame && captures == 1 && pool.queue_count == 1);
    assert(frame.time_stamp == 123 && frame.time_stamp_ivdc == 123);
    pool.queue_count = 0; out = NULL;
    pool.oem80 = 0; /* a separate SDK88 pool has its own frozen profile */
    assert(!setenv("OPENIMP_T23_DRIVER_ABI", "oem80", 1)); completion_kind = 3;
    assert(!VBMKernelDequeue(1, 19, &out) && captures == 2 && pool.queue_count == 1);
    FsDqMetadata meta = {1,1}; int index = -1; uint64_t timestamp = 0;
    fail_dequeue = 1;
    assert(fs_dqbuf_metadata(19, &index, &timestamp, &meta) == -2);
    assert(!meta.kind && !meta.overflow);
    fail_dequeue = 0;
    assert(!fs_dqbuf(19, &index, &timestamp) && index == 0 && timestamp == 12000034);
    /* Real direct-pool creation and initial/recycle queues use the OEM
     * sentinel, with no image allocation or publication even for kind 0. */
    unsigned char fmt[0xd0] = {0};
    uint32_t value = 640; memcpy(fmt, &value, 4);
    value = 360; memcpy(fmt+4, &value, 4);
    value = 10; memcpy(fmt+8, &value, 4);
    value = 353280; memcpy(fmt+12, &value, 4);
    value = 1; memcpy(fmt+0x34, &value, 4);
    assert(!VBMCreatePoolOem80(0, fmt, NULL, NULL, 2));
    VBMPool *direct = vbm_instance[0];
    assert(direct->oem80 && direct->oem_direct && !direct->phys_base && !direct->virt_base);
    assert(!direct->frames[0].phys_addr && !direct->frames[0].virt_addr);
    assert(direct->frames[0].direct_phys_addr == 0x80000000);
    expected_address = 0x80000000; expected_length = 353280;
    assert(VBMFillPool(0) == 1 && VBMPrimeKernelQueue(0,19,1) == 1);
    completion_kind = 0; out = NULL;
    assert(VBMKernelDequeue(0,19,&out) == VBM_DQ_HELD && !out && !direct->queue_count);
    assert(captures == 2);
    assert(!VBMDestroyPoolParked(0)); free(vbm_retired[0]); vbm_retired[0] = NULL;
    assert(VBMCreatePoolOem80(0, fmt, NULL, NULL, 3) < 0 && errno == ENOTSUP);
    pthread_mutex_destroy(&pool.queue_mutex);
    puts("OEM80 DQBUF metadata: direct/unknown completions recycled without publication; ordinary NV12 published");
    return 0;
}
