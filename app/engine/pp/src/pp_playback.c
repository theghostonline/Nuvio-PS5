/*
 * pp_playback — decode + pace + clock + seek.
 *
 * GL-4 (#80) Stage 3 removed everything else: the CPU YUV->BGRA converters, the
 * tiled VideoOut present, the sceAgc GPU present, the V8/V3/1080 backend enum
 * and the `display` double buffer. push_frame now paces the decoded frame on
 * the presentation clock and publishes its planes; ui_rml/evo_gl_context_device
 * uploads them as R8/RG8 textures and converts on the quad.
 */
#include "pp_playback.h"

#include "pp_stage_breadcrumb.h"
#include "evo_boot_log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

static uint64_t now_ms(void)
{
    return now_us() / 1000ull;
}

static pthread_mutex_t *mtx(pp_playback *pb)
{
    return (pthread_mutex_t *)pb->lock;
}

/* Defined below; pp_playback_pause() needs it. Call with the lock held. */
static int hold_snapshot(pp_playback *pb);

void pp_playback_init(pp_playback *pb)
{
    pthread_mutex_t *m;
    if (!pb)
        return;
    memset(pb, 0, sizeof(*pb));
    m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (m) {
        pthread_mutex_init(m, NULL);
        pb->lock = m;
    }
    /*
     * Soft UHD: very wide late window (never freeze on late-drop).
     *
     * Early sleep is capped at 100 ms - enough to wait out one whole frame
     * interval of anything down to 10 fps. It was 25 ms, shorter than a
     * 24 fps frame (41.7 ms): a frame that arrived early slept 25 ms and was
     * shown anyway, so whenever the decoder ran ahead of real time the picture
     * ran at decoder speed. Files with audio never showed it - the audio clock
     * paces them in evo_playback.c - but a video-only file did: Chimera's 4K
     * AV1 played 1.37x fast (#94, hardware 2026-09-26). Waiting out an early
     * frame costs nothing: the decoder is by definition ahead.
     */
    pp_clock_init(&pb->clock, 800000, 100000);
    pb->aspect = PP_ASPECT_FIT;
    pb->out_w = 1920;
    pb->out_h = 1080;
    pb->stats.output_w = 1920;
    pb->stats.output_h = 1080;
    pb->stats.aspect = (int)PP_ASPECT_FIT;
}

void pp_playback_shutdown(pp_playback *pb)
{
    if (!pb)
        return;
    pp_playback_on_file_close(pb);
    free(pb->hold_buf);
    pb->hold_buf = NULL;
    pb->hold_cap = 0;
    pb->gl_ready = 0;
    pb->gl_src_y = pb->gl_src_uv = pb->gl_src_u = pb->gl_src_v = NULL;
    if (pb->lock) {
        pthread_mutex_destroy(mtx(pb));
        free(pb->lock);
        pb->lock = NULL;
    }
    memset(pb, 0, sizeof(*pb));
}

int pp_playback_set_output(pp_playback *pb, uint32_t w, uint32_t h,
                           pp_aspect_mode aspect)
{
    if (!pb || w < 64 || h < 64)
        return -1;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->out_w = w;
    pb->out_h = h;
    pb->aspect = aspect;
    pb->stats.output_w = (int)w;
    pb->stats.output_h = (int)h;
    pb->stats.aspect = (int)aspect;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return 0;
}

void pp_playback_on_file_open(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    pb->active = 1;
    pb->seek_discarding = 0;
    pb->display_pts_us = 0;
    /* Don't show the previous file's last frame, or hold it through its seek. */
    pb->gl_ready = 0;
    pb->hold_valid = 0;
    pb->stats.frames_in = 0;
    pb->stats.frames_converted = 0;
    pb->stats.frames_published = 0;
    pb->stats.frames_late_dropped = 0;
    pb->stats.frames_discarded_seek = 0;
    pb->stats.convert_us_total = 0;
    pb->stats.convert_us_max = 0;
    pb->stats.convert_ring_count = 0;
    pb->stats.sample_luma = 0;
}

