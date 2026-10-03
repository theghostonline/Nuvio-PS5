/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nuvio_subs.h"

#include "ui_assets.h"

#include "evo_boot_trace.h"

#include <ass/ass.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <zlib.h>

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_TRACKS 64
#define MAX_STREAMS 128

typedef struct bmp_rect {
    ui_image img;
    int x, y;                 /* in the subtitle's own frame (src_w x src_h) */
} bmp_rect;

typedef struct bmp_event {
    int64_t start, end;       /* ms; end = INT64_MAX until the next event */
    int n;
    bmp_rect *r;
} bmp_event;

typedef struct strack {
    nuvio_sub_track info;
    int stream;               /* embedded stream index; -1 external */
    AVRational tb;
    int ass_raw;              /* packets are Matroska-style ASS lines */
    ASS_Track *ass;
    AVCodecContext *dec;      /* text-to-ASS or bitmap decoder */
    int src_w, src_h;         /* bitmap frame size */
    int64_t *seen;            /* packets already taken: start ms x content hash */
    int nseen, capseen;
    bmp_event *ev;
    int nev, capev;
    int need_flush;
    char *url, *headers;      /* external */
    int loading;
} strack;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ext_cond = PTHREAD_COND_INITIALIZER;
static ASS_Library *s_lib;
static ASS_Renderer *s_rend;
static strack s_tracks[MAX_TRACKS];
static int s_ntracks;
static int s_stream_map[MAX_STREAMS];
static int s_selected = -1;
static int s_delay_ms;
static int64_t s_start_ms;    /* video stream start: external cues are 0-based */
static nuvio_sub_style s_style = {100, 0xffffff, 0, 1, 0.0f, 0.0f};
static unsigned s_style_gen = 1;
static int s_session;         /* bumped by close: stale loader results are dropped */
static int s_loader_up;

/* What was last drawn, to tell when a redraw is needed. */
static struct {
    int sel;
    int drew;
    int64_t bmp_start;        /* start of the bitmap event drawn, INT64_MIN = none */
    nuvio_rect video;
    float lift;
    unsigned style_gen;
    int canvas_w, canvas_h;
} s_last = {-2, 0, INT64_MIN, {0, 0, 0, 0}, 0.0f, 0, 0, 0};

/* ---- fonts -------------------------------------------------------------- */

/*
 * The family name a font calls itself (name table, nameID 1).
 *
 * This matters because the renderer is set up with ASS_FONTPROVIDER_NONE:
 * there is no system font provider, so libass has no coverage-based fallback
 * and will not reach a face the style did not ask for by family name. Adding
 * a font with ass_add_font() is not enough - the style has to name it, and it
 * has to be the name inside the file ("Noto Naskh Arabic UI", not the file
 * stem). Measured on libass 0.17.5: an Arabic cue in a Roboto style logs
 * "failed to find any fallback with glyph 0x645" for every character and
 * draws .notdef boxes; the same cue in the real family name renders.
 */
static int font_family_name(const uint8_t *d, size_t n, char *out, size_t outn)
{
    if (!d || n < 12 || !out || outn < 2)
        return -1;
    const unsigned ntab = ((unsigned)d[4] << 8) | d[5];
    size_t off = 0, len = 0;
    for (unsigned i = 0; i < ntab; i++) {
        const size_t rec = 12 + (size_t)i * 16;
        if (rec + 16 > n)
            return -1;
        if (!memcmp(d + rec, "name", 4)) {
            off = ((size_t)d[rec + 8] << 24) | ((size_t)d[rec + 9] << 16) |
                  ((size_t)d[rec + 10] << 8) | (size_t)d[rec + 11];
            len = ((size_t)d[rec + 12] << 24) | ((size_t)d[rec + 13] << 16) |
                  ((size_t)d[rec + 14] << 8) | (size_t)d[rec + 15];
            break;
        }
    }
    if (len < 6 || off > n || off + len > n)
        return -1;
    const uint8_t *t = d + off;
    const unsigned count = ((unsigned)t[2] << 8) | t[3];
    const unsigned stroff = ((unsigned)t[4] << 8) | t[5];
    for (unsigned i = 0; i < count; i++) {
        if (6 + (size_t)i * 12 + 12 > len)
            break;
        const uint8_t *r = t + 6 + (size_t)i * 12;
        const unsigned pid = ((unsigned)r[0] << 8) | r[1];
        const unsigned nid = ((unsigned)r[6] << 8) | r[7];
        const unsigned slen = ((unsigned)r[8] << 8) | r[9];
        const unsigned so = ((unsigned)r[10] << 8) | r[11];
        if (nid != 1 || (size_t)stroff + so + slen > len)
            continue;
        const uint8_t *v = t + stroff + so;
        size_t k = 0;
        if (pid == 3 || pid == 0) {                  /* UTF-16BE */
            for (unsigned j = 1; j < slen && k + 1 < outn; j += 2)
                if (v[j - 1] == 0 && v[j] >= 0x20)
                    out[k++] = (char)v[j];
        } else {
            for (unsigned j = 0; j < slen && k + 1 < outn; j++)
                if (v[j] >= 0x20)
                    out[k++] = (char)v[j];
        }
        if (k) {
            out[k] = 0;
            return 0;
        }
    }
    return -1;
}

/* Family of the bundled Arabic face, read once at init. */
static char s_family_arabic[64];

