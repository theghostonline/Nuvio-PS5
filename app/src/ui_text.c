#include "ui_text.h"

#include "ui_assets.h"

#include "evo_boot_trace.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fribidi.h>
#include <hb-ot.h>
#include <hb.h>

#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- fonts ---------------------------------------------------------------- */

enum {
    F_INTER_R, F_INTER_M, F_INTER_SB, F_INTER_B,
    F_NASKH_R, F_NASKH_B,
    F_EMOJI,                  /* Noto Emoji (monochrome): addon descriptions use emoji */
    /* The console's fonts, loaded the first time a character needs them. */
    F_SYS_JP_R, F_SYS_JP_B, F_SYS_KR_R, F_SYS_KR_B, F_SYS_CN, F_SYS_TH_R, F_SYS_TH_B,
    F_COUNT
};

static const char *const k_sys_paths[F_COUNT] = {
    [F_SYS_JP_R] = "/preinst/common/font/SSTJpPro-Regular.otf",
    [F_SYS_JP_B] = "/preinst/common/font/SSTJpPro-Bold.otf",
    [F_SYS_KR_R] = "/preinst/common/font/YoonGothicProSIE720.otf",
    [F_SYS_KR_B] = "/preinst/common/font/YoonGothicProSIE780.otf",
    [F_SYS_CN]   = "/preinst/common/font/DFHEI5-SONY.ttf",
    [F_SYS_TH_R] = "/preinst/common/font/SSTThai-Roman.otf",
    [F_SYS_TH_B] = "/preinst/common/font/SSTThai-Bold.otf",
};

typedef struct font {
    FT_Face ft;
    hb_font_t *hb;
    int tried;          /* system fonts: load attempted */
    int size64;         /* FT char size currently set, 26.6 */
    uint8_t *owned;     /* file data read from disk */
} font;

static FT_Library s_ft;
static font s_fonts[F_COUNT];
static int s_ready;

static int load_font(int idx, const uint8_t *data, size_t size)
{
    font *f = &s_fonts[idx];
    if (FT_New_Memory_Face(s_ft, data, (FT_Long)size, 0, &f->ft) != 0) {
        f->ft = NULL;
        return -1;
    }
    hb_blob_t *blob = hb_blob_create((const char *)data, (unsigned)size, HB_MEMORY_MODE_READONLY,
                                     NULL, NULL);
    hb_face_t *face = hb_face_create(blob, 0);
    f->hb = hb_font_create(face);
    hb_ot_font_set_funcs(f->hb);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    return 0;
}

