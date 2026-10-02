/*
 * evo_vdec_ffmpeg.c — the FFmpeg implementation of evo_vdec.h.
 *
 * A near-verbatim lift of the inline avcodec_* video path out of main.c
 * (Track A step A6, docs/modularisation-plan.md §5). It owns the
 * AVCodecContext and the receive AVFrame, and adapts decoded frames into the
 * FFmpeg-free pp_frame. PURE: no clocks, no sleeps, no pp_playback, no
 * globals — pacing and present stay in the play loop.
 *
 * pp_map_avframe adapts a decoded AVFrame into a pp_frame. GL-5 (#81) removed
 * the old pp_map_yuv420p10_to_8 CPU pack — 10-bit planar goes through as
 * PP_FRAME_YUV420P10 and the GL shader samples it as GL_R16.
 */
#include "evo_vdec.h"
#include "evo_vdec_native.h"   /* sceVideodec2 backend (stubs off the app module) */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>

#ifndef FF_PROFILE_UNKNOWN
#define FF_PROFILE_UNKNOWN (-99)
#endif

/*
 * This file owns the public evo_vdec.h surface and dispatches: a NATIVE
 * request that opens goes to evo_vdec_native.c, everything else (and every
 * native failure) is the FFmpeg path below. `nat` is non-NULL iff
 * backend == EVO_VDEC_BACKEND_NATIVE, and then the ctx/frame/pkt fields are
 * unused.
 */
#define FFMPEG_FRAME_RING 4
struct evo_vdec {
    evo_vdec_backend backend;
    int              codec_id;   /* AVCodecID — backend-independent, for the UI */
    AVCodecContext  *ctx;
    AVFrame         *frames[FFMPEG_FRAME_RING]; /* ring for receive; planes borrowed until slot recycled */
    int              frame_idx;
    AVPacket        *pkt;     /* scratch for send */
    evo_vdec_native *nat;     /* sceVideodec2 sub-backend, or NULL */

    /* #8 instrumentation. `pending_us` accumulates the cost of the send() calls
     * and the empty receive() polls since the last frame came out, so the cost
     * of a frame includes the work that produced it rather than only the call
     * that collected it — a decoder with a 4-deep pipeline otherwise reports
     * near-zero per-frame times. */
    evo_vdec_stats stats;
    uint64_t       pending_us;
};

/* ---------------------------------------------------------------------------
 * #8 — decode timing at the seam
 * ------------------------------------------------------------------------ */

static int vdec_send_inner(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us);
static int vdec_receive_inner(evo_vdec *v, pp_frame *out);

static evo_vdec_open_result g_last_open_result = EVO_VDEC_OPEN_OK;

static uint64_t vdec_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

/* Charge `us` to the decoder. `produced` marks the call that yielded a frame:
 * the accumulated pending cost is flushed into the per-frame ring with it. */
static void vdec_note(evo_vdec *v, uint64_t us, int produced)
{
    v->stats.decode_us_total += us;
    v->pending_us += us;
    if (!produced)
        return;
    uint64_t frame_us = v->pending_us;
    v->pending_us = 0;
    if (frame_us > v->stats.decode_us_max)
        v->stats.decode_us_max = frame_us;
    /* Rolling ring of the last 256 frames, same shape as pp_playback's. */
    uint32_t i;
    if (v->stats.decode_ring_count < 256u) {
        i = v->stats.decode_ring_count;
        v->stats.decode_ring_count++;
    } else {
        i = (uint32_t)(v->stats.frames_out % 256u);
    }
    v->stats.decode_ring[i] = frame_us;
}

evo_vdec_open_result evo_vdec_last_open_result(void)
{
    return g_last_open_result;
}

const char *evo_vdec_open_result_name(evo_vdec_open_result r)
{
    switch (r) {
    case EVO_VDEC_OPEN_OK:         return "ok";
    case EVO_VDEC_OPEN_DOWNGRADED: return "downgraded";
    case EVO_VDEC_OPEN_NO_DECODER: return "no_decoder";
    case EVO_VDEC_OPEN_CTX_FAIL:   return "ctx_fail";
    case EVO_VDEC_OPEN_BAD_ARGS:   return "bad_args";
    }
    return "?";
}

void evo_vdec_get_stats(const evo_vdec *v, evo_vdec_stats *out)
{
    if (!out)
        return;
    if (!v) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = v->stats;
    out->backend  = v->backend;
    out->codec_id = v->codec_id;
}

