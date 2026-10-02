/*
 * sce_videodec2.h — libSceVideodec2 ABI (hardware H.264/HEVC/VP9 decode).
 *
 * Native-decode plan Phase 0 (docs/evo-pro/native-decode-plan.md §4).
 * Nothing here is linked at build time: the module is loaded and every symbol
 * resolved by NID at run time, module absent => feature absent.
 *
 * SOURCE OF TRUTH
 * ---------------
 * These struct layouts and the call sequence in docs/evo-pro/videodec2-abi.md were
 * lifted verbatim from blackbearreloaded/ProsperoLight
 * (src/moonlight_stream.cpp), a real Moonlight/PS5 streaming client that
 * decodes on the VCN block in production. On 2026-09-01 that exact sequence
 * was run on hardware from a fake-signed game-category app module (the
 * PROSPEROLIGHT_VDEC_SELF_TEST build) and every call returned 0, producing a
 * valid 1920x1088 NV12 H.264 frame. See [[native-decode-app-slot-plan]] and
 * the status block of docs/evo-pro/native-decode-plan.md.
 *
 * The errno 5200 wall recorded in docs/hardware-decode.md was a process-context
 * limit (elfldr payload / hbldr borrowed slot), NOT a driver or signing limit.
 * EVO Player reaches this path only once it ships as a registered app module.
 *
 * Field names that ProsperoLight left as `reservedN` / `padding` are kept as
 * such — do not assume they are unused, only that their meaning is unverified.
 */
#ifndef EVO_SCE_VIDEODEC2_H
#define EVO_SCE_VIDEODEC2_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- sysmodule id -------------------------------------------------------- */
/* sceSysmoduleLoadModule(SCE_SYSMODULE_VIDEODEC2) — the game-process module.
 * ProsperoLight loads it by the bare number; this is that number. */
#define SCE_SYSMODULE_VIDEODEC2 0x00CF /* 207 */

/* --- codec_type (SceVideodec2Codec) ------------------------------------- */
/* Not sequential — these are the service's own tag values. */
#define SCE_VIDEODEC2_CODEC_AVC   1u          /* H.264                       */
#define SCE_VIDEODEC2_CODEC_HEVC  974921u     /* 0x000EE049                  */
/* 2382845 == 0x00245BFD. The comment here used to read 0x0024603D, which is a
 * different number (2384061) - a transcription slip, not a second candidate.
 * The decimal is what the code has always passed and it matches SharpProspero's
 * Videodec2Codec.Vp9; hardware confirms it: sceVideodec2CreateDecoder accepts it
 * (klog 2026-09-10, VP9 1920x1088 resident slot up, every stage rc=0). */
#define SCE_VIDEODEC2_CODEC_VP9   2382845u    /* 0x00245BFD                  */

/* profile: the ITU/ISO profile_idc for the codec.
 *   AVC : 66 Baseline, 77 Main, 100 High         (ProsperoLight uses 100)
 *   HEVC: 1 Main, 2 Main10                       (ProsperoLight uses 1 / 2)
 * max_level: ITU level x10 for AVC (51 = 5.1), HEVC general_level_idc-style
 * value ProsperoLight passes as 123/150/153 for 1080/1440/2160. */

/* resource_type (SceVideodec2ResourceType): compute-queue path = 1. */
#define SCE_VIDEODEC2_RESOURCE_COMPUTE 1u

/* --- structs ----------------------------------------------------------- */
/* Every struct starts with `size = sizeof(struct)` and the service rejects a
 * wrong size. Zero the whole struct first, then set `size`. */

typedef struct SceVideodec2DecoderConfigInfo {
    uint64_t size;
    uint32_t resource_type;        /* SCE_VIDEODEC2_RESOURCE_COMPUTE          */
    uint32_t codec_type;           /* SCE_VIDEODEC2_CODEC_*                   */
    uint32_t profile;              /* profile_idc (see above)                */
    uint32_t max_level;            /* see above                              */
    int32_t  max_width;            /* e.g. 1920 (1080p), 3840 (2160p)        */
    int32_t  max_height;           /* e.g. 1088 (1080p padded), 2176 (2160p) */
    int32_t  max_dpb_frames;       /* ProsperoLight: 4                       */
    uint32_t pipeline_depth;       /* ProsperoLight: 1                       */
    uint64_t compute_queue;        /* (uint64_t) handle from AllocateComputeQueue */
    uint64_t cpu_affinity;         /* ProsperoLight: 0x3F (cores 0-5)        */
    int32_t  cpu_priority;         /* ProsperoLight: 700                     */
    uint32_t optimize_progressive; /* ProsperoLight: 1                       */
    uint32_t check_memory_type;    /* ProsperoLight: 0                       */
    uint32_t reserved;
} SceVideodec2DecoderConfigInfo;

typedef struct SceVideodec2DecoderMemoryInfo {
    uint64_t size;
    uint64_t cpu_size;             /* out: map with MapNamedFlexibleMemory prot 0x03 */
    void    *cpu;                  /* in/out: the mapped CPU workspace pointer       */
    uint64_t gpu_size;             /* out: alloc direct, MapDirectMemory prot 0x32   */
    void    *gpu;
    uint64_t cpu_gpu_size;         /* out: alloc direct, MapDirectMemory prot 0x33   */
    void    *cpu_gpu;
    uint64_t max_frame_size;       /* out: bytes for ONE decoded frame buffer        */
    uint32_t frame_alignment;      /* out                                            */
    uint32_t reserved;
} SceVideodec2DecoderMemoryInfo;

