/* SPDX-License-Identifier: MIT */
/* Exercise actual CREATED/ENABLED unwind and public global-exit gates, with
 * independent fake ioctl ownership. No camera or real DMA operations. */
#define main helper_contract_main
#include "ivdc_oem80_test.c"
#undef main
#include "../../src/framesource/framesource_tseries.c"

FrameSourceState *gFrameSource;
Module *g_modules[6][IMP_MAX_GROUPS];
#if !defined(__mips__)
char _gp;
#endif
static unsigned char device[FS_DEV_SIZE + 128];
static int fail_stop, stops, pool_releases, group_releases, jpeg_exits;
int openimp_t23_persist_enabled(void) { return 0; }
void openimp_t23_persist_write(const char *s, size_t n) { (void)s; (void)n; }
int IMP_Log_Get_Option(void) { return 0; }
int imp_log_fun(int level, int option, int type, ...) { (void)level; (void)option; (void)type; return 0; }
int fs_stream_off(int fd) { CHECK(fd == 23 || fd == 24); ++stops; if (fail_stop) { errno = EIO; return -1; } return 0; }
void fs_close_device(int fd) { CHECK(fd == 23 || fd == 24); if (fd == 23) frame_closed = 1; }
int fs_set_buffer_count(int fd, int count) { CHECK(fd == 24 && count == 0); return 0; }
int VBMGetFrame(int ch, void **frame) { (void)ch; (void)frame; CHECK(!"teardown test must not capture"); return -1; }
int fs_qbuf(int fd, int index, unsigned long phys_addr, unsigned int length) { (void)fd; (void)index; (void)phys_addr; (void)length; CHECK(!"teardown test must not queue frames"); return -1; }
void VBMWaitReleases(int ch) { CHECK(ch == 0 || ch == 1); }
int VBMFlushFrame(int ch) { CHECK(ch == 0 || ch == 1); return 0; }
int VBMDestroyPoolParked(int ch) { CHECK(ch == 0 || ch == 1); if (!ch) CHECK(consumed == 0 && supplied == 0); ++pool_releases; return 0; }
void VBMReleaseParked(int ch) { CHECK(ch == 0 || ch == 1); ++pool_releases; }
int32_t destroy_group(Subject *sub, int32_t dev) { (void)sub; CHECK(dev == 0 && consumed == 0); ++group_releases; return 0; }
void free_device(void *dev) { (void)dev; CHECK(!"busy FrameSource must not be freed"); }
void OpenIMP_HelixJpeg_Exit(void) { ++jpeg_exits; }