uint64_t evo_vdec_decode_p95_us(const evo_vdec *v)
{
    uint64_t tmp[256];
    uint32_t n, i, j, idx;
    if (!v)
        return 0;
    n = v->stats.decode_ring_count;
    if (n == 0)
        return 0;
    if (n > 256u)
        n = 256u;
    memcpy(tmp, v->stats.decode_ring, n * sizeof(uint64_t));
    for (i = 1; i < n; i++) {          /* insertion sort, n <= 256 */
        uint64_t val = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > val) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = val;
    }
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return tmp[idx];
}

int evo_vdec_probe(void)
{
    return evo_vdec_native_probe();
}

void evo_vdec_prefer_nv12(int on)
{
    evo_vdec_native_prefer_nv12(on);
}

evo_vdec_backend evo_vdec_pref_resolve(evo_vdec_pref pref, int codec_id)
{
    if (pref == EVO_VDEC_PREF_FFMPEG)
        return EVO_VDEC_BACKEND_FFMPEG;
    if (!evo_vdec_probe())
        return EVO_VDEC_BACKEND_FFMPEG;   /* NATIVE or AUTO, but nothing to open */
    /* AUTO only asks for native on a codec whose resident decoder actually came
     * up this session (H.264 / HEVC Main / VP9 Profile 0) — an explicit NATIVE
     * request is still passed through unsupported, since evo_vdec_open()
     * downgrades that itself. Profile/bit-depth/dimension gating happens in
     * evo_vdec_native_open(); here we only need the codec-level check. */
    if (pref == EVO_VDEC_PREF_AUTO &&
        !evo_vdec_native_supports(codec_id, FF_PROFILE_UNKNOWN, 8, 0, 0))
        return EVO_VDEC_BACKEND_FFMPEG;
    return EVO_VDEC_BACKEND_NATIVE;
}

int evo_vdec_native_can_open(int codec_id, int profile, int bit_depth,
                             int w, int h)
{
    if (!evo_vdec_probe())
        return 0;
    return evo_vdec_native_supports(codec_id, profile, bit_depth, w, h);
}