void pp_playback_on_file_close(pp_playback *pb)
{
    if (!pb)
        return;
    pb->active = 0;
    pb->seek_discarding = 0;
    pp_clock_reset(&pb->clock);
    pb->stats.clock_resets++;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->gl_ready = 0;
    pb->hold_valid = 0;
    pb->gl_src_y = pb->gl_src_uv = pb->gl_src_u = pb->gl_src_v = NULL;
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_pause(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_pause(&pb->clock);
    /*
     * Take the same snapshot a seek takes.
     *
     * A scrub pauses this clock but NOT the decode threads: the FSM's
     * Scrubbing state leaves player_paused at 0, so the demux and decode
     * threads keep running forward from the old position while push_frame
     * stops publishing (the pp_clock_is_paused early-out below). The renderer
     * is still holding borrowed pointers into the decoder's 8-slot frame pool
     * (FRAME_POOL_SLOTS, evo_vdec_native.c), so the picture that is supposed
     * to be frozen behind the scrub OSD is overwritten every 8th decoded frame
     * and the scene visibly crawls forward instead - the "I seek 10 minutes
     * ahead and watch the current shot advance frame by frame" report.
     *
     * One copy per pause, not per frame; the steady state stays zero-copy.
     */
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (!pb->hold_valid)
        pb->hold_valid = hold_snapshot(pb);
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_resume(pp_playback *pb)
{
    if (!pb)
        return;
    pp_clock_resume(&pb->clock);
}

static void note_convert(pp_playback *pb, uint64_t us)
{
    uint32_t i;
    pb->stats.convert_us_total += us;
    if (us > pb->stats.convert_us_max)
        pb->stats.convert_us_max = us;
    /* Rolling ring of last 256 publish times for p95. */
    if (pb->stats.convert_ring_count < 256u) {
        i = pb->stats.convert_ring_count;
        pb->stats.convert_ring[i] = us;
        pb->stats.convert_ring_count++;
    } else {
        i = (uint32_t)(pb->stats.frames_converted % 256u);
        pb->stats.convert_ring[i] = us;
    }
}

/*
 * Late frames in a row before the presentation clock is assumed wrong and
 * re-based. Comfortably more than a genuine catch-up needs (a few frames),
 * comfortably less than a second at any frame rate.
 */
#define PP_LATE_DROP_RESYNC 20

/*
 * Stage 2d: freeze the currently published frame into pp_playback's own memory.
 *
 * Taken the instant the seek is submitted, before the decode thread has been
 * let loose on the discard window: the native decoder's 12-slot pool and
 * FFmpeg's receive AVFrame both still hold the last published frame at that
 * point, and a slot is only recycled once decoding has moved that far on.
 * Caller holds the lock.
 *
 * Returns 1 if a snapshot is now being served.
 */
static int hold_snapshot(pp_playback *pb)
{
    size_t y_rows, c_rows, y_sz, need;
    const uint8_t *sy, *suv, *su, *sv;
    int planar;

    if (!pb->gl_ready || !pb->gl_src_y || pb->gl_src_ypitch <= 0)
        return 0;

    planar = (pb->gl_src_uv == NULL);
    if (planar && (!pb->gl_src_u || !pb->gl_src_v))
        return 0;

    y_rows = pb->gl_ch;
    c_rows = (pb->gl_ch + 1u) / 2u;
    y_sz   = (size_t)pb->gl_src_ypitch * y_rows;

    if (planar)
        need = y_sz + (size_t)pb->gl_src_upitch * c_rows
                    + (size_t)pb->gl_src_vpitch * c_rows;
    else
        need = y_sz + (size_t)pb->gl_src_uvpitch * c_rows;

    if (pb->hold_cap < need) {
        uint8_t *nb = (uint8_t *)realloc(pb->hold_buf, need);
        if (!nb)
            return 0;      /* out of memory: fall back to the borrowed planes */
        pb->hold_buf = nb;
        pb->hold_cap = need;
    }

    sy  = pb->gl_src_y;
    suv = pb->gl_src_uv;
    su  = pb->gl_src_u;
    sv  = pb->gl_src_v;

    memcpy(pb->hold_buf, sy, y_sz);
    pb->hold_uv_off = pb->hold_u_off = pb->hold_v_off = 0;
    if (planar) {
        pb->hold_u_off = y_sz;
        pb->hold_v_off = y_sz + (size_t)pb->gl_src_upitch * c_rows;
        memcpy(pb->hold_buf + pb->hold_u_off, su,
               (size_t)pb->gl_src_upitch * c_rows);
        memcpy(pb->hold_buf + pb->hold_v_off, sv,
               (size_t)pb->gl_src_vpitch * c_rows);
    } else {
        pb->hold_uv_off = y_sz;
        memcpy(pb->hold_buf + pb->hold_uv_off, suv,
               (size_t)pb->gl_src_uvpitch * c_rows);
    }

    pb->hold_planar  = planar;
    pb->hold_ten_bit = pb->gl_ten_bit;
    pb->hold_color_trc = pb->gl_color_trc;
    pb->hold_ypitch  = pb->gl_src_ypitch;
    pb->hold_uvpitch = pb->gl_src_uvpitch;
    pb->hold_upitch  = pb->gl_src_upitch;
    pb->hold_vpitch  = pb->gl_src_vpitch;
    pb->hold_cw = pb->gl_cw;
    pb->hold_ch = pb->gl_ch;
    pb->hold_dw = pb->gl_dw;
    pb->hold_dh = pb->gl_dh;
    return 1;
}

int pp_playback_push_frame(pp_playback *pb, const pp_frame *src)
{
    uint32_t cw, ch, dw, dh;
    uint64_t t0;
    int syp;

    if (!pb || !src || !pb->active)
        return -1;

    pb->stats.frames_in++;

    if (pb->seek_discarding) {
        if (src->pts_us < pb->seek_target_us) {
            pb->stats.frames_discarded_seek++;
            return 1;
        }
        pp_clock_reset(&pb->clock);
        pb->stats.clock_resets++;
        pp_clock_start(&pb->clock, src->pts_us);
        pb->seek_discarding = 0;
        if (pb->seek_begin_ms) {
            pb->stats.seek_to_first_frame_ms = now_ms() - pb->seek_begin_ms;
            pb->seek_begin_ms = 0;
        }
    }

    if (pp_clock_is_paused(&pb->clock))
        return 1;

    if (!pb->clock.started) {
        pp_clock_start(&pb->clock, src->pts_us);
    } else if (pp_clock_wait_or_drop(&pb->clock, src->pts_us) == PP_CLOCK_DROP) {
        pb->stats.frames_late_dropped++;
        /*
         * Dropping late frames is how the clock catches up, and normally it
         * does so within a handful of frames. A long unbroken run means the
         * clock is not behind the video - it is wrong: its wall-time base
         * advanced during a period when nothing was being pushed, so every
         * frame now looks late and the picture freezes for good while audio
         * carries on. Re-base on the frame in hand and let it through.
         */
        if (++pb->late_drop_streak < PP_LATE_DROP_RESYNC)
            return 1;
        pp_clock_reset(&pb->clock);
        pp_clock_start(&pb->clock, src->pts_us);
        pb->stats.clock_resets++;
        pb->late_drop_streak = 0;
    } else {
        pb->late_drop_streak = 0;
    }

    if (!src->planes[0])
        return -4;
    if (src->format != PP_FRAME_NV12 && src->format != PP_FRAME_YUV420P &&
        src->format != PP_FRAME_YUV420P10 && src->format != PP_FRAME_NV12_10)
        return -4;

    {
        int ten = (src->format == PP_FRAME_YUV420P10 || src->format == PP_FRAME_NV12_10);
        int bpp = ten ? 2 : 1;
        dw = src->width;
        dh = src->height;
        cw = (dw + 1u) & ~1u;             /* even luma width -> texture width  */
        ch = src->coded_height ? src->coded_height : src->height;
        if (ch < dh) ch = dh;
        ch = (ch + 1u) & ~1u;             /* even -> UV plane is ch/2 rows     */
        syp = src->strides[0] > 0 ? src->strides[0] : (int)src->width * bpp;
    }

    t0 = now_us();

    /*
     * Stash the borrowed decoder planes — no copy anywhere. push_frame is on
     * the decode thread and has just pace-slept to this frame's PTS, so the
     * decoder won't recycle the pool slot before the render loop's
     * pp_playback_get_video_frame() + upload runs (~1 frame; the pool is 12 slots).
     * The one case that breaks is a seek's discard window, which has no pacing
     * — hold_snapshot() covers it.
     */
    if (pb->lock) pthread_mutex_lock(mtx(pb));
    pb->gl_ten_bit = (src->format == PP_FRAME_YUV420P10 || src->format == PP_FRAME_NV12_10);
    pb->gl_color_trc = src->color_trc;
    pb->gl_src_y  = src->planes[0];
    pb->gl_src_ypitch = syp;
    if (src->format == PP_FRAME_NV12 || src->format == PP_FRAME_NV12_10) {
        pb->gl_src_uv = src->planes[1];
        pb->gl_src_uvpitch = src->strides[1] > 0 ? src->strides[1] : syp;
        pb->gl_src_u = pb->gl_src_v = NULL;
    } else {
        int cbpp = pb->gl_ten_bit ? 2 : 1;
        pb->gl_src_uv = NULL;
        pb->gl_src_u = src->planes[1];
        pb->gl_src_v = src->planes[2];
        pb->gl_src_upitch = src->strides[1] > 0 ? src->strides[1] : (int)((src->width + 1u) / 2u) * cbpp;
        pb->gl_src_vpitch = src->strides[2] > 0 ? src->strides[2] : pb->gl_src_upitch;
    }
    pb->gl_cw = cw;
    pb->gl_ch = ch;
    pb->gl_dw = dw;
    pb->gl_dh = dh;
    pb->gl_ready = 1;
    pb->hold_valid = 0;          /* live frames again */
    pb->display_pts_us = src->pts_us;
    pb->stats.frames_converted++;
    pb->stats.frames_published++;
    if (pb->stats.sample_luma == 0)
        pb->stats.sample_luma = src->planes[0][0];
    if (pb->lock) pthread_mutex_unlock(mtx(pb));

    if (pb->stats.frames_converted == 1)
        pp_stage_bc_checkpoint("010_FIRST_FRAME_DECODED", "gl video path");
    note_convert(pb, now_us() - t0);
    return 0;
}

int pp_playback_has_display(const pp_playback *pb)
{
    return pb && pb->gl_ready;
}

int pp_playback_get_video_frame(pp_playback *pb, pp_video_frame *f)
{
    int got = 0;
    if (f)
        memset(f, 0, sizeof(*f));
    if (!pb || !f)
        return 0;
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    if (pb->hold_valid && pb->hold_buf) {
        f->y        = pb->hold_buf;
        f->y_pitch  = pb->hold_ypitch;
        if (pb->hold_planar) {
            f->u        = pb->hold_buf + pb->hold_u_off;
            f->v        = pb->hold_buf + pb->hold_v_off;
            f->u_pitch  = pb->hold_upitch;
            f->v_pitch  = pb->hold_vpitch;
        } else {
            f->uv       = pb->hold_buf + pb->hold_uv_off;
            f->uv_pitch = pb->hold_uvpitch;
        }
        f->coded_w  = pb->hold_cw;
        f->coded_h  = pb->hold_ch;
        f->disp_w   = pb->hold_dw;
        f->disp_h   = pb->hold_dh;
        f->ten_bit  = pb->hold_ten_bit;
        f->color_trc = pb->hold_color_trc;
        f->ready    = 1;
        f->held     = 1;
        got = 1;
    } else if (pb->gl_ready && pb->gl_src_y) {
        f->y        = pb->gl_src_y;
        f->uv       = pb->gl_src_uv;
        f->u        = pb->gl_src_u;
        f->v        = pb->gl_src_v;
        f->y_pitch  = pb->gl_src_ypitch;
        f->uv_pitch = pb->gl_src_uvpitch;
        f->u_pitch  = pb->gl_src_upitch;
        f->v_pitch  = pb->gl_src_vpitch;
        f->coded_w  = pb->gl_cw;
        f->coded_h  = pb->gl_ch;
        f->disp_w   = pb->gl_dw;
        f->disp_h   = pb->gl_dh;
        f->ten_bit  = pb->gl_ten_bit;
        f->color_trc = pb->gl_color_trc;
        f->ready    = 1;
        got = 1;
    }
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
    return got;
}

void pp_playback_notify_seek_begin(pp_playback *pb, int64_t target_pts_us)
{
    if (!pb)
        return;
    /*
     * One seek announces itself twice: PlaybackController::seekTo arms it on
     * the UI thread, then the demux thread arms it again when it picks the
     * request up. Arming twice double-counts the request, restarts the
     * seek_to_first_frame timer, and re-snapshots the held frame from decoder
     * planes the second caller has no pacing guarantee on. Arm once.
     */
    if (pb->seek_discarding && pb->seek_target_us == target_pts_us)
        return;
    pb->stats.seek_requests++;
    pb->seek_discarding = 1;
    pb->seek_target_us = target_pts_us;
    pb->seek_begin_ms = now_ms();
    pp_clock_pause(&pb->clock);
    if (pb->lock)
        pthread_mutex_lock(mtx(pb));
    pb->hold_valid = hold_snapshot(pb);
    if (pb->lock)
        pthread_mutex_unlock(mtx(pb));
}

void pp_playback_notify_seek_end(pp_playback *pb, int success,
                                 uint64_t discarded, uint64_t elapsed_ms)
{
    if (!pb)
        return;
    (void)discarded;
    (void)elapsed_ms;
    if (success)
        pb->stats.seek_successes++;
    else {
        pb->stats.seek_failures++;
        pb->seek_discarding = 0;
        /* No frames are coming to clear the hold; go back to the live planes. */
        if (pb->lock)
            pthread_mutex_lock(mtx(pb));
        pb->hold_valid = 0;
        if (pb->lock)
            pthread_mutex_unlock(mtx(pb));
    }
    /* clock re-start happens on first post-seek push_frame */
}

void pp_playback_get_stats(const pp_playback *pb, pp_playback_stats *out)
{
    if (!pb || !out)
        return;
    *out = pb->stats;
}

uint64_t pp_playback_convert_p95_us(const pp_playback *pb)
{
    uint64_t tmp[256];
    uint32_t n, i, j, idx;
    if (!pb)
        return 0;
    n = pb->stats.convert_ring_count;
    if (n == 0)
        return 0;
    if (n > 256)
        n = 256;
    memcpy(tmp, pb->stats.convert_ring, n * sizeof(uint64_t));
    /* insertion sort small */
    for (i = 1; i < n; i++) {
        uint64_t v = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > v) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = v;
    }
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return tmp[idx];
}

void pp_playback_log_stats(const pp_playback *pb)
{
    pp_clock_stats cs;
    uint64_t avg = 0;
    if (!pb)
        return;
    pp_clock_get_stats(&pb->clock, &cs);
    if (pb->stats.frames_converted)
        avg = pb->stats.convert_us_total / pb->stats.frames_converted;
    evo_boot_log("stats output=%ux%u aspect=%d", pb->out_w, pb->out_h, (int)pb->aspect);
    evo_boot_log("stats frames_in=%llu converted=%llu published=%llu late_drop=%llu seek_disc=%llu",
            (unsigned long long)pb->stats.frames_in,
            (unsigned long long)pb->stats.frames_converted,
            (unsigned long long)pb->stats.frames_published,
            (unsigned long long)pb->stats.frames_late_dropped,
            (unsigned long long)pb->stats.frames_discarded_seek);
    evo_boot_log("stats publish_us_avg=%llu p95=%llu max=%llu",
            (unsigned long long)avg,
            (unsigned long long)pp_playback_convert_p95_us(pb),
            (unsigned long long)pb->stats.convert_us_max);
    evo_boot_log("stats seek_req=%llu ok=%llu fail=%llu clock_resets=%llu seek_to_first_ms=%llu",
            (unsigned long long)pb->stats.seek_requests,
            (unsigned long long)pb->stats.seek_successes,
            (unsigned long long)pb->stats.seek_failures,
            (unsigned long long)pb->stats.clock_resets,
            (unsigned long long)pb->stats.seek_to_first_frame_ms);
    evo_boot_log("stats clock_late_drops=%llu early_sleeps=%llu pause=%llu resume=%llu sample_luma=%u",
            (unsigned long long)cs.late_drops,
            (unsigned long long)cs.early_sleeps,
            (unsigned long long)cs.pause_count,
            (unsigned long long)cs.resume_count,
            (unsigned)pb->stats.sample_luma);
}