static void add_font_asset_family(const char *name, ui_asset a, char *family, size_t fn)
{
    if (!a.data || !a.size)
        return;
    ass_add_font(s_lib, name, (const char *)a.data, (int)a.size);
    if (family && font_family_name(a.data, a.size, family, fn) == 0)
        evo_bt("subs: font %s is family '%s'", name, family);
}

static void add_font_asset(const char *name, ui_asset a)
{
    add_font_asset_family(name, a, NULL, 0);
}

/* The console's CJK / Thai fonts, added when a track needs them. */
static const char *add_system_font(const char *path)
{
    static char added[8][64];
    static char fams[8][64];
    static int nadded;
    struct stat st;
    for (int i = 0; i < nadded; i++)
        if (!strcmp(added[i], path))
            return fams[i][0] ? fams[i] : NULL;
    if (nadded >= 8 || stat(path, &st) != 0 || st.st_size <= 0)
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    char *buf = (char *)malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    const char *family = NULL;
    if (buf && got == (size_t)st.st_size) {
        ass_add_font(s_lib, path, buf, (int)got);
        ass_set_fonts(s_rend, NULL, "Roboto", ASS_FONTPROVIDER_NONE, NULL, 0);
        const int slot = nadded++;
        snprintf(added[slot], sizeof added[0], "%s", path);
        if (font_family_name((const uint8_t *)buf, got, fams[slot], sizeof fams[0]) == 0)
            family = fams[slot];
        evo_bt("subs: added font %s (family '%s')", path, family ? family : "?");
    }
    free(buf);
    return family;
}

/*
 * Loads the face a language needs and returns the family the style has to ask
 * for, or NULL to leave the default alone. With no font provider there is no
 * fallback, so naming the family is the only way the face is ever used.
 */
static const char *fonts_for_language(const char *lang)
{
    if (!lang || !*lang)
        return NULL;
    if (!strncasecmp(lang, "ar", 2) || !strncasecmp(lang, "ara", 3) ||
        !strncasecmp(lang, "fa", 2) || !strncasecmp(lang, "fas", 3) ||
        !strncasecmp(lang, "per", 3) || !strncasecmp(lang, "ur", 2) ||
        !strncasecmp(lang, "urd", 3))
        return s_family_arabic[0] ? s_family_arabic : NULL;   /* bundled */
    if (!strncasecmp(lang, "ja", 2) || !strncasecmp(lang, "jpn", 3))
        return add_system_font("/preinst/common/font/SSTJpPro-Regular.otf");
    if (!strncasecmp(lang, "ko", 2) || !strncasecmp(lang, "kor", 3))
        return add_system_font("/preinst/common/font/YoonGothicProSIE760.otf");
    if (!strncasecmp(lang, "zh", 2) || !strncasecmp(lang, "chi", 3) ||
        !strncasecmp(lang, "zho", 3) || !strncasecmp(lang, "cmn", 3) ||
        !strncasecmp(lang, "yue", 3))
        return add_system_font("/preinst/common/font/DFHEI5-SONY.ttf");
    if (!strncasecmp(lang, "th", 2))
        return add_system_font("/preinst/common/font/SSTThai-Roman.otf");
    return NULL;
}

static void ass_log(int level, const char *fmt, va_list va, void *data)
{
    (void)data;
    if (level > 2)
        return;
    char line[256];
    vsnprintf(line, sizeof line, fmt, va);
    evo_bt("libass: %s", line);
}

int nuvio_subs_init(void)
{
    if (s_lib)
        return 0;
    if (!(s_lib = ass_library_init()))
        return -1;
    ass_set_message_cb(s_lib, ass_log, NULL);
    ass_set_extract_fonts(s_lib, 1);
    add_font_asset("Roboto-Regular.ttf", ui_asset_font_roboto_regular());
    add_font_asset("Roboto-Bold.ttf", ui_asset_font_roboto_bold());
    add_font_asset_family("NotoNaskhArabicUI-Regular.ttf", ui_asset_font_naskh_regular(),
                          s_family_arabic, sizeof s_family_arabic);
    add_font_asset("NotoNaskhArabicUI-Bold.ttf", ui_asset_font_naskh_bold());
    add_font_asset("Inter-Regular.ttf", ui_asset_font_inter_regular());
    if (!(s_rend = ass_renderer_init(s_lib)))
        return -1;
    ass_set_fonts(s_rend, NULL, "Roboto", ASS_FONTPROVIDER_NONE, NULL, 0);
    ass_set_hinting(s_rend, ASS_HINTING_NONE);
    ass_set_shaper(s_rend, ASS_SHAPING_COMPLEX);
    ass_set_cache_limits(s_rend, 0, 64);
    for (int i = 0; i < MAX_STREAMS; i++)
        s_stream_map[i] = -1;
    return 0;
}

/* ---- Nuvio's style for text tracks ---------------------------------------- */

/* A colour as libass keeps it in ASS_Style: 0xRRGGBBAA, AA = transparency
 * (the parsed form of the file's &HAABBGGRR). */
static unsigned ass_colour(uint32_t rgb, float opacity)
{
    const unsigned a = (unsigned)((1.0f - opacity) * 255.0f + 0.5f);
    return ((rgb & 0xffffffu) << 8) | (a & 0xff);
}

