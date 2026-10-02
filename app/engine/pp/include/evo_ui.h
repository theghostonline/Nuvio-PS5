/*
 * evo_ui — themed drawing primitives.
 *
 * Everything here reads from evo_theme, so screens describe *what* they are
 * drawing (a card, a rail, a scrim) rather than picking colours themselves.
 *
 * All shapes are rasterised from signed-distance fields, so edges carry true
 * analytic coverage and corners are smooth at any radius. This is what the
 * old hand-rolled rectangles could not do - see the note in evo_ui.c.
 */
/* Guard is EVO_UI_H_INCLUDED, not EVO_UI_H: the latter is the framebuffer
 * height below, and defining it twice made every translation unit that
 * included this header emit a -Wmacro-redefined warning. */
#ifndef EVO_UI_H_INCLUDED
#define EVO_UI_H_INCLUDED

#include <stdint.h>
#include "evo_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Framebuffer geometry. The UI is authored at 1080p. */
#define EVO_UI_W 1920
#define EVO_UI_H 1080

/* Source-over blend, 0xAABBGGRR. */
uint32_t evo_ui_blend(uint32_t dst, uint32_t src, int alpha);

/* Full-screen vertical gradient using the theme's background. */
void evo_ui_background(uint32_t *fb);

/*
 * Vertical gradient rectangle, hard edges.
 *
 * REPLACES what is underneath - it does not blend. That is what the page
 * background wants, and it is why the colours passed here should be opaque.
 * For a scrim, use evo_ui_vgrad_over(): passing a transparent colour to this
 * function writes the transparency into the framebuffer, and since scanout
 * ignores alpha the result is a solid black slab rather than a fade.
 */
void evo_ui_vgrad(uint32_t *fb, int x, int y, int w, int h,
                  uint32_t top, uint32_t bottom);

/*
 * Vertical gradient composited over what is already there, honouring the
 * alpha of both endpoints. This is the one to use for scrims: a gradient from
 * alpha 0 to alpha 200 fades in over whatever it covers.
 */
void evo_ui_vgrad_over(uint32_t *fb, int x, int y, int w, int h,
                       uint32_t top, uint32_t bottom);

/*
 * A card.
 *
 *   - drop shadow beneath, so it sits above the page instead of being a
 *     coloured box painted onto it
 *   - vertical gradient fill
 *   - hairline border
 *   - optional accent rail down the left edge, which is how the selected row
 *     is identified without shouting
 *
 * `selected` picks the theme's selected surface/border and draws the rail.
 */
void evo_ui_card(uint32_t *fb, int x, int y, int w, int h, int selected);

/* Rounded rectangle with explicit colours - for anything that is not a card. */
void evo_ui_round_rect(uint32_t *fb, int x, int y, int w, int h, int radius,
                       uint32_t fill_top, uint32_t fill_bottom,
                       uint32_t border, int border_px,
                       uint32_t shadow, int shadow_px);

/* Filled circle, antialiased. */
void evo_ui_circle(uint32_t *fb, int cx, int cy, int r, uint32_t colour);

/* 1px-accurate horizontal / vertical hairline. */
void evo_ui_hline(uint32_t *fb, int x, int y, int w, uint32_t colour);
void evo_ui_vline(uint32_t *fb, int x, int y, int h, uint32_t colour);

#ifdef __cplusplus
}
#endif

#endif /* EVO_UI_H_INCLUDED */
