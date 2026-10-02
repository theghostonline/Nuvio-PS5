/*
 * evo_audio_out.h — media audio output path: decode → resample → AudioOut.
 *
 * Owns the AudioOut port handle, the block ring feeding it, the audio session
 * clock, the audio decode thread, and the audio-track switch. Verbatim move of
 * the audio-out / mix / AUDIO_TRACK_SWITCH regions from main.c — Track A step
 * A3 of docs/modularisation-plan.md.
 *
 * TRANSITIONAL: this module still reads a dozen playback-core globals from
 * main.c (screen, player_paused, video_clock_seconds, the subtitle/resume
 * state used by the track switch, ...) as plain externs, and main.c's
 * start_video_playback / stop_video_playback / seek still poke the audio-out
 * state exported below directly. Step A8 replaces both directions with the
 * evo_pb_*() façade (plan §4).
 */
#ifndef EVO_AUDIO_OUT_H
#define EVO_AUDIO_OUT_H

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

#define EVO_AUDIO_MAX_CH    8
#define AUDIO_QUEUE_BLOCKS  64
#define AUDIO_BLOCK_SAMPLES 2048

/*
 * Cold-start window after an open or a seek, in seconds of media.
 *
 * Video waits on the audio clock (decode_next_video_frame) and audio waits on
 * the video clock (audio_output_thread). Both comparisons are meaningful only
 * once the two clocks have been running long enough to mean something. A seek
 * resets BOTH to zero, so for the first fraction of a second afterwards each
 * side is throttling itself against a number that has barely moved - which is
 * what made the seconds after every seek run at half rate on hardware
 * (2026-09-28: 9.19/s and 12.79/s in the windows containing a seek, against
 * 23.98/s once settled).
 *
 * Inside this window neither side throttles the other. Nothing is unpaced:
 * video is still paced frame-by-frame by the presentation clock in
 * pp_playback_push_frame(), and audio is still paced by the output port
 * accepting blocks. The cross-throttles only exist to correct long-term
 * drift, and there is no long-term drift to correct yet.
 */
#define EVO_AV_SYNC_SETTLE_SEC 0.75

/* ---------------------------------------------------------------------------
 * Audio-out session state. Owned by evo_audio_out.c; still written directly by
 * main.c's start_video_playback / stop_video_playback / seek path until A8.
 * ------------------------------------------------------------------------ */
extern int                audio_handle;
extern int                audio_accum_pos;
extern volatile int       audio_queue_read;
extern volatile int       audio_queue_write;
extern volatile int       audio_queue_count;
extern volatile long long audio_samples_played;
extern volatile long long audio_samples_decoded;
extern volatile double    audio_clock_seconds;
extern volatile double    audio_pts_seconds;
extern double             first_audio_pts_seconds;
extern int                detected_audio_rate;

/*
 * Seek discard gate, in stream seconds, or -1.0 when no seek is settling.
 *
 * The video path throws away every decoded frame before the seek target
 * (pp_playback_push_frame) because the demuxer has to restart from the
 * keyframe at or before it. Without the same gate here the audio replays that
 * whole run-up, so the moment the picture resumes at the target the audio
 * clock is a GOP behind the video clock and the audio-master pacer in
 * decode_next_video_frame stalls the picture until audio covers the gap.
 * Set by the demux thread's seek; cleared by the first packet that reaches it.
 */
extern volatile double    audio_seek_discard_until;

/* Native audio decoder for the open stream, or NULL when it is on FFmpeg. */
struct evo_adec;
extern struct evo_adec   *g_adec;
extern volatile int       audio_thread_running;
extern pthread_t          audio_thread;
extern volatile int       audio_decode_thread_running;
extern pthread_t          audio_decode_thread;

/* Audio-track switch: the pending stream request and the label of the live
 * track. start_video_playback reads both while selecting streams. */
extern int  prospero_audio_requested_stream;
extern char prospero_audio_active_label[128];

/* Threads (spawned by main.c via pthread_create). */
void *audio_output_thread(void *arg);
void *audio_decode_thread_func(void *arg);

/* Build the "1/3  eng  eac3  6CH"-style label for a given audio stream. */
void prospero_audio_build_label(AVFormatContext *format, int selected_stream,
                                char *output, size_t output_size);

/* Audio track selection lives in evo::PlaybackController::switchAudioTrack()
 * and the AudioTrackPickerScreen; the old cycle entry point is gone. */

#ifdef __cplusplus
}
#endif

#endif /* EVO_AUDIO_OUT_H */
