/*
 * evo_direct_mem.h — Direct Memory Region & Zero-Fragmentation Slab Manager.
 *
 * Implements high-performance, 2MB-aligned direct memory management for PS5
 * video frame buffers, subtitle texture surfaces, and streaming I/O buffers.
 */
#ifndef EVO_DIRECT_MEM_H
#define EVO_DIRECT_MEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pool size (#6). Kept at the proven 64 MiB: a larger WB_ONION reservation
 * competes with the GPU / sceAgc / VideoOut direct-memory budget and wedged
 * the first 4K V8 present on hardware (2026-09-03). The 1080p CPU working set
 * (swscale rotate ring, pp_playback display, nv12 scratch) fits here; the 4K
 * display / staging buffers deliberately spill to malloc() via
 * evo_direct_mem_alloc's graceful fallback — exactly as before #6, and the
 * V8 GPU present path never allocates them anyway.
 */
#define EVO_DIRECT_MEM_POOL_BYTES ((size_t)64 * 1024 * 1024)

typedef struct {
    size_t total_bytes;
    size_t allocated_bytes;
    size_t peak_bytes;
    size_t num_allocations;
    int    is_direct_hardware_mem;
} evo_direct_mem_stats_t;

/**
 * Initialize direct memory region manager.
 * Pre-allocates a 2MB-aligned direct memory pool (default 64 MiB).
 * On PS5: uses sceKernelAllocateDirectMemory + sceKernelMapDirectMemory (WB_ONION).
 * On host: uses 2MB-aligned virtual memory pool (posix_memalign / mmap).
 */
int evo_direct_mem_init(size_t pool_size_bytes);

/**
 * Shutdown direct memory pool and free backing direct memory.
 */
void evo_direct_mem_shutdown(void);

/**
 * Allocate a buffer from the direct memory region.
 * Guaranteed 64-byte alignment for fast SIMD/GPU/DMA access.
 */
void *evo_direct_mem_alloc(size_t bytes);

/**
 * Allocate zero-initialized memory from the direct memory region.
 */
void *evo_direct_mem_calloc(size_t count, size_t size);

/**
 * Free buffer back to direct memory region.
 */
void evo_direct_mem_free(void *ptr);

/**
 * Query current direct memory statistics.
 */
void evo_direct_mem_get_stats(evo_direct_mem_stats_t *out_stats);

/*
 * Log the title's ACTUAL memory budget - all four numbers, in one line, tagged
 * with `when`.
 *
 * This exists because the budget was argued about from inferred figures for
 * months, and both of them were wrong. Answers, measured 2026-09-25:
 * direct memory is 12288 MB with an 11 GB contiguous free run, flexible is
 * 448 MB. EVO reserves 64 MiB of the former - 0.5% - because that size was
 * proven not to wedge the GPU, not because anything asked the kernel.
 * docs/hardware/memory-budget.md is the write-up; keep this call, because a
 * figure nobody prints is a figure that gets guessed at again.
 *
 * Reports: direct total and largest free run, flexible configured and free,
 * and EVO's own pool. Safe to call before the unjail - a call that fails is
 * reported as -1 rather than skipped, because "the call failed here" is itself
 * the answer to whether this can be measured at that point.
 */
void evo_mem_budget_log(const char *when);

/*
 * Boot probe: allocate direct memory (type 11) step_bytes at a time up to
 * max_bytes or the first refusal, touch and verify every page, log each step,
 * release it all. Behind /mnt/usb0/evo_dm_probe - see Application.cpp.
 */
void evo_direct_mem_probe(size_t step_bytes, size_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DIRECT_MEM_H */
