/*
 * evo_adec_native.c - AAC and MP3 decode through libSceAudiodec.
 *
 * The public sceAudiodec* family is AJM-backed: one access unit in, interleaved
 * PCM out, with the decoder reporting the real rate and channel count after a
 * successful decode. Two codecs are proven callable this way (see
 * third_party/ps5-audio-decoding-research/docs/API-MATRIX.md):
 *
 *   codec 3  AAC - ConfigNumber 2 ("raw AAC blocks") for container streams with
 *                  extradata (MP4/MKV), or ConfigNumber 1 (self-describing ADTS)
 *                  when no container extradata is present.
 *   codec 2  MP3
 *
 * AC-3, E-AC-3 and DTS - the common MKV audio codecs - have no public native
 * decoder on this firmware and stay on FFmpeg. This is an offload for part of
 * the library, not a replacement for it.
 *
 * Lifetime mirrors evo_vdec_native: the sysmodule load and library init happen
 * once in evo_adec_native_probe(), which must run BEFORE evo_jailbreak_self()
 * because the credential swap that makes libSceVideodec2 calls fail applies
 * here too. The per-stream decoder handle is created on open, deleted on close.
 */

#include "evo_adec.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libavcodec/avcodec.h>

#include "evo_boot_log.h"

#ifdef EVO_APP_MODULE

#define SCE_SYSMODULE_AUDIODEC_NUM 0x0088

#define EVO_ADEC_CODEC_MP3 2u
#define EVO_ADEC_CODEC_AAC 3u
#define EVO_ADEC_WORD_S16  1

/* Public sceAudiodec control ABI. The sizes are asserted below: getting these
 * wrong is silent memory corruption, not a link error. */
typedef struct {
    uint32_t size;
    void    *address;
    uint32_t length;
} sce_adec_au_info;

typedef struct {
    uint32_t size;
    void    *address;
    uint32_t length;
} sce_adec_pcm_item;

typedef struct {
    void *param;
    void *stream_info;
    sce_adec_au_info  *au_info;
    sce_adec_pcm_item *pcm_item;
} sce_adec_control;

typedef struct {
    uint32_t size;
    int32_t  word_size;
    uint32_t config_number;
    uint32_t sampling_frequency_index;
    uint32_t max_channels;
    uint32_t enable_he_aac;
} sce_adec_aac_param;

typedef struct {
    uint32_t size;
    uint32_t sampling_frequency;
    uint32_t channel_count;
    uint32_t he_aac;
    int32_t  result;
} sce_adec_aac_info;

typedef struct {
    uint32_t size;
    int32_t  word_size;
} sce_adec_mp3_param;

typedef struct {
    uint32_t size;
    uint32_t header;         /* 4 bytes: raw 32-bit MPEG audio frame header */
    uint8_t  crc;            /* 1 byte */
    uint8_t  mode;           /* 1 byte: 0=Stereo, 1=JointStereo, 2=Dual, 3=Mono */
    uint8_t  mode_extension; /* 1 byte */
    uint8_t  copyright;      /* 1 byte */
    uint8_t  original;       /* 1 byte */
    uint8_t  emphasis;       /* 1 byte */
    uint8_t  reserved[2];    /* 2 bytes */
    int32_t  result;         /* 4 bytes */
} sce_adec_mp3_info;

_Static_assert(sizeof(sce_adec_au_info)   == 24, "au_info ABI");
_Static_assert(sizeof(sce_adec_pcm_item)  == 24, "pcm_item ABI");
_Static_assert(sizeof(sce_adec_control)   == 32, "control ABI");
_Static_assert(sizeof(sce_adec_aac_param) == 24, "aac param ABI");
_Static_assert(sizeof(sce_adec_aac_info)  == 20, "aac info ABI");
_Static_assert(sizeof(sce_adec_mp3_param) ==  8, "mp3 param ABI");
_Static_assert(sizeof(sce_adec_mp3_info)  == 20, "mp3 info ABI");

extern int sceSysmoduleLoadModule(uint16_t id);
extern int sceAudiodecInitLibrary(uint32_t codec_type);
extern int sceAudiodecCreateDecoder(sce_adec_control *control, uint32_t codec_type);
extern int sceAudiodecDeleteDecoder(int handle);
extern int sceAudiodecDecode(int handle, sce_adec_control *control);

/* The raw-block sampling-frequency table the AAC parameter indexes into. */
static const int k_aac_rates[] = {
    96000, 88200, 64000, 48000, 44100, 32000,
    24000, 22050, 16000, 12000, 11025, 8000
};

struct evo_adec {
    uint32_t codec_type;
    int      handle;
    int      rate;
    int      channels;
    int      fatal;

    sce_adec_aac_param aac_param;
    sce_adec_aac_info  aac_info;
    sce_adec_mp3_param mp3_param;
    sce_adec_mp3_info  mp3_info;

    sce_adec_au_info   au;
    sce_adec_pcm_item  pcm;
    sce_adec_control   control;
};

