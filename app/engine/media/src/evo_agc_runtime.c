#include "evo_agc_runtime.h"
#include "evo_agc_shader_header.h"
#include "evo_agc_pipes.h"
#include "evo_boot_log.h"
#include "evo_direct_mem.h"
#include "evo_hw.h"

#include <emmintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>   /* access() for the diagnostic flag files */

/* PS5_tilemap, for de-swizzling the scanout in evo_agc_runtime_read_scanout().
 * Included here rather than linked: its only previous includer was main.c,
 * which is now main.c.legacy and is not compiled by anything. */
#include "../../SDL_ps5tilemap.inc"

#define PS5_TILE_W_SHIFT 9    /* 1 << 9 == PS5_TILE_WIDTH  (512) */
#define PS5_TILE_H_SHIFT 7    /* 1 << 7 == PS5_TILE_HEIGHT (128) */
#define PS5_TILE_W_MASK  (PS5_TILE_WIDTH  - 1)
#define PS5_TILE_H_MASK  (PS5_TILE_HEIGHT - 1)
typedef char evo_tile_dims_must_be_pow2[
    ((1 << PS5_TILE_W_SHIFT) == PS5_TILE_WIDTH &&
     (1 << PS5_TILE_H_SHIFT) == PS5_TILE_HEIGHT) ? 1 : -1];

#define EVO_AGC_LOG_PREFIX "[evo_agc] "

#define EVO_AGC_DIRECT_MEM_TYPE 12  /* SCE_KERNEL_WB_ONION */
#define EVO_AGC_MAP_PROTECTION  0x33 /* PROT_CPU_RW | PROT_GPU_RW */

/*
 * The scanout surface is TILED - 128x128 pixel tiles of 0x10000 bytes each -
 * and its base address must be 2MB aligned. ps5-opengl states both as a
 * build-time contract (src/platform/ps5_scanout.h):
 *
 *   #define PS5_SCANOUT_ALIGNMENT 0x200000u
 *   #define PS5_SCANOUT_TILED_BYTES \
 *      (((W + 127)/128) * ((H + 127)/128) * 0x10000u)
 *   #if PS5_SCANOUT_BYTES < PS5_SCANOUT_TILED_BYTES || ...ALIGNMENT != 0
 *   #error Invalid tiled display buffer size or alignment
 *
 * This pool was allocated with 0x20000 (128KB), which put the buffers at
 * 0x2_7a7e0000 - 0x1e0000 past a 2MB boundary. The tiling hardware derives
 * every tile's address from an aligned base, so a misaligned one shuffles the
 * image into a regular repeating pattern: the "railway track" artifact chased
 * across this whole investigation. 1920x1080 needs 15*9*0x10000 = 0x870000
 * tiled bytes, well inside the 64MB per-buffer stride below.
 */
#define EVO_AGC_DIRECT_MEM_ALIGN UINT64_C(0x200000)

/*
 * The composite / stencil / depth budgets below are sized for the largest
 * render size this runtime will drive, not for 1080p: the panel is rendered
 * at its own resolution. Hardware reports full=3840x2160 on the dev console,
 * so 4K is the cap and the budgets are cut for it:
 *
 *   composite BGRA  align(3840*4,256) * 2160 = 33.2 MB  -> 40 MB
 *   depth D32F      3840*2160*4       = 33.2 MB          -> 40 MB
 *   stencil S8      3840*2160         =  8.3 MB          -> 16 MB
 *
 * each with room for the 64KB tiled padding. The 64 MB scanout stride already
 * had the headroom: 4K tiled needs ceil(3840/128)*ceil(2160/128)*0x10000 =
 * 30*17*0x10000 = 0x1FE0000, just under 32 MB.
 */
#define EVO_AGC_MAX_RENDER_W 3840
#define EVO_AGC_MAX_RENDER_H 2160

#define EVO_AGC_SCANOUT_STRIDE      UINT64_C(0x04000000) /* 64 MB per scanout buffer */
#define EVO_AGC_SCANOUT_TOTAL       UINT64_C(0x08000000) /* 128 MB for 2 buffers */
/* 128 MB = ~42.7 MB per frame slot (EVO_AGC_FRAME_SLOTS = 3). At 64 MB a slot
 * was 21.3 MB, and a software-decoded 4K 10-bit planar frame stages 16.6 MB of
 * Y plus 8.3 MB of interleaved RG16 chroma = 24.9 MB - so every such frame
 * failed "stage planar UV to RG16" and never reached the screen (#94, 4K AV1;
 * hardware 2026-09-25). 1080p frames and native NV12 never came near it. */
#define EVO_AGC_TRANSIENT_RING_SIZE UINT64_C(0x08000000) /* 128 MB transient ring */
#define EVO_AGC_COMMAND_BUFFER_SIZE UINT64_C(0x00600000) /* 6 MB (2 MB per slot * 3) */
#define EVO_AGC_SHADER_STORAGE_SIZE UINT64_C(0x00400000) /* 4 MB shader storage */
#define EVO_AGC_FENCE_STORAGE_SIZE  UINT64_C(0x00010000) /* 64 KB fence storage */
/* Persistent 1920x1080 BGRA staging texture for the OSD composite, plus its
 * quad. main.c rasterises the OSD into gl_scratch and hands it to
 * evo_gl_composite_bgra(); in --agc builds that used to be a no-op stub, so the
 * OSD simply never reached the panel during playback. */
#define EVO_AGC_COMPOSITE_SIZE      UINT64_C(0x02800000) /* 40 MB */
/* Stencil buffer for RmlUi clip masks (border-radius clipping and masked
 * overlays). S8 at 1920x1080 is ~2 MB raw; 8 MB covers the 64KB_Z_X tiled
 * padding with room to spare. Depth is left disabled - nothing here needs a
 * Z test, only stencil. */
#define EVO_AGC_STENCIL_SIZE        UINT64_C(0x01000000) /* 16 MB */
/* D32F at 1920x1080 is ~8 MB raw; 16 MB covers the tiled padding. The depth
 * TEST is never enabled - the surface exists because configuring DB with an
 * invalid Z format stopped the stencil planes working. */
#define EVO_AGC_DEPTH_SIZE          UINT64_C(0x02800000) /* 40 MB */
/* Full-canvas RGBA8 layer surfaces for backdrop-filter / filter composition.
 * Sized for the maximum render size (4K): pitch-aligned 256, standard pitch =
 * 4K*4 = 15360 bytes. Each layer is 32 MB, covering 4K RGBA8 (31.64 MB) with
 * margin. Layers use COMP_SWAP=STD (memory = R,G,B,A) so they can be sampled
 * back via the rgba8 T#; only the scanout uses COMP_SWAP=ALT for BGRA. */
#define EVO_AGC_LAYER_BYTES         UINT64_C(0x02000000) /* 32 MB per layer */
#define EVO_AGC_LAYER_TOTAL         (EVO_AGC_MAX_LAYERS * EVO_AGC_LAYER_BYTES)

#define EVO_AGC_TOTAL_DIRECT_MEM \
    (EVO_AGC_SCANOUT_TOTAL + EVO_AGC_TRANSIENT_RING_SIZE + \
     EVO_AGC_COMMAND_BUFFER_SIZE + EVO_AGC_SHADER_STORAGE_SIZE + \
     EVO_AGC_FENCE_STORAGE_SIZE + EVO_AGC_COMPOSITE_SIZE + \
     EVO_AGC_STENCIL_SIZE + EVO_AGC_DEPTH_SIZE + \
     EVO_AGC_LAYER_TOTAL)

/*
 * VideoOut buffer attribute. 0x...22000000 is the TILED BGRA attribute and
 * 0x...00000000 is the linear SDR one - EVO's own hardware-verified VideoOut
 * code had both as PP_VO_ATTR_TILED_BGRA / PP_VO_ATTR_SDR_LINEAR (deleted with
 * pp_videoout.c in GL-6, still in git at c588037^).
 *
 * This path MUST register LINEAR. Everything writing these buffers writes
 * linearly - the CPU backdrop clear, and the GPU colour target set up by
 * setup_color_target() - so registering them tiled makes the display walk the
 * same bytes in tile order, which paints a regular repeating block/stripe
 * pattern instead of the picture. It was registering tiled (this constant was
 * the tiled value under an "SDR" name), which is what the "railway track"
 * artifact was, and why even a pure-CPU linear test pattern never appeared.
 * #27 hit the same wall from the other side and recorded it: the sceAgc
 * present path needs the linear attribute, the CPU-tiler attribute comes out
 * R<->B swapped / garbled (hw 2026-09-04).
 */
#define EVO_AGC_VIDEO_FORMAT_SDR UINT64_C(0x8000000000000000)
/*
 * HDR10: Bgr10A2Bt2100Pq - 10:10:10:2, BT.2020 primaries, ST.2084 PQ
 * (third_party/SharpProspero VideoOutTypes.cs). Note the 0x22000000 bits are
 * the RGB-vs-BGR channel order, not tiling: Rgba8Srgb is 0x8000000022000000
 * and Bgra8Srgb 0x8000000000000000. EVO's scanout is BGR ordered (the GPU
 * writes it with COMP_SWAP=ALT), so HDR must be the BGR variant too - the
 * RGB one this used to hold would swap red and blue. Used by
 * evo_agc_runtime_set_hdr_output().
 */
#define EVO_AGC_VIDEO_FORMAT_HDR UINT64_C(0x8100070400000000)

/* Platform declarations */
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end,
                                      size_t bytes, size_t alignment,
                                      int memory_type, int64_t *offset);
int32_t sceKernelMapDirectMemory(void **address, size_t bytes, int protection,
                                 int flags, int64_t offset, size_t alignment);
int32_t sceKernelReleaseDirectMemory(int64_t offset, size_t bytes);
int32_t sceKernelMunmap(void *address, size_t bytes);
int     sceKernelUsleep(unsigned int microseconds);

int32_t sceVideoOutOpen(int32_t user_id, int32_t bus_type, int32_t index, const void *param);
int32_t sceVideoOutClose(int32_t handle);
int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
int32_t sceVideoOutConfigureOutput(int32_t handle, uint64_t mode,
                                   const void *options, const void *reserved0, uint64_t reserved1);
int32_t sceVideoOutIsOutputSupported(int32_t handle, uint64_t mode,
                                     const void *options, const void *reserved0, uint64_t reserved1);

/* Output-mode readback. ABI verified in third_party/ps5-opengl
 * (ps5_agc_native_runtime.c: runtime_resolution_status_t). */
typedef struct evo_vo_resolution_status {
    uint32_t full_width, full_height, pane_width, pane_height;
    uint64_t refresh_rate;
    float    screen_inches;
    uint32_t reserved[4];
} evo_vo_resolution_status;
int32_t sceVideoOutGetResolutionStatus(int32_t handle, evo_vo_resolution_status *status);

/* Display output status & dynamic range (SDR/HDR) readback. ABI verified in third_party/SharpProspero. */
typedef struct evo_vo_output_status {
    uint32_t resolution;
    uint32_t dynamic_range; /* 0 unknown, 1 SDR, 2 HDR */
    uint64_t refresh_rate;
    uint64_t flags;        /* bit 0: HDR output active */
    uint64_t reserved[3];
} evo_vo_output_status;
int32_t sceVideoOutGetOutputStatus(int32_t handle, evo_vo_output_status *status);
void    sceVideoOutSetBufferAttribute2(void *attribute, uint64_t format, uint32_t tiling,
                                       uint32_t width, uint32_t height, uint64_t option,
                                       uint32_t reserved0, uint64_t reserved1);
int32_t sceVideoOutRegisterBuffers2(int32_t handle, int32_t set_index, int32_t start_index,
                                    void *buffers, int32_t count, void *attribute,
                                    int32_t option, void *reserved);
int32_t sceVideoOutUnregisterBuffers(int32_t handle, int32_t set_index);
int32_t sceVideoOutWaitVblank(int32_t handle);
/* CPU-side flip - what pp_videoout.c used (hardware-verified) rather than
 * relying solely on the GPU's sceAgcDcbSetFlip packet. */
int32_t sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index,
                              int32_t flip_mode, int64_t flip_arg);
int32_t sceVideoOutGetFlipStatus(int32_t handle, void *status);

typedef struct {
    void *data;
    void *metadata;
    void *reserved0;
    void *reserved1;
} evo_video_buffer_t;

typedef struct {
    uint8_t reserved[80];
} evo_video_attribute_t;

typedef struct evo_agc_device {
    int                     initialized;
    int                     width;
    int                     height;
    int                     is_hdr;          /* scanout registered HDR10 right now */
    int                     display_is_hdr;
    int                     last_video_trc;  /* -1 none yet, else the frame's color_trc */
    int                     display_dynamic_range;
    uint32_t                display_resolution_token;
    int                     supports_120hz;
    int                     is_120hz;
    int                     current_refresh_rate;
    int                     is_player_mode;

    /* Per-scanout-buffer: UI was composited into it, so it cannot be reused
     * without a clear even in player mode. See evo_agc_runtime_note_ui_drawn. */
    int                     ui_dirty[2];
    /* Per-scanout-buffer: the video PTS its quad currently holds, or
     * INT64_MIN if it holds no video. See evo_agc_runtime_video_slot_stale. */
    int64_t                 video_pts[2];
    /* Last staged copy of each video plane, so redrawing the same frame into
     * the other scanout buffer does not re-copy it. See stage_plane. */
    struct {
        const uint8_t *src;
        int            src_pitch;
        uint32_t       width, height, bpp;
        int64_t        pts_us;
        uint64_t       frame_staged;
        uint32_t       gpu_pitch;
        uint64_t       gpu_addr;
        int            valid;
    }                       stage_cache[3];

    int32_t                 video_handle;
    int                     active_backbuffer;
    uint32_t                current_slot;
    uint64_t                frame_counter;
    int64_t                 flip_arg;

    int64_t                 direct_mem_offset;
    uint8_t                *direct_mem_base;
    size_t                  direct_mem_bytes;

    uint8_t                *scanout_buffers[2];
    evo_agc_transient_ring_t transient_ring;

    uint32_t               *dcb_slots[EVO_AGC_FRAME_SLOTS];
    uint32_t                dcb_slot_capacity_dwords;
    SceAgcCommandBuffer     current_cb;

    /* The GPU writes a 32-bit marker here at end-of-pipe (RELEASE_MEM event
     * 40). A slot is retired when its fence reads back the marker that slot's
     * last submit asked for - "wait for it to become 0" cannot work, because
     * the value written is whatever we pass to the packet. */
    volatile uint32_t      *fences[EVO_AGC_FRAME_SLOTS];
    uint32_t                fence_expect[EVO_AGC_FRAME_SLOTS];
    uint32_t                fence_marker;
    /* The token each slot's transient-ring region was sealed with. Reopening a
     * sealed slot requires handing that exact token back once the GPU is proven
     * done with it. */
    uint64_t                ring_token[EVO_AGC_FRAME_SLOTS];

    struct evo_agc_gpu_regs *gpu_regs;
    evo_agc_pipeline_t      pipelines[EVO_AGC_PIPE_COUNT];
    int                     bound_pipeline;

    uint16_t               *quad_indices;
    uint8_t                *composite_pixels;   /* 256-aligned, pitched */
    uint32_t                composite_pitch;
    void                   *composite_quad;     /* 4 verts, Rml::Vertex layout */
    uint8_t                *stencil_base;
    uint8_t                *depth_base;
    /* RmlUi layer surfaces (backdrop-filter / filter composition). Each is an
     * independent full-canvas RGBA8 target; `current_target` records which one
     * the DCB's MRT0 currently points at (NULL = scanout backbuffer), so
     * switching can skip redundant *RegistersIndirect packets. */
    evo_agc_layer_surface_t layers[EVO_AGC_MAX_LAYERS];
    const evo_agc_layer_surface_t *current_layer_target;
    /* Active scissor, mirrored from evo_agc_runtime_set_scissor. A pushed
     * layer is only required to be transparent-black inside it, so the
     * acquire clear uses this instead of wiping the whole canvas. */
    int scissor_x, scissor_y, scissor_w, scissor_h;
    /* RmlUi clip-mask state. Rather than clearing the stencil buffer before
     * every Set (a multi-MB fill per mask), each Set claims the next unused
     * value and the test compares against it. The buffer is zeroed once per
     * frame, so an unwritten texel can never collide with a live mask. */
    uint32_t                stencil_ref;        /* value the test compares to */
    uint32_t                stencil_counter;    /* last value handed out */
    int                     stencil_func_equal; /* 0 = NOTEQUAL (SetInverse) */
    int                     clip_mask_enabled;
    uint32_t                clip_mask_calls;    /* RenderToClipMask this frame */
    uint32_t                clip_enable_calls;  /* EnableClipMask this frame   */
    int                     frame_active;
    /* Whether any draw was emitted into the current frame's DCB. */
    int                     frame_has_draws;
    /* Health counters. Content silently vanishing as a frame gets busier looks
     * like "a black area creeping across the screen while things load", so each
     * way a draw can be dropped is counted rather than ignored. */
    uint32_t                ring_alloc_fail;
    uint32_t                tex_alloc_fail;
    uint32_t                dcb_peak_dwords;
    /* Smallest DCB actually presented in the current reporting window. A frame
     * that clears the whole back buffer and then draws only a fraction of the
     * UI flips a mostly-black screen - which is what a "black flash" is. */
    uint32_t                dcb_min_presented;
    uint32_t                presents;
    uint32_t                flip_waits;
    uint32_t                flip_timeouts;

    /* sceAgcGetRegisterDefaults(), kept for colour targets built per frame
     * (the upscaler's scratch surfaces change size with the source). */
    void                   *agc_defaults;

    /* #103 upscaler. See agc_upscale_* below. */
    struct {
        int          requested;        /* EVO_AGC_UPSCALE_* from Settings */
        int          cap;              /* highest mode GPU time allows */
        int          downgrade_notice; /* -1, or the mode just capped to */
        int          net_pref;         /* EVO_AGC_UPNET_* from Settings */
        int          net_cap;          /* largest network GPU time allows: 0 S, 1 M, 2 UL */
        const char  *label;            /* what the last frame used */
        /* Scratch surfaces, in two blocks of EVO_AGC_UP_SLOT_BYTES slots
         * allocated on first use: [0] the 5 every mode needs, [1] the 8 more
         * only Anime4K UL does. 0 = not yet, 1 = ok, -1 = failed for good. */
        int          alloc_state[2];
        int64_t      mem_offset[2];
        uint8_t     *mem_base[2];
        /* The last plan, so a change of source or mode is logged once. */
        int          last_key[6];
        /* This frame carried upscale passes; frame_end times its GPU work. */
        int          this_frame;
        uint32_t     window_frames;
        uint64_t     window_us;
        uint32_t     over_budget_windows;
    } up;
} evo_agc_device_t;

typedef struct evo_agc_gpu_regs {
    SceAgcRegister color_targets[2][16];
    SceAgcRegister layer_targets[EVO_AGC_MAX_LAYERS][16];
    SceAgcRegister depth_target[16];
    struct {
        SceAgcRegister cx_regs[128];
        SceAgcRegister sh_regs[32];
        SceAgcRegister uc_regs[8];
    } pipes[EVO_AGC_PIPE_COUNT];
} evo_agc_gpu_regs_t;

static evo_agc_device_t g_agc_dev = {0};

/* #103 upscaler, defined beside evo_agc_blit_yuv. */
static void agc_upscale_release(void);
static void agc_upscale_note_gpu_time(uint64_t us);

/*
 * Present path: no GPU SetFlip in the DCB; the CPU calls sceVideoOutSubmitFlip
 * once the frame's end-of-pipe fence retires.
 *
 * This was picked over the GPU-side flip because it is the one that is actually
 * proven on hardware here, and because the fence it waits on is what turned a
 * blank screen into a per-frame yes/no signal: a DCB with no draws retired in
 * ~0ms while the first DCB containing draws never retired at all, which is what
 * localised the GPU wedge to the draw. Revisit the in-DCB flip once the picture
 * is correct; it saves a CPU round trip per frame.
 */

/*
 * Read back what is ACTUALLY in the scanout after a frame, instead of reasoning
 * about registers. Logs a coarse luminance thumbnail plus how many pixels
 * differ from the backdrop clear. This is what turned "blank screen" into a
 * fact: changed=0 proved no fragment was reaching the colour target at all,
 * which killed the tiling theory and localised the bug to the draw.
 *
 *   all blank, changed==0  -> draws are not reaching the colour target
 *   recognisable layout    -> GPU drew correctly; the panel is descrambling it
 *   changed>0, no structure-> GPU wrote, but to the wrong addresses
 *
 * Runs once, on memory the GPU has already fenced as complete.
 */
static void agc_dump_scanout(const uint32_t *buf, int w, int h)
{
    static const char ramp[] = " .:-=+*#%@";
    const uint32_t backdrop = 0xff100d0du;
    size_t changed = 0;

    for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; ++i)
        if (buf[i] != backdrop)
            ++changed;

    evo_boot_log("agc scanout dump %dx%d changed=%zu (%.2f%%) px0=%08x px_mid=%08x",
                 w, h, changed, 100.0 * (double)changed / ((double)w * (double)h),
                 buf[0], buf[((size_t)h / 2) * (size_t)w + (size_t)w / 2]);

    /*
     * The most common non-backdrop colours actually sitting in the scanout.
     * Compare these against the RCSS palette (#38bdf8 sky blue, #2a3b55 navy,
     * #121b2e near-black navy): if the buffer holds 0x..38bdf8 the GPU wrote
     * the right bytes and the DISPLAY is reordering them; if it holds
     * 0x..f8bd38 the swap happened before the scanout. Guessing channel order
     * from screenshots has cost two builds - this settles which side it is on.
     */
    struct { uint32_t colour; uint32_t count; } top[6] = {{0, 0}};
    for (size_t i = 0; i < (size_t)w * (size_t)h; i += 16) {
        const uint32_t c = buf[i];
        if (c == backdrop)
            continue;
        int slot = -1;
        for (int k = 0; k < 6; ++k) {
            if (top[k].count && top[k].colour == c) { slot = k; break; }
            if (!top[k].count && slot < 0) slot = k;
        }
        if (slot < 0) continue;
        top[slot].colour = c;
        top[slot].count++;
    }
    for (int k = 0; k < 6; ++k)
        if (top[k].count)
            evo_boot_log("agc scanout top[%d] %08x n=%u", k, top[k].colour,
                         top[k].count);

    for (int cy = 0; cy < 18; ++cy) {
        char row[40];
        for (int cx = 0; cx < 32; ++cx) {
            const int x = (cx * w) / 32 + w / 64;
            const int y = (cy * h) / 18 + h / 36;
            const uint32_t p = buf[(size_t)y * (size_t)w + (size_t)x];
            const unsigned lum = (((p >> 16) & 0xff) * 77u +
                                  ((p >> 8) & 0xff) * 151u +
                                  (p & 0xff) * 28u) >> 8;
            row[cx] = ramp[(lum * 9u) / 255u];
        }
        row[32] = 0;
        evo_boot_log("agc scan[%02d] |%s|", cy, row);
    }
}

