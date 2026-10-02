#include "evo/services/PlaybackController.hpp"
extern "C" {
#include <libavutil/cpu.h>
}
#include "evo/interfaces/IMediaMetadataService.hpp"
#include "evo/Application.hpp"

#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_demux.h"
#include "evo_audio_out.h"
#include "evo_audio_resample.h"
#include "evo_packet_queue.h"
#include "evo_thread.h"
#include "evo_vdec.h"
#include "evo_sweep.h"
#include "evo_direct_mem.h"   /* evo_mem_budget_log - the numbers behind the 1080p rule */

/*
 * malloc_shim.c (tools/native-app/stubs). Weak so the non-app-module compile
 * check, which never links the shim, still builds - there the calls no-op.
 */
extern "C" {
__attribute__((weak)) void evo_alloc_stats(uint64_t *live, uint64_t *peak, uint64_t *large_n);
__attribute__((weak)) void evo_alloc_map_info(uint64_t *fails, uint64_t *served_flex,
                                              uint64_t *served_anon, uint64_t *flex_avail);
__attribute__((weak)) void evo_alloc_direct_info(uint64_t *served, uint64_t *live,
                                                 uint64_t *peak);
}

/*
 * The heap as the malloc shim sees it. map_fail > 0 means an allocation was
 * refused outright - on 2026-09-25 that was the whole 4K AV1 failure, not
 * FFmpeg. direct_* is the shim's last-resort fallback (#94): nonzero means
 * flexible memory ran dry and direct memory carried the rest.
 */
#include "evo_boot_log.h"
/* Not static: Application's render loop also samples it during playback, so a
 * run that crashes still leaves the heap's last state in evo.log. */
extern "C" void evo_log_alloc_state(const char *when)
{
    uint64_t live = 0, peak = 0, large_n = 0;
    uint64_t fails = 0, flex = 0, anon = 0, avail = 0;
    uint64_t dserved = 0, dlive = 0, dpeak = 0;
    if (evo_alloc_stats)       evo_alloc_stats(&live, &peak, &large_n);
    if (evo_alloc_map_info)    evo_alloc_map_info(&fails, &flex, &anon, &avail);
    if (evo_alloc_direct_info) evo_alloc_direct_info(&dserved, &dlive, &dpeak);
    evo_boot_log("  alloc [%s] live=%lluMB peak=%lluMB large=%llu "
                 "map_fail=%llu flex=%llu anon=%llu flex_avail=%lluMB "
                 "direct=%llu direct_live=%lluMB direct_peak=%lluMB",
                 when,
                 (unsigned long long)(live >> 20),
                 (unsigned long long)(peak >> 20),
                 (unsigned long long)large_n,
                 (unsigned long long)fails,
                 (unsigned long long)flex,
                 (unsigned long long)anon,
                 (unsigned long long)(avail >> 20),
                 (unsigned long long)dserved,
                 (unsigned long long)(dlive >> 20),
                 (unsigned long long)(dpeak >> 20));
}

#include "evo_adec.h"
#include "evo_subtitle.h"
#ifdef NUVIO_APP
#include "nuvio_subs.h"
#include "dv_rpu.h"
#include "evo_agc_runtime.h"
extern "C" {
#include <libavutil/dovi_meta.h>
}
extern "C" int nuvio_pick_audio_stream(struct AVFormatContext *fmt, int current);
#endif
#include "evo_stream_io.h"
#include "pp_stage_breadcrumb.h"
/* #90: the provider seam. Needed for report_progress from saveResumePosition
 * and for the resolver chain's stream choices; no provider-specific header is
 * included here, which is the point of the vtable. */
#include "evo_provider.h"
#include "evo_recent.h"
#include "evo_toast.h"
#include "evo_data_path.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "prospero_thumbnail.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>

int sceAudioOutInit(void);
int sceAudioOutOpen(int userId, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
#ifdef NUVIO_APP
extern "C" char nuvio_vdec_conf[512];   /* test switches, see evo_vdec_native.c */
#endif
int sceAudioOutClose(int handle);
}

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cstdlib>

// Global playback instance
extern pp_playback g_pp_pb;
extern AVFormatContext *play_fmt;
extern AVCodecContext *audio_ctx;
extern evo_vdec *g_vdec;
extern int g_vdec_force_ffmpeg;
extern AVPacket *video_pending_pkt;
extern struct SwsContext *play_sws;
extern int screen;
extern int player_paused;
extern double media_duration_sec;
extern double resume_base_offset_seconds;
extern volatile double resume_base_anchor_pending;   /* evo_audio_out.c */
extern double requested_resume_seek_pos;
extern char current_media_path[768];
extern int evo_audio_channels;
/* Start-of-stream pre-buffer hold - see the note in Bridge.cpp. */
extern volatile int pb_prebuffer_hold;
extern volatile int pb_scrub_hold;

namespace evo {

/*
 * Ceiling on avformat_find_stream_info(). It has to clear the slowest probe we
 * expect to be worth waiting for - a long-GOP MPEG-TS over USB, where the 4 s
 * analyzeduration plus read time is the honest cost of finding a late audio
 * track - while still being short enough that a demuxer scanning a REMUX for a
 * Cluster it will not find gives up before the user does.
 */
static constexpr double kProbeDeadlineSeconds = 12.0;

#ifdef EVO_APP_MODULE
#  define SIO_STAGE(id, d) pp_stage_bc((id), (d))
#else
#  define SIO_STAGE(id, d) ((void)0)
#endif

/*
 * Pixel count of a video stream, or 0 if it is not one we could play.
 *
 * A stream whose codec parameters never resolved scores 0 rather than being
 * treated as a tiny one: an HLS master playlist can carry a variant that
 * find_stream_info could not pin down, and opening a decoder on it gives
 * width=height=0, which makes av_guess_frame_rate fall back to the 90 kHz
 * time base and leaves the pacer with a 0.01 ms frame budget.
 */
static long long videoStreamPixels(const AVStream* st) {
    if (!st || !st->codecpar) return 0;
    const AVCodecParameters* cp = st->codecpar;
    if (cp->codec_type != AVMEDIA_TYPE_VIDEO) return 0;
    if (cp->codec_id == AV_CODEC_ID_NONE)     return 0;
    if (cp->width <= 0 || cp->height <= 0)    return 0;
    /*
     * Cover art is a video stream with a width and a height, and album art is
     * routinely larger than the picture in a low-bitrate file. Scoring on
     * pixels would hand it the win and play a still image over the movie -
     * which "first video stream wins" happened not to do, since the art is
     * usually last. Attached pictures are never the thing to play.
     */
    if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) return 0;
    return static_cast<long long>(cp->width) * cp->height;
}

/* Index into fmt->programs of the program carrying `stream_index`, or -1. */
static int programOfStream(const AVFormatContext* fmt, int stream_index) {
    if (!fmt || stream_index < 0) return -1;
    for (unsigned p = 0; p < fmt->nb_programs; ++p) {
        const AVProgram* prog = fmt->programs[p];
        if (!prog) continue;
        for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
            if (prog->stream_index[k] == static_cast<unsigned>(stream_index))
                return static_cast<int>(p);
        }
    }
    return -1;
}

static bool streamInProgram(const AVFormatContext* fmt, int prog_idx, int stream_index) {
    if (!fmt || prog_idx < 0 || prog_idx >= static_cast<int>(fmt->nb_programs))
        return false;
    const AVProgram* prog = fmt->programs[prog_idx];
    if (!prog) return false;
    for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
        if (prog->stream_index[k] == static_cast<unsigned>(stream_index))
            return true;
    }
    return false;
}

static uint64_t GetCurrentTimeMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<uint64_t>(tv.tv_sec) * 1000ULL + static_cast<uint64_t>(tv.tv_usec / 1000ULL);
}

PlaybackController::PlaybackController()
    : m_playbackFsm(PlaybackState::Stopped, "PlaybackFSM")
{
    initStateMachine();
}

PlaybackController::~PlaybackController() {
    stopPlayback();
}

void PlaybackController::initStateMachine() {
    m_playbackFsm
        .addState(PlaybackState::Stopped, "Stopped")
        .addState(PlaybackState::Opening, "Opening")
        .addState(PlaybackState::Playing, "Playing",
            []() {
                player_paused = 0;
                pp_playback_resume(&g_pp_pb);
            })
        .addState(PlaybackState::Paused, "Paused",
            []() {
                player_paused = 1;
                pp_playback_pause(&g_pp_pb);
            })
        .addState(PlaybackState::Scrubbing, "Scrubbing")
        .addState(PlaybackState::Seeking, "Seeking")
        .addState(PlaybackState::Finished, "Finished")
        .addState(PlaybackState::Error, "Error");

    // From Stopped
    m_playbackFsm
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Stopped, PlaybackEvent::Play, PlaybackState::Playing);

    // From Opening
    m_playbackFsm
        .addTransition(PlaybackState::Opening, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Fail, PlaybackState::Error)
        .addTransition(PlaybackState::Opening, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Playing
    m_playbackFsm
        .addTransition(PlaybackState::Playing, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Playing, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Finish, PlaybackState::Finished)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Playing, PlaybackEvent::Fail, PlaybackState::Error);

    // From Paused
    m_playbackFsm
        .addTransition(PlaybackState::Paused, PlaybackEvent::Resume, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::StartScrub, PlaybackState::Scrubbing)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Seek, PlaybackState::Seeking)
        .addTransition(PlaybackState::Paused, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Scrubbing
    m_playbackFsm
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::ConfirmScrub, PlaybackState::Seeking)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::CancelScrub, PlaybackState::Playing)
        .addTransition(PlaybackState::Scrubbing, PlaybackEvent::Stop, PlaybackState::Stopped);

    // From Seeking
    m_playbackFsm
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Play, PlaybackState::Playing)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Pause, PlaybackState::Paused)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Seeking, PlaybackEvent::Fail, PlaybackState::Error);

    // From Finished / Error
    m_playbackFsm
        .addTransition(PlaybackState::Finished, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Finished, PlaybackEvent::Stop, PlaybackState::Stopped)
        .addTransition(PlaybackState::Error, PlaybackEvent::Open, PlaybackState::Opening)
        .addTransition(PlaybackState::Error, PlaybackEvent::Stop, PlaybackState::Stopped);
}