static void load_system_font(int idx)
{
    font *f = &s_fonts[idx];
    struct stat st;
    int fd;
    if (f->tried)
        return;
    f->tried = 1;
    if (!k_sys_paths[idx] || stat(k_sys_paths[idx], &st) != 0 || st.st_size <= 0 ||
        (fd = open(k_sys_paths[idx], O_RDONLY)) < 0)
        return;
    uint8_t *buf = (uint8_t *)malloc((size_t)st.st_size);
    size_t got = 0;
    while (buf && got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (!buf || got != (size_t)st.st_size || load_font(idx, buf, got) != 0) {
        free(buf);
        evo_bt("text: system font %s unavailable", k_sys_paths[idx]);
        return;
    }
    f->owned = buf;
    evo_bt("text: loaded %s", k_sys_paths[idx]);
}

int ui_text_init(void)
{
    if (s_ready)
        return 0;
    if (FT_Init_FreeType(&s_ft) != 0)
        return -1;
    const ui_asset a[] = {
        ui_asset_font_inter_regular(), ui_asset_font_inter_medium(),
        ui_asset_font_inter_semibold(), ui_asset_font_inter_bold(),
        ui_asset_font_naskh_regular(), ui_asset_font_naskh_bold(), ui_asset_font_noto_emoji(),
    };
    for (int i = 0; i < 7; i++) {
        if (load_font(i, a[i].data, a[i].size) != 0) {
            evo_bt("text: embedded font %d failed", i);
            return -1;
        }
        s_fonts[i].tried = 1;
    }
    s_ready = 1;
    return 0;
}

/* Fallback chain for a weight. */
static int chain(ui_weight w, int *out)
{
    const int bold = w >= UI_SEMIBOLD;
    int n = 0;
    out[n++] = F_INTER_R + (int)w;
    out[n++] = bold ? F_NASKH_B : F_NASKH_R;
    out[n++] = F_EMOJI;
    out[n++] = bold ? F_SYS_JP_B : F_SYS_JP_R;
    out[n++] = bold ? F_SYS_KR_B : F_SYS_KR_R;
    out[n++] = F_SYS_CN;
    out[n++] = bold ? F_SYS_TH_B : F_SYS_TH_R;
    return n;
}

static int font_has(int idx, uint32_t cp)
{
    font *f = &s_fonts[idx];
    if (!f->tried)
        load_system_font(idx);
    return f->ft && FT_Get_Char_Index(f->ft, cp) != 0;
}

static void set_size(font *f, float size)
{
    const int s64 = (int)lroundf(size * 64.0f);
    if (f->size64 != s64) {
        FT_Set_Char_Size(f->ft, 0, s64, 72, 72);
        hb_font_set_scale(f->hb, s64, s64);
        f->size64 = s64;
    }
}

/* ---- UTF-8 ----------------------------------------------------------------- */

static int utf8_decode(const char *s, uint32_t *out, int max)
{
    int n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < max) {
        uint32_t cp;
        int len;
        if (p[0] < 0x80) { cp = p[0]; len = 1; }
        else if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f); len = 2;
        } else if ((p[0] & 0xf0) == 0xe0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
            len = 3;
        } else if ((p[0] & 0xf8) == 0xf0 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
                   (p[3] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                 ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
            len = 4;
        } else { cp = 0xfffd; len = 1; }
        /* Control characters and tabs become spaces. */
        if (cp < 0x20 || cp == 0x7f)
            cp = ' ';
        out[n++] = cp;
        p += len;
    }
    return n;
}

/* ---- layout ------------------------------------------------------------- */

typedef struct glyph {
    uint32_t gid;
    float adv, xoff, yoff;
    uint8_t font;
} glyph;

typedef struct run {
    int font, level;
    int g0, g1;        /* glyphs [g0, g1) */
} run;

#define MAX_CPS 1024
#define MAX_GLYPHS 2048
#define MAX_RUNS 128

typedef struct layout {
    glyph g[MAX_GLYPHS];
    run r[MAX_RUNS];
    int order[MAX_RUNS];   /* visual order of runs */
    int ng, nr;
    float width;
} layout;

static layout s_lay;   /* the render thread's scratch (one layout at a time) */

static int is_space_like(uint32_t cp)
{
    return cp == ' ' || cp == 0xa0 || (cp >= 0x2000 && cp <= 0x200b) || cp == 0x3000;
}

static int needs_bidi(const uint32_t *cps, int n)
{
    for (int i = 0; i < n; i++)
        if ((cps[i] >= 0x0590 && cps[i] <= 0x08ff) || (cps[i] >= 0xfb1d && cps[i] <= 0xfeff) ||
            (cps[i] >= 0x200e && cps[i] <= 0x202e))
            return 1;
    return 0;
}

