/*
 * evo_direct_mem.c — Direct Memory Region & Zero-Fragmentation Slab Manager.
 *
 * Provides thread-safe 2MB-aligned memory management for frame buffers,
 * subtitle surfaces, and streaming I/O ring buffers.
 */
#include "evo_direct_mem.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#if defined(EVO_TARGET_PS5)
#include <ps5/kernel.h>
/* PS5 SDK direct memory definitions if not present in standard headers */
extern int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd,
                                         size_t length, size_t alignment,
                                         int memoryType, off_t *physAddr);
extern int sceKernelMapDirectMemory(void **addr, size_t length, int protection,
                                    int flags, off_t physAddr, size_t alignment);
extern int sceKernelReleaseDirectMemory(off_t physAddr, size_t length);

/* The budget queries. Signatures cross-checked against the SharpProspero ABI
 * reference (third_party/SharpProspero, Interop/Kernel/KernelMemory.cs) rather
 * than guessed - getting the out-parameter widths wrong here would corrupt the
 * stack and read as a boot crash, not as a bad number. */
extern int64_t sceKernelGetDirectMemorySize(void);
extern int     sceKernelAvailableDirectMemorySize(int64_t searchStart,
                                                  int64_t searchEnd,
                                                  size_t alignment,
                                                  int64_t *physAddrOut,
                                                  size_t *sizeOut);
extern int     sceKernelAvailableFlexibleMemorySize(size_t *outSize);
extern int     sceKernelConfiguredFlexibleMemorySize(size_t *outSize);
extern int     sceKernelMunmap(void *addr, size_t len);
#include <stdint.h>
#include <time.h>
#endif

/* evo_boot_log(): the one sink that reaches /mnt/usb0/evo.log AND klog. */
extern void evo_boot_log(const char *fmt, ...);

/* Lazy-init size when a caller allocates before evo_direct_mem_init() runs.
 * The real pool is sized by main.c (EVO_DIRECT_MEM_POOL_BYTES, #6). */
#define EVO_DIRECT_MEM_DEFAULT_SIZE EVO_DIRECT_MEM_POOL_BYTES
#define EVO_DIRECT_MEM_ALIGN        64                  /* 64-byte SIMD alignment */
#define EVO_CHUNK_HEADER_SIZE       ((sizeof(mem_chunk_header_t) + EVO_DIRECT_MEM_ALIGN - 1) & ~(EVO_DIRECT_MEM_ALIGN - 1))

typedef struct mem_chunk_header {
    size_t                   size;
    int                      is_free;
    struct mem_chunk_header *next;
    struct mem_chunk_header *prev;
} mem_chunk_header_t;

typedef struct {
    pthread_mutex_t     lock;
    uint8_t            *base_ptr;
    size_t              total_size;
    size_t              allocated_size;
    size_t              peak_size;
    size_t              num_allocs;
    int                 is_direct_hw;
    off_t               ps5_phys_addr;
    mem_chunk_header_t *first_chunk;
    int                 initialized;
} direct_pool_t;

static direct_pool_t g_direct_pool = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .initialized = 0
};

int evo_direct_mem_init(size_t pool_size_bytes)
{
    pthread_mutex_lock(&g_direct_pool.lock);
    if (g_direct_pool.initialized) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return 0;
    }

    if (pool_size_bytes == 0)
        pool_size_bytes = EVO_DIRECT_MEM_DEFAULT_SIZE;

    /* Align pool to 2MB boundary */
    size_t align_2mb = 2 * 1024 * 1024;
    pool_size_bytes = (pool_size_bytes + align_2mb - 1) & ~(align_2mb - 1);

    void *base = NULL;
    int is_hw = 0;

