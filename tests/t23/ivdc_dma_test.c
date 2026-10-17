/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <assert.h>
#include "../../src/dma_alloc.c"
static unsigned int checks;
static int unhealthy;
int openimp_t23_ivdc_check_detached(void) { ++checks; errno = EBUSY; return -1; }
int openimp_t23_ivdc_kernel_healthy(void) { if (unhealthy) { errno = EIO; return -1; } return 0; }
int __wrap_open(const char *path, int flags, ...) { (void)path; (void)flags; assert(!"stale ownership must be checked before any device open"); return -1; }
int main(void)
{
    assert(!setenv("OPENIMP_T23_DRIVER_ABI", "oem80", 1));
    IMPDMABufferInfo info = {0};
    assert(DMA_AllocDescriptor(&info, 4096, "must-not-reuse") < 0 && errno == EBUSY);
    assert(!g_dma_initialized && checks == 1);
    assert(DMA_Ensure_RMEM() < 0 && errno == EBUSY);
    assert(!g_dma_initialized && checks == 2);
    /* A later fault must also quarantine an already initialized allocator. */
    g_dma_initialized = g_rmem_supported = g_t23_oem80 = unhealthy = 1;
    assert(DMA_AllocDescriptor(&info, 4096, "must-not-reuse") < 0 && errno == EIO);
    assert(DMA_Ensure_RMEM() < 0 && errno == EIO);
    assert(g_dma_initialized && checks == 2 && !info.phys_addr);
    unhealthy = 0;
    assert(!dma_init());
    unhealthy = 1; g_t23_oem80 = 0;
    assert(!dma_init()); /* SDK88 has no OEM80 proc dependency. */
    for (unsigned i = 0; i < MAX_DMA_BUFFERS; ++i) assert(!g_buffer_registry[i]);
    /* Exercise a real allocator record without mapping any camera memory. */
    unhealthy = 0; g_t23_oem80 = g_is_rmem = g_is_devmem_rmem = 1;
    g_mem_fd = 17; g_rmem_base_phys = 0x05000000; g_rmem_size = 8192;
    g_rmem_virt_base = malloc(g_rmem_size); assert(g_rmem_virt_base);
    assert(!DMA_AllocDescriptor(&info, 4096, "retained-after-oops"));
    DMABufferRecord *record = lookup_buffer_by_phys(info.phys_addr);
    assert(record && g_rmem_arena.used == 4096);
    unhealthy = 1;
    assert(DMA_FreePhys(info.phys_addr) < 0 && errno == EIO);
    assert(IMP_Free((uintptr_t)record->virt_addr) < 0 && errno == EIO);
    assert(lookup_buffer_by_phys(info.phys_addr) == record && g_rmem_arena.used == 4096);
    unhealthy = 0; /* Fake reset only, never a recovery option on a real kernel. */
    assert(!DMA_FreePhys(info.phys_addr) && !g_rmem_arena.used);
    assert(!lookup_buffer_by_phys(info.phys_addr));
    free(g_rmem_virt_base);
    puts("OEM80 allocator refuses stale workspace and quarantines allocations/frees after Oops");
    return 0;
}