bool PlaybackController::isActive() const {
    return evo_pb_is_active() || (video_stream_index >= 0 && video_thread_running) || m_musicMode;
}

bool PlaybackController::isPaused() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Paused;
}

bool PlaybackController::isScrubbing() const {
    return m_playbackFsm.getCurrentState() == PlaybackState::Scrubbing;
}

double PlaybackController::getPositionSeconds() const {
    if (isScrubbing()) {
        return m_scrubTargetSeconds;
    }
    /* The global base, not m_resumeBaseOffset: the resume re-anchor and the
     * demux's in-place seek move it without going through this class. */
    double pos = resume_base_offset_seconds + evo_pb_position_s();
    return (pos < 0.0) ? 0.0 : pos;
}

double PlaybackController::getPercentage() const {
    if (m_durationSeconds <= 0.1) return 0.0;
    double pos = getPositionSeconds();
    double pct = pos / m_durationSeconds;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    return pct;
}

void PlaybackController::togglePause() {
    setPaused(!isPaused());
}

void PlaybackController::setPaused(bool paused) {
    if (paused) {
        m_playbackFsm.postEvent(PlaybackEvent::Pause);
    } else {
        m_playbackFsm.postEvent(PlaybackEvent::Resume);
    }
}

void PlaybackController::stopPlayback() {
    if (isActive()) {
        saveResumePosition();
    }

    /*
     * #90: close the provider's playback session. After saveResumePosition so
     * the final position has already gone out as an UPDATE, and before the
     * source identity is cleared further down.
     */
    if (m_source.isProvider()) {
        const evo_provider_t* prov = evo_provider_find(m_source.provider.c_str());
        if (prov && (prov->caps & EVO_PROVIDER_CAP_PROGRESS) && prov->report_progress)
            prov->report_progress(m_source.item_id.c_str(),
                                  static_cast<int64_t>(getPositionSeconds()),
                                  static_cast<int64_t>(m_durationSeconds),
                                  EVO_PROVIDER_PLAY_STOP);
    }

    m_playbackFsm.postEvent(PlaybackEvent::Stop);
    player_paused = 0;
    resetScrubHold();

    prospero_subtitle_clear();
#ifdef NUVIO_APP
    dv_session_end();
#endif

    if (demux_thread_running) {
        /* A network read can block for rw_timeout and then sit in FFmpeg's
         * reconnect loop; stop must not wait that out. */
        evo_stream_io_abort(m_streamIo);
        demux_thread_running = 0;
        pthread_join(demux_thread, nullptr);
    }

    prospero_embedded_subtitle_close();

    if (video_thread_running) {
        video_thread_running = 0;
        pthread_join(video_thread, nullptr);
    }

    if (audio_decode_thread_running) {
        audio_decode_thread_running = 0;
        pthread_join(audio_decode_thread, nullptr);
    }

    if (audio_thread_running) {
        audio_thread_running = 0;
        pthread_join(audio_thread, nullptr);
    }

    packet_queue_clear(&video_packet_queue);
    packet_queue_clear(&audio_packet_queue);

    audio_queue_count = 0;
    audio_queue_read = 0;
    audio_queue_write = 0;
    audio_accum_pos = 0;

    if (video_pending_pkt) {
        av_packet_free(&video_pending_pkt);
        video_pending_pkt = nullptr;
    }

    /*
     * The decode loop in evo_playback.c holds its half-consumed AU here. Left
     * behind, it is the first thing fed to the *next* file's decoder - a
     * mid-GOP packet from the previous stream.
     */
    if (video_video_pending_pkt) {
        av_packet_free(&video_video_pending_pkt);
        video_video_pending_pkt = nullptr;
    }

    /* Scaler is sized to the closed file's geometry; the next file may differ. */
    if (play_sws) {
        sws_freeContext(play_sws);
        play_sws = nullptr;
    }

    if (audio_handle >= 1) {
        sceAudioOutClose(audio_handle);
        audio_handle = -1;
    }

    if (g_adec) {
        evo_adec_close(g_adec);
        g_adec = nullptr;
    }

    prospero_audio_resampler_destroy();

    if (audio_ctx) {
        avcodec_free_context(&audio_ctx);
        audio_ctx = nullptr;
    }

    if (g_vdec || evo_sweep_active()) {
        pp_playback_stats ps;
        std::memset(&ps, 0, sizeof(ps));
        pp_playback_get_stats(&g_pp_pb, &ps);
        evo_sweep_file_end(g_vdec, &ps);
    }

    if (g_vdec) {
        evo_vdec_close(g_vdec);
        g_vdec = nullptr;
    }

    if (play_fmt) {
        avformat_close_input(&play_fmt);
        play_fmt = nullptr;
    }

    /* Paired with evo_stream_io_open() in startPlaybackSource(). After
     * avformat_close_input, so nothing is still reading through it. */
    if (m_streamIo) {
        evo_stream_io_close(m_streamIo);
        m_streamIo = nullptr;
    }

    video_stream_index = -1;
    audio_stream_index = -1;
    m_durationSeconds = 0.0;
    media_duration_sec = 0.0;
    m_resumeBaseOffset = 0.0;
    resume_base_offset_seconds = 0.0;
    resume_base_anchor_pending = -1.0;
    m_musicMode = false;
    m_currentFilePath.clear();
    m_source = PlaybackSource{};
    m_lastProgressReport = 0.0;

    video_decode_ready = 0;
    video_decode_done = 0;
    video_frame_loaded = 0;

    /* Clear the pre-buffer hold on the way out. A file stopped mid-buffer
     * would otherwise leave it armed, and the next file's decode threads would
     * park on a hold that this file's demux thread is no longer running to
     * clear - a permanent freeze on a source that needs no buffering at all. */
    pb_prebuffer_hold = 0;
    pb_scrub_hold = 0;

    /*
     * Reset the media clocks. These are session globals, not per-file state:
     * carried over, the next file's frames are compared against the previous
     * file's audio clock, so `audio_clock_seconds > video_rel + 0.50` holds
     * forever in audio_output_thread() - the audio queue fills, the decode
     * thread blocks on it, the demuxer blocks on the decode thread, and the
     * second file never plays at all.
     */
    audio_samples_played = 0;
    audio_samples_decoded = 0;
    audio_clock_seconds = 0.0;
    audio_pts_seconds = 0.0;
    video_clock_seconds = 0.0;
    first_audio_pts_seconds = -1.0;
    first_video_pts_seconds = -1.0;
    audio_seek_discard_until = -1.0;

    /* Don't let the next file's scrub previews reuse this file's demuxer. */
    prospero_thumbnail_close_context();

    pp_playback_on_file_close(&g_pp_pb);
    pp_playback_log_stats(&g_pp_pb);
    evo_log_alloc_state("stop");
}

/*
 * #90: the local-file form. Everything a path can tell us is the path, so the
 * source carries only that and the title is derived from the filename
 * downstream, exactly as before.
 */
bool PlaybackController::startPlayback(const std::string& filePath, double resumeOffset) {
    PlaybackSource src;
    src.url = filePath;
    return startPlaybackSource(src, resumeOffset);
}

