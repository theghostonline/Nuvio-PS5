/*
 * evo_demux.c — the demux thread + in-place seek path.
 *
 * Verbatim move of the PROSPERO_TRUE_AV_SEEK region, the two PacketQueue
 * instances and the stream indices from main.c (Track A step A5 of
 * docs/modularisation-plan.md). The only edits are `static` -> external
 * linkage on what main.c still touches and the transitional extern block
 * below.
 */
#include "evo_demux.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>

#include "pp_playback.h"
#include "evo_packet_queue.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_subtitle.h"
#ifdef NUVIO_APP
#include "nuvio_subs.h"
#endif
#include "evo_vdec.h"
#include "evo_adec.h"
#include "pp_stage_breadcrumb.h"

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core decode context + flags + the app playback
 * object, all still owned by main.c. Replaced by the evo_pb_*() façade and a
 * passed-in pp_playback* at A7/A8.
 * ------------------------------------------------------------------------ */
extern AVFormatContext *play_fmt;
extern AVCodecContext  *audio_ctx;
extern evo_vdec        *g_vdec;   /* owns the video codec context (A6) */

extern int      player_paused;
extern char     current_media_path[512];
extern double   media_duration_sec;
extern double   resume_base_offset_seconds;
extern volatile double resume_base_anchor_pending;
extern long long controls_last_used_ms;

extern int      video_decode_done;
extern int      video_decode_ready;
extern int      dbg_read_fail;
extern int      dbg_video_packets;
extern double   first_video_pts_seconds;
extern double   video_clock_seconds;

extern AVPacket *video_pending_pkt;
extern AVPacket *video_video_pending_pkt;
extern volatile int video_thread_running;  /* evo_playback.c */
extern volatile int video_decode_parked;
extern volatile int video_decode_hold;

extern int      playback_profile;
extern int      video_packet_cap;
extern int      audio_packet_cap;

/* Start-of-stream pre-buffer. Armed by PlaybackController for a network
 * source, cleared here - see the note in Bridge.cpp. */
extern volatile int pb_prebuffer_hold;
extern int          pb_prebuffer_packets;
extern int          pb_prebuffer_max_ms;

extern pp_playback g_pp_pb;

long long now_ms(void);
void      toast(const char *title, const char *msg);

/* ---------------------------------------------------------------------------
 * Demux state (exported via evo_demux.h).
 * ------------------------------------------------------------------------ */
PacketQueue video_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };
PacketQueue audio_packet_queue = { .mutex = PTHREAD_MUTEX_INITIALIZER };

int video_stream_index = -1;
int audio_stream_index = -1;

volatile int demux_thread_running = 0;
/* What the demux thread is doing, for the status line: 1 reading, 2 waiting
 * for video room, 3 waiting for audio room, 4 seeking, 5 read failed. */
volatile int evo_demux_state = 0;
pthread_t    demux_thread;


static pthread_mutex_t prospero_seek_mutex =
    PTHREAD_MUTEX_INITIALIZER;

static volatile int prospero_seek_pending = 0;
static volatile int prospero_seek_in_progress = 0;

static double prospero_seek_target_seconds = 0.0;
static int prospero_seek_restore_paused = 0;
static volatile long long s_seek_done_ms = 0;

/* A seek is queued, running, or finished less than a moment ago - the video
 * queue is empty on purpose then, not starved. */
int evo_demux_seek_busy(void)
{
    return prospero_seek_pending || prospero_seek_in_progress ||
           now_ms() - s_seek_done_ms < 1500;
}


int prospero_request_inplace_seek(
    double target_seconds,
    int restore_paused
) {
#ifdef NUVIO_APP
    /* Nuvio streams are provider sources, which keep current_media_path empty
     * (no sidecar scan, no favourites); they seek like any open file. */
    if (!play_fmt || (video_stream_index < 0 && audio_stream_index < 0))
        return 0;
#else
    if (
        !play_fmt ||
        (video_stream_index < 0 && audio_stream_index < 0) ||
        !current_media_path[0]
    ) {
        return 0;
    }
#endif

    if (target_seconds < 0.0) {
        target_seconds = 0.0;
    }

    if (media_duration_sec > 1.0) {
        double maximum =
            media_duration_sec - 1.0;

        if (maximum < 0.0) {
            maximum = 0.0;
        }

        if (target_seconds > maximum) {
            target_seconds = maximum;
        }
    }

    player_paused = 1;
    controls_last_used_ms = now_ms();

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    prospero_seek_target_seconds =
        target_seconds;

    prospero_seek_restore_paused =
        restore_paused;

    prospero_seek_pending = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    return 1;
}




