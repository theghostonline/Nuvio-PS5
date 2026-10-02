/*
 * prospero_thumbnail — the scrub-preview thumbnail worker.
 *
 * Decodes a frame at a requested timestamp on a background thread, caches it,
 * and crossfades between the last two when the scrub position moves. The
 * player asks for a time and blits whatever is ready; nothing here blocks the
 * render thread.
 *
 * WHY THIS IS THE FIRST MODULE OUT OF main.c
 * ------------------------------------------
 * main.c is ~18k lines in one translation unit, and the received wisdom was to
 * pull decode/demux out first. Measuring the file says otherwise: half its 263
 * file-scope globals are referenced across spans of more than 5,000 lines, and
 * the *most* smeared of them are exactly the playback ones — `g_pp_pb` is
 * touched across 17,899 lines, `player_paused` across 17,311. Extracting that
 * first would mean publishing sixty globals through a header, which converts a
 * tangle into a tangle with an API.
 *
 * This module was chosen because the measurement pointed at it: 1,096 lines,
 * 39 symbols, and its entire dependency on the rest of the program is two
 * helpers and two screen constants. It owns its own thread, mutex, condition
 * variable and request queue, so the seam already existed — it just was not
 * a file.
 *
 * The interface below is everything the player may touch.
 */
#ifndef PROSPERO_THUMBNAIL_H
#define PROSPERO_THUMBNAIL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cached frame size. 320x180 is 16:9 — the shape the source frames are — so
 * the preview minifies into its on-screen box rather than magnifying. */
#ifndef PROSPERO_THUMB_W
#define PROSPERO_THUMB_W 320
#endif
#ifndef PROSPERO_THUMB_H
#define PROSPERO_THUMB_H 180
#endif

/**
 * Ask for the frame at `target_seconds` of `path`.
 *
 * Returns immediately. Repeated requests for the same path and a nearby time
 * coalesce, so calling this every frame while the user scrubs is the intended
 * usage, not an abuse of it.
 *
 * `scrub_active` being 0 makes the call a no-op. The gate lives here rather
 * than at the call site so a caller cannot forget it and leave the worker
 * decoding frames nobody is going to look at.
 */
void prospero_thumbnail_request(const char *path,
                                double target_seconds,
                                int scrub_active);

/** True once a decoded frame is available to blit. */
int prospero_thumbnail_is_valid(void);

/** True while the worker is decoding. Drives the spinner, not the picture. */
int prospero_thumbnail_is_loading(void);

/**
 * Blit the current thumbnail into `fb`, scaled to the destination box and
 * crossfaded from the previous one if that transition is still running.
 * `opacity` is 0–255 and multiplies the whole blit.
 */
void prospero_thumbnail_blit(uint32_t *fb,
                             int destination_x,
                             int destination_y,
                             int destination_w,
                             int destination_h,
                             int opacity);

/**
 * Copy the current preview frame out as BGRA (0xAABBGGRR, the framebuffer
 * order), for callers that upload it as a texture rather than blitting it.
 *
 * `dst` must have room for PROSPERO_THUMB_W * PROSPERO_THUMB_H words; pass
 * that count as `dst_words`. Returns a serial that changes every time a newly
 * decoded frame is published, so a caller can skip the copy and the upload
 * when nothing has moved, or 0 when there is nothing to show yet.
 *
 * Unlike prospero_thumbnail_blit() this hands back the current frame only -
 * no crossfade. The fade is a property of the blit, and a texture consumer
 * gets its transition from the UI layer instead.
 */
unsigned long long prospero_thumbnail_snapshot(uint32_t *dst, int dst_words);

/**
 * The serial alone, without copying anything. Cheap enough to poll per frame.
 */
unsigned long long prospero_thumbnail_serial(void);

/**
 * Release the decoder held open for the current file.
 *
 * The worker keeps the format context open between requests because reopening
 * per scrub step is what made scrubbing stutter. Call this when playback stops
 * or the file changes.
 */
void prospero_thumbnail_close_context(void);

#ifdef __cplusplus
}
#endif

#endif /* PROSPERO_THUMBNAIL_H */
