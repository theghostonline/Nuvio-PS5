/*
 * evo_vdec.h — video decoder interface. One compressed access unit in,
 * one pp_frame out. FFmpeg is one implementation (evo_vdec_ffmpeg.c); a
 * native Sony-module backend slots in beside it later (native-decode plan
 * Phase 4). This header is the seam from that plan's §3.
 *
 * The FFmpeg implementation is PURE: no clocks, no sleeps, no pp_playback,
 * no globals. The play loop (evo_playback.c / main.c) owns pacing and present.
 *
 * Track A step A6 of docs/modularisation-plan.md.
 *
 * PHASE 4 SLOT-IN (native decode)
 * ------------------------------
 * The native backend is a second .c file — evo_vdec_native.c — that
 * implements exactly this header against sceVideodec2 (see
 * docs/evo-pro/videodec2-abi.md). Nothing in main.c / evo_playback.c /
 * evo_demux.c changes: they already speak only evo_vdec_open / _send /
 * _receive / _flush / _close. evo_vdec_open() picks the backend from
 * p->backend and reports what it actually built through *chosen; the ffmpeg
 * path always downgrades to FFMPEG. Guard evo_vdec_native.c behind the SDK
 * macro so the host build keeps linking only evo_vdec_ffmpeg.c.
 *
 * Scope note: the play stream and, since the EVO_TEST_hevc8_4k.mp4 crash, the
 * cover/poster extractor (core/src/services/CoverArtService.cpp) go through
 * this seam. FFmpeg's software HEVC decoder faults rather than fails when a
 * picture buffer cannot be allocated, and it does so on a file the hardware
 * decoder plays without complaint - so the poster path asks for the hardware
 * decoder first and keeps its own avcodec path as the fallback. It still
 * demuxes and scales itself; this interface deliberately does neither.
 * The scrub-preview worker (media/src/prospero_thumbnail.c) stays purely on
 * avcodec: it runs during playback, when the resident decoder is claimed.
 */
#ifndef EVO_VDEC_H
#define EVO_VDEC_H

#include <stdint.h>

#include "pp_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_vdec evo_vdec;

typedef enum {
    EVO_VDEC_BACKEND_FFMPEG = 0,   /* always available */
    EVO_VDEC_BACKEND_NATIVE = 1    /* only if probe succeeded */
} evo_vdec_backend;

/* Per-field sentinel for the tuning knobs below: "leave the codec default". */
#define EVO_VDEC_KEEP (-32768)

typedef struct {
    evo_vdec_backend backend;      /* requested; may be downgraded          */
    int   codec_id;                /* AVCodecID from the demuxer            */
    int   width, height;
    const uint8_t *extradata; int extradata_size;   /* SPS/PPS etc.         */
    void *avctx_params;            /* AVCodecParameters* for the ffmpeg path */

    /* --- ffmpeg tuning, resolved by the caller's playback-profile logic.
     *     Kept out of the native path, which ignores them. Any field set to
     *     EVO_VDEC_KEEP is left at the codec's default. --- */
    int   thread_count;            /* 0 / EVO_VDEC_KEEP => codec default     */
    int   thread_type;             /* FF_THREAD_* bitmask, or EVO_VDEC_KEEP  */
    int   flag2_fast;              /* non-zero => set AV_CODEC_FLAG2_FAST    */
    int   skip_loop_filter;        /* AVDISCARD_*, or EVO_VDEC_KEEP          */
    int   skip_frame;              /* AVDISCARD_*, or EVO_VDEC_KEEP          */
    int   skip_idct;               /* AVDISCARD_*, or EVO_VDEC_KEEP          */
} evo_vdec_open_params;

/* Preload the native backend's system module (libSceVideodec2, sysmodule 207).
 * MUST be called from main() BEFORE the first evo_jailbreak_self() — the
 * self-unjail's mid-run credential swap makes the module load fail
 * (docs/evo-pro/status.md, 2026-09-03). No-op returning 0 on host + payload
 * builds. Returns 1 if native decode may be available this session — the
 * caller then requests EVO_VDEC_BACKEND_NATIVE; evo_vdec_open() still falls
 * back to FFmpeg if bring-up fails. Idempotent. */
int evo_vdec_probe(void);

/* GL-4 (#80): ask the native backend to emit NV12 (Y + interleaved UV) rather
 * than de-interleaving to planar I420 — the EVO_GL_DEVICE GL video path samples
 * NV12 directly on the GPU. No-op for FFmpeg / host / payload. Set once at boot. */
void evo_vdec_prefer_nv12(int on);

/* User-facing decoder preference (settings row, #37). Persisted as one int
 * in evo_player_settings.cfg — see prospero_settings_save/_load in main.c. */
typedef enum {
    EVO_VDEC_PREF_AUTO   = 0,   /* default: native when the probe passed, else FFmpeg */
    EVO_VDEC_PREF_FFMPEG = 1,   /* always software */
    EVO_VDEC_PREF_NATIVE = 2    /* native; falls back to FFmpeg if unavailable */
} evo_vdec_pref;

/* Resolve a settings-row preference (+ the codec about to be opened) into the
 * backend evo_vdec_open() should be asked for. This is advisory, not final:
 * evo_vdec_open() is still the authority and downgrades to FFmpeg on its own
 * for an unsupported codec or a native bring-up failure, regardless of what
 * this returns — so a NATIVE request that can't be honoured never crashes,
 * it just silently opens FFmpeg one call later. Never probes twice; reuses
 * the cached evo_vdec_probe() result. */
evo_vdec_backend evo_vdec_pref_resolve(evo_vdec_pref pref, int codec_id);

