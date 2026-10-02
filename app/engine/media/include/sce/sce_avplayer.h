/*
 * sce_avplayer.h — libSceAvPlayer ABI (hardware demux + decode + A/V sync).
 *
 * Native-decode plan Phase 0 (docs/evo-pro/native-decode-plan.md §4), Route A.
 * Nothing here is linked at build time: the module is loaded and every symbol
 * resolved by NID at run time (nid_encode + kernel_dynlib_resolve in a payload;
 * sceKernelLoadStartModule + sceKernelDlsym-by-NID in the app module). Module
 * absent => feature absent.
 *
 * SOURCE OF TRUTH
 * ---------------
 * Struct layouts, field offsets and the call sequence are transcribed from
 * SvenGDK/SharpProspero — a clean-room C# SDK for on-device app modules:
 *   third_party/SharpProspero/src/SharpProspero/Interop/Media/AvPlayer.cs
 *   third_party/SharpProspero/src/SharpProspero/Media/MediaPlayer.cs
 * Every field there carries its byte offset in a doc-comment; this header keeps
 * those offsets in comments so a mismatch is caught by eye. The six core NIDs
 * are additionally pinned against a hardware run — see the NID table below and
 * docs/native-media-research.md#results-log (elfldr payload, fw 12.70,
 * 2026-08-09: libSceAvPlayer maps and all six resolve).
 *
 * The C# `delegate* unmanaged<...>` callback signatures are reproduced as C
 * function-pointer typedefs verbatim. SharpProspero's MediaPlayer.cs is the
 * reference implementation of the allocator + frame-buffer ("texture")
 * callbacks — see docs/evo-pro/avplayer-abi.md for the port and the
 * memory-typing diff against docs/hardware-decode.md.
 *
 * WHAT IS *NOT* VERIFIED
 * ----------------------
 * That sceAvPlayerInit / AddSource / GetVideoDataEx actually succeed and
 * produce a frame from EVO's process context. That is exactly the Phase 2
 * spike (projects/avplayer_test). The prior hardware note only proves the
 * symbols resolve, not that the pipeline runs.
 */
#ifndef EVO_SCE_AVPLAYER_H
#define EVO_SCE_AVPLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- sysmodule id ------------------------------------------------------- */
/* sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER) — must be called before
 * sceAvPlayerInit (SharpProspero's prospero-media sample does exactly this:
 * `SystemModule.Load(SystemModuleId.AvPlayer)`). A link-time NEEDED entry only
 * maps the .prx; this runs its module_start. Value from SharpProspero
 * Interop/Sysmodule/SystemModuleId.cs. */
#define SCE_SYSMODULE_AVPLAYER 0x00A5 /* 165 */

/* --- debug level (SceAvPlayerDebugLevel) ------------------------------- */
#define SCE_AVPLAYER_DEBUG_NONE     0u
#define SCE_AVPLAYER_DEBUG_INFO     1u   /* errors only                     */
#define SCE_AVPLAYER_DEBUG_WARNINGS 2u   /* errors + warnings               */
#define SCE_AVPLAYER_DEBUG_ALL      3u   /* everything — use this for the spike */

/* --- stream type (SceAvPlayerStreamType) ------------------------------- */
#define SCE_AVPLAYER_STREAM_UNKNOWN   0u
#define SCE_AVPLAYER_STREAM_VIDEO     1u
#define SCE_AVPLAYER_STREAM_AUDIO     2u
#define SCE_AVPLAYER_STREAM_TIMEDTEXT 3u

/* Thread priority: 0 => default 700, else clamp to [637, 764]. */
#define SCE_AVPLAYER_DEFAULT_PRIORITY 700u

/* --- callback signatures --------------------------------------------------
 * The player owns no allocator; all four memory callbacks are mandatory.
 * They are invoked from the player's own threads — they must not touch any
 * state those threads cannot reach, and must never let an exception/longjmp
 * escape. Return NULL to fail an allocation (the player handles it); do not
 * abort. */