/* Shapes cps[0..n) into s_lay (visual order ready to draw). */
static void layout_cps(ui_weight w, float size, const uint32_t *in_cps, int in_n)
{
    layout *L = &s_lay;
    int ch[8];
    /* Characters no font can draw are left out rather than shown as boxes
     * (and emoji variation selectors / joiners go with them). */
    static uint32_t cps[MAX_CPS];
    int n = 0;
    {
        int chk[8];
        const int nchk = chain(w, chk);
        for (int i = 0; i < in_n && n < MAX_CPS; i++) {
            const uint32_t cp = in_cps[i];
            int ok = is_space_like(cp);
            for (int k = 0; !ok && k < nchk; k++)
                ok = font_has(chk[k], cp);
            if (ok)
                cps[n++] = cp;
        }
    }
    const int nch = chain(w, ch);
    static FriBidiLevel levels[MAX_CPS];
    static int fonts_of[MAX_CPS];

    L->ng = L->nr = 0;
    L->width = 0.0f;
    if (n <= 0)
        return;

    /* Embedding levels (all 0 for plain left-to-right text). */
    if (needs_bidi(cps, n)) {
        static FriBidiCharType types[MAX_CPS];
        static FriBidiBracketType btypes[MAX_CPS];
        FriBidiParType base = FRIBIDI_PAR_ON;
        fribidi_get_bidi_types((const FriBidiChar *)cps, n, types);
        fribidi_get_bracket_types((const FriBidiChar *)cps, n, types, btypes);
        if (!fribidi_get_par_embedding_levels_ex(types, btypes, n, &base, levels))
            memset(levels, 0, (size_t)n * sizeof levels[0]);
    } else {
        memset(levels, 0, (size_t)n * sizeof levels[0]);
    }

    /* Font per character: the first in the chain that has it (so digits
     * after an emoji go back to Inter); spaces keep the run's font. */
    int cur = ch[0];
    for (int i = 0; i < n; i++) {
        int pick = -1;
        if (is_space_like(cps[i])) {
            pick = cur == F_EMOJI ? ch[0] : cur;   /* the emoji font's space is wide */
        } else {
            for (int k = 0; k < nch; k++)
                if (font_has(ch[k], cps[i])) { pick = ch[k]; break; }
        }
        if (pick < 0)
            pick = ch[0];          /* nothing has it: .notdef from Inter */
        fonts_of[i] = cur = pick;
    }

    /* Runs of one font and level, shaped in logical order. */
    hb_buffer_t *buf = hb_buffer_create();
    for (int i = 0; i < n && L->nr < MAX_RUNS;) {
        int j = i + 1;
        while (j < n && fonts_of[j] == fonts_of[i] && levels[j] == levels[i])
            j++;
        font *f = &s_fonts[fonts_of[i]];
        set_size(f, size);
        hb_buffer_clear_contents(buf);
        hb_buffer_add_utf32(buf, cps, n, (unsigned)i, j - i);
        hb_buffer_set_direction(buf, (levels[i] & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(f->hb, buf, NULL, 0);
        unsigned gn = 0;
        hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, &gn);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, NULL);
        run *r = &L->r[L->nr];
        r->font = fonts_of[i];
        r->level = levels[i];
        r->g0 = L->ng;
        for (unsigned k = 0; k < gn && L->ng < MAX_GLYPHS; k++) {
            glyph *g = &L->g[L->ng++];
            g->gid = gi[k].codepoint;
            g->adv = gp[k].x_advance / 64.0f;
            g->xoff = gp[k].x_offset / 64.0f;
            g->yoff = gp[k].y_offset / 64.0f;
            g->font = (uint8_t)fonts_of[i];
            L->width += g->adv;
        }
        r->g1 = L->ng;
        L->nr++;
        i = j;
    }
    hb_buffer_destroy(buf);

    /* Visual order of the runs (UAX #9, rule L2). */
    int maxl = 0, minodd = 255;
    for (int k = 0; k < L->nr; k++) {
        L->order[k] = k;
        if (L->r[k].level > maxl) maxl = L->r[k].level;
        if ((L->r[k].level & 1) && L->r[k].level < minodd) minodd = L->r[k].level;
    }
    for (int lv = maxl; lv >= minodd && lv > 0; lv--) {
        for (int k = 0; k < L->nr;) {
            if (L->r[L->order[k]].level < lv) { k++; continue; }
            int e = k;
            while (e < L->nr && L->r[L->order[e]].level >= lv)
                e++;
            for (int a = k, b = e - 1; a < b; a++, b--) {
                int t = L->order[a]; L->order[a] = L->order[b]; L->order[b] = t;
            }
            k = e;
        }
    }
}

static int decode_into(const char *utf8, uint32_t *cps)
{
    return utf8 ? utf8_decode(utf8, cps, MAX_CPS) : 0;
}

/* ---- glyph cache -------------------------------------------------------- */

typedef struct gent {
    uint32_t gid;
    int32_t size64;
    uint8_t font, sub, used, pad;
    int16_t left, top;
    ui_mask mask;
} gent;

#define GCACHE 8192
static gent s_cache[GCACHE];
static int s_cache_used;

static void cache_flush(void)
{
    for (int i = 0; i < GCACHE; i++)
        if (s_cache[i].used)
            free(s_cache[i].mask.a);
    memset(s_cache, 0, sizeof s_cache);
    s_cache_used = 0;
}

