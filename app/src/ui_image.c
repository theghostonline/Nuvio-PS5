/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui_image.h"

#include "evo_boot_trace.h"

#include <stdio.h>    /* jpeglib.h needs FILE */
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include <libavformat/avio.h>
#include <libavutil/dict.h>

#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 24
#define MAX_FETCH (16 * 1024 * 1024)

enum { EMPTY = 0, PENDING, LOADING, READY, FAILED };

typedef struct slot {
    char url[1024];
    int max_w, max_h, blur;
    int state;
    int discard;           /* cleared while loading: drop the result */
    unsigned seq;          /* request order, for FIFO and eviction */
    ui_image img;
} slot;

static slot s_slots[SLOTS];
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static int s_worker;
static unsigned s_seq;
static volatile unsigned s_generation;

/* ---- decoding --------------------------------------------------------------- */

static void premultiply(ui_image *img)
{
    const size_t n = (size_t)img->w * (size_t)img->h;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = img->px[i];
        uint32_t a = p >> 24;
        if (a == 255)
            continue;
        uint32_t r = ((p & 0xff) * a + 127) / 255;
        uint32_t g = (((p >> 8) & 0xff) * a + 127) / 255;
        uint32_t b = (((p >> 16) & 0xff) * a + 127) / 255;
        img->px[i] = r | (g << 8) | (b << 16) | (a << 24);
    }
}

static int decode_png(const uint8_t *d, size_t n, ui_image *out)
{
    png_image pi;
    memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&pi, d, n))
        return -1;
    pi.format = PNG_FORMAT_RGBA;
    if (ui_image_alloc(out, (int)pi.width, (int)pi.height) != 0) {
        png_image_free(&pi);
        return -1;
    }
    if (!png_image_finish_read(&pi, NULL, out->px, (png_int_32)(pi.width * 4), NULL)) {
        ui_image_free(out);
        return -1;
    }
    return 0;
}

struct jerr {
    struct jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_fail(j_common_ptr cinfo)
{
    longjmp(((struct jerr *)cinfo->err)->jump, 1);
}

static int decode_jpeg(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    struct jpeg_decompress_struct ci;
    struct jerr je;
    memset(out, 0, sizeof *out);
    ci.err = jpeg_std_error(&je.pub);
    je.pub.error_exit = jpeg_fail;
    if (setjmp(je.jump)) {
        jpeg_destroy_decompress(&ci);
        ui_image_free(out);
        return -1;
    }
    jpeg_create_decompress(&ci);
    jpeg_mem_src(&ci, d, (unsigned long)n);
    jpeg_read_header(&ci, TRUE);
    /* Decode no larger than needed: libjpeg scales by 1/2, 1/4, 1/8 for free. */
    ci.scale_num = 1;
    ci.scale_denom = 1;
    while (ci.scale_denom < 8 && (int)(ci.image_width / (ci.scale_denom * 2)) >= max_w &&
           (int)(ci.image_height / (ci.scale_denom * 2)) >= max_h)
        ci.scale_denom *= 2;
    ci.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&ci);
    if (ui_image_alloc(out, (int)ci.output_width, (int)ci.output_height) != 0) {
        jpeg_destroy_decompress(&ci);
        return -1;
    }
    while (ci.output_scanline < ci.output_height) {
        JSAMPROW row = (JSAMPROW)(out->px + (size_t)ci.output_scanline * out->w);
        jpeg_read_scanlines(&ci, &row, 1);
    }
    jpeg_finish_decompress(&ci);
    jpeg_destroy_decompress(&ci);
    return 0;
}

static int decode_webp(const uint8_t *d, size_t n, ui_image *out)
{
    int w = 0, h = 0;
    uint8_t *rgba = WebPDecodeRGBA(d, n, &w, &h);
    if (!rgba)
        return -1;
    if (ui_image_alloc(out, w, h) != 0) {
        WebPFree(rgba);
        return -1;
    }
    memcpy(out->px, rgba, (size_t)w * h * 4);
    WebPFree(rgba);
    return 0;
}

static int decode_svg(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    char *text = (char *)malloc(n + 1);
    if (!text)
        return -1;
    memcpy(text, d, n);
    text[n] = 0;
    NSVGimage *svg = nsvgParse(text, "px", 96.0f);
    free(text);
    if (!svg || svg->width <= 0.0f || svg->height <= 0.0f) {
        if (svg) nsvgDelete(svg);
        return -1;
    }
    float s = (float)max_w / svg->width;
    if (svg->height * s > (float)max_h)
        s = (float)max_h / svg->height;
    const int w = (int)(svg->width * s + 0.5f), h = (int)(svg->height * s + 0.5f);
    NSVGrasterizer *r = nsvgCreateRasterizer();
    int rc = -1;
    if (r && w > 0 && h > 0 && ui_image_alloc(out, w, h) == 0) {
        nsvgRasterize(r, svg, 0, 0, s, (unsigned char *)out->px, w, h, w * 4);
        rc = 0;
    }
    if (r) nsvgDeleteRasterizer(r);
    nsvgDelete(svg);
    return rc;
}

static int looks_like_svg(const uint8_t *d, size_t n)
{
    const size_t lim = n < 1024 ? n : 1024;
    for (size_t i = 0; i + 4 <= lim; i++)
        if (d[i] == '<' && d[i + 1] == 's' && d[i + 2] == 'v' && d[i + 3] == 'g')
            return 1;
    return 0;
}

