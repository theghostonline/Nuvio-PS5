/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * The software canvas the Nuvio Player's overlay is drawn into: premultiplied
 * RGBA8 (memory R,G,B,A - the 0xAABBGGRR words evo_agc_composite_overlay
 * samples), with clipping and anti-aliased primitives. Coordinates are float
 * canvas pixels; the overlay is laid out in Nuvio's 1920x1080 CSS pixels and
 * the canvas is that size, so the two are the same.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Colours are 0xAARRGGBB with straight (CSS-style) alpha. */
typedef uint32_t ui_color;

#define UI_RGBA(r, g, b, a) \
    ((ui_color)(((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b)))
#define UI_WHITE 0xffffffffu
#define UI_BLACK 0xff000000u

/* Multiplies a colour's alpha by a (0..1). */
ui_color ui_color_alpha(ui_color c, float a);
/* Linear mix of two colours, t in 0..1. */
ui_color ui_color_mix(ui_color a, ui_color b, float t);

typedef struct ui_canvas {
    uint32_t *px;
    int w, h;
    int cx0, cy0, cx1, cy1;      /* clip rectangle, [x0, x1) x [y0, y1) */
    int clip_depth;
    int clip_stack[8][4];
} ui_canvas;

/* A premultiplied RGBA8 image (same pixel layout as the canvas). */
typedef struct ui_image {
    uint32_t *px;
    int w, h;
} ui_image;

/* An 8-bit coverage mask (glyphs, icons). */
typedef struct ui_mask {
    uint8_t *a;
    int w, h;
    int pitch;
} ui_mask;

int  ui_canvas_init(ui_canvas *c, int w, int h);
void ui_canvas_free(ui_canvas *c);
/* Clears to transparent (the clip rectangle only). */
void ui_canvas_clear(ui_canvas *c);

void ui_push_clip(ui_canvas *c, float x, float y, float w, float h);
void ui_pop_clip(ui_canvas *c);

void ui_fill_rect(ui_canvas *c, float x, float y, float w, float h, ui_color color);
void ui_fill_rrect(ui_canvas *c, float x, float y, float w, float h, float r, ui_color color);
/* A rounded-rectangle outline of the given stroke width, inside the box. */
void ui_stroke_rrect(ui_canvas *c, float x, float y, float w, float h, float r, float stroke,
                     ui_color color);
void ui_fill_circle(ui_canvas *c, float cx, float cy, float r, ui_color color);
/* An arc of a ring (for spinners): angles in radians, clockwise from 12 o'clock. */
void ui_stroke_arc(ui_canvas *c, float cx, float cy, float r, float stroke, float a0, float a1,
                   ui_color color);

/* Vertical gradient through n stops (positions 0..1, ascending). */
void ui_fill_vgradient(ui_canvas *c, float x, float y, float w, float h, const float *pos,
                       const ui_color *colors, int n);
/* Horizontal gradient through n stops. */
void ui_fill_hgradient(ui_canvas *c, float x, float y, float w, float h, const float *pos,
                       const ui_color *colors, int n);

/* A coverage mask at integer canvas position, tinted with color. */
void ui_draw_mask(ui_canvas *c, const ui_mask *m, int x, int y, ui_color color);

/* An image scaled into the rectangle (bilinear), at opacity. */
void ui_draw_image(ui_canvas *c, const ui_image *img, float x, float y, float w, float h,
                   float opacity);
/* An image cropped to fill the rectangle (CSS object-fit: cover). */
void ui_draw_image_cover(ui_canvas *c, const ui_image *img, float x, float y, float w, float h,
                         float opacity);
/* Largest size that fits img inside max_w x max_h keeping its aspect. */
void ui_image_fit(const ui_image *img, float max_w, float max_h, float *out_w, float *out_h);

/* Images. ui_image_alloc zero-fills. */
int  ui_image_alloc(ui_image *img, int w, int h);
void ui_image_free(ui_image *img);
/* A resized copy (box-filtered when shrinking, bilinear when growing). */
int  ui_image_resize(const ui_image *src, int w, int h, ui_image *out);
/* Blurs in place: three box passes approximating a Gaussian of the radius. */
void ui_image_blur(ui_image *img, int radius);

#ifdef __cplusplus
}
#endif