static const gent *glyph_bitmap(int fidx, uint32_t gid, float size, int sub)
{
    const int32_t s64 = (int32_t)lroundf(size * 64.0f);
    uint32_t h = (gid * 2654435761u) ^ ((uint32_t)fidx * 40503u) ^ ((uint32_t)s64 * 9973u) ^ (uint32_t)sub;
    for (int probe = 0; probe < 32; probe++) {
        gent *e = &s_cache[(h + (uint32_t)probe) & (GCACHE - 1)];
        if (!e->used)
            break;
        if (e->gid == gid && e->size64 == s64 && e->font == fidx && e->sub == sub)
            return e;
    }
    if (s_cache_used > GCACHE * 3 / 4)
        cache_flush();

    font *f = &s_fonts[fidx];
    set_size(f, size);
    FT_Vector delta = {sub * 16, 0};    /* quarter-pixel horizontal phase */
    FT_Set_Transform(f->ft, NULL, &delta);
    int err = FT_Load_Glyph(f->ft, gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP);
    if (!err)
        err = FT_Render_Glyph(f->ft->glyph, FT_RENDER_MODE_NORMAL);
    FT_Set_Transform(f->ft, NULL, NULL);

    for (int probe = 0; probe < 32; probe++) {
        gent *e = &s_cache[(h + (uint32_t)probe) & (GCACHE - 1)];
        if (e->used)
            continue;
        e->used = 1;
        e->gid = gid;
        e->size64 = s64;
        e->font = (uint8_t)fidx;
        e->sub = (uint8_t)sub;
        memset(&e->mask, 0, sizeof e->mask);
        if (!err) {
            const FT_Bitmap *bm = &f->ft->glyph->bitmap;
            e->left = (int16_t)f->ft->glyph->bitmap_left;
            e->top = (int16_t)f->ft->glyph->bitmap_top;
            if (bm->width && bm->rows && bm->pixel_mode == FT_PIXEL_MODE_GRAY) {
                e->mask.a = (uint8_t *)malloc((size_t)bm->width * bm->rows);
                if (e->mask.a) {
                    for (unsigned y = 0; y < bm->rows; y++)
                        memcpy(e->mask.a + (size_t)y * bm->width,
                               bm->buffer + (ptrdiff_t)y * bm->pitch, bm->width);
                    e->mask.w = (int)bm->width;
                    e->mask.h = (int)bm->rows;
                    e->mask.pitch = (int)bm->width;
                }
            }
        }
        s_cache_used++;
        return e;
    }
    return NULL;
}

/* ---- public ----------------------------------------------------------------- */

void ui_text_metrics(ui_weight w, float size, float *ascent, float *descent)
{
    font *f = &s_fonts[F_INTER_R + (int)w];
    if (!s_ready || !f->ft) {
        *ascent = size * 0.97f;
        *descent = size * 0.24f;
        return;
    }
    set_size(f, size);
    *ascent = f->ft->size->metrics.ascender / 64.0f;
    *descent = -f->ft->size->metrics.descender / 64.0f;
}

float ui_text_width(ui_weight w, float size, const char *utf8)
{
    static uint32_t cps[MAX_CPS];
    if (!s_ready || !utf8 || !*utf8)
        return 0.0f;
    layout_cps(w, size, cps, decode_into(utf8, cps));
    return s_lay.width;
}

static void draw_layout(ui_canvas *c, float size, float x, float baseline, ui_color color)
{
    const layout *L = &s_lay;
    float pen = x;
    const int by = (int)lroundf(baseline);
    for (int k = 0; k < L->nr; k++) {
        const run *r = &L->r[L->order[k]];
        for (int i = r->g0; i < r->g1; i++) {
            const glyph *g = &L->g[i];
            const float gx = pen + g->xoff;
            int ix = (int)floorf(gx);
            int sub = (int)lroundf((gx - (float)ix) * 4.0f);
            if (sub == 4) { ix++; sub = 0; }
            const gent *e = glyph_bitmap(g->font, g->gid, size, sub);
            if (e && e->mask.a)
                ui_draw_mask(c, &e->mask, ix + e->left, by - (int)lroundf(g->yoff) - e->top, color);
            pen += g->adv;
        }
    }
}

