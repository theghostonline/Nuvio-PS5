/*
 * evo_playback.c — the playback session: video decode/pace/present loop, the
 * media clock, the video thread. Track A step A7 of
 * docs/modularisation-plan.md §5.
 *
 * prospero_media_clock_seconds, present_pp_frame, convert_frame_via_sws,
 * prospero_video_queue_drain_nonkey, decode_next_video_frame and
 * video_decode_thread_func are lifted verbatim from main.c. The A/V-sync
 * pacing block inside decode_next_video_frame is unchanged.
 */
#include "evo_playback.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include "pp_playback.h"
#include "pp_frame.h"
#include "pp_stage_breadcrumb.h"

#include "evo_vdec.h"
#include "evo_packet_queue.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_direct_mem.h"

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

#define WIDTH  1920
#define HEIGHT 1080

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: state still owned by main.c. Replaced by evo_pb_*() at A8.
 * ------------------------------------------------------------------------ */
extern int                 player_paused;
extern int                 screen;
extern double              media_duration_sec;
extern int                 playback_profile;
/* Start-of-stream pre-buffer hold - see the note in Bridge.cpp. */
extern volatile int        pb_prebuffer_hold;
extern volatile int        pb_scrub_hold;
extern struct SwsContext  *play_sws;
extern AVFormatContext    *play_fmt;

extern evo_vdec           *g_vdec;   /* the live video decoder (A6, owned by main.c) */
extern pp_playback         g_pp_pb;
extern int                 g_first_frame_bc_done;

extern int       dbg_video_frames;
extern int       dbg_video_thread_alive;
extern int       dbg_swaps;
extern long long dbg_last_pts;
extern int       perf_decode_frames;

long long      now_ms(void);
void           toast(const char *title, const char *msg);
pp_aspect_mode prospero_view_mode_to_aspect(void);

/* ---------------------------------------------------------------------------
 * Playback session state (exported via evo_playback.h).
 * ------------------------------------------------------------------------ */
double first_video_pts_seconds = -1.0;
double video_clock_seconds = 0.0;
double video_fps = 60.0;
int    video_decode_ready = 0;
int    video_decode_done = 0;

/* Sustained decoder failure. The decode loop keeps feeding packets to a
 * decoder that returns fatal on every receive; the state corrupts (POC errors)
 * and it eventually faults. Instead: count the streak, and once it is clearly
 * not recoverable, raise this flag so main.c ends playback with a toast rather
 * than crashing. Reset per open in start_video_playback.
 *
 * NOTE: this fires for the hardware (sceVideodec2) backend too, so the toast
 * must not say "software". A stream whose SPS/PPS the HW decoder rejects after
 * a seek is the common trigger (#57). */
int    g_pb_decode_fatal = 0;
static int s_vdec_fatal_streak = 0;
#define EVO_VDEC_FATAL_STREAK_LIMIT 16
int evo_pb_decode_fatal(void) { return g_pb_decode_fatal; }

/* Clear the fatal state AND the streak counter. start_video_playback() calls
 * this on every (re)open — clearing only g_pb_decode_fatal leaves the streak
 * latched at the limit, so the first transient fatal after a reopen (e.g. the
 * FFmpeg-fallback reopen for #57) would re-trip it immediately. */
void evo_pb_reset_decode_fatal(void)
{
    g_pb_decode_fatal   = 0;
    s_vdec_fatal_streak = 0;
}

#ifdef EVO_APP_MODULE
extern void pp_stage_bc(const char *stage_id, const char *detail);
#endif

static void note_vdec_result(int fatal)
{
    if (!fatal) {
        s_vdec_fatal_streak = 0;
        return;
    }
    if (++s_vdec_fatal_streak < EVO_VDEC_FATAL_STREAK_LIMIT || g_pb_decode_fatal)
        return;
    g_pb_decode_fatal = 1;
    video_decode_done = 1;   /* stop the demux/decode loop */
#ifdef EVO_APP_MODULE
    pp_stage_bc("P8_VDEC_FATAL", "decode failed repeatedly - ending playback");
#endif
    toast("PLAYBACK", "Video decode failed - can't play this file");
}