static void seed(int streaming)
{
    memset(device, 0, sizeof device); memset(g_fs_ctx, 0, sizeof g_fs_ctx);
    gFrameSource = (FrameSourceState *)(device + 64);
    for (int i = 0; i < FS_MAX_CHANNELS; ++i) { g_fs_ctx[i].fd = -1; g_fs_ctx[i].ivdc.fd = -1; }
    FsChnCtx *ctx = &g_fs_ctx[0];
    ctx->oem80 = ctx->created = ctx->queue_may_exist = 1;
    ctx->fd = 23; ctx->ivdc = create();
    CHECK(!openimp_t23_ivdc_prepare(&ctx->ivdc, 640, 360));
    supplied = consumed = phys;
    ctx->stream_may_run = streaming;
    fs_chan_set_state(0, streaming ? 2 : 1);
}
int main(void)
{
    CHECK(!IMP_System_Init());
    seed(0); /* failed Enable: no STREAMON, Destroy must detach directly. */
    fail_queue = 1;
    unsigned before = frees;
    CHECK(IMP_FrameSource_DestroyChn(0) < 0);
    CHECK(g_fs_ctx[0].created && g_fs_ctx[0].fd == 23 && frees == before);
    CHECK(stops == 0 && g_fs_ctx[0].ivdc.installed);
    CHECK(IMP_System_Exit() < 0 && jpeg_exits == 0);
    FrameSourceExit(); CHECK(gFrameSource != NULL);
    fail_queue = 0; keep_consumed = 1;
    CHECK(IMP_FrameSource_DestroyChn(0) < 0 && frees == before);
    keep_consumed = 0;
    CHECK(!IMP_FrameSource_DestroyChn(0));
    CHECK(!g_fs_ctx[0].created && g_fs_ctx[0].fd == -1 && frees == before + 1);
    CHECK(!IMP_FrameSource_DisableChn(0) && fs_chan_get_state(0) == 0);
    CHECK(!IMP_System_Exit() && jpeg_exits == 1);

    CHECK(!IMP_System_Init()); seed(1); fail_stop = 1; before = frees;
    CHECK(IMP_FrameSource_DisableChn(0) < 0);
    CHECK(g_fs_ctx[0].stream_may_run && g_fs_ctx[0].fd == 23 && frees == before);
    CHECK(IMP_System_Exit() < 0 && jpeg_exits == 1);
    fail_stop = 0;
    CHECK(!IMP_FrameSource_DisableChn(0));
    CHECK(!g_fs_ctx[0].stream_may_run && g_fs_ctx[0].fd == -1 && frees == before + 1);
    CHECK(!IMP_FrameSource_DestroyChn(0));
    CHECK(!IMP_System_Exit() && jpeg_exits == 2);

    CHECK(!IMP_System_Init()); seed(1);
    FsChnCtx *sub = &g_fs_ctx[1];
    sub->oem80 = sub->created = sub->queue_may_exist = sub->stream_may_run = 1;
    sub->fd = 24; fs_chan_set_state(1, 2); before = stops;
    CHECK(IMP_FrameSource_DisableChn(0) < 0 && stops == (int)before);
    CHECK(g_fs_ctx[0].ivdc.installed && supplied == phys);
    CHECK(!IMP_FrameSource_DisableChn(1));
    CHECK(!IMP_FrameSource_DestroyChn(1));
    CHECK(!IMP_FrameSource_DisableChn(0));
    CHECK(!IMP_FrameSource_DestroyChn(0));
    CHECK(!IMP_System_Exit());
    /* A failed STREAMON in CREATED state must still stop before detach. */
    CHECK(!IMP_System_Init()); seed(0); g_fs_ctx[0].stream_may_run = 1;
    fail_stop = 1; before = frees;
    CHECK(IMP_FrameSource_DestroyChn(0) < 0 && frees == before);
    fail_stop = 0; CHECK(!IMP_FrameSource_DestroyChn(0));
    CHECK(!IMP_System_Exit());

    /* Detached DMA-free failure: preserve the descriptor despite closed fd. */
    CHECK(!IMP_System_Init()); seed(0); fail_free = 1;
    CHECK(IMP_FrameSource_DestroyChn(0) < 0 && g_fs_ctx[0].fd == -1);
    CHECK(!g_fs_ctx[0].ivdc.installed && g_fs_ctx[0].ivdc.allocated);
    CHECK(IMP_System_Exit() < 0);
    fail_free = 0; CHECK(!IMP_FrameSource_DestroyChn(0)); CHECK(!IMP_System_Exit());

    /* Supplied but not consumed: no STREAMOFF, yet clear pending PADDR. */
    CHECK(!IMP_System_Init()); seed(0); consumed = 0; g_fs_ctx[0].queue_may_exist = 0;
    before = stops; CHECK(!IMP_FrameSource_DestroyChn(0) && stops == (int)before);
    CHECK(!IMP_System_Exit());

    CHECK(!IMP_System_Init()); seed(1); kernel_taint = 1ul << 7; before = frees;
    CHECK(IMP_FrameSource_DisableChn(0) < 0 && frees == before);
    CHECK(g_fs_ctx[0].ivdc.installed && g_fs_ctx[0].fd == 23);
    CHECK(IMP_System_Exit() < 0);
    kernel_taint = 0; /* Fake reset only; real kernel Oops requires device reset. */
    CHECK(!IMP_FrameSource_DisableChn(0)); CHECK(!IMP_FrameSource_DestroyChn(0));
    CHECK(!IMP_System_Exit());

    /* Dependency gates run before any format, allocation or queue mutation. */
    memset(g_fs_ctx, 0, sizeof g_fs_ctx); memset(device, 0, sizeof device);
    g_fs_ctx[1].oem80 = g_fs_ctx[1].created = 1;
    CHECK(fs_oem_enable_ready(1) < 0);
    g_fs_ctx[0].oem80 = g_fs_ctx[0].created = 1;
    g_fs_ctx[0].ivdc.mode = 2; fs_chan_set_state(0, 2);
    CHECK(fs_oem_enable_ready(1) < 0);
    g_fs_ctx[0].ivdc.installed = 1;
    CHECK(!fs_oem_enable_ready(1));
    g_fs_ctx[1].queue_may_exist = 1; g_fs_ctx[0].ivdc.installed = 0;
    CHECK(fs_oem_enable_ready(0) < 0);
    g_fs_ctx[1].queue_may_exist = 0; CHECK(!fs_oem_enable_ready(0));
    kernel_taint = 1ul << 7;
    CHECK(fs_oem_enable_ready(0) < 0 && errno == EIO);
    CHECK(fs_oem_enable_ready(1) < 0 && errno == EIO);
    kernel_taint = 0;
    CHECK(allocs == frees);
    printf("OEM80 FrameSource lifecycle: %u checks passed\n", checks);
    return 0;
}
