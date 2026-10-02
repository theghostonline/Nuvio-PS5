/*
 * The playback engine's shared state.
 *
 * EVO Player's engine (engine/) still talks through a set of globals and a few
 * helper functions that EVO kept in core/src/Bridge.cpp, next to its screen
 * and dev-remote glue. This is the engine half of that file, unchanged in
 * meaning; the Nuvio Player owns them through player.cpp.
 */
#include "evo/Application.hpp"
#include "pp_playback.h"
#include "evo_playback.h"
#include "evo_vdec.h"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>

namespace evo {
/* Replaced at start-up with what the VideoOut layer reports. */
int DisplayWidth  = 1920;
int DisplayHeight = 1080;
}  // namespace evo

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <sys/time.h>

pp_playback g_pp_pb;
int g_ps5_user_id = 0;
int screen = 0;
int g_ps5_video_out_hdr = 0;

int player_paused = 0;
double media_duration_sec = 0.0;
double resume_base_offset_seconds = 0.0;
double requested_resume_seek_pos = 0.0;
char current_media_path[768] = {0};
int evo_audio_channels = 2;

AVFormatContext *play_fmt = nullptr;
AVCodecContext *audio_ctx = nullptr;
evo_vdec *g_vdec = nullptr;
int g_vdec_force_ffmpeg = 0;
AVPacket *video_pending_pkt = nullptr;

int perf_render_fps = 0;
int perf_decode_fps = 0;
int perf_render_frames = 0;
int perf_decode_frames = 0;
double g_gl_present_ms = 0.0;
int show_debug_overlay = 0;

int current_profile = 0;
int playback_profile = 1;
/* Read-ahead for internet streams: ~8-10 s of video at film rates (EVO's 96
 * is ~3 s, sized for local files), audio up to the queue's 512 slots. */
int video_packet_cap = 240;
int audio_packet_cap = 480;

/* Network pre-buffer and scrub hold: see EVO's Bridge.cpp for the history.
 * Decode threads park on these while the demux thread builds a cushion. */
volatile int pb_prebuffer_hold = 0;
int pb_prebuffer_packets = 48;
int pb_prebuffer_max_ms = 4000;
volatile int pb_scrub_hold = 0;
int decoder_thread_count = 4;
int video_view_mode = 0;   /* 0 = fit: show the whole picture */
long long controls_last_used_ms = 0;

int dbg_read_fail = 0;
int dbg_video_packets = 0;
int dbg_video_frames = 0;
int dbg_video_thread_alive = 0;
int dbg_swaps = 0;
long long dbg_last_pts = 0;
int g_first_frame_bc_done = 0;
struct SwsContext *play_sws = nullptr;

pp_aspect_mode prospero_view_mode_to_aspect(void) {
    if (video_view_mode == 1)
        return PP_ASPECT_FILL;
    if (video_view_mode == 2)
        return PP_ASPECT_STRETCH;
    return PP_ASPECT_FIT;
}

int prospero_get_initial_user_id(void) {
    return g_ps5_user_id;
}

long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (long long)tv.tv_sec * 1000LL + (tv.tv_usec / 1000);
}

long long perf_now_ms(void) {
    return now_ms();
}

int str_contains_ci(const char *s, const char *needle) {
    if (!s || !needle) return 0;
    if (!*needle) return 1;
    size_t needle_len = strlen(needle);
    for (; *s; s++) {
        if (strncasecmp(s, needle, needle_len) == 0)
            return 1;
    }
    return 0;
}

void format_time_mmss(char *out, size_t out_size, double sec) {
    if (!out || out_size == 0) return;
    if (sec < 0.0) sec = 0.0;
    int s = (int)sec;
    int m = s / 60;
    s %= 60;
    snprintf(out, out_size, "%02d:%02d", m, s);
}

/* The page supplies the title; there is no file name to clean up. */
void clean_media_title(const char *path, char *line1, size_t line1_sz, char *line2, size_t line2_sz) {
    (void)path;
    if (line1 && line1_sz > 0) line1[0] = 0;
    if (line2 && line2_sz > 0) line2[0] = 0;
}

double prospero_player_position(void) {
    return resume_base_offset_seconds + prospero_media_clock_seconds();
}

double evo_player_position_s(void) {
    return prospero_player_position();
}

/* Non-zero on success (the subtitle track switch restarts playback here). */
int start_video_playback_at(const char *path, double resume_seconds) {
    if (auto pb = evo::Application::getInstance().getPlaybackController())
        return pb->startPlayback(path ? path : "", resume_seconds) ? 1 : 0;
    return 0;
}

int start_video_playback(const char *path) {
    return start_video_playback_at(path, 0.0);
}

void stop_video_playback(void) {
    if (auto pb = evo::Application::getInstance().getPlaybackController())
        pb->stopPlayback();
}

void evo_stop_media_playback(void) {
    stop_video_playback();
}

}  // extern "C"