static int prospero_process_seek_request(void) {
    double target_seconds;
    int restore_paused;

    pthread_mutex_lock(
        &prospero_seek_mutex
    );

    if (!prospero_seek_pending) {
        pthread_mutex_unlock(
            &prospero_seek_mutex
        );

        return 0;
    }

    target_seconds =
        prospero_seek_target_seconds;

    restore_paused =
        prospero_seek_restore_paused;

    prospero_seek_pending = 0;
    prospero_seek_in_progress = 1;

    pthread_mutex_unlock(
        &prospero_seek_mutex
    );

    pp_playback_notify_seek_begin(
        &g_pp_pb,
        (int64_t)(target_seconds * 1000000.0)
    );

    /*
     * Wait for the video decode thread to be out of the decoder before
     * anything below touches it: evo_vdec_flush() and the pending-packet free
     * both race a decode call still in flight. The old fixed 5 ms was enough
     * while a decode call was short; a 4K AV1 frame in dav1d is not, and a
     * .mkv seek (av_seek_frame ~0 ms) flushed dav1d under a live
     * dav1d_send_data - SIGSEGV in dav1d_parse_obus (#94, hardware
     * 2026-09-26). Bounded, so a wedged decode (#39) cannot hang the seek.
     *
     * video_decode_hold, not player_paused: a committed scrub requests the
     * seek and then moves the FSM straight to Playing, whose entry clears
     * player_paused before this thread even gets here - so the decode thread
     * never parked and every seek sat out the full 2 s timeout, then flushed
     * unsynchronized anyway. The hold belongs to the seek alone.
     */
    video_decode_hold = 1;
    usleep(5000);
    if (video_thread_running) {
        int waited_ms = 0;
        while (!video_decode_parked && waited_ms < 2000) {
            usleep(1000);
            waited_ms++;
        }
        if (!video_decode_parked || waited_ms > 50) {
            char d[64];
            snprintf(d, sizeof d, "parked=%d waited_ms=%d",
                     (int)video_decode_parked, waited_ms);
            pp_stage_bc("SEEK_PARK", d);
        }
    }

    
    /*
     * Clear EOF immediately. This wakes the video and audio decoder
     * loops while the seek and queue reset are being completed.
     */
    video_decode_done = 0;
    video_decode_ready = 1;
    dbg_read_fail = 0;

packet_queue_clear(
        &video_packet_queue
    );

    packet_queue_clear(
        &audio_packet_queue
    );

    if (video_pending_pkt) {
        av_packet_free(
            &video_pending_pkt
        );

        video_pending_pkt = NULL;
    }

    if (video_video_pending_pkt) {
        av_packet_free(
            &video_video_pending_pkt
        );

        video_video_pending_pkt = NULL;
    }

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    double decoder_seek_seconds =
        target_seconds;

    /*
     * No extra backstep. AVSEEK_FLAG_BACKWARD already lands on the keyframe at
     * or before this timestamp, which is exactly what an inter-frame codec
     * needs to restart. The 0.5 s that used to be subtracted here only widened
     * the run-up the decoder then has to chew through and throw away - and
     * when the target sat just after a keyframe it pushed the seek back a
     * whole extra GOP, which is what made a longer seek hitch harder than a
     * short one.
     */

    int seek_stream =
        video_stream_index >= 0
            ? video_stream_index
            : audio_stream_index;

    AVRational time_base =
        play_fmt->streams[
            seek_stream
        ]->time_base;

    int64_t seek_timestamp =
        (int64_t)(
            decoder_seek_seconds /
            av_q2d(time_base)
        );

    /* #94: a seek in a raw .obu (no index - the demuxer scans forward from
     * the last keyframe it has seen) took EVO down with nothing after it in
     * evo.log. This line and the ms= on SEEK_AVFRAME bracket the call. */
    {
        char d[112];
        snprintf(d, sizeof d, "fmt=%s ts=%lld target=%.3f",
                 play_fmt->iformat ? play_fmt->iformat->name : "?",
                 (long long)seek_timestamp, target_seconds);
        pp_stage_bc("SEEK_BEGIN", d);
    }
    const int64_t seek_t0 = av_gettime_relative();

    int result =
        av_seek_frame(
            play_fmt,
            seek_stream,
            seek_timestamp,
            AVSEEK_FLAG_BACKWARD
        );

    /* Big files (14 GB GTA trailer) whose stream index doesn't cover the byte
     * range can fail the timestamp seek; fall back to a byte seek. */
    if (result < 0) {
        result = av_seek_frame(play_fmt, seek_stream, seek_timestamp,
                               AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    }
    {
        char d[112];
        snprintf(d, sizeof d, "rc=%d ts=%lld strm=%d target=%.3f ms=%lld",
                 result, (long long)seek_timestamp, seek_stream, target_seconds,
                 (long long)((av_gettime_relative() - seek_t0) / 1000));
        pp_stage_bc("SEEK_AVFRAME", d);   /* #32 diagnostics -> /mnt/usb0/evo.log */
    }

    if (result >= 0) {
        /*
         * av_seek_frame() already flushes the demuxer before it repositions,
         * then sets the stream's running dts to the timestamp it landed on.
         * Flushing again here resets that dts. A container stamps its own
         * packets so it never mattered - but a stream with no timestamps (raw
         * .obu, AVFMT_NOTIMESTAMPS) has only that dts, and after the extra
         * flush restarted at 0: every frame then read as before the target
         * and the whole seek was decoded and thrown away in the dark (#94,
         * Chimera: a jump to 118 s discarded 1005 frames and never played).
         */
        if (!(play_fmt->iformat->flags & AVFMT_NOTIMESTAMPS))
            avformat_flush(play_fmt);

        evo_vdec_flush(g_vdec);   /* video codec + scratch frame/packet (A6) */
        if (g_adec) {
            evo_adec_flush(g_adec);
        }

        if (audio_ctx) {
            avcodec_flush_buffers(
                audio_ctx
            );
        }

        prospero_audio_resampler_reset();

        if (
            prospero_embedded_subtitle_ctx
        ) {
            avcodec_flush_buffers(
                prospero_embedded_subtitle_ctx
            );
        }

        prospero_embedded_subtitle_reset();
#ifdef NUVIO_APP
        nuvio_subs_on_seek();
#endif

        /*
         * The UI position is base offset plus the new audio clock.
         */
        resume_base_offset_seconds =
            target_seconds;
        resume_base_anchor_pending = -1.0;

        /*
         * Arm the audio discard window before the decode threads are let go,
         * so the run-up between the keyframe this seek landed on and the
         * target is dropped on the audio side too. Both clocks then restart
         * from the target and the picture resumes without waiting for audio.
         */
        if (target_seconds > 0.05)
            audio_seek_discard_until = target_seconds;
        else
            audio_seek_discard_until = -1.0;

        audio_samples_played = 0;
        audio_samples_decoded = 0;

        audio_clock_seconds = 0.0;
        audio_pts_seconds = 0.0;
        video_clock_seconds = 0.0;

        first_audio_pts_seconds = -1.0;
        first_video_pts_seconds = -1.0;

        video_decode_done = 0;
        dbg_read_fail = 0;

        /*
         * Older builds allowed the audio decoder thread to exit at
         * EOF. Restart it if this session reached EOF before seeking.
         */
        if (
            audio_ctx &&
            !audio_decode_thread_running
        ) {
            audio_decode_thread_running = 1;

            pthread_create(
                &audio_decode_thread,
                NULL,
                audio_decode_thread_func,
                NULL
            );
        }

    } else {
        audio_seek_discard_until = -1.0;
        toast(
            "SEEK",
            "Decoder seek failed"
        );
    }

    prospero_seek_in_progress = 0;
    s_seek_done_ms = now_ms();
    video_decode_hold = 0;

    pp_playback_notify_seek_end(
        &g_pp_pb,
        result >= 0,
        0,
        0
    );
    /* notify_seek_begin() paused the clock. Always lift that if we were
     * playing — on a FAILED seek notify_seek_end() only clears seek_discarding
     * and leaves the clock paused, which drops every frame -> frozen picture. */
    if (!restore_paused)
        pp_playback_resume(&g_pp_pb);

    player_paused =
        restore_paused ? 1 : 0;

    controls_last_used_ms =
        now_ms();

    return result >= 0;
}


/* ---------------------------------------------------------------------------
 * Read-ahead budget.
 *
 * A queue is full by the playback time it holds, not by its packet count.
 * Packet rates run from 24/s (film video) to ~250/s (TrueHD in Matroska), so
 * any one count cap suits one stream and throttles the other: EVO's 96/96
 * held a TrueHD remux to under two seconds of video read-ahead, because the
 * audio queue capped out first and av_read_frame() hands packets back in
 * interleave order. An internet stream with two seconds of cushion stalls on
 * every dip in throughput.
 *
 * Bytes bound it as well, for the stream whose bitrate makes thirty seconds
 * expensive (a 4K remux peaks near 100 Mbit/s). The packets themselves come
 * from direct memory - see the malloc shim's DIRECT_FIRST_MIN in
 * scripts/build.sh - so the flexible pool the decoders live in is not what
 * pays for it.
 * ------------------------------------------------------------------------ */
#define READAHEAD_NET_US     (30LL * 1000000)  /* internet / LAN stream */
#define READAHEAD_LOCAL_US   (8LL * 1000000)   /* file: reads far faster than real time */
#define VIDEO_RING_BYTES     ((size_t)512 << 20)
#define AUDIO_RING_BYTES     ((size_t)96 << 20)
#define VIDEO_BYTES_CAP      (384LL << 20)
#define AUDIO_BYTES_CAP      (64LL << 20)
/* No ring (direct memory refused): the packets stay in the demuxer's own
 * buffers, in flexible memory, which the decoders need most of. */
#define NO_RING_BYTES_CAP    (48LL << 20)
#define QUEUE_FLOOR_PACKETS  16                /* always room for this many */

/* Pre-buffer target at open: enough to absorb the first second of TCP ramp. */
#define PREBUFFER_OPEN_US    (2LL * 1000000)

static int64_t s_readahead_us = READAHEAD_LOCAL_US;
static int64_t s_video_bytes_cap = NO_RING_BYTES_CAP;
static int64_t s_audio_bytes_cap = NO_RING_BYTES_CAP;

/*
 * Low-water mark on the OTHER stream's queue. Below this, that decoder is
 * within a fraction of a second of running dry and the demux thread must not
 * stay parked on a full queue - see demux_wait_for_room().
 */
#define DEMUX_STARVE_LOW_PACKETS 12

/* What the current pre-buffer hold is waiting for. Written before the hold is
 * raised (evo_demux_rebuffer), read by the demux thread. */
static volatile long long s_prebuffer_deadline_ms = 0;
static volatile int64_t   s_prebuffer_target_us   = PREBUFFER_OPEN_US;

/*
 * At or over its budget. `scale` > 1 is the bounded overshoot allowed while
 * the other stream starves (demux_wait_for_room). Durations come from the
 * demuxer; a stream that never supplies them falls back to the packet caps.
 */
static int queue_full(PacketQueue *q, int is_video, int scale)
{
    int     n     = 0;
    int64_t dur   = 0;
    int64_t bytes = 0;
    packet_queue_level(q, &n, &dur, &bytes);

    if (n >= PACKET_QUEUE_SIZE - 1)
        return 1;
    /* Bytes overshoot by a quarter at most: the ring has that and no more. */
    const int64_t bcap = is_video ? s_video_bytes_cap : s_audio_bytes_cap;
    if (bytes >= bcap + (scale > 1 ? bcap / 4 : 0))
        return 1;
    if (n < QUEUE_FLOOR_PACKETS)
        return 0;
    if (dur > 0)
        return dur >= s_readahead_us * scale;
    return n >= (is_video ? video_packet_cap : audio_packet_cap) * scale;
}

/* Under half a second left, or nearly no packets: that decoder is about to
 * run dry and only the demux thread can feed it. */
static int queue_starving(PacketQueue *q)
{
    int     n   = 0;
    int64_t dur = 0;
    packet_queue_level(q, &n, &dur, NULL);
    return n < DEMUX_STARVE_LOW_PACKETS || (dur > 0 && dur < 500000);
}

/* Playback time a packet carries, in microseconds. The container's duration
 * when it has one; otherwise the stream's average packet spacing, measured
 * from successive timestamps (TrueHD in Matroska carries no durations, and
 * laced frames carry no timestamps either), with the frame rate as video's
 * first guess. 0 means unknown yet and only the byte cap applies. */
static int64_t packet_dur_us(const AVPacket *pkt)
{
    static struct { int64_t last_pts; int since; int64_t avg_us; } s_rate[2] = {
        { AV_NOPTS_VALUE, 0, 0 }, { AV_NOPTS_VALUE, 0, 0 } };
    const int k = (pkt->stream_index == video_stream_index) ? 0 : 1;
    AVStream *st = play_fmt->streams[pkt->stream_index];

    if (pkt->pts != AV_NOPTS_VALUE) {
        if (s_rate[k].last_pts != AV_NOPTS_VALUE && s_rate[k].since > 0) {
            int64_t d = av_rescale_q(pkt->pts - s_rate[k].last_pts, st->time_base,
                                     AV_TIME_BASE_Q);
            /* A seek or a discontinuity jumps; only steady spacing counts. */
            if (d > 0 && d < 2000000)
                s_rate[k].avg_us = d / s_rate[k].since;
        }
        s_rate[k].last_pts = pkt->pts;
        s_rate[k].since = 0;
    }
    s_rate[k].since++;

    if (pkt->duration > 0) {
        int64_t us = av_rescale_q(pkt->duration, st->time_base, AV_TIME_BASE_Q);
        if (us > 0 && us < 10 * 1000000)
            return us;
    }
    if (k == 0) {
        AVRational r = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        if (r.num > 0 && r.den > 0)
            return av_rescale(1000000, r.den, r.num);
    }
    return s_rate[k].avg_us;
}

/*
 * Release the pre-buffer hold once the queue has a cushion, the deadline has
 * passed, or the stream ended. Called from the demux loop after each packet.
 * `ended` is set on a read failure, where waiting for depth that will never
 * arrive would park the decode threads for the whole deadline.
 */
static void prebuffer_check(int ended)
{
    if (!pb_prebuffer_hold)
        return;

    /* An audio-only stream never fills the video queue, so measure whichever
     * queue this stream actually feeds. */
    const int have_video = (video_stream_index >= 0);
    PacketQueue *q = have_video ? &video_packet_queue : &audio_packet_queue;
    int     depth = 0;
    int64_t held  = 0;
    packet_queue_level(q, &depth, &held, NULL);

    const int filled = (held > 0) ? (held >= s_prebuffer_target_us)
                                  : (depth >= pb_prebuffer_packets);

    /*
     * A queue at its budget is as much cushion as this stream will ever get,
     * so release on that too rather than sitting out the deadline.
     */
    const int any_full = queue_full(&video_packet_queue, 1, 1) ||
                         queue_full(&audio_packet_queue, 0, 1);

    const int timed_out = (now_ms() >= s_prebuffer_deadline_ms);
    if (!ended && !timed_out && !any_full && !filled)
        return;

    pb_prebuffer_hold = 0;
    {
        char d[80];
        snprintf(d, sizeof d, "%s packets=%d held_ms=%lld target_ms=%lld",
                 ended     ? "ended"
                 : timed_out ? "deadline"
                 : any_full  ? "queue-full"
                             : "filled",
                 depth, (long long)(held / 1000),
                 (long long)(s_prebuffer_target_us / 1000));
        pp_stage_bc("P8_03_PREBUFFER_DONE", d);
    }
}

/* Re-arm the pre-buffer mid-play: the decoders park, the clocks hold, and the
 * demux thread refills `target_us` of video (or gives up at `max_ms`) before
 * playback carries on - one clean pause instead of a stutter per frame. */
void evo_demux_rebuffer(int64_t target_us, int max_ms)
{
    if (target_us > s_readahead_us - 1000000)
        target_us = s_readahead_us - 1000000;
    s_prebuffer_target_us   = target_us;
    s_prebuffer_deadline_ms = now_ms() + max_ms;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    pb_prebuffer_hold = 1;
    {
        char d[48];
        snprintf(d, sizeof d, "target_ms=%lld", (long long)(target_us / 1000));
        pp_stage_bc("P8_03_REBUFFER", d);
    }
}

/* 0..1 toward the pre-buffer target, for the loading screen. */
float evo_demux_prebuffer_progress(void)
{
    PacketQueue *q = (video_stream_index >= 0) ? &video_packet_queue
                                               : &audio_packet_queue;
    int     n    = 0;
    int64_t held = 0;
    packet_queue_level(q, &n, &held, NULL);
    float p = (held > 0 && s_prebuffer_target_us > 0)
                  ? (float)held / (float)s_prebuffer_target_us
                  : (pb_prebuffer_packets > 0 ? (float)n / (float)pb_prebuffer_packets : 0.0f);
    return p > 1.0f ? 1.0f : p;
}

/* Seconds of playback held in the video (or, audio-only, audio) queue. */
double evo_demux_buffered_s(void)
{
    PacketQueue *q = (video_stream_index >= 0) ? &video_packet_queue
                                               : &audio_packet_queue;
    int64_t held = 0;
    packet_queue_level(q, NULL, &held, NULL);
    return (double)held / 1000000.0;
}

/*
 * Hard ceiling on how far a queue may overshoot its budget while the other
 * stream is starving. The budget is a memory guard, not a correctness
 * invariant, and PACKET_QUEUE_SIZE is the real limit - packet_queue_push()
 * simply returns 0 there, so overshooting can never corrupt the ring.
 */
#define DEMUX_OVERSHOOT_FACTOR 2

/*
 * Wait for room in `q`, but never at the cost of starving the other stream.
 *
 * av_read_frame() hands packets back in interleave order, so parking here on a
 * full queue also stops the OTHER stream's packets arriving. That is one leg of
 * a four-way deadlock hit on hardware after seeking into 4K HEVC + E-AC-3
 * (2026-09-28, Avatar UHD remux):
 *
 *   audio output parks because the audio clock is >0.5 s ahead of video
 *     -> the decoded-PCM ring stays above its high-water mark
 *     -> the audio decode thread stops popping packets
 *     -> the audio packet queue caps out
 *     -> the demux thread parks HERE
 *     -> the video packet queue drains to empty
 *     -> video stops decoding, so video_clock_seconds stops advancing
 *     -> audio output's "ahead of video" test is now permanently true.
 *
 * Releasing as soon as the other queue runs dry cuts that cycle: the queue in
 * hand overshoots its budget by a bounded amount instead, which costs memory
 * and keeps both decoders fed.
 */
static void demux_wait_for_room(PacketQueue *q, int is_video, PacketQueue *other,
                                int sleep_us)
{
    /* Not released by a pause: the packet in hand would be dropped, and a
     * lost reference frame smears the picture until the next keyframe. */
    while (demux_thread_running &&
           queue_full(q, is_video, 1)) {
        evo_demux_state = is_video ? 2 : 3;
        /* Still re-check the pre-buffer here: this loop does not return to the
         * top of the demux loop, so it is the only place the deadline can fire
         * once a queue is full. */
        prebuffer_check(0);

        /* A seek is waiting: return so the loop can run it. */
        if (prospero_seek_pending)
            return;

        if (other && queue_starving(other) && !queue_full(q, is_video, DEMUX_OVERSHOOT_FACTOR)) {
            /* Rate-limited: one line per 2 s is enough to tell a starved
             * interleave apart from a healthy one in evo.log. */
            static long long s_last_bc_ms = 0;
            long long now = now_ms();
            if (now - s_last_bc_ms >= 2000) {
                char d[80];
                snprintf(d, sizeof d, "q=%d other=%d video=%d",
                         packet_queue_count(q), packet_queue_count(other), is_video);
                pp_stage_bc("DEMUX_OVERSHOOT", d);
                s_last_bc_ms = now;
            }
            return;
        }

        usleep(sleep_us);
    }
}

void *demux_thread_func(void *arg) {
    (void)arg;

    AVPacket *pkt =
        av_packet_alloc();

    if (!pkt) {
        /* Nothing will ever fill the queue, so do not leave the decode
         * threads parked on a hold that can no longer be cleared. */
        pb_prebuffer_hold = 0;
        return NULL;
    }

    /* Deadline for the pre-buffer, measured from when this thread actually
     * starts reading rather than from when it was created. */
    s_prebuffer_target_us   = PREBUFFER_OPEN_US;
    s_prebuffer_deadline_ms = now_ms() + (long long)pb_prebuffer_max_ms;

    {
        const char *u = (play_fmt && play_fmt->url) ? play_fmt->url : "";
        const int net = strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0 ||
                        strncmp(u, "ftp://", 6) == 0 || strncmp(u, "smb://", 6) == 0;
        s_readahead_us = net ? READAHEAD_NET_US : READAHEAD_LOCAL_US;
    }
    /* Budget by what the rings actually got: cap plus the quarter of
     * overshoot must still fit, or the packets past it fall back to the
     * plain allocator. */
    if (packet_queue_use_ring(&video_packet_queue, VIDEO_RING_BYTES)) {
        int64_t cap = (int64_t)packet_queue_ring_size(&video_packet_queue) * 3 / 4;
        s_video_bytes_cap = cap < VIDEO_BYTES_CAP ? cap : VIDEO_BYTES_CAP;
    } else {
        s_video_bytes_cap = NO_RING_BYTES_CAP;
    }
    if (packet_queue_use_ring(&audio_packet_queue, AUDIO_RING_BYTES)) {
        int64_t cap = (int64_t)packet_queue_ring_size(&audio_packet_queue) * 3 / 4;
        s_audio_bytes_cap = cap < AUDIO_BYTES_CAP ? cap : AUDIO_BYTES_CAP;
    } else {
        s_audio_bytes_cap = NO_RING_BYTES_CAP;
    }

    while (demux_thread_running) {
        /*
         * Process seek requests before checking the paused state.
         */
        if (prospero_seek_pending)
            evo_demux_state = 4;
        if (prospero_process_seek_request()) {
            av_packet_unref(pkt);
            continue;
        }

        /* Paused or not, keep reading up to the budget: a pause is the
         * cheapest time to build cushion on a network stream, and the queue
         * budget (demux_wait_for_room) bounds it either way. */

        evo_demux_state = 1;
        int read_result =
            av_read_frame(
                play_fmt,
                pkt
            );

        if (read_result < 0) {
            /*
             * Keep the demux thread alive so seeking backward from EOF
             * does not require reopening the file.
             */
            video_decode_done = 1;
            evo_demux_state = 5;
            prebuffer_check(1);
            usleep(5000);
            continue;
        }

        video_decode_done = 0;
        prebuffer_check(0);

        if (
            pkt->stream_index ==
            video_stream_index
        ) {
            /*
             * Do not drop non-keyframes in demux — that freezes for a full GOP
             * (often every 1–2s). Cap queue by waiting only.
             */
            demux_wait_for_room(&video_packet_queue, 1, &audio_packet_queue,
                                playback_profile >= 3 ? 300 : 500);

            if (demux_thread_running) {
                packet_queue_push_timed(&video_packet_queue, pkt,
                                        packet_dur_us(pkt));
                dbg_video_packets++;
            }
        } else if (
            pkt->stream_index ==
            audio_stream_index
        ) {
            demux_wait_for_room(&audio_packet_queue, 0, &video_packet_queue, 1000);

            if (demux_thread_running)
                packet_queue_push_timed(&audio_packet_queue, pkt,
                                        packet_dur_us(pkt));
        }

#ifdef NUVIO_APP
        /* The Nuvio Player renders subtitles itself (src/nuvio_subs.c). */
        nuvio_subs_on_packet(play_fmt, pkt);
        if (0) {
#else
        if (
            prospero_subtitle_wants_stream(
                pkt->stream_index
            )
        ) {
#endif
            if (
                pkt->stream_index ==
                prospero_embedded_subtitle_stream_index
            ) {
                dbg_sub_demuxed++;
            }

            prospero_embedded_subtitle_decode_packet(
                pkt
            );
        }

        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    return NULL;
}