volatile int video_thread_running = 0;
pthread_t    video_thread;

/*
 * 1 while the decode thread is idling (paused / off-screen / not ready) and is
 * therefore NOT inside pp_playback_push_frame. Kept as a debug/diagnostic
 * signal; the VO reconfigure it used to guard (#32) went with the CPU present
 * path in GL-4.
 */
volatile int video_decode_parked = 1;
/* Set by a seek (evo_demux.c) for as long as it needs the decoder to itself:
 * the decode thread parks and stays out of evo_vdec_* until it clears. */
volatile int video_decode_hold = 0;

AVPacket *video_video_pending_pkt = NULL;
pthread_mutex_t video_frame_mutex = PTHREAD_MUTEX_INITIALIZER;

uint32_t *video_frame_pixels = NULL;   /* aliases into the ring; read by main.c's legacy renderers */
int video_frame_w = 0;
int video_frame_h = 0;
int video_frame_loaded = 0;

/*
 * Internal to convert_frame_via_sws — nothing outside touches the ring.
 *
 * #6: this is the exotic-pixel-format swscale fallback (evo_vdec_receive() == 2
 * or the product backend inactive) — the product path presents through
 * pp_playback's display model, not this ring. Backed by the evo_direct_mem
 * slab and grow-only: the buffers are (re)allocated only when a frame arrives
 * larger than the current slot size, so a seek or a same-resolution re-open
 * reuses them and playback never churns the heap. Count trimmed 8 -> 3
 * (VIDEO_ROTATE_BUFFERS) — enough for one presented + one just-written + one
 * in-flight without the legacy renderer tearing.
 */
static uint32_t *video_rotate_pixels[VIDEO_ROTATE_BUFFERS] = {0};
static size_t    video_rotate_slot_bytes = 0;
static int video_rotate_index = 0;

static int present_pp_frame(const pp_frame *pf);
static int convert_frame_via_sws(AVFrame *frame);
static void prospero_video_queue_drain_nonkey(int max_packets);

double prospero_media_clock_seconds(void)
{
    double audio_rel = audio_clock_seconds;
    double video_rel = 0.0;

    if (first_video_pts_seconds >= 0.0)
        video_rel = video_clock_seconds - first_video_pts_seconds;
    else if (video_clock_seconds > 0.0)
        video_rel = video_clock_seconds;
    if (video_rel < 0.0)
        video_rel = 0.0;

    /*
     * Prefer audio when a playable track is open and the out clock has
     * started. Otherwise drive UI/resume from video so silent E-AC3
     * files still move the progress bar.
     */
    if (audio_stream_index >= 0 && audio_handle >= 1 && audio_rel >= 0.05)
        return audio_rel;
    return video_rel;
}


/*
 * present_pp_frame — job 4 of the old decode_next_video_frame (§5): hand a
 * decoded pp_frame to the product playback pipeline. The VO-ready gate and the
 * first-frame breadcrumb are kept verbatim from convert_frame_to_rgb().
 * Returns 1 when the frame was pushed, 0 when dropped (VO not ready).
 */
static int present_pp_frame(const pp_frame *pf)
{
    if (!g_first_frame_bc_done) {
        char d[80];
        snprintf(d, sizeof(d), "fmt=%d %ux%u", (int)pf->format, pf->width, pf->height);
        pp_stage_bc_checkpoint("009_FIRST_FRAME_ENTER", d);
        g_first_frame_bc_done = 1;
    }
    g_pp_pb.aspect = prospero_view_mode_to_aspect();
    g_pp_pb.stats.aspect = (int)g_pp_pb.aspect;
    (void)pp_playback_push_frame(&g_pp_pb, (pp_frame *)pf);
    video_frame_loaded = pp_playback_has_display(&g_pp_pb);
    return 1;
}

