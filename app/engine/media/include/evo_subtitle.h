/*
 * evo_subtitle.h — subtitle engine: embedded (MKV text tracks) + external SRT.
 *
 * Verbatim move of the EMBEDDED_SUBTITLE_MODULE, SRT_MODULE and
 * SUBTITLE_CONTROLS regions from main.c — Track A step A4 of
 * docs/modularisation-plan.md.
 *
 * TRANSITIONAL: still reads playback-core / resume globals from main.c and
 * the rr_text renderer as plain externs; main.c's seek + start_video_playback
 * still poke prospero_embedded_subtitle_ctx / dbg_sub_demuxed directly. A8
 * (façade) and the eventual rr_* renderer module clean both directions up.
 */
#ifndef EVO_SUBTITLE_H
#define EVO_SUBTITLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/* Below this a subtitle track is signage or a watermark, not dialogue.
 * Used by the embedded-track scorer and the media-info track picker. */
#define PROSPERO_SUBTITLE_MIN_USEFUL_CUES 10

/* External-SRT cue text buffer. The RmlUi caption overlay (#81) wraps and
 * shapes the text itself; MAX_LINES/LINE_SIZE are legacy and unused. */
#define PROSPERO_SUBTITLE_TEXT_SIZE  512
#define PROSPERO_SUBTITLE_MAX_LINES  3
#define PROSPERO_SUBTITLE_LINE_SIZE  160
#define PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE 768

typedef struct {
    double start_seconds;
    double end_seconds;
    char   text[PROSPERO_SUBTITLE_TEXT_SIZE];
} ProsperoSubtitleCue;

/* ---------------------------------------------------------------------------
 * Subtitle state. Owned by evo_subtitle.c; read (and, for the marked few,
 * written) across main.c's OSD, settings screens, track picker, debug overlay
 * and the demux/seek path until A8.
 * ------------------------------------------------------------------------ */
extern int prospero_subtitle_count;              /* external-SRT cue count   */
extern int prospero_subtitle_enabled;
extern int prospero_subtitle_face;               /* EVO_FACE_SUB/MENU/TITLE  */
extern int prospero_subtitle_delay_ms;
extern double prospero_subtitle_time_scale;      /* external SRT only, #102 */

/* Manual nudge and auto-sync share one range (#102). */
#define PROSPERO_SUBTITLE_MAX_DELAY_MS 60000
extern int prospero_subtitle_requested_stream;   /* -2 auto, -1 SRT, >=0 emb */
extern int prospero_subtitle_use_external;
extern int prospero_embedded_subtitle_stream_index;
extern int prospero_embedded_subtitle_count;

/* ---- secondary track (#110) ---------------------------------------------
 * A second line of dialogue drawn with the first: the external SRT or one
 * embedded text track, never the same source as the primary. */
#define PROSPERO_SECONDARY_NONE        (-2)   /* requested: no secondary track */
#define PROSPERO_SECONDARY_POS_STACKED 0      /* directly above the primary    */
#define PROSPERO_SECONDARY_POS_TOP     1      /* top of the screen             */
#define PROSPERO_SECONDARY_COLORS      3      /* yellow, cyan, white           */

extern int prospero_secondary_subtitle_requested; /* -2 none, -1 SRT, >=0 stream */
extern int prospero_secondary_subtitle_stream_index; /* embedded stream, or -1  */
extern int prospero_secondary_use_external;       /* the secondary is the SRT   */
extern int prospero_secondary_delay_ms;           /* its own sync, like the primary's */
extern int prospero_secondary_position;           /* PROSPERO_SECONDARY_POS_*   */
extern int prospero_secondary_color;              /* 0 yellow, 1 cyan, 2 white  */

/* Written by main.c's seek / start_video_playback flush path (A5 cleans up). */
extern AVCodecContext *prospero_embedded_subtitle_ctx;

/* Subtitle pipeline counters — dbg_sub_demuxed is bumped by the demux thread
 * in main.c; all five are read by the debug overlay. */
extern int dbg_sub_demuxed;
extern int dbg_sub_entered;
extern int dbg_sub_blank;
extern int dbg_sub_added;
extern int dbg_sub_cid;

/* ---- lifecycle (demux/seek/open/close) ---- */
void prospero_subtitle_clear(void);
int  prospero_subtitle_load_for_media(const char *media_path);
int  prospero_embedded_subtitle_open(AVFormatContext *format);
void prospero_embedded_subtitle_reset(void);
void prospero_embedded_subtitle_close(void);
void prospero_embedded_subtitle_decode_packet(AVPacket *packet);

/* Open the secondary track after the primary's open, from what was chosen for
 * this file. Returns 1 when a secondary track is active. */
int  prospero_secondary_subtitle_open(AVFormatContext *format);

/* Demux: does either slot want a packet from this stream? */
int  prospero_subtitle_wants_stream(int stream_index);

/* ---- query used by the media-info track picker ---- */
int  prospero_embedded_subtitle_supported(enum AVCodecID codec_id);
int  prospero_subtitle_declared_cues(AVStream *stream);

/* ---- render helpers for prospero_subtitle_draw() (which lives in main.c) ---- */
const ProsperoSubtitleCue *prospero_subtitle_active_cue(double position);
int  prospero_embedded_subtitle_text_at(double position, char *output,
                                        size_t output_size);
void prospero_subtitle_trim(char *text);   /* also used by wrap_text in main.c */

/* ---- controls (input dispatch + settings screen) ---- */
void prospero_subtitle_toggle(void);
void prospero_subtitle_apply_track(int track);
void prospero_subtitle_nudge_delay(int delta_ms);

/* Secondary track (#110). `track` is PROSPERO_SECONDARY_NONE to clear it, -1
 * for the external SRT, or an embedded stream index. Switches in place: the
 * demuxer already delivers every subtitle stream. Returns 1 on success. */
int  prospero_secondary_subtitle_select(int track);

/* Make `track` (-1 the external SRT, or an embedded stream index) the primary.
 * A secondary that held the same source is dropped. Returns 1 on success. */
int  prospero_subtitle_select_primary(int track);
int  prospero_secondary_subtitle_active(void);
void prospero_secondary_nudge_delay(int delta_ms);

/* The secondary caption at media-clock time `clock_seconds`, with its own
 * delay applied. Copies "" and returns 0 when nothing shows. */
int  prospero_secondary_subtitle_text_at(double clock_seconds, char *output,
                                         size_t output_size);

/* Media clock -> subtitle timeline: clock * scale - delay, floored at 0. */
double prospero_subtitle_position(double clock_seconds);

/* ---- auto-sync (#102, worker in evo_subsync.c) ---- */
int  prospero_subtitle_autosync_available(void);  /* SRT or text track active, local file */
int  prospero_subtitle_autosync_running(void);
void prospero_subtitle_autosync_toggle(void);     /* start, or cancel a run  */
void prospero_subtitle_autosync_pump(void);       /* UI thread, every frame  */
const char *prospero_subtitle_autosync_detail(char *buf, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SUBTITLE_H */
