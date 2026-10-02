/*
 * pp_playback — product session façade over the presentation clock.
 *
 * Since GL-4 (#80) this is decode + pace + clock + seek and nothing else. The
 * decoded frame's planes are published as-is (borrowed, no copy); the GL render
 * loop uploads them as R8/RG8 textures and a GLSL shader does YUV->RGB on the
 * quad. The CPU converters, the tiled VideoOut present, the V8/V3/1080 backend
 * enum and the `display` BGRA double buffer are all gone.
 *
 * No FFmpeg types in this header.
 */
#ifndef PP_PLAYBACK_H
#define PP_PLAYBACK_H

#include "pp_clock.h"
#include "pp_frame.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pp_playback_stats {
    uint64_t frames_in;
    uint64_t frames_converted;   /* == frames handed to the GL uploader */
    uint64_t frames_published;
    uint64_t frames_late_dropped;
    uint64_t frames_discarded_seek;
    uint64_t convert_us_total;
    uint64_t convert_us_max;
    uint32_t convert_ring_count;
    uint64_t convert_ring[256];
    uint64_t seek_requests;
    uint64_t seek_successes;
    uint64_t seek_failures;
    uint64_t clock_resets;
    uint64_t seek_to_first_frame_ms;
    uint8_t  sample_luma;        /* first published Y byte - "is there a picture" probe */
    int output_w;
    int output_h;
    int aspect;
} pp_playback_stats;

typedef struct pp_playback {
    pp_clock clock;
    pp_aspect_mode aspect;

    uint32_t out_w;             /* panel size; reporting only since GL-4 */
    uint32_t out_h;
    int64_t display_pts_us;

    int active;
    int seek_discarding;
    int64_t seek_target_us;
    uint64_t seek_begin_ms;

    /*
     * Consecutive frames rejected as late by the presentation clock. If the
     * clock's wall-time base ever runs on while frames are not being pushed -
     * a modal overlay covering playback, a long stall - it comes back
     * believing every frame is late and drops all of them, permanently. This
     * counter lets that resolve itself. See pp_playback_push_frame().
     */
    int late_drop_streak;

    /*
     * GL-4 (#80): the decoded frame's planes, borrowed from the decoder's frame
     * pool (or FFmpeg's AVFrame) — valid because push_frame pace-sleeps on this
     * frame's PTS before returning, so the decoder can't recycle the slot until
     * the render thread has had its turn. pp_playback_get_video_frame() hands these
     * straight out (under the lock, no pixel copy); the GL uploader reads them.
     * gl_src_uv != NULL => NV12 (RG8 chroma); else gl_src_u/_v are planar I420.
     */
    const uint8_t *gl_src_y, *gl_src_uv, *gl_src_u, *gl_src_v;
    int      gl_src_ypitch, gl_src_uvpitch, gl_src_upitch, gl_src_vpitch;
    uint32_t gl_cw, gl_ch;      /* coded (padded) luma w/h = texture size */
    uint32_t gl_dw, gl_dh;      /* display (cropped) w/h                  */
    int      gl_ready;
    int      gl_ten_bit;        /* GL-5 (#81): planar source is 16-bit (yuv420p10le) */
    int      gl_color_trc;

    /*
     * GL-4 Stage 2d: a seek's discard window is the one time the borrowed
     * pointers go bad — the decode thread runs flat out into the same pool with
     * nothing published, so the slot under gl_src_y is overwritten mid-upload
     * and the "held" frame tears into garbage for the seconds a long-GOP 4K
     * seek takes. Snapshot the last published frame into memory pp_playback
     * owns when the seek starts, serve that until the first post-seek frame
     * lands, and the picture behind the scrub OSD simply freezes. One copy per
     * seek, not per frame — the steady state stays zero-copy.
     */
    uint8_t *hold_buf;
    size_t   hold_cap;
    int      hold_valid;        /* serve hold_buf from get_nv12           */
    int      hold_planar;
    int      hold_ten_bit;
    int      hold_color_trc;
    int      hold_ypitch, hold_uvpitch, hold_upitch, hold_vpitch;
    size_t   hold_uv_off, hold_u_off, hold_v_off;
    uint32_t hold_cw, hold_ch, hold_dw, hold_dh;

    void *lock;
    pp_playback_stats stats;
} pp_playback;

void pp_playback_init(pp_playback *pb);
void pp_playback_shutdown(pp_playback *pb);

/**
 * Record the panel size + aspect mode. Since GL-4 nothing is scaled on the CPU
 * (the quad's vertex transform does FIT/FILL/STRETCH), so this only feeds the
 * stats line.
 */
int pp_playback_set_output(pp_playback *pb, uint32_t w, uint32_t h,
                           pp_aspect_mode aspect);

void pp_playback_on_file_open(pp_playback *pb);
void pp_playback_on_file_close(pp_playback *pb);

void pp_playback_pause(pp_playback *pb);
void pp_playback_resume(pp_playback *pb);

/**
 * Pace the frame on the presentation clock and publish its planes.
 * Returns 0 published, 1 late-drop/paused/seek-discard, <0 error.
 */
int pp_playback_push_frame(pp_playback *pb, const pp_frame *src);

/** 1 once a frame has been published for the current file. */
int pp_playback_has_display(const pp_playback *pb);

/**
 * The ready frame's planes — normally borrowed from the decoder (no pixel
 * copy). During a seek's discard window these point at pp_playback's own
 * snapshot of the last published frame instead, so the picture holds still.
 * For an FFmpeg planar source `uv` is NULL and `u`/`v` are the chroma planes.
 */
typedef struct pp_video_frame {
    const uint8_t *y, *uv, *u, *v;
    int      y_pitch, uv_pitch, u_pitch, v_pitch;   /* bytes (2x samples when ten_bit) */
    uint32_t coded_w, coded_h;   /* padded luma plane = R8/R16 texture size */
    uint32_t disp_w, disp_h;     /* valid (cropped) region              */
    int      ready;
    int      held;               /* 1 = the frozen mid-seek snapshot    */
    int      ten_bit;            /* GL-5 (#81): planar 16-bit (yuv420p10le), sample as GL_R16 */
    int      color_trc;
} pp_video_frame;

/**
 * Fill *f with the ready frame's planes under the display lock (a pointer/int
 * copy, no pixels). Returns 1 if ready, 0 otherwise.
 */
int pp_playback_get_video_frame(pp_playback *pb, pp_video_frame *f);

void pp_playback_notify_seek_begin(pp_playback *pb, int64_t target_pts_us);
void pp_playback_notify_seek_end(pp_playback *pb, int success,
                                 uint64_t discarded, uint64_t elapsed_ms);

void pp_playback_get_stats(const pp_playback *pb, pp_playback_stats *out);
uint64_t pp_playback_convert_p95_us(const pp_playback *pb);
void pp_playback_log_stats(const pp_playback *pb);   /* -> /mnt/usb0/evo.log */

#ifdef __cplusplus
}
#endif

#endif /* PP_PLAYBACK_H */
