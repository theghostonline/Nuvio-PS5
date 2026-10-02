#pragma once
/*
 * The overlay's icons: Nuvio's own player SVGs (assets/icons/ic_player_*)
 * plus a few Material icons, rasterised with NanoSVG at the exact size they
 * are drawn at and cached as coverage masks.
 */
#include "ui_canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ui_icon {
    UI_ICON_PLAY = 0,
    UI_ICON_PAUSE,
    UI_ICON_NEXT,
    UI_ICON_SUBTITLES,
    UI_ICON_AUDIO,
    UI_ICON_AUDIO_FILLED,
    UI_ICON_SOURCE,
    UI_ICON_EPISODES,
    UI_ICON_ASPECT,
    UI_ICON_CHECK,
    UI_ICON_CHEVRON_LEFT,
    UI_ICON_CHEVRON_RIGHT,
    UI_ICON_FAST_FORWARD,
    UI_ICON_FAST_REWIND,
    UI_ICON_WARNING,
    UI_ICON_COUNT
} ui_icon;

/* The icon as a size x size coverage mask (cached), or NULL. */
const ui_mask *ui_icon_mask(ui_icon icon, int size);

/* Draws the icon centred on (cx, cy). */
void ui_draw_icon(ui_canvas *c, ui_icon icon, float cx, float cy, int size, ui_color color);

#ifdef __cplusplus
}
#endif