#if defined(EVO_TARGET_PS5)
    /* PS5 Direct Memory: memoryType 3 = WB_ONION (CPU cacheable + GPU shared).
     *
     * #6: the pool is now sized for the 4K CPU video working set (rotate ring,
     * pp_playback display / held-frame snapshot), which is far larger than the
     * original 64 MiB. If the console cannot satisfy the full request
     * from physical direct memory, step down rather than fall through to the
     * heap fallback below (posix_memalign here means flexible memory on the
     * app module, and the whole point of #6 is to keep these buffers OFF the
     * flexible-memory heap that the 4K decode pools contend for). Each step
     * still leaves the 1080p working set entirely in-slab. */
    {
        size_t try_sizes[4];
        int n = 0;
        try_sizes[n++] = pool_size_bytes;
        if (pool_size_bytes > (size_t)128 * 1024 * 1024)
            try_sizes[n++] = (size_t)128 * 1024 * 1024;
        if (pool_size_bytes > (size_t)96 * 1024 * 1024)
            try_sizes[n++] = (size_t)96 * 1024 * 1024;
        if (pool_size_bytes > (size_t)64 * 1024 * 1024)
            try_sizes[n++] = (size_t)64 * 1024 * 1024;

        for (int t = 0; t < n && !base; t++) {
            off_t phys = 0;
            size_t want = (try_sizes[t] + align_2mb - 1) & ~(align_2mb - 1);
            int ret = sceKernelAllocateDirectMemory(
                0, (off_t)16 * 1024 * 1024 * 1024ULL, want, align_2mb, 3, &phys);
            if (ret != 0 || phys == 0)
                continue;
            void *mapped = NULL;
            ret = sceKernelMapDirectMemory(&mapped, want, 0x33 /* PROT_CPU_RW | PROT_GPU_RW */,
                                           0, phys, align_2mb);
            if (ret == 0 && mapped != NULL) {
                base = mapped;
                pool_size_bytes = want;
                g_direct_pool.ps5_phys_addr = phys;
                is_hw = 1;
            } else {
                sceKernelReleaseDirectMemory(phys, want);
            }
        }
    }
#endif

    if (!base) {
        /* Fallback: 2MB aligned heap allocation */
        if (posix_memalign(&base, align_2mb, pool_size_bytes) != 0) {
            base = malloc(pool_size_bytes);
        }
        is_hw = 0;
    }

    if (!base) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return -1;
    }

    memset(base, 0, pool_size_bytes);
    g_direct_pool.base_ptr = (uint8_t *)base;
    g_direct_pool.total_size = pool_size_bytes;
    g_direct_pool.allocated_size = 0;
    g_direct_pool.peak_size = 0;
    g_direct_pool.num_allocs = 0;
    g_direct_pool.is_direct_hw = is_hw;

    /* Initialize root chunk */
    mem_chunk_header_t *root = (mem_chunk_header_t *)base;
    root->size = pool_size_bytes - EVO_CHUNK_HEADER_SIZE;
    root->is_free = 1;
    root->next = NULL;
    root->prev = NULL;
    g_direct_pool.first_chunk = root;

    g_direct_pool.initialized = 1;
    pthread_mutex_unlock(&g_direct_pool.lock);
    return 0;
}

void evo_direct_mem_shutdown(void)
{
    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        return;
    }

#if defined(EVO_TARGET_PS5)
    if (g_direct_pool.is_direct_hw && g_direct_pool.ps5_phys_addr != 0) {
        sceKernelReleaseDirectMemory(g_direct_pool.ps5_phys_addr, g_direct_pool.total_size);
    } else
#endif
    {
        free(g_direct_pool.base_ptr);
    }

    g_direct_pool.base_ptr = NULL;
    g_direct_pool.total_size = 0;
    g_direct_pool.allocated_size = 0;
    g_direct_pool.first_chunk = NULL;
    g_direct_pool.initialized = 0;
    pthread_mutex_unlock(&g_direct_pool.lock);
}

void *evo_direct_mem_alloc(size_t bytes)
{
    if (bytes == 0) return NULL;

    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized) {
        if (evo_direct_mem_init(EVO_DIRECT_MEM_DEFAULT_SIZE) != 0) {
            pthread_mutex_unlock(&g_direct_pool.lock);
            return malloc(bytes);
        }
    }

    /* Round up requested bytes to alignment boundary */
    size_t req = (bytes + EVO_DIRECT_MEM_ALIGN - 1) & ~(EVO_DIRECT_MEM_ALIGN - 1);

    mem_chunk_header_t *curr = g_direct_pool.first_chunk;
    while (curr) {
        if (curr->is_free && curr->size >= req) {
            /* Split chunk if remaining space is useful */
            if (curr->size >= req + EVO_CHUNK_HEADER_SIZE + EVO_DIRECT_MEM_ALIGN) {
                mem_chunk_header_t *next_chunk = (mem_chunk_header_t *)((uint8_t *)curr + EVO_CHUNK_HEADER_SIZE + req);
                next_chunk->size = curr->size - req - EVO_CHUNK_HEADER_SIZE;
                next_chunk->is_free = 1;
                next_chunk->next = curr->next;
                next_chunk->prev = curr;
                if (curr->next) curr->next->prev = next_chunk;
                curr->next = next_chunk;
                curr->size = req;
            }

            curr->is_free = 0;
            g_direct_pool.allocated_size += curr->size;
            g_direct_pool.num_allocs++;
            if (g_direct_pool.allocated_size > g_direct_pool.peak_size) {
                g_direct_pool.peak_size = g_direct_pool.allocated_size;
            }

            void *ptr = (void *)((uint8_t *)curr + EVO_CHUNK_HEADER_SIZE);
            pthread_mutex_unlock(&g_direct_pool.lock);
            return ptr;
        }
        curr = curr->next;
    }

    pthread_mutex_unlock(&g_direct_pool.lock);
    /* Direct pool exhausted; graceful fallback to system allocator */
    return malloc(bytes);
}