static int pp_map_avframe(const AVFrame *frame, pp_frame *out, int64_t pts_us)
{
    if (!frame || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->width = (uint32_t)frame->width;
    out->height = (uint32_t)frame->height;
    out->pts_us = pts_us;
    out->color_trc = (int)frame->color_trc;
    if (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P) {
        out->format = PP_FRAME_YUV420P;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->planes[2] = frame->data[2];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        out->strides[2] = frame->linesize[2];
        return 0;
    }
    if (frame->format == AV_PIX_FMT_NV12) {
        out->format = PP_FRAME_NV12;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->strides[0] = frame->linesize[0];
        out->strides[1] = frame->linesize[1];
        return 0;
    }
    /* GL-5 (#81): 10-bit planar 4:2:0 goes straight through as a 16-bit frame -
     * the GL video shader samples it as GL_R16. No CPU pack. Little-endian only
     * (PS5 is LE, and yuv420p10be effectively never occurs in real files) - a BE
     * frame falls through to the swscale path like any other exotic format. */
    if (frame->format == AV_PIX_FMT_YUV420P10LE) {
        out->format = PP_FRAME_YUV420P10;
        out->planes[0] = frame->data[0];
        out->planes[1] = frame->data[1];
        out->planes[2] = frame->data[2];
        out->strides[0] = frame->linesize[0];   /* bytes (= 2 * samples) */
        out->strides[1] = frame->linesize[1];
        out->strides[2] = frame->linesize[2];
        return 0;
    }
    return -2;
}

/* ---------------------------------------------------------------------------
 * evo_vdec interface
 * ------------------------------------------------------------------------ */

static void ffmpeg_apply_tuning(AVCodecContext *ctx, const evo_vdec_open_params *p)
{
    if (p->thread_count > 0 && p->thread_count != EVO_VDEC_KEEP)
        ctx->thread_count = p->thread_count;
    if (p->thread_type != EVO_VDEC_KEEP)
        ctx->thread_type = p->thread_type;
#ifdef AV_CODEC_FLAG2_FAST
    if (p->flag2_fast)
        ctx->flags2 |= AV_CODEC_FLAG2_FAST;
#endif
    if (p->skip_loop_filter != EVO_VDEC_KEEP)
        ctx->skip_loop_filter = (enum AVDiscard)p->skip_loop_filter;
    if (p->skip_frame != EVO_VDEC_KEEP)
        ctx->skip_frame = (enum AVDiscard)p->skip_frame;
    if (p->skip_idct != EVO_VDEC_KEEP)
        ctx->skip_idct = (enum AVDiscard)p->skip_idct;
}

evo_vdec *evo_vdec_open(const evo_vdec_open_params *p, evo_vdec_backend *chosen)
{
    if (chosen)
        *chosen = EVO_VDEC_BACKEND_FFMPEG;
    g_last_open_result = EVO_VDEC_OPEN_BAD_ARGS;
    if (!p)
        return NULL;
    int wanted_native = (p->backend == EVO_VDEC_BACKEND_NATIVE);

    /* Native first, if asked. Any failure falls through to FFmpeg — the seam
     * contract is "never NULL when FFmpeg could have opened". */
    if (p->backend == EVO_VDEC_BACKEND_NATIVE) {
        evo_vdec_native *nat = evo_vdec_native_open(p);
        if (nat) {
            evo_vdec *v = (evo_vdec *)calloc(1, sizeof(*v));
            if (v) {
                v->backend  = EVO_VDEC_BACKEND_NATIVE;
                v->codec_id = p->codec_id;
                v->nat = nat;
                if (chosen)
                    *chosen = EVO_VDEC_BACKEND_NATIVE;
                g_last_open_result = EVO_VDEC_OPEN_OK;
                return v;
            }
            evo_vdec_native_close(nat);
        }
    }

    /* Past this point any failure is FFmpeg's, so the sweep can separate "no
     * decoder exists" from "the decoder exists but would not come up". */
    g_last_open_result = EVO_VDEC_OPEN_CTX_FAIL;

    const AVCodec *dec = avcodec_find_decoder((enum AVCodecID)p->codec_id);
    if (!dec) {
        g_last_open_result = EVO_VDEC_OPEN_NO_DECODER;
        return NULL;
    }

    evo_vdec *v = (evo_vdec *)calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->backend  = EVO_VDEC_BACKEND_FFMPEG;
    v->codec_id = p->codec_id;

    v->ctx = avcodec_alloc_context3(dec);
    for (int i = 0; i < FFMPEG_FRAME_RING; i++) {
        v->frames[i] = av_frame_alloc();
    }
    v->pkt = av_packet_alloc();
    if (!v->ctx || !v->pkt || !v->frames[0] || !v->frames[1] || !v->frames[2] || !v->frames[3]) {
        evo_vdec_close(v);
        return NULL;
    }

    if (p->avctx_params &&
        avcodec_parameters_to_context(v->ctx,
                                      (const AVCodecParameters *)p->avctx_params) < 0) {
        evo_vdec_close(v);
        return NULL;
    }

    ffmpeg_apply_tuning(v->ctx, p);

    /* Feed the decoder timestamps in microseconds so best_effort_timestamp
     * comes straight back as pts_us — the caller rescales stream PTS -> us. */
    v->ctx->pkt_timebase = (AVRational){ 1, 1000000 };

    if (avcodec_open2(v->ctx, dec, NULL) < 0) {
        evo_vdec_close(v);
        return NULL;
    }
    g_last_open_result = wanted_native ? EVO_VDEC_OPEN_DOWNGRADED
                                       : EVO_VDEC_OPEN_OK;
    return v;
}

int evo_vdec_send(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us)
{
    if (!v)
        return -1;
    uint64_t t0 = vdec_now_us();
    int r = vdec_send_inner(v, data, size, pts_us);
    vdec_note(v, vdec_now_us() - t0, 0);
    v->stats.send_calls++;
    if (r > 0)
        v->stats.send_stalls++;
    else if (r < 0)
        v->stats.fatal_errors++;
    return r;
}

static int vdec_send_inner(evo_vdec *v, const uint8_t *data, int size, int64_t pts_us)
{
    if (v->backend == EVO_VDEC_BACKEND_NATIVE)
        return evo_vdec_native_send(v->nat, data, size, pts_us);
    if (!v->ctx)
        return -1;

    av_packet_unref(v->pkt);

    int flush = !(data && size > 0);
    if (!flush) {
        /* Borrow the caller's buffer (alive for this call); avcodec_send_packet
         * copies into the decoder's own storage. dts left unset — packets are in
         * decode order and B-frame pts is non-monotonic; the decoder reorders to
         * display order and fills best_effort_timestamp from the pts we give it
         * (microseconds, matching ctx->pkt_timebase). */
        v->pkt->data = (uint8_t *)data;
        v->pkt->size = size;
        v->pkt->pts  = (pts_us == INT64_MIN) ? AV_NOPTS_VALUE : pts_us;
        v->pkt->dts  = AV_NOPTS_VALUE;
    }

    int ret = avcodec_send_packet(v->ctx, flush ? NULL : v->pkt);

    /* Drop the borrowed pointer before the next unref touches the packet. */
    v->pkt->data = NULL;
    v->pkt->size = 0;

    if (ret == 0)
        return 0;
    if (ret == AVERROR(EAGAIN))
        return 1;   /* drain receive first, packet not consumed */
    return -1;
}

int evo_vdec_receive(evo_vdec *v, pp_frame *out)
{
    if (!v || !out)
        return -1;
    uint64_t t0 = vdec_now_us();
    int r = vdec_receive_inner(v, out);
    vdec_note(v, vdec_now_us() - t0, r > 0);
    if (r > 0) {
        v->stats.frames_out++;
        if (r == 2)
            v->stats.frames_sw_mapped++;
    } else if (r < 0) {
        v->stats.fatal_errors++;
    }
    return r;
}

static int vdec_receive_inner(evo_vdec *v, pp_frame *out)
{
    if (v->backend == EVO_VDEC_BACKEND_NATIVE)
        return evo_vdec_native_receive(v->nat, out);   /* 1 / 0 / <0, never 2 */
    if (!v->ctx)
        return -1;

    int next_idx = (v->frame_idx + 1) % FFMPEG_FRAME_RING;
    av_frame_unref(v->frames[next_idx]);

    int ret = avcodec_receive_frame(v->ctx, v->frames[next_idx]);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;   /* EOF: drained after a NULL send — not a decode failure */
    if (ret < 0)
        return -1;

    v->frame_idx = next_idx;
    AVFrame *frame = v->frames[next_idx];

    int64_t pts_us = INT64_MIN;
    if (frame->best_effort_timestamp != AV_NOPTS_VALUE)
        pts_us = frame->best_effort_timestamp;
    else if (frame->pts != AV_NOPTS_VALUE)
        pts_us = frame->pts;

    if (pp_map_avframe(frame, out, pts_us) == 0)
        return 1;

    /* Decoded, but an exotic pixel format — caller runs the swscale path. */
    return 2;
}

void evo_vdec_flush(evo_vdec *v)
{
    if (!v)
        return;
    v->pending_us = 0;   /* #8: don't charge pre-seek work to the next frame */
    if (v->backend == EVO_VDEC_BACKEND_NATIVE) {
        evo_vdec_native_flush(v->nat);
        return;
    }
    if (v->ctx)
        avcodec_flush_buffers(v->ctx);
    for (int i = 0; i < FFMPEG_FRAME_RING; i++) {
        if (v->frames[i])
            av_frame_unref(v->frames[i]);
    }
    if (v->pkt)
        av_packet_unref(v->pkt);
}

void evo_vdec_close(evo_vdec *v)
{
    if (!v)
        return;
    if (v->backend == EVO_VDEC_BACKEND_NATIVE) {
        evo_vdec_native_close(v->nat);
        free(v);
        return;
    }
    if (v->pkt)
        av_packet_free(&v->pkt);
    for (int i = 0; i < FFMPEG_FRAME_RING; i++) {
        if (v->frames[i])
            av_frame_free(&v->frames[i]);
    }
    if (v->ctx)
        avcodec_free_context(&v->ctx);
    free(v);
}

evo_vdec_backend evo_vdec_active(const evo_vdec *v)
{
    return v ? v->backend : EVO_VDEC_BACKEND_FFMPEG;
}

int evo_vdec_codec_id(const evo_vdec *v)
{
    return v ? v->codec_id : 0 /* AV_CODEC_ID_NONE */;
}

/* ---- FFmpeg-backend accessors ---- */
int evo_vdec_ffmpeg_width(const evo_vdec *v)
{
    return (v && v->ctx) ? v->ctx->width : 0;
}
int evo_vdec_ffmpeg_height(const evo_vdec *v)
{
    return (v && v->ctx) ? v->ctx->height : 0;
}
int evo_vdec_ffmpeg_color_trc(const evo_vdec *v)
{
    return (v && v->ctx) ? (int)v->ctx->color_trc : 0;
}
int evo_vdec_ffmpeg_pix_fmt(const evo_vdec *v)
{
    return (v && v->ctx) ? (int)v->ctx->pix_fmt : -1;
}
const char *evo_vdec_ffmpeg_codec_name(const evo_vdec *v)
{
    if (v && v->ctx && v->ctx->codec && v->ctx->codec->name)
        return v->ctx->codec->name;
    return "";
}
void *evo_vdec_ffmpeg_avframe(evo_vdec *v)
{
    return (v && v->backend == EVO_VDEC_BACKEND_FFMPEG) ? v->frames[v->frame_idx] : NULL;
}