void evo_agc_runtime_cache_flush(const void *address, size_t bytes)
{
    if (!address || !bytes)
        return;
    /* Walk whole 64B cache lines: round the start down and the end up so a
     * range that doesn't happen to start/end on a line boundary still gets
     * every line it touches evicted. */
    const uint8_t *at  = (const uint8_t *)((uintptr_t)address & ~(uintptr_t)63);
    const uint8_t *end = (const uint8_t *)address + bytes;
    for (; at < end; at += 64)
        __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/*
 * Non-temporal fill/copy for the GPU-visible direct-memory buffers.
 *
 * Everything the CPU writes for the GPU - the backdrop clear, the staged video
 * planes, the OSD composite texture - lands in write-back memory that the
 * display controller and the command processor read straight from DRAM, so
 * every write used to be chased by evo_agc_runtime_cache_flush() walking the
 * range one 64-byte clflush at a time. At 1080p the backdrop clear alone is
 * 8.3 MB of stores plus ~130,000 clflushes, every frame the menu is up.
 *
 * movntdq writes through the write-combining buffers without allocating a
 * cache line, so the range needs no flush at all and one sfence orders the
 * whole batch. Every buffer these run on comes out of the 256-byte-aligned
 * direct-memory arena; the fallback covers a range that is not 16-byte
 * aligned or sized and keeps the old memcpy + clflush behaviour.
 *
 * Touch /mnt/usb0/evo_agc_no_nt to put every caller back on memcpy + clflush
 * without a rebuild, should a future firmware disagree about non-temporal
 * stores into the scanout pool.
 */
static int nt_stores_enabled(void)
{
    static int s_state = -1;
    if (s_state < 0)
        s_state = (access("/mnt/usb0/evo_agc_no_nt", F_OK) == 0) ? 0 : 1;
    return s_state;
}

void evo_agc_runtime_stream_fill(void *dst, uint32_t value32, size_t bytes)
{
    uint8_t *d = (uint8_t *)dst;
    if (!d || !bytes)
        return;
    if (!nt_stores_enabled() || ((uintptr_t)d & 15u) || (bytes & 15u)) {
        uint32_t *p = (uint32_t *)dst;
        size_t words = bytes / 4u;
        for (size_t i = 0; i < words; ++i)
            p[i] = value32;
        if (bytes & 3u)
            memset(d + words * 4u, (int)(value32 & 0xffu), bytes & 3u);
        evo_agc_runtime_cache_flush(dst, bytes);
        return;
    }
    const __m128i v = _mm_set1_epi32((int)value32);
    const size_t n = bytes / 16u;
    for (size_t i = 0; i < n; ++i)
        _mm_stream_si128(((__m128i *)d) + i, v);
    _mm_sfence();
}

/*
 * Bytes a scanout buffer actually occupies. The scanout is TILED in
 * PS5_TILE_WIDTH x PS5_TILE_HEIGHT (512x128) blocks, so a height that is not a
 * multiple of 128 - 2160 and 1080 both - is padded to a whole tile row, and the
 * surface is ceil(h/128)*128 rows of `width` long, not width*height.
 *
 * The backdrop clears used width*height*4, which stops 245,760 bytes short at
 * 4K. In the tile layout that tail is the bottom-right corner (the right end
 * of the last, partial tile row), so it was never cleared: the menu's
 * background stayed there as a dark rectangle in the letterbox bar through
 * every film, because the video quad does not cover the bars. The allocation
 * is EVO_AGC_SCANOUT_STRIDE (64 MB), far above the 33.4 MB this returns.
 */
static size_t scanout_tiled_bytes(void)
{
    const size_t rows = ((size_t)g_agc_dev.height + PS5_TILE_HEIGHT - 1) &
                        ~(size_t)(PS5_TILE_HEIGHT - 1);
    size_t bytes = rows * (size_t)g_agc_dev.width * 4u;
    if (bytes > (size_t)EVO_AGC_SCANOUT_STRIDE)
        bytes = (size_t)EVO_AGC_SCANOUT_STRIDE;
    return bytes;
}

/*
 * Copy `height` rows of `row_bytes` from a src pitch to a dst pitch. The dst
 * rows start 16-byte aligned (every staging pitch here is 256-byte aligned),
 * so only the ragged tail of a row whose width is not a multiple of 16 falls
 * back - and only that tail is flushed.
 */
static void stream_copy_rows(uint8_t *dst, size_t dst_pitch,
                             const uint8_t *src, size_t src_pitch,
                             size_t row_bytes, uint32_t height)
{
    if (!dst || !src || !row_bytes || !height)
        return;

    if (!nt_stores_enabled() || ((uintptr_t)dst & 15u) || (dst_pitch & 15u)) {
        for (uint32_t r = 0; r < height; ++r)
            memcpy(dst + (size_t)r * dst_pitch, src + (size_t)r * src_pitch,
                   row_bytes);
        evo_agc_runtime_cache_flush(dst,
                                    dst_pitch * (size_t)(height - 1u) + row_bytes);
        return;
    }

    const size_t bulk = row_bytes & ~(size_t)15;
    const size_t tail = row_bytes - bulk;
    const size_t n = bulk / 16u;

    for (uint32_t r = 0; r < height; ++r) {
        uint8_t *d = dst + (size_t)r * dst_pitch;
        const uint8_t *sp = src + (size_t)r * src_pitch;
        if (((uintptr_t)sp & 15u) == 0u) {
            for (size_t i = 0; i < n; ++i)
                _mm_stream_si128(((__m128i *)d) + i,
                                 _mm_load_si128(((const __m128i *)sp) + i));
        } else {
            for (size_t i = 0; i < n; ++i)
                _mm_stream_si128(((__m128i *)d) + i,
                                 _mm_loadu_si128(((const __m128i *)sp) + i));
        }
        if (tail) {
            memcpy(d + bulk, sp + bulk, tail);
            evo_agc_runtime_cache_flush(d + bulk, tail);
        }
    }
    _mm_sfence();
}

static SceAgcRegister *alloc_transient_cx(uint32_t count)
{
    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(&g_agc_dev.transient_ring,
                                     g_agc_dev.current_slot,
                                     count * sizeof(SceAgcRegister),
                                     16, &slice) != EVO_AGC_TRANSIENT_OK)
        return NULL;
    return (SceAgcRegister *)slice.cpu;
}

/* -------------------------------------------------------------------------
 * Pipeline construction
 *
 * The opengnm-psbc "package" path that used to live here - an ELF carrying
 * .shader_header / .shader_text, parsed by extract_shader_sections() and
 * validate_shader_header() - is gone along with the hand-written build wrapper
 * that produced it. Shaders now come from tools/build_agc_pipes.py: amdllpc
 * compiles a .pipe whose [ResourceMapping] declares the user-data layout next
 * to the shader source, and every AGC register is DERIVED from the PAL metadata
 * in the resulting ELF rather than chosen by a wrapper's command line.
 *
 * That wrapper shipped two silent, hardware-only defects this replaces:
 * VGT_ESGS_RING_ITEMSIZE packaged as 4 instead of 1 (which wedged the GPU on
 * the first DCB containing a real draw), and a vertex stage compiled with no
 * descriptor binding for its own uniform block (which drew zero fragments while
 * faulting nothing). Neither is expressible in the .pipe form.
 * ------------------------------------------------------------------------- */

static int compile_agc_pipeline(evo_agc_pipeline_t *pipe, uint8_t *storage_base,
                                size_t *storage_used,
                                const evo_agc_shader_metadata_t *meta,
                                const char *name)
{
    pipe->vs_shader = NULL;
    pipe->ps_shader = NULL;
    pipe->cx_reg_count = 0;
    pipe->sh_reg_count = 0;
    pipe->uc_reg_count = 0;
    pipe->draw_modifier = 0;
    pipe->valid = 0;

    if (!meta || !meta->gs_isa || !meta->ps_isa)
        return -1;

    const uint32_t gs_bytes = meta->gs_isa_bytes + EVO_AGC_SHADER_FOOTER_BYTES;
    const uint32_t ps_bytes = meta->ps_isa_bytes + EVO_AGC_SHADER_FOOTER_BYTES;

    /* Everything below lives in direct memory the GPU reads; the whole region
     * is cache-flushed once at the end of evo_agc_runtime_init(). */
    size_t at = (*storage_used + 0x3fffu) & ~(size_t)0x3fff;
    evo_agc_shader_arena_t *gs_arena = (evo_agc_shader_arena_t *)(storage_base + at);
    at = (at + sizeof(*gs_arena) + 0xffu) & ~(size_t)0xff;
    evo_agc_shader_arena_t *ps_arena = (evo_agc_shader_arena_t *)(storage_base + at);
    at = (at + sizeof(*ps_arena) + 0xffu) & ~(size_t)0xff;

    uint8_t *gs_code = storage_base + at;
    at = (at + gs_bytes + 0xffu) & ~(size_t)0xff;
    uint8_t *ps_code = storage_base + at;
    at = (at + ps_bytes + 0xffu) & ~(size_t)0xff;

    uint8_t *linked_cx = storage_base + at;
    at = (at + 0x1000u + 0xffu) & ~(size_t)0xff;
    uint8_t *linked_uc = storage_base + at;
    at = (at + 0x1000u + 0xffu) & ~(size_t)0xff;
    *storage_used = at;

    if (evo_agc_shader_header_build(gs_arena, EVO_AGC_SHADER_PRE_RASTER,
                                    gs_bytes, meta) != 0 ||
        evo_agc_shader_header_build(ps_arena, EVO_AGC_SHADER_PIXEL,
                                    ps_bytes, meta) != 0) {
        evo_boot_log("agc pipe %s: header build failed", name);
        return -2;
    }
    evo_agc_shader_write_code(gs_code, meta->gs_isa, meta->gs_isa_bytes);
    evo_agc_shader_write_code(ps_code, meta->ps_isa, meta->ps_isa_bytes);

    /* sceAgcCreateShader resolves the arena's self-relative pointer fields in
     * place and hands the arena back as the object, so a returned pointer that
     * is not the arena means the header was rejected. */
    int ret = sceAgcCreateShader(&pipe->vs_shader, gs_arena, gs_code);
    if (ret != 0 || pipe->vs_shader != gs_arena) {
        evo_boot_log("agc pipe %s: CreateShader(GS) rc=%d obj=%p arena=%p",
                     name, ret, pipe->vs_shader, (void *)gs_arena);
        return -3;
    }
    ret = sceAgcCreateShader(&pipe->ps_shader, ps_arena, ps_code);
    if (ret != 0 || pipe->ps_shader != ps_arena) {
        evo_boot_log("agc pipe %s: CreateShader(PS) rc=%d obj=%p arena=%p",
                     name, ret, pipe->ps_shader, (void *)ps_arena);
        return -4;
    }

    /* 4 = triangle list. */
    ret = sceAgcLinkShaders(linked_cx, linked_uc, NULL,
                            pipe->vs_shader, pipe->ps_shader, 4);
    if (ret != 0) {
        evo_boot_log("agc pipe %s: LinkShaders rc=%d", name, ret);
        return -5;
    }

    /* Register plan order matches ps5-opengl's append_shader_state and
     * ps5-xash3d's ps5_pipeline_build: the linker's 34 context registers, then
     * the pre-raster stage's 10, then the pixel stage's 9. Read straight out of
     * our own arenas rather than chasing pointers inside the shader object. */
    uint32_t cx_total = 0;
    memcpy(pipe->cx_regs, linked_cx, 34u * sizeof(SceAgcRegister));
    cx_total = 34u;
    memcpy(pipe->cx_regs + cx_total, gs_arena->cx,
           meta->pre_raster_cx_count * sizeof(SceAgcRegister));
    cx_total += meta->pre_raster_cx_count;
    memcpy(pipe->cx_regs + cx_total, ps_arena->cx,
           meta->pixel_cx_count * sizeof(SceAgcRegister));
    cx_total += meta->pixel_cx_count;
    /* SPI_PS_INPUT_CNTL_0..N - the parameter slot and flat/interpolated mode
     * for each pixel-stage input. These cannot go in the arena (its CX array
     * is a fixed 9 for the pixel stage), so they are appended here. */
    if (meta->ps_input_cntl && meta->ps_input_cntl_count &&
        cx_total + meta->ps_input_cntl_count <= 128u) {
        memcpy(pipe->cx_regs + cx_total, meta->ps_input_cntl,
               meta->ps_input_cntl_count * sizeof(SceAgcRegister));
        cx_total += meta->ps_input_cntl_count;
    }
    pipe->cx_reg_count = cx_total;

    memcpy(pipe->sh_regs, gs_arena->sh, 6u * sizeof(SceAgcRegister));
    memcpy(pipe->sh_regs + 6, ps_arena->sh, 6u * sizeof(SceAgcRegister));
    pipe->sh_reg_count = 12u;

    memcpy(pipe->uc_regs, linked_uc, 3u * sizeof(SceAgcRegister));
    pipe->uc_reg_count = 3u;

    pipe->draw_modifier = meta->draw_modifier;
    pipe->user_data = (evo_agc_user_data_layout_t){
        .vs_count = meta->vs_user_sgpr_count,
        .ps_count = meta->ps_user_sgpr_count,
        .vs_const_table_dword = meta->vs_const_table_dword,
        .vs_vertex_table_dword = meta->vs_vertex_table_dword,
        .ps_const_table_dword = meta->ps_const_table_dword,
        .ps_texture_table_dword = meta->ps_texture_table_dword,
    };
    pipe->valid = 1;

    /* The VS-out/PS-in linkage, logged the way ps5-opengl logs it under
     * PSBC_DEBUG_IO. gs_pgm/ps_pgm must be non-zero: those are the shader entry
     * addresses sceAgcCreateShader filled in. */
    evo_boot_log("agc pipe %s cx=%u sh=%u uc=%u modifier=%#llx "
                 "gs_pgm=%08x:%08x ps_pgm=%08x:%08x",
                 name, pipe->cx_reg_count, pipe->sh_reg_count,
                 pipe->uc_reg_count, (unsigned long long)pipe->draw_modifier,
                 gs_arena->sh[4].value, gs_arena->sh[5].value,
                 ps_arena->sh[2].value, ps_arena->sh[3].value);
    evo_boot_log("agc pipe %s user_data vs_n=%u ps_n=%u const=%d vtx=%d ps_const=%d tex=%d",
                 name, meta->vs_user_sgpr_count, meta->ps_user_sgpr_count,
                 meta->vs_const_table_dword, meta->vs_vertex_table_dword,
                 meta->ps_const_table_dword, meta->ps_texture_table_dword);
    return 0;
}

/* -------------------------------------------------------------------------
 * Target & Surface Defaults Setup
 * ------------------------------------------------------------------------- */

static int setup_color_target(SceAgcRegister out[16], void *defaults, void *target_addr,
                              uint32_t width, uint32_t height, int comp_swap_alt)
{
    static const uint16_t target_offsets[16] = {
        0x318, 0x31b, 0x31c, 0x31d, 0x31e, 0x31f, 0x321, 0x323,
        0x324, 0x325, 0x390, 0x398, 0x3a0, 0x3a8, 0x3b0, 0x3b8
    };

    SceAgcRegister **blocks = *(SceAgcRegister ***)defaults;
    uint32_t default_count = *(uint32_t *)((uint8_t *)defaults + 0x20);

    if (!blocks || !blocks[0])
        return -1;

    for (uint32_t i = 0; i < 16; ++i) {
        uint32_t candidate;
        out[i] = (SceAgcRegister){target_offsets[i], 0};
        for (candidate = 0; candidate < default_count; ++candidate) {
            if (blocks[0][candidate].offset == target_offsets[i]) {
                out[i].value = blocks[0][candidate].value;
                break;
            }
        }
        if (candidate == default_count)
            return -1;
    }

    uintptr_t target = (uintptr_t)target_addr;
    out[0].value = (uint32_t)(target >> 8);
    out[1].value &= 0xfc001fffu;
    /*
     * CB_COLOR0_INFO. FORMAT=COLOR_8_8_8_8 (0x28), plus COMP_SWAP=ALT in bits
     * [12:11] when comp_swap_alt is set, so the colour block stores B,G,R,A
     * instead of R,G,B,A.
     *
     * The scanout really is BGRA-ordered. Measured, not assumed: with COMP_SWAP
     * left at STD the framebuffer held ff160a05 and ffedbe00 - correct RGBA for
     * the theme's #121b2e navy and #00cdff cyan - while the panel displayed
     * those same pixels as brown and gold. So the GPU was writing the right
     * bytes and the display was reading them in the other order.
     *
     * ps5-opengl gets away with COMP_SWAP=STD because Mesa bakes the swizzle
     * into the shader for a BGRA pipe format; doing it here keeps the shader
     * exporting plain RGBA, which is what the .pipe and the texture descriptors
     * already agree on.
     *
     * Layer surfaces pass comp_swap_alt=0 and keep COMP_SWAP=STD: their memory
     * order (R,G,B,A) must match the rgba8 T# swizzle the blur/composite
     * shaders sample them with. Only the scanout backbuffer is BGRA (and is
     * sampled with the bgra8 swizzle instead).
     */
    out[2].value = (out[2].value &
                    ~(0x7cu | 0x700u | 0x1800u | 0x10000000u |
                      0x10000u | 0x8000u | 0x40000u | 0x4000u)) |
                   0x28u | 0x8000u | (comp_swap_alt ? 0x800u /* COMP_SWAP = ALT (BGRA) */ : 0u);
    out[3].value &= ~(0x7000u | 0x18000u);
    out[4].value = (out[4].value &
                    ~(0x60u | 0x0cu | 0x00100200u | 0x80000u)) |
                   0x48u;
    out[5].value = out[6].value = out[9].value = 0;
    out[10].value = (out[10].value & 0xffffff00u) | (uint32_t)(target >> 40);
    out[11].value &= 0xffffff00u;
    out[12].value &= 0xffffff00u;
    out[13].value &= 0xffffff00u;
    out[14].value = (height - 1u) | ((width - 1u) << 14);
    out[15].value = (out[15].value &
                     ~(0x1fffu | 0x7c000u | 0x03000000u | 0x44000000u)) |
                    0x6c000u | 0x01000000u | 0x44000000u;
    return 0;
}

/* -------------------------------------------------------------------------
 * Stencil / clip mask
 *
 * RmlUi clips to non-rectangular shapes - rounded-corner containers and masked
 * overlays - through EnableClipMask()/RenderToClipMask(). The AGC interface
 * implemented neither, so those calls hit base-class no-ops: rounded containers
 * drew rounded but their children were never clipped to them (square thumbnail
 * corners), and gradient masks covered the wrong region. Anything rectangular
 * still worked, because that goes through the scissor instead.
 *
 * Register layout follows ps5-opengl's append_depth_target_state().
 * ------------------------------------------------------------------------- */

enum {
    /* DB_DEPTH_CONTROL (0x200) */
    DB_STENCIL_ENABLE   = 1u << 0,
    DB_Z_ENABLE         = 1u << 1,      /* must be on even for stencil-only */
    DB_ZFUNC_SHIFT      = 4,            /* bits [6:4] */
    DB_STENCILFUNC_SHIFT = 8,           /* bits [10:8] */
    /* Compare functions */
    DB_CMP_NEVER = 0, DB_CMP_EQUAL = 2, DB_CMP_NOTEQUAL = 5, DB_CMP_ALWAYS = 7,
    /* Stencil ops, DB_STENCIL_CONTROL (0x10b) */
    DB_STENCIL_KEEP = 0, DB_STENCIL_REPLACE = 2, DB_STENCIL_INCR_CLAMP = 3,
};

static int setup_depth_target(SceAgcRegister out[16], const uint8_t *depth,
                              const uint8_t *stencil,
                              uint32_t width, uint32_t height)
{
    static const uint16_t db_offsets[16] = {
        0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x001a, 0x001b,
        0x001c, 0x001d, 0x001e, 0x0002, 0x0005, 0x0007, 0x000b, 0x000a,
    };
    const uintptr_t z = (uintptr_t)depth;
    const uintptr_t s = (uintptr_t)stencil;
    for (uint32_t i = 0; i < 16; ++i)
        out[i] = (SceAgcRegister){db_offsets[i], 0u};

    /* Exactly ps5-opengl's values. An earlier revision set FORMAT=0 here to
     * avoid allocating a depth surface, reasoning that only stencil was needed;
     * the stencil planes then never went live and every clip mask tested as
     * empty, so content inside a rounded container vanished. The depth surface
     * is allocated and configured even though the depth TEST stays off. */
    out[0].value = 0x80000183u;                 /* DB_Z_INFO: D32F, 64KB_Z_X   */
    out[1].value = 0x20000181u;                 /* DB_STENCIL_INFO: S8, 64KB_Z_X */
    out[2].value = (uint32_t)(z >> 8);          /* Z_READ_BASE        */
    out[3].value = (uint32_t)(s >> 8);          /* STENCIL_READ_BASE  */
    out[4].value = (uint32_t)(z >> 8);          /* Z_WRITE_BASE       */
    out[5].value = (uint32_t)(s >> 8);          /* STENCIL_WRITE_BASE */
    out[6].value = (uint32_t)(z >> 40);         /* Z_READ_BASE_HI     */
    out[7].value = (uint32_t)(s >> 40);         /* STENCIL_READ_BASE_HI  */
    out[8].value = (uint32_t)(z >> 40);         /* Z_WRITE_BASE_HI    */
    out[9].value = (uint32_t)(s >> 40);         /* STENCIL_WRITE_BASE_HI */
    out[13].value = (width - 1u) | ((height - 1u) << 16);
    return 0;
}

/* Emit the four stencil state registers for the current mask state. */
static void emit_stencil_state(int stencil_enable, uint32_t func, uint32_t ref,
                               uint32_t zpass_op, int colour_writes)
{
    SceAgcRegister *r = alloc_transient_cx(5);
    if (!r)
        return;
    /* Z_ENABLE with ZFUNC=ALWAYS and Z_WRITE_ENABLE off: the depth test always
     * passes and nothing is written, but the depth block stays active. Leaving
     * Z_ENABLE clear bypasses DB entirely, so the stencil op never executes and
     * every mask reads back empty - three earlier attempts at this bug were
     * looking at the stencil registers when the depth enable was the problem. */
    r[0] = (SceAgcRegister){0x0200, stencil_enable
                                ? (DB_STENCIL_ENABLE | DB_Z_ENABLE |
                                   ((uint32_t)DB_CMP_ALWAYS << DB_ZFUNC_SHIFT) |
                                   (func << DB_STENCILFUNC_SHIFT))
                                : 0u};
    r[1] = (SceAgcRegister){0x010b, (uint32_t)DB_STENCIL_KEEP |
                                    (zpass_op << 4) |
                                    ((uint32_t)DB_STENCIL_KEEP << 8)};
    /* ref | read mask | write mask | STENCILOPVAL.
     * REPLACE writes STENCILTESTVAL, but INCR/DECR step by STENCILOPVAL in bits
     * [31:24]; leaving it 0 made Intersect increment by nothing. */
    const uint32_t refmask = (ref & 0xffu) | (0xffu << 8) | (0xffu << 16) |
                             (1u << 24);
    r[2] = (SceAgcRegister){0x010c, refmask};
    r[3] = (SceAgcRegister){0x010d, refmask};
    /* CB_TARGET_MASK: writing the mask must not touch colour. */
    r[4] = (SceAgcRegister){0x008e, colour_writes ? 0x0000000fu : 0u};
    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, r, 5);
}

/*
 * Clip masks are OFF by default.
 *
 * The stencil path is written and the DB registers demonstrably bind
 * (z_info=0x80000183 s_info=0x20000181 in the boot log), but on hardware every
 * mask tests as empty: content drawn inside a rounded container disappears
 * while everything outside one renders normally. Until that is understood, the
 * feature fails OPEN - square corners with visible content beats rounded
 * corners with none.
 *
 * Modes: 0 = off entirely. 1 = full (write + test). 2 = DIAGNOSTIC - write the
 * masks but never enable the test, so the UI is identical to mode 0 while the
 * stencil buffer still gets written. Mode 2 plus the read-back below answers
 * "do the mask writes land at all?" without shipping a broken screen.
 *
 * Build with -DEVO_AGC_CLIP_MASK=1 to turn it fully on for debugging. The
 * counters below are logged either way; they show RmlUi issuing ~13 masks per
 * frame, so this is worth finishing.
 *
 * Ruled out by hardware, do not retry:
 *   1. DB_Z_INFO FORMAT=0 with no depth surface  - allocate D32F, use
 *      0x80000183 exactly as ps5-opengl does.
 *   2. STENCILOPVAL left 0 (bits [31:24] of DB_STENCILREFMASK) - breaks
 *      INCR/Intersect, but not plain Set.
 *   3. DB_DEPTH_CONTROL with Z_ENABLE clear - tried both with and without;
 *      neither makes the mask test pass.
 *   4. Clearing only width*height bytes of a tiled S8 surface instead of its
 *      full ~2.6 MB footprint.
 * All four are fixed in the code below and the masks still test empty, so the
 * fault is elsewhere. The next step is NOT another register guess: read the
 * stencil buffer back after a masked frame (as agc_dump_scanout does for
 * colour) and count non-zero bytes. Zero means the mask WRITE never happens and
 * the fault is in the DB/write path; non-zero means writes land and the
 * comparison is wrong. That splits the problem in one launch.
 */
#ifndef EVO_AGC_CLIP_MASK
#define EVO_AGC_CLIP_MASK 0
#endif

int evo_agc_runtime_clip_mask_supported(void)
{
    return EVO_AGC_CLIP_MASK;
}

void evo_agc_runtime_set_clip_mask(int enable)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    g_agc_dev.clip_enable_calls++;
    if (EVO_AGC_CLIP_MASK != 1)
        return;          /* mode 2 writes masks but never tests against them */
    g_agc_dev.clip_mask_enabled = enable ? 1 : 0;
    emit_stencil_state(g_agc_dev.clip_mask_enabled,
                       g_agc_dev.stencil_func_equal ? DB_CMP_EQUAL : DB_CMP_NOTEQUAL,
                       g_agc_dev.stencil_ref, DB_STENCIL_KEEP, 1);
}

void evo_agc_runtime_clip_mask_begin(int operation)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    g_agc_dev.clip_mask_calls++;
    if (!EVO_AGC_CLIP_MASK)
        return;
    uint32_t zpass;
    switch (operation) {
    case 0:  /* Set - claim a fresh value and stamp it where the geometry covers */
    case 1:  /* SetInverse - same stamp, but the test is inverted afterwards */
        if (g_agc_dev.stencil_counter >= 255u)
            g_agc_dev.stencil_counter = 0u;   /* 255 masks/frame is not a real case */
        g_agc_dev.stencil_ref = ++g_agc_dev.stencil_counter;
        g_agc_dev.stencil_func_equal = (operation == 0);
        zpass = DB_STENCIL_REPLACE;
        break;
    default: /* Intersect - increment, so only texels already carrying the
              * previous value reach ref+1 */
        g_agc_dev.stencil_ref += 1u;
        g_agc_dev.stencil_func_equal = 1;
        zpass = DB_STENCIL_INCR_CLAMP;
        break;
    }
    /* Always pass the test while writing the mask; colour writes off. */
    emit_stencil_state(1, DB_CMP_ALWAYS, g_agc_dev.stencil_ref, zpass, 0);
}