bool PlaybackController::startPlaybackSource(const PlaybackSource& source,
                                             double resumeOffset) {
    const std::string& filePath = source.url;
    if (filePath.empty()) {
        return false;
    }

    stopPlayback();

    /* A pinned quality belongs to the source it was pinned on. */
    if (m_pinnedVideoFor != filePath) {
        m_pinnedVideoStream = -1;
        m_pinnedVideoFor.clear();
    }

    m_playbackFsm.postEvent(PlaybackEvent::Open);
    m_source = source;
    m_currentFilePath = filePath;
    m_lastProgressReport = 0.0;

    /*
     * current_media_path is read by the subtitle sidecar scan, the demuxer and
     * the favourites toggle, all of which expect a filesystem path. A provider
     * URL is not one - the sidecar scan would stat() nonsense and the
     * favourites toggle would persist a token - so it stays empty for a
     * provider source, which is the "no external subtitles, no favouriting"
     * behaviour those call sites already handle for an empty path.
     */
    if (source.isProvider())
        current_media_path[0] = 0;
    else
        std::snprintf(current_media_path, sizeof(current_media_path), "%s", filePath.c_str());

    /*
     * #90 scope 15: evo_stream_io_open() is the single open path for local and
     * network alike. It had no callers at all - every open went straight to
     * avformat_open_input - which meant the reconnect, reconnect_streamed and
     * timeout options it sets for a network URL existed and were never
     * applied. The timeout is the one that matters most: without it a stalled
     * HTTP open wedges the thread that called it, and on the app slot that is
     * the same class of hang as #39's decode call.
     *
     * For a local file it is the same avformat_open_input with the same
     * probesize plus the sequential read-ahead hint, so local playback is
     * unchanged.
     */
    evo_stream_io_config_t io_cfg;
    std::memset(&io_cfg, 0, sizeof io_cfg);
    io_cfg.ring_buffer_size = 8u * 1024u * 1024u;

    int open_rc = evo_stream_io_open(filePath.c_str(), &play_fmt, &io_cfg, &m_streamIo);
    if (open_rc < 0) {
        m_playbackFsm.postEvent(PlaybackEvent::Fail);
        toast("OPEN FAIL", source.isProvider() ? "Could not open the stream"
                                              : "Could not open media");
        return false;
    }

    /*
     * avformat_find_stream_info() under a deadline, and not fatal if it runs
     * out.
     *
     * Two separate things were wrong here. The call had no bound, so a file
     * that sends the Matroska demuxer into matroska_resync() took the playback
     * thread with it - the 2026-09-28 log ends mid-scan, 32 s and 1.2 GB into
     * a DV P7 UHD REMUX, with no breadcrumb after P8_01e because there was
     * none to write. And the result was treated as pass/fail, when for MKV and
     * MP4 it is neither: the container header already carries codec id,
     * geometry and extradata, and find_stream_info only refines what is
     * mostly there. The poster path proves it on this exact file - it opened
     * the same REMUX, and decoded a 3840x2160 frame from it, seconds earlier.
     *
     * So: bound it, then judge the streams we actually ended up with rather
     * than the return code. A probe that timed out having already identified a
     * decodable video stream costs a frame-rate estimate, not the file.
     */
    SIO_STAGE("P8_02a_PRE_FIND_INFO", filePath.c_str());
    evo_stream_io_set_deadline(m_streamIo, kProbeDeadlineSeconds);
    const int find_rc = avformat_find_stream_info(play_fmt, nullptr);
    evo_stream_io_set_deadline(m_streamIo, 0.0);
    const bool probe_timed_out = evo_stream_io_deadline_expired(m_streamIo) != 0;
    {
        char d[64];
        std::snprintf(d, sizeof d, "rc=%d timeout=%d nb_streams=%u",
                      find_rc, probe_timed_out ? 1 : 0,
                      play_fmt ? play_fmt->nb_streams : 0u);
        SIO_STAGE("P8_02b_FIND_INFO_RC", d);
    }

    /*
     * The deadline is checked as well as the return code, not instead of it:
     * find_stream_info() swallows the AVERROR_EXIT from an interrupted read
     * and still returns the stream count it had managed to gather, so a probe
     * can time out and report success. Both cases leave the demuxer parked
     * wherever the scan stopped, and both need the rewind below.
     */
    /*
     * An open can succeed and still hold nothing. An HLS playlist whose variant
     * playlists the CDN refuses (HTTP 403, a signed link that is not ours, a
     * geo-block) opens with rc=0 - the master playlist itself was fine - and
     * nb_streams=0. Nothing then decodes, no frame ever arrives, and the player
     * sat on its buffering screen until the user backed out: hardware,
     * 2026-09-30, 110 s of video_frames=0 on a channel that could never play.
     * It is a failure like any other, so it takes the failure path below, which
     * also lets the provider try the stream's next choice.
     */
    const bool no_streams = play_fmt && play_fmt->nb_streams == 0;

    if (find_rc < 0 || probe_timed_out || no_streams) {
        /*
         * Usable means: a stream we can hand to a decoder as-is. Geometry and
         * codec id come from the header, so this is satisfied for MKV/MP4 even
         * when the probe never ran to completion. An audio-only file counts -
         * music mode is a supported outcome below.
         */
        bool have_video = false;
        bool have_audio = false;
        for (unsigned int i = 0; play_fmt && i < play_fmt->nb_streams; ++i) {
            const AVStream* st = play_fmt->streams[i];
            if (!st || !st->codecpar) continue;
            const AVCodecParameters* cp = st->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO &&
                cp->codec_id != AV_CODEC_ID_NONE &&
                cp->width > 0 && cp->height > 0) {
                have_video = true;
            } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO &&
                       cp->codec_id != AV_CODEC_ID_NONE) {
                have_audio = true;
            }
        }

        if (!have_video && !have_audio) {
            evo_bt("PlaybackController: find_stream_info failed (rc=%d, timeout=%d) "
                   "and the header named no usable stream - giving up",
                   find_rc, probe_timed_out ? 1 : 0);
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            evo_stream_io_close(m_streamIo);
            m_streamIo = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            toast("STREAM FAIL", probe_timed_out ? "Timed out reading this file"
                                 : no_streams    ? "The server sent no playable stream"
                                                 : "Could not find streams");
            return false;
        }

        evo_bt("PlaybackController: find_stream_info incomplete (rc=%d, timeout=%d) - "
               "continuing on the container header (video=%d audio=%d)",
               find_rc, probe_timed_out ? 1 : 0, (int)have_video, (int)have_audio);

        /*
         * A probe that completes leaves its packets queued for av_read_frame to
         * replay, so the demuxer being parked past them does not matter. One
         * that is interrupted has no such guarantee: it was abandoned wherever
         * the scan had reached - 1.2 GB in, on the file this was written for -
         * and playback would start from there. Put it back on the first frame.
         * A resume offset seeks again below; this is the floor under it.
         */
        const int64_t rewind_to = (play_fmt->start_time != AV_NOPTS_VALUE)
                                      ? play_fmt->start_time : 0;
        const int seek_rc = av_seek_frame(play_fmt, -1, rewind_to, AVSEEK_FLAG_BACKWARD);
        if (seek_rc < 0) {
            evo_bt("PlaybackController: rewind after an incomplete probe failed (rc=%d)",
                   seek_rc);
        }

        if (probe_timed_out)
            toast("SLOW FILE", "Starting without a full scan");
    }

    /*
     * A live stream has no duration and cannot be seeked. Announce it rather
     * than leaving the OSD, the resume store and the #32-era seek/overlay path
     * to work it out from a seek that fails - none of them can.
     */
    if (m_source.isProvider() && !m_source.is_live && play_fmt->duration <= 0)
        m_source.is_live = true;

    if (play_fmt->duration > 0) {
        m_durationSeconds = static_cast<double>(play_fmt->duration) / static_cast<double>(AV_TIME_BASE);
        media_duration_sec = m_durationSeconds;
    }

    // Identify primary video and audio streams
    video_stream_index = -1;
    audio_stream_index = -1;

    /*
     * Pick the BEST video stream, not the first one.
     *
     * An HLS master playlist exposes every bitrate variant as its own streams,
     * and by convention lists the lowest first - so "first video stream wins"
     * played the worst rendition of every live channel. Hardware, 2026-09-28:
     * a 1080p channel opened at 320x180, a 720p one at 640x360, and a
     * 24-stream master whose first variant had no codec parameters at all
     * opened a decoder at 0x0 @ 90000 fps.
     *
     * Score on pixel count, tie-break on bitrate.
     */
    int chosen_program = -1;
    {
        long long best_pixels  = 0;
        int64_t   best_bitrate = -1;

        /*
         * A quality the user pinned wins over the automatic choice, provided it
         * is still a video stream with a size in this open. The audio below
         * follows the program of whatever video is chosen here, so pinning 720p
         * gets the 720p variant's own audio.
         */
        bool pinned = false;
        if (m_pinnedVideoStream >= 0 && m_pinnedVideoFor == filePath &&
            m_pinnedVideoStream < static_cast<int>(play_fmt->nb_streams)) {
            const AVStream* ps = play_fmt->streams[m_pinnedVideoStream];
            if (ps && ps->codecpar && ps->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                videoStreamPixels(ps) > 0) {
                video_stream_index = m_pinnedVideoStream;
                pinned = true;
                evo_bt("PlaybackController: quality pinned to video stream %d", video_stream_index);
            } else {
                evo_bt("PlaybackController: pinned video stream %d is not usable here - "
                       "choosing the best", m_pinnedVideoStream);
            }
        }

        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            if (pinned) break;
            const AVStream* st = play_fmt->streams[i];
            const long long pixels = videoStreamPixels(st);
            if (pixels <= 0) continue;

            const int64_t br = st->codecpar->bit_rate;
            if (pixels > best_pixels || (pixels == best_pixels && br > best_bitrate)) {
                best_pixels        = pixels;
                best_bitrate       = br;
                video_stream_index = static_cast<int>(i);
            }
        }

        /*
         * No video stream resolved its geometry. Fall back to the first one so
         * an awkward container still plays rather than being refused, and say
         * so - this is the shape that produced the 0x0 decoder.
         */
        if (video_stream_index < 0) {
            for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
                const AVStream* st = play_fmt->streams[i];
                if (st && st->codecpar &&
                    st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                    video_stream_index = static_cast<int>(i);
                    evo_bt("PlaybackController: no video stream resolved its size - "
                           "falling back to stream %d", video_stream_index);
                    break;
                }
            }
        }

        chosen_program = programOfStream(play_fmt, video_stream_index);
    }

    /*
     * Audio from the SAME variant as the video where the demuxer groups them
     * into programs. Taking the best video from one rendition and audio from
     * another gives two different encodes of the same channel, which the
     * shared clock cannot reconcile. Decodable-first, exactly as below.
     */
    for (unsigned int i = 0; chosen_program >= 0 && i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar) continue;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        if (!streamInProgram(play_fmt, chosen_program, static_cast<int>(i))) continue;
        if (avcodec_find_decoder(st->codecpar->codec_id)) {
            audio_stream_index = static_cast<int>(i);
            break;
        }
    }

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar) continue;

        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio_stream_index < 0) {
            /*
             * Take the first audio stream we can actually decode, not simply
             * the first one.
             *
             * A Dolby Atmos title normally carries TrueHD first and an E-AC-3
             * track beside it for players that cannot manage TrueHD. Taking
             * stream order on trust meant picking the TrueHD, finding no
             * decoder, and playing nothing at all - not even the video, since
             * the clock waits on an audio stream that never opens - while a
             * decodable track sat one index away. Verified on hardware with
             * "Dolby Atmos TrueHD, E-AC-3 7.1.4.mkv": pos stayed at 0.00.
             *
             * The track-switch path below already checks this; the default
             * did not.
             */
            if (avcodec_find_decoder(st->codecpar->codec_id))
                audio_stream_index = static_cast<int>(i);
        }
    }

    /*
     * Nothing decodable: fall back to the first audio stream anyway, so the
     * file still opens and the track picker can show what is in it. Better a
     * silent file the user can inspect than a refusal with no explanation.
     */
    if (audio_stream_index < 0) {
        for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
            AVStream* st = play_fmt->streams[i];
            if (st && st->codecpar &&
                st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = static_cast<int>(i);
                evo_bt("PlaybackController: no decodable audio stream (codec=%d) - "
                       "playing without sound",
                       (int)st->codecpar->codec_id);
                break;
            }
        }
    }

    /*
     * A track switch asks for a specific stream. Honour it only if it really is
     * a decodable audio stream in this file, so a stale index from a previous
     * file can never leave the session with no audio at all.
     */
