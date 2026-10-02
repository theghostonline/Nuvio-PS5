/*
 * evo_playback.h — the playback session: the video decode/pace/present loop,
 * the media clock, and the video thread lifecycle. Track A step A7 of
 * docs/modularisation-plan.md.
 *
 * §4 of the plan: everything outside the playback core that reads a playback
 * global should go through the evo_pb_*() accessors below. Step A8 sweeps the
 * remaining `extern` reads in ui/ / osd / screen code over to them; until then
 * the owned state is also exported raw (marked TRANSITIONAL).
 *
 * main.c keeps the open/close *policy* (which file, browser selection, the VO
 * / g_pp_pb orchestration) — start_video_playback() / stop_video_playback()
 * live there and drive this module.
 */
#ifndef EVO_PLAYBACK_H
#define EVO_PLAYBACK_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>

/*
 * Slots in the swscale-fallback present ring (convert_frame_via_sws). #6:
 * trimmed 8 -> 3 — one presented + one just-written + one in-flight is enough
 * for the legacy renderer not to tear, and at 4K each slot is 33 MB. The
 * product present path does not use this ring (see evo_playback.c).
 */
#define VIDEO_ROTATE_BUFFERS 3

/* ---- §4 playback façade — the stable interface ---- */
int    evo_pb_is_active(void);        /* a video decode session is running   */
int    evo_pb_is_paused(void);        /* was: player_paused                  */
int    evo_pb_is_eof(void);           /* was: video_decode_done              */
int    evo_pb_decode_fatal(void);     /* sustained decoder failure — abort   */
void   evo_pb_reset_decode_fatal(void); /* clear fatal flag + streak (re-open) */
double evo_pb_position_s(void);       /* audio-preferred media clock         */
double evo_pb_duration_s(void);
double evo_pb_audio_clock_s(void);
double evo_pb_video_clock_s(void);    /* raw video presentation clock (s)    */
double evo_pb_video_fps(void);        /* 0 until a stream is open            */
void   evo_pb_queue_depth(int *vpkts, int *apkts, int *ablocks);  /* debug overlay */
int    evo_pb_active_backend(void);   /* evo_vdec_backend of the live decoder */

/* The audio-preferred media clock (kept under its historical name — many call
 * sites). evo_pb_position_s() is the façade alias. */
double prospero_media_clock_seconds(void);

/* ---- the video thread (spawned / joined by start/stop_video_playback,
 *      mirroring how main.c drives evo_demux's thread) ---- */
void *video_decode_thread_func(void *arg);
int   decode_next_video_frame(void);   /* one pump iteration; return unused   */

extern volatile int video_thread_running;
extern pthread_t    video_thread;

/* 1 while the decode thread is idle (not inside push_frame's unlocked convert).
 * main.c waits on this before a #32 scrub-overlay VO reconfigure. */
extern volatile int video_decode_parked;

/* ---- TRANSITIONAL raw state.
 * A8 migrated main.c's OSD / debug / completion *reads* to evo_pb_*(); what
 * remains is (a) the session-lifecycle *writes* in start/stop_video_playback
 * (main.c owns open/close policy, §A.2), and (b) reads from the sibling media
 * modules — the audio threads compare against video_clock_seconds, the demux
 * seek executor resets the clocks, evo_subtitle reads the position. Those stay
 * extern until the media modules share a clock struct. `player_paused` is
 * still a main.c global (written from 4 modules) — not yet privatised. ---- */
extern double   video_clock_seconds;
extern double   first_video_pts_seconds;
extern double   video_fps;
extern int      video_decode_ready;
extern int      video_decode_done;
extern int      g_pb_decode_fatal;

extern AVPacket *video_video_pending_pkt;   /* freed by the evo_demux seek path */

/* Legacy swscale RGBA output — the !PP_BACKEND_ENABLED renderers and the
 * thumbnail path in main.c still read these; the rotate ring itself is private
 * to evo_playback.c. Retire when the rr_* renderers / cover art move (B6/B8). */
extern pthread_mutex_t video_frame_mutex;
extern int       video_frame_w;
extern int       video_frame_h;
extern int       video_frame_loaded;
extern uint32_t *video_frame_pixels;

#ifdef __cplusplus
}
#endif

#endif /* EVO_PLAYBACK_H */