void evo_agc_runtime_clip_mask_end(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    if (EVO_AGC_CLIP_MASK != 1) {
        /* Diagnostic mode: stop writing the mask and restore colour writes, but
         * leave the stencil test disabled so drawing is unaffected. */
        emit_stencil_state(0, DB_CMP_ALWAYS, 0u, DB_STENCIL_KEEP, 1);
        return;
    }
    g_agc_dev.clip_mask_enabled = 1;
    emit_stencil_state(1, g_agc_dev.stencil_func_equal ? DB_CMP_EQUAL : DB_CMP_NOTEQUAL,
                       g_agc_dev.stencil_ref, DB_STENCIL_KEEP, 1);
}

/* -------------------------------------------------------------------------
 * Public AGC Runtime Lifecycle
 * ------------------------------------------------------------------------- */

/*
 * Bind a render size to the surfaces. Split out of evo_agc_runtime_init so it
 * can run twice: once with the size the caller asked for, then again once the
 * VideoOut handle exists and the panel has told us what it is actually
 * running at. Only touches CPU-side register images and offsets into memory
 * that is already carved, so the second call is free of any sce* ordering -
 * in particular it never re-opens VideoOut or allocates a queue.
 */
static int evo_agc_apply_render_size(void *agc_defaults, int w, int h)
{
    g_agc_dev.width  = w;
    g_agc_dev.height = h;

    g_agc_dev.composite_pitch = ((uint32_t)w * 4u + 255u) & ~255u;
    g_agc_dev.composite_quad  = g_agc_dev.composite_pixels +
        ((((size_t)g_agc_dev.composite_pitch * (size_t)h) + 255u) & ~(size_t)255);

    int ct0 = setup_color_target(g_agc_dev.gpu_regs->color_targets[0], agc_defaults,
                                 g_agc_dev.scanout_buffers[0], w, h, 1);
    int ct1 = setup_color_target(g_agc_dev.gpu_regs->color_targets[1], agc_defaults,
                                 g_agc_dev.scanout_buffers[1], w, h, 1);
    setup_depth_target(g_agc_dev.gpu_regs->depth_target, g_agc_dev.depth_base,
                       g_agc_dev.stencil_base, (uint32_t)w, (uint32_t)h);

    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        /* COMP_SWAP stays STD for layers: their R,G,B,A memory order must match
         * the rgba8 T# they are sampled back through. */
        if (setup_color_target(layer->mrt, agc_defaults, layer->cpu_base, w, h, 0) != 0) {
            printf(EVO_AGC_LOG_PREFIX "setup layer target %d failed\n", i);
            return -1;
        }
        layer->width = (uint32_t)w;
        layer->height = (uint32_t)h;
        layer->pitch_bytes = ((uint32_t)w * 4u + 255u) & ~255u;
    }
    evo_agc_runtime_cache_flush(g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));

    if (ct0 != 0 || ct1 != 0) {
        printf(EVO_AGC_LOG_PREFIX "setup_color_target failed: %d/%d\n", ct0, ct1);
        return -1;
    }
    return 0;
}

int evo_agc_runtime_init(int width, int height, int hdr)
{
    if (g_agc_dev.initialized)
        return 0;

    printf(EVO_AGC_LOG_PREFIX "Initializing Bare-Metal AGC (%dx%d, HDR=%d)...\n",
           width, height, hdr);

    memset(&g_agc_dev, 0, sizeof(g_agc_dev));
    g_agc_dev.width = width ? width : 1920;
    g_agc_dev.height = height ? height : 1080;
    g_agc_dev.is_hdr = hdr;
    g_agc_dev.last_video_trc = -1;
    g_agc_dev.video_handle = -1;
    g_agc_dev.up.cap = EVO_AGC_UPSCALE_AI;
    g_agc_dev.up.downgrade_notice = -1;
    g_agc_dev.up.net_cap = 2;
    g_agc_dev.up.label = "Off";
    g_agc_dev.up.mem_offset[0] = g_agc_dev.up.mem_offset[1] = -1;

    /* 1. Allocate Direct Memory Pool */
    int ret = sceKernelAllocateDirectMemory(
        0, (off_t)16 * 1024 * 1024 * 1024ULL, EVO_AGC_TOTAL_DIRECT_MEM,
        EVO_AGC_DIRECT_MEM_ALIGN, EVO_AGC_DIRECT_MEM_TYPE, &g_agc_dev.direct_mem_offset);
    if (ret != 0 || g_agc_dev.direct_mem_offset < 0) {
        printf(EVO_AGC_LOG_PREFIX "Failed to allocate direct memory: %d\n", ret);
        return -1;
    }

    void *mapped = NULL;
    ret = sceKernelMapDirectMemory(&mapped, EVO_AGC_TOTAL_DIRECT_MEM,
                                   EVO_AGC_MAP_PROTECTION, 0,
                                   g_agc_dev.direct_mem_offset, EVO_AGC_DIRECT_MEM_ALIGN);
    if (ret != 0 || !mapped) {
        printf(EVO_AGC_LOG_PREFIX "Failed to map direct memory: %d\n", ret);
        sceKernelReleaseDirectMemory(g_agc_dev.direct_mem_offset, EVO_AGC_TOTAL_DIRECT_MEM);
        return -2;
    }

    g_agc_dev.direct_mem_base = (uint8_t *)mapped;
    g_agc_dev.direct_mem_bytes = EVO_AGC_TOTAL_DIRECT_MEM;
    memset(g_agc_dev.direct_mem_base, 0, g_agc_dev.direct_mem_bytes);

    /* 2. Subdivide Direct Memory */
    size_t cur_offset = 0;

    /* Scanout buffer 0 & 1 */
    g_agc_dev.scanout_buffers[0] = g_agc_dev.direct_mem_base + cur_offset;
    cur_offset += EVO_AGC_SCANOUT_STRIDE;
    g_agc_dev.scanout_buffers[1] = g_agc_dev.direct_mem_base + cur_offset;
    cur_offset += EVO_AGC_SCANOUT_STRIDE;

    /* Transient ring buffer (EVO_AGC_TRANSIENT_RING_SIZE) */
    uint8_t *transient_base = g_agc_dev.direct_mem_base + cur_offset;
    evo_agc_transient_ring_init(&g_agc_dev.transient_ring, transient_base,
                                (uint64_t)(uintptr_t)transient_base,
                                EVO_AGC_TRANSIENT_RING_SIZE,
                                EVO_AGC_FRAME_SLOTS, 256);
    cur_offset += EVO_AGC_TRANSIENT_RING_SIZE;

    /* Command buffers (3 slots, 2 MB each = 6 MB) */
    const size_t dcb_slot_bytes = 2 * 1024 * 1024;
    g_agc_dev.dcb_slot_capacity_dwords = (uint32_t)(dcb_slot_bytes / sizeof(uint32_t));
    for (int i = 0; i < EVO_AGC_FRAME_SLOTS; ++i) {
        g_agc_dev.dcb_slots[i] = (uint32_t *)(g_agc_dev.direct_mem_base + cur_offset);
        cur_offset += dcb_slot_bytes;
    }

    /* Fence markers */
    uint8_t *fence_base = g_agc_dev.direct_mem_base + cur_offset;
    for (int i = 0; i < EVO_AGC_FRAME_SLOTS; ++i) {
        g_agc_dev.fences[i] = (volatile uint32_t *)(fence_base + i * 256);
        *g_agc_dev.fences[i] = 0;
        g_agc_dev.fence_expect[i] = 0; /* 0 = never submitted, nothing to wait for */
    }
    g_agc_dev.fence_marker = 0;
    cur_offset += EVO_AGC_FENCE_STORAGE_SIZE;

    /* GPU-mapped registers for color targets and pipeline states */
    g_agc_dev.gpu_regs = (evo_agc_gpu_regs_t *)(g_agc_dev.direct_mem_base + cur_offset);
    cur_offset += (sizeof(evo_agc_gpu_regs_t) + 255u) & ~255u;
    memset(g_agc_dev.gpu_regs, 0, sizeof(evo_agc_gpu_regs_t));

    for (int i = 0; i < EVO_AGC_PIPE_COUNT; ++i) {
        g_agc_dev.pipelines[i].cx_regs = g_agc_dev.gpu_regs->pipes[i].cx_regs;
        g_agc_dev.pipelines[i].sh_regs = g_agc_dev.gpu_regs->pipes[i].sh_regs;
        g_agc_dev.pipelines[i].uc_regs = g_agc_dev.gpu_regs->pipes[i].uc_regs;
    }

    /* OSD composite staging texture + its quad. 256-byte aligned because a
     * GFX10 image descriptor stores address>>8. */
    {
        uint8_t *comp = g_agc_dev.direct_mem_base + cur_offset;
        comp = (uint8_t *)(((uintptr_t)comp + 255u) & ~(uintptr_t)255);
        g_agc_dev.composite_pitch =
            ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
        g_agc_dev.composite_pixels = comp;
        g_agc_dev.composite_quad = comp +
            ((size_t)g_agc_dev.composite_pitch * (size_t)g_agc_dev.height + 255u
             & ~(size_t)255);
        cur_offset += EVO_AGC_COMPOSITE_SIZE;
    }

    /* Stencil buffer, 2 MB aligned like the scanout: DB bases are address>>8
     * and the surface is tiled, so a tightly-aligned base is not enough. */
    {
        uint8_t *st = g_agc_dev.direct_mem_base + cur_offset;
        g_agc_dev.stencil_base =
            (uint8_t *)(((uintptr_t)st + 0x1fffffu) & ~(uintptr_t)0x1fffff);
        cur_offset += EVO_AGC_STENCIL_SIZE;

        uint8_t *dp = g_agc_dev.direct_mem_base + cur_offset;
        g_agc_dev.depth_base =
            (uint8_t *)(((uintptr_t)dp + 0x1fffffu) & ~(uintptr_t)0x1fffff);
        cur_offset += EVO_AGC_DEPTH_SIZE;
    }

    /* Layer surfaces for RmlUi backdrop-filter: full-canvas RGBA8 targets,
     * COMP_SWAP=STD so the rgba8 T# can sample them back. 256-byte aligned for
     * the image descriptors (base stored as >>8), and each is 32 MB - 4K RGBA8
     * needs 31.64 MB, so 4K straddles a 32 MB slot exactly. */
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        uint8_t *lg = g_agc_dev.direct_mem_base + cur_offset;
        layer->cpu_base = (uint8_t *)(((uintptr_t)lg + 255u) & ~(uintptr_t)255);
        layer->gpu_addr = (uint64_t)(uintptr_t)layer->cpu_base;
        layer->pool_index = i;
        layer->in_use = 0;
        layer->pitch_bytes = 0; /* set with width/height below */
        layer->mrt = g_agc_dev.gpu_regs->layer_targets[i];
        cur_offset += EVO_AGC_LAYER_BYTES;
    }

    /* Shader storage */
    uint8_t *shader_storage = g_agc_dev.direct_mem_base + cur_offset;
    size_t shader_storage_used = 0;

    /* 3. Initialize AGC Hardware */
    ret = sceAgcInit(8);
    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcInit failed: %d\n", ret);
        goto cleanup_fail;
    }

    void *agc_defaults = sceAgcGetRegisterDefaults();
    if (!agc_defaults) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcGetRegisterDefaults failed\n");
        goto cleanup_fail;
    }

    g_agc_dev.agc_defaults = agc_defaults;

    /* 4. Setup MRT0 Color Targets for Scanouts */
    /* A failure here leaves the MRT0 registers partly zeroed, which the GPU
     * happily accepts and then draws nothing into - exactly the symptom this
     * path spent a session chasing. It was being called for its side effect
     * with the result dropped; fail the init instead. */
    int ct0 = setup_color_target(g_agc_dev.gpu_regs->color_targets[0], agc_defaults,
                                 g_agc_dev.scanout_buffers[0], g_agc_dev.width, g_agc_dev.height, 1);
    int ct1 = setup_color_target(g_agc_dev.gpu_regs->color_targets[1], agc_defaults,
                                 g_agc_dev.scanout_buffers[1], g_agc_dev.width, g_agc_dev.height, 1);
    setup_depth_target(g_agc_dev.gpu_regs->depth_target, g_agc_dev.depth_base,
                       g_agc_dev.stencil_base,
                       (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        if (setup_color_target(layer->mrt, agc_defaults, layer->cpu_base,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height, 0) != 0) {
            printf(EVO_AGC_LOG_PREFIX "setup layer target %d failed\n", i);
            goto cleanup_fail;
        }
        layer->width = (uint32_t)g_agc_dev.width;
        layer->height = (uint32_t)g_agc_dev.height;
        layer->pitch_bytes = ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
    }
    evo_boot_log("agc stencil base=%p z_info=%#x s_info=%#x size_xy=%#x",
                 (void *)g_agc_dev.stencil_base,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[0].value,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[1].value,
                 (unsigned)g_agc_dev.gpu_regs->depth_target[13].value);
    evo_boot_log("agc color_target rc=%d/%d base0=%#x info=%#x (comp_swap=%u) view=%#x attrib=%#x",
                 ct0, ct1,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][0].value,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][2].value,
                 (unsigned)((g_agc_dev.gpu_regs->color_targets[0][2].value >> 11) & 3u),
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][1].value,
                 (unsigned)g_agc_dev.gpu_regs->color_targets[0][15].value);
    if (ct0 != 0 || ct1 != 0) {
        printf(EVO_AGC_LOG_PREFIX "setup_color_target failed: %d/%d\n", ct0, ct1);
        goto cleanup_fail;
    }

    /* 5. Compile All AGC Pipelines */
    /* Only the UI pipeline is required. The video pipelines are converted to
     * .pipe form one at a time; a missing or failed one disables that path
     * rather than aborting the whole runtime, so the UI can be brought up and
     * verified independently. */
    ret = compile_agc_pipeline(&g_agc_dev.pipelines[EVO_AGC_PIPE_UI],
                               shader_storage, &shader_storage_used,
                               &ui_screen_2d_metadata, "ui_screen_2d");
    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "UI pipeline failed to compile: %d\n", ret);
        goto cleanup_fail;
    }

#ifdef EVO_AGC_HAVE_VIDEO_PIPES
    static const struct {
        int pipe_id;
        const evo_agc_shader_metadata_t *meta;
        const char *name;
    } video_pipes[] = {
        {EVO_AGC_PIPE_VIDEO_NV12,   &video_yuv_nv12_metadata,     "video_yuv_nv12"},
        {EVO_AGC_PIPE_VIDEO_HDR,    &video_yuv_p010_hdr_metadata, "video_yuv_p010_hdr"},
        {EVO_AGC_PIPE_VIDEO_HLG,    &video_yuv_p010_hlg_metadata, "video_yuv_p010_hlg"},
        {EVO_AGC_PIPE_VIDEO_P010_SDR, &video_yuv_p010_sdr_metadata, "video_yuv_p010_sdr"},
        /* real HDR10 output; missing ones just keep playback tone-mapped SDR */
        {EVO_AGC_PIPE_VIDEO_HDR_PQ, &video_yuv_p010_pq_out_metadata,     "video_yuv_p010_pq_out"},
        {EVO_AGC_PIPE_VIDEO_HLG_PQ, &video_yuv_p010_hlg_pq_out_metadata, "video_yuv_p010_hlg_pq_out"},
        {EVO_AGC_PIPE_UI_PQ,        &ui_screen_2d_pq_out_metadata,       "ui_screen_2d_pq_out"},
        {EVO_AGC_PIPE_NV12_HDR,     &video_yuv_nv12_hdr_metadata,        "video_yuv_nv12_hdr"},
        {EVO_AGC_PIPE_NV12_HLG,     &video_yuv_nv12_hlg_metadata,        "video_yuv_nv12_hlg"},
        {EVO_AGC_PIPE_NV12_HDR_PQ,  &video_yuv_nv12_pq_out_metadata,     "video_yuv_nv12_pq_out"},
        {EVO_AGC_PIPE_NV12_HLG_PQ,  &video_yuv_nv12_hlg_pq_out_metadata, "video_yuv_nv12_hlg_pq_out"},
        {EVO_AGC_PIPE_VIDEO_PLANAR, &video_yuv_planar_metadata,   "video_yuv_planar"},
        /* #103 upscaler. Each is optional: a missing one makes the upscaler
         * fall back a mode (AI -> Sharp -> Off), never fail the runtime. */
        {EVO_AGC_PIPE_UP_EASU,      &upscale_easu_metadata,       "upscale_easu"},
        {EVO_AGC_PIPE_UP_RCAS,      &upscale_rcas_metadata,       "upscale_rcas"},
        {EVO_AGC_PIPE_UP_A4K_FINAL, &upscale_a4k_final_metadata,  "upscale_a4k_final"},
        {EVO_AGC_PIPE_UP_S_CONV0 + 0, &upscale_a4k_s_conv0_metadata, "upscale_a4k_s_conv0"},
        {EVO_AGC_PIPE_UP_S_CONV0 + 1, &upscale_a4k_s_conv1_metadata, "upscale_a4k_s_conv1"},
        {EVO_AGC_PIPE_UP_S_CONV0 + 2, &upscale_a4k_s_conv2_metadata, "upscale_a4k_s_conv2"},
        {EVO_AGC_PIPE_UP_S_CONV0 + 3, &upscale_a4k_s_conv3_metadata, "upscale_a4k_s_conv3"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 0, &upscale_a4k_m_conv0_metadata, "upscale_a4k_m_conv0"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 1, &upscale_a4k_m_conv1_metadata, "upscale_a4k_m_conv1"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 2, &upscale_a4k_m_conv2_metadata, "upscale_a4k_m_conv2"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 3, &upscale_a4k_m_conv3_metadata, "upscale_a4k_m_conv3"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 4, &upscale_a4k_m_conv4_metadata, "upscale_a4k_m_conv4"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 5, &upscale_a4k_m_conv5_metadata, "upscale_a4k_m_conv5"},
        {EVO_AGC_PIPE_UP_M_CONV0 + 6, &upscale_a4k_m_conv6_metadata, "upscale_a4k_m_conv6"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 0, &upscale_a4k_m_acc0_metadata, "upscale_a4k_m_acc0"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 1, &upscale_a4k_m_acc1_metadata, "upscale_a4k_m_acc1"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 2, &upscale_a4k_m_acc2_metadata, "upscale_a4k_m_acc2"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 3, &upscale_a4k_m_acc3_metadata, "upscale_a4k_m_acc3"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 4, &upscale_a4k_m_acc4_metadata, "upscale_a4k_m_acc4"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 5, &upscale_a4k_m_acc5_metadata, "upscale_a4k_m_acc5"},
        {EVO_AGC_PIPE_UP_M_ACC0 + 6, &upscale_a4k_m_acc6_metadata, "upscale_a4k_m_acc6"},
        {EVO_AGC_PIPE_UP_RGB_FINAL, &upscale_a4k_rgb_final_metadata, "upscale_a4k_rgb_final"},
#include "upscale_wide_pipes.inc"
    };
    for (unsigned i = 0; i < sizeof(video_pipes) / sizeof(video_pipes[0]); ++i) {
        int vrc = compile_agc_pipeline(&g_agc_dev.pipelines[video_pipes[i].pipe_id],
                                       shader_storage, &shader_storage_used,
                                       video_pipes[i].meta, video_pipes[i].name);
        if (vrc != 0)
            evo_boot_log("agc pipe %s unavailable (rc=%d); that video path is off",
                         video_pipes[i].name, vrc);
    }
#else
    evo_boot_log("agc video pipelines not built yet (.pipe conversion pending)");
