/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * Text for the Nuvio Player's overlay: Inter (Nuvio's interface font) shaped
 * with HarfBuzz, laid out right-to-left where the text is (fribidi), drawn
 * through FreeType. Characters Inter lacks fall back to Noto Naskh Arabic and
 * then to the console's own fonts (/preinst/common/font: Japanese, Korean,
 * Chinese, Thai) when the app can read them.
 *
 * Sizes are canvas pixels; y is the baseline.
 */
#include "ui_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_weight {
    UI_REGULAR = 0,   /* 400 */
    UI_MEDIUM,        /* 500 */
    UI_SEMIBOLD,      /* 600 */
    UI_BOLD,          /* 700 */
} ui_weight;

/* Loads the embedded fonts (once). 0 on success. */
int ui_text_init(void);

/* Ascent (above the baseline) and descent (below, positive) for a size. */
void ui_text_metrics(ui_weight w, float size, float *ascent, float *descent);

/* Width of one line of text. */
float ui_text_width(ui_weight w, float size, const char *utf8);

/* Draws one line at (x, baseline). max_w > 0 shortens it with an ellipsis
 * to fit. Returns the width drawn. */
float ui_text_draw(ui_canvas *c, ui_weight w, float size, float x, float baseline, ui_color color,
                   const char *utf8, float max_w);

/* Same, aligned: align 0 = left edge at x, 1 = centred on x, 2 = right edge at x. */
float ui_text_draw_aligned(ui_canvas *c, ui_weight w, float size, float x, float baseline,
                           int align, ui_color color, const char *utf8, float max_w);

/* Word-wrapped text from (x, first baseline), at most max_lines lines (the
 * last one ellipsised if text remains), line_h apart. With c == NULL it only
 * measures. Returns the number of lines. */
int ui_text_draw_wrapped(ui_canvas *c, ui_weight w, float size, float x, float baseline,
                         float max_w, float line_h, int max_lines, ui_color color,
                         const char *utf8);

#ifdef __cplusplus
}
#endif
