/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * Remote artwork for the overlay - title logos, backdrops, episode stills -
 * fetched and decoded (PNG, JPEG, WebP, SVG) on a worker thread, premultiplied,
 * scaled to fit the size it will be drawn at and optionally blurred, so the
 * render loop never waits on the network.
 */
#include "ui_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts loading url scaled to fit max_w x max_h (blur > 0: blurred by that
 * radius after scaling). Asking again for the same url and size returns the
 * same handle. Returns a handle >= 0, or -1 for an empty url. */
int ui_image_request(const char *url, int max_w, int max_h, int blur);

/* The image once it has arrived, else NULL. *failed (optional) is set when it
 * never will. */
const ui_image *ui_image_get(int handle, int *failed);

/* Bumped whenever an image finishes, so the overlay knows to redraw. */
unsigned ui_image_generation(void);

/* Decodes an in-memory PNG/JPEG/WebP/SVG into a premultiplied image. */
int ui_image_decode(const uint8_t *data, size_t size, int max_w, int max_h, ui_image *out);

/* Drops every image (between playbacks). */
void ui_image_clear(void);

#ifdef __cplusplus
}
#endif