#endif

    /* The backdrop-blur pipe (backdrop-filter for RmlUi): the fragment shader
     * owns both its BlurConstants buffer AND its source texture, so its user
     * data has a PS const table in addition to the PS texture table. Optional
     * like the video pipes - a failure just disables backdrop blur. */
    {
        int brc = compile_agc_pipeline(&g_agc_dev.pipelines[EVO_AGC_PIPE_UI_BLUR],
                                       shader_storage, &shader_storage_used,
                                       &ui_backdrop_blur_metadata, "ui_backdrop_blur");
        if (brc != 0)
            evo_boot_log("agc pipe ui_backdrop_blur unavailable (rc=%d); "
                         "backdrop-filter is off", brc);
    }

    /* Quad index buffer for fullscreen video drawing */
    size_t qat = (shader_storage_used + 255u) & ~255u;
    g_agc_dev.quad_indices = (uint16_t *)(shader_storage + qat);
    const uint16_t initial_quad_indices[6] = { 0, 1, 2, 2, 1, 3 };
    memcpy(g_agc_dev.quad_indices, initial_quad_indices, sizeof(initial_quad_indices));
    shader_storage_used = qat + sizeof(initial_quad_indices);

    /*
     * Flush everything the GPU will DMA-read for the rest of the process's
     * life. This pool is write-back cached (memory type 12), and all of it was
     * just written by the CPU:
     *
     *   shader_storage - the shader HEADERS, the MACHINE CODE the GPU fetches
     *       instructions from, the sceAgcLinkShaders output, and the quad index
     *       buffer.
     *   gpu_regs       - the MRT0 colour targets plus every pipeline's cx/sh/uc
     *       register array. The *RegistersIndirect packets make the command
     *       processor DMA-read these at submit time, and the sh array carries
     *       SPI_SHADER_PGM_LO/HI - the shader's entry address.
     *
     * Written once, never flushed, so the GPU read stale lines for both: it was
     * being pointed at a garbage program address and fetching garbage code.
     * That is why a DCB with no draws retired its fence in 0ms while the first
     * DCB containing a draw never retired at all and wedged the GPU. The
     * per-frame flushes (DCB, transient ring) and the per-resource ones in the
     * render interface (vertices, indices, textures) were already right; these
     * two init-time regions were simply missed.
     */
    evo_agc_runtime_cache_flush(shader_storage, shader_storage_used);
    evo_agc_runtime_cache_flush(g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));
    evo_boot_log("agc init flush shader_storage=%p bytes=%zu gpu_regs=%p bytes=%zu",
                 (void *)shader_storage, shader_storage_used,
                 (void *)g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));

    /* 6. Initialize VideoOut */
    for (int attempt = 1; attempt <= 3; ++attempt) {
        g_agc_dev.video_handle = sceVideoOutOpen(0xff, 0, 0, NULL);
        if (g_agc_dev.video_handle >= 0)
            break;
        sceKernelUsleep(500000);
    }
    if (g_agc_dev.video_handle < 0) {
        printf(EVO_AGC_LOG_PREFIX "sceVideoOutOpen failed\n");
        goto cleanup_fail;
    }

    sceVideoOutSetFlipRate(g_agc_dev.video_handle, 0);

    int32_t sup120 = sceVideoOutIsOutputSupported(g_agc_dev.video_handle, 0x000000000000000FUL, NULL, NULL, 0);
    g_agc_dev.supports_120hz = (sup120 >= 0) ? 1 : 0;
    evo_boot_log("agc display 120hz support probe rc=%d (%s)",
                 sup120, g_agc_dev.supports_120hz ? "YES" : "NO");

    /*
     * What the panel is actually running at. The render size below is still
     * whatever evo_agc_runtime_init() was handed - the UI is authored at a
     * fixed 1920x1080 canvas (ui/include/evo_metrics.h, every .rcss) - so this
     * is a readback, not a mode request. It is the input to deciding whether
     * rendering at the panel's own resolution is worth doing.
     */
    {
        evo_vo_resolution_status vres;
        memset(&vres, 0, sizeof(vres));
        int32_t vrc = sceVideoOutGetResolutionStatus(g_agc_dev.video_handle, &vres);
        g_agc_dev.current_refresh_rate = (int)vres.refresh_rate;
        if (vres.refresh_rate == 13) {
            g_agc_dev.is_120hz = 1;
            g_agc_dev.supports_120hz = 1;
        }
        evo_boot_log("agc display probe rc=%d full=%ux%u pane=%ux%u refresh_id=%llu (120hz=%d) "
                     "inches=%d render=%dx%d",
                     vrc, vres.full_width, vres.full_height,
                     vres.pane_width, vres.pane_height,
                     (unsigned long long)vres.refresh_rate,
                     g_agc_dev.is_120hz,
                     (int)vres.screen_inches,
                     g_agc_dev.width, g_agc_dev.height);


        /*
         * Drive the panel at its own resolution. The UI is resolution
         * independent (dp against the EVO_UI_DESIGN canvas), so this is a
         * render-size change, not a layout change. Clamped at
         * EVO_AGC_MAX_RENDER: the composite/depth/stencil budgets above are
         * cut for that size, and a 4K panel would need four times the 1080p
         * footprint of each.
         */
        int panel_w = (vrc == 0) ? (int)vres.full_width  : 0;
        int panel_h = (vrc == 0) ? (int)vres.full_height : 0;
        if (panel_w > 0 && panel_h > 0 &&
            (panel_w != g_agc_dev.width || panel_h != g_agc_dev.height)) {
            if (panel_w > EVO_AGC_MAX_RENDER_W || panel_h > EVO_AGC_MAX_RENDER_H) {
                evo_boot_log("agc display: panel %dx%d over the %dx%d render cap, "
                             "staying at %dx%d", panel_w, panel_h,
                             EVO_AGC_MAX_RENDER_W, EVO_AGC_MAX_RENDER_H,
                             g_agc_dev.width, g_agc_dev.height);
            } else if (evo_agc_apply_render_size(agc_defaults, panel_w, panel_h) == 0) {
                evo_boot_log("agc display: render size -> %dx%d (composite pitch %u)",
                             g_agc_dev.width, g_agc_dev.height,
                             (unsigned)g_agc_dev.composite_pitch);
            } else {
                evo_boot_log("agc display: %dx%d target setup failed, reverting to %dx%d",
                             panel_w, panel_h, width, height);
                (void)evo_agc_apply_render_size(agc_defaults, width, height);
            }
        }

        evo_vo_output_status vout;
        memset(&vout, 0, sizeof(vout));
        int32_t vorc = sceVideoOutGetOutputStatus(g_agc_dev.video_handle, &vout);
        if (vorc == 0) {
            g_agc_dev.display_dynamic_range = (int)vout.dynamic_range;
            g_agc_dev.display_resolution_token = vout.resolution;
            g_agc_dev.display_is_hdr = (vout.dynamic_range == 2 || (vout.flags & 1)) ? 1 : 0;
            evo_boot_log("agc display output probe rc=%d res_token=%u dynamic_range=%u (%s) refresh=%llu flags=%#llx",
                         vorc, vout.resolution, vout.dynamic_range,
                         vout.dynamic_range == 2 ? "HDR" : (vout.dynamic_range == 1 ? "SDR" : "Unknown"),
                         (unsigned long long)vout.refresh_rate,
                         (unsigned long long)vout.flags);
        } else {
            evo_boot_log("agc display output probe failed rc=%d", vorc);
        }
        evo_boot_log_flush();
    }

    evo_video_buffer_t video_buffers[2] = {
        {g_agc_dev.scanout_buffers[0], NULL, NULL, NULL},
        {g_agc_dev.scanout_buffers[1], NULL, NULL, NULL},
    };
    evo_video_attribute_t attr;
    memset(&attr, 0, sizeof(attr));

    uint64_t vfmt = g_agc_dev.is_hdr ? EVO_AGC_VIDEO_FORMAT_HDR : EVO_AGC_VIDEO_FORMAT_SDR;
    sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                   (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height,
                                   0, 0, 0);

    ret = sceVideoOutRegisterBuffers2(g_agc_dev.video_handle, 0, 0,
                                      video_buffers, 2, &attr, 0, NULL);
    /* evo_boot_log, not printf: this file's printf output never reaches
     * evo.log or klog, so every one of these diagnostics was invisible. */
    /* Both scanout bases must be 2MB aligned or the tiled display surface is
     * shuffled; mis0/mis1 are the offsets past that boundary and must read 0. */
    evo_boot_log("agc scanout align mis0=%#llx mis1=%#llx tiled_need=%#x stride=%#llx",
                 (unsigned long long)((uintptr_t)g_agc_dev.scanout_buffers[0] &
                                      (EVO_AGC_DIRECT_MEM_ALIGN - 1u)),
                 (unsigned long long)((uintptr_t)g_agc_dev.scanout_buffers[1] &
                                      (EVO_AGC_DIRECT_MEM_ALIGN - 1u)),
                 (unsigned)((((uint32_t)g_agc_dev.width + 127u) / 128u) *
                            (((uint32_t)g_agc_dev.height + 127u) / 128u) * 0x10000u),
                 (unsigned long long)EVO_AGC_SCANOUT_STRIDE);
    evo_boot_log("agc vo handle=%d register_rc=%d (0x%08x) fmt=%#llx %dx%d "
                 "buf0=%p buf1=%p memtype=%d",
                 g_agc_dev.video_handle, ret, (unsigned)ret,
                 (unsigned long long)vfmt, g_agc_dev.width, g_agc_dev.height,
                 (void *)g_agc_dev.scanout_buffers[0],
                 (void *)g_agc_dev.scanout_buffers[1],
                 EVO_AGC_DIRECT_MEM_TYPE);
    if (ret != 0 && (g_agc_dev.width != width || g_agc_dev.height != height)) {
        /*
         * VideoOut would not take the panel's own size. Nothing has been
         * registered, so going back to the size the caller asked for is just
         * a second attempt, not a re-registration.
         */
        evo_boot_log("agc vo register rc=%d at %dx%d -> falling back to %dx%d",
                     ret, g_agc_dev.width, g_agc_dev.height, width, height);
        if (evo_agc_apply_render_size(agc_defaults, width, height) == 0) {
            sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                           (uint32_t)g_agc_dev.width,
                                           (uint32_t)g_agc_dev.height,
                                           0, 0, 0);
            ret = sceVideoOutRegisterBuffers2(g_agc_dev.video_handle, 0, 0,
                                              video_buffers, 2, &attr, 0, NULL);
        }
    }

    if (ret != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceVideoOutRegisterBuffers2 failed: %d\n", ret);
        goto cleanup_fail;
    }

    g_agc_dev.active_backbuffer = 0;
    g_agc_dev.current_slot = 0;
    g_agc_dev.video_pts[0] = INT64_MIN;
    g_agc_dev.video_pts[1] = INT64_MIN;
    memset(g_agc_dev.stage_cache, 0, sizeof(g_agc_dev.stage_cache));
    g_agc_dev.frame_counter = 0;
    g_agc_dev.flip_arg = 1;
    g_agc_dev.bound_pipeline = -1;
    g_agc_dev.initialized = 1;

    printf(EVO_AGC_LOG_PREFIX "AGC runtime successfully initialized! 100%% GPU ready.\n");
    return 0;

cleanup_fail:
    evo_agc_runtime_shutdown();
    return -1;
}

/*
 * Wait for every submitted command buffer to retire before anything it points
 * at is handed back.
 *
 * Teardown released the scanout registration and then the direct-memory pool
 * without ever asking whether the GPU was finished with them. Everything the
 * GPU reads while drawing a frame - command buffers, vertex and index data,
 * textures, the transient ring, the scanout target itself - lives in that one
 * mapping, so unregistering and releasing it while a submit is still in flight
 * leaves the GPU writing into physical pages the kernel has already reclaimed.
 * That is a kernel panic, and it is a race, which is why it does not fire every
 * time.
 *
 * The per-slot fence protocol needed to avoid it already exists for frame
 * pacing (see frame_begin): the GPU writes fence_marker into fences[slot] when
 * that slot's submit retires, fence_expect[slot] holds the value to wait for,
 * and 0 means nothing is outstanding. This waits on all of them rather than
 * just the slot about to be reused. The fence line must be invalidated before
 * every read - it sits in the same write-back pool the CPU wrote, so without
 * evicting it the CPU spins on its own stale copy forever.
 *
 * Bounded, and deliberately so: if the GPU is already wedged then no amount of
 * waiting will retire the fence, and hanging here would turn a panic on exit
 * into a hang on exit. Past the deadline we give up and release anyway, which
 * is no worse than the behaviour this replaces.
 */
static void agc_wait_gpu_idle(unsigned timeout_ms)
{
    if (!g_agc_dev.initialized)
        return;

    int waited_any = 0;
    for (int slot = 0; slot < EVO_AGC_FRAME_SLOTS; ++slot) {
        if (!g_agc_dev.fence_expect[slot] || !g_agc_dev.fences[slot])
            continue;
        waited_any = 1;

        unsigned waited = 0;
        for (; waited < timeout_ms; ++waited) {
            evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);
            if (*g_agc_dev.fences[slot] == g_agc_dev.fence_expect[slot])
                break;
            sceKernelUsleep(1000);
        }

        evo_boot_log("agc shutdown: slot=%d %s after %ums (fence=%u expect=%u)",
                     slot, waited >= timeout_ms ? "STILL BUSY - releasing anyway"
                                                : "idle",
                     waited,
                     (unsigned)*g_agc_dev.fences[slot],
                     (unsigned)g_agc_dev.fence_expect[slot]);
        g_agc_dev.fence_expect[slot] = 0;
    }
    if (!waited_any)
        evo_boot_log("agc drain: no submits outstanding");
    evo_boot_log_flush();
}

void evo_agc_runtime_wait_idle(unsigned timeout_ms)
{
    agc_wait_gpu_idle(timeout_ms);
}

void evo_agc_runtime_shutdown(void)
{
    /* Order matters: drain first, then drop the scanout registration, then
     * unmap, then hand the physical pages back. */
    agc_wait_gpu_idle(500);

    evo_boot_log("agc shutdown: drained, video_handle=%d base=%p",
                 g_agc_dev.video_handle, (void *)g_agc_dev.direct_mem_base);
    evo_boot_log_flush();

    agc_upscale_release();

    if (g_agc_dev.video_handle >= 0) {
        if (g_agc_dev.is_120hz) {
            evo_boot_log("agc shutdown: restoring default 60hz output mode");
            sceVideoOutConfigureOutput(g_agc_dev.video_handle, 0x0000000000000001UL, NULL, NULL, 0);
            g_agc_dev.is_120hz = 0;
        }
        sceVideoOutUnregisterBuffers(g_agc_dev.video_handle, 0);
        sceVideoOutClose(g_agc_dev.video_handle);
        g_agc_dev.video_handle = -1;
    }

    evo_boot_log("agc shutdown: videoout closed");
    evo_boot_log_flush();

    if (g_agc_dev.direct_mem_base) {
        sceKernelMunmap(g_agc_dev.direct_mem_base, g_agc_dev.direct_mem_bytes);
        g_agc_dev.direct_mem_base = NULL;
    }
    evo_boot_log("agc shutdown: unmapped");
    evo_boot_log_flush();

    if (g_agc_dev.direct_mem_offset >= 0) {
        sceKernelReleaseDirectMemory(g_agc_dev.direct_mem_offset, g_agc_dev.direct_mem_bytes);
        g_agc_dev.direct_mem_offset = -1;
    }

    g_agc_dev.initialized = 0;
    evo_boot_log("agc shutdown: released, done");
    evo_boot_log_flush();
}

int evo_agc_runtime_is_active(void)
{
    return g_agc_dev.initialized;
}

void evo_agc_runtime_frame_begin(void)
{
    if (!g_agc_dev.initialized || g_agc_dev.frame_active)
        return;

    const uint32_t slot = g_agc_dev.current_slot;

    /* Diagnostic trace for the first dozen frames only - enough to cross every
     * slot's fence-wait more than once without spamming evo.log forever. Drop
     * once the black-screen-then-hang symptom is understood; not gated behind
     * a flag file since the very first frames are exactly what's in question
     * and there is no window to drop one in before they run. */
    static int s_trace_frames = 12;
    int tracing = s_trace_frames > 0;
    if (tracing) {
        s_trace_frames--;
        evo_boot_log("agc frame_begin slot=%u fence=%u expect=%u", slot,
                     (unsigned)*g_agc_dev.fences[slot],
                     (unsigned)g_agc_dev.fence_expect[slot]);
    }

    /* Wait for this slot's last submit to retire. The fence line must be
     * clflush'd before every read: it lives in the same write-back pool the
     * CPU wrote, so without evicting it the CPU re-reads its own stale copy
     * forever. ps5-opengl's poll loop does exactly this. Bounded so a wedged
     * GPU degrades to a dropped frame instead of hanging the render thread. */
    if (g_agc_dev.fence_expect[slot]) {
        unsigned waits = 0;
        for (; waits < 2000; ++waits) {
            evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);
            if (*g_agc_dev.fences[slot] == g_agc_dev.fence_expect[slot])
                break;
            sceKernelUsleep(1000);
        }
        if (waits >= 2000) {
            static int s_timeout_log = 4;
            if (s_timeout_log > 0) {
                s_timeout_log--;
                evo_boot_log("agc frame_begin slot=%u FENCE TIMEOUT fence=%u expect=%u",
                             slot, (unsigned)*g_agc_dev.fences[slot],
                             (unsigned)g_agc_dev.fence_expect[slot]);
            }
        } else if (tracing) {
            evo_boot_log("agc frame_begin slot=%u retired after %u waits", slot, waits);
        }
    }

    /*
     * Menu backdrop - dark neutral (0x0d,0x0d,0x10,0xff). In player mode the
     * video quad is drawn straight into the tiled backbuffer, so the clear is
     * normally skipped: it is an 8.3 MB CPU write plus ~130,000 clflushes,
     * 4-6 ms per frame at 1080p and far worse at 4K.
     *
     * But the quad only covers the *image*. Anything the UI composited on top
     * - OSD, scrub bar, subtitles - and everything outside a letterboxed image
     * survives into the next use of this buffer. Skipping the clear there left
     * each subtitle line stacked on the last and the OSD frozen on screen once
     * it faded out, which is only obvious when the quad is not being redrawn
     * every frame (software decode). So the clear is skipped only for a buffer
     * that carried nothing but video.
     */
    if (!g_agc_dev.is_player_mode ||
        g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer]) {
        g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer] = 0;
        g_agc_dev.video_pts[g_agc_dev.active_backbuffer] = INT64_MIN;
        uint32_t *backbuffer = (uint32_t *)g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
        if (backbuffer) {
            /* Streamed, not stored-then-flushed: this is the single biggest
             * CPU cost in the menus - 8.3 MB a frame at 1080p, 33 MB at 4K.
             * See evo_agc_runtime_stream_fill. */
            /* 0xff100d0d is dark grey only as 8-bit BGRA. On a 10:10:10:2
             * HDR scanout those bits would light one channel near full, so
             * HDR clears to black with opaque alpha. */
#ifdef NUVIO_APP
            /* Nuvio: letterbox bars are true black, not the menu grey. */
            const uint32_t sdr_clear = g_agc_dev.is_player_mode ? 0xff000000u : 0xff100d0du;
#else
            const uint32_t sdr_clear = 0xff100d0du;
#endif
            evo_agc_runtime_stream_fill(backbuffer,
                                        g_agc_dev.is_hdr ? 0xc0000000u : sdr_clear,
                                        scanout_tiled_bytes());
        }
    }

    /* Open transient ring slot */
    /*
     * Reopen this slot's transient-ring region with the token it was sealed
     * under. This used to pass (0, 1) - a proven completion with a zero token -
     * which transient_ring_begin rejects as TOKEN_MISMATCH for any SEALED slot.
     * The return value was ignored, so each slot silently stayed sealed after
     * its first present; once all three had been used, every allocation
     * returned SLOT_BUSY, RenderGeometry bailed before drawing, and the UI
     * froze on the last frame that made it out. The fence wait above is what
     * proves the GPU is done, so the token is safe to hand back here.
     */
    int ring_rc = evo_agc_transient_ring_begin(&g_agc_dev.transient_ring, slot,
                                               g_agc_dev.ring_token[slot], 1);
    if (ring_rc != EVO_AGC_TRANSIENT_OK) {
        static int s_ring_log = 6;
        if (s_ring_log > 0) {
            s_ring_log--;
            evo_boot_log("agc frame_begin slot=%u transient_ring_begin FAILED rc=%d token=%llu",
                         slot, ring_rc,
                         (unsigned long long)g_agc_dev.ring_token[slot]);
        }
    }

    /* Initialize DCB writer for this slot */
    evo_agc_writer_init(&g_agc_dev.current_cb,
                        g_agc_dev.dcb_slots[slot],
                        g_agc_dev.dcb_slot_capacity_dwords);

    /* Wait rendering packet for VideoOut scanout safety */
    uint32_t wait_size = sceAgcDriverGetWaitRenderingPacketSizeInDwords();
    if (wait_size > 0) {
        sceAgcDriverWaitUntilSafeForRendering(&g_agc_dev.current_cb.up, wait_size, 0,
                                              (uint32_t)g_agc_dev.video_handle,
                                              g_agc_dev.active_backbuffer);
    }

    /* Set MRT0 Color Target to the current scanout backbuffer */
    evo_agc_writer_set_target(&g_agc_dev.current_cb,
                              g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer], 16);
    g_agc_dev.current_layer_target = NULL;

    /* Bind the stencil target, then zero the stencil planes for this frame.
     * Clearing once here is what lets each RmlUi clip mask claim its own value
     * instead of clearing a multi-MB surface per mask. The DMA fill is CP-synced
     * so it completes before any draw in this DCB reads the buffer. */
    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb,
                                   g_agc_dev.gpu_regs->depth_target, 16);
    if (g_agc_dev.stencil_base) {
        sceAgcDcbDmaData(&g_agc_dev.current_cb, 0u, 3u, 0u,
                         (uint64_t)(uintptr_t)g_agc_dev.stencil_base,
                         2u, 0u, 0u,
                         /* 4 MB covers the 64KB_Z_X tiled footprint of an S8
                          * 1920x1080 surface (~2.6 MB) with margin; a
                          * width*height linear fill leaves the padding dirty. */
                         (uint32_t)(4u * 1024u * 1024u),
                         0u, 0u, 1u /* cp_sync */);
    }
    g_agc_dev.stencil_ref = 0u;
    g_agc_dev.stencil_counter = 0u;
    g_agc_dev.stencil_func_equal = 1;
    g_agc_dev.clip_mask_enabled = 0;

    /* Set default viewport & scissor */
    evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12), 0.0f, 0.0f,
                                (float)g_agc_dev.width, (float)g_agc_dev.height);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    /* Default blend: premultiplied alpha */
    evo_agc_writer_set_blend(&g_agc_dev.current_cb, alloc_transient_cx(2), EVO_AGC_BLEND_PREMULTIPLIED);

    g_agc_dev.bound_pipeline = -1;
    g_agc_dev.frame_has_draws = 0;
    g_agc_dev.frame_active = 1;
}

/* HDR10 output: drawing to the scanout swaps the SDR pipelines for their PQ
 * twins. Offscreen targets (RmlUi layers, upscaler scratch) stay SDR - a
 * layer composited onto the scanout goes through EVO_AGC_PIPE_UI, which is
 * swapped then. */
static int agc_hdr_remap(int pipeline_id)
{
    if (!g_agc_dev.is_hdr || g_agc_dev.current_layer_target != NULL)
        return pipeline_id;
    int to = pipeline_id;
    switch (pipeline_id) {
    case EVO_AGC_PIPE_UI:         to = EVO_AGC_PIPE_UI_PQ; break;
    case EVO_AGC_PIPE_VIDEO_HDR:  to = EVO_AGC_PIPE_VIDEO_HDR_PQ; break;
    case EVO_AGC_PIPE_VIDEO_HLG:  to = EVO_AGC_PIPE_VIDEO_HLG_PQ; break;
    case EVO_AGC_PIPE_NV12_HDR:   to = EVO_AGC_PIPE_NV12_HDR_PQ; break;
    case EVO_AGC_PIPE_NV12_HLG:   to = EVO_AGC_PIPE_NV12_HLG_PQ; break;
    default: break;
    }
    return g_agc_dev.pipelines[to].valid ? to : pipeline_id;
}

static int s_bound_logical = -1;   /* what the caller asked for, pre-remap */

void evo_agc_runtime_bind_pipeline(int pipeline_id)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT)
        return;
    s_bound_logical = pipeline_id;
    pipeline_id = agc_hdr_remap(pipeline_id);
    if (g_agc_dev.bound_pipeline == pipeline_id)
        return;

    evo_agc_pipeline_t *pipe = &g_agc_dev.pipelines[pipeline_id];
    if (!pipe->valid)
        return;

    evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, pipe->cx_regs, pipe->cx_reg_count);
    evo_agc_writer_set_sh_indirect(&g_agc_dev.current_cb, pipe->sh_regs, pipe->sh_reg_count);
    evo_agc_writer_set_uc_indirect(&g_agc_dev.current_cb, pipe->uc_regs, pipe->uc_reg_count);

    /*
     * Rasteriser state this path never programmed, emitted AFTER the pipeline
     * registers so it wins over anything sceAgcLinkShaders put in linked_cx.
     *
     * Every one of these was left to whatever sceAgcGetRegisterDefaults()
     * happened to contain. ps5-opengl programs all of them explicitly from its
     * own pipeline state (runtime_rasterizer_control at 0x205,
     * runtime_color_control at 0x202, runtime_depth_control at 0x200) and only
     * relies on defaults for registers it never uses. With blending forced off
     * the scanout still came back completely untouched (changed=0), so no
     * fragment is reaching the colour block - and a default that culls both
     * faces, disables the viewport transform, or leaves the colour block
     * disabled produces exactly that: draws that execute, retire and export
     * nothing.
     *
     *   0x205 PA_SU_SC_MODE_CNTL - 0: cull nothing, solid fill, CCW front.
     *         RmlUi emits both windings, so any culling silently drops half or
     *         all of the UI.
     *   0x206 PA_CL_VTE_CNTL - 0x43f: enable the viewport X/Y/Z scale+offset
     *         transform and VTX_W0_FMT. Without it clip space is never mapped
     *         through the viewport we set, so the geometry lands nowhere.
     *   0x200 DB_DEPTH_CONTROL - 0: depth test/write off. There is no depth
     *         buffer bound at all here, and a default with Z_ENABLE set kills
     *         every fragment against a target that does not exist.
     *   0x202 CB_COLOR_CONTROL - MODE=CB_NORMAL(1)<<4, ROP3=0xcc (copy).
     *         MODE=CB_DISABLE is a completely silent "write nothing".
     */
    SceAgcRegister *ff = alloc_transient_cx(4);
    if (ff) {
        ff[0] = (SceAgcRegister){0x205, 0u};
        ff[1] = (SceAgcRegister){0x206, 0x43fu};
        ff[2] = (SceAgcRegister){0x200, 0u};
        ff[3] = (SceAgcRegister){0x202, 0x00cc0010u};
        evo_agc_writer_set_cx_indirect(&g_agc_dev.current_cb, ff, 4);
    }

    g_agc_dev.bound_pipeline = pipeline_id;
}

void evo_agc_runtime_set_scissor(int x, int y, int w, int h)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    SceAgcRegister *sc = alloc_transient_cx(2);
    if (!sc)
        return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) {
        evo_agc_writer_set_scissor(&g_agc_dev.current_cb, sc, 0, 0, 0, 0);
        g_agc_dev.scissor_x = g_agc_dev.scissor_y = 0;
        g_agc_dev.scissor_w = g_agc_dev.scissor_h = 0;
        return;
    }
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, sc,
                               (uint32_t)x, (uint32_t)y,
                               (uint32_t)(x + w), (uint32_t)(y + h));
    g_agc_dev.scissor_x = x; g_agc_dev.scissor_y = y;
    g_agc_dev.scissor_w = w; g_agc_dev.scissor_h = h;
}

void evo_agc_runtime_set_blend(int blend_mode)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    SceAgcRegister *bl = alloc_transient_cx(2);
    if (!bl)
        return;
    evo_agc_writer_set_blend(&g_agc_dev.current_cb, bl, blend_mode);
}

int evo_agc_has_layers(void)
{
    return g_agc_dev.initialized;
}

/* Clear a freshly-acquired layer surface. RmlUi only requires a pushed layer to
 * be transparent black within the ACTIVE SCISSOR REGION - which is what
 * evo_agc_runtime.h has always documented ("memset the scissor region + clflush,
 * not the full surface"). Wiping the whole canvas instead cost ~33 MB of stores
 * plus a 33 MB cache flush per acquire, and a blurred element takes three
 * acquires per frame; for a card-sized region this is roughly 70x less work.
 * RmlUi sets the backdrop scissor before PushLayer, so the rect is current. */
static void layer_surface_clear(evo_agc_layer_surface_t *layer)
{
    if (!layer || !layer->cpu_base || !layer->pitch_bytes)
        return;

    int x = g_agc_dev.scissor_x, y = g_agc_dev.scissor_y;
    int w = g_agc_dev.scissor_w, h = g_agc_dev.scissor_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w > (int)layer->width  - x) w = (int)layer->width  - x;
    if (h > (int)layer->height - y) h = (int)layer->height - y;
    if (w <= 0 || h <= 0)
        return;

    const size_t pitch = (size_t)layer->pitch_bytes;
    const size_t span  = (size_t)w * 4u;
    uint8_t *row = layer->cpu_base + (size_t)y * pitch + (size_t)x * 4u;
    for (int i = 0; i < h; ++i, row += pitch) {
        /* Only the span just written, never the whole row: flushing whole rows
         * walked pitch/span times the cache lines - 2.8x for a sidebar-width
         * element, and this runs three times per blurred element per frame.
         * stream_fill drops the flush entirely when the span is aligned. */
        evo_agc_runtime_stream_fill(row, 0u, span);
    }
}

