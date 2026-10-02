/*
 * evo_audio_resample.h — libswresample configuration for the audio-out path.
 *
 * Owns the single SwrContext and the "what was it last configured for"
 * tracking (input layout/format/rate). Converts any decoded layout to
 * interleaved S16 at 48 kHz, matching the AudioOut port that is actually
 * open (stereo or 7.1). Verbatim move of the PROSPERO_AUDIO_RESAMPLER_STATE
 * region from main.c — Track A step A2 of docs/modularisation-plan.md.
 *
 * FFmpeg-only leaf. The two pieces of external state the configure path
 * needs (open port channel count, the audio AVCodecContext) are passed in
 * rather than read as globals.
 */
#ifndef EVO_AUDIO_RESAMPLE_H
#define EVO_AUDIO_RESAMPLE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

/* AudioOut always runs at 48 kHz; the resampler output rate is fixed to it. */
#define PROSPERO_AUDIO_OUTPUT_RATE 48000

/* Free the SwrContext and forget the cached input description. */
void prospero_audio_resampler_destroy(void);

/* Flush + re-init the existing context (seek / codec flush). No-op if unset. */
void prospero_audio_resampler_reset(void);

/*
 * Ensure the resampler is configured for `frame`. Rebuilds it only when the
 * input layout/format/rate or `out_channels` (2 or 8) changed since last time.
 * `audio_ctx` supplies the fallback rate/layout when the frame lacks them; it
 * may be NULL. Returns 1 when ready, 0 on failure.
 */
int prospero_audio_resampler_configure(const AVFrame *frame,
                                       int out_channels,
                                       const AVCodecContext *audio_ctx);

/* The live SwrContext (NULL until configured) — for swr_convert / swr_get_delay. */
SwrContext *prospero_audio_resampler_ctx(void);

/* Sample rate the context was last configured for (0 until configured). */
int prospero_audio_resampler_input_rate(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AUDIO_RESAMPLE_H */