static void apply_style(ASS_Track *t)
{
    if (!t || t->n_styles <= 0)
        return;
    /* Nuvio: clamp(30px, 4.4vh, 82px) at the 1080-line canvas. */
    const double size = 47.5 * (s_style.size_pct > 0 ? s_style.size_pct : 100) / 100.0;
    for (int i = 0; i < t->n_styles; i++) {
        ASS_Style *s = &t->styles[i];
        s->FontSize = size;
        s->PrimaryColour = ass_colour(s_style.color, 1.0f);
        s->Bold = s_style.bold ? 1 : 0;
        if (s_style.background > 0.01f) {
            s->BorderStyle = 3;                      /* opaque box */
            s->OutlineColour = ass_colour(0x000000, s_style.background);
            s->BackColour = ass_colour(0x000000, s_style.background);
            s->Outline = 6.0;
            s->Shadow = 0.0;
        } else {
            s->BorderStyle = 1;
            s->OutlineColour = ass_colour(0x000000, 1.0f);
            s->BackColour = ass_colour(0x000000, 0.75f);
            s->Outline = s_style.outline ? 2.6 : 0.0;
            s->Shadow = s_style.outline ? 1.2 : 0.0;
        }
        s->MarginV = (int)(54 + 10.8 * s_style.offset_pct);
    }
}

/*
 * Point a synthesized text track's styles at `family`. Only for tracks whose
 * header we wrote ourselves (SRT, WebVTT, addon subtitles): an authored ASS
 * file names its own fonts and we leave the author's choice alone.
 */
static void set_track_font(strack *t, const char *family)
{
    if (!t || !t->ass || t->ass_raw || !family || !*family)
        return;
    int changed = 0;
    for (int i = 0; i < t->ass->n_styles; i++) {
        ASS_Style *st = &t->ass->styles[i];
        if (st->FontName && !strcmp(st->FontName, family))
            continue;
        char *dup = strdup(family);
        if (!dup)
            continue;
        free(st->FontName);
        st->FontName = dup;
        changed = 1;
    }
    if (changed) {
        s_style_gen++;          /* the cached render is for the old face */
        evo_bt("subs: track font -> '%s'", family);
    }
}

static ASS_Track *new_text_track(void)
{
    static const char header[] =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n"
        "ScaledBorderAndShadow: yes\nWrapStyle: 0\n\n"
        "[V4+ Styles]\nFormat: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, "
        "Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Roboto,47.5,&H00FFFFFF,&H000000FF,&H00000000,&H40000000,0,0,0,0,100,100,0,"
        "0,1,2.6,1.2,2,120,120,54,1\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    ASS_Track *t = ass_new_track(s_lib);
    if (!t)
        return NULL;
    ass_process_codec_private(t, (char *)header, (int)sizeof header - 1);
    apply_style(t);
    return t;
}

/* ---- tracks --------------------------------------------------------------- */

static int is_bitmap_codec(enum AVCodecID id)
{
    return id == AV_CODEC_ID_HDMV_PGS_SUBTITLE || id == AV_CODEC_ID_DVD_SUBTITLE ||
           id == AV_CODEC_ID_DVB_SUBTITLE || id == AV_CODEC_ID_XSUB ||
           id == AV_CODEC_ID_DVB_TELETEXT;
}

static AVCodecContext *open_decoder(const AVCodecParameters *par, AVRational tb, const char *charenc)
{
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    if (!codec)
        return NULL;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;
    if (avcodec_parameters_to_context(ctx, par) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    ctx->pkt_timebase = tb;
    /* A flush must not restart the ASS ReadOrder: libass would take every
     * later line for a duplicate of an earlier one and drop it. */
    ctx->flags2 |= AV_CODEC_FLAG2_RO_FLUSH_NOOP;
    AVDictionary *o = NULL;
    if (charenc && *charenc)
        av_dict_set(&o, "sub_charenc", charenc, 0);
    int rc = avcodec_open2(ctx, codec, &o);
    av_dict_free(&o);
    if (rc < 0 && charenc && *charenc) {
        avcodec_free_context(&ctx);
        return open_decoder(par, tb, NULL);
    }
    if (rc < 0)
        avcodec_free_context(&ctx);
    return ctx;
}

static void free_events(strack *t)
{
    for (int i = 0; i < t->nev; i++) {
        for (int k = 0; k < t->ev[i].n; k++)
            ui_image_free(&t->ev[i].r[k].img);
        free(t->ev[i].r);
    }
    free(t->ev);
    t->ev = NULL;
    t->nev = t->capev = 0;
}

static void free_track(strack *t)
{
    if (t->ass)
        ass_free_track(t->ass);
    if (t->dec)
        avcodec_free_context(&t->dec);
    free_events(t);
    free(t->seen);
    free(t->url);
    free(t->headers);
    memset(t, 0, sizeof *t);
}

/* Whether this packet (start time and bytes) was fed before: a seek back
 * re-reads packets, and a cue must not appear twice. */
static int seen_before(strack *t, int64_t start, const uint8_t *data, int size)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < size; i++)
        h = (h ^ data[i]) * 16777619u;
    const int64_t key = start * 1000003LL ^ (int64_t)h ^ ((int64_t)size << 40);
    for (int i = t->nseen - 1; i >= 0; i--)
        if (t->seen[i] == key)
            return 1;
    if (t->nseen == t->capseen) {
        int cap = t->capseen ? t->capseen * 2 : 256;
        int64_t *n = (int64_t *)realloc(t->seen, (size_t)cap * sizeof *n);
        if (!n)
            return 0;
        t->seen = n;
        t->capseen = cap;
    }
    t->seen[t->nseen++] = key;
    return 0;
}