void *evo_direct_mem_calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *ptr = evo_direct_mem_alloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void evo_direct_mem_free(void *ptr)
{
    if (!ptr) return;

    pthread_mutex_lock(&g_direct_pool.lock);
    if (!g_direct_pool.initialized ||
        (uint8_t *)ptr < g_direct_pool.base_ptr ||
        (uint8_t *)ptr >= g_direct_pool.base_ptr + g_direct_pool.total_size) {
        pthread_mutex_unlock(&g_direct_pool.lock);
        free(ptr);
        return;
    }

    mem_chunk_header_t *chunk = (mem_chunk_header_t *)((uint8_t *)ptr - EVO_CHUNK_HEADER_SIZE);
    if (!chunk->is_free) {
        chunk->is_free = 1;
        g_direct_pool.allocated_size -= chunk->size;
        g_direct_pool.num_allocs--;

        /* Merge with next chunk if free */
        if (chunk->next && chunk->next->is_free) {
            chunk->size += EVO_CHUNK_HEADER_SIZE + chunk->next->size;
            chunk->next = chunk->next->next;
            if (chunk->next) chunk->next->prev = chunk;
        }

        /* Merge with previous chunk if free */
        if (chunk->prev && chunk->prev->is_free) {
            chunk->prev->size += EVO_CHUNK_HEADER_SIZE + chunk->size;
            chunk->prev->next = chunk->next;
            if (chunk->next) chunk->next->prev = chunk->prev;
        }
    }
    pthread_mutex_unlock(&g_direct_pool.lock);
}

void evo_direct_mem_get_stats(evo_direct_mem_stats_t *out_stats)
{
    if (!out_stats) return;
    pthread_mutex_lock(&g_direct_pool.lock);
    out_stats->total_bytes = g_direct_pool.total_size;
    out_stats->allocated_bytes = g_direct_pool.allocated_size;
    out_stats->peak_bytes = g_direct_pool.peak_size;
    out_stats->num_allocations = g_direct_pool.num_allocs;
    out_stats->is_direct_hardware_mem = g_direct_pool.is_direct_hw;
    pthread_mutex_unlock(&g_direct_pool.lock);
}

void evo_mem_budget_log(const char *when)
{
    evo_direct_mem_stats_t st;
    evo_direct_mem_get_stats(&st);

#if defined(EVO_TARGET_PS5)
    /* -1 for a call that failed, so a failure is distinguishable from a zero.
     * Megabytes, because the decisions these numbers feed are made in tens of
     * megabytes and the raw byte counts are unreadable in a log line. */
    long long dm_total = -1, dm_free = -1, flex_total = -1, flex_free = -1;

    int64_t total = sceKernelGetDirectMemorySize();
    if (total > 0) dm_total = (long long)(total / (1024 * 1024));

    /* Largest free RUN, not the sum of free space: a fragmented pool with a lot
     * free and no contiguous stretch cannot back a frame buffer, and that
     * distinction is the whole question for a 4K plane. */
    if (total > 0) {
        int64_t phys = 0;
        size_t  avail = 0;
        if (sceKernelAvailableDirectMemorySize(0, total, 2u * 1024u * 1024u,
                                               &phys, &avail) == 0)
            dm_free = (long long)(avail / (1024 * 1024));
    }

    size_t fsz = 0;
    if (sceKernelConfiguredFlexibleMemorySize(&fsz) == 0)
        flex_total = (long long)(fsz / (1024 * 1024));
    if (sceKernelAvailableFlexibleMemorySize(&fsz) == 0)
        flex_free = (long long)(fsz / (1024 * 1024));

    evo_boot_log("mem budget [%s] direct_total=%lldMB direct_largest_free=%lldMB "
                 "flex_total=%lldMB flex_free=%lldMB "
                 "evo_pool=%zu/%zuMB peak=%zuMB hw=%d",
                 when ? when : "?",
                 dm_total, dm_free, flex_total, flex_free,
                 st.allocated_bytes / (1024 * 1024),
                 st.total_bytes / (1024 * 1024),
                 st.peak_bytes / (1024 * 1024),
                 st.is_direct_hardware_mem);
#else
    /* stderr, not evo_boot_log: the host test binary links this file without
     * evo_boot_log.c, and there is no kernel budget to report off-device
     * anyway - only EVO's own pool, which is ordinary malloc there. */
    fprintf(stderr, "mem budget [%s] host build - evo_pool=%zu/%zuMB peak=%zuMB\n",
            when ? when : "?",
            st.allocated_bytes / (1024 * 1024),
            st.total_bytes / (1024 * 1024),
            st.peak_bytes / (1024 * 1024));
#endif
}