int evo_agc_layer_acquire(evo_agc_layer_surface_t **out)
{
    if (!g_agc_dev.initialized || !out)
        return -1;
    for (int i = 0; i < EVO_AGC_MAX_LAYERS; ++i) {
        evo_agc_layer_surface_t *layer = &g_agc_dev.layers[i];
        if (!layer->in_use) {
            layer->in_use = 1;
            /* No logging here: this runs three times per blurred element
             * per frame, and evo_log_flush() fsyncs to USB (~3-4 ms a
             * line), which cost ~42 ms/frame and pinned the UI at ~20fps. */
            layer_surface_clear(layer);
            *out = layer;
            return 0;
        }
    }
    return -1;
}

void evo_agc_layer_release(evo_agc_layer_surface_t *layer)
{
    if (!layer)
        return;
    layer->in_use = 0;
}

int evo_agc_set_layer_target(const evo_agc_layer_surface_t *layer)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return -1;

    if (layer != g_agc_dev.current_layer_target) {
        if (layer) {
            /* The layer MRT register block lives inside GPU-mapped gpu_regs
             * (carved at init, rebuilt+flushed by apply_render_size), so SetTarget
             * can DMA-read it at submit time. */
            evo_agc_writer_set_target(&g_agc_dev.current_cb, layer->mrt, 16);
        } else {
            evo_agc_writer_set_target(&g_agc_dev.current_cb,
                                      g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer],
                                      16);
        }
        g_agc_dev.current_layer_target = layer;
        /* HDR10: the same logical pipeline maps to a different variant on
         * the scanout than in a layer, so re-resolve it for the new target. */
        if (g_agc_dev.is_hdr && s_bound_logical >= 0 &&
            agc_hdr_remap(s_bound_logical) != g_agc_dev.bound_pipeline) {
            g_agc_dev.bound_pipeline = -1;
            evo_agc_runtime_bind_pipeline(s_bound_logical);
        }
        /* A target switch returns the CP to the frame's default full-canvas
         * viewport: the caller (CompositeLayers) diverges via scissor alone.
         * Re-emit the viewport here so a stale 12-register viewport block from
         * an earlier frame cannot survive into the new target. */
        SceAgcRegister *vp = alloc_transient_cx(12);
        if (vp)
            evo_agc_writer_set_viewport(&g_agc_dev.current_cb, vp, 0.0f, 0.0f,
                                        (float)g_agc_dev.width, (float)g_agc_dev.height);
    }
    return 0;
}

void evo_agc_get_scanout_layer(evo_agc_layer_surface_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->width       = (uint32_t)g_agc_dev.width;
    out->height      = (uint32_t)g_agc_dev.height;
    out->pitch_bytes = ((uint32_t)g_agc_dev.width * 4u + 255u) & ~255u;
    out->gpu_addr    = (uint64_t)(uintptr_t)g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
    out->cpu_base    = g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer];
    out->pool_index  = -1;
}

void evo_agc_flush_color_target(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;
    evo_agc_writer_flush_color_target(&g_agc_dev.current_cb);
}

void evo_agc_runtime_frame_end(void)
{
    if (!g_agc_dev.initialized || !g_agc_dev.frame_active)
        return;

    const uint32_t slot = g_agc_dev.current_slot;

    /*
     * A frame with no draws must not be presented.
     *
     * frame_begin() runs whenever the render loop wants a frame (gl_active),
     * but RmlUi only actually redraws a couple of times a second - the loop
     * itself runs at ~370/s. Presenting the frames in between flips a buffer
     * carrying nothing but the backdrop clear, so the panel alternates between
     * the UI and a blank screen: on hardware that reads as the UI flashing once
     * and then going black. The per-frame DCB sizes show it exactly - 2830
     * dwords when RmlUi drew, 68 when it did not.
     *
     * So close the frame and leave the displayed buffer alone: no submit, no
     * flip, and no back-buffer toggle. The scanout keeps showing the last frame
     * that actually had content, which is what a retained-mode UI wants.
     */
    if (!g_agc_dev.frame_has_draws) {
        /* ABORT, not seal. Sealing hands the slot a retire token that only a
         * submitted frame can ever clear: the next frame_begin calls
         * transient_ring_begin(slot, 0, 1), which sees SEALED with a zero token
         * and returns TOKEN_MISMATCH, so the slot is never reopened, every
         * subsequent alloc returns SLOT_BUSY, RenderGeometry bails before
         * drawing - and the frame is then empty for that reason, which seals it
         * again. One sealed slot and the UI never draws again. abort() is the
         * call meant for a frame that is discarded rather than submitted: it
         * drops the slot straight back to EMPTY. */
        evo_agc_transient_ring_abort(&g_agc_dev.transient_ring, slot);
        g_agc_dev.ring_token[slot] = 0u;   /* nothing outstanding on this slot */
        g_agc_dev.frame_active = 0;
        return;
    }

    /* Mirrors frame_begin's trace - the two are called once each per frame in
     * lockstep, so an independent counter here stays in sync with it. */
    static int s_trace_frames = 12;
    int tracing = s_trace_frames > 0;
    if (tracing) {
        s_trace_frames--;
        evo_boot_log("agc frame_end slot=%u dwords_before_flip=%u", slot,
                     evo_agc_writer_dwords_written(&g_agc_dev.current_cb));
    }

    /* Prepare this frame's fence marker up front; every mode below uses it. */
    if (++g_agc_dev.fence_marker == 0u)
        g_agc_dev.fence_marker = 1u;
    const uint32_t marker = g_agc_dev.fence_marker;
    *g_agc_dev.fences[slot] = 0;
    g_agc_dev.fence_expect[slot] = marker;
    evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);

    const uint64_t fence_addr = (uint64_t)(uintptr_t)g_agc_dev.fences[slot];
    g_agc_dev.flip_arg++;

    /* Flush the colour-block caches out to memory, then stamp the frame's
     * end-of-pipe fence (which also writes GPU L2 back). The flip itself is
     * issued from the CPU after the fence retires - see the note above. */
    evo_agc_writer_flush_color_target(&g_agc_dev.current_cb);
    evo_agc_writer_release_mem(&g_agc_dev.current_cb, fence_addr, marker);

    /* 3. Seal transient ring */
    /* Remember the token so the next frame_begin on this slot can reopen it. */
    g_agc_dev.ring_token[slot] = (uint64_t)g_agc_dev.flip_arg;
    evo_agc_transient_ring_seal(&g_agc_dev.transient_ring, slot,
                                g_agc_dev.ring_token[slot]);

    /* 4. Submit DCB to GPU Driver.
     *
     * Mandatory before every submit (see evo_agc_runtime_cache_flush's doc
     * comment, and the historical hardware-verified #27 present path this
     * bare-metal rewrite otherwise mirrors): everything the GPU CP is about to
     * DMA-read this frame - the command words themselves, plus every register/
     * constant/descriptor slice this frame allocated from the transient ring
     * (color target + viewport/scissor/blend registers, pipeline registers on
     * a bind, video constants, texture descriptors) - was just written by the
     * CPU into write-back memory and needs evicting before the GPU can see it.
     */
    uint32_t dwords = evo_agc_writer_dwords_written(&g_agc_dev.current_cb);
    if (dwords > g_agc_dev.dcb_peak_dwords)
        g_agc_dev.dcb_peak_dwords = dwords;
    if (!g_agc_dev.dcb_min_presented || dwords < g_agc_dev.dcb_min_presented)
        g_agc_dev.dcb_min_presented = dwords;
    g_agc_dev.presents++;
    /* A presented frame far smaller than the busiest one is a partial render
     * going to the panel over a freshly cleared buffer. Log the first few. */
    if (g_agc_dev.dcb_peak_dwords > 1000u &&
        dwords < g_agc_dev.dcb_peak_dwords / 4u) {
        static int s_partial_log = 10;
        if (s_partial_log > 0) {
            s_partial_log--;
            evo_boot_log("agc PARTIAL present frame=%llu dwords=%u peak=%u",
                         (unsigned long long)g_agc_dev.frame_counter,
                         dwords, g_agc_dev.dcb_peak_dwords);
        }
    }
    /*
     * Periodic telemetry, emitted from inside the render loop - so one line
     * rather than three, and every 600 frames rather than 120. It was three
     * USB writes every two seconds, each a visible hitch in the very frame
     * times it is meant to be measuring; it is now one write every ten.
     * Still written through: a line that does not survive a crash is no use
     * for the crashes this is here to explain.
     */
    if ((g_agc_dev.frame_counter % 600u) == 0u) {
        evo_direct_mem_stats_t dm;
        evo_direct_mem_get_stats(&dm);
        evo_boot_log(
            "agc health frame=%llu dcb=%u/%u peak=%u dcb_full=%u ring_fail=%u tex_fail=%u "
            "direct_mem=%zu/%zu peak=%zu allocs=%zu | "
            "clip_masks=%u clip_enables=%u (feature=%d) | "
            "presents=%u dcb_min_presented=%u flip_waits=%u timeouts=%u",
            (unsigned long long)g_agc_dev.frame_counter,
            dwords, g_agc_dev.dcb_slot_capacity_dwords,
            g_agc_dev.dcb_peak_dwords,
            evo_agc_writer_out_of_space_count(),
            g_agc_dev.ring_alloc_fail, g_agc_dev.tex_alloc_fail,
            dm.allocated_bytes, dm.total_bytes, dm.peak_bytes,
            dm.num_allocations,
            g_agc_dev.clip_mask_calls, g_agc_dev.clip_enable_calls,
            EVO_AGC_CLIP_MASK,
            g_agc_dev.presents, g_agc_dev.dcb_min_presented,
            g_agc_dev.flip_waits, g_agc_dev.flip_timeouts);
        evo_agc_writer_reset_out_of_space_count();
        g_agc_dev.clip_mask_calls = 0;
        g_agc_dev.clip_enable_calls = 0;
        g_agc_dev.dcb_min_presented = 0;
        g_agc_dev.presents = 0;
        g_agc_dev.flip_waits = 0;
    }
    evo_agc_runtime_cache_flush(g_agc_dev.dcb_slots[slot], (size_t)dwords * sizeof(uint32_t));
    {
        size_t ring_used = evo_agc_transient_ring_used(&g_agc_dev.transient_ring, slot);
        if (ring_used)
            evo_agc_runtime_cache_flush(g_agc_dev.transient_ring.base +
                                        g_agc_dev.transient_ring.slots[slot].offset,
                                        ring_used);
    }

    SceAgcSubmit submit = {
        .words = g_agc_dev.dcb_slots[slot],
        .count = dwords,
        .flag = 0,
        .padding = {0, 0, 0},
    };
    if (tracing)
        evo_boot_log("agc frame_end slot=%u dwords=%u submitting", slot, dwords);
    int32_t submit_rc = sceAgcDriverSubmitDcb(&submit);
    if (tracing)
        evo_boot_log("agc frame_end slot=%u submit_rc=%d", slot, submit_rc);
    if (submit_rc != 0) {
        printf(EVO_AGC_LOG_PREFIX "sceAgcDriverSubmitDcb failed: %d\n", submit_rc);
    } else {
        /* Cooperative yield the platform's GPU scheduler requires: go too long
         * between suspend points and the OS force-kills the process as a GPU
         * hang (GPU_FAULT_SUSPENDPOINT_TIMEOUT_IN_RUN_ASYNC) - which is exactly
         * what happened before this call existed. One per submitted DCB,
         * matching the historical #27 present path's SubmitDcb -> SuspendPoint
         * sequence. */
        int32_t sp_rc = sceAgcSuspendPoint();
        if (tracing)
            evo_boot_log("agc frame_end slot=%u suspend_rc=%d", slot, sp_rc);
        if (sp_rc != 0) {
            printf(EVO_AGC_LOG_PREFIX "sceAgcSuspendPoint failed: %d\n", sp_rc);
        }

        /* Mode 3: no SetFlip went into the DCB. Wait for this frame's own fence
         * so we never hand VideoOut a half-drawn buffer, then flip from the CPU
         * - the path already proven to reach the panel. */
        {
            /* #103: submit -> retire of a frame carrying upscale passes is the
             * GPU time the budget check needs (1 ms poll granularity). */
            struct timespec up_t0;
            if (g_agc_dev.up.this_frame)
                clock_gettime(CLOCK_MONOTONIC, &up_t0);
            /* 100 us steps on an upscaled frame so its GPU time is measured
             * rather than rounded up to the first 1 ms poll; same 500 ms cap. */
            const unsigned step_us = g_agc_dev.up.this_frame ? 100u : 1000u;
            unsigned waits = 0;
            for (; waits < 500000u / step_us; ++waits) {
                evo_agc_runtime_cache_flush((const void *)g_agc_dev.fences[slot], 4);
                if (*g_agc_dev.fences[slot] == marker)
                    break;
                sceKernelUsleep(step_us);
            }
            if (g_agc_dev.up.this_frame) {
                struct timespec up_t1;
                clock_gettime(CLOCK_MONOTONIC, &up_t1);
                const int64_t us = (int64_t)(up_t1.tv_sec - up_t0.tv_sec) * 1000000 +
                                   (int64_t)(up_t1.tv_nsec - up_t0.tv_nsec) / 1000;
                agc_upscale_note_gpu_time(us > 0 ? (uint64_t)us : 0u);
                g_agc_dev.up.this_frame = 0;
            }
            int32_t fliprc = sceVideoOutSubmitFlip(g_agc_dev.video_handle,
                                                   g_agc_dev.active_backbuffer,
                                                   1 /* VSYNC */,
                                                   (int64_t)g_agc_dev.flip_arg);
            g_agc_dev.fence_expect[slot] = 0; /* already waited; don't re-wait */

            /*
             * Wait for the flip to actually retire before this frame ends.
             *
             * sceVideoOutSubmitFlip is asynchronous, and there are only two
             * scanout buffers. Without this wait the sequence is: flip A, then
             * clear/draw/flip B, then clear A again - while A may still be the
             * buffer the display is scanning out. The CPU memset then wipes the
             * live framebuffer top to bottom, which on screen is a black band
             * sweeping down the picture, worst while something slow is loading
             * and frames are queueing. The counters showed nothing because no
             * draw was ever dropped: the content was correct, it was being
             * erased after the fact.
             *
             * ps5-opengl's native runtime does exactly this poll
             * (wait_for_flip_marker: get_flip_status until status[3] == marker).
             * status[3] carries the flip_arg of the last completed flip.
             * Bounded so a stalled display degrades to tearing, not a hang.
             */
            if (fliprc == 0) {
                uint64_t status[16];
                unsigned fwaits = 0;
                for (; fwaits < 120u; ++fwaits) {
                    memset(status, 0, sizeof(status));
                    if (sceVideoOutGetFlipStatus(g_agc_dev.video_handle, status) == 0 &&
                        status[3] == (uint64_t)g_agc_dev.flip_arg)
                        break;
                    sceVideoOutWaitVblank(g_agc_dev.video_handle);
                }
                g_agc_dev.flip_waits += fwaits;
                if (fwaits >= 120u)
                    g_agc_dev.flip_timeouts++;
            }

            /* Frame 40: late enough that RmlUi has drawn a real screen, and the
             * fence above guarantees the GPU is done with this buffer. */
            static int s_dumped = 0;
            if (!s_dumped && g_agc_dev.frame_counter >= 40) {
                s_dumped = 1;
                evo_agc_runtime_cache_flush(
                    g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer],
                    (size_t)g_agc_dev.width * (size_t)g_agc_dev.height * 4u);
                agc_dump_scanout((const uint32_t *)
                                     g_agc_dev.scanout_buffers[g_agc_dev.active_backbuffer],
                                 g_agc_dev.width, g_agc_dev.height);
            }
            if (tracing)
                evo_boot_log("agc frame_end slot=%u cpu_flip buf=%d waits=%u "
                             "fence=%u marker=%u rc=%d",
                             slot, g_agc_dev.active_backbuffer, waits,
                             (unsigned)*g_agc_dev.fences[slot],
                             (unsigned)marker, fliprc);
        }
    }

    /* 5. Flip buffers and advance slot */
    g_agc_dev.active_backbuffer = 1 - g_agc_dev.active_backbuffer;
    g_agc_dev.current_slot = (g_agc_dev.current_slot + 1) % EVO_AGC_FRAME_SLOTS;
    g_agc_dev.frame_counter++;
    g_agc_dev.frame_active = 0;
}

void evo_agc_runtime_present(void)
{
    evo_agc_runtime_frame_end();
}

/*
 * Mark the buffer being drawn now as carrying UI pixels, so the next frame that
 * reuses it clears first. Called by the render loop after the UI pass on the
 * player screen; harmless outside player mode, where every frame clears anyway.
 */
void evo_agc_runtime_note_ui_drawn(void)
{
    if (!g_agc_dev.initialized)
        return;
    g_agc_dev.ui_dirty[g_agc_dev.active_backbuffer] = 1;
}

/*
 * There are two scanout buffers, and in player mode frame_begin deliberately
 * skips the clear for a buffer that carried nothing but video. A present whose
 * frame did not redraw the video quad therefore puts the picture from two
 * presents ago back on the panel - the image flicks between the current frame
 * and the previous one instead of simply holding, which is what a paused
 * picture, a decoder hiccup and the settle after a seek all looked like.
 *
 * The render loop asks whether the buffer it is about to draw into already
 * holds this PTS and redraws the quad when it does not, so a frame slower than
 * the panel is blitted into both buffers before it stops being redrawn.
 * Queried before the blit and stamped after it, both while active_backbuffer
 * still names the buffer being drawn - frame_end flips it afterwards.
 */
int evo_agc_runtime_video_slot_stale(int64_t pts_us)
{
    if (!g_agc_dev.initialized)
        return 0;
    return g_agc_dev.video_pts[g_agc_dev.active_backbuffer] != pts_us;
}

void evo_agc_runtime_note_video_pts(int64_t pts_us)
{
    if (!g_agc_dev.initialized)
        return;
    g_agc_dev.video_pts[g_agc_dev.active_backbuffer] = pts_us;
}

void evo_agc_runtime_set_player_mode(int is_player)
{
    if (g_agc_dev.is_player_mode == is_player)
        return;
    g_agc_dev.is_player_mode = is_player;
    /* a new file decides HDR10 by its own first frame, not the last file's */
    g_agc_dev.last_video_trc = -1;
    if (is_player) {
        /* Clear both scanout buffers once upon entering player mode so letterbox borders are dark */
        for (int b = 0; b < 2; ++b) {
            uint32_t *buf = (uint32_t *)g_agc_dev.scanout_buffers[b];
            if (buf) {
#ifdef NUVIO_APP
                evo_agc_runtime_stream_fill(buf, g_agc_dev.is_hdr ? 0xc0000000u : 0xff000000u,
                                            scanout_tiled_bytes());
#else
                evo_agc_runtime_stream_fill(buf, g_agc_dev.is_hdr ? 0xc0000000u : 0xff100d0du,
                                            scanout_tiled_bytes());
#endif
            }
            g_agc_dev.video_pts[b] = INT64_MIN;
        }
        memset(g_agc_dev.stage_cache, 0, sizeof(g_agc_dev.stage_cache));
    }
}

/*
 * Capture the front buffer as linear BGRA.
 *
 * The scanout is TILED. It is registered with tiling mode 0 (tile, not
 * linear) in both sceVideoOutSetBufferAttribute2 call sites, which is why the
 * bases have to be 2 MB aligned and why the diagnostic above sizes the
 * surface in 64 KB tiles. This function used to walk it as if it were linear:
 *
 *     memcpy(bgra + y * width, src + y * g_agc_dev.width, w * 4);
 *
 * which reads a swizzled surface in raster order and produces horizontal
 * streak garbage. Every screenshot EVO has ever written looked like that; the
 * earlier screenshot fixes corrected the button binding, the toast and the
 * byte order, but nothing ever corrected the layout.
 *
 * The de-swizzle is the inverse of the old CPU present path (pp/src/tile_copy.c,
 * deleted with the software presenter in GL-4) and uses the same table, which
 * is still in the tree:
 *
 *     tiled_index = TILE_HEIGHT * (y / TILE_HEIGHT) * surface_w      [tile row]
 *                 + TILE_SIZE   * (x / TILE_WIDTH)                   [tile in row]
 *                 + PS5_tilemap[y % TILE_HEIGHT][x % TILE_WIDTH]     [within tile]
 *
 * tile_copy wrote linear -> tiled with exactly this address; reading it back
 * the other way is the same expression with source and destination swapped.
 * Verified before it was written: the formula was applied offline to a 4K
 * capture pulled off the console, and turned that streaked image into a
 * pixel-correct Settings screen.
 *
 * Single-threaded and only called for a screenshot, so the per-pixel table
 * lookup does not need the worker pool tile_copy had for per-frame present.
 */
void evo_agc_runtime_read_scanout(uint32_t *bgra, int width, int height)
{
    if (!g_agc_dev.initialized || !bgra || width <= 0 || height <= 0)
        return;
    /* frame_end flips active_backbuffer AFTER handing the just-rendered one to
     * VideoOut, so the buffer actually on screen is the other one. */
    const int front = 1 - g_agc_dev.active_backbuffer;
    const uint32_t *src = (const uint32_t *)g_agc_dev.scanout_buffers[front];
    if (!src)
        return;

    const int sw = g_agc_dev.width;
    const int w  = width  < sw ? width  : sw;
    const int h  = height < g_agc_dev.height ? height : g_agc_dev.height;
    if (w <= 0 || h <= 0)
        return;

    /*
     * The GPU wrote this buffer; the CPU's copy of those lines is stale. Evict
     * before reading or every capture returns whatever the CPU last put there
     * (i.e. the backdrop clear), which would make a working GPU frame look
     * black in a screenshot.
     *
     * Flush the tiled extent, not w*h: the last tile row reaches past the
     * visible pixel count, and a partly-stale tail shows up as torn blocks.
     */
    {
        size_t last = (size_t)((h - 1) >> PS5_TILE_H_SHIFT) * PS5_TILE_HEIGHT * (size_t)sw
                    + (size_t)((w - 1) >> PS5_TILE_W_SHIFT) * PS5_TILE_SIZE
                    + (PS5_TILE_SIZE - 1);
        size_t bytes = (last + 1u) * sizeof(uint32_t);
        if (bytes > (size_t)EVO_AGC_SCANOUT_STRIDE)
            bytes = (size_t)EVO_AGC_SCANOUT_STRIDE;
        evo_agc_runtime_cache_flush(src, bytes);
    }

    for (int y = 0; y < h; ++y) {
        const unsigned short *lut = PS5_tilemap[y & PS5_TILE_H_MASK];
        const size_t row_base =
            (size_t)(y >> PS5_TILE_H_SHIFT) * PS5_TILE_HEIGHT * (size_t)sw;
        uint32_t *dst = bgra + (size_t)y * (size_t)width;

        for (int x0 = 0; x0 < w; x0 += PS5_TILE_WIDTH) {
            const uint32_t *tile = src + row_base +
                ((size_t)(x0 >> PS5_TILE_W_SHIFT) * (size_t)PS5_TILE_SIZE);
            int n = w - x0;
            if (n > PS5_TILE_WIDTH)
                n = PS5_TILE_WIDTH;
            for (int k = 0; k < n; ++k)
                dst[x0 + k] = tile[lut[k]];
        }
    }
}

int evo_agc_probe_rgb(uint8_t *rgb, int n)
{
    if (!g_agc_dev.initialized || !rgb || n <= 0)
        return 0;
    const int front = 1 - g_agc_dev.active_backbuffer;
    const uint32_t *src = (const uint32_t *)g_agc_dev.scanout_buffers[front];
    if (!src)
        return 0;

    const int sw = g_agc_dev.width;
    const int sh = g_agc_dev.height;
    if (sw <= 0 || sh <= 0)
        return 0;

    const int is_hdr = g_agc_dev.is_hdr;

    static const float kGolden = 0.61803398875f;
    float fx = 0.5f, fy = 0.5f;
    for (int i = 0; i < n; i++) {
        /* Low-discrepancy additive recurrence sequence, inset from edges */
        fx += kGolden;        if (fx >= 1.0f) fx -= 1.0f;
        fy += kGolden * 0.5f; if (fy >= 1.0f) fy -= 1.0f;
        int x = (int)((0.1f + 0.8f * fx) * (float)sw);
        int y = (int)((0.1f + 0.8f * fy) * (float)sh);
        if (x < 0) x = 0; if (x >= sw) x = sw - 1;
        if (y < 0) y = 0; if (y >= sh) y = sh - 1;

        const unsigned short *lut = PS5_tilemap[y & PS5_TILE_H_MASK];
        const size_t row_base =
            (size_t)(y >> PS5_TILE_H_SHIFT) * PS5_TILE_HEIGHT * (size_t)sw;
        const uint32_t *tile = src + row_base +
            ((size_t)(x >> PS5_TILE_W_SHIFT) * (size_t)PS5_TILE_SIZE);
        const uint32_t *pixel_addr = &tile[lut[x & PS5_TILE_W_MASK]];

        evo_agc_runtime_cache_flush(pixel_addr, sizeof(uint32_t));
        uint32_t px = *pixel_addr;

        if (is_hdr) {
            /* 10:10:10:2 Bgr10A2 -> scale 10-bit to 8-bit */
            rgb[i * 3 + 0] = (uint8_t)(((px >> 20) & 0x3FF) >> 2); /* R */
            rgb[i * 3 + 1] = (uint8_t)(((px >> 10) & 0x3FF) >> 2); /* G */
            rgb[i * 3 + 2] = (uint8_t)((px         & 0x3FF) >> 2); /* B */
        } else {
            /* 8:8:8:8 Bgra8 */
            rgb[i * 3 + 0] = (uint8_t)((px >> 16) & 0xFF);         /* R */
            rgb[i * 3 + 1] = (uint8_t)((px >> 8)  & 0xFF);         /* G */
            rgb[i * 3 + 2] = (uint8_t)(px         & 0xFF);         /* B */
        }
    }
    return n;
}