#ifdef NUVIO_APP
    /* Nuvio: the viewer's language, and the best track in it (src/nuvio_player.cpp). */
    if (m_requestedAudioStream < 0) {
        const int pick = nuvio_pick_audio_stream(play_fmt, audio_stream_index);
        if (pick >= 0)
            audio_stream_index = pick;
    }
#endif
    if (m_requestedAudioStream >= 0 &&
        m_requestedAudioStream < static_cast<int>(play_fmt->nb_streams)) {
        AVStream* want = play_fmt->streams[m_requestedAudioStream];
        if (want && want->codecpar &&
            want->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
            avcodec_find_decoder(want->codecpar->codec_id)) {
            audio_stream_index = m_requestedAudioStream;
        }
    }
    m_requestedAudioStream = -1;

    /*
     * Stop the demuxer fetching the variants we did not choose.
     *
     * libavformat's hls demuxer downloads a variant's playlist whenever ANY of
     * its streams is still wanted, so leaving the others enabled pulls every
     * rendition of the channel at once - eight copies, on the 24-stream master
     * that prompted this. Whole programs go, not individual streams: one live
     * stream left in a variant keeps that whole variant downloading.
     *
     * Done last, so nothing we actually selected can be discarded - a track
     * switch may legitimately have picked audio from another program.
     */
    if (chosen_program >= 0 && play_fmt->nb_programs > 1) {
        unsigned dropped = 0;
        for (unsigned p = 0; p < play_fmt->nb_programs; ++p) {
            if (static_cast<int>(p) == chosen_program) continue;
            AVProgram* prog = play_fmt->programs[p];
            if (!prog) continue;
            for (unsigned k = 0; k < prog->nb_stream_indexes; ++k) {
                const unsigned si = prog->stream_index[k];
                if (si >= play_fmt->nb_streams) continue;
                if (static_cast<int>(si) == video_stream_index ||
                    static_cast<int>(si) == audio_stream_index) continue;
                play_fmt->streams[si]->discard = AVDISCARD_ALL;
                ++dropped;
            }
        }
        evo_bt("PlaybackController: %u programs, using #%d - discarded %u streams "
               "from the other variants",
               play_fmt->nb_programs, chosen_program, dropped);
    }

    if (video_stream_index >= 0) {
        const AVCodecParameters* vcp = play_fmt->streams[video_stream_index]->codecpar;
        evo_bt("PlaybackController: video stream %d of %u (%dx%d, program %d)",
               video_stream_index, play_fmt->nb_streams,
               vcp ? vcp->width : 0, vcp ? vcp->height : 0, chosen_program);
    }

    m_musicMode = (video_stream_index < 0 && audio_stream_index >= 0);

    // Initialize Video Decoder if video present
    if (video_stream_index >= 0) {
        AVStream* vStream = play_fmt->streams[video_stream_index];

        AVRational fr = av_guess_frame_rate(play_fmt, vStream, nullptr);
        if (fr.num && fr.den)
            video_fps = (double)fr.num / (double)fr.den;
        else
            video_fps = 60.0;

        evo_vdec_open_params vp;
        std::memset(&vp, 0, sizeof(vp));
        /* Settings -> VIDEO DECODER. It was never read here - this always
         * resolved AUTO, so choosing SOFTWARE still decoded on the hardware.
         * The two enums order their values differently; map, don't cast. */
        evo_vdec_pref pref = EVO_VDEC_PREF_AUTO;
        if (ISettingsService *settings = Application::getInstance().getSettingsService()) {
            switch (settings->getVideoDecoderPreference()) {
                case DecoderPreference::FFmpegSoftware: pref = EVO_VDEC_PREF_FFMPEG; break;
                case DecoderPreference::NativeHardware: pref = EVO_VDEC_PREF_NATIVE; break;
                default:                                pref = EVO_VDEC_PREF_AUTO;   break;
            }
        }
        vp.backend = g_vdec_force_ffmpeg
                         ? EVO_VDEC_BACKEND_FFMPEG
                         : evo_vdec_pref_resolve(pref, vStream->codecpar->codec_id);
        vp.codec_id = vStream->codecpar->codec_id;
        vp.width = vStream->codecpar->width;
        vp.height = vStream->codecpar->height;
        vp.avctx_params = vStream->codecpar;
#ifdef NUVIO_APP
        /* Dolby Vision profile 5: collect each frame's RPU from here on so the
         * renderer can rebuild the picture (src/dv_rpu.c). */
        {
            const AVPacketSideData *dsd = av_packet_side_data_get(
                vStream->codecpar->coded_side_data, vStream->codecpar->nb_coded_side_data,
                AV_PKT_DATA_DOVI_CONF);
            const AVDOVIDecoderConfigurationRecord *dcfg =
                dsd ? (const AVDOVIDecoderConfigurationRecord *)dsd->data : nullptr;
            /* Only when this build carries the profile 5 pipelines: without
             * them nothing would use the parsed RPUs. */
            if (dcfg && dcfg->dv_profile == 5 &&
                evo_agc_runtime_pipeline_valid(EVO_AGC_PIPE_VIDEO_DV5))
                dv_session_begin();
            else
                dv_session_end();
        }
#endif
        /*
         * Software decode above 1080p - allowed by default since #95.
         *
         * This used to be refused, and the reason was always memory: first a
         * mis-measured "~180 MB" flexible pool, then - once the real 448 MB was
         * measured - the fear of an out-of-memory mid-playback, which on this
         * platform is a crash, not a stutter. #94 showed that fear was right:
         * 4K 10-bit dav1d drives flexible memory to 0, and the first refused
         * allocation broke FFmpeg's AV1 handling for the rest of the file.
         *
         * It is answered now. The malloc shim takes blocks of 1 MB and up from
         * direct memory first (115d9ff), EVO can hold at least 8 GB of it
         * (boot probe, 2026-09-26), and 4K 10-bit AV1 plays in real time and
         * seeks with flexible memory at 200+ MB free and map_fail=0.
         * docs/hardware/memory-budget.md has the figures.
         *
         * What is left is speed, not memory: a codec the CPU cannot decode in
         * real time at 4K plays slowly. Touch /mnt/usb0/evo_no_sw_4k to go back
         * to refusing - a field escape hatch, read once per session.
         *
         * This only concerns the software path: codecs the hardware cannot do
         * (AV1), or formats sceVideodec2 declines (HEVC 10-bit it will not take)
         * and hands back to FFmpeg - which is why the check also runs again on
         * the chosen backend after the open, below.
         */
        const bool sw_backend = (vp.backend != EVO_VDEC_BACKEND_NATIVE);
        const bool above_1080p = ((long)vp.width * (long)vp.height) > (1920L * 1080L);
        static int sw_4k_allowed = -1;
        if (sw_4k_allowed < 0)
            sw_4k_allowed = (access("/mnt/usb0/evo_no_sw_4k", F_OK) == 0) ? 0 : 1;

        if (sw_backend && above_1080p && sw_4k_allowed) {
            evo_boot_log("PlaybackController: %dx%d on the software decoder",
                         vp.width, vp.height);
            evo_mem_budget_log("sw-4k");
        }

        if (sw_backend && above_1080p && !sw_4k_allowed) {
            evo_boot_log("PlaybackController: refusing %dx%d on the software "
                         "decoder - /mnt/usb0/evo_no_sw_4k is set",
                         vp.width, vp.height);
            evo_mem_budget_log("sw-refuse");
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            toast("UNSUPPORTED", "4K software decode is switched off (evo_no_sw_4k)");
            return false;
        }
        /* Software 4K (an AV1 file, a tiled HEVC remux the hardware will not
         * take) needs every core it can get to hold 24 fps. */
        if (vp.width * vp.height > 1920 * 1088) {
            const int cpus = av_cpu_count();
            vp.thread_count = cpus < 8 ? 8 : (cpus > 16 ? 16 : cpus);
        } else {
            vp.thread_count = 4;
        }
        evo_bt("PlaybackController: software decode threads=%d (cpus=%d)", vp.thread_count, av_cpu_count());
        /* Frame threading for HEVC; slice threading for H.264 to prevent frame worker stack exhaustion */
        vp.thread_type = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC)
                            ? FF_THREAD_FRAME
                            : FF_THREAD_SLICE;
        vp.flag2_fast = (vStream->codecpar->codec_id == AV_CODEC_ID_HEVC);
        vp.skip_loop_filter = EVO_VDEC_KEEP;
        vp.skip_frame = EVO_VDEC_KEEP;
        vp.skip_idct = EVO_VDEC_KEEP;

        evo_vdec_backend chosen = EVO_VDEC_BACKEND_FFMPEG;
        g_vdec = evo_vdec_open(&vp, &chosen);
        /* The native decoder can decline a format it was asked for and hand
         * back FFmpeg - the same software-above-1080p case as above. */
        if (g_vdec && above_1080p && vp.backend == EVO_VDEC_BACKEND_NATIVE &&
            chosen != EVO_VDEC_BACKEND_NATIVE) {
            if (sw_4k_allowed) {
                evo_boot_log("PlaybackController: native declined %dx%d (codec=%d); "
                             "decoding it in software", vp.width, vp.height,
                             vStream->codecpar->codec_id);
                evo_mem_budget_log("native-declined-sw");
            }
        }
        if (g_vdec && above_1080p && chosen != EVO_VDEC_BACKEND_NATIVE && !sw_4k_allowed) {
            evo_boot_log("PlaybackController: native declined %dx%d (codec=%d); "
                         "refusing the FFmpeg fallback - /mnt/usb0/evo_no_sw_4k "
                         "is set", vp.width, vp.height,
                         vStream->codecpar->codec_id);
            evo_mem_budget_log("native-declined");
            evo_sweep_file_failed(filePath.c_str(),
                                  avcodec_get_name(vStream->codecpar->codec_id),
                                  vp.width, vp.height,
                                  static_cast<int>(EVO_VDEC_OPEN_DOWNGRADED),
                                  "native_pref_refused");
            evo_vdec_close(g_vdec);
            g_vdec = nullptr;
            avformat_close_input(&play_fmt);
            play_fmt = nullptr;
            m_playbackFsm.postEvent(PlaybackEvent::Fail);
            toast("UNSUPPORTED", "4K software decode is switched off (evo_no_sw_4k)");
            return false;
        }
        if (!g_vdec) {
            evo_boot_log("PlaybackController: failed to open video decoder");
            evo_sweep_file_failed(filePath.c_str(),
                                  avcodec_get_name(vStream->codecpar->codec_id),
                                  vp.width, vp.height,
                                  static_cast<int>(evo_vdec_last_open_result()),
                                  "vdec_open_null");
        } else {
            evo_boot_log("PlaybackController: video decoder opened (backend=%s, codec=%d, %dx%d @ %.2f fps)",
                         (chosen == EVO_VDEC_BACKEND_NATIVE) ? "NATIVE (sceVideodec2)" : "FFmpeg",
                         vStream->codecpar->codec_id,
                         vp.width, vp.height, video_fps);
            evo_sweep_file_begin(filePath.c_str(),
                                 avcodec_get_name(vStream->codecpar->codec_id),
                                 vp.width, vp.height, video_fps,
                                 (play_fmt && play_fmt->duration > 0)
                                     ? static_cast<double>(play_fmt->duration) / static_cast<double>(AV_TIME_BASE)
                                     : 0.0,
                                 static_cast<int>(chosen),
                                 static_cast<int>(evo_vdec_last_open_result()));
            /*
             * Allocator state at the moment decode starts. malloc_shim.c has
             * tracked this all along, but its only reader was main.c, so the
             * numbers went dark at the carve-up and three separate 4K crashes
             * were diagnosed by guesswork instead. Logged here because every
             * one of those crashes happened within ~100ms of this line:
             *   hevc10_pq_4k -> "get_buffer() failed"
             *   av1_4k       -> dav1d "Failed to read unit" / "parse temporal unit"
             * If fails>0 or flex_avail is small, it is the heap, not the codec.
             */
            evo_log_alloc_state("decode-open");
            /* The line above is the heap side only, which is why the direct
             * pool never entered the argument: nothing printed it. This is the
             * other half, at the same instant, so an accepted decode can be
             * compared against a refused one. */
            evo_mem_budget_log("decode-open");
        }
    }

    // Initialize Audio Decoder if audio present
    if (audio_stream_index >= 0) {
        AVStream* aStream = play_fmt->streams[audio_stream_index];
        const AVCodec* aDec = avcodec_find_decoder(aStream->codecpar->codec_id);
        if (aDec) {
            audio_ctx = avcodec_alloc_context3(aDec);
            if (audio_ctx && avcodec_parameters_to_context(audio_ctx, aStream->codecpar) >= 0) {
                /*
                 * Without pkt_timebase, libavcodec cannot rebase timestamps
                 * when it discards a codec's priming samples, and logs
                 * "Could not update timestamps for skipped samples". Opus
                 * always has pre-skip, so that path runs on its very first
                 * frame and essentially never for AAC/AC-3/E-AC-3 - which is
                 * exactly the pattern the codec sweep measured. The video and
                 * subtitle decoders have always set this; audio never did.
                 */
                audio_ctx->pkt_timebase = aStream->time_base;
                if (avcodec_open2(audio_ctx, aDec, nullptr) >= 0) {
                    sceAudioOutInit();

                    /*
                     * The grain passed to sceAudioOutOpen is how many frames
                     * sceAudioOutOutput() consumes per call, and the output
                     * thread hands it whole AUDIO_BLOCK_SAMPLES blocks. Open
                     * the port with any other grain and the tail of every
                     * block is silently discarded while audio_samples_played
                     * still advances by a full block - the audio clock then
                     * runs fast by exactly that ratio and drags the
                     * audio-mastered video pacing along with it.
                     */
                    int chCount = audio_ctx->ch_layout.nb_channels;
                    int handle = -1;

                    /* 32-bit float ports first (FLOAT_8CH 5, FLOAT_STEREO 4), so
                     * 24-bit lossless audio keeps its resolution; 16-bit ports
                     * (S16_8CH 2, S16_STEREO 1) only if those are refused. The
                     * test switch "audio_s16=1" (vdec.conf) opens 16-bit ports
                     * only, to check what reaches HDMI bit for bit. */
                    bool s16_only = false;
#ifdef NUVIO_APP
                    s16_only = std::strstr(nuvio_vdec_conf, "audio_s16=1") != nullptr;
#endif
                    evo_audio_port_float = 1;
                    if (chCount > 2) {
                        handle = s16_only ? -1
                                          : sceAudioOutOpen(0xFF, 0, 0, AUDIO_BLOCK_SAMPLES, 48000,
                                                            5 /* FLOAT_8CH */);
                        if (handle < 1) {
                            handle = sceAudioOutOpen(0xFF, 0, 0, AUDIO_BLOCK_SAMPLES, 48000,
                                                     2 /* S16_8CH */);
                            if (handle >= 1)
                                evo_audio_port_float = 0;
                        }
                        if (handle >= 1) {
                            evo_audio_channels = 8;
                        } else {
                            toast("AUDIO",
                                  "surround port refused, falling back to stereo");
                        }
                    }

                    /* Surround refused (or a genuinely stereo source): stereo. */
                    if (handle < 1) {
                        evo_audio_channels = 2;
                        evo_audio_port_float = 1;
                        handle = s16_only ? -1
                                          : sceAudioOutOpen(0xFF, 0, 0, AUDIO_BLOCK_SAMPLES, 48000,
                                                            4 /* FLOAT_STEREO */);
                        if (handle < 1) {
                            handle = sceAudioOutOpen(0xFF, 0, 0, AUDIO_BLOCK_SAMPLES, 48000,
                                                     1 /* S16_STEREO */);
                            evo_audio_port_float = 0;
                        }
                    }
                    evo_bt("PlaybackController: audio port %dch %s (handle %d)", evo_audio_channels,
                           evo_audio_port_float ? "float32" : "s16", handle);

                    audio_handle = handle;

                    if (audio_handle < 1) {
                        toast("AUDIO OUTPUT ERROR",
                              "PS5 audio output could not start");
                    }

                    detected_audio_rate = (audio_ctx->sample_rate > 0)
                                              ? audio_ctx->sample_rate
                                              : 48000;

                    audio_queue_read = 0;
                    audio_queue_write = 0;
                    audio_queue_count = 0;
                    audio_accum_pos = 0;

                    prospero_audio_resampler_reset();

                    /*
                     * Offload AAC/MP3 to libSceAudiodec when it will take the
                     * stream. audio_ctx stays open either way: it is what the
                     * OSD reads for codec metadata, and it is the fallback the
                     * decode thread drops to if a native AU ever fails.
                     */
                    evo_adec_open_params ap;
                    std::memset(&ap, 0, sizeof(ap));
                    ap.codec_id = aStream->codecpar->codec_id;
                    ap.sample_rate = aStream->codecpar->sample_rate;
                    ap.channels = chCount;
                    ap.extradata = aStream->codecpar->extradata;
                    ap.extradata_size = aStream->codecpar->extradata_size;

                    evo_adec_backend achosen = EVO_ADEC_BACKEND_FFMPEG;
                    g_adec = evo_adec_open(&ap, &achosen);
                    evo_boot_log("PlaybackController: audio %s (codec=%d, %dch, %d Hz)",
                                 (achosen == EVO_ADEC_BACKEND_NATIVE)
                                     ? "NATIVE (sceAudiodec)" : "FFmpeg",
                                 (int)ap.codec_id, ap.channels, ap.sample_rate);
                }
            }
        }
    }

    if (video_stream_index < 0 && audio_stream_index >= 0) {
        AVStream* aStream = play_fmt->streams[audio_stream_index];
        evo_sweep_file_begin(filePath.c_str(),
                             avcodec_get_name(aStream->codecpar->codec_id),
                             0, 0, 0.0,
                             (play_fmt && play_fmt->duration > 0)
                                 ? static_cast<double>(play_fmt->duration) / static_cast<double>(AV_TIME_BASE)
                                 : 0.0,
                             0, 0);
    }

    // Subtitle setup
    prospero_subtitle_clear();
    prospero_subtitle_load_for_media(filePath.c_str());
    prospero_embedded_subtitle_open(play_fmt);
    prospero_secondary_subtitle_open(play_fmt);   // #110: only if one was chosen for this file

    /*
     * Apply the resume position. This has to be a real seek on play_fmt before
     * the demux thread starts: m_resumeBaseOffset is added to the decode clock
     * for every position readout (OSD, progress bar, subtitle timing), so
     * setting it without moving the file makes the whole UI report a time the
     * picture is not at - the OSD showed 5:15 while frame one was still 0:00,
     * and every subtitle cue was out by the same amount. requested_resume_seek_pos
     * used to drive this in start_video_playback(); after the C++ migration it
     * had no reader at all.
     */
    /*
     * Seeded with the position we are about to seek to, not 0.
     *
     * Zeroing it here and only correcting it after the seek left a window -
     * an avformat open, probe and av_seek_frame on a 4K file, easily hundreds
     * of milliseconds - during which every position readout said 0:00. So
     * resuming a part-watched file visibly started at the beginning and then
     * snapped to the resume point.
     *
     * This does not break the invariant above. That rule is that the base must
     * agree with where the picture is; during the open there is no picture
     * yet, only the position it is about to arrive at. If the seek fails the
     * base goes back to 0 with it, which is where playback really will start.
     */
    const bool wantResume = (resumeOffset > 0.0 && resumeOffset < m_durationSeconds &&
                             (video_stream_index >= 0 || audio_stream_index >= 0));

    m_resumeBaseOffset = wantResume ? resumeOffset : 0.0;
    resume_base_offset_seconds = m_resumeBaseOffset;
    requested_resume_seek_pos = 0.0;

    if (wantResume) {
        double seekPos = resumeOffset;
        int seekStream = (video_stream_index >= 0) ? video_stream_index
                                                   : audio_stream_index;
        /* Land slightly early so the first decoded frame is at or before the
         * resume point rather than just past it. */
        if (video_stream_index >= 0 && seekPos > 3.0)
            seekPos -= 1.0;

        int64_t seekTs = static_cast<int64_t>(
            seekPos / av_q2d(play_fmt->streams[seekStream]->time_base));

        if (av_seek_frame(play_fmt, seekStream, seekTs, AVSEEK_FLAG_BACKWARD) >= 0) {
            evo_vdec_flush(g_vdec);
            if (g_adec)
                evo_adec_flush(g_adec);
            if (audio_ctx) {
                avcodec_flush_buffers(audio_ctx);
                prospero_audio_resampler_reset();
            }
            prospero_embedded_subtitle_reset();
#ifdef NUVIO_APP
            nuvio_subs_on_seek();
#endif
            packet_queue_clear(&video_packet_queue);
            packet_queue_clear(&audio_packet_queue);

            m_resumeBaseOffset = resumeOffset;
            resume_base_offset_seconds = resumeOffset;
            /* The seek landed on a keyframe before resumeOffset; the first
             * decoded audio frame says where exactly, and re-anchors the base
             * (subtitle cues were early by the keyframe gap otherwise). */
            if (audio_stream_index >= 0)
                resume_base_anchor_pending = resumeOffset;
        } else {
            /* Playback will start at 0, so the base has to follow it back. */
            m_resumeBaseOffset = 0.0;
            resume_base_offset_seconds = 0.0;
            evo_boot_log("PlaybackController: resume seek to %.2fs failed; starting at 0",
                         resumeOffset);
        }
    }

    // Load chapters
    if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->loadChapters(play_fmt);
    }

    /*
     * Add to recent files database.
     *
     * #90: a provider source is keyed and titled by its identity, never by its
     * URL - see saveResumePosition() for why. A live stream is not recorded at
     * all: there is no position to come back to, and a channel that happens to
     * be on right now is not a thing the user asked to resume.
     */
    if (!m_source.is_live) {
        std::string recentKey = filePath;
        std::string mediaTitle = m_source.title, mediaCat;
        if (m_source.isProvider())
            recentKey = "evo://" + m_source.provider + "/" + m_source.item_id;
        if (mediaTitle.empty()) {
            if (auto metaService = Application::getInstance().getMediaMetadataService())
                metaService->cleanMediaTitle(filePath.c_str(), mediaTitle, mediaCat);
        }
        recent_add_or_update(recentKey.c_str(), mediaTitle.c_str(),
                             m_resumeBaseOffset, m_durationSeconds);
        recent_save();
    }

    /* Let the provider know a session started, if it tracks them. */
    if (m_source.isProvider()) {
        const evo_provider_t* prov = evo_provider_find(m_source.provider.c_str());
        if (prov && (prov->caps & EVO_PROVIDER_CAP_PROGRESS) && prov->report_progress)
            prov->report_progress(m_source.item_id.c_str(), 0,
                                  static_cast<int64_t>(m_durationSeconds),
                                  EVO_PROVIDER_PLAY_START);
    }
    player_paused = 0;
    video_decode_ready = (video_stream_index >= 0) ? 1 : 0;
    video_decode_done = 0;
    evo_pb_reset_decode_fatal();
    pp_playback_on_file_open(&g_pp_pb);

    applyViewMode();

    /*
     * Pre-buffer a network source before letting the decoders run.
     *
     * Local files read far faster than real time, so their queue is full
     * before the first frame is wanted and holding would only add latency for
     * nothing. A network source is the one that arrives at roughly real time
     * with jitter on top, which is where the cushion earns its keep.
     *
     * Armed here and cleared by the demux thread; the decode threads created
     * below park on it themselves, so this call does not block - the frame
     * loop keeps running and PlayerScreen shows "BUFFERING..." meanwhile.
     */
    {
        const bool network_source =
            filePath.rfind("http://", 0) == 0 || filePath.rfind("https://", 0) == 0;
        pb_prebuffer_hold = network_source ? 1 : 0;
        if (network_source)
            SIO_STAGE("P8_03_PREBUFFER_ARM", filePath.c_str());
    }

    // Start background worker threads
    demux_thread_running = 1;
    video_thread_running = (video_stream_index >= 0) ? 1 : 0;
    audio_decode_thread_running = (audio_ctx != nullptr) ? 1 : 0;
    audio_thread_running = (audio_handle >= 1) ? 1 : 0;

    evo_boot_log("  pb: threads demux=1 video=%d adec=%d aout=%d",
                 video_thread_running, audio_decode_thread_running,
                 audio_thread_running);
    evo_thread_create(&demux_thread, demux_thread_func, nullptr);

    if (video_thread_running) {
        evo_thread_create(&video_thread, video_decode_thread_func, nullptr);
    }
    if (audio_decode_thread_running) {
        evo_thread_create(&audio_decode_thread, audio_decode_thread_func, nullptr);
    }
    if (audio_thread_running) {
        evo_thread_create(&audio_thread, audio_output_thread, nullptr);
    }

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

