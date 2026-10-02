#pragma once
/*
 * Subtitles for the Nuvio Player.
 *
 * Text tracks - embedded (ASS/SSA, SubRip, WebVTT, mov_text, ...) and the
 * external ones Nuvio's subtitle addons find - are rendered by libass, so
 * styled ASS keeps its fonts, positioning and effects; plain text tracks get
 * Nuvio's subtitle style. Bitmap tracks (PGS, VobSub, DVB) are decoded with
 * FFmpeg and scaled onto the picture. Everything is timed against the video
 * frame on screen.
 *
 * The demux thread feeds packets (nuvio_subs_on_packet); the render loop asks
 * for a frame (nuvio_subs_render). Text tracks keep every cue seen so far,
 * so switching tracks or seeking back never loses lines.
 */
#include "ui_canvas.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct AVFormatContext;
struct AVPacket;

typedef struct nuvio_rect {
    float x, y, w, h;
} nuvio_rect;

typedef struct nuvio_sub_track {
    char lang[16];        /* ISO 639 code as given ("eng", "en", ...), may be empty */
    char title[160];      /* stream title or the addon's label */
    char codec[24];       /* "subrip", "ass", "hdmv_pgs_subtitle", "srt" (external) */
    int  external;        /* from an addon */
    int  bitmap;          /* PGS / VobSub / DVB */
    int  forced;
    int  is_default;
    int  hearing_impaired;
    int  state;           /* external: 0 loading, 1 ready, -1 failed; embedded: 1 */
} nuvio_sub_track;

/* Nuvio's subtitle appearance for text tracks (ignored by styled ASS). */
typedef struct nuvio_sub_style {
    int      size_pct;          /* 100 = default */
    uint32_t color;             /* 0xRRGGBB */
    int      bold;
    int      outline;           /* outline + shadow */
    float    background;        /* 0..1 opacity of a box behind the text */
    float    offset_pct;        /* extra lift from the bottom, % of height */
} nuvio_sub_style;

/* Once, early: libass and the fonts. */
int  nuvio_subs_init(void);

/* After a stream opens: the embedded subtitle tracks and font attachments. */
void nuvio_subs_open(struct AVFormatContext *fmt, int video_stream);
/* An addon subtitle; fetched in the background. Returns its track id. */
int  nuvio_subs_add_external(const char *url, const char *lang, const char *label,
                             const char *headers);
/* At the end of a playback. */
void nuvio_subs_close(void);

int  nuvio_subs_count(void);
/* Copies track i's description; 0 on success. */
int  nuvio_subs_track(int i, nuvio_sub_track *out);
int  nuvio_subs_selected(void);              /* -1 = off */
void nuvio_subs_select(int id);
void nuvio_subs_set_delay_ms(int ms);
int  nuvio_subs_delay_ms(void);
void nuvio_subs_set_style(const nuvio_sub_style *style);
void nuvio_subs_get_style(nuvio_sub_style *out);

/* Demux thread hooks (engine/media/src/evo_demux.c). */
void nuvio_subs_on_packet(struct AVFormatContext *fmt, const struct AVPacket *pkt);
void nuvio_subs_on_seek(void);

/* Whether the last render left subtitles on the canvas. */
int  nuvio_subs_visible(void);

/* Draws the subtitles for the frame at pts_us into c (cleared first) when
 * anything changed. video is where the picture is on the canvas; lift moves
 * text tracks up (the controls are showing). Returns 1 if c changed. */
int  nuvio_subs_render(ui_canvas *c, int64_t pts_us, nuvio_rect video, float lift);

#ifdef __cplusplus
}
#endif
