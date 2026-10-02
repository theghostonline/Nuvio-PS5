/*
 * evo_adec — the audio decoder seam.
 *
 * Mirrors evo_vdec: one interface, an FFmpeg backend and a native
 * (libSceAudiodec / AJM) backend, chosen per stream. The native route covers
 * AAC and MP3 only — those are the two codecs proven callable through the
 * public sceAudiodec* family. AC-3, E-AC-3, DTS, FLAC, Vorbis and PCM have no
 * public native decoder on this firmware and stay on FFmpeg, so this is an
 * offload for part of the library, not a replacement for it.
 *
 * Evidence and ABI: third_party/ps5-audio-decoding-research/docs/{DECODING,
 * API-MATRIX}.md and examples/native-audio-poc/.
 */
#ifndef EVO_ADEC_H
#define EVO_ADEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_ADEC_BACKEND_FFMPEG = 0,
    EVO_ADEC_BACKEND_NATIVE = 1
} evo_adec_backend;

typedef struct evo_adec evo_adec;

typedef struct {
    int codec_id;        /* AVCodecID */
    int sample_rate;     /* from the stream's codecpar */
    int channels;
    const uint8_t *extradata;      /* AudioSpecificConfig for raw AAC, or NULL */
    int extradata_size;
} evo_adec_open_params;

/*
 * Load libSceAudiodec and report whether the native route is usable at all.
 * Call once early in boot, before evo_jailbreak_self(): the same credential
 * swap that makes libSceVideodec2 calls fail applies here.
 */
int evo_adec_native_probe(void);

/* Whether this codec has a native decoder, before committing to an open. */
int evo_adec_native_supports(int codec_id);

/*
 * Open a decoder for one stream. Falls back to reporting FFmpeg through
 * `chosen` when the native route is unavailable or refuses the stream; the
 * caller keeps its AVCodecContext either way.
 */
evo_adec *evo_adec_open(const evo_adec_open_params *p, evo_adec_backend *chosen);

/*
 * Decode one access unit. `pcm` receives interleaved signed-16 at the rate and
 * channel count reported by evo_adec_rate()/evo_adec_channels(). Returns the
 * number of bytes written, 0 for "no output from this AU", or negative on a
 * fatal decoder error — the caller should fall back to FFmpeg on negative.
 */
int evo_adec_decode(evo_adec *a, const uint8_t *data, int size,
                    int16_t *pcm, int pcm_capacity_bytes);

int evo_adec_rate(const evo_adec *a);
int evo_adec_channels(const evo_adec *a);
evo_adec_backend evo_adec_active(const evo_adec *a);

void evo_adec_flush(evo_adec *a);   /* seek */
void evo_adec_close(evo_adec *a);

#ifdef __cplusplus
}
#endif

#endif /* EVO_ADEC_H */