/*
 * How much direct memory can EVO actually take? (follow-up to #94)
 *
 * The kernel reports ~11 GB free, but a number reported is not a number
 * usable: the GPU runtime, VideoOut and the resident hardware decoders already
 * hold their share, and a larger type-3 pool once "wedged" 4K present. So this
 * takes it for real - step_bytes at a time, type 11 (cached, CPU read/write,
 * the type the malloc shim's fallback uses), writing one byte per 16 KiB page
 * and reading it back - until max_bytes or the first refusal, logging every
 * step, then gives all of it back. Nothing else runs meanwhile; it is a boot
 * probe behind a trigger file, never a normal-boot path.
 */
void evo_direct_mem_probe(size_t step_bytes, size_t max_bytes)
{
#if defined(EVO_TARGET_PS5)
    enum { MAX_CHUNKS = 128 };
    const size_t align = 64u * 1024u;
    off_t  phys[MAX_CHUNKS];
    void  *va[MAX_CHUNKS];
    int    n = 0;
    size_t total = 0;
    int64_t dm_size = sceKernelGetDirectMemorySize();

    if (step_bytes < align || dm_size <= 0) {
        evo_boot_log("dm probe: not run (step=%zu dm_size=%lld)",
                     step_bytes, (long long)dm_size);
        return;
    }
    step_bytes &= ~(align - 1u);
    evo_boot_log("dm probe: begin step=%zuMB max=%zuMB",
                 step_bytes >> 20, max_bytes >> 20);

    while (n < MAX_CHUNKS && total + step_bytes <= max_bytes) {
        struct timespec t0, t1, t2, t3;
        off_t pa = 0;
        void *p = NULL;

        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = sceKernelAllocateDirectMemory(0, (off_t)dm_size, step_bytes,
                                              align, 11, &pa);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (r != 0) {
            evo_boot_log("dm probe: STOP at %zuMB - allocate refused rc=0x%x",
                         total >> 20, (unsigned)r);
            break;
        }
        r = sceKernelMapDirectMemory(&p, step_bytes, 0x03, 0, pa, align);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        if (r != 0 || !p) {
            sceKernelReleaseDirectMemory(pa, step_bytes);
            evo_boot_log("dm probe: STOP at %zuMB - map refused rc=0x%x",
                         total >> 20, (unsigned)r);
            break;
        }

        volatile uint8_t *b = (volatile uint8_t *)p;
        const uint8_t tag = (uint8_t)(0x5a ^ n);
        for (size_t off = 0; off < step_bytes; off += 16384u)
            b[off] = tag;
        b[step_bytes - 1u] = tag;
        int bad = 0;
        for (size_t off = 0; off < step_bytes; off += 16384u)
            if (b[off] != tag) { bad = 1; break; }
        if (b[step_bytes - 1u] != tag) bad = 1;
        clock_gettime(CLOCK_MONOTONIC, &t3);

        phys[n] = pa;
        va[n] = p;
        n++;
        total += step_bytes;

        int64_t fphys = 0;
        size_t  favail = 0, flex = 0;
        long long largest = -1, flex_mb = -1;
        if (sceKernelAvailableDirectMemorySize(0, dm_size, 2u * 1024u * 1024u,
                                               &fphys, &favail) == 0)
            largest = (long long)(favail >> 20);
        if (sceKernelAvailableFlexibleMemorySize(&flex) == 0)
            flex_mb = (long long)(flex >> 20);

#define PROBE_US(a, b) ((long long)(((b).tv_sec - (a).tv_sec) * 1000000LL + \
                        ((b).tv_nsec - (a).tv_nsec) / 1000))
        evo_boot_log("dm probe: +%d total=%zuMB alloc_us=%lld map_us=%lld "
                     "touch_us=%lld verify=%s largest_free=%lldMB flex_free=%lldMB",
                     n, total >> 20, PROBE_US(t0, t1), PROBE_US(t1, t2),
                     PROBE_US(t2, t3), bad ? "BAD" : "ok", largest, flex_mb);
#undef PROBE_US
        if (bad)
            break;
    }

    size_t held = total;
    for (int i = n - 1; i >= 0; --i) {
        sceKernelMunmap(va[i], step_bytes);
        sceKernelReleaseDirectMemory(phys[i], step_bytes);
    }
    evo_boot_log("dm probe: held %zuMB in %d chunks, all released", held >> 20, n);
    evo_mem_budget_log("dm-probe-after");
#else
    (void)step_bytes;
    (void)max_bytes;
#endif
}