double PlaybackController::clampScrubTarget(double target) const {
    if (target < 0.0) return 0.0;
    if (m_durationSeconds > 1.0 && target > m_durationSeconds) {
        return m_durationSeconds;
    }
    return target;
}

void PlaybackController::beginScrub() {
    if (!isActive()) return;

    /*
     * Capture the live position BEFORE entering Scrubbing.
     *
     * getPositionSeconds() opens with `if (isScrubbing()) return
     * m_scrubTargetSeconds;`, so once the FSM has taken the StartScrub
     * transition this line assigns the field to itself and the scrub head
     * never learns where playback actually is. It kept whatever the previous
     * scrub left behind - and 0.0 on the first scrub of a session, which is
     * why the bar and the clock jumped to the start of the file the moment the
     * D-pad was touched, then seeked to wherever the user nudged it from
     * there.
     */
    m_scrubTargetSeconds = getPositionSeconds();
    m_playbackFsm.postEvent(PlaybackEvent::StartScrub);
    pp_playback_pause(&g_pp_pb);
    /* Park demux's two consumers for the drag. Everything decoded from here
     * until the commit would be thrown away by the clock pp_playback_pause()
     * just stopped; see pb_scrub_hold in Bridge.cpp. */
    pb_scrub_hold = 1;
    resetScrubHold();
}