static void copy_meta(char *dst, size_t cap, AVDictionary *m, const char *key)
{
    const AVDictionaryEntry *e = av_dict_get(m, key, NULL, 0);
    snprintf(dst, cap, "%s", e && e->value ? e->value : "");
}

void nuvio_subs_open(AVFormatContext *fmt, int video_stream)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < MAX_STREAMS; i++)
        s_stream_map[i] = -1;
    s_start_ms = 0;
    if (fmt && video_stream >= 0 && video_stream < (int)fmt->nb_streams &&
        fmt->streams[video_stream]->start_time != AV_NOPTS_VALUE)
        s_start_ms = av_rescale_q(fmt->streams[video_stream]->start_time,
                                  fmt->streams[video_stream]->time_base, (AVRational){1, 1000});
    else if (fmt && fmt->start_time != AV_NOPTS_VALUE)
        s_start_ms = fmt->start_time / 1000;

    for (unsigned i = 0; fmt && i < fmt->nb_streams && i < MAX_STREAMS; i++) {
        AVStream *st = fmt->streams[i];
        const AVCodecParameters *par = st->codecpar;
        if (par->codec_type == AVMEDIA_TYPE_ATTACHMENT) {
            /* Fonts the file carries for its styled subtitles. */
            const AVDictionaryEntry *mime = av_dict_get(st->metadata, "mimetype", NULL, 0);
            const AVDictionaryEntry *name = av_dict_get(st->metadata, "filename", NULL, 0);
            int is_font = (mime && mime->value && (strstr(mime->value, "font") ||
                                                    strstr(mime->value, "truetype") ||
                                                    strstr(mime->value, "opentype"))) ||
                          (name && name->value && (strcasestr(name->value, ".ttf") ||
                                                   strcasestr(name->value, ".otf") ||
                                                   strcasestr(name->value, ".ttc")));
            if (is_font && par->extradata && par->extradata_size > 0)
                ass_add_font(s_lib, name && name->value ? name->value : "attachment",
                             (const char *)par->extradata, par->extradata_size);
            continue;
        }
        if (par->codec_type != AVMEDIA_TYPE_SUBTITLE || s_ntracks >= MAX_TRACKS)
            continue;
        strack *t = &s_tracks[s_ntracks];
        memset(t, 0, sizeof *t);
        t->stream = (int)i;
        t->tb = st->time_base;
        copy_meta(t->info.lang, sizeof t->info.lang, st->metadata, "language");
        copy_meta(t->info.title, sizeof t->info.title, st->metadata, "title");
        snprintf(t->info.codec, sizeof t->info.codec, "%s", avcodec_get_name(par->codec_id));
        t->info.bitmap = is_bitmap_codec(par->codec_id);
        t->info.forced = (st->disposition & AV_DISPOSITION_FORCED) != 0 ||
                         (t->info.title[0] && strcasestr(t->info.title, "forced"));
        t->info.is_default = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        t->info.hearing_impaired = (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED) != 0 ||
                                   (t->info.title[0] && (strcasestr(t->info.title, "sdh") ||
                                                         strcasestr(t->info.title, "cc")));
        t->info.state = 1;
        if (par->codec_id == AV_CODEC_ID_ASS || par->codec_id == AV_CODEC_ID_SSA) {
            t->ass_raw = 1;
            if ((t->ass = ass_new_track(s_lib)) && par->extradata && par->extradata_size > 0)
                ass_process_codec_private(t->ass, (char *)par->extradata, par->extradata_size);
        } else if (t->info.bitmap) {
            t->dec = open_decoder(par, st->time_base, NULL);
            t->src_w = par->width;
            t->src_h = par->height;
        } else {
            t->dec = open_decoder(par, st->time_base, NULL);
            t->ass = new_text_track();
            set_track_font(t, fonts_for_language(t->info.lang));
        }
        if (!t->ass && !t->dec) {
            evo_bt("subs: stream %u (%s) has no decoder", i, t->info.codec);
            continue;
        }
        s_stream_map[i] = s_ntracks++;
        evo_bt("subs: track %d stream %u %s lang=%s title='%s'%s%s", s_ntracks - 1, i,
               t->info.codec, t->info.lang, t->info.title, t->info.forced ? " forced" : "",
               t->info.is_default ? " default" : "");
    }
    pthread_mutex_unlock(&s_lock);
}

void nuvio_subs_close(void)
{
    pthread_mutex_lock(&s_lock);
    /* A track still downloading is freed too: the loader parses into its own
     * copy and drops the result once it sees the session has changed. */
    for (int i = 0; i < s_ntracks; i++)
        free_track(&s_tracks[i]);
    s_ntracks = 0;
    s_selected = -1;
    s_delay_ms = 0;
    s_session++;
    for (int i = 0; i < MAX_STREAMS; i++)
        s_stream_map[i] = -1;
    s_last.sel = -2;
    s_last.drew = 0;
    pthread_mutex_unlock(&s_lock);
}

int nuvio_subs_count(void)
{
    return s_ntracks;
}