void evo_agc_runtime_note_draw(void)
{
    g_agc_dev.frame_has_draws = 1;
}

void evo_agc_runtime_note_drop(int kind)
{
    if (kind == 0)
        g_agc_dev.ring_alloc_fail++;
    else
        g_agc_dev.tex_alloc_fail++;
}

evo_agc_user_data_layout_t evo_agc_runtime_get_user_data_layout(int pipeline_id)
{
    evo_agc_user_data_layout_t empty = {0, 0, -1, -1, -1, -1};
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT ||
        !g_agc_dev.pipelines[pipeline_id].valid)
        return empty;
    return g_agc_dev.pipelines[pipeline_id].user_data;
}

uint64_t evo_agc_runtime_get_pipe_draw_modifier(int pipeline_id)
{
    if (pipeline_id < 0 || pipeline_id >= EVO_AGC_PIPE_COUNT ||
        !g_agc_dev.pipelines[pipeline_id].valid)
        return 0;
    return g_agc_dev.pipelines[pipeline_id].draw_modifier;
}

SceAgcCommandBuffer *evo_agc_runtime_get_current_cb(void)
{
    return g_agc_dev.frame_active ? &g_agc_dev.current_cb : NULL;
}

evo_agc_transient_ring_t *evo_agc_runtime_get_transient_ring(void)
{
    return &g_agc_dev.transient_ring;
}

uint32_t evo_agc_runtime_get_current_slot(void)
{
    return g_agc_dev.current_slot;
}

void evo_agc_runtime_get_size(int *width, int *height)
{
    if (width) *width = g_agc_dev.width;
    if (height) *height = g_agc_dev.height;
}

int evo_agc_runtime_is_display_hdr(void)
{
    return g_agc_dev.initialized ? g_agc_dev.display_is_hdr : 0;
}

int evo_agc_runtime_get_display_dynamic_range(void)
{
    return g_agc_dev.initialized ? g_agc_dev.display_dynamic_range : 0;
}

int evo_agc_runtime_supports_120hz(void)
{
    return g_agc_dev.initialized ? g_agc_dev.supports_120hz : 0;
}

int evo_agc_runtime_is_120hz(void)
{
    return g_agc_dev.initialized ? g_agc_dev.is_120hz : 0;
}

int evo_agc_runtime_get_refresh_rate(void)
{
    return g_agc_dev.initialized ? g_agc_dev.current_refresh_rate : 0;
}

/* CB_COLOR0_INFO is word 2 of every block setup_color_target() builds (the
 * upscaler patches the same word for its FP16 targets). FORMAT is bits 2..6:
 * COLOR_8_8_8_8 = 10, COLOR_2_10_10_10 = 9. NUMBER_TYPE stays UNORM and
 * COMP_SWAP stays ALT, so the 10:10:10:2 word is BGR ordered like the
 * 8-bit one - matching Bgr10A2Bt2100Pq. */
static void agc_scanout_set_10bit(int ten_bit)
{
    for (int i = 0; i < 2; ++i) {
        SceAgcRegister *ct = g_agc_dev.gpu_regs->color_targets[i];
        ct[2].value = (ct[2].value & ~0x7cu) | ((ten_bit ? 9u : 10u) << 2);
    }
    evo_agc_runtime_cache_flush(g_agc_dev.gpu_regs, sizeof(evo_agc_gpu_regs_t));
}

int32_t sceVideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t set_index,
                                                const void *attribute, const void *option);

int evo_agc_runtime_set_hdr_output(int enable)
{
    enable = enable ? 1 : 0;
    if (!g_agc_dev.initialized || g_agc_dev.video_handle < 0)
        return -1;
    if (g_agc_dev.frame_active) {
        evo_boot_log("agc hdr: switch requested mid-frame - refused");
        return -1;
    }
    if (enable == g_agc_dev.is_hdr)
        return 0;
    if (enable && !g_agc_dev.pipelines[EVO_AGC_PIPE_VIDEO_HDR_PQ].valid) {
        evo_boot_log("agc hdr: PQ video pipeline missing - staying SDR");
        return -1;
    }

    const uint64_t vfmt = enable ? EVO_AGC_VIDEO_FORMAT_HDR : EVO_AGC_VIDEO_FORMAT_SDR;
    evo_boot_log("agc hdr: switching scanout to %s (fmt=%#llx)",
                 enable ? "HDR10 Bgr10A2Bt2100Pq" : "SDR Bgra8",
                 (unsigned long long)vfmt);
    evo_boot_log_flush();

    /* No GPU write in the old format may land after the retype. */
    agc_wait_gpu_idle(200);

    /*
     * Retype the registered set in place, applied by VideoOut at the next
     * flip - the call games use to toggle HDR. NOT unregister + register:
     * the set on screen can't be unregistered (0x80290009 RESOURCE_BUSY,
     * hardware 2026-09-28), the re-register then hits 0x80290010
     * SLOT_OCCUPIED, and that failed sequence once left the next present
     * hanging - the "display went blank" run. A refusal here changes
     * nothing, so playback just carries on in SDR.
     */
    evo_video_attribute_t attr;
    memset(&attr, 0, sizeof(attr));
    sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                   (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height,
                                   0, 0, 0);
    int32_t rc = sceVideoOutSubmitChangeBufferAttribute2(g_agc_dev.video_handle, 0, &attr, NULL);
    evo_boot_log("agc hdr: SubmitChangeBufferAttribute2 rc=%d (0x%08x)", rc, (unsigned)rc);
    if (rc != 0) {
        evo_boot_log("agc hdr: display refused the %s attribute - unchanged",
                     enable ? "HDR10" : "SDR");
        evo_boot_log_flush();
        return -1;
    }

    agc_scanout_set_10bit(enable);
    g_agc_dev.is_hdr = enable;
    /* Both buffers still hold pixels in the old format: clear them on their
     * next use, and redraw the video into each. */
    g_agc_dev.ui_dirty[0] = g_agc_dev.ui_dirty[1] = 1;
    g_agc_dev.video_pts[0] = g_agc_dev.video_pts[1] = INT64_MIN;
    g_agc_dev.bound_pipeline = -1;

    evo_vo_output_status vout;
    memset(&vout, 0, sizeof(vout));
    int32_t vorc = sceVideoOutGetOutputStatus(g_agc_dev.video_handle, &vout);
    evo_boot_log("agc hdr: now %s - output status rc=%d dynamic_range=%u (%s) flags=%#llx",
                 enable ? "HDR10" : "SDR", vorc, vout.dynamic_range,
                 vout.dynamic_range == 2 ? "HDR" : (vout.dynamic_range == 1 ? "SDR" : "unknown"),
                 (unsigned long long)vout.flags);
    evo_boot_log_flush();
    return 0;
}

int evo_agc_runtime_hdr_output_active(void)
{
    return g_agc_dev.initialized ? g_agc_dev.is_hdr : 0;
}

int evo_agc_runtime_last_video_trc(void)
{
    return g_agc_dev.initialized ? g_agc_dev.last_video_trc : -1;
}

int evo_agc_runtime_set_120hz(int enable)
{
    if (!g_agc_dev.initialized || g_agc_dev.video_handle < 0)
        return -1;

    if (enable && !g_agc_dev.supports_120hz) {
        evo_boot_log("agc: 120hz requested but not supported by display");
        return -1;
    }

    if ((enable && g_agc_dev.is_120hz) || (!enable && !g_agc_dev.is_120hz)) {
        return 0;
    }

    uint64_t mode = enable ? UINT64_C(0x000000000000000F) /* Mode119_88Hz */
                           : UINT64_C(0x0000000000000001); /* Default */;

    evo_boot_log("agc: switching output mode to %#llx (120hz=%d)",
                 (unsigned long long)mode, enable);

    agc_wait_gpu_idle(200);

    int32_t rc = sceVideoOutConfigureOutput(g_agc_dev.video_handle, mode, NULL, NULL, 0);
    evo_boot_log("agc: sceVideoOutConfigureOutput rc=%d (0x%08x)", rc, (unsigned)rc);

    evo_vo_resolution_status vres;
    memset(&vres, 0, sizeof(vres));
    int32_t vrc = sceVideoOutGetResolutionStatus(g_agc_dev.video_handle, &vres);

    evo_vo_output_status vout;
    memset(&vout, 0, sizeof(vout));
    int32_t vorc = sceVideoOutGetOutputStatus(g_agc_dev.video_handle, &vout);

    evo_boot_log("agc post-config: res_rc=%d full=%ux%u pane=%ux%u refresh_id=%llu | out_rc=%d dynamic_range=%u refresh=%llu",
                 vrc, vres.full_width, vres.full_height, vres.pane_width, vres.pane_height,
                 (unsigned long long)vres.refresh_rate,
                 vorc, vout.dynamic_range, (unsigned long long)vout.refresh_rate);

    if (rc == 0) {
        g_agc_dev.is_120hz = enable ? 1 : 0;
        g_agc_dev.current_refresh_rate = (int)(vres.refresh_rate != 0 ? vres.refresh_rate : vout.refresh_rate);
    }

    int new_w = (vrc == 0 && vres.full_width > 0) ? (int)vres.full_width : g_agc_dev.width;
    int new_h = (vrc == 0 && vres.full_height > 0) ? (int)vres.full_height : g_agc_dev.height;
    if (new_w > 0 && new_h > 0 && (new_w != g_agc_dev.width || new_h != g_agc_dev.height)) {
        if (new_w <= EVO_AGC_MAX_RENDER_W && new_h <= EVO_AGC_MAX_RENDER_H) {
            evo_boot_log("agc: display resolution changed to %dx%d, updating scanout", new_w, new_h);
            sceVideoOutUnregisterBuffers(g_agc_dev.video_handle, 0);
            (void)evo_agc_apply_render_size(g_agc_dev.agc_defaults, new_w, new_h);

            evo_video_buffer_t video_buffers[2] = {
                {g_agc_dev.scanout_buffers[0], NULL, NULL, NULL},
                {g_agc_dev.scanout_buffers[1], NULL, NULL, NULL},
            };
            evo_video_attribute_t attr;
            memset(&attr, 0, sizeof(attr));
            uint64_t vfmt = g_agc_dev.is_hdr ? EVO_AGC_VIDEO_FORMAT_HDR : EVO_AGC_VIDEO_FORMAT_SDR;
            sceVideoOutSetBufferAttribute2(&attr, vfmt, 0,
                                           (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height,
                                           0, 0, 0);
            int reg_rc = sceVideoOutRegisterBuffers2(g_agc_dev.video_handle, 0, 0,
                                                    video_buffers, 2, &attr, 0, NULL);
            evo_boot_log("agc: re-register buffers rc=%d", reg_rc);
        }
    }

    evo_boot_log_flush();
    return rc;
}


/*
 * Stage one plane where the GPU can read it.
 *
 * `cache_slot` (0 = Y, 1 = U/UV, 2 = V) keys a one-entry memo of the last
 * staged copy. A frame slower than the panel is now blitted into both scanout
 * buffers (see evo_agc_runtime_video_slot_stale), and without the memo the
 * second blit re-copied the whole plane - 3.1 MB at 1080p, 12.4 MB at 4K.
 * A transient-ring slice stays intact until its slot is reopened, which is
 * EVO_AGC_FRAME_SLOTS frames later, so a copy is safe to reuse for one more
 * frame. The native decoder's own buffers skip all of this via `is_direct`.
 */
static int stage_plane(evo_agc_transient_ring_t *ring, uint32_t slot,
                       const uint8_t *src, int src_pitch,
                       uint32_t width, uint32_t height, uint32_t bpp,
                       int is_direct, int cache_slot, int64_t pts_us,
                       uint32_t *out_pitch, uint64_t *out_gpu)
{
    if (!ring || !src || src_pitch <= 0 || width == 0 || height == 0 || bpp == 0) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t row_bytes = width * bpp;
    if ((uint32_t)src_pitch < row_bytes) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t pitch = (row_bytes + 255u) & ~255u;

    if (is_direct && ((uintptr_t)src & 255u) == 0u && ((uint32_t)src_pitch & 255u) == 0u) {
        *out_pitch = (uint32_t)src_pitch;
        *out_gpu = (uint64_t)(uintptr_t)src;
        return 0;
    }

    const int cacheable = (cache_slot >= 0 && cache_slot < 3 && pts_us != INT64_MIN);
    if (cacheable) {
        const typeof(g_agc_dev.stage_cache[0]) *c = &g_agc_dev.stage_cache[cache_slot];
        if (c->valid && c->src == src && c->src_pitch == src_pitch &&
            c->width == width && c->height == height && c->bpp == bpp &&
            c->pts_us == pts_us &&
            (g_agc_dev.frame_counter - c->frame_staged) < 2u) {
            *out_pitch = c->gpu_pitch;
            *out_gpu = c->gpu_addr;
            return 0;
        }
    }

    size_t total_bytes = (size_t)pitch * (size_t)height;
    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(ring, slot, total_bytes, 256, &slice) != EVO_AGC_TRANSIENT_OK) {
        *out_pitch = pitch;
        *out_gpu = 0;
        return -1;
    }

    stream_copy_rows((uint8_t *)slice.cpu, pitch, src, (size_t)src_pitch,
                     row_bytes, height);

    if (cacheable) {
        typeof(g_agc_dev.stage_cache[0]) *c = &g_agc_dev.stage_cache[cache_slot];
        c->src = src;
        c->src_pitch = src_pitch;
        c->width = width;
        c->height = height;
        c->bpp = bpp;
        c->pts_us = pts_us;
        c->frame_staged = g_agc_dev.frame_counter;
        c->gpu_pitch = pitch;
        c->gpu_addr = slice.gpu_addr;
        c->valid = 1;
    }

    *out_pitch = pitch;
    *out_gpu = slice.gpu_addr;
    return 0;
}

static int stage_planar_uv_to_rg16(evo_agc_transient_ring_t *ring, uint32_t slot,
                                   const uint8_t *u, int u_pitch,
                                   const uint8_t *v, int v_pitch,
                                   uint32_t cw2, uint32_t ch2,
                                   uint32_t *out_pitch, uint64_t *out_gpu)
{
    if (!ring || !u || !v || u_pitch <= 0 || v_pitch <= 0 || cw2 == 0 || ch2 == 0) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    /* cw2 chroma pixels per row. Each pixel has a 16-bit U and a 16-bit V (4 bytes total). */
    uint32_t row_bytes = cw2 * 4u;
    if ((uint32_t)u_pitch < cw2 * 2u || (uint32_t)v_pitch < cw2 * 2u) {
        if (out_pitch) *out_pitch = 0;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    uint32_t pitch = (row_bytes + 255u) & ~255u;
    size_t total_bytes = (size_t)pitch * (size_t)ch2;

    evo_agc_transient_slice_t slice;
    if (evo_agc_transient_ring_alloc(ring, slot, total_bytes, 256, &slice) != EVO_AGC_TRANSIENT_OK) {
        if (out_pitch) *out_pitch = pitch;
        if (out_gpu) *out_gpu = 0;
        return -1;
    }

    /* Interleave straight into the GPU buffer with non-temporal stores - at
     * 4K HDR this plane is 8.3 MB a frame, and the clflush walk it used to
     * need afterwards cost about as much again. */
    uint8_t *dst = (uint8_t *)slice.cpu;
    const int nt = nt_stores_enabled() && ((uintptr_t)dst & 15u) == 0u &&
                   (pitch & 15u) == 0u;
    const uint32_t vec = nt ? (cw2 & ~3u) : 0u;
    for (uint32_t r = 0; r < ch2; ++r) {
        const uint16_t * __restrict src_u = (const uint16_t *)(u + (size_t)r * (size_t)u_pitch);
        const uint16_t * __restrict src_v = (const uint16_t *)(v + (size_t)r * (size_t)v_pitch);
        uint32_t * __restrict dst_row = (uint32_t *)(dst + (size_t)r * (size_t)pitch);
        for (uint32_t c = 0; c < vec; c += 4u) {
            const __m128i q = _mm_set_epi32(
                (int)((uint32_t)src_u[c + 3] | ((uint32_t)src_v[c + 3] << 16)),
                (int)((uint32_t)src_u[c + 2] | ((uint32_t)src_v[c + 2] << 16)),
                (int)((uint32_t)src_u[c + 1] | ((uint32_t)src_v[c + 1] << 16)),
                (int)((uint32_t)src_u[c + 0] | ((uint32_t)src_v[c + 0] << 16)));
            _mm_stream_si128((__m128i *)(dst_row + c), q);
        }
        for (uint32_t c = vec; c < cw2; ++c)
            dst_row[c] = (uint32_t)src_u[c] | ((uint32_t)src_v[c] << 16);
        if (vec < cw2)
            evo_agc_runtime_cache_flush(dst_row + vec, (size_t)(cw2 - vec) * 4u);
    }
    if (nt)
        _mm_sfence();
    else
        evo_agc_runtime_cache_flush(slice.cpu, total_bytes);

    if (out_pitch) *out_pitch = pitch;
    if (out_gpu) *out_gpu = slice.gpu_addr;
    return 0;
}

/*
 * Composite the CPU-rasterised OSD over whatever is already in the frame.
 *
 * main.c rasterises the playback OSD into gl_scratch and hands it to
 * evo_gl_composite_bgra(). In --agc builds that symbol resolves to the no-op in
 * evo_gl_context_stub.c, so the OSD was rendered every frame and then thrown
 * away - video played with no scrub bar, no subtitles, no HUD. The stub now
 * forwards here.
 *
 * `fb` is 0xAABBGGRR, which little-endian is the byte order R,G,B,A - exactly
 * what the texture descriptor's XYZW selects expect, so no swizzle is needed.
 * Alpha is premultiplied (see evo_rmlui_render.cpp), hence BLEND_PREMULTIPLIED.
 *
 * `upload` is main.c's "the OSD actually changed" hint: the 8 MB copy is skipped
 * when it has not, and the previous contents are drawn again.
 */
void evo_agc_composite_bgra(const uint32_t *fb, int w, int h, int upload)
{
    if (!g_agc_dev.initialized || !fb || w <= 0 || h <= 0 ||
        !g_agc_dev.composite_pixels || !g_agc_dev.frame_active)
        return;
    if (w > g_agc_dev.width || h > g_agc_dev.height)
        return;

    const uint32_t pitch = g_agc_dev.composite_pitch;
    if (upload) {
        stream_copy_rows(g_agc_dev.composite_pixels, pitch,
                         (const uint8_t *)fb, (size_t)w * 4u,
                         (size_t)w * 4u, (uint32_t)h);
    }

    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;

    /* Full-screen quad in pixel space, white vertex colour so the texture
     * passes through untinted. */
    struct { float x, y; uint32_t rgba; float u, v; } *quad =
        (void *)g_agc_dev.composite_quad;
    quad[0] = (typeof(*quad)){0.0f,        0.0f,        0xffffffffu, 0.0f, 0.0f};
    quad[1] = (typeof(*quad)){(float)w,    0.0f,        0xffffffffu, 1.0f, 0.0f};
    quad[2] = (typeof(*quad)){0.0f,        (float)h,    0xffffffffu, 0.0f, 1.0f};
    quad[3] = (typeof(*quad)){(float)w,    (float)h,    0xffffffffu, 1.0f, 1.0f};
    evo_agc_runtime_cache_flush(quad, 4 * 20);

    evo_agc_transient_slice_t cons, cons_d, vsh, tex_d;
    if (evo_agc_transient_ring_alloc(ring, slot, 80, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48, 16, &tex_d) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return;
    }

    float *m = (float *)cons.cpu;                 /* pixel space -> NDC */
    memset(m, 0, 80);
    m[0]  =  2.0f / (float)g_agc_dev.width;
    m[5]  = -2.0f / (float)g_agc_dev.height;
    m[10] =  1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;

    evo_agc_build_constant_vsharp((uint32_t *)cons_d.cpu, cons.gpu_addr, 80);
    evo_agc_build_vsharp((uint32_t *)vsh.cpu,
                         (uint64_t)(uintptr_t)g_agc_dev.composite_quad, 20, 4);
    if (evo_agc_build_tsharp_rgba8((uint32_t *)tex_d.cpu,
                                   (uint64_t)(uintptr_t)g_agc_dev.composite_pixels,
                                   (uint32_t)w, (uint32_t)h, pitch) != 0) {
        g_agc_dev.tex_alloc_fail++;
        return;
    }
    evo_agc_build_ssharp((uint32_t *)tex_d.cpu + 8, 1 /* clamp */, 0 /* point */);

    g_agc_dev.bound_pipeline = -1;                /* video pipeline was bound */
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    const evo_agc_user_data_layout_t ud =
        evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 ||
        ud.vs_vertex_table_dword < 0 || ud.ps_texture_table_dword < 0 ||
        ud.vs_count > 16 || ud.ps_count > 16)
        return;

    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword]  = (uint32_t)cons_d.gpu_addr;
    vs_user[ud.vs_vertex_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, ud.vs_count);

    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_d.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices);
    evo_agc_runtime_note_draw();
}

/*
 * Nuvio: the player's overlays - layer 0 subtitles, layer 1 controls and
 * menus, drawn in that order. Unlike
 * evo_agc_composite_bgra above, the canvas may be smaller than the scanout -
 * it is drawn at 1920x1080 and stretched over the whole display with bilinear
 * filtering - and it can be faded as a whole: `opacity` scales every channel,
 * which is exactly a fade for premultiplied pixels. Each layer has two
 * staging textures in the composite carve that alternate between uploads, so
 * new pixels never land in a texture an earlier, still-queued frame samples;
 * the quad lives in the per-frame transient ring for the same reason.
 * upload=0 redraws the layer's last upload.
 */
void evo_agc_composite_overlay(int layer, const uint32_t *fb, int w, int h, int upload,
                               float opacity)
{
    static int s_buf[2];       /* which staging texture holds the last upload */
    static int s_w[2], s_h[2];

    if (!g_agc_dev.initialized || !g_agc_dev.composite_pixels || !g_agc_dev.frame_active ||
        layer < 0 || layer > 1 || w <= 0 || h <= 0 || opacity <= 0.0f)
        return;
    if (opacity > 1.0f)
        opacity = 1.0f;

    const uint32_t pitch = ((uint32_t)w * 4u + 255u) & ~255u;
    const size_t one = (((size_t)pitch * (size_t)h) + 255u) & ~(size_t)255;
    /* Two layers x two buffers fit the 40 MB carve up to 1920x1080 canvases. */
    if (4u * one > (size_t)EVO_AGC_COMPOSITE_SIZE)
        return;
    uint8_t *base = g_agc_dev.composite_pixels + (size_t)layer * 2u * one;

    if (upload && fb) {
        s_buf[layer] ^= 1;
        s_w[layer] = w;
        s_h[layer] = h;
        stream_copy_rows(base + (size_t)s_buf[layer] * one, pitch,
                         (const uint8_t *)fb, (size_t)w * 4u, (size_t)w * 4u, (uint32_t)h);
    } else if (s_w[layer] != w || s_h[layer] != h) {
        return;                /* nothing of this size uploaded yet */
    }
    uint8_t *pixels = base + (size_t)s_buf[layer] * one;

    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;
    evo_agc_transient_slice_t cons, cons_d, verts, vsh, tex_d;
    if (evo_agc_transient_ring_alloc(ring, slot, 80, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 4 * 20, 16, &verts) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48, 16, &tex_d) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return;
    }

    /* Whole display, tinted by the opacity in every channel. */
    const uint32_t o = (uint32_t)(opacity * 255.0f + 0.5f);
    const uint32_t tint = o | (o << 8) | (o << 16) | (o << 24);
    const float dw = (float)g_agc_dev.width, dh = (float)g_agc_dev.height;
    struct { float x, y; uint32_t rgba; float u, v; } *quad = (void *)verts.cpu;
    quad[0] = (typeof(*quad)){0.0f, 0.0f, tint, 0.0f, 0.0f};
    quad[1] = (typeof(*quad)){dw,   0.0f, tint, 1.0f, 0.0f};
    quad[2] = (typeof(*quad)){0.0f, dh,   tint, 0.0f, 1.0f};
    quad[3] = (typeof(*quad)){dw,   dh,   tint, 1.0f, 1.0f};

    float *m = (float *)cons.cpu;                 /* pixel space -> NDC */
    memset(m, 0, 80);
    m[0]  =  2.0f / dw;
    m[5]  = -2.0f / dh;
    m[10] =  1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;

    evo_agc_build_constant_vsharp((uint32_t *)cons_d.cpu, cons.gpu_addr, 80);
    evo_agc_build_vsharp((uint32_t *)vsh.cpu, verts.gpu_addr, 20, 4);
    if (evo_agc_build_tsharp_rgba8((uint32_t *)tex_d.cpu, (uint64_t)(uintptr_t)pixels,
                                   (uint32_t)w, (uint32_t)h, pitch) != 0) {
        g_agc_dev.tex_alloc_fail++;
        return;
    }
    /* Bilinear unless the canvas already matches the panel pixel for pixel. */
    evo_agc_build_ssharp((uint32_t *)tex_d.cpu + 8, 1 /* clamp */,
                         (w == g_agc_dev.width && h == g_agc_dev.height) ? 0 : 1);

    g_agc_dev.bound_pipeline = -1;                /* video pipeline was bound */
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_PREMULTIPLIED);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;

    const evo_agc_user_data_layout_t ud =
        evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 ||
        ud.vs_vertex_table_dword < 0 || ud.ps_texture_table_dword < 0 ||
        ud.vs_count > 16 || ud.ps_count > 16)
        return;

    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword]  = (uint32_t)cons_d.gpu_addr;
    vs_user[ud.vs_vertex_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, ud.vs_count);

    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_d.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices);
    evo_agc_runtime_note_draw();
}