static int g_probe_tried;
static int g_aac_ok;
static int g_mp3_ok;

static int aac_rate_index(int rate)
{
    for (unsigned i = 0; i < sizeof(k_aac_rates) / sizeof(k_aac_rates[0]); ++i) {
        if (k_aac_rates[i] == rate)
            return (int)i;
    }
    return 4;   /* 44100 - the least surprising default */
}

int evo_adec_native_probe(void)
{
    if (g_probe_tried)
        return g_aac_ok || g_mp3_ok;
    g_probe_tried = 1;

    int sm = sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIODEC_NUM);

    int aac_rc = sceAudiodecInitLibrary(EVO_ADEC_CODEC_AAC);
    g_aac_ok = (aac_rc >= 0);
    int mp3_rc = sceAudiodecInitLibrary(EVO_ADEC_CODEC_MP3);
    g_mp3_ok = (mp3_rc >= 0);

    /* Both libraries stay initialised for the life of the process, like the
     * resident video decoder: there is no point tearing them down between
     * files, and sceAudiodecTermLibrary is deliberately never imported - an
     * unused .syms entry becomes a dead positional import and the loader
     * rejects the whole module. */
    evo_boot_log("EVO adec native: sysmod=0x%08x aac_init=0x%08x mp3_init=0x%08x",
                 (unsigned)sm, (unsigned)aac_rc, (unsigned)mp3_rc);
    return g_aac_ok || g_mp3_ok;
}

int evo_adec_native_supports(int codec_id)
{
    if (!evo_adec_native_probe())
        return 0;
    if (codec_id == AV_CODEC_ID_AAC)
        return g_aac_ok;
    if (codec_id == AV_CODEC_ID_MP3)
        return g_mp3_ok;
    return 0;
}

evo_adec *evo_adec_open(const evo_adec_open_params *p, evo_adec_backend *chosen)
{
    if (chosen)
        *chosen = EVO_ADEC_BACKEND_FFMPEG;
    if (!p || !evo_adec_native_supports(p->codec_id))
        return NULL;

    /*
     * Public AAC exposes up to six channels, but only mono and stereo are
     * runtime-proven. A 5.1 AAC track goes to FFmpeg rather than risking a
     * silent or half-decoded surround mix.
     */
    if (p->channels <= 0 || p->channels > 2)
        return NULL;

    evo_adec *a = (evo_adec *)calloc(1, sizeof(*a));
    if (!a)
        return NULL;

    a->handle   = -1;
    a->rate     = p->sample_rate > 0 ? p->sample_rate : 48000;
    a->channels = p->channels;

    if (p->codec_id == AV_CODEC_ID_AAC) {
        a->codec_type = EVO_ADEC_CODEC_AAC;
        a->aac_param.size = sizeof(a->aac_param);
        a->aac_param.word_size = EVO_ADEC_WORD_S16;
        /* 1 = self-describing ADTS (no extradata), 2 = raw AAC blocks (MP4/MKV extradata) */
        a->aac_param.config_number = (p->extradata && p->extradata_size > 0) ? 2 : 1;
        a->aac_param.sampling_frequency_index = (uint32_t)aac_rate_index(a->rate);
        a->aac_param.max_channels = (uint32_t)a->channels;
        a->aac_param.enable_he_aac = 0;
        a->aac_info.size = sizeof(a->aac_info);
        a->control.param = &a->aac_param;
        a->control.stream_info = &a->aac_info;
    } else {
        a->codec_type = EVO_ADEC_CODEC_MP3;
        a->mp3_param.size = sizeof(a->mp3_param);
        a->mp3_param.word_size = EVO_ADEC_WORD_S16;
        a->mp3_info.size = sizeof(a->mp3_info);
        a->control.param = &a->mp3_param;
        a->control.stream_info = &a->mp3_info;
    }

    a->au.size  = sizeof(a->au);
    a->pcm.size = sizeof(a->pcm);
    a->control.au_info  = &a->au;
    a->control.pcm_item = &a->pcm;

    a->handle = sceAudiodecCreateDecoder(&a->control, a->codec_type);
    if (a->handle < 0) {
        evo_boot_log("EVO adec native: CreateDecoder(%u) failed 0x%08x -> FFmpeg",
                     a->codec_type, (unsigned)a->handle);
        free(a);
        return NULL;
    }

    evo_boot_log("EVO adec native: OPEN ok codec=%u %dch %dHz",
                 a->codec_type, a->channels, a->rate);
    if (chosen)
        *chosen = EVO_ADEC_BACKEND_NATIVE;
    return a;
}