void PlaybackController::moveScrub(double deltaSeconds) {
    if (!isScrubbing()) {
        beginScrub();
    }
    m_scrubTargetSeconds = clampScrubTarget(m_scrubTargetSeconds + deltaSeconds);
    m_scrubAutoCommitDeadlineMs = GetCurrentTimeMs() + 600ULL; // auto-commit after 600ms idle
}

bool PlaybackController::confirmScrub() {
    if (!isScrubbing()) return false;

    double target = m_scrubTargetSeconds;
    m_playbackFsm.postEvent(PlaybackEvent::ConfirmScrub);
    resetScrubHold();

    /* Nothing else clears this: the commit is the end of the drag, so the hold
     * has to come down here or the decode threads never restart. Released
     * before the seek rather than after, so the pipeline is already live when
     * the demux thread picks the request up - the couple of frames the decoder
     * may take from the old position in between are ones the seek discards
     * anyway. (Either order is safe for the demux handler's park wait: a
     * thread parked on any hold still sets video_decode_parked.) */
    pb_scrub_hold = 0;
    seekTo(target);

    m_playbackFsm.postEvent(PlaybackEvent::Play);
    return true;
}

void PlaybackController::cancelScrub() {
    if (!isScrubbing()) return;

    m_playbackFsm.postEvent(PlaybackEvent::CancelScrub);
    resetScrubHold();
    pb_scrub_hold = 0;
    pp_playback_resume(&g_pp_pb);
}