typedef void *(*SceAvPlayerAllocate)(void *arg, uint32_t alignment, uint32_t size);
typedef void  (*SceAvPlayerDeallocate)(void *arg, void *ptr);
typedef void *(*SceAvPlayerAllocateTexture)(void *arg, uint32_t alignment, uint32_t size);
typedef void  (*SceAvPlayerDeallocateTexture)(void *arg, void *ptr);

/* File-replacement callbacks. Optional as a block (leave the whole struct zero
 * to let the player open the path itself), but if any is set they all must be.
 * NOTE the layout: object pointer FIRST, and a single read entry point that
 * takes an absolute offset — there is no separate sequential read(). This is
 * the PS5 shape and differs from the older PS4 SceAvPlayer (which had both
 * read + readOffset and a memSize field). */
typedef int      (*SceAvPlayerOpenFile)(void *arg, const char *path);        /* 0 on success            */
typedef int      (*SceAvPlayerCloseFile)(void *arg);                          /* 0 on success            */
typedef int      (*SceAvPlayerReadOffsetFile)(void *arg, uint8_t *buf,
                                              uint64_t position, uint32_t length); /* bytes read         */
typedef uint64_t (*SceAvPlayerSizeFile)(void *arg);                           /* file length in bytes    */

/* Event callback. Optional — leave zero and poll instead. */
typedef void (*SceAvPlayerEventCallback)(void *arg, int32_t event_id,
                                         int32_t source_id, void *event_data);

/* --- SceAvPlayerMemAllocator (Size = 40) ------------------------------- */
typedef struct SceAvPlayerMemAllocator {
    void                       *object_ptr;         /* 0x00 — passed back to each cb */
    SceAvPlayerAllocate          allocate;          /* 0x08 */
    SceAvPlayerDeallocate        deallocate;        /* 0x10 */
    SceAvPlayerAllocateTexture   allocate_texture;  /* 0x18 */
    SceAvPlayerDeallocateTexture deallocate_texture;/* 0x20 */
} SceAvPlayerMemAllocator;

/* --- SceAvPlayerFileReplacement (Size = 40) --------------------------- */
typedef struct SceAvPlayerFileReplacement {
    void                     *object_ptr;   /* 0x00 */
    SceAvPlayerOpenFile        open;         /* 0x08 */
    SceAvPlayerCloseFile       close;        /* 0x10 */
    SceAvPlayerReadOffsetFile  read_offset;  /* 0x18 */
    SceAvPlayerSizeFile        size;         /* 0x20 */
} SceAvPlayerFileReplacement;

/* --- SceAvPlayerEventReplacement (Size = 16) ------------------------- */
typedef struct SceAvPlayerEventReplacement {
    void                    *object_ptr;    /* 0x00 */
    SceAvPlayerEventCallback  event_callback;/* 0x08 */
} SceAvPlayerEventReplacement;

/* --- SceAvPlayerInitData (Size = 120) --------------------------------- */
typedef struct SceAvPlayerInitData {
    SceAvPlayerMemAllocator     memory_replacement;   /* 0x00..0x27 */
    SceAvPlayerFileReplacement  file_replacement;     /* 0x28..0x4F */
    SceAvPlayerEventReplacement event_replacement;    /* 0x50..0x5F */
    uint32_t                    debug_level;          /* 0x60 — SCE_AVPLAYER_DEBUG_* */
    uint32_t                    base_priority;        /* 0x64 — 0 => 700           */
    int32_t                     num_output_video_framebuffers; /* 0x68 — 2..16, else 2 */
    uint8_t                     auto_start;           /* 0x6C */
    uint8_t                     reserved0;            /* 0x6D */
    uint8_t                     reserved1;            /* 0x6E */
    uint8_t                     reserved2;            /* 0x6F */
    const char                 *default_language;     /* 0x70 — optional (e.g. "eng") */
} SceAvPlayerInitData;