/*
 * Nuvio: paint the whole backbuffer opaque black on the GPU. The player calls
 * it first in every frame it presents, so overlays drawn into the letterbox
 * bars never linger and frame_begin never needs its CPU clear (33 MB of
 * stores at 4K - most of a frame). The source is a small black texture at the
 * very end of the composite carve, past both overlay layers.
 */
void evo_agc_runtime_clear_black(void)
{
    static int s_tex_ready;
    if (!g_agc_dev.initialized || !g_agc_dev.composite_pixels || !g_agc_dev.frame_active)
        return;
    uint8_t *tex = g_agc_dev.composite_pixels + (size_t)EVO_AGC_COMPOSITE_SIZE - 4096u;
    tex = (uint8_t *)((uintptr_t)tex & ~(uintptr_t)255);
    if (!s_tex_ready) {
        /* 8x8 texels at a 256-byte pitch: 8 rows of 64 words. */
        uint32_t *px = (uint32_t *)tex;
        for (int i = 0; i < 8 * 64; i++)
            px[i] = 0xff000000u;               /* R,G,B = 0, A = 255 */
        evo_agc_runtime_cache_flush(tex, 8 * 256);
        s_tex_ready = 1;
    }

    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;
    evo_agc_transient_slice_t cons, cons_d, verts, vsh, tex_d;
    if (evo_agc_transient_ring_alloc(ring, slot, 80, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons_d) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 4 * 20, 16, &verts) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 48, 16, &tex_d) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return;
    }
    const float dw = (float)g_agc_dev.width, dh = (float)g_agc_dev.height;
    struct { float x, y; uint32_t rgba; float u, v; } *quad = (void *)verts.cpu;
    quad[0] = (typeof(*quad)){0.0f, 0.0f, 0xffffffffu, 0.0f, 0.0f};
    quad[1] = (typeof(*quad)){dw,   0.0f, 0xffffffffu, 1.0f, 0.0f};
    quad[2] = (typeof(*quad)){0.0f, dh,   0xffffffffu, 0.0f, 1.0f};
    quad[3] = (typeof(*quad)){dw,   dh,   0xffffffffu, 1.0f, 1.0f};
    float *m = (float *)cons.cpu;
    memset(m, 0, 80);
    m[0]  =  2.0f / dw;
    m[5]  = -2.0f / dh;
    m[10] =  1.0f;
    m[12] = -1.0f;
    m[13] =  1.0f;
    m[15] =  1.0f;
    evo_agc_build_constant_vsharp((uint32_t *)cons_d.cpu, cons.gpu_addr, 80);
    evo_agc_build_vsharp((uint32_t *)vsh.cpu, verts.gpu_addr, 20, 4);
    if (evo_agc_build_tsharp_rgba8((uint32_t *)tex_d.cpu, (uint64_t)(uintptr_t)tex, 8, 8, 256) != 0) {
        g_agc_dev.tex_alloc_fail++;
        return;
    }
    evo_agc_build_ssharp((uint32_t *)tex_d.cpu + 8, 1, 0);

    g_agc_dev.bound_pipeline = -1;
    evo_agc_runtime_bind_pipeline(EVO_AGC_PIPE_UI);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                               (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
    g_agc_dev.scissor_x = 0;
    g_agc_dev.scissor_y = 0;
    g_agc_dev.scissor_w = g_agc_dev.width;
    g_agc_dev.scissor_h = g_agc_dev.height;
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(EVO_AGC_PIPE_UI);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 || ud.vs_vertex_table_dword < 0 ||
        ud.ps_texture_table_dword < 0 || ud.vs_count > 16 || ud.ps_count > 16)
        return;
    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword]  = (uint32_t)cons_d.gpu_addr;
    vs_user[ud.vs_vertex_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)tex_d.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, ud.ps_count);
    evo_agc_writer_draw_index(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices);
    evo_agc_runtime_note_draw();
}

/* =========================================================================
 * #103 upscaler
 *
 * Off is the single pass below: YUV -> RGB, bilinear, straight into the
 * scanout. With an upscaler on, and a source smaller than the image on the
 * panel, the YUV pass instead renders at SOURCE size into scratch surface L0,
 * and the chain draws the upscaled picture into the scanout:
 *
 *   Sharp   L0 -EASU-> E (visible image size) -RCAS-> scanout
 *   AI (S)  L0 -conv0..3-> F (RGBA16F, ping-pong) ; L0 + F -final-> scanout
 *   AI (M)  L0 -conv0..6-> F, and after each conv acc_k = acc_{k-1} +
 *           W_k * crelu(f_k) into A (ping-pong) ; L0 + A -final-> scanout
 *
 * The chain writes only the visible image rectangle - the same pixels the
 * Off quad covers - with the Fit/Fill/Stretch scale folded into that rect and
 * the source UV sub-rect it shows, so letterbox bars behave exactly as before.
 *
 * Scratch surfaces are a dedicated lazily-allocated block, not the RmlUi layer
 * pool: RmlUi CPU-clears a layer on acquire, and doing that to a surface the
 * GPU is still upscaling the previous frame from would corrupt one or the
 * other. Like every colour target setup_color_target() builds, they are
 * rendered 64KB_R_X TILED, so they are sampled back with a tiled T#
 * (evo_agc_build_tsharp_render_target) sized to the exact target - a linear
 * T# reads them as scrambled blocks, which was the first hardware run.
 *
 * Passes are separated by evo_agc_flush_color_target(): RELEASE_MEM event 45
 * with GCR 0xC = CB flush + GLV/GL1 invalidate, the same barrier the RmlUi
 * blur relies on between its H and V passes.
 * ========================================================================= */

#define EVO_AGC_UP_SURFACES   5
#define EVO_AGC_UP_EXT_SURFACES 8   /* Anime4K UL only */
/* 36 MB: colour targets are 64KB_R_X tiled, so a 4K RGBA8 image takes
 * 30 x 17 blocks of 64 KB = 33.4 MB, not its 31.6 MB linear size. */
#define EVO_AGC_UP_SLOT_BYTES UINT64_C(0x02400000)
enum {
    UP_SLOT_L0 = 0, UP_SLOT_E = 1, UP_SLOT_F0 = 1, UP_SLOT_A0 = 3,
    /* UL: two sets of 3 feature maps, two sets of 3 accumulators. Slots 5+
     * live in the second block. */
    UP_SLOT_UL_F = 1, UP_SLOT_UL_A = 7,
};

/* Whole-frame GPU time (submit -> retire) of an upscaled frame. A 60 fps
 * frame has 16.7 ms; leave room for the UI, the flip and poll granularity. */
#define EVO_AGC_UP_BUDGET_US  12000u
#define EVO_AGC_UP_WINDOW     120u

typedef struct {
    uint64_t addr;
    uint32_t width, height;   /* rendered extent */
    int      fp16;
    int      scanout;
} agc_up_surface_t;

typedef struct {
    uint64_t addr;
    uint32_t width, height;
    int      fp16;
    int      bilinear;
} agc_up_tex_t;

typedef struct {
    int      mode;                        /* SHARP or AI, after every fallback */
    int      net;                         /* AI: 0 = S, 1 = M, 2 = UL */
    uint32_t src_w, src_h;
    int      x0, y0, x1, y1;              /* visible image rect on the scanout */
    float    uv[4];                       /* source UV origin + extent shown there */
} agc_up_plan_t;

/* current_layer_target while a scratch surface is bound, so the next
 * evo_agc_set_layer_target() always re-emits its target. */
static const evo_agc_layer_surface_t s_up_target_sentinel;


static agc_up_surface_t up_surface(int slot, uint32_t w, uint32_t h, int fp16)
{
    agc_up_surface_t t;
    memset(&t, 0, sizeof(t));
    const int blk = slot >= EVO_AGC_UP_SURFACES;
    t.addr = (uint64_t)(uintptr_t)g_agc_dev.up.mem_base[blk] +
             (uint64_t)(slot - (blk ? EVO_AGC_UP_SURFACES : 0)) * EVO_AGC_UP_SLOT_BYTES;
    t.width = w;
    t.height = h;
    t.fp16 = fp16;
    return t;
}

/* Tiled footprint: 64 KB blocks of 128x128 pixels at 4 bpp, 128x64 at 8. */
static int up_fits(uint32_t w, uint32_t h, uint32_t bpp)
{
    const uint32_t bh = bpp == 8u ? 64u : 128u;
    const uint64_t blocks = (uint64_t)((w + 127u) / 128u) * ((h + bh - 1u) / bh);
    return blocks * 0x10000u <= EVO_AGC_UP_SLOT_BYTES;
}

static agc_up_tex_t up_tex(const agc_up_surface_t *s, int bilinear)
{
    agc_up_tex_t t = { s->addr, s->width, s->height, s->fp16, bilinear };
    return t;
}

/* Direct memory on first use rather than at boot: the upscaler defaults to
 * Off and most sessions never need it. Block 0 (180 MB) serves every mode,
 * block 1 (288 MB) only Anime4K UL. One attempt per block. */
static int agc_upscale_alloc(int blk)
{
    if (g_agc_dev.up.alloc_state[blk])
        return g_agc_dev.up.alloc_state[blk] > 0 ? 0 : -1;

    const size_t bytes = (size_t)(blk ? EVO_AGC_UP_EXT_SURFACES : EVO_AGC_UP_SURFACES) *
                         EVO_AGC_UP_SLOT_BYTES;
    int64_t off = -1;
    void *va = NULL;
    int rc = sceKernelAllocateDirectMemory(0, (off_t)16 * 1024 * 1024 * 1024ULL, bytes,
                                           EVO_AGC_DIRECT_MEM_ALIGN,
                                           EVO_AGC_DIRECT_MEM_TYPE, &off);
    if (rc == 0 && off >= 0) {
        rc = sceKernelMapDirectMemory(&va, bytes, EVO_AGC_MAP_PROTECTION, 0, off,
                                      EVO_AGC_DIRECT_MEM_ALIGN);
        if (rc != 0 || !va) {
            sceKernelReleaseDirectMemory(off, bytes);
            va = NULL;
        }
    }
    if (!va) {
        g_agc_dev.up.alloc_state[blk] = -1;
        evo_boot_log("agc upscale: scratch block %d alloc of %zu MB FAILED rc=%#x; %s",
                     blk, bytes >> 20, (unsigned)rc,
                     blk ? "AI Maximum unavailable" : "upscaler off");
        return -1;
    }
    g_agc_dev.up.mem_offset[blk] = off;
    g_agc_dev.up.mem_base[blk] = (uint8_t *)va;
    g_agc_dev.up.alloc_state[blk] = 1;
    evo_boot_log("agc upscale: %zu MB scratch block %d at %p (%zu x %llu MB)",
                 bytes >> 20, blk, va, bytes / EVO_AGC_UP_SLOT_BYTES,
                 (unsigned long long)(EVO_AGC_UP_SLOT_BYTES >> 20));
    return 0;
}

/* Called from shutdown after the GPU drain. */
static void agc_upscale_release(void)
{
    for (int blk = 0; blk < 2; ++blk) {
        const size_t bytes = (size_t)(blk ? EVO_AGC_UP_EXT_SURFACES : EVO_AGC_UP_SURFACES) *
                             EVO_AGC_UP_SLOT_BYTES;
        if (g_agc_dev.up.mem_base[blk]) {
            sceKernelMunmap(g_agc_dev.up.mem_base[blk], bytes);
            g_agc_dev.up.mem_base[blk] = NULL;
        }
        if (g_agc_dev.up.mem_offset[blk] >= 0) {
            sceKernelReleaseDirectMemory(g_agc_dev.up.mem_offset[blk], bytes);
            g_agc_dev.up.mem_offset[blk] = -1;
        }
        g_agc_dev.up.alloc_state[blk] = 0;
    }
}

static void up_viewport_scissor(int x, int y, int w, int h, int lim_w, int lim_h)
{
    evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12),
                                (float)x, (float)y, (float)w, (float)h);
    int l = x < 0 ? 0 : x, t = y < 0 ? 0 : y;
    int r = x + w > lim_w ? lim_w : x + w, b = y + h > lim_h ? lim_h : y + h;
    evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2),
                               (uint32_t)l, (uint32_t)t, (uint32_t)r, (uint32_t)b);
    g_agc_dev.scissor_x = l;
    g_agc_dev.scissor_y = t;
    g_agc_dev.scissor_w = r - l;
    g_agc_dev.scissor_h = b - t;
}

/* Point MRT0 at `t` and the viewport at (x, y, w, h) inside it. A scratch
 * target's registers are built per call into the transient ring, because its
 * size follows the source; the scanout reuses its prebuilt block. */
static int agc_up_bind_target(const agc_up_surface_t *t, int x, int y, int w, int h)
{
    if (t->scanout) {
        evo_agc_writer_set_target(&g_agc_dev.current_cb,
                                  g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer],
                                  16);
        g_agc_dev.current_layer_target = NULL;
        up_viewport_scissor(x, y, w, h, g_agc_dev.width, g_agc_dev.height);
        return 0;
    }
    SceAgcRegister *mrt = alloc_transient_cx(16);
    if (!mrt || !g_agc_dev.agc_defaults ||
        setup_color_target(mrt, g_agc_dev.agc_defaults, (void *)(uintptr_t)t->addr,
                           t->width, t->height, 0) != 0) {
        g_agc_dev.ring_alloc_fail++;
        return -1;
    }
    if (t->fp16) {
        /* CB_COLOR0_INFO: FORMAT = COLOR_16_16_16_16 (12), NUMBER_TYPE = FLOAT
         * (7), ROUND_MODE = 1 and no BLEND_CLAMP, as Mesa programs a float
         * target. setup_color_target wrote the 8_8_8_8 UNORM encoding. */
        mrt[2].value = (mrt[2].value & ~(0x7cu | 0x700u | 0x8000u)) |
                       (12u << 2) | (7u << 8) | 0x40000u;
    }
    evo_agc_writer_set_target(&g_agc_dev.current_cb, mrt, 16);
    g_agc_dev.current_layer_target = &s_up_target_sentinel;
    up_viewport_scissor(x, y, w, h, (int)t->width, (int)t->height);
    return 0;
}

/* One fullscreen pass of an upscale pipe: every one shares the vertex stage
 * of tools/gen_upscale_pipes.py (a vec4 source-UV rect) and reads `n`
 * combined textures from one fragment table. */
static int agc_up_pass(int pipe_id, const agc_up_surface_t *dst,
                       int x, int y, int w, int h, const float uv[4],
                       const agc_up_tex_t *tex, int n)
{
    SceAgcCommandBuffer *cb = &g_agc_dev.current_cb;
    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;
    const uint32_t slot = g_agc_dev.current_slot;

    if (!g_agc_dev.pipelines[pipe_id].valid)
        return -1;
    const evo_agc_user_data_layout_t ud = evo_agc_runtime_get_user_data_layout(pipe_id);
    if (!ud.vs_count || ud.vs_const_table_dword < 0 || ud.ps_texture_table_dword < 0 ||
        ud.vs_count > 16 || ud.ps_count > 16)
        return -1;

    evo_agc_transient_slice_t cons, vsh, desc;
    if (evo_agc_transient_ring_alloc(ring, slot, 16, 16, &cons) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsh) != EVO_AGC_TRANSIENT_OK ||
        evo_agc_transient_ring_alloc(ring, slot, (size_t)n * 48u, 16, &desc) != EVO_AGC_TRANSIENT_OK) {
        g_agc_dev.ring_alloc_fail++;
        return -1;
    }
    memcpy(cons.cpu, uv, 16);
    evo_agc_build_constant_vsharp((uint32_t *)vsh.cpu, cons.gpu_addr, 16);

    uint32_t *d = (uint32_t *)desc.cpu;
    memset(d, 0, (size_t)n * 48u);
    for (int i = 0; i < n; ++i) {
        uint32_t *td = d + 12 * i;
        int rc = evo_agc_build_tsharp_render_target(td, tex[i].addr, tex[i].width,
                                                    tex[i].height, tex[i].fp16);
        if (rc != 0) {
            g_agc_dev.tex_alloc_fail++;
            return -1;
        }
        evo_agc_build_ssharp(td + 8, 1, tex[i].bilinear);
    }

    if (agc_up_bind_target(dst, x, y, w, h) != 0)
        return -1;
    evo_agc_runtime_bind_pipeline(pipe_id);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);

    uint32_t vs_user[16] = {0};
    vs_user[ud.vs_const_table_dword] = (uint32_t)vsh.gpu_addr;
    evo_agc_writer_set_user_data_gs(cb, vs_user, ud.vs_count);
    uint32_t ps_user[16] = {0};
    ps_user[ud.ps_texture_table_dword] = (uint32_t)desc.gpu_addr;
    evo_agc_writer_set_user_data_ps(cb, ps_user, ud.ps_count);

    evo_agc_writer_draw_index_modifier(cb, 6, g_agc_dev.quad_indices,
                                       g_agc_dev.pipelines[pipe_id].draw_modifier);
    /* The next pass samples what this one wrote. */
    if (!dst->scanout)
        evo_agc_flush_color_target();
    return 0;
}

/* Hand the UI back a full-canvas scanout target. */
static void agc_up_restore_scanout(void)
{
    evo_agc_writer_set_target(&g_agc_dev.current_cb,
                              g_agc_dev.gpu_regs->color_targets[g_agc_dev.active_backbuffer], 16);
    g_agc_dev.current_layer_target = NULL;
    up_viewport_scissor(0, 0, g_agc_dev.width, g_agc_dev.height,
                        g_agc_dev.width, g_agc_dev.height);
}

static int up_pipes_valid(int first, int count)
{
    for (int i = 0; i < count; ++i)
        if (!g_agc_dev.pipelines[first + i].valid)
            return 0;
    return 1;
}

static const char *const k_up_mode_name[] = { "Off", "Sharp", "AI" };

/*
 * Decide what this frame gets. `sx`/`sy` are the Off quad's NDC half-extents,
 * so the image covers [W*(1-sx)/2, W*(1+sx)/2] - the plan keeps exactly that
 * footprint. Returns 1 with `pl` filled when the chain should run.
 */
static int agc_upscale_plan(uint32_t src_w, uint32_t src_h, int ten_bit,
                            float sx, float sy, agc_up_plan_t *pl)
{
    int mode = g_agc_dev.up.requested;
    if (mode > g_agc_dev.up.cap)
        mode = g_agc_dev.up.cap;
    const char *reason = NULL;
    int net = 0;

    const float W = (float)g_agc_dev.width, H = (float)g_agc_dev.height;
    const float img_w = sx * W, img_h = sy * H;
    const float ratio = (src_w && src_h)
        ? (img_w / (float)src_w < img_h / (float)src_h ? img_w / (float)src_w
                                                       : img_h / (float)src_h)
        : 0.0f;

    /* Visible part of the image, in whole pixels, and the source UV it shows. */
    const float fx0 = (W - img_w) * 0.5f, fy0 = (H - img_h) * 0.5f;
    int x0 = (int)(fx0 + 0.5f), y0 = (int)(fy0 + 0.5f);
    int x1 = (int)(fx0 + img_w + 0.5f), y1 = (int)(fy0 + img_h + 0.5f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_agc_dev.width) x1 = g_agc_dev.width;
    if (y1 > g_agc_dev.height) y1 = g_agc_dev.height;

    if (mode != EVO_AGC_UPSCALE_OFF) {
        if (ten_bit)
            reason = "HDR source";
        else if (ratio <= 1.05f)
            reason = "source >= output";
        else if (x1 <= x0 || y1 <= y0 || !up_fits(src_w, src_h, 4u) ||
                 !up_fits((uint32_t)(x1 - x0), (uint32_t)(y1 - y0), 4u))
            reason = "size";
    }
    /* AI: Anime4K only engages above 1.2x, and needs its feature maps to fit. */
    if (!reason && mode == EVO_AGC_UPSCALE_AI) {
        /* Auto follows detection; Standard/Large/Maximum are the Settings
         * override, which is how a PS5 Pro the probe cannot identify still
         * gets the big networks. Each steps down one network when it cannot
         * run: UL -> M -> S. */
        net = g_agc_dev.up.net_pref == EVO_AGC_UPNET_MAXIMUM ? 2
            : g_agc_dev.up.net_pref == EVO_AGC_UPNET_LARGE ? 1
            : g_agc_dev.up.net_pref == EVO_AGC_UPNET_STANDARD ? 0
            : evo_hw_is_ps5_pro();
        if (net > g_agc_dev.up.net_cap)
            net = g_agc_dev.up.net_cap;
        if (net == 2 && !(up_pipes_valid(EVO_AGC_PIPE_UP_UL_CONV0, EVO_AGC_UP_UL_CONVS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_UL_ACC0, EVO_AGC_UP_UL_ACCS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_RGB_FINAL, 1) &&
                          agc_upscale_alloc(1) == 0))
            net = 1;
        if (net == 1 && !(up_pipes_valid(EVO_AGC_PIPE_UP_M_CONV0, EVO_AGC_UP_M_CONVS) &&
                          up_pipes_valid(EVO_AGC_PIPE_UP_M_ACC0, EVO_AGC_UP_M_CONVS)))
            net = 0;
        if (ratio < 1.2f || !up_fits(src_w, src_h, 8u) ||
            (net < 2 && !up_pipes_valid(EVO_AGC_PIPE_UP_A4K_FINAL, 1)) ||
            (net == 0 && !up_pipes_valid(EVO_AGC_PIPE_UP_S_CONV0, EVO_AGC_UP_S_CONVS)))
            mode = EVO_AGC_UPSCALE_SHARP;
    }
    if (!reason && mode == EVO_AGC_UPSCALE_SHARP &&
        !(up_pipes_valid(EVO_AGC_PIPE_UP_EASU, 1) && up_pipes_valid(EVO_AGC_PIPE_UP_RCAS, 1)))
        reason = "pipeline missing";
    if (!reason && mode != EVO_AGC_UPSCALE_OFF && agc_upscale_alloc(0) != 0)
        reason = "no memory";

    if (reason || mode == EVO_AGC_UPSCALE_OFF) {
        g_agc_dev.up.label = mode == EVO_AGC_UPSCALE_OFF ? "Off"
            : !strcmp(reason, "HDR source") ? "Off (HDR source)"
            : !strcmp(reason, "source >= output") ? "Off (source >= output)"
            : "Off (unavailable)";
        mode = EVO_AGC_UPSCALE_OFF;
    } else {
        static const char *const k_net_label[] = { "AI (Standard)", "AI (Large)", "AI (Maximum)" };
        g_agc_dev.up.label = mode != EVO_AGC_UPSCALE_AI ? "Sharp" : k_net_label[net];
    }

    /* Log a change of plan once, not per frame. */
    const int key[6] = { g_agc_dev.up.requested, mode, net, (int)src_w, (int)src_h,
                         x1 - x0 };
    if (memcmp(key, g_agc_dev.up.last_key, sizeof(key)) != 0) {
        memcpy(g_agc_dev.up.last_key, key, sizeof(key));
        if (g_agc_dev.up.requested != EVO_AGC_UPSCALE_OFF) {
            if (reason)
                evo_boot_log("agc upscale: bypass requested=%s reason=%s src=%ux%u "
                             "img=%dx%d ten_bit=%d",
                             k_up_mode_name[g_agc_dev.up.requested], reason,
                             src_w, src_h, (int)img_w, (int)img_h, ten_bit);
            else
                evo_boot_log("agc upscale: mode=%s net=%s src=%ux%u -> rect=%d,%d %dx%d "
                             "(image %dx%d, %.2fx) cap=%s",
                             k_up_mode_name[mode],
                             mode != EVO_AGC_UPSCALE_AI ? "fsr1"
                             : net == 2 ? "anime4k-UL" : net ? "anime4k-M" : "anime4k-S",
                             src_w, src_h, x0, y0, x1 - x0, y1 - y0,
                             (int)img_w, (int)img_h, (double)ratio,
                             k_up_mode_name[g_agc_dev.up.cap]);
        }
    }
    if (mode == EVO_AGC_UPSCALE_OFF)
        return 0;

    pl->mode = mode;
    pl->net = net;
    pl->src_w = src_w;
    pl->src_h = src_h;
    pl->x0 = x0; pl->y0 = y0; pl->x1 = x1; pl->y1 = y1;
    pl->uv[0] = ((float)x0 - fx0) / img_w;
    pl->uv[1] = ((float)y0 - fy0) / img_h;
    pl->uv[2] = (float)(x1 - x0) / img_w;
    pl->uv[3] = (float)(y1 - y0) / img_h;
    return 1;
}