/*
 * convert_frame_via_sws — the legacy swscale RGBA path, used only when the
 * decoder hands back an exotic pixel format evo_vdec can't map to pp_frame
 * (evo_vdec_receive() == 2), or when the product backend is inactive. Verbatim
 * from the tail of the old convert_frame_to_rgb().
 */
static int convert_frame_via_sws(AVFrame *frame)
{
    if (!frame) return 0;

    {
        static int s_sws_warned;
        if (!s_sws_warned) {
            const char *pn = av_get_pix_fmt_name((enum AVPixelFormat)frame->format);
            char msg[80];
            snprintf(msg, sizeof(msg), "slow path fmt=%s", pn ? pn : "?");
            toast("CONVERT", msg);
            s_sws_warned = 1;
        }
    }

    if (video_frame_w != frame->width || video_frame_h != frame->height || video_rotate_pixels[0] == NULL) {
        size_t need = (size_t)frame->width * (size_t)frame->height * 4u;

        pthread_mutex_lock(&video_frame_mutex);

        /* Grow-only: keep the existing slots when the new frame fits. */
        if (need > video_rotate_slot_bytes || video_rotate_pixels[0] == NULL) {
            for (int i = 0; i < VIDEO_ROTATE_BUFFERS; i++) {
                evo_direct_mem_free(video_rotate_pixels[i]);
                video_rotate_pixels[i] = NULL;
            }
            video_rotate_slot_bytes = 0;

            for (int i = 0; i < VIDEO_ROTATE_BUFFERS; i++) {
                video_rotate_pixels[i] = (uint32_t *)evo_direct_mem_alloc(need);
                if (!video_rotate_pixels[i]) {
                    for (int j = 0; j < i; j++) {
                        evo_direct_mem_free(video_rotate_pixels[j]);
                        video_rotate_pixels[j] = NULL;
                    }
                    video_frame_pixels = NULL;
                    video_frame_loaded = 0;
                    pthread_mutex_unlock(&video_frame_mutex);
                    return 0;
                }
            }
            video_rotate_slot_bytes = need;
        }

        video_frame_w = frame->width;
        video_frame_h = frame->height;
        video_rotate_index = 0;
        video_frame_loaded = 0;

        video_frame_pixels = video_rotate_pixels[0];
        pthread_mutex_unlock(&video_frame_mutex);
    }

    if (!play_sws) {
        play_sws = sws_getContext(
            frame->width, frame->height, frame->format,
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, NULL, NULL, NULL
        );

        if (!play_sws) return 0;
    }

    int write_index = (video_rotate_index + 1) % VIDEO_ROTATE_BUFFERS;

    uint8_t *dst_data[4] = {(uint8_t*)video_rotate_pixels[write_index], NULL, NULL, NULL};
    int dst_linesize[4] = {video_frame_w * 4, 0, 0, 0};

    sws_scale(play_sws, (const uint8_t * const*)frame->data,
              frame->linesize, 0, frame->height,
              dst_data, dst_linesize);

    pthread_mutex_lock(&video_frame_mutex);
    video_rotate_index = write_index;
    video_frame_pixels = video_rotate_pixels[video_rotate_index];
    video_frame_loaded = 1;
    dbg_swaps++;
    pthread_mutex_unlock(&video_frame_mutex);

    return 1;
}


static void prospero_video_queue_drain_nonkey(int max_packets)
{
    /*
     * Drop only non-keyframes so the decoder never waits on a full flush
     * (flush → freeze until next key every few seconds).
     */
    int n = 0;
    while (n < max_packets) {
        AVPacket *drop = packet_queue_pop(&video_packet_queue);
        if (!drop)
            break;
        if (drop->flags & AV_PKT_FLAG_KEY) {
            /* Put keyframe back as pending so we don't skip the GOP start */
            if (!video_video_pending_pkt)
                video_video_pending_pkt = drop;
            else
                av_packet_free(&drop);
            break;
        }
        av_packet_free(&drop);
        n++;
    }
}