/* --- stream-info structs -------------------------------------------------- */
typedef struct SceAvPlayerAudioDetails {   /* Size = 16 */
    uint16_t channel_count;   /* 0x00 */
    uint16_t reserved;        /* 0x02 */
    uint32_t sample_rate;     /* 0x04 */
    uint32_t size;            /* 0x08 — payload bytes                       */
    uint32_t language_code;   /* 0x0C */
} SceAvPlayerAudioDetails;

typedef struct SceAvPlayerVideoDetails {   /* Size = 16 */
    uint32_t width;           /* 0x00 */
    uint32_t height;          /* 0x04 */
    float    aspect_ratio;    /* 0x08 */
    uint32_t language_code;   /* 0x0C */
} SceAvPlayerVideoDetails;

typedef union SceAvPlayerStreamDetails {   /* Size = 16 */
    SceAvPlayerAudioDetails audio;
    SceAvPlayerVideoDetails video;
    uint8_t                 raw[16];
} SceAvPlayerStreamDetails;

typedef struct SceAvPlayerStreamInfo {     /* Size = 32 */
    uint32_t                 type;         /* 0x00 — SCE_AVPLAYER_STREAM_*   */
    uint32_t                 reserved;     /* 0x04 */
    SceAvPlayerStreamDetails details;      /* 0x08 */
    uint64_t                 duration_ms;  /* 0x18 */
} SceAvPlayerStreamInfo;

/* --- SceAvPlayerFrameInfo (Size = 40) — the basic (audio) form -------- */
typedef struct SceAvPlayerFrameInfo {
    uint8_t                 *data;         /* 0x00 */
    uint32_t                 reserved;     /* 0x08 */
    uint32_t                 padding;      /* 0x0C */
    uint64_t                 timestamp_ms; /* 0x10 */
    SceAvPlayerStreamDetails details;      /* 0x18 */
} SceAvPlayerFrameInfo;

/* --- SceAvPlayerFrameInfoEx (Size = 104) — the extended (video) form ---
 * Explicit-layout union in C#; only the fields the SDK actually reads are
 * named. The gaps (0x08..0x0F, 0x20..0x23, 0x24 named fields aside, 0x40 gap)
 * are the rest of an 80-byte details union — reserved, meaning unverified.
 *
 * CROP SEMANTICS (from MediaPlayer.cs VideoFrame remarks — this is the detail
 * a naive blit gets wrong):
 *   - the buffer is wider & taller than the picture.
 *   - crop_left / crop_right are measured FROM THE PITCH, not the width, so
 *     the pitch padding is counted inside crop_right.
 *   - visible_width  = pitch  - crop_left - crop_right
 *   - visible_height = height - crop_top  - crop_bottom
 *   - luma   plane starts at data
 *   - chroma plane starts at data + (int64)pitch * height   (buffer height,
 *     NOT visible height), then both planes are indexed from the picture
 *     corner (crop_top row, crop_left column). */
typedef struct SceAvPlayerFrameInfoEx {
    void    *data;          /* 0x00 — NV12 planes (luma then interleaved UV) */
    uint8_t  _pad08[8];     /* 0x08 */
    uint64_t timestamp_ms;  /* 0x10 */
    uint32_t video_width;   /* 0x18 — details.video.width                    */
    uint32_t video_height;  /* 0x1C — details.video.height (buffer height)   */
    uint8_t  _pad20[12];    /* 0x20..0x2B */
    uint32_t crop_left;     /* 0x2C */
    uint32_t crop_right;    /* 0x30 — counted from pitch (includes padding)  */
    uint32_t crop_top;      /* 0x34 */
    uint32_t crop_bottom;   /* 0x38 */
    uint32_t video_pitch;   /* 0x3C — luma & chroma row pitch in BYTES       */
    uint8_t  _pad40[40];    /* 0x40..0x67 — remainder of the details union   */
} SceAvPlayerFrameInfoEx;                                /* sizeof == 104 (0x68) */