int nuvio_subs_track(int i, nuvio_sub_track *out)
{
    int rc = -1;
    pthread_mutex_lock(&s_lock);
    if (i >= 0 && i < s_ntracks) {
        *out = s_tracks[i].info;
        rc = 0;
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

int nuvio_subs_selected(void)
{
    return s_selected;
}

void nuvio_subs_select(int id)
{
    pthread_mutex_lock(&s_lock);
    if (id < 0 || id >= s_ntracks)
        id = -1;
    if (id >= 0) {
        strack *t = &s_tracks[id];
        set_track_font(t, fonts_for_language(t->info.lang));
        if (t->info.bitmap) {
            /* Bitmap tracks decode only while selected: start clean. */
            free_events(t);
            t->nseen = 0;
            t->need_flush = 1;
        }
    }
    s_selected = id;
    pthread_mutex_unlock(&s_lock);
    evo_bt("subs: selected %d", id);
}

void nuvio_subs_set_delay_ms(int ms)
{
    s_delay_ms = ms;
}

int nuvio_subs_delay_ms(void)
{
    return s_delay_ms;
}

void nuvio_subs_set_style(const nuvio_sub_style *style)
{
    pthread_mutex_lock(&s_lock);
    s_style = *style;
    for (int i = 0; i < s_ntracks; i++)
        if (s_tracks[i].ass && !s_tracks[i].ass_raw)
            apply_style(s_tracks[i].ass);
    s_style_gen++;
    pthread_mutex_unlock(&s_lock);
}

void nuvio_subs_get_style(nuvio_sub_style *out)
{
    *out = s_style;
}

/* ---- packets -------------------------------------------------------------- */

static void add_bitmap_event(strack *t, const AVSubtitle *sub, int64_t start, int64_t end)
{
    /* An event closes the one before it when that had no end of its own. */
    if (t->nev > 0 && t->ev[t->nev - 1].end == INT64_MAX && t->ev[t->nev - 1].start <= start)
        t->ev[t->nev - 1].end = start;
    if (sub->num_rects == 0)
        return;               /* a clear: only ends the previous one */
    if (t->nev == t->capev) {
        int cap = t->capev ? t->capev * 2 : 64;
        bmp_event *n = (bmp_event *)realloc(t->ev, (size_t)cap * sizeof *n);
        if (!n)
            return;
        t->ev = n;
        t->capev = cap;
    }
    bmp_event *e = &t->ev[t->nev];
    e->start = start;
    e->end = end;
    e->n = 0;
    e->r = (bmp_rect *)calloc(sub->num_rects, sizeof *e->r);
    if (!e->r)
        return;
    for (unsigned k = 0; k < sub->num_rects; k++) {
        const AVSubtitleRect *r = sub->rects[k];
        if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0 || !r->data[0] || !r->data[1])
            continue;
        bmp_rect *br = &e->r[e->n];
        if (ui_image_alloc(&br->img, r->w, r->h) != 0)
            continue;
        const uint32_t *pal = (const uint32_t *)r->data[1];
        for (int y = 0; y < r->h; y++) {
            const uint8_t *src = r->data[0] + (size_t)y * r->linesize[0];
            uint32_t *dst = br->img.px + (size_t)y * r->w;
            for (int x = 0; x < r->w; x++) {
                const uint32_t c = pal[src[x]];          /* 0xAARRGGBB */
                const uint32_t a = c >> 24;
                const uint32_t rr = ((c >> 16) & 0xff) * a / 255;
                const uint32_t gg = ((c >> 8) & 0xff) * a / 255;
                const uint32_t bb = (c & 0xff) * a / 255;
                dst[x] = rr | (gg << 8) | (bb << 16) | (a << 24);
            }
        }
        br->x = r->x;
        br->y = r->y;
        e->n++;
    }
    if (e->n == 0) {
        free(e->r);
        return;
    }
    /* Keep events ordered by start (packets arrive in order except after a seek). */
    t->nev++;
    for (int i = t->nev - 1; i > 0 && t->ev[i - 1].start > t->ev[i].start; i--) {
        bmp_event tmp = t->ev[i];
        t->ev[i] = t->ev[i - 1];
        t->ev[i - 1] = tmp;
    }
}

/* Feeds one packet to a track. Caller holds s_lock. */
static void feed(strack *t, const AVPacket *pkt, AVRational tb, int64_t offset_ms)
{
    int64_t pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
    if (pts == AV_NOPTS_VALUE)
        return;
    const int64_t start = av_rescale_q(pts, tb, (AVRational){1, 1000}) + offset_ms;
    int64_t dur = pkt->duration > 0 ? av_rescale_q(pkt->duration, tb, (AVRational){1, 1000}) : 0;

    /* Matroska ASS carries a ReadOrder that libass de-duplicates by itself;
     * several events may share one start time, so no time-based check. */
    if (!t->ass_raw && seen_before(t, start, pkt->data, pkt->size))
        return;

    if (t->ass_raw && t->ass) {
        if (pkt->data && pkt->size > 0)
            ass_process_chunk(t->ass, (char *)pkt->data, pkt->size, start, dur > 0 ? dur : 5000);
        return;
    }
    if (!t->dec)
        return;
    if (t->need_flush) {
        avcodec_flush_buffers(t->dec);
        t->need_flush = 0;
    }
    AVSubtitle sub;
    int got = 0;
    memset(&sub, 0, sizeof sub);
    if (avcodec_decode_subtitle2(t->dec, &sub, &got, (AVPacket *)pkt) < 0 || !got)
        return;
    const int64_t s0 = start + sub.start_display_time;
    int64_t end = INT64_MAX;
    if (sub.end_display_time > sub.start_display_time && sub.end_display_time != UINT32_MAX)
        end = start + sub.end_display_time;
    else if (dur > 0)
        end = start + dur;

    if (t->info.bitmap) {
        add_bitmap_event(t, &sub, s0, end);
    } else if (t->ass) {
        for (unsigned k = 0; k < sub.num_rects; k++) {
            const AVSubtitleRect *r = sub.rects[k];
            if (r->type == SUBTITLE_ASS && r->ass)
                ass_process_chunk(t->ass, r->ass, (int)strlen(r->ass), s0,
                                  end == INT64_MAX ? 4000 : end - s0);
        }
    }
    avsubtitle_free(&sub);
}

void nuvio_subs_on_packet(AVFormatContext *fmt, const AVPacket *pkt)
{
    (void)fmt;
    if (!pkt || pkt->stream_index < 0 || pkt->stream_index >= MAX_STREAMS)
        return;
    const int id = s_stream_map[pkt->stream_index];
    if (id < 0)
        return;
    pthread_mutex_lock(&s_lock);
    strack *t = &s_tracks[id];
    /* Text tracks all keep their cues; a bitmap track decodes only while shown. */
    if (id < s_ntracks && (!t->info.bitmap || id == s_selected))
        feed(t, pkt, t->tb, 0);
    pthread_mutex_unlock(&s_lock);
}

void nuvio_subs_on_seek(void)
{
    pthread_mutex_lock(&s_lock);
    /* Only bitmap decoders keep state across packets (PGS display sets). */
    for (int i = 0; i < s_ntracks; i++)
        if (s_tracks[i].dec && s_tracks[i].info.bitmap)
            s_tracks[i].need_flush = 1;
    pthread_mutex_unlock(&s_lock);
}

/* ---- external subtitles -------------------------------------------------- */

static int valid_utf8(const uint8_t *p, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
        if (!len || i + len > n)
            return 0;
        for (int k = 1; k < len; k++)
            if ((p[i + k] & 0xc0) != 0x80)
                return 0;
        i += len;
    }
    return 1;
}