int ui_image_decode(const uint8_t *d, size_t n, int max_w, int max_h, ui_image *out)
{
    int rc;
    ui_image raw;
    memset(&raw, 0, sizeof raw);
    memset(out, 0, sizeof *out);
    if (!d || n < 12)
        return -1;
    if (!memcmp(d, "\x89PNG", 4))
        rc = decode_png(d, n, &raw);
    else if (d[0] == 0xff && d[1] == 0xd8)
        rc = decode_jpeg(d, n, max_w, max_h, &raw);
    else if (!memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        rc = decode_webp(d, n, &raw);
    else if (looks_like_svg(d, n))
        rc = decode_svg(d, n, max_w, max_h, &raw);
    else
        rc = -1;
    if (rc != 0)
        return -1;
    premultiply(&raw);
    if (raw.w > max_w || raw.h > max_h) {
        float s = (float)max_w / raw.w;
        if (raw.h * s > (float)max_h)
            s = (float)max_h / raw.h;
        int w = (int)(raw.w * s + 0.5f), h = (int)(raw.h * s + 0.5f);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        rc = ui_image_resize(&raw, w, h, out);
        ui_image_free(&raw);
        return rc;
    }
    *out = raw;
    return 0;
}

/* ---- fetching ------------------------------------------------------------------ */

static uint8_t *fetch(const char *url, size_t *len)
{
    AVIOContext *io = NULL;
    AVDictionary *opts = NULL;
    uint8_t *buf = NULL;
    size_t got = 0, cap = 0;

    *len = 0;
    av_dict_set(&opts, "rw_timeout", "10000000", 0);
    av_dict_set(&opts, "user_agent", "Mozilla/5.0 (PlayStation 5) NuvioPS5", 0);
    if (avio_open2(&io, url, AVIO_FLAG_READ, NULL, &opts) < 0) {
        av_dict_free(&opts);
        return NULL;
    }
    av_dict_free(&opts);
    for (;;) {
        if (got + 65536 > cap) {
            size_t ncap = cap ? cap * 2 : 256 * 1024;
            if (ncap > MAX_FETCH) ncap = MAX_FETCH;
            if (ncap <= got) break;
            uint8_t *nb = (uint8_t *)realloc(buf, ncap);
            if (!nb) break;
            buf = nb;
            cap = ncap;
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

static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_lock);
        slot *next = NULL;
        while (!next) {
            for (int i = 0; i < SLOTS; i++)
                if (s_slots[i].state == PENDING && (!next || s_slots[i].seq < next->seq))
                    next = &s_slots[i];
            if (!next)
                pthread_cond_wait(&s_cond, &s_lock);
        }
        char url[1024];
        memcpy(url, next->url, sizeof url);
        const int mw = next->max_w, mh = next->max_h, blur = next->blur;
        next->state = LOADING;
        next->discard = 0;
        pthread_mutex_unlock(&s_lock);

        size_t len = 0;
        ui_image img;
        memset(&img, 0, sizeof img);
        uint8_t *data = fetch(url, &len);
        int ok = data && ui_image_decode(data, len, mw, mh, &img) == 0;
        free(data);
        if (ok && blur > 0)
            ui_image_blur(&img, blur);
        evo_bt("image: %s %dx%d %.100s", ok ? "ok" : "FAILED", img.w, img.h, url);

        pthread_mutex_lock(&s_lock);
        if (next->discard || next->state != LOADING) {
            ui_image_free(&img);
            if (next->discard) {
                next->state = EMPTY;
                next->discard = 0;
            }
        } else {
            next->img = img;
            next->state = ok ? READY : FAILED;
        }
        s_generation++;
        pthread_mutex_unlock(&s_lock);
    }
    return NULL;
}

int ui_image_request(const char *url, int max_w, int max_h, int blur)
{
    if (!url || !*url || strlen(url) >= sizeof s_slots[0].url)
        return -1;
    pthread_mutex_lock(&s_lock);
    if (!s_worker) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) == 0) {
            pthread_detach(t);
            s_worker = 1;
        }
    }
    int found = -1, free_i = -1, oldest = -1;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &s_slots[i];
        if (s->state != EMPTY && !s->discard && s->max_w == max_w && s->max_h == max_h &&
            s->blur == blur && !strcmp(s->url, url)) {
            found = i;
            break;
        }
        if (s->state == EMPTY && free_i < 0)
            free_i = i;
        if ((s->state == READY || s->state == FAILED) &&
            (oldest < 0 || s->seq < s_slots[oldest].seq))
            oldest = i;
    }
    if (found < 0) {
        found = free_i >= 0 ? free_i : oldest;
        if (found >= 0) {
            slot *s = &s_slots[found];
            ui_image_free(&s->img);
            snprintf(s->url, sizeof s->url, "%s", url);
            s->max_w = max_w;
            s->max_h = max_h;
            s->blur = blur;
            s->state = PENDING;
            s->discard = 0;
            s->seq = ++s_seq;
            pthread_cond_signal(&s_cond);
        }
    }
    pthread_mutex_unlock(&s_lock);
    return found;
}

const ui_image *ui_image_get(int handle, int *failed)
{
    const ui_image *out = NULL;
    if (failed)
        *failed = handle < 0;
    if (handle < 0 || handle >= SLOTS)
        return NULL;
    pthread_mutex_lock(&s_lock);
    if (s_slots[handle].state == READY)
        out = &s_slots[handle].img;
    else if (failed && s_slots[handle].state == FAILED)
        *failed = 1;
    pthread_mutex_unlock(&s_lock);
    return out;
}

unsigned ui_image_generation(void)
{
    return s_generation;
}

void ui_image_clear(void)
{
    pthread_mutex_lock(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &s_slots[i];
        if (s->state == LOADING) {
            s->discard = 1;
        } else {
            ui_image_free(&s->img);
            s->state = EMPTY;
        }
        s->url[0] = 0;
    }
    pthread_mutex_unlock(&s_lock);
}
