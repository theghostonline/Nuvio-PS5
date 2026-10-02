/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui_canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- pixels ------------------------------------------------------------- */

static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* ui_color at coverage cov (0..255) -> premultiplied packed pixel. */
static inline uint32_t premul(ui_color c, uint32_t cov)
{
    uint32_t a = ((c >> 24) * cov + 127) / 255;
    uint32_t r = (((c >> 16) & 0xff) * a + 127) / 255;
    uint32_t g = (((c >> 8) & 0xff) * a + 127) / 255;
    uint32_t b = ((c & 0xff) * a + 127) / 255;
    return pack(r, g, b, a);
}

/* Premultiplied source over destination. */
static inline uint32_t over(uint32_t d, uint32_t s)
{
    const uint32_t sa = s >> 24;
    if (sa == 255)
        return s;
    if (s == 0)
        return d;
    const uint32_t ia = 255 - sa;
    uint32_t rb = (d & 0x00ff00ffu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ga = ((d >> 8) & 0x00ff00ffu) * ia + 0x00800080u;
    ga = ((ga + ((ga >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    return s + (rb | (ga << 8));
}

/* A premultiplied pixel scaled by k (0..256). */
static inline uint32_t scale_px(uint32_t p, uint32_t k)
{
    uint32_t rb = ((p & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu;
    uint32_t ga = (((p >> 8) & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu;
    return rb | (ga << 8);
}

ui_color ui_color_alpha(ui_color c, float a)
{
    if (a >= 1.0f)
        return c;
    if (a <= 0.0f)
        return c & 0x00ffffffu;
    uint32_t na = (uint32_t)((float)(c >> 24) * a + 0.5f);
    return (c & 0x00ffffffu) | (na << 24);
}

ui_color ui_color_mix(ui_color a, ui_color b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        float va = (float)((a >> sh) & 0xff), vb = (float)((b >> sh) & 0xff);
        out |= ((uint32_t)(va + (vb - va) * t + 0.5f) & 0xff) << sh;
    }
    return out;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- canvas -------------------------------------------------------------- */

int ui_canvas_init(ui_canvas *c, int w, int h)
{
    memset(c, 0, sizeof *c);
    c->px = (uint32_t *)calloc((size_t)w * (size_t)h, 4);
    if (!c->px)
        return -1;
    c->w = w;
    c->h = h;
    c->cx1 = w;
    c->cy1 = h;
    return 0;
}

void ui_canvas_free(ui_canvas *c)
{
    free(c->px);
    memset(c, 0, sizeof *c);
}

void ui_canvas_clear(ui_canvas *c)
{
    if (c->cx0 == 0 && c->cx1 == c->w) {
        memset(c->px + (size_t)c->cy0 * c->w, 0, (size_t)(c->cy1 - c->cy0) * c->w * 4);
        return;
    }
    for (int y = c->cy0; y < c->cy1; y++)
        memset(c->px + (size_t)y * c->w + c->cx0, 0, (size_t)(c->cx1 - c->cx0) * 4);
}

void ui_push_clip(ui_canvas *c, float x, float y, float w, float h)
{
    if (c->clip_depth < 8) {
        int *s = c->clip_stack[c->clip_depth];
        s[0] = c->cx0; s[1] = c->cy0; s[2] = c->cx1; s[3] = c->cy1;
    }
    c->clip_depth++;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 > c->cx0) c->cx0 = x0;
    if (y0 > c->cy0) c->cy0 = y0;
    if (x1 < c->cx1) c->cx1 = x1;
    if (y1 < c->cy1) c->cy1 = y1;
    if (c->cx1 < c->cx0) c->cx1 = c->cx0;
    if (c->cy1 < c->cy0) c->cy1 = c->cy0;
}

void ui_pop_clip(ui_canvas *c)
{
    if (c->clip_depth <= 0)
        return;
    c->clip_depth--;
    if (c->clip_depth < 8) {
        const int *s = c->clip_stack[c->clip_depth];
        c->cx0 = s[0]; c->cy0 = s[1]; c->cx1 = s[2]; c->cy1 = s[3];
    } else {
        c->cx0 = c->cy0 = 0;
        c->cx1 = c->w;
        c->cy1 = c->h;
    }
}

/* Pixel bounds of a float rectangle, clipped. 0 when empty. */
static int bounds(const ui_canvas *c, float x, float y, float w, float h,
                  int *x0, int *y0, int *x1, int *y1)
{
    *x0 = (int)floorf(x);
    *y0 = (int)floorf(y);
    *x1 = (int)ceilf(x + w);
    *y1 = (int)ceilf(y + h);
    if (*x0 < c->cx0) *x0 = c->cx0;
    if (*y0 < c->cy0) *y0 = c->cy0;
    if (*x1 > c->cx1) *x1 = c->cx1;
    if (*y1 > c->cy1) *y1 = c->cy1;
    return *x1 > *x0 && *y1 > *y0;
}

static void blend_span(uint32_t *row, int n, uint32_t s)
{
    if ((s >> 24) == 255) {
        for (int i = 0; i < n; i++)
            row[i] = s;
        return;
    }
    for (int i = 0; i < n; i++)
        row[i] = over(row[i], s);
}

void ui_fill_rect(ui_canvas *c, float x, float y, float w, float h, ui_color color)
{
    ui_fill_rrect(c, x, y, w, h, 0.0f, color);
}

/* Coverage of a rounded box at a pixel centre: straight edges by area,
 * corners by signed distance. */
static inline float rrect_cov(float fx, float fy, float x, float y, float w, float h, float r)
{
    if (r > 0.0f) {
        float cx = -1.0f, cy = -1.0f;
        if (fx < x + r) cx = x + r;
        else if (fx > x + w - r) cx = x + w - r;
        if (fy < y + r) cy = y + r;
        else if (fy > y + h - r) cy = y + h - r;
        if (cx >= 0.0f && cy >= 0.0f) {
            float dx = fx - cx, dy = fy - cy;
            float d = sqrtf(dx * dx + dy * dy) - r;
            return clampf(0.5f - d, 0.0f, 1.0f);
        }
    }
    float cxv = clampf(fx - x + 0.5f, 0.0f, 1.0f) * clampf(x + w - fx + 0.5f, 0.0f, 1.0f);
    float cyv = clampf(fy - y + 0.5f, 0.0f, 1.0f) * clampf(y + h - fy + 0.5f, 0.0f, 1.0f);
    return cxv * cyv;
}

void ui_fill_rrect(ui_canvas *c, float x, float y, float w, float h, float r, ui_color color)
{
    int x0, y0, x1, y1;
    if (w <= 0.0f || h <= 0.0f || (color >> 24) == 0 || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
        return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    const uint32_t solid = premul(color, 255);

    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w;
        const float fy = py + 0.5f;
        const int in_corner_rows = (fy < y + r) || (fy > y + h - r);
        const int full_row = fy - 0.5f >= y && fy + 0.5f <= y + h;
        if (!in_corner_rows && full_row) {
            /* Only the two edge pixels can be partial. */
            int sx0 = (int)ceilf(x), sx1 = (int)floorf(x + w);
            if (sx0 < x0) sx0 = x0;
            if (sx1 > x1) sx1 = x1;
            for (int px = x0; px < sx0 && px < x1; px++) {
                float cov = rrect_cov(px + 0.5f, fy, x, y, w, h, r);
                if (cov > 0.0f) row[px] = over(row[px], premul(color, (uint32_t)(cov * 255.0f + 0.5f)));
            }
            if (sx1 > sx0)
                blend_span(row + sx0, sx1 - sx0, solid);
            for (int px = sx1 > sx0 ? sx1 : sx0; px < x1; px++) {
                float cov = rrect_cov(px + 0.5f, fy, x, y, w, h, r);
                if (cov > 0.0f) row[px] = over(row[px], premul(color, (uint32_t)(cov * 255.0f + 0.5f)));
            }
            continue;
        }
        for (int px = x0; px < x1; px++) {
            float cov = rrect_cov(px + 0.5f, fy, x, y, w, h, r);
            if (cov >= 0.998f)
                row[px] = over(row[px], solid);
            else if (cov > 0.002f)
                row[px] = over(row[px], premul(color, (uint32_t)(cov * 255.0f + 0.5f)));
        }
    }
}

void ui_stroke_rrect(ui_canvas *c, float x, float y, float w, float h, float r, float stroke,
                     ui_color color)
{
    int x0, y0, x1, y1;
    if (w <= 0.0f || h <= 0.0f || stroke <= 0.0f || (color >> 24) == 0 ||
        !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
        return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    const float ix = x + stroke, iy = y + stroke, iw = w - 2 * stroke, ih = h - 2 * stroke;
    const float ir = r - stroke > 0.0f ? r - stroke : 0.0f;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w;
        const float fy = py + 0.5f;
        for (int px = x0; px < x1; px++) {
            const float fx = px + 0.5f;
            float cov = rrect_cov(fx, fy, x, y, w, h, r);
            if (cov <= 0.0f)
                continue;
            if (iw > 0.0f && ih > 0.0f)
                cov -= rrect_cov(fx, fy, ix, iy, iw, ih, ir);
            if (cov > 0.002f)
                row[px] = over(row[px], premul(color, (uint32_t)(clampf(cov, 0, 1) * 255.0f + 0.5f)));
        }
    }
}

void ui_fill_circle(ui_canvas *c, float cx, float cy, float r, ui_color color)
{
    ui_fill_rrect(c, cx - r, cy - r, 2 * r, 2 * r, r, color);
}

void ui_stroke_arc(ui_canvas *c, float cx, float cy, float r, float stroke, float a0, float a1,
                   ui_color color)
{
    int x0, y0, x1, y1;
    const float two_pi = 6.28318530718f;
    if (!bounds(c, cx - r - 1, cy - r - 1, 2 * r + 2, 2 * r + 2, &x0, &y0, &x1, &y1))
        return;
    float span = a1 - a0;
    if (span <= 0.0f)
        return;
    if (span > two_pi) span = two_pi;
    a0 = fmodf(a0, two_pi);
    if (a0 < 0.0f) a0 += two_pi;
    const float mid = r - stroke * 0.5f;
    const float cap = stroke * 0.5f;
    /* Round caps: the ends are circles of the stroke's radius. */
    const float e0x = cx + mid * sinf(a0), e0y = cy - mid * cosf(a0);
    const float e1x = cx + mid * sinf(a0 + span), e1y = cy - mid * cosf(a0 + span);

    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w;
        const float fy = py + 0.5f;
        for (int px = x0; px < x1; px++) {
            const float fx = px + 0.5f;
            const float dx = fx - cx, dy = fy - cy;
            const float d = sqrtf(dx * dx + dy * dy);
            float cov = clampf(0.5f - (fabsf(d - mid) - cap), 0.0f, 1.0f);
            if (cov <= 0.0f)
                continue;
            float ang = atan2f(dx, -dy);
            if (ang < 0.0f) ang += two_pi;
            float rel = ang - a0;
            if (rel < 0.0f) rel += two_pi;
            if (rel > span) {
                /* Outside the sweep: only the caps reach here. */
                float d0 = sqrtf((fx - e0x) * (fx - e0x) + (fy - e0y) * (fy - e0y)) - cap;
                float d1 = sqrtf((fx - e1x) * (fx - e1x) + (fy - e1y) * (fy - e1y)) - cap;
                float dm = d0 < d1 ? d0 : d1;
                cov = clampf(0.5f - dm, 0.0f, 1.0f);
                if (cov <= 0.0f)
                    continue;
            }
            row[px] = over(row[px], premul(color, (uint32_t)(cov * 255.0f + 0.5f)));
        }
    }
}

/* Colour at t along n gradient stops. */
static ui_color stop_color(const float *pos, const ui_color *colors, int n, float t)
{
    if (n <= 0)
        return 0;
    if (t <= pos[0])
        return colors[0];
    for (int i = 1; i < n; i++) {
        if (t <= pos[i]) {
            float span = pos[i] - pos[i - 1];
            return ui_color_mix(colors[i - 1], colors[i], span > 0.0f ? (t - pos[i - 1]) / span : 1.0f);
        }
    }
    return colors[n - 1];
}

void ui_fill_vgradient(ui_canvas *c, float x, float y, float w, float h, const float *pos,
                       const ui_color *colors, int n)
{
    int x0, y0, x1, y1;
    if (h <= 0.0f || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
        return;
    for (int py = y0; py < y1; py++) {
        const float t = (py + 0.5f - y) / h;
        const uint32_t s = premul(stop_color(pos, colors, n, t), 255);
        blend_span(c->px + (size_t)py * c->w + x0, x1 - x0, s);
    }
}

void ui_fill_hgradient(ui_canvas *c, float x, float y, float w, float h, const float *pos,
                       const ui_color *colors, int n)
{
    int x0, y0, x1, y1;
    if (w <= 0.0f || !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
        return;
    uint32_t *cols = (uint32_t *)malloc((size_t)(x1 - x0) * 4);
    if (!cols)
        return;
    for (int px = x0; px < x1; px++)
        cols[px - x0] = premul(stop_color(pos, colors, n, (px + 0.5f - x) / w), 255);
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w + x0;
        for (int i = 0; i < x1 - x0; i++)
            row[i] = over(row[i], cols[i]);
    }
    free(cols);
}

void ui_draw_mask(ui_canvas *c, const ui_mask *m, int x, int y, ui_color color)
{
    if (!m || !m->a || (color >> 24) == 0)
        return;
    int x0 = x < c->cx0 ? c->cx0 : x;
    int y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + m->w > c->cx1 ? c->cx1 : x + m->w;
    int y1 = y + m->h > c->cy1 ? c->cy1 : y + m->h;
    if (x1 <= x0 || y1 <= y0)
        return;
    const uint32_t full = premul(color, 255);
    for (int py = y0; py < y1; py++) {
        const uint8_t *src = m->a + (size_t)(py - y) * m->pitch + (x0 - x);
        uint32_t *row = c->px + (size_t)py * c->w;
        for (int px = x0; px < x1; px++, src++) {
            const uint32_t a = *src;
            if (!a)
                continue;
            row[px] = over(row[px], a == 255 ? full : scale_px(full, a + (a >> 7)));
        }
    }
}

/* ---- images -------------------------------------------------------------- */

int ui_image_alloc(ui_image *img, int w, int h)
{
    img->px = (w > 0 && h > 0) ? (uint32_t *)calloc((size_t)w * (size_t)h, 4) : NULL;
    img->w = img->px ? w : 0;
    img->h = img->px ? h : 0;
    return img->px ? 0 : -1;
}

void ui_image_free(ui_image *img)
{
    free(img->px);
    img->px = NULL;
    img->w = img->h = 0;
}

static inline uint32_t sample_bilinear(const ui_image *img, float sx, float sy)
{
    if (sx < 0.0f) sx = 0.0f;
    if (sy < 0.0f) sy = 0.0f;
    int ix = (int)sx, iy = (int)sy;
    if (ix >= img->w - 1) { ix = img->w - 1; sx = (float)ix; }
    if (iy >= img->h - 1) { iy = img->h - 1; sy = (float)iy; }
    const uint32_t fx = (uint32_t)((sx - ix) * 256.0f);
    const uint32_t fy = (uint32_t)((sy - iy) * 256.0f);
    const int ix1 = ix + 1 < img->w ? ix + 1 : ix;
    const int iy1 = iy + 1 < img->h ? iy + 1 : iy;
    const uint32_t *r0 = img->px + (size_t)iy * img->w;
    const uint32_t *r1 = img->px + (size_t)iy1 * img->w;
    const uint32_t p00 = r0[ix], p10 = r0[ix1], p01 = r1[ix], p11 = r1[ix1];
    uint32_t out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const uint32_t a = (p00 >> sh) & 0xff, b = (p10 >> sh) & 0xff;
        const uint32_t cc = (p01 >> sh) & 0xff, d = (p11 >> sh) & 0xff;
        const uint32_t top = a * (256 - fx) + b * fx;
        const uint32_t bot = cc * (256 - fx) + d * fx;
        out |= (((top * (256 - fy) + bot * fy) >> 16) & 0xff) << sh;
    }
    return out;
}

static void draw_image_src(ui_canvas *c, const ui_image *img, float x, float y, float w, float h,
                           float sx0, float sy0, float sw, float sh, float opacity)
{
    int x0, y0, x1, y1;
    if (!img || !img->px || w <= 0.0f || h <= 0.0f || opacity <= 0.0f ||
        !bounds(c, x, y, w, h, &x0, &y0, &x1, &y1))
        return;
    const uint32_t k = (uint32_t)(clampf(opacity, 0.0f, 1.0f) * 256.0f);
    const float kx = sw / w, ky = sh / h;
    /* 1:1 at whole pixels (a backdrop already scaled to the canvas): no
     * filtering, just a blend per pixel. */
    if (kx == 1.0f && ky == 1.0f && x == floorf(x) && y == floorf(y) && sx0 == floorf(sx0) &&
        sy0 == floorf(sy0)) {
        for (int py = y0; py < y1; py++) {
            const uint32_t *src = img->px + (size_t)(py - (int)y + (int)sy0) * img->w + (x0 - (int)x + (int)sx0);
            uint32_t *row = c->px + (size_t)py * c->w;
            if (k >= 256) {
                for (int px = x0; px < x1; px++)
                    row[px] = over(row[px], *src++);
            } else {
                for (int px = x0; px < x1; px++)
                    row[px] = over(row[px], scale_px(*src++, k));
            }
        }
        return;
    }
    for (int py = y0; py < y1; py++) {
        uint32_t *row = c->px + (size_t)py * c->w;
        const float sy = sy0 + (py + 0.5f - y) * ky - 0.5f;
        /* Partial coverage on the rectangle's own fractional edges. */
        float cov_y = clampf(py + 1.0f - y, 0.0f, 1.0f) * clampf(y + h - py, 0.0f, 1.0f);
        for (int px = x0; px < x1; px++) {
            const float sx = sx0 + (px + 0.5f - x) * kx - 0.5f;
            uint32_t p = sample_bilinear(img, sx, sy);
            float cov = cov_y * clampf(px + 1.0f - x, 0.0f, 1.0f) * clampf(x + w - px, 0.0f, 1.0f);
            uint32_t kk = (uint32_t)(k * cov);
            if (kk < 256)
                p = scale_px(p, kk);
            row[px] = over(row[px], p);
        }
    }
}

void ui_draw_image(ui_canvas *c, const ui_image *img, float x, float y, float w, float h,
                   float opacity)
{
    if (img && img->px)
        draw_image_src(c, img, x, y, w, h, 0.0f, 0.0f, (float)img->w, (float)img->h, opacity);
}

void ui_draw_image_cover(ui_canvas *c, const ui_image *img, float x, float y, float w, float h,
                         float opacity)
{
    if (!img || !img->px || w <= 0.0f || h <= 0.0f)
        return;
    const float ia = (float)img->w / (float)img->h, ra = w / h;
    float sw = (float)img->w, sh = (float)img->h, sx = 0.0f, sy = 0.0f;
    if (ia > ra) {
        sw = sh * ra;
        sx = ((float)img->w - sw) * 0.5f;
    } else {
        sh = sw / ra;
        sy = ((float)img->h - sh) * 0.5f;
    }
    draw_image_src(c, img, x, y, w, h, sx, sy, sw, sh, opacity);
}

void ui_image_fit(const ui_image *img, float max_w, float max_h, float *out_w, float *out_h)
{
    if (!img || img->w <= 0 || img->h <= 0) {
        *out_w = *out_h = 0.0f;
        return;
    }
    float s = max_w / (float)img->w;
    if ((float)img->h * s > max_h)
        s = max_h / (float)img->h;
    *out_w = (float)img->w * s;
    *out_h = (float)img->h * s;
}

int ui_image_resize(const ui_image *src, int w, int h, ui_image *out)
{
    if (!src || !src->px || w <= 0 || h <= 0 || ui_image_alloc(out, w, h) != 0)
        return -1;
    if (w >= src->w && h >= src->h) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                out->px[(size_t)y * w + x] = sample_bilinear(
                    src, (x + 0.5f) * src->w / (float)w - 0.5f, (y + 0.5f) * src->h / (float)h - 0.5f);
        return 0;
    }
    /* Box filter: each output pixel averages the source pixels it covers. */
    for (int y = 0; y < h; y++) {
        const int sy0 = (int)((int64_t)y * src->h / h);
        int sy1 = (int)((int64_t)(y + 1) * src->h / h);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int x = 0; x < w; x++) {
            const int sx0 = (int)((int64_t)x * src->w / w);
            int sx1 = (int)((int64_t)(x + 1) * src->w / w);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
            for (int yy = sy0; yy < sy1 && yy < src->h; yy++) {
                const uint32_t *r = src->px + (size_t)yy * src->w;
                for (int xx = sx0; xx < sx1 && xx < src->w; xx++, n++) {
                    const uint32_t p = r[xx];
                    acc[0] += p & 0xff;
                    acc[1] += (p >> 8) & 0xff;
                    acc[2] += (p >> 16) & 0xff;
                    acc[3] += p >> 24;
                }
            }
            if (!n) n = 1;
            out->px[(size_t)y * w + x] = pack(acc[0] / n, acc[1] / n, acc[2] / n, acc[3] / n);
        }
    }
    return 0;
}

/* One box-blur pass along rows (or columns, via stride/step). */
static void box_pass(uint32_t *data, int count, int len, size_t line_stride, size_t step, int r,
                     uint32_t *tmp)
{
    const int win = 2 * r + 1;
    for (int line = 0; line < count; line++) {
        uint32_t *p = data + (size_t)line * line_stride;
        uint32_t s[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; i++) {
            const int k = i < 0 ? 0 : (i >= len ? len - 1 : i);
            const uint32_t v = p[(size_t)k * step];
            s[0] += v & 0xff; s[1] += (v >> 8) & 0xff; s[2] += (v >> 16) & 0xff; s[3] += v >> 24;
        }
        for (int i = 0; i < len; i++) {
            tmp[i] = pack(s[0] / win, s[1] / win, s[2] / win, s[3] / win);
            const int out_i = i - r < 0 ? 0 : i - r;
            const int in_i = i + r + 1 >= len ? len - 1 : i + r + 1;
            const uint32_t vo = p[(size_t)out_i * step], vi = p[(size_t)in_i * step];
            s[0] += (vi & 0xff) - (vo & 0xff);
            s[1] += ((vi >> 8) & 0xff) - ((vo >> 8) & 0xff);
            s[2] += ((vi >> 16) & 0xff) - ((vo >> 16) & 0xff);
            s[3] += (vi >> 24) - (vo >> 24);
        }
        for (int i = 0; i < len; i++)
            p[(size_t)i * step] = tmp[i];
    }
}

void ui_image_blur(ui_image *img, int radius)
{
    if (!img || !img->px || radius <= 0)
        return;
    const int len = img->w > img->h ? img->w : img->h;
    uint32_t *tmp = (uint32_t *)malloc((size_t)len * 4);
    if (!tmp)
        return;
    const int r = radius / 2 > 0 ? radius / 2 : 1;   /* three passes ~ one Gaussian */
    for (int pass = 0; pass < 3; pass++) {
        box_pass(img->px, img->h, img->w, (size_t)img->w, 1, r, tmp);
        box_pass(img->px, img->w, img->h, 1, (size_t)img->w, r, tmp);
    }
    free(tmp);
}