/* The likely legacy code page for a subtitle that is not UTF-8. */
static const char *charset_for(const char *lang)
{
    static const struct { const char *l; const char *cs; } k[] = {
        {"ar", "CP1256"}, {"fa", "CP1256"}, {"ur", "CP1256"},
        {"ru", "CP1251"}, {"uk", "CP1251"}, {"bg", "CP1251"}, {"sr", "CP1251"}, {"mk", "CP1251"},
        {"be", "CP1251"},
        {"el", "CP1253"}, {"he", "CP1255"}, {"iw", "CP1255"}, {"tr", "CP1254"},
        {"pl", "CP1250"}, {"cs", "CP1250"}, {"sk", "CP1250"}, {"hu", "CP1250"}, {"ro", "CP1250"},
        {"hr", "CP1250"}, {"sl", "CP1250"}, {"bs", "CP1250"},
        {"lt", "CP1257"}, {"lv", "CP1257"}, {"et", "CP1257"},
        {"vi", "CP1258"}, {"th", "CP874"},
        {"zh", "GB18030"}, {"ja", "SHIFT_JIS"}, {"ko", "CP949"},
    };
    for (unsigned i = 0; lang && i < sizeof k / sizeof k[0]; i++)
        if (!strncasecmp(lang, k[i].l, 2))
            return k[i].cs;
    return "CP1252";
}

typedef struct membuf {
    const uint8_t *p;
    size_t n, pos;
} membuf;

static int mem_read(void *opaque, uint8_t *buf, int size)
{
    membuf *m = (membuf *)opaque;
    size_t left = m->n - m->pos;
    if (!left)
        return AVERROR_EOF;
    if ((size_t)size > left)
        size = (int)left;
    memcpy(buf, m->p + m->pos, (size_t)size);
    m->pos += (size_t)size;
    return size;
}

static int64_t mem_seek(void *opaque, int64_t off, int whence)
{
    membuf *m = (membuf *)opaque;
    if (whence == AVSEEK_SIZE)
        return (int64_t)m->n;
    int64_t base = whence == SEEK_CUR ? (int64_t)m->pos : whence == SEEK_END ? (int64_t)m->n : 0;
    int64_t np = base + off;
    if (np < 0 || np > (int64_t)m->n)
        return -1;
    m->pos = (size_t)np;
    return np;
}

static uint8_t *download(const char *url, const char *headers, size_t *len)
{
    AVIOContext *io = NULL;
    AVDictionary *o = NULL;
    uint8_t *buf = NULL;
    size_t got = 0, cap = 0;
    *len = 0;
    av_dict_set(&o, "rw_timeout", "15000000", 0);
    av_dict_set(&o, "user_agent", "Mozilla/5.0 (PlayStation 5) NuvioPS5", 0);
    if (headers && *headers)
        av_dict_set(&o, "headers", headers, 0);
    if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &o) < 0) {
        av_dict_free(&o);
        return NULL;
    }
    av_dict_free(&o);
    for (;;) {
        if (got + 65536 > cap) {
            size_t nc = cap ? cap * 2 : 128 * 1024;
            if (nc > 24 * 1024 * 1024) break;
            uint8_t *nb = (uint8_t *)realloc(buf, nc + 1);
            if (!nb) break;
            buf = nb;
            cap = nc;
        }
        int r = avio_read(io, buf + got, (int)(cap - got));
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    avio_closep(&io);
    if (!got) {
        free(buf);
        return NULL;
    }
    *len = got;
    return buf;
}