/* Would evo_vdec_open() honour a NATIVE request for this stream? Answers the
 * same profile / bit-depth / dimension gate evo_vdec_open() applies, without
 * opening anything, so a caller that only wants the hardware path can decide
 * before paying for a decoder it would immediately throw away. Advisory: a
 * slot already claimed by playback still turns into an FFmpeg open. Returns 0
 * on host / payload builds. */
int evo_vdec_native_can_open(int codec_id, int profile, int bit_depth,
                             int w, int h);

/* Open a decoder. Returns NULL on failure. `*chosen` (may be NULL) reports the
 * backend actually created. A EVO_VDEC_BACKEND_NATIVE request that cannot be
 * honoured (probe failed, unsupported codec, bring-up error) silently opens
 * FFmpeg instead — this never returns NULL when FFmpeg could have opened. */
evo_vdec *evo_vdec_open(const evo_vdec_open_params *p, evo_vdec_backend *chosen);

/* Feed one compressed access unit. pts_us is the presentation timestamp in
 * microseconds, or INT64_MIN for "unknown".
 *   0  = consumed
 *  >0  = not consumed, drain evo_vdec_receive() first then re-send
 *  <0  = fatal (caller falls back / stops) */
int evo_vdec_send(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us);

/* Pull one decoded frame.
 *   1  = frame written to *out (planes borrow decoder memory — valid until the
 *        next send/receive/flush; present synchronously)
 *   2  = a frame decoded but its pixel format is not pp_frame-mappable; use
 *        evo_vdec_ffmpeg_avframe() and the slow (swscale) path
 *   0  = need more input
 *  <0  = fatal */
int evo_vdec_receive(evo_vdec *v, pp_frame *out);

void evo_vdec_flush(evo_vdec *v);      /* seek: drop all buffered state */
void evo_vdec_close(evo_vdec *v);
evo_vdec_backend evo_vdec_active(const evo_vdec *v);

/* ---- #8: codec-sweep instrumentation -------------------------------------
 * The seam is the only place both backends are visible, so decode cost is
 * measured here once and is directly comparable between FFmpeg and native
 * (#38's A/B). "Decode" = wall time inside evo_vdec_send() + evo_vdec_receive(),
 * charged to the frames that came out; it excludes demux, pacing and present.
 */

/* Why an open produced the backend it did. Recorded per attempt so a sweep can
 * tell "this console has no decoder for that codec" apart from "it decoded, but
 * not fast enough" — the two look identical in a pass/fail table. */
typedef enum {
    EVO_VDEC_OPEN_OK          = 0,  /* opened on the backend that was asked for  */
    EVO_VDEC_OPEN_DOWNGRADED  = 1,  /* NATIVE asked, FFmpeg delivered            */
    EVO_VDEC_OPEN_NO_DECODER  = 2,  /* no FFmpeg decoder built for this codec    */
    EVO_VDEC_OPEN_CTX_FAIL    = 3,  /* decoder exists; alloc/params/open2 failed */
    EVO_VDEC_OPEN_BAD_ARGS    = 4
} evo_vdec_open_result;

/* Outcome of the most recent evo_vdec_open() on any thread. Valid until the
 * next open. Kept out of `evo_vdec` so it survives a NULL return. */
evo_vdec_open_result evo_vdec_last_open_result(void);
const char *evo_vdec_open_result_name(evo_vdec_open_result r);

typedef struct {
    evo_vdec_backend backend;       /* what actually decoded                    */
    int      codec_id;
    uint64_t frames_out;            /* receive() == 1 or 2                      */
    uint64_t frames_sw_mapped;      /* receive() == 2: the swscale detour       */
    uint64_t send_calls;
    uint64_t send_stalls;           /* send() == 1, "drain first"               */
    uint64_t decode_us_total;       /* send + receive, all calls                */
    uint64_t decode_us_max;
    uint64_t fatal_errors;          /* send/receive < 0                         */
    uint64_t decode_ring[256];      /* per-output-frame cost, for p95           */
    uint32_t decode_ring_count;
} evo_vdec_stats;

void     evo_vdec_get_stats(const evo_vdec *v, evo_vdec_stats *out);
uint64_t evo_vdec_decode_p95_us(const evo_vdec *v);

/* AVCodecID of the stream this decoder is playing — backend-independent (the
 * FFmpeg codec-name accessors below return "" on the native backend). Use with
 * avcodec_get_name() for the UI codec badge. 0 (AV_CODEC_ID_NONE) if v==NULL. */
int evo_vdec_codec_id(const evo_vdec *v);

/* ---- FFmpeg-backend-only accessors.
 *      CONTRACT: the native backend implements these as hard stubs —
 *      the int accessors return 0, evo_vdec_ffmpeg_codec_name() returns NULL,
 *      evo_vdec_ffmpeg_avframe() returns NULL (the r==2 swscale path cannot
 *      occur on the native backend, which only ever yields pp_frame-mappable
 *      output). Callers must treat 0 / NULL as "unknown" and fall back to the
 *      demuxer's AVCodecParameters, which are backend-independent.
 *      Transitional: the OSD / media-info badges still read codec context
 *      fields directly. --- */
int         evo_vdec_ffmpeg_width(const evo_vdec *v);
int         evo_vdec_ffmpeg_height(const evo_vdec *v);
int         evo_vdec_ffmpeg_color_trc(const evo_vdec *v);   /* AVColorTransferCharacteristic */
int         evo_vdec_ffmpeg_pix_fmt(const evo_vdec *v);     /* AVPixelFormat */
const char *evo_vdec_ffmpeg_codec_name(const evo_vdec *v);
void       *evo_vdec_ffmpeg_avframe(evo_vdec *v);           /* AVFrame* for the r==2 path */

#ifdef __cplusplus
}
#endif

#endif /* EVO_VDEC_H */