/* Anime4K UL: every layer is 3 RGBA16F textures (12 channels), each output
 * texture its own pass reading the whole previous layer. The 1x1 conv that
 * ends the network produces 3 textures - the residual for R, G and B - and is
 * run as accumulate passes for each layer it reads (2..6), per output. */
static int agc_upscale_run_ul(const agc_up_plan_t *pl, const agc_up_tex_t *l0_tex,
                              const agc_up_surface_t *scan)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    enum { W = EVO_AGC_UP_UL_WIDTH };
    const uint32_t sw = pl->src_w, sh = pl->src_h;
    agc_up_surface_t f[2][W], a[2][W];
    for (int s = 0; s < 2; ++s)
        for (int j = 0; j < W; ++j) {
            f[s][j] = up_surface(UP_SLOT_UL_F + s * W + j, sw, sh, 1);
            a[s][j] = up_surface(UP_SLOT_UL_A + s * W + j, sw, sh, 1);
        }

    agc_up_tex_t cur[W + 1], acc[W];
    int have_acc = 0, pass = 0, apass = 0, rc = 0;
    for (int layer = 0; layer < EVO_AGC_UP_UL_LAYERS && rc == 0; ++layer) {
        const int set = layer & 1;
        agc_up_tex_t in[W];
        for (int j = 0; j < W; ++j)
            in[j] = layer ? cur[j] : *l0_tex;
        for (int j = 0; j < W && rc == 0; ++j, ++pass)
            rc = agc_up_pass(EVO_AGC_PIPE_UP_UL_CONV0 + pass, &f[set][j], 0, 0,
                             (int)sw, (int)sh, full, in, layer ? W : 1);
        for (int j = 0; j < W; ++j)
            cur[j] = up_tex(&f[set][j], 0);
        if (layer < EVO_AGC_UP_UL_FED_FIRST)
            continue;
        const int aset = (layer - EVO_AGC_UP_UL_FED_FIRST) & 1;
        for (int j = 0; j < W && rc == 0; ++j, ++apass) {
            cur[W] = have_acc ? acc[j] : cur[0];
            rc = agc_up_pass(EVO_AGC_PIPE_UP_UL_ACC0 + apass, &a[aset][j], 0, 0,
                             (int)sw, (int)sh, full, cur, have_acc ? W + 1 : W);
        }
        for (int j = 0; j < W; ++j)
            acc[j] = up_tex(&a[aset][j], 0);
        have_acc = 1;
    }
    if (rc == 0) {
        const agc_up_tex_t fin[4] = { *l0_tex, acc[0], acc[1], acc[2] };
        rc = agc_up_pass(EVO_AGC_PIPE_UP_RGB_FINAL, scan, pl->x0, pl->y0,
                         pl->x1 - pl->x0, pl->y1 - pl->y0, pl->uv, fin, 4);
    }
    return rc;
}

/* Everything after the YUV pass has filled L0. */
static int agc_upscale_run(const agc_up_plan_t *pl)
{
    static const float full[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    const uint32_t sw = pl->src_w, sh = pl->src_h;
    const int vw = pl->x1 - pl->x0, vh = pl->y1 - pl->y0;

    agc_up_surface_t l0 = up_surface(UP_SLOT_L0, sw, sh, 0);
    agc_up_surface_t scan;
    memset(&scan, 0, sizeof(scan));
    scan.scanout = 1;
    const agc_up_tex_t l0_tex = up_tex(&l0, 1);
    int rc = 0;

    if (pl->mode == EVO_AGC_UPSCALE_SHARP) {
        agc_up_surface_t e = up_surface(UP_SLOT_E, (uint32_t)vw, (uint32_t)vh, 0);
        const agc_up_tex_t e_tex = up_tex(&e, 0);
        rc = agc_up_pass(EVO_AGC_PIPE_UP_EASU, &e, 0, 0, vw, vh, pl->uv, &l0_tex, 1);
        if (rc == 0)
            rc = agc_up_pass(EVO_AGC_PIPE_UP_RCAS, &scan, pl->x0, pl->y0, vw, vh, full, &e_tex, 1);
    } else {
        agc_up_surface_t f[2] = { up_surface(UP_SLOT_F0, sw, sh, 1),
                                  up_surface(UP_SLOT_F0 + 1, sw, sh, 1) };
        agc_up_surface_t a[2] = { up_surface(UP_SLOT_A0, sw, sh, 1),
                                  up_surface(UP_SLOT_A0 + 1, sw, sh, 1) };
        agc_up_tex_t in = l0_tex, residual;
        if (pl->net == 2) {
            rc = agc_upscale_run_ul(pl, &l0_tex, &scan);
            goto done;
        }
        if (!pl->net) {
            for (int i = 0; i < EVO_AGC_UP_S_CONVS && rc == 0; ++i) {
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_S_CONV0 + i, &f[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, &in, 1);
                in = up_tex(&f[i & 1], 0);
            }
            residual = in;
        } else {
            agc_up_tex_t acc = {0};
            for (int i = 0; i < EVO_AGC_UP_M_CONVS && rc == 0; ++i) {
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_M_CONV0 + i, &f[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, &in, 1);
                in = up_tex(&f[i & 1], 0);
                const agc_up_tex_t acc_in[2] = { in, acc };
                rc |= agc_up_pass(EVO_AGC_PIPE_UP_M_ACC0 + i, &a[i & 1], 0, 0,
                                  (int)sw, (int)sh, full, acc_in, i ? 2 : 1);
                acc = up_tex(&a[i & 1], 0);
            }
            residual = acc;
        }
        const agc_up_tex_t fin[2] = { l0_tex, residual };
        if (rc == 0)
            rc |= agc_up_pass(EVO_AGC_PIPE_UP_A4K_FINAL, &scan, pl->x0, pl->y0, vw, vh,
                              pl->uv, fin, 2);
    }

done:
    agc_up_restore_scanout();
    if (rc == 0) {
        g_agc_dev.up.this_frame = 1;
    } else {
        static int s_fail_log = 4;
        if (s_fail_log > 0) {
            s_fail_log--;
            evo_boot_log("agc upscale: chain FAILED mode=%s src=%ux%u (ring_fail=%u tex_fail=%u)",
                         k_up_mode_name[pl->mode], sw, sh,
                         g_agc_dev.ring_alloc_fail, g_agc_dev.tex_alloc_fail);
        }
    }
    return rc ? -1 : 0;
}

/* frame_end hands over the GPU time of every upscaled frame. Two windows in a
 * row over budget (~4 s at 60 fps, so one slow frame cannot trip it) cap the
 * mode one step: AI -> Sharp -> Off, for the rest of the session. */
static void agc_upscale_note_gpu_time(uint64_t us)
{
    g_agc_dev.up.window_us += us;
    if (++g_agc_dev.up.window_frames < EVO_AGC_UP_WINDOW)
        return;
    const uint64_t avg = g_agc_dev.up.window_us / g_agc_dev.up.window_frames;
    evo_boot_log("agc upscale us=%llu n=%u mode=%s budget_us=%u (frame GPU submit->retire, 100 us grain)",
                 (unsigned long long)avg, g_agc_dev.up.window_frames,
                 g_agc_dev.up.label, EVO_AGC_UP_BUDGET_US);
    g_agc_dev.up.window_us = 0;
    g_agc_dev.up.window_frames = 0;

    if (avg <= EVO_AGC_UP_BUDGET_US) {
        g_agc_dev.up.over_budget_windows = 0;
        return;
    }
    if (++g_agc_dev.up.over_budget_windows < 2)
        return;
    g_agc_dev.up.over_budget_windows = 0;
    /* A big network steps down one size before AI gives way to Sharp. */
    if (g_agc_dev.up.last_key[1] == EVO_AGC_UPSCALE_AI && g_agc_dev.up.last_key[2] > 0) {
        static const char *const k_net[] = { "Standard", "Large", "Maximum" };
        const int from_net = g_agc_dev.up.last_key[2];
        g_agc_dev.up.net_cap = from_net - 1;
        g_agc_dev.up.downgrade_notice = EVO_AGC_UPSCALE_AI;
        evo_boot_log("agc upscale: over budget (us=%llu > %u) - AI %s -> AI %s "
                     "for this session", (unsigned long long)avg, EVO_AGC_UP_BUDGET_US,
                     k_net[from_net], k_net[from_net - 1]);
        return;
    }
    const int from = g_agc_dev.up.last_key[1];
    const int to = from > EVO_AGC_UPSCALE_OFF ? from - 1 : EVO_AGC_UPSCALE_OFF;
    g_agc_dev.up.cap = to;
    g_agc_dev.up.downgrade_notice = to;
    evo_boot_log("agc upscale: over budget (us=%llu > %u) - %s -> %s for this session",
                 (unsigned long long)avg, EVO_AGC_UP_BUDGET_US,
                 k_up_mode_name[from], k_up_mode_name[to]);
}

void evo_agc_upscale_set_mode(int mode)
{
    if (mode < EVO_AGC_UPSCALE_OFF || mode > EVO_AGC_UPSCALE_AI)
        mode = EVO_AGC_UPSCALE_OFF;
    if (mode == g_agc_dev.up.requested)
        return;
    /* A fresh choice in Settings is a fresh chance: drop the GPU-time cap. */
    g_agc_dev.up.requested = mode;
    g_agc_dev.up.cap = EVO_AGC_UPSCALE_AI;
    g_agc_dev.up.net_cap = 2;
    g_agc_dev.up.over_budget_windows = 0;
    g_agc_dev.up.window_frames = 0;
    g_agc_dev.up.window_us = 0;
}

void evo_agc_upscale_set_network(int pref)
{
    if (pref < EVO_AGC_UPNET_AUTO || pref > EVO_AGC_UPNET_MAXIMUM)
        pref = EVO_AGC_UPNET_AUTO;
    if (pref == g_agc_dev.up.net_pref)
        return;
    g_agc_dev.up.net_pref = pref;
    g_agc_dev.up.net_cap = 2;
    g_agc_dev.up.over_budget_windows = 0;
    g_agc_dev.up.window_frames = 0;
    g_agc_dev.up.window_us = 0;
}

const char *evo_agc_upscale_label(void)
{
    return g_agc_dev.up.label ? g_agc_dev.up.label : "Off";
}

int evo_agc_upscale_take_downgrade(void)
{
    const int v = g_agc_dev.up.downgrade_notice;
    g_agc_dev.up.downgrade_notice = -1;
    return v;
}

/* The Off quad's NDC half-extents for Fit (0) / Fill (1) / Stretch (2). */
static void agc_video_scale(int disp_w, int disp_h, int view_mode, float *sx, float *sy)
{
    *sx = 1.0f;
    *sy = 1.0f;
    if (view_mode != 2 && disp_w > 0 && disp_h > 0 && g_agc_dev.width > 0 && g_agc_dev.height > 0) {
        float va = (float)disp_w / (float)disp_h;
        float sa = (float)g_agc_dev.width / (float)g_agc_dev.height;
        if (view_mode == 0) { /* FIT (letterbox) */
            if (va > sa) *sy = sa / va; else *sx = va / sa;
        } else {               /* FILL (crop overflow) */
            if (va > sa) *sx = va / sa; else *sy = sa / va;
        }
    }
}

int evo_agc_blit_yuv(const uint8_t *y,  int y_pitch,
                      const uint8_t *uv, int uv_pitch,
                      const uint8_t *u,  int u_pitch,
                      const uint8_t *v,  int v_pitch,
                      int coded_w, int coded_h,
                      int disp_w, int disp_h,
                      int view_mode, int ten_bit, int color_trc,
                      int is_direct, int64_t pts_us)
{
    if (!g_agc_dev.initialized || !y || y_pitch <= 0 || coded_w <= 0 || coded_h <= 0)
        return -1;
    const int planar = (uv == NULL);
    if (planar ? (!u || !v) : (uv == NULL))
        return -1;

    evo_agc_runtime_frame_begin();

    const uint32_t slot = g_agc_dev.current_slot;
    evo_agc_transient_ring_t *ring = &g_agc_dev.transient_ring;

    /* HDR is the transfer, not the bit depth: 8-bit HEVC does carry HLG
     * (a broadcast 4K HLG channel, hardware 2026-09-28). */
    const int hdr_src = (color_trc == 16 || color_trc == 18);
    g_agc_dev.last_video_trc = hdr_src ? color_trc : 1;

    /* 1. Select Pipeline */
    int pipe_id;
    if (ten_bit) {
        if (color_trc == 16)
            pipe_id = EVO_AGC_PIPE_VIDEO_HDR;
        else if (color_trc == 18)
            pipe_id = EVO_AGC_PIPE_VIDEO_HLG;
        else
            pipe_id = EVO_AGC_PIPE_VIDEO_P010_SDR;
    } else if (!planar && hdr_src &&
               g_agc_dev.pipelines[color_trc == 18 ? EVO_AGC_PIPE_NV12_HLG
                                                   : EVO_AGC_PIPE_NV12_HDR].valid) {
        pipe_id = (color_trc == 18) ? EVO_AGC_PIPE_NV12_HLG : EVO_AGC_PIPE_NV12_HDR;
    } else {
        pipe_id = planar ? EVO_AGC_PIPE_VIDEO_PLANAR : EVO_AGC_PIPE_VIDEO_NV12;
    }
    evo_agc_runtime_bind_pipeline(pipe_id);
    evo_agc_runtime_set_blend(EVO_AGC_BLEND_NONE);

    float sx, sy;
    agc_video_scale(disp_w, disp_h, view_mode, &sx, &sy);

    /* #103: with an upscaler engaged this pass renders the picture at source
     * size into scratch surface L0 instead, and agc_upscale_run() below takes
     * it to the scanout. */
    const uint32_t src_w = (disp_w > 0 && disp_w <= coded_w) ? (uint32_t)disp_w : (uint32_t)coded_w;
    const uint32_t src_h = (disp_h > 0 && disp_h <= coded_h) ? (uint32_t)disp_h : (uint32_t)coded_h;
    agc_up_plan_t up_plan;
    /* the upscalers are SDR-only: off for any HDR source, 8-bit ones too */
    const int upscale = agc_upscale_plan(src_w, src_h, ten_bit || hdr_src, sx, sy, &up_plan);

    /* 2. Fullscreen viewport and scissor */
    if (!upscale) {
        evo_agc_writer_set_viewport(&g_agc_dev.current_cb, alloc_transient_cx(12), 0.0f, 0.0f,
                                    (float)g_agc_dev.width, (float)g_agc_dev.height);
        evo_agc_writer_set_scissor(&g_agc_dev.current_cb, alloc_transient_cx(2), 0, 0,
                                   (uint32_t)g_agc_dev.width, (uint32_t)g_agc_dev.height);
        g_agc_dev.scissor_x = 0;
        g_agc_dev.scissor_y = 0;
        g_agc_dev.scissor_w = g_agc_dev.width;
        g_agc_dev.scissor_h = g_agc_dev.height;
    }

    /* 3. Compute VideoConstants (Crop & Aspect Scale) */
    struct {
        float crop[2];
        float scale[2];
    } constants;

    float cx = (disp_w > 0 && disp_w <= coded_w) ? (float)disp_w / (float)coded_w : 1.0f;
    float cy = (disp_h > 0 && disp_h <= coded_h) ? (float)disp_h / (float)coded_h : 1.0f;
    constants.crop[0] = cx;
    constants.crop[1] = cy;

    /* Into L0 the picture fills the viewport exactly. */
    constants.scale[0] = upscale ? 1.0f : sx;
    constants.scale[1] = upscale ? 1.0f : sy;

    /* Allocate VideoConstants in transient ring (64 bytes) */
    evo_agc_transient_slice_t const_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 64, 16, &const_slice) != EVO_AGC_TRANSIENT_OK)
        return -1;
    memcpy(const_slice.cpu, &constants, sizeof(constants));

    /* Build uniform buffer V# descriptor in transient ring (16 bytes) */
    evo_agc_transient_slice_t vsharp_slice;
    if (evo_agc_transient_ring_alloc(ring, slot, 16, 16, &vsharp_slice) != EVO_AGC_TRANSIENT_OK)
        return -1;
    evo_agc_build_constant_vsharp((uint32_t *)vsharp_slice.cpu, const_slice.gpu_addr, sizeof(constants));

    /* VS user SGPR (GS stage: compact register 0x8c).
     *
     * Slots come from the compiled pipeline's PAL metadata, never from
     * constants here - this used to hardcode the psbc layout (const buffer at
     * dword 2 plus an ngg_lds_layout at 3), and LLPC puts the constant table at
     * dword 1 with no ngg_lds entry at all. Writing a pointer to the wrong
     * dword is silent: the shader reads its crop/scale through an unset pointer
     * and the quad lands nowhere. */
    const evo_agc_user_data_layout_t vud =
        evo_agc_runtime_get_user_data_layout(pipe_id);
    if (!vud.vs_count || vud.vs_const_table_dword < 0 ||
        vud.ps_texture_table_dword < 0 || vud.vs_count > 16 || vud.ps_count > 16)
        return -1;

    uint32_t vs_user[16] = {0};
    vs_user[vud.vs_const_table_dword] = (uint32_t)vsharp_slice.gpu_addr;
    evo_agc_writer_set_user_data_gs(&g_agc_dev.current_cb, vs_user, vud.vs_count);

    /* 4. Prepare Texture Descriptors (T#) and Samplers (S#) */
    const uint32_t bpp = ten_bit ? 2u : 1u;
    uint32_t y_pitch_gpu = 0;
    uint64_t y_gpu = 0;
    if (stage_plane(ring, slot, y, y_pitch, coded_w, coded_h, bpp, is_direct, 0, pts_us,
                    &y_pitch_gpu, &y_gpu) != 0) {
        evo_boot_log("agc_blit_yuv: stage Y plane failed");
        evo_boot_log_flush();
        return -1;
    }

    evo_agc_transient_slice_t desc_slice;
    if (planar && !ten_bit) {
        /* Planar 3-plane (SDR): 144 bytes descriptor table (3 * 48B) */
        if (evo_agc_transient_ring_alloc(ring, slot, 144, 16, &desc_slice) != EVO_AGC_TRANSIENT_OK) {
            evo_boot_log("agc_blit_yuv: desc_slice alloc failed");
            evo_boot_log_flush();
            return -1;
        }
        uint32_t *desc = (uint32_t *)desc_slice.cpu;
        memset(desc, 0, 144);

        uint32_t u_pitch_gpu = 0, v_pitch_gpu = 0;
        uint64_t u_gpu = 0, v_gpu = 0;
        uint32_t cw2 = (uint32_t)(coded_w / 2);
        uint32_t ch2 = (uint32_t)(coded_h / 2);
        if (stage_plane(ring, slot, u, u_pitch, cw2, ch2, 1u, is_direct, 1, pts_us,
                        &u_pitch_gpu, &u_gpu) != 0) {
            evo_boot_log("agc_blit_yuv: stage U plane failed");
            evo_boot_log_flush();
            return -1;
        }
        if (stage_plane(ring, slot, v, v_pitch, cw2, ch2, 1u, is_direct, 2, pts_us,
                        &v_pitch_gpu, &v_gpu) != 0) {
            evo_boot_log("agc_blit_yuv: stage V plane failed");
            evo_boot_log_flush();
            return -1;
        }

        /* Binding 0: Y plane */
        int r0 = evo_agc_build_tsharp_r8(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu);
        evo_agc_build_ssharp(desc + 8, 1, 1);

        /* Binding 1: U plane (offset 48 = 12 dwords) */
        int r1 = evo_agc_build_tsharp_r8(desc + 12, u_gpu, cw2, ch2, u_pitch_gpu);
        evo_agc_build_ssharp(desc + 20, 1, 1);

        /* Binding 2: V plane (offset 96 = 24 dwords) */
        int r2 = evo_agc_build_tsharp_r8(desc + 24, v_gpu, cw2, ch2, v_pitch_gpu);
        evo_agc_build_ssharp(desc + 32, 1, 1);

        if (r0 != 0 || r1 != 0 || r2 != 0) {
            evo_boot_log("agc_blit_yuv: build tsharp failed rc=%d/%d/%d", r0, r1, r2);
            evo_boot_log_flush();
            return -1;
        }
    } else {
        /* NV12 / P010 2-plane: 96 bytes descriptor table (2 * 48B).
         * Used for NV12 (SDR 8-bit), NV12_10 (HDR 10-bit), and planar 10-bit (interleaved to RG16). */
        if (evo_agc_transient_ring_alloc(ring, slot, 96, 16, &desc_slice) != EVO_AGC_TRANSIENT_OK) {
            evo_boot_log("agc_blit_yuv: 2-plane desc_slice alloc failed");
            evo_boot_log_flush();
            return -1;
        }
        uint32_t *desc = (uint32_t *)desc_slice.cpu;
        memset(desc, 0, 96);

        uint32_t uv_pitch_gpu = 0;
        uint64_t uv_gpu = 0;
        uint32_t cw2 = (uint32_t)(coded_w / 2);
        uint32_t ch2 = (uint32_t)(coded_h / 2);

        if (ten_bit && planar) {
            if (stage_planar_uv_to_rg16(ring, slot, u, u_pitch, v, v_pitch, cw2, ch2, &uv_pitch_gpu, &uv_gpu) != 0) {
                evo_boot_log("agc_blit_yuv: stage planar UV to RG16 failed");
                evo_boot_log_flush();
                return -1;
            }
        } else {
            uint32_t uv_bpp = bpp * 2u;
            if (stage_plane(ring, slot, uv, uv_pitch, cw2, ch2, uv_bpp, is_direct, 1, pts_us,
                            &uv_pitch_gpu, &uv_gpu) != 0) {
                evo_boot_log("agc_blit_yuv: stage UV plane failed");
                evo_boot_log_flush();
                return -1;
            }
        }

        /* Binding 0: Y plane */
        int r0 = ten_bit ? evo_agc_build_tsharp_r16(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu)
                         : evo_agc_build_tsharp_r8(desc + 0, y_gpu, (uint32_t)coded_w, (uint32_t)coded_h, y_pitch_gpu);
        evo_agc_build_ssharp(desc + 8, 1, 1);

        /* Binding 1: UV plane (offset 48 = 12 dwords) */
        int r1 = ten_bit ? evo_agc_build_tsharp_rg16(desc + 12, uv_gpu, cw2, ch2, uv_pitch_gpu)
                         : evo_agc_build_tsharp_rg8(desc + 12, uv_gpu, cw2, ch2, uv_pitch_gpu);
        evo_agc_build_ssharp(desc + 20, 1, 1);

        if (r0 != 0 || r1 != 0) {
            evo_boot_log("agc_blit_yuv: build 2-plane tsharp failed rc=%d/%d", r0, r1);
            evo_boot_log_flush();
            return -1;
        }
    }

    /* PS user SGPR (PS stage: compact register 0x0c) - same derivation. */
    uint32_t ps_user[16] = {0};
    ps_user[vud.ps_texture_table_dword] = (uint32_t)desc_slice.gpu_addr;
    evo_agc_writer_set_user_data_ps(&g_agc_dev.current_cb, ps_user, vud.ps_count);

    /* #103: switch to L0 only now, after every early return above - one of
     * those leaving MRT0 on a scratch surface would send the UI there too. */
    if (upscale) {
        agc_up_surface_t l0 = up_surface(UP_SLOT_L0, src_w, src_h, 0);
        if (agc_up_bind_target(&l0, 0, 0, (int)src_w, (int)src_h) != 0) {
            agc_up_restore_scanout();
            return -1;
        }
    }

    /* 5. Dispatch hardware quad draw call with pipeline draw modifier */
    evo_agc_writer_draw_index_modifier(&g_agc_dev.current_cb, 6, g_agc_dev.quad_indices,
                                      g_agc_dev.pipelines[pipe_id].draw_modifier);
    /* Without this the frame carries no recorded draw and frame_end discards it
     * instead of presenting - video would decode and never reach the panel. */
    evo_agc_runtime_note_draw();

    if (upscale) {
        evo_agc_flush_color_target();   /* the chain samples L0 */
        if (agc_upscale_run(&up_plan) != 0)
            return -1;
    }

    /* This buffer now holds this frame; the render loop stops redrawing the
     * quad until the buffer it is about to draw into holds something else. */
    evo_agc_runtime_note_video_pts(pts_us);
    return 0;
}