int decode_next_video_frame(void)
{
    if (!video_decode_ready || player_paused) return 0;

    /*
     * Audio-master pacing:
     *  - If video is AHEAD of audio: wait (full remaining gap, chunked) so
     *    picture is not "very fast".
     *  - If video is slightly late: present now (no freeze).
     *  - If video is badly late: drain non-keys, still present this frame.
     *  - Always convert (skip-convert = frozen picture).
     *  - Never avcodec_flush mid-stream.
     */
    for (int attempts = 0; attempts < 48; attempts++) {
        /* A seek (or pause) set player_paused and is waiting for this thread
         * to park before it flushes the decoder - get out between calls
         * rather than after up to 47 more of them. */
        if (player_paused || video_decode_hold)
            return 1;
        pp_frame pf;
        int recv_ret = evo_vdec_receive(g_vdec, &pf);   /* job 1 — pure decode */
        note_vdec_result(recv_ret < 0);
        if (g_pb_decode_fatal)
            return 0;
        if (recv_ret >= 1) {
            double video_rel = 0.0;
            double audio_rel = 0.0;
            double behind = 0.0;
            int wait_iters = 0;

            dbg_video_frames++;
            dbg_last_pts = pf.pts_us;   /* now microseconds (was raw stream PTS) */
            perf_decode_frames++;

            /*
             * #32: in the seek-discard window every frame between the keyframe
             * and the seek target is dropped by pp_playback_push_frame(). Pacing
             * them to the frame rate (the branches below) makes a seek across a
             * long 4K GOP take ~GOP-length wall time - the slow GTA-trailer
             * seek. Decode + discard as fast as the decoder returns instead;
             * sceVideodec2Decode is synchronous so this can't outrun it.
             */
            int seek_discarding = g_pp_pb.active && g_pp_pb.seek_discarding &&
                                  pf.pts_us < g_pp_pb.seek_target_us;

            /*
             * job 2 — media clock from the frame PTS (already microseconds).
             *
             * Frames inside the discard window are decoded only to reach the
             * target; none of them is shown. Latching first_video_pts_seconds
             * on one of them anchors the relative video clock at the keyframe
             * while the audio clock restarts at the target, so the instant the
             * picture resumes video_rel reads a whole run-up ahead of
             * audio_rel and the audio-master wait below freezes the picture
             * until audio covers the difference - the post-seek hitch. Anchor
             * on the first frame that is actually presented instead. The seek
             * path zeroes video_clock_seconds, so the UI position falls back
             * to the seek target while the window is open.
             */
            if (pf.pts_us != INT64_MIN && !seek_discarding) {
                video_clock_seconds = (double)pf.pts_us / 1000000.0;
                if (first_video_pts_seconds < 0.0)
                    first_video_pts_seconds = video_clock_seconds;
            }

            video_rel = 0.0;
            if (first_video_pts_seconds >= 0.0)
                video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;
            audio_rel = audio_clock_seconds;
            behind = audio_rel - video_rel; /* >0 => video late; <0 => video early */

            if (seek_discarding) {
                /* no pacing - the frame is about to be thrown away */
            } else if (audio_rel > 0.05 &&
                       video_rel >= EVO_AV_SYNC_SETTLE_SEC) {
                /*
                 * Outside the cold-start window (EVO_AV_SYNC_SETTLE_SEC - see
                 * evo_audio_out.h for why a freshly reset clock pair must not
                 * throttle each other):
                 *
                 * Video ahead of audio: wait for audio, but NEVER freeze the
                 * picture if audio clock stops (underrun / 44.1k stall).
                 * Y2JB-class clips froze ~2s in when wait never broke out.
                 */
                double audio_at_wait = audio_rel;
                int stuck_iters = 0;

                while (behind < -0.008 &&
                       !player_paused &&
                       video_decode_ready &&
                       wait_iters < 100) {
                    int sleep_us = (int)((-behind) * 1000000.0);
                    if (sleep_us > 15000)
                        sleep_us = 15000;
                    if (sleep_us < 500)
                        sleep_us = 500;
                    usleep(sleep_us);
                    audio_rel = audio_clock_seconds;
                    behind = audio_rel - video_rel;
                    wait_iters++;
                    if (audio_rel <= audio_at_wait + 0.0005)
                        stuck_iters++;
                    else {
                        audio_at_wait = audio_rel;
                        stuck_iters = 0;
                    }
                    /* ~120ms with no audio progress → present anyway */
                    if (stuck_iters >= 8)
                        break;
                }

                /* Badly late vs audio: drop queued non-keys, still show frame.
                 * When g_pp_pb is active, pp_clock handles late frame dropping
                 * at presentation time; dropping demux packets breaks decoder GOP integrity. */
                if (behind > 0.45 && !g_pp_pb.active)
                    prospero_video_queue_drain_nonkey(24);
            } else if (video_fps > 1.0 && !g_pp_pb.active) {
                /*
                 * Audio not running yet: pace by nominal frame interval so
                 * we don't race through the open before audio primes.
                 */
                static double s_last_present_host = -1.0;
                struct timespec ts;
                double now_s, target_s, frame_s;

                /* Reset host pacer on a fresh open (no first video PTS yet) */
                if (first_video_pts_seconds < 0.0 &&
                    audio_samples_played == 0)
                    s_last_present_host = -1.0;

                clock_gettime(CLOCK_MONOTONIC, &ts);
                now_s = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
                frame_s = 1.0 / video_fps;
                if (frame_s < 0.016)
                    frame_s = 0.016;
                if (frame_s > 0.050)
                    frame_s = 0.050;
                if (s_last_present_host > 0.0) {
                    target_s = s_last_present_host + frame_s;
                    if (now_s < target_s) {
                        int sleep_us = (int)((target_s - now_s) * 1000000.0);
                        if (sleep_us > 40000)
                            sleep_us = 40000;
                        if (sleep_us > 500)
                            usleep(sleep_us);
                    }
                }
                clock_gettime(CLOCK_MONOTONIC, &ts);
                s_last_present_host =
                    (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
            }


            /* jobs 3+4 — present. Always show something (skip = frozen). */
            if (recv_ret == 1 && g_pp_pb.active)
                present_pp_frame(&pf);
            else
                convert_frame_via_sws((AVFrame *)evo_vdec_ffmpeg_avframe(g_vdec));

            /*
             * One line per seek, the moment the discard window closes: what
             * the settle cost and how far apart the two clocks are when the
             * picture comes back. A healthy seek reads vrel ~= arel; a large
             * arel - vrel gap is audio replaying the run-up to the target.
             */
            {
                static int s_was_discarding = 0;
                int now_discarding = g_pp_pb.active && g_pp_pb.seek_discarding;
                if (s_was_discarding && !now_discarding) {
                    pp_playback_stats st;
                    char d[128];
                    pp_playback_get_stats(&g_pp_pb, &st);
                    snprintf(d, sizeof d,
                             "ms=%llu disc=%llu pts=%.3f vrel=%.3f arel=%.3f aq=%d",
                             (unsigned long long)st.seek_to_first_frame_ms,
                             (unsigned long long)st.frames_discarded_seek,
                             (double)pf.pts_us / 1000000.0,
                             video_rel, (double)audio_clock_seconds,
                             audio_queue_count);
                    pp_stage_bc("SEEK_SETTLE", d);
                }
                s_was_discarding = now_discarding;
            }
            /* evo_vdec_receive() unrefs its scratch frame on the next call. */
            return 1;
        }

        if (!video_video_pending_pkt) {
            video_video_pending_pkt = packet_queue_pop(&video_packet_queue);
        }

        if (!video_video_pending_pkt) {
            /*
             * End of stream: flush the decoder once so buffered pictures still
             * come out (the native backend holds a small PTS-reorder window;
             * FFmpeg buffers frame-threaded latency). Send a NULL AU, loop back
             * so evo_vdec_receive() drains the tail, then stop. s_eof_drained
             * re-arms as soon as the stream is no longer at EOF (seek/replay).
             */
            static int s_eof_drained = 0;
            if (!video_decode_done) {
                s_eof_drained = 0;
                usleep(100);
                return 1;
            }
            if (!s_eof_drained) {
                s_eof_drained = 1;
                evo_vdec_send(g_vdec, NULL, 0, INT64_MIN);
                continue;
            }
            return 0;
        }

        int64_t send_pts_us = INT64_MIN;
        if (video_video_pending_pkt->pts != AV_NOPTS_VALUE && play_fmt &&
            video_stream_index >= 0)
            send_pts_us = av_rescale_q(
                video_video_pending_pkt->pts,
                play_fmt->streams[video_stream_index]->time_base,
                (AVRational){ 1, 1000000 });

        int send_ret = evo_vdec_send(g_vdec, video_video_pending_pkt->data,
                                     video_video_pending_pkt->size, send_pts_us);

        if (send_ret == 0) {
            av_packet_free(&video_video_pending_pkt);
            continue;
        }

        if (send_ret > 0) {   /* EAGAIN: keep the packet pending, drain first */
            usleep(50);
            continue;
        }

        /* send_ret < 0 — fatal for this packet. */
        note_vdec_result(1);
        if (g_pb_decode_fatal)
            return 0;
        av_packet_free(&video_video_pending_pkt);
    }

    return 1;
}


void *video_decode_thread_func(void *arg) {
    long long next_ms = 0;
    double frame_ms = 33.333;

    while (video_thread_running) {
        if (
            player_paused ||
            video_decode_hold ||
            pb_prebuffer_hold ||
            pb_scrub_hold ||
            screen != SCREEN_PLAYER ||
            !video_decode_ready
        ) {
            video_decode_parked = 1;
            /* Parked: paused, off the player screen, or not ready. 1 ms was
             * 1000 pointless wakeups a second; 5 ms is still well inside a
             * frame, so nothing notices on resume. */
            usleep(5000);
            next_ms = 0;
            continue;
        }

        video_decode_parked = 0;
        dbg_video_thread_alive++;

        /* Audio-master wait is inside decode_next_video_frame. */
        decode_next_video_frame();
        usleep(playback_profile >= 2 ? 100 : 200);
    }

    return NULL;
}


/* ---- §4 façade ---- */
int evo_pb_is_active(void)          { return video_decode_ready; }
int evo_pb_is_paused(void)          { return player_paused; }
int evo_pb_is_eof(void)            { return video_decode_done; }
double evo_pb_position_s(void)      { return prospero_media_clock_seconds(); }
double evo_pb_duration_s(void)      { return media_duration_sec; }
double evo_pb_audio_clock_s(void)   { return (double)audio_clock_seconds; }
double evo_pb_video_clock_s(void)   { return video_clock_seconds; }
double evo_pb_video_fps(void)       { return video_fps; }
int evo_pb_active_backend(void)     { return (int)evo_vdec_active(g_vdec); }

void evo_pb_queue_depth(int *vpkts, int *apkts, int *ablocks)
{
    if (vpkts)  *vpkts  = packet_queue_count(&video_packet_queue);
    if (apkts)  *apkts  = packet_queue_count(&audio_packet_queue);
    if (ablocks) *ablocks = audio_queue_count;
}