void PlaybackController::resetScrubHold() {
    m_scrubHoldDirection = 0;
    m_scrubHoldStartMs = 0;
    m_scrubHoldLastStepMs = 0;
    m_scrubAutoCommitDeadlineMs = 0;
}

void PlaybackController::updateScrubHold(uint32_t heldButtons) {
    if (!isScrubbing()) return;

    int dir = 0;
    if (heldButtons & PadButtons::Right) dir = 1;
    else if (heldButtons & PadButtons::Left) dir = -1;

    if (dir == 0) {
        m_scrubHoldDirection = 0;
        m_scrubHoldStartMs = 0;
        m_scrubHoldLastStepMs = 0;
        return;
    }

    uint64_t now = GetCurrentTimeMs();
    if (m_scrubHoldDirection != dir) {
        m_scrubHoldDirection = dir;
        m_scrubHoldStartMs = now;
        m_scrubHoldLastStepMs = now;
        return;
    }

    // Accelerate scrub hold rate
    uint64_t holdDuration = now - m_scrubHoldStartMs;
    uint64_t stepInterval = 100ULL; // step every 100ms
    if (now - m_scrubHoldLastStepMs >= stepInterval) {
        double speed = 1.0;
        if (holdDuration > 3000) speed = 30.0;
        else if (holdDuration > 1500) speed = 10.0;
        else if (holdDuration > 500) speed = 3.0;

        moveScrub(static_cast<double>(dir) * speed);
        m_scrubHoldLastStepMs = now;
    }
}

void PlaybackController::tickScrubAutoCommit() {
    if (isScrubbing() && m_scrubAutoCommitDeadlineMs > 0) {
        if (GetCurrentTimeMs() >= m_scrubAutoCommitDeadlineMs) {
            confirmScrub();
        }
    }
}

void PlaybackController::seekTo(double targetSeconds) {
    if (!isActive() || targetSeconds < 0.0) return;

    m_resumeBaseOffset = targetSeconds;
    resume_base_offset_seconds = targetSeconds;

    int64_t targetUs = static_cast<int64_t>(targetSeconds * 1000000.0);
#ifdef NUVIO_APP
    /* A seek while paused stays paused (Nuvio's players behave so); and only
     * tell the presenter a seek is coming if the demuxer took it, or it waits
     * for a target that never arrives. */
    if (prospero_request_inplace_seek(targetSeconds, isPaused() ? 1 : 0))
        pp_playback_notify_seek_begin(&g_pp_pb, targetUs);
#else
    prospero_request_inplace_seek(targetSeconds, 0);
    pp_playback_notify_seek_begin(&g_pp_pb, targetUs);
#endif
}

void PlaybackController::jumpChapter(int direction) {
    auto metaService = Application::getInstance().getMediaMetadataService();
    if (!metaService || metaService->getChapterCount() == 0) {
        // Default relative jump by 30 seconds
        double target = getPositionSeconds() + (direction > 0 ? 30.0 : -30.0);
        seekTo(clampScrubTarget(target));
        return;
    }

    int currentChapter = metaService->getChapterIndexAtPosition(getPositionSeconds());
    int targetChapter = currentChapter + direction;

    if (targetChapter >= 0 && targetChapter < static_cast<int>(metaService->getChapterCount())) {
        const ChapterInfo* ch = metaService->getChapter(targetChapter);
        if (ch) {
            seekTo(ch->startTimeSeconds);
            toast("CHAPTER", ch->title.c_str());
        }
    }
}

void PlaybackController::cycleViewMode() {
    int nextMode = (static_cast<int>(m_viewMode) + 1) % 3;
    setViewMode(static_cast<ViewMode>(nextMode));
}

void PlaybackController::setViewMode(ViewMode mode) {
    m_viewMode = mode;
    applyViewMode();
}

void PlaybackController::applyViewMode() {
    pp_aspect_mode aspect = PP_ASPECT_FIT;
    if (m_viewMode == ViewMode::Fill) aspect = PP_ASPECT_FILL;
    else if (m_viewMode == ViewMode::Stretch) aspect = PP_ASPECT_STRETCH;

    g_pp_pb.aspect = aspect;
    g_pp_pb.stats.aspect = static_cast<int>(aspect);
    pp_playback_set_output(&g_pp_pb, DisplayWidth, DisplayHeight, aspect);
}

void PlaybackController::saveResumePosition() {
    if (m_currentFilePath.empty()) return;

    double pos = getPositionSeconds();

    /*
     * #90 scope 15: tell the provider where we are, before the local
     * bookkeeping. Rate-limited to roughly every ten seconds: this function is
     * the periodic tick (Bridge.cpp's save_resume_position), and Emby's
     * /Sessions/Playing/Progress on every call would be one request per tick
     * for a value that has moved by a second.
     */
    if (m_source.isProvider()) {
        const evo_provider_t* p = evo_provider_find(m_source.provider.c_str());
        if (p && (p->caps & EVO_PROVIDER_CAP_PROGRESS) && p->report_progress) {
            double now = static_cast<double>(GetCurrentTimeMs()) / 1000.0;
            if (m_lastProgressReport <= 0.0 || now - m_lastProgressReport >= 10.0) {
                m_lastProgressReport = now;
                p->report_progress(m_source.item_id.c_str(),
                                   static_cast<int64_t>(pos),
                                   static_cast<int64_t>(m_durationSeconds),
                                   EVO_PROVIDER_PLAY_UPDATE);
            }
        }
    }

    /*
     * A live stream has no position worth keeping. Writing one would make the
     * next launch of that channel seek to a point the stream does not have,
     * which on an HLS endpoint is a failed open rather than a wrong position.
     */
    if (m_source.is_live) return;

    if (pos < 5.0 || (m_durationSeconds > 0.0 && pos >= m_durationSeconds - 10.0)) {
        pos = 0.0; // Clear near start or finish
    }

    /*
     * What gets PERSISTED is never the URL.
     *
     * A provider URL carries an auth token and an expiry. Storing it in Recent
     * or Favorites leaks the token into a plaintext file and stores something
     * that will not work later anyway. The durable key is the provider id plus
     * the opaque item id - which is exactly what resolve() takes, so a future
     * "resume from Recent" has everything it needs and nothing it should not.
     */
    std::string persistKey = m_currentFilePath;
    if (m_source.isProvider())
        persistKey = "evo://" + m_source.provider + "/" + m_source.item_id;

    const char* resumeFile = evo_data_path("ps5_media_resume.txt");
    FILE* fp = std::fopen(resumeFile, "w");
    if (fp) {
        std::fprintf(fp, "%s\n%.2f\n", persistKey.c_str(), pos);
        std::fclose(fp);
    }

    /*
     * The resume file above is only read by the browser. Home, Recent and
     * Favorites all start playback from recent_files[i].last_pos instead, and
     * the only writer of that field is startPlayback(), which stores the offset
     * playback *began* at. Without this second half the stored position never
     * advances past the point a file was last opened from, so every launch
     * replays from the same frozen timestamp. (Pre-refactor this was
     * recent_update_current_position(); it has no caller any more.)
     */
    std::string title, category;
    if (!m_source.title.empty()) {
        /* The provider's own title. Deriving one from the URL is what put a
         * query string on the OSD - the root cause of #9. */
        title = m_source.title;
    } else if (auto metaService = Application::getInstance().getMediaMetadataService()) {
        metaService->cleanMediaTitle(m_currentFilePath, title, category);
    }
    recent_add_or_update(persistKey.c_str(), title.c_str(),
                         pos, m_durationSeconds);
    recent_save();
}