int evo_adec_decode(evo_adec *a, const uint8_t *data, int size,
                    int16_t *pcm, int pcm_capacity_bytes)
{
    if (!a || a->fatal || a->handle < 0 || !data || size <= 0 ||
        !pcm || pcm_capacity_bytes <= 0) {
        return -1;
    }

    a->au.size     = sizeof(a->au);
    a->au.address  = (void *)(uintptr_t)data;
    a->au.length   = (uint32_t)size;
    a->pcm.size    = sizeof(a->pcm);
    a->pcm.address = pcm;
    a->pcm.length  = (uint32_t)pcm_capacity_bytes;

    int rc = sceAudiodecDecode(a->handle, &a->control);
    if (rc < 0 || (a->codec_type == EVO_ADEC_CODEC_AAC && a->aac_info.result < 0) ||
        (a->codec_type == EVO_ADEC_CODEC_MP3 && a->mp3_info.result < 0)) {
        /*
         * One bad AU is not a dead decoder, but a decoder that keeps refusing
         * is - and the caller cannot tell them apart mid-stream. Latch fatal so
         * playback falls back to FFmpeg instead of emitting silence forever.
         */
        a->fatal = 1;
        int info_res = (a->codec_type == EVO_ADEC_CODEC_AAC) ? a->aac_info.result : a->mp3_info.result;
        evo_boot_log("EVO adec native: Decode failed rc=0x%08x info_res=%d -> FFmpeg fallback",
                     (unsigned)rc, info_res);
        return -1;
    }

    /* Trust what the decoder reports over what the container header claimed. */
    if (a->codec_type == EVO_ADEC_CODEC_AAC) {
        if (a->aac_info.sampling_frequency > 0)
            a->rate = (int)a->aac_info.sampling_frequency;
        if (a->aac_info.channel_count > 0)
            a->channels = (int)a->aac_info.channel_count;
    } else {
        /* MP3: mode 3 is single channel (mono); mode 0, 1, 2 are stereo/joint/dual */
        a->channels = (a->mp3_info.mode == 3) ? 1 : 2;
        /* Parse sample rate from MPEG header if present */
        for (int i = 0; i <= size - 4 && i < 32; i++) {
            if (data[i] == 0xff && (data[i + 1] & 0xe0) == 0xe0) {
                uint32_t version = (data[i + 1] >> 3) & 3;
                uint32_t layer = (data[i + 1] >> 1) & 3;
                uint32_t rate_index = (data[i + 2] >> 2) & 3;
                if (version != 1 && layer == 1 && rate_index < 3) {
                    static const int k_base_mp3_rates[4] = { 44100, 48000, 32000, 0 };
                    int r = k_base_mp3_rates[rate_index];
                    if (version == 2) r /= 2;
                    else if (version == 0) r /= 4;
                    if (r > 0) a->rate = r;
                    break;
                }
            }
        }
    }

    uint32_t produced = a->pcm.length;
    if (produced > (uint32_t)pcm_capacity_bytes || (produced & 1u) != 0u) {
        a->fatal = 1;
        evo_boot_log("EVO adec native: bogus PCM length %u (cap %d) -> FFmpeg fallback",
                     produced, pcm_capacity_bytes);
        return -1;
    }
    return (int)produced;
}

int evo_adec_rate(const evo_adec *a)     { return a ? a->rate : 0; }
int evo_adec_channels(const evo_adec *a) { return a ? a->channels : 0; }

evo_adec_backend evo_adec_active(const evo_adec *a)
{
    return (a && a->handle >= 0) ? EVO_ADEC_BACKEND_NATIVE : EVO_ADEC_BACKEND_FFMPEG;
}

void evo_adec_flush(evo_adec *a)
{
    /*
     * There is no public sceAudiodec reset. AAC and MP3 both resynchronise from
     * the next frame, so a seek only needs the fatal latch cleared - recreating
     * the handle would cost a round trip for nothing.
     */
    if (a)
        a->fatal = 0;
}

void evo_adec_close(evo_adec *a)
{
    if (!a)
        return;
    if (a->handle >= 0)
        sceAudiodecDeleteDecoder(a->handle);
    free(a);
}

#else  /* !EVO_APP_MODULE - host build: no native audio decode */

int evo_adec_native_probe(void) { return 0; }
int evo_adec_native_supports(int codec_id) { (void)codec_id; return 0; }
evo_adec *evo_adec_open(const evo_adec_open_params *p, evo_adec_backend *chosen)
{
    (void)p;
    if (chosen) *chosen = EVO_ADEC_BACKEND_FFMPEG;
    return NULL;
}
int evo_adec_decode(evo_adec *a, const uint8_t *d, int s, int16_t *p, int c)
{ (void)a; (void)d; (void)s; (void)p; (void)c; return -1; }
int evo_adec_rate(const evo_adec *a)     { (void)a; return 0; }
int evo_adec_channels(const evo_adec *a) { (void)a; return 0; }
evo_adec_backend evo_adec_active(const evo_adec *a)
{ (void)a; return EVO_ADEC_BACKEND_FFMPEG; }
void evo_adec_flush(evo_adec *a) { (void)a; }
void evo_adec_close(evo_adec *a) { (void)a; }

#endif /* EVO_APP_MODULE */