float ui_text_draw(ui_canvas *c, ui_weight w, float size, float x, float baseline, ui_color color,
                   const char *utf8, float max_w)
{
    static uint32_t cps[MAX_CPS + 1];
    if (!s_ready || !utf8 || !*utf8)
        return 0.0f;
    int n = decode_into(utf8, cps);
    layout_cps(w, size, cps, n);
    if (max_w > 0.0f && s_lay.width > max_w) {
        /* Longest prefix (trailing spaces dropped) that fits with an ellipsis. */
        static uint32_t trial[MAX_CPS + 1];
        int lo = 0, hi = n;
        while (lo < hi) {
            const int mid = (lo + hi + 1) / 2;
            int m = mid;
            while (m > 0 && is_space_like(cps[m - 1]))
                m--;
            memcpy(trial, cps, (size_t)m * sizeof trial[0]);
            trial[m] = 0x2026;
            layout_cps(w, size, trial, m + 1);
            if (s_lay.width <= max_w) lo = mid; else hi = mid - 1;
        }
        int m = lo;
        while (m > 0 && is_space_like(cps[m - 1]))
            m--;
        memcpy(trial, cps, (size_t)m * sizeof trial[0]);
        trial[m] = 0x2026;
        layout_cps(w, size, trial, m + 1);
    }
    if (c)
        draw_layout(c, size, x, baseline, color);
    return s_lay.width;
}

float ui_text_draw_aligned(ui_canvas *c, ui_weight w, float size, float x, float baseline,
                           int align, ui_color color, const char *utf8, float max_w)
{
    float tw = ui_text_width(w, size, utf8);
    if (max_w > 0.0f && tw > max_w)
        tw = max_w;
    const float left = align == 1 ? x - tw * 0.5f : (align == 2 ? x - tw : x);
    return ui_text_draw(c, w, size, left, baseline, color, utf8, max_w);
}

static int is_cjk(uint32_t cp)
{
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0xff00 && cp <= 0xffef) || (cp >= 0x20000 && cp <= 0x2fa1f);
}

/* UTF-8 of cps[a..b) into out. */
static void encode_range(const uint32_t *cps, int a, int b, char *out, size_t cap)
{
    size_t o = 0;
    for (int i = a; i < b && o + 5 < cap; i++) {
        uint32_t cp = cps[i];
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) { out[o++] = (char)(0xc0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3f)); }
        else if (cp < 0x10000) {
            out[o++] = (char)(0xe0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        } else {
            out[o++] = (char)(0xf0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    out[o] = 0;
}

int ui_text_draw_wrapped(ui_canvas *c, ui_weight w, float size, float x, float baseline,
                         float max_w, float line_h, int max_lines, ui_color color,
                         const char *utf8)
{
    static uint32_t all[MAX_CPS];
    static char line[MAX_CPS * 4 + 8];
    if (!s_ready || !utf8 || !*utf8 || max_lines <= 0)
        return 0;
    const int n = decode_into(utf8, all);
    int start = 0, lines = 0;
    while (start < n && lines < max_lines) {
        while (start < n && is_space_like(all[start]))
            start++;
        if (start >= n)
            break;
        /* Greedy: the furthest break whose text fits. */
        int best = -1;
        for (int i = start + 1; i <= n; i++) {
            const int brk = i == n || is_space_like(all[i]) || is_cjk(all[i]) || is_cjk(all[i - 1]);
            if (!brk)
                continue;
            layout_cps(w, size, all + start, i - start);
            if (s_lay.width <= max_w)
                best = i;
            else
                break;
        }
        if (best < 0) {
            /* One word wider than the line: cut it by characters. */
            best = start + 1;
            for (int i = start + 1; i <= n; i++) {
                layout_cps(w, size, all + start, i - start);
                if (s_lay.width > max_w)
                    break;
                best = i;
            }
        }
        const int last = lines == max_lines - 1;
        if (last && best < n) {
            encode_range(all, start, n, line, sizeof line);
            ui_text_draw(c, w, size, x, baseline + lines * line_h, color, line, max_w);
        } else {
            encode_range(all, start, best, line, sizeof line);
            if (c)
                ui_text_draw(c, w, size, x, baseline + lines * line_h, color, line, 0.0f);
        }
        lines++;
        start = best;
    }
    return lines;
}