/* gzip / zlib -> plain bytes (OpenSubtitles serves .gz). */
static uint8_t *gunzip(const uint8_t *in, size_t n, size_t *out_len)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 15 + 32) != Z_OK)
        return NULL;
    size_t cap = n * 4 + 65536, got = 0;
    uint8_t *out = (uint8_t *)malloc(cap + 1);
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)n;
    int rc = Z_OK;
    while (out && rc == Z_OK) {
        if (got == cap) {
            uint8_t *nb = (uint8_t *)realloc(out, cap * 2 + 1);
            if (!nb) { free(out); out = NULL; break; }
            out = nb;
            cap *= 2;
        }
        z.next_out = out + got;
        z.avail_out = (uInt)(cap - got);
        rc = inflate(&z, Z_NO_FLUSH);
        got = cap - z.avail_out;
    }
    inflateEnd(&z);
    if (!out || rc != Z_STREAM_END) {
        free(out);
        return NULL;
    }
    *out_len = got;
    return out;
}

/* Loads one external track into a fresh libass track (or bitmap events). */
static int load_external(strack *t, const char *url, const char *headers)
{
    size_t n = 0;
    uint8_t *data = download(url, headers, &n);
    if (!data)
        return -1;
    if (n > 2 && data[0] == 0x1f && data[1] == 0x8b) {
        size_t m = 0;
        uint8_t *plain = gunzip(data, n, &m);
        free(data);
        if (!plain)
            return -1;
        data = plain;
        n = m;
    }
    const char *charenc = NULL;
    const int bom16 = n > 2 && ((data[0] == 0xff && data[1] == 0xfe) || (data[0] == 0xfe && data[1] == 0xff));
    if (!bom16 && !valid_utf8(data, n))
        charenc = charset_for(t->info.lang);

    membuf mb = {data, n, 0};
    uint8_t *iobuf = (uint8_t *)av_malloc(32768);
    AVIOContext *pb = iobuf ? avio_alloc_context(iobuf, 32768, 0, &mb, mem_read, NULL, mem_seek) : NULL;
    AVFormatContext *fmt = avformat_alloc_context();
    int rc = -1;
    if (!pb || !fmt) {
        av_free(iobuf);
        goto out;
    }
    fmt->pb = pb;
    if (avformat_open_input(&fmt, NULL, NULL, NULL) < 0) {
        fmt = NULL;
        goto out;
    }
    avformat_find_stream_info(fmt, NULL);
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, NULL, 0);
    if (si < 0)
        goto out;
    AVStream *st = fmt->streams[si];
    const enum AVCodecID cid = st->codecpar->codec_id;
    t->tb = st->time_base;
    snprintf(t->info.codec, sizeof t->info.codec, "%s", avcodec_get_name(cid));
    t->info.bitmap = is_bitmap_codec(cid);
    if (cid == AV_CODEC_ID_ASS || cid == AV_CODEC_ID_SSA) {
        t->ass_raw = 1;
        t->ass = ass_new_track(s_lib);
        if (t->ass && st->codecpar->extradata)
            ass_process_codec_private(t->ass, (char *)st->codecpar->extradata,
                                      st->codecpar->extradata_size);
    } else {
        t->dec = open_decoder(st->codecpar, st->time_base, charenc);
        if (!t->info.bitmap) {
            t->ass = new_text_track();
            set_track_font(t, fonts_for_language(t->info.lang));
        }
        t->src_w = st->codecpar->width;
        t->src_h = st->codecpar->height;
    }
    if (!t->ass && !t->dec)
        goto out;
    AVPacket *pkt = av_packet_alloc();
    int events = 0;
    while (pkt && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si) {
            feed(t, pkt, st->time_base, 0);
            events++;
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (t->dec && !t->info.bitmap)
        avcodec_free_context(&t->dec);     /* everything is in the libass track now */
    rc = events > 0 ? 0 : -1;
    evo_bt("subs: external %s %s, %d cues%s%s", rc == 0 ? "ok" : "EMPTY", t->info.codec, events,
           charenc ? " charset=" : "", charenc ? charenc : "");
out:
    if (fmt)
        avformat_close_input(&fmt);
    if (pb) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
    }
    free(data);
    return rc;
}