typedef struct SceVideodec2ComputeConfigInfo {
    uint64_t size;
    uint16_t pipe_id;              /* ProsperoLight: 0 */
    uint16_t queue_id;             /* ProsperoLight: 0 */
    uint8_t  check_memory_type;    /* ProsperoLight: 0 */
    uint8_t  reserved0;
    uint16_t reserved1;
} SceVideodec2ComputeConfigInfo;

typedef struct SceVideodec2ComputeMemoryInfo {
    uint64_t size;
    uint64_t cpu_gpu_size;         /* out from Query; caller 16K-aligns and re-stores
                                      the aligned value before AllocateComputeQueue */
    void    *cpu_gpu;              /* in: direct memory mapped prot 0x33 */
} SceVideodec2ComputeMemoryInfo;

/* Only used by sceVideodec2MapDirectMemory (an alternative to the split
 * gpu/cpu_gpu maps above). ProsperoLight does NOT use it — it maps the two
 * regions itself and passes them in DecoderMemoryInfo. Kept for completeness. */
typedef struct SceVideodec2DirectMemory {
    uint64_t size;
    uint64_t allocation_size;
    void    *address;
    int64_t  direct_start;
} SceVideodec2DirectMemory;

typedef struct SceVideodec2InputData {
    uint64_t size;
    void    *au;                   /* one Annex-B access unit (SPS+PPS+slice...) */
    uint64_t au_size;
    uint64_t pts;                  /* presentation ts, arbitrary units          */
    uint64_t dts;                  /* ProsperoLight: UINT64_MAX ("unknown")     */
    uint64_t attached;             /* ProsperoLight: 0                          */
} SceVideodec2InputData;

typedef struct SceVideodec2FrameBuffer {
    uint64_t size;
    void    *buffer;               /* in: one slot of the frame pool (max_frame_size) */
    uint64_t buffer_size;          /* in: that slot's size                            */
    uint32_t accepted;             /* out: non-zero once the decoder took the slot    */
    uint32_t reserved;
} SceVideodec2FrameBuffer;

typedef struct SceVideodec2OutputInfo {
    uint64_t size;
    uint8_t  valid;                /* out: 1 => `buffer` holds a decoded picture */
    uint8_t  error;                /* out: 0 => no error                        */
    uint8_t  picture_count;        /* out: 1 for a normal frame                 */
    uint8_t  padding;
    uint32_t codec;                /* out: echoes codec_type                    */
    uint32_t width;                /* out: luma width  (e.g. 1920)             */
    uint32_t pitch;                /* out: luma stride in SAMPLES (e.g. 2048)  */
    uint32_t height;               /* out: luma height (e.g. 1088)            */
    uint32_t reserved;
    void    *buffer;               /* out: NV12 (or P010 for Main10) planes;
                                      points inside the FrameBuffer slot        */
    uint64_t buffer_size;          /* out                                       */
    uint32_t frame_format;         /* out                                       */
    uint32_t pitch_bytes;          /* out: pitch in bytes (== pitch*2 for P010) */
} SceVideodec2OutputInfo;

/* --- functions (resolve by NID at run time) --------------------------- */

int32_t sceVideodec2QueryComputeMemoryInfo(SceVideodec2ComputeMemoryInfo *memory);

int32_t sceVideodec2AllocateComputeQueue(const SceVideodec2ComputeConfigInfo *config,
                                         const SceVideodec2ComputeMemoryInfo *memory,
                                         void **compute_queue);
int32_t sceVideodec2ReleaseComputeQueue(void *compute_queue);

int32_t sceVideodec2QueryDecoderMemoryInfo(const SceVideodec2DecoderConfigInfo *config,
                                           SceVideodec2DecoderMemoryInfo *memory);

int32_t sceVideodec2CreateDecoder(const SceVideodec2DecoderConfigInfo *config,
                                  const SceVideodec2DecoderMemoryInfo *memory,
                                  void **decoder);
int32_t sceVideodec2DeleteDecoder(void *decoder);

int32_t sceVideodec2MapDirectMemory(void *decoder, const SceVideodec2DirectMemory *memory);

int32_t sceVideodec2Reset(void *decoder);

/* Submit one AU. output->valid==0 with rc==0 means "buffered, call Flush". */
int32_t sceVideodec2Decode(void *decoder,
                           SceVideodec2InputData *input,
                           SceVideodec2FrameBuffer *frame,
                           SceVideodec2OutputInfo *output);

/* Drain a buffered picture (and at end-of-stream). */
int32_t sceVideodec2Flush(void *decoder,
                          SceVideodec2FrameBuffer *frame,
                          SceVideodec2OutputInfo *output);

/* --- memory-typing crib (from ProsperoLight allocate_direct + maps) ----
 * sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, &start)  [type 12]
 * then sceKernelMapDirectMemory(&ptr, size, PROT, 0, start, 0x4000):
 *   PROT 0x33  -> CPU r/w + GPU all  : compute cpu_gpu, decoder cpu_gpu, AU pool
 *   PROT 0x32  -> CPU w   + GPU all  : decoder gpu, frame (output) pool
 * decoder cpu_size  -> sceKernelMapNamedFlexibleMemory(&cpu, sz, 0x03, 0, name)
 * All sizes 16K-aligned ((v + 0x3FFF) & ~0x3FFF) before allocation.
 * ProsperoLight pools AU and frame buffers x3 (PIPELINE_BUFFER_COUNT). */

#ifdef __cplusplus
}
#endif

#endif /* EVO_SCE_VIDEODEC2_H */
