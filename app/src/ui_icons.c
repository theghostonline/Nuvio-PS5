#include "ui_icons.h"

#include "ui_assets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvg.h"
#include "nanosvg/nanosvgrast.h"

static ui_asset icon_asset(ui_icon icon)
{
    switch (icon) {
    case UI_ICON_PLAY:          return ui_asset_svg_play();
    case UI_ICON_PAUSE:         return ui_asset_svg_pause();
    case UI_ICON_NEXT:          return ui_asset_svg_skip_next();
    case UI_ICON_SUBTITLES:     return ui_asset_svg_subtitles();
    case UI_ICON_AUDIO:         return ui_asset_svg_audio_outline();
    case UI_ICON_AUDIO_FILLED:  return ui_asset_svg_audio_filled();
    case UI_ICON_SOURCE:        return ui_asset_svg_source();
    case UI_ICON_EPISODES:      return ui_asset_svg_episodes();
    case UI_ICON_ASPECT:        return ui_asset_svg_aspect_ratio();
    case UI_ICON_CHECK:         return ui_asset_svg_check();
    case UI_ICON_CHEVRON_LEFT:  return ui_asset_svg_chevron_left();
    case UI_ICON_CHEVRON_RIGHT: return ui_asset_svg_chevron_right();
    case UI_ICON_FAST_FORWARD:  return ui_asset_svg_fast_forward();
    case UI_ICON_FAST_REWIND:   return ui_asset_svg_fast_rewind();
    case UI_ICON_WARNING:       return ui_asset_svg_warning();
    default: break;
    }
    ui_asset none = {NULL, 0};
    return none;
}

typedef struct cached {
    int size;
    ui_mask mask;
} cached;

#define SIZES_PER_ICON 6
static NSVGimage *s_svg[UI_ICON_COUNT];
static int s_parsed[UI_ICON_COUNT];
static cached s_cache[UI_ICON_COUNT][SIZES_PER_ICON];
static NSVGrasterizer *s_rast;

const ui_mask *ui_icon_mask(ui_icon icon, int size)
{
    if ((int)icon < 0 || icon >= UI_ICON_COUNT || size <= 0 || size > 512)
        return NULL;
    for (int i = 0; i < SIZES_PER_ICON; i++)
        if (s_cache[icon][i].size == size)
            return &s_cache[icon][i].mask;

    if (!s_parsed[icon]) {
        s_parsed[icon] = 1;
        ui_asset a = icon_asset(icon);
        if (a.data) {
            char *text = (char *)malloc(a.size + 1);   /* nsvgParse edits its input */
            if (text) {
                memcpy(text, a.data, a.size);
                text[a.size] = 0;
                s_svg[icon] = nsvgParse(text, "px", 96.0f);
                free(text);
            }
        }
    }
    if (!s_svg[icon] || s_svg[icon]->width <= 0.0f)
        return NULL;
    if (!s_rast && !(s_rast = nsvgCreateRasterizer()))
        return NULL;

    NSVGimage *img = s_svg[icon];
    unsigned char *rgba = (unsigned char *)calloc((size_t)size * size, 4);
    uint8_t *alpha = (uint8_t *)malloc((size_t)size * size);
    if (!rgba || !alpha) {
        free(rgba);
        free(alpha);
        return NULL;
    }
    const float scale = (float)size / (img->width > img->height ? img->width : img->height);
    const float tx = ((float)size - img->width * scale) * 0.5f;
    const float ty = ((float)size - img->height * scale) * 0.5f;
    nsvgRasterize(s_rast, img, tx, ty, scale, rgba, size, size, size * 4);
    for (int i = 0; i < size * size; i++)
        alpha[i] = rgba[i * 4 + 3];
    free(rgba);

    /* Replace the least recent size slot (round robin). */
    static int s_next[UI_ICON_COUNT];
    cached *slot = &s_cache[icon][s_next[icon]];
    s_next[icon] = (s_next[icon] + 1) % SIZES_PER_ICON;
    free(slot->mask.a);
    slot->size = size;
    slot->mask.a = alpha;
    slot->mask.w = slot->mask.h = slot->mask.pitch = size;
    return &slot->mask;
}

void ui_draw_icon(ui_canvas *c, ui_icon icon, float cx, float cy, int size, ui_color color)
{
    const ui_mask *m = ui_icon_mask(icon, size);
    if (m)
        ui_draw_mask(c, m, (int)lroundf(cx - size * 0.5f), (int)lroundf(cy - size * 0.5f), color);
}