static void *ext_loader(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        int id = -1;
        while (id < 0) {
            for (int i = 0; i < s_ntracks; i++)
                if (s_tracks[i].info.external && s_tracks[i].info.state == 0 && !s_tracks[i].loading) {
                    id = i;
                    break;
                }
            if (id < 0)
                pthread_cond_wait(&s_ext_cond, &s_lock);
        }
        const int session = s_session;
        strack work;
        memset(&work, 0, sizeof work);
        work.info = s_tracks[id].info;
        work.stream = -1;
        char *url = strdup(s_tracks[id].url ? s_tracks[id].url : "");
        char *headers = s_tracks[id].headers ? strdup(s_tracks[id].headers) : NULL;
        s_tracks[id].loading = 1;
        pthread_mutex_unlock(&s_lock);

        /* Parsed outside the lock into a private track, then swapped in. */
        const int rc = url ? load_external(&work, url, headers) : -1;
        free(url);
        free(headers);

        pthread_mutex_lock(&s_lock);
        strack *t = &s_tracks[id];
        if (session != s_session) {
            free_track(&work);             /* the playback it belonged to is over */
        } else {
            t->loading = 0;
            if (rc == 0) {
                char *u = t->url, *h = t->headers;
                nuvio_sub_track info = t->info;
                *t = work;
                t->url = u;
                t->headers = h;
                snprintf(info.codec, sizeof info.codec, "%s", work.info.codec);
                info.bitmap = work.info.bitmap;
                info.state = 1;
                t->info = info;
                if (s_selected == id)
                    set_track_font(t, fonts_for_language(t->info.lang));
            } else {
                free_track(&work);
                t->info.state = -1;
            }
        }
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

int nuvio_subs_add_external(const char *url, const char *lang, const char *label,
                            const char *headers)
{
    int id = -1;
    if (!url || !*url)
        return -1;
    pthread_mutex_lock(&s_lock);
    if (!s_loader_up) {
        pthread_t th;
        if (pthread_create(&th, NULL, ext_loader, NULL) == 0) {
            pthread_detach(th);
            s_loader_up = 1;
        }
    }
    if (s_ntracks < MAX_TRACKS) {
        id = s_ntracks++;
        strack *t = &s_tracks[id];
        memset(t, 0, sizeof *t);
        t->stream = -1;
        t->info.external = 1;
        t->info.state = 0;
        snprintf(t->info.lang, sizeof t->info.lang, "%s", lang ? lang : "");
        snprintf(t->info.title, sizeof t->info.title, "%s", label ? label : "");
        snprintf(t->info.codec, sizeof t->info.codec, "%s", "external");
        t->url = strdup(url);
        t->headers = headers && *headers ? strdup(headers) : NULL;
        pthread_cond_signal(&s_ext_cond);
    }
    pthread_mutex_unlock(&s_lock);
    return id;
}

/* ---- rendering ------------------------------------------------------------ */

static void blend_ass(ui_canvas *c, const ASS_Image *img)
{
    for (; img; img = img->next) {
        if (img->w <= 0 || img->h <= 0)
            continue;
        const uint32_t col = img->color;                /* RRGGBBAA, AA = transparency */
        const uint32_t a = 255 - (col & 0xff);
        if (!a)
            continue;
        ui_mask m = {img->bitmap, img->w, img->h, img->stride};
        ui_draw_mask(c, &m, img->dst_x, img->dst_y, (a << 24) | (col >> 8));
    }
}

int nuvio_subs_render(ui_canvas *c, int64_t pts_us, nuvio_rect v, float lift)
{
    pthread_mutex_lock(&s_lock);
    const int sel = s_selected;
    const strack *t = (sel >= 0 && sel < s_ntracks) ? &s_tracks[sel] : NULL;
    const int ready = t && t->info.state == 1 && !t->loading;
    const int layout_changed = s_last.sel != sel || s_last.video.x != v.x || s_last.video.y != v.y ||
                               s_last.video.w != v.w || s_last.video.h != v.h ||
                               s_last.lift != lift || s_last.style_gen != s_style_gen ||
                               s_last.canvas_w != c->w || s_last.canvas_h != c->h;
    int changed = 0;

    if (!ready) {
        if (s_last.drew || layout_changed) {
            ui_canvas_clear(c);
            changed = s_last.drew;
            s_last.drew = 0;
        }
        goto done;
    }

    int64_t now = pts_us / 1000 - s_delay_ms;
    if (t->info.external)
        now -= s_start_ms;

    if (t->ass) {
        ass_set_frame_size(s_rend, c->w, c->h);
        ass_set_storage_size(s_rend, (int)(v.w + 0.5f), (int)(v.h + 0.5f));
        const int top = (int)(v.y + 0.5f), left = (int)(v.x + 0.5f);
        const int bottom = c->h - (int)(v.y + v.h + 0.5f), right = c->w - (int)(v.x + v.w + 0.5f);
        ass_set_margins(s_rend, top, bottom, left, right);
        /* Plain text may sit in the letterbox bars; styled ASS keeps to the picture. */
        ass_set_use_margins(s_rend, t->ass_raw ? 0 : 1);
        ass_set_line_position(s_rend, lift > 0.0f ? (double)lift * 100.0 / c->h : 0.0);
        int chg = 0;
        ASS_Image *img = ass_render_frame(s_rend, t->ass, (long long)now, &chg);
        if (chg || layout_changed) {
            ui_canvas_clear(c);
            blend_ass(c, img);
            changed = 1;
            s_last.drew = img != NULL;
        }
    } else {
        int idx = -1;
        for (int i = t->nev - 1; i >= 0; i--)
            if (t->ev[i].start <= now) {
                if (now < t->ev[i].end)
                    idx = i;
                break;
            }
        const int64_t ev_start = idx >= 0 ? t->ev[idx].start : INT64_MIN;
        if (ev_start != s_last.bmp_start || layout_changed) {
            ui_canvas_clear(c);
            if (idx >= 0) {
                const bmp_event *e = &t->ev[idx];
                const float sw = t->src_w > 0 ? (float)t->src_w : 1920.0f;
                const float sh = t->src_h > 0 ? (float)t->src_h : 1080.0f;
                const float kx = v.w / sw, ky = v.h / sh;
                const float dy = -lift;     /* lift like text */
                for (int k = 0; k < e->n; k++)
                    ui_draw_image(c, &e->r[k].img, v.x + e->r[k].x * kx, v.y + e->r[k].y * ky + dy,
                                  e->r[k].img.w * kx, e->r[k].img.h * ky, 1.0f);
            }
            changed = s_last.drew || idx >= 0;
            s_last.drew = idx >= 0;
            s_last.bmp_start = ev_start;
        }
    }

done:
    s_last.sel = sel;
    s_last.video = v;
    s_last.lift = lift;
    s_last.style_gen = s_style_gen;
    s_last.canvas_w = c->w;
    s_last.canvas_h = c->h;
    pthread_mutex_unlock(&s_lock);
    return changed;
}

int nuvio_subs_visible(void)
{
    return s_last.drew;
}