double PlaybackController::loadResumePosition(const std::string& filePath) const {
    if (filePath.empty()) return 0.0;

    const char* resumeFile = evo_data_path("ps5_media_resume.txt");
    FILE* fp = std::fopen(resumeFile, "r");
    if (!fp) return 0.0;

    char pathBuf[512] = {0};
    double pos = 0.0;
    if (std::fgets(pathBuf, sizeof(pathBuf), fp)) {
        size_t len = std::strlen(pathBuf);
        while (len > 0 && (pathBuf[len - 1] == '\n' || pathBuf[len - 1] == '\r')) {
            pathBuf[len - 1] = '\0';
            len--;
        }
        if (std::fscanf(fp, "%lf", &pos) == 1 && std::strcmp(pathBuf, filePath.c_str()) == 0) {
            std::fclose(fp);
            return pos;
        }
    }

    std::fclose(fp);
    return 0.0;
}

bool PlaybackController::playNextVideo() {
    // Attempt auto-advancing to next video entry in current directory
    auto browser = Application::getInstance().getFileSystemBrowser();
    if (!browser) return false;

    std::string currentFile = m_currentFilePath;
    size_t lastSlash = currentFile.find_last_of('/');
    std::string filename = (lastSlash != std::string::npos) ? currentFile.substr(lastSlash + 1) : currentFile;

    const auto& entries = browser->getEntries();
    int currentIndex = -1;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == filename || entries[i].relativePath == filename ||
            (!entries[i].fullPath.empty() && entries[i].fullPath == currentFile)) {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    if (currentIndex >= 0) {
        for (size_t i = currentIndex + 1; i < entries.size(); ++i) {
            if (entries[i].category == FileCategory::Video) {
                std::string nextPath = browser->getFullPath(i);
                return startPlayback(nextPath, 0.0);
            }
        }
    }

    return false;
}

std::vector<PlaybackController::AudioTrackInfo>
PlaybackController::getAudioTracks() const {
    std::vector<AudioTrackInfo> out;
    if (!play_fmt) return out;

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar ||
            st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) {
            continue;
        }
        /* Listing a track EVO cannot decode is an invitation to silence. */
        if (!avcodec_find_decoder(st->codecpar->codec_id)) {
            continue;
        }

        AudioTrackInfo t;
        t.streamIndex = static_cast<int>(i);
        t.channels    = st->codecpar->ch_layout.nb_channels;
        t.sampleRate  = st->codecpar->sample_rate;
        const char* cn = avcodec_get_name(st->codecpar->codec_id);
        t.codecName = cn ? cn : "unknown";

        const AVDictionaryEntry* lang =
            av_dict_get(st->metadata, "language", nullptr, 0);
        t.language = (lang && lang->value && lang->value[0]) ? lang->value : "UND";

        const AVDictionaryEntry* title =
            av_dict_get(st->metadata, "title", nullptr, 0);
        if (title && title->value && title->value[0]) {
            t.title = title->value;
        }

        out.push_back(std::move(t));
    }
    return out;
}

int PlaybackController::getActiveAudioStream() const {
    return audio_stream_index;
}

bool PlaybackController::switchAudioTrack(int streamIndex) {
    if (m_currentFilePath.empty() || streamIndex < 0) return false;
    if (streamIndex == audio_stream_index) return true;

    /*
     * Resume where the picture is, not where the file started: getPositionSeconds()
     * already folds in the resume base, and startPlayback() seeks to whatever it
     * is handed. Copy the path first - stopPlayback() clears m_currentFilePath.
     */
    std::string path = m_currentFilePath;
    double resumeAt = getPositionSeconds();
    bool wasPaused = isPaused();

    /* The whole source, not just its URL: reopening through startPlayback(path)
     * builds a bare-URL source, so a provider stream lost its title, identity and
     * live flag on every audio switch (the OSD fell back to the URL - the #9
     * defect again). */
    PlaybackSource savedSource = m_source;
    if (savedSource.url.empty()) savedSource.url = path;

    // Preserve subtitle state across audio reopen
    int savedSubEnabled = prospero_subtitle_enabled;
    int savedSubUseExt = prospero_subtitle_use_external;
    int savedSubStream = prospero_embedded_subtitle_stream_index;

    m_requestedAudioStream = streamIndex;
    if (!savedSubUseExt && savedSubStream >= 0) {
        prospero_subtitle_requested_stream = savedSubStream;
    }

    if (!startPlaybackSource(savedSource, resumeAt)) {
        m_requestedAudioStream = -1;
        prospero_subtitle_requested_stream = -2;
        return false;
    }

    // Restore subtitle state
    prospero_subtitle_enabled = savedSubEnabled;
    prospero_subtitle_use_external = savedSubUseExt;
    if (!savedSubUseExt && savedSubStream >= 0) {
        prospero_embedded_subtitle_stream_index = savedSubStream;
    }
    prospero_subtitle_requested_stream = -2;

    if (wasPaused) setPaused(true);
    return true;
}

std::vector<PlaybackController::VideoVariantInfo>
PlaybackController::getVideoVariants() const {
    std::vector<VideoVariantInfo> out;
    if (!play_fmt) return out;

    for (unsigned int i = 0; i < play_fmt->nb_streams; ++i) {
        const AVStream* st = play_fmt->streams[i];
        if (!st || !st->codecpar || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            continue;
        if (videoStreamPixels(st) <= 0) continue;      /* no size: not selectable */

        VideoVariantInfo v;
        v.streamIndex = static_cast<int>(i);
        v.width  = st->codecpar->width;
        v.height = st->codecpar->height;
        v.bitrate = st->codecpar->bit_rate;
        if (v.bitrate <= 0) {
            /* An HLS variant states its bandwidth on its program, not its stream. */
            const int prog = programOfStream(play_fmt, static_cast<int>(i));
            if (prog >= 0) {
                const AVDictionaryEntry* e =
                    av_dict_get(play_fmt->programs[prog]->metadata, "variant_bitrate", nullptr, 0);
                if (e && e->value) v.bitrate = std::strtoll(e->value, nullptr, 10);
            }
        }
        const AVRational fr = av_guess_frame_rate(play_fmt, const_cast<AVStream*>(st), nullptr);
        if (fr.num > 0 && fr.den > 0)
            v.fps = static_cast<double>(fr.num) / static_cast<double>(fr.den);
        const char* cn = avcodec_get_name(st->codecpar->codec_id);
        v.codecName = cn ? cn : "unknown";
        out.push_back(std::move(v));
    }

    /* Best first: taller, then faster. */
    std::stable_sort(out.begin(), out.end(), [](const VideoVariantInfo& a, const VideoVariantInfo& b) {
        if (a.height != b.height) return a.height > b.height;
        return a.bitrate > b.bitrate;
    });
    return out;
}

int PlaybackController::getActiveVideoStream() const {
    return video_stream_index;
}

bool PlaybackController::isVideoQualityPinned() const {
    return m_pinnedVideoStream >= 0 && !m_currentFilePath.empty() &&
           m_pinnedVideoFor == m_currentFilePath;
}

bool PlaybackController::switchVideoVariant(int streamIndex) {
    if (m_currentFilePath.empty()) return false;

    /* Nothing to do: already on it, or already automatic. */
    if (streamIndex >= 0 && isVideoQualityPinned() && streamIndex == m_pinnedVideoStream)
        return true;
    if (streamIndex < 0 && !isVideoQualityPinned())
        return true;

    /* The whole source, so the title, provider identity and live flag survive. */
    PlaybackSource src = m_source;
    if (src.url.empty()) src.url = m_currentFilePath;

    const double resumeAt = getPositionSeconds();
    const bool wasPaused = isPaused();

    /* The subtitle choice rides across the reopen exactly as it does for an
     * audio switch. */
    const int savedSubEnabled = prospero_subtitle_enabled;
    const int savedSubUseExt = prospero_subtitle_use_external;
    const int savedSubStream = prospero_embedded_subtitle_stream_index;
    if (!savedSubUseExt && savedSubStream >= 0)
        prospero_subtitle_requested_stream = savedSubStream;

    const int oldPin = m_pinnedVideoStream;
    const std::string oldPinFor = m_pinnedVideoFor;
    m_pinnedVideoStream = streamIndex;
    m_pinnedVideoFor = streamIndex >= 0 ? src.url : std::string();

    evo_bt("PlaybackController: switching video quality -> %s%d (at %.1f s)",
           streamIndex >= 0 ? "stream " : "auto ", streamIndex, resumeAt);

    if (!startPlaybackSource(src, resumeAt)) {
        m_pinnedVideoStream = oldPin;
        m_pinnedVideoFor = oldPinFor;
        prospero_subtitle_requested_stream = -2;
        return false;
    }

    prospero_subtitle_enabled = savedSubEnabled;
    prospero_subtitle_use_external = savedSubUseExt;
    if (!savedSubUseExt && savedSubStream >= 0)
        prospero_embedded_subtitle_stream_index = savedSubStream;
    prospero_subtitle_requested_stream = -2;

    if (wasPaused) setPaused(true);
    return true;
}

bool PlaybackController::replay() {
    if (m_currentFilePath.empty()) return false;
    /* The whole source (see switchAudioTrack), not a bare URL. */
    PlaybackSource src = m_source;
    if (src.url.empty()) src.url = m_currentFilePath;
    return startPlaybackSource(src, 0.0);
}

} // namespace evo
