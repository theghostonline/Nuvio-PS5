/*
 * evo_sweep — per-file playback measurement for the codec sweep (#8).
 *
 * A pass/fail sweep has now twice called a broken build green: once for a clip
 * that played with no audio track and was blamed on E-AC3 (baseline-defects.md),
 * once for every 10-bit file playing in the wrong colours because the PQ shader
 * was selected off profile rather than colour transfer (#41). So this records,
 * per clip, how fast it decoded, how much of it was dropped, AND what colour it
 * actually ended up on screen.
 *
 * One record per played file, emitted as a single machine-readable `sweep`
 * line into /mnt/usb0/evo.log at file close. tools/sweep_report.py turns a
 * log full of them into the docs/validation.md table.
 *
 * Cost: three counters on the present path and one 16-pixel glReadPixels per
 * file. Compiled in unconditionally — the numbers are as useful for a bug
 * report as for a sweep — but nothing is written unless a file was played.
 */
#ifndef EVO_SWEEP_H
#define EVO_SWEEP_H

#include <stdint.h>

#include "evo_vdec.h"
#include "pp_playback.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic microseconds. Exposed so the present hook in main.c times itself
 * against the same clock the decode seam uses. */
uint64_t evo_sweep_now_us(void);

/* Start a record. Call once the stream is open and the backend is known;
 * `open_result` is evo_vdec_last_open_result(). Discards any record in flight. */
void evo_sweep_file_begin(const char *path, const char *codec_name,
                          int w, int h, double fps, double duration_s,
                          int backend, int open_result);

/* The file never got as far as a decoder. Emits a row immediately so a clip
 * that cannot play still appears in the table, with why. `note` may be NULL. */
void evo_sweep_file_failed(const char *path, const char *codec_name,
                           int w, int h, int open_result, const char *note);

/* One swapped player frame, split at the eglSwapBuffers boundary.
 *
 * The split is the point: `begin..blit_done` is the GPU work EVO actually asked
 * for (texture upload, YUV->RGB on the quad, OSD composite), while
 * `blit_done..swap_done` is dominated by waiting for vblank and pins itself to
 * the refresh period no matter how cheap or expensive the frame was. Timing the
 * two together produces ~16 ms for everything and measures nothing.
 *
 * `video` marks a swap that uploaded a new decoded frame (as opposed to an
 * OSD-only redraw). No-op outside a record. */
void evo_sweep_note_present(uint64_t begin_us, uint64_t blit_done_us,
                            uint64_t swap_done_us, int video);

/* Take the colour probe now (see evo_gl_probe_rgb). Call from the render loop
 * right after the video quad has been drawn and before the OSD composite, so
 * the sample is video pixels. Self-limiting: only the first call per record
 * after the warm-up frame count does anything. */
void evo_sweep_probe_colour(void);

/* Close the record and write the `sweep` line. `v` may be NULL (the decoder is
 * usually closed just after); call this BEFORE evo_vdec_close(). */
void evo_sweep_file_end(const evo_vdec *v, const pp_playback_stats *ps);

/* 1 while a record is open — lets callers skip work when nothing is measuring. */
int evo_sweep_active(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SWEEP_H */