/* --- functions (resolve by NID at run time) ---------------------------
 * NID table — names verified against SharpProspero's LibraryImport bindings;
 * the six marked (hw) additionally match docs/native-media-research.md's
 * on-device resolve log (fw 12.70, elfldr payload).
 *
 *   sceAvPlayerInit             aS66RI0gGgo   (hw)
 *   sceAvPlayerAddSource        KMcEa+rHsIo   (hw)
 *   sceAvPlayerGetVideoData     o3+RWnHViSg   (hw)   — basic form
 *   sceAvPlayerGetAudioData     Wnp1OVcrZgk   (hw)
 *   sceAvPlayerIsActive         UbQoYawOsfY   (hw)
 *   sceAvPlayerClose            NkJwDzKmIlw   (hw)
 *   sceAvPlayerGetVideoDataEx   — resolve at run time (nid_encode); the Ex
 *                                 form is what carries pitch + crop.
 *   sceAvPlayerStart / Stop / Pause / Resume / SetLooping / CurrentTime /
 *   JumpToTime / StreamCount / GetStreamInfo / EnableStream — resolve by name
 *   via nid_encode(); do not hardcode.
 *
 * Do NOT paste NID literals into calling code — compute them (nid_encode) so a
 * typo can't silently resolve the wrong symbol. */

void    *sceAvPlayerInit(const SceAvPlayerInitData *init_data);           /* handle, or NULL */
int32_t  sceAvPlayerAddSource(void *handle, const char *path);
int32_t  sceAvPlayerStart(void *handle);
int32_t  sceAvPlayerStop(void *handle);
int32_t  sceAvPlayerPause(void *handle);
int32_t  sceAvPlayerResume(void *handle);
int32_t  sceAvPlayerSetLooping(void *handle, int32_t loop);
int32_t  sceAvPlayerIsActive(void *handle);                              /* bool (U1)       */
uint64_t sceAvPlayerCurrentTime(void *handle);                           /* ms              */
int32_t  sceAvPlayerJumpToTime(void *handle, uint64_t ms);
int32_t  sceAvPlayerStreamCount(void *handle);                           /* 0 until source read */
int32_t  sceAvPlayerGetStreamInfo(void *handle, uint32_t stream_id,
                                  SceAvPlayerStreamInfo *info);
int32_t  sceAvPlayerEnableStream(void *handle, uint32_t stream_id);
int32_t  sceAvPlayerGetAudioData(void *handle, SceAvPlayerFrameInfo *frame);   /* bool */
int32_t  sceAvPlayerGetVideoData(void *handle, SceAvPlayerFrameInfo *frame);   /* bool, basic */
int32_t  sceAvPlayerGetVideoDataEx(void *handle, SceAvPlayerFrameInfoEx *frame); /* bool, w/ pitch+crop */
int32_t  sceAvPlayerClose(void *handle);

/* --- memory-typing crib (from MediaPlayer.cs AllocateTexture / AllocateGeneral) ----
 * general  (allocate / deallocate)      : plain aligned heap — posix_memalign
 *                                         with alignment rounded up to a power
 *                                         of two (>=1). Player threads only.
 * texture  (allocate_texture / dealloc) : GPU-visible DIRECT memory. Per call:
 *     align = max(alignment, 0x4000) rounded to pow2
 *     bytes = round_up(size, align)
 *     sceKernelAllocateDirectMemory(0, GetDirectMemorySize(), bytes, align,
 *                                   12 [MemoryTypeCachedShared], &off)
 *     sceKernelMapDirectMemory(&addr, bytes, 0x33 [CPU rw | GPU all],
 *                              0, off, align)
 *   deallocate_texture MUST munmap AND releaseDirectMemory (release alone
 *   leaks the address range and a player that recycles buffers runs the VA
 *   space out). Track (addr -> off, size) in a small fixed table the player
 *   threads can read without locking the general heap.
 *
 * This is the exact detail the earlier docs/hardware-decode.md try may have
 * gotten wrong (it used WC_GARLIC type 3). See docs/evo-pro/avplayer-abi.md. */

#ifdef __cplusplus
}
#endif

#endif /* EVO_SCE_AVPLAYER_H */
