/*
 * The Nuvio Player's interface: drawing. Sizes, colours and spacing are
 * NuvioTVSmart's Android TV player port (css/components-29..37, the
 * "ATV dp x2" values for a 1920x1080 canvas), so the native controls look
 * like the ones Nuvio draws in the browser on every other TV.
 */
#include "nuvio_osd.h"

#include "nuvio_subs.h"
#include "ui_icons.h"
#include "ui_image.h"
#include "ui_text.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr float W = 1920.0f, H = 1080.0f;
constexpr float PAD_X = 64.0f, PAD_Y = 48.0f;           /* --player-controls-x/y */
constexpr float BTN = 96.0f, BTN_ICON = 48.0f, BTN_GAP = 8.0f;
constexpr uint32_t SECONDARY = 0xf5f5f5;               /* --secondary-color */
constexpr uint32_t ON_SECONDARY = 0x111111;
constexpr uint32_t FOCUS_BG = 0x303030;                /* --focus-bg */
constexpr uint32_t ELEVATED = 0x1a1a1a;
constexpr uint32_t CARD = 0x242424;
constexpr uint32_t TEXT_2 = 0xb3b3b3, TEXT_3 = 0x808080;
constexpr float RAISED_BOTTOM = 335.0f;                /* --player-action-controls-open-bottom */

inline ui_color col(uint32_t rgb, float a)
{
    a = a < 0 ? 0 : (a > 1 ? 1 : a);
    return ((uint32_t)(a * 255.0f + 0.5f) << 24) | (rgb & 0xffffff);
}

/* Baseline for text of `size` centred in a line box of height lh at top. */
float baseline(float top, float lh, float size, ui_weight w)
{
    float asc = 0, desc = 0;
    ui_text_metrics(w, size, &asc, &desc);
    return top + (lh - (asc + desc)) * 0.5f + asc;
}

void vgrad2(ui_canvas &c, float x, float y, float w, float h, float a0, float a1)
{
    const float pos[2] = {0.0f, 1.0f};
    const ui_color cs[2] = {col(0, a0), col(0, a1)};
    ui_fill_vgradient(&c, x, y, w, h, pos, cs, 2);
}

std::string fmt_replace(const char *pattern, const std::string &value)
{
    std::string s = pattern;
    for (const char *ph : {"%1$s", "%s", "{0}"}) {
        const size_t at = s.find(ph);
        if (at != std::string::npos)
            return s.replace(at, std::strlen(ph), value);
    }
    return s + " " + value;
}

std::string upper(const std::string &s)
{
    std::string o = s;
    for (auto &ch : o)
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 32);
    return o;
}

/* A rounded focus ring drawn just outside a box. */
void ring(ui_canvas &c, float x, float y, float w, float h, float r, float t, ui_color color)
{
    ui_stroke_rrect(&c, x - t, y - t, w + 2 * t, h + 2 * t, r + t, t, color);
}

/* A selectable row in Nuvio's dialog style: main line, optional sub line,
 * check on the right when selected. Returns the row height. */
float dialog_item(ui_canvas &c, float x, float y, float w, const std::string &main,
                  const std::string &sub, bool focused, bool selected, bool disabled, float a,
                  const std::string &chip = std::string())
{
    const float pad_y = 16, pad_x = 24, main_lh = 38, sub_lh = 34;
    const float h = pad_y * 2 + main_lh + (sub.empty() ? 0 : 4 + sub_lh);
    const float fa = disabled ? 0.58f : 1.0f;
    if (selected)
        ui_fill_rrect(&c, x, y, w, h, 24, col(SECONDARY, a));
    if (focused && selected)
        ring(c, x, y, w, h, 24, 4, col(0xffffff, a));      /* outside: visible on the light fill */
    else if (focused)
        ui_stroke_rrect(&c, x, y, w, h, 24, 4, col(0xffffff, a));
    const uint32_t fg = selected ? ON_SECONDARY : 0xffffff;
    const uint32_t fg2 = selected ? 0x333333 : TEXT_2;
    float right = x + w - pad_x;
    if (selected) {
        ui_draw_icon(&c, UI_ICON_CHECK, right - 20, y + h * 0.5f, 40, col(fg, a));
        right -= 52;
    }
    if (!chip.empty()) {
        const float cw = ui_text_width(UI_SEMIBOLD, 22, chip.c_str()) + 28;
        ui_fill_rrect(&c, right - cw, y + pad_y + 3, cw, 32, 16,
                      col(selected ? 0x000000 : 0xffffff, selected ? 0.14f * a : 0.10f * a));
        ui_text_draw_aligned(&c, UI_SEMIBOLD, 22, right - cw * 0.5f,
                             baseline(y + pad_y + 3, 32, 22, UI_SEMIBOLD), 1, col(fg, a * fa),
                             chip.c_str(), 0);
        right -= cw + 12;
    }
    ui_text_draw(&c, UI_SEMIBOLD, 32, x + pad_x, baseline(y + pad_y, main_lh, 32, UI_SEMIBOLD),
                 col(fg, a * fa), main.c_str(), right - (x + pad_x));
    if (!sub.empty())
        ui_text_draw(&c, UI_REGULAR, 26, x + pad_x,
                     baseline(y + pad_y + main_lh + 4, sub_lh, 26, UI_REGULAR), col(fg2, a * fa),
                     sub.c_str(), right - (x + pad_x));
    return h;
}

/* Keeps row `focus` of a list inside [0, visible) by moving *scroll. */
void keep_visible(int focus, int count, int visible, int *scroll)
{
    if (visible <= 0)
        return;
    if (focus < *scroll)
        *scroll = focus;
    if (focus >= *scroll + visible)
        *scroll = focus - visible + 1;
    *scroll = std::max(0, std::min(*scroll, std::max(0, count - visible)));
}

} // namespace

/* ---- render ------------------------------------------------------------------- */

bool NuvioOsd::render(ui_canvas &c, const NuvioStatus &st)
{
    if (!m_req)
        return false;
    const double shown_pos = m_seeking ? m_seek_target : st.position;
    const bool time_visible = a_controls.v > 0 || a_seek.v > 0 || a_pause.v > 0 || a_card.v > 0;
    const int second = (int)shown_pos;
    const time_t now_t = time(nullptr);
    const int minute = (int)(now_t / 60);
    const unsigned images = ui_image_generation();
    int sub_state = 0;
    if (m_dialog == DLG_SUBTITLES) {
        const int n = nuvio_subs_count();
        sub_state = n * 7 + nuvio_subs_selected() * 131;
        for (int i = 0; i < n; i++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(i, &t) == 0)
                sub_state = sub_state * 3 + t.state + 1;
        }
        if (sub_state != m_drawn_sub_state && m_sub_langs.size() <= 1)
            build_sub_langs();
    }
    if (!m_dirty && (!time_visible || second == m_drawn_second) && minute == m_drawn_minute &&
        images == m_drawn_images && sub_state == m_drawn_sub_state)
        return false;
    if (sub_state != m_drawn_sub_state && m_dialog == DLG_SUBTITLES)
        build_sub_langs();
    m_dirty = false;
    m_drawn_second = second;
    m_drawn_minute = minute;
    m_drawn_images = images;
    m_drawn_sub_state = sub_state;

    ui_canvas_clear(&c);
    const float la = a_loading.eased();
    if (la > 0.001f)
        draw_loading(c, st, la);
    if (a_pause.v > 0.001f)
        draw_pause(c, st, a_pause.eased());
    if (a_controls.v > 0.001f)
        draw_controls(c, st, a_controls.eased());
    if (a_seek.v > 0.001f)
        draw_seekbar(c, st, a_seek.eased() * (1.0f - a_controls.eased()));
    if (a_skip.v > 0.001f)
        draw_skip(c, st, a_skip.eased());
    if (a_card.v > 0.001f)
        draw_card(c, st, a_card.eased());
    if (a_spinner.v > 0.001f)
        draw_spinner(c, st, a_spinner.eased());
    if (a_dialog.v > 0.001f)
        draw_dialog(c, st, a_dialog.eased());
    if (a_error.v > 0.001f)
        draw_error(c, st, a_error.eased());
    if (a_toast.v > 0.001f)
        draw_toast(c, a_toast.eased());

    m_drew_something = la > 0.001f || a_pause.v > 0.001f || a_controls.v > 0.001f ||
                       a_seek.v > 0.001f || a_skip.v > 0.001f || a_card.v > 0.001f ||
                       a_spinner.v > 0.001f || a_dialog.v > 0.001f || a_error.v > 0.001f ||
                       a_toast.v > 0.001f;
    return true;
}

/* ---- loading -------------------------------------------------------------------- */

void NuvioOsd::draw_loading(ui_canvas &c, const NuvioStatus &st, float a)
{
    /* .player-loading-backdrop + .player-loading-gradient */
    ui_fill_rect(&c, 0, 0, W, H, col(0x080b10, a));
    int failed = 0;
    if (const ui_image *bd = ui_image_get(m_img_backdrop, &failed))
        ui_draw_image_cover(&c, bd, 0, 0, W, H, a * 0.92f);
    const float pos[4] = {0.0f, 0.35f, 0.7f, 1.0f};
    const ui_color g[4] = {col(0, 0.3f * a), col(0, 0.6f * a), col(0, 0.8f * a), col(0, 0.9f * a)};
    ui_fill_vgradient(&c, 0, 0, W, H, pos, g, 4);

    /* .player-loading-identity: fades in after 400 ms, pulses until the
     * stream starts filling, then the logo fills left to right. */
    const float t = (float)(st.now - m_load_since);
    float ia = t < 0.4f ? 0.0f : std::min(1.0f, (t - 0.4f) / 0.7f);
    const float progress = std::max(0.0f, std::min(1.0f, st.open_progress));
    if (progress <= 0.0f)
        ia *= 0.75f + 0.25f * (0.5f + 0.5f * std::cos((t - 0.4f) * 3.14159f));
    ia *= a;

    const float cx = W * 0.5f;
    float y = H * 0.5f - 150.0f;
    int logo_failed = 0;
    const ui_image *logo = ui_image_get(m_img_logo, &logo_failed);
    const bool use_logo = logo && !m_req->logo.empty();
    if (use_logo) {
        float lw, lh;
        ui_image_fit(logo, 640, 230, &lw, &lh);
        const float lx = cx - lw * 0.5f, ly = y + (230 - lh) * 0.5f;
        if (progress > 0.0f) {
            ui_draw_image(&c, logo, lx, ly, lw, lh, ia * 0.25f);
            ui_push_clip(&c, lx, ly, lw * progress, lh);
            ui_draw_image(&c, logo, lx, ly, lw, lh, ia);
            ui_pop_clip(&c);
        } else {
            ui_draw_image(&c, logo, lx, ly, lw, lh, ia);
        }
        y += 236;
    } else if (m_req->logo.empty() || logo_failed) {
        y += 90;
        ui_text_draw_aligned(&c, UI_BOLD, 42, cx, baseline(y, 52, 42, UI_BOLD), 1, col(0xffffff, ia),
                             m_req->header_title().c_str(), 1500);
        y += 58;
        const std::string sub = m_req->header_subtitle();
        if (!sub.empty()) {
            ui_text_draw_aligned(&c, UI_MEDIUM, 30, cx, baseline(y, 38, 30, UI_MEDIUM), 1,
                                 col(0xf2f7fe, 0.9f * ia), sub.c_str(), 1500);
            y += 42;
        }
    } else {
        y += 236;   /* the logo is still on its way: keep the layout steady */
    }
    if (st.open_stage && *st.open_stage) {
        ui_text_draw_aligned(&c, UI_REGULAR, 24, cx, baseline(y + 6, 32, 24, UI_REGULAR), 1,
                             col(0xffffff, 0.72f * ia), st.open_stage, 1300);
    }
}

/* ---- controls ------------------------------------------------------------------- */

void NuvioOsd::draw_controls(ui_canvas &c, const NuvioStatus &st, float a)
{
    /* Gradients: top 300 px to 0.7 black, bottom 400 px to 0.8 black. */
    vgrad2(c, 0, 0, W, 300, 0.7f * a, 0.0f);
    vgrad2(c, 0, H - 400, W, 400, 0.0f, 0.8f * a);

    /* Clock and "Ends at", top right. */
    if (m_req->prefs.show_clock) {
        const float right = W - PAD_X - 4, top = PAD_Y + 4;
        ui_text_draw_aligned(&c, UI_SEMIBOLD, 26, right, baseline(top, 31, 26, UI_SEMIBOLD), 2,
                             col(0xffffff, 0.96f * a), clock_text(st.now).c_str(), 0);
        if (st.duration > 0) {
            const double left = std::max(0.0, st.duration - st.position);
            const std::string ends = fmt_replace(m_req->str("ends_at", "Ends at %1$s"),
                                                 clock_text(st.now, left));
            ui_text_draw_aligned(&c, UI_REGULAR, 20, right, baseline(top + 31, 24, 20, UI_REGULAR), 2,
                                 col(0xffffff, 0.78f * a), ends.c_str(), 0);
        }
    }

    /* Buttons row, bottom. */
    const float row_top = H - PAD_Y - BTN;
    const std::vector<Button> bs = buttons(st);
    float x = PAD_X;
    for (size_t i = 0; i < bs.size(); i++) {
        const bool focused = m_zone == ZONE_BUTTONS && (int)i == m_focus;
        if (focused)
            ui_fill_circle(&c, x + BTN * 0.5f, row_top + BTN * 0.5f, BTN * 0.5f, col(0xffffff, a));
        const ui_color ic = focused ? col(0x000000, a) : col(0xf4f8fd, a);
        ui_icon icon = UI_ICON_PLAY;
        switch (bs[i]) {
        case BTN_PLAY:     icon = st.paused ? UI_ICON_PLAY : UI_ICON_PAUSE; break;
        case BTN_NEXT:     icon = UI_ICON_NEXT; break;
        case BTN_SUBS:     icon = UI_ICON_SUBTITLES; break;
        case BTN_AUDIO:    icon = st.audio_active >= 0 ? UI_ICON_AUDIO_FILLED : UI_ICON_AUDIO; break;
        case BTN_SOURCES:  icon = UI_ICON_SOURCE; break;
        case BTN_EPISODES: icon = UI_ICON_EPISODES; break;
        case BTN_MORE:     icon = UI_ICON_CHEVRON_RIGHT; break;
        case BTN_LESS:     icon = UI_ICON_CHEVRON_LEFT; break;
        case BTN_ASPECT:   icon = UI_ICON_ASPECT; break;
        }
        const int isz = (bs[i] == BTN_MORE || bs[i] == BTN_LESS) ? 56 : (int)BTN_ICON;
        ui_draw_icon(&c, icon, x + BTN * 0.5f, row_top + BTN * 0.5f, isz, ic);
        x += BTN + BTN_GAP;
    }

    /* Time label, right of the buttons. */
    const double shown = m_seeking ? m_seek_target : st.position;
    std::string tl = nuvio_format_time(shown);
    if (st.duration > 0)
        tl += " / " + nuvio_format_time(st.duration);
    ui_text_draw_aligned(&c, UI_REGULAR, 32, W - PAD_X, baseline(row_top, BTN, 32, UI_REGULAR), 2,
                         col(0xffffff, 0.9f * a), tl.c_str(), 0);

    /* Progress bar: 12 px, 20 px while focused. */
    const bool pf = m_zone == ZONE_PROGRESS;
    const float th = pf ? 20.0f : 12.0f;
    const float tcy = row_top - 32.0f - 10.0f;
    const float tx = PAD_X, tw = W - 2 * PAD_X, ty = tcy - th * 0.5f;
    ui_fill_rrect(&c, tx, ty, tw, th, th * 0.5f, col(0xffffff, (pf ? 0.45f : 0.3f) * a));
    if (st.duration > 0) {
        const float bfrac = (float)std::max(0.0, std::min(1.0, st.buffered / st.duration));
        if (bfrac > 0)
            ui_fill_rrect(&c, tx, ty, tw * bfrac, th, th * 0.5f, col(SECONDARY, 0.35f * a));
        const float frac = (float)std::max(0.0, std::min(1.0, shown / st.duration));
        if (frac > 0)
            ui_fill_rrect(&c, tx, ty, std::max(th, tw * frac), th, th * 0.5f, col(SECONDARY, a));
        if (pf || m_seeking)
            ui_fill_circle(&c, tx + tw * frac, tcy, pf ? 16.0f : 12.0f, col(0xffffff, a));
    }

    /* Title block above the bar (Nuvio: .player-meta). */
    float bottom = ty - 24.0f;
    const float maxw = W - 2 * PAD_X;
    std::string info = st.quality_line;
    if (!m_req->stream_addon.empty())
        info = info.empty() ? m_req->stream_addon : m_req->stream_addon + "  \xC2\xB7  " + info;
    if (!info.empty()) {
        ui_text_draw(&c, UI_REGULAR, 28, PAD_X, baseline(bottom - 40, 40, 28, UI_REGULAR),
                     col(0xffffff, 0.68f * a), info.c_str(), maxw);
        bottom -= 40;
    }
    const std::string sub = m_req->header_subtitle();
    if (!sub.empty()) {
        ui_text_draw(&c, UI_MEDIUM, 32, PAD_X, baseline(bottom - 48, 48, 32, UI_MEDIUM),
                     col(0xffffff, 0.9f * a), sub.c_str(), maxw);
        bottom -= 48;
    }
    ui_text_draw(&c, UI_SEMIBOLD, 48, PAD_X, baseline(bottom - 64, 64, 48, UI_SEMIBOLD),
                 col(0xffffff, a), m_req->header_title().c_str(), maxw);
}

void NuvioOsd::draw_seekbar(ui_canvas &c, const NuvioStatus &st, float a)
{
    if (a <= 0.001f || st.duration <= 0)
        return;
    vgrad2(c, 0, H - 220, W, 220, 0.0f, 0.62f * a);
    const float gx = 56.0f, gw = W - 112.0f;
    const float text_lh = 26.0f, bottom = H - 32.0f;
    const float ty = bottom - text_lh - 14.0f - 8.0f;
    const double shown = m_seeking ? m_seek_target : st.position;
    const float frac = (float)std::max(0.0, std::min(1.0, shown / st.duration));
    ui_fill_rrect(&c, gx, ty, gw, 8, 3, col(0xffffff, 0.3f * a));
    if (frac > 0)
        ui_fill_rrect(&c, gx, ty, std::max(8.0f, gw * frac), 8, 3, col(SECONDARY, a));
    ui_fill_circle(&c, gx + gw * frac, ty + 4, 10, col(0xffffff, a));

    const float by = baseline(bottom - text_lh, text_lh, 21, UI_BOLD);
    const double delta = shown - st.position;
    if (m_seeking && std::fabs(delta) >= 1.0) {
        const ui_icon icon = delta < 0 ? UI_ICON_FAST_REWIND : UI_ICON_FAST_FORWARD;
        ui_draw_icon(&c, icon, gx + 14, bottom - text_lh * 0.5f, 28, col(0xf6faff, 0.96f * a));
        const std::string d = std::string(delta < 0 ? "-" : "+") + nuvio_format_time(std::fabs(delta));
        ui_text_draw(&c, UI_BOLD, 21, gx + 36, by, col(0xf6faff, 0.96f * a), d.c_str(), 0);
    }
    const std::string pv = nuvio_format_time(shown) + " / " + nuvio_format_time(st.duration);
    ui_text_draw_aligned(&c, UI_SEMIBOLD, 21, gx + gw, by, 2, col(0xf6faff, 0.96f * a), pv.c_str(), 0);
}

/* ---- pause overlay -------------------------------------------------------------- */

void NuvioOsd::draw_pause(ui_canvas &c, const NuvioStatus &st, float a)
{
    {
        const float p[3] = {0.0f, 0.46f, 1.0f};
        const ui_color g[3] = {col(0, 0.28f * a), col(0, 0.56f * a), col(0, 0.82f * a)};
        ui_fill_vgradient(&c, 0, 0, W, H, p, g, 3);
        const float q[4] = {0.0f, 0.34f, 0.62f, 1.0f};
        const ui_color h[4] = {col(0, 0.9f * a), col(0, 0.72f * a), col(0, 0.36f * a), col(0, 0.12f * a)};
        ui_fill_hgradient(&c, 0, 0, W, H, q, h, 4);
    }
    const float gut = 56.0f;
    if (m_req->prefs.show_clock) {
        ui_text_draw_aligned(&c, UI_SEMIBOLD, 40, W - gut, baseline(gut, 40, 40, UI_SEMIBOLD), 2,
                             col(0xffffff, 0.9f * a), clock_text(st.now).c_str(), 0);
        if (st.duration > 0) {
            const std::string ends = fmt_replace(m_req->str("ends_at", "Ends at %1$s"),
                                                 clock_text(st.now, std::max(0.0, st.duration - st.position)));
            ui_text_draw_aligned(&c, UI_REGULAR, 18, W - gut, baseline(gut + 42, 22, 18, UI_REGULAR), 2,
                                 col(0xffffff, 0.68f * a), ends.c_str(), 0);
        }
    }

    /* Measure the column, then centre it vertically. */
    const ui_image *logo = ui_image_get(m_img_logo_small, nullptr);
    float lw = 0, lh = 0;
    if (logo)
        ui_image_fit(logo, 520, 120, &lw, &lh);
    const std::string meta = [&]() {
        std::string m = m_req->year;
        if (m_req->season > 0 && m_req->episode > 0) {
            char b[32];
            std::snprintf(b, sizeof b, "S%dE%d", m_req->season, m_req->episode);
            m += (m.empty() ? "" : "  \xE2\x80\xA2  ") + std::string(b);
        }
        return m;
    }();
    const float desc_w = 1180.0f;
    const int desc_lines = m_req->description.empty()
                               ? 0
                               : ui_text_draw_wrapped(nullptr, UI_REGULAR, 26, 0, 0, desc_w, 37, 4, 0,
                                                      m_req->description.c_str());
    float total = 22;                                       /* kicker */
    total += 14 + (logo ? lh : 65);
    if (!meta.empty()) total += 14 + 26;
    if (!m_req->episode_title.empty()) total += 14 + 43;
    if (desc_lines) total += 14 + desc_lines * 37;
    float y = (H - total) * 0.5f;

    ui_text_draw(&c, UI_SEMIBOLD, 18, gut, baseline(y, 22, 18, UI_SEMIBOLD), col(0xffffff, 0.64f * a),
                 upper(m_req->str("youre_watching", "You're watching")).c_str(), 0);
    y += 22 + 14;
    if (logo) {
        ui_draw_image(&c, logo, gut, y, lw, lh, a);
        y += lh;
    } else {
        ui_text_draw(&c, UI_BOLD, 62, gut, baseline(y, 65, 62, UI_BOLD), col(0xffffff, a),
                     m_req->header_title().c_str(), W - 2 * gut);
        y += 65;
    }
    if (!meta.empty()) {
        y += 14;
        ui_text_draw(&c, UI_SEMIBOLD, 21, gut, baseline(y, 26, 21, UI_SEMIBOLD), col(0xffffff, 0.72f * a),
                     meta.c_str(), 0);
        y += 26;
    }
    if (!m_req->episode_title.empty()) {
        y += 14;
        ui_text_draw(&c, UI_SEMIBOLD, 38, gut, baseline(y, 43, 38, UI_SEMIBOLD), col(0xffffff, 0.94f * a),
                     m_req->episode_title.c_str(), W - 2 * gut);
        y += 43;
    }
    if (desc_lines) {
        y += 14;
        ui_text_draw_wrapped(&c, UI_REGULAR, 26, gut, baseline(y, 37, 26, UI_REGULAR), desc_w, 37, 4,
                             col(0xffffff, 0.8f * a), m_req->description.c_str());
    }
}

/* ---- small overlays -------------------------------------------------------------- */

void NuvioOsd::draw_spinner(ui_canvas &c, const NuvioStatus &, float a)
{
    const float cx = W * 0.5f, cy = H * 0.5f, r = 48.0f;
    ui_stroke_arc(&c, cx, cy, r, 6.0f, 0.0f, 6.2831f, col(0xffffff, 0.14f * a));
    ui_stroke_arc(&c, cx, cy, r, 6.0f, m_spin, m_spin + 1.9f, col(0xffffff, 0.72f * a));
}

void NuvioOsd::draw_toast(ui_canvas &c, float a)
{
    if (m_toast.empty())
        return;
    const float tw = std::min(1600.0f, ui_text_width(UI_MEDIUM, 26, m_toast.c_str()));
    const float w = tw + 68, h = 64, x = (W - w) * 0.5f, y = 40;
    ui_fill_rrect(&c, x, y, w, h, 32, UI_RGBA(9, 13, 20, (int)(0.88f * 255 * a)));
    ui_stroke_rrect(&c, x, y, w, h, 32, 1.5f, col(0xffffff, 0.18f * a));
    ui_text_draw_aligned(&c, UI_MEDIUM, 26, W * 0.5f, baseline(y, h, 26, UI_MEDIUM), 1,
                         col(0xf3f8ff, a), m_toast.c_str(), 1600);
}

void NuvioOsd::draw_skip(ui_canvas &c, const NuvioStatus &st, float a)
{
    const int k = m_skip_index;
    if (k < 0 || k >= (int)m_req->skips.size())
        return;
    const std::string &type = m_req->skips[k].type;
    const char *label = type == "recap" ? m_req->str("skip_recap", "Skip Recap")
                        : type == "preview" ? m_req->str("skip_preview", "Skip Preview")
                                            : m_req->str("skip_intro", "Skip Intro");
    const float tw = ui_text_width(UI_MEDIUM, 28, label);
    const float w = 36 + 40 + 16 + tw + 36, h = 24 + 40 + 24;
    const float bottom = a_controls.eased() > 0 ? 60 + (RAISED_BOTTOM - 60) * a_controls.eased() : 60;
    const float x = PAD_X, y = H - bottom - h;
    const bool focused = (m_controls && m_zone == ZONE_SKIP) || !m_controls;
    ui_fill_rrect(&c, x, y, w, h, 24, focused ? col(FOCUS_BG, a) : UI_RGBA(30, 30, 30, (int)(0.85f * 255 * a)));
    if (focused)
        ring(c, x, y, w, h, 24, 4, col(0xffffff, a));
    ui_draw_icon(&c, UI_ICON_FAST_FORWARD, x + 36 + 20, y + h * 0.5f, 40, col(0xffffff, a));
    ui_text_draw(&c, UI_MEDIUM, 28, x + 36 + 40 + 16, baseline(y, h, 28, UI_MEDIUM), col(0xffffff, a),
                 label, 0);
    (void)st;
}

void NuvioOsd::draw_card(ui_canvas &c, const NuvioStatus &st, float a)
{
    const NuvioEpisode &n = m_req->next;
    const float w = 640, pad = 18, th = 102, tw = 180, h = pad * 2 + th;
    const float bottom = a_controls.eased() > 0 ? 46 + (RAISED_BOTTOM - 46) * a_controls.eased() : 46;
    const float x = W - 56 - w, y = H - bottom - h;
    const bool focused = (m_controls && m_zone == ZONE_NEXT) || (!m_controls && m_skip_index < 0) || m_post_play;
    ui_fill_rrect(&c, x, y, w, h, 24, focused ? col(FOCUS_BG, a) : UI_RGBA(25, 25, 25, (int)(0.89f * 255 * a)));
    if (focused)
        ring(c, x, y, w, h, 24, 4, col(0xffffff, a));
    else
        ui_stroke_rrect(&c, x, y, w, h, 24, 1.5f, col(0xffffff, 0.16f * a));
    ui_push_clip(&c, x + pad, y + pad, tw, th);
    ui_fill_rrect(&c, x + pad, y + pad, tw, th, 16, col(0xffffff, 0.06f * a));
    if (const ui_image *img = ui_image_get(m_img_next, nullptr))
        ui_draw_image_cover(&c, img, x + pad, y + pad, tw, th, a);
    ui_pop_clip(&c);

    /* Pill on the right: "Play" (or when it airs). */
    const char *pill = n.released ? m_req->str("play", "Play") : m_req->str("upcoming", "Upcoming");
    const float pw = ui_text_width(UI_MEDIUM, 18, pill) + 28 + (n.released ? 24 : 0);
    const float px = x + w - pad - pw, py = y + (h - 40) * 0.5f;
    if (focused && n.released) {
        ui_fill_rrect(&c, px, py, pw, 40, 20, col(0xffffff, a));
    } else {
        ui_stroke_rrect(&c, px, py, pw, 40, 20, 1.5f, col(0xffffff, 0.2f * a));
    }
    const ui_color pc = focused && n.released ? col(0x000000, a) : col(0xffffff, (n.released ? 1.0f : 0.72f) * a);
    float tx = px + 14;
    if (n.released) {
        ui_draw_icon(&c, UI_ICON_PLAY, tx + 9, py + 20, 18, pc);
        tx += 24;
    }
    ui_text_draw(&c, UI_MEDIUM, 18, tx, baseline(py, 40, 18, UI_MEDIUM), pc, pill, 0);

    const float cx = x + pad + tw + 20, cw = px - 16 - cx;
    float cy = y + pad + 6;
    ui_text_draw(&c, UI_MEDIUM, 17, cx, baseline(cy, 22, 17, UI_MEDIUM), col(0xffffff, 0.8f * a),
                 m_req->str("next_episode", "Next episode"), cw);
    cy += 24;
    char code[32] = "";
    if (n.season > 0 && n.episode > 0)
        std::snprintf(code, sizeof code, "S%dE%d", n.season, n.episode);
    std::string title = code;
    if (!n.title.empty())
        title += (title.empty() ? "" : "  \xE2\x80\xA2  ") + n.title;
    ui_text_draw(&c, UI_SEMIBOLD, 24, cx, baseline(cy, 30, 24, UI_SEMIBOLD), col(0xffffff, a), title.c_str(), cw);
    cy += 32;
    std::string status;
    if (m_post_play) {
        status = m_req->str("press_to_play_next", "Press \xE2\x9C\x95 to play");
    } else if (!n.released) {
        status = n.released_label;
    } else if (m_req->prefs.autoplay_next && st.duration > 0) {
        status = fmt_replace(m_req->str("next_in", "Playing in %1$s"),
                             nuvio_format_time(std::max(0.0, st.duration - st.position)));
    }
    if (!status.empty())
        ui_text_draw(&c, UI_REGULAR, 15, cx, baseline(cy, 20, 15, UI_REGULAR), col(0xffffff, 0.72f * a),
                     status.c_str(), cw);
}

void NuvioOsd::draw_error(ui_canvas &c, const NuvioStatus &st, float a)
{
    ui_fill_rect(&c, 0, 0, W, H, col(0, 0.94f * a));
    float y = 360;
    ui_draw_icon(&c, UI_ICON_WARNING, W * 0.5f, y, 72, col(0xffffff, 0.86f * a));
    y += 64;
    ui_text_draw_aligned(&c, UI_BOLD, 42, W * 0.5f, baseline(y, 52, 42, UI_BOLD), 1, col(0xffffff, a),
                         m_req->str("playback_error", "Playback Error"), 0);
    y += 68;
    const float mw = 1200;
    const int lines = ui_text_draw_wrapped(nullptr, UI_REGULAR, 24, 0, 0, mw, 32, 4, 0, st.error.c_str());
    {
        /* One line is centred; a longer message is a centred block. */
        const float box_x = (W - mw) * 0.5f;
        const float tw = std::min(mw, ui_text_width(UI_REGULAR, 24, st.error.c_str()));
        const float x = lines <= 1 ? (W - tw) * 0.5f : box_x;
        ui_text_draw_wrapped(&c, UI_REGULAR, 24, x, baseline(y, 32, 24, UI_REGULAR), mw, 32, 4,
                             col(0xffffff, 0.82f * a), st.error.c_str());
    }
    y += std::max(1, lines) * 32 + 40;
    const bool two = m_req->sources.size() > 1;
    const char *b0 = m_req->str("go_back", "Go Back");
    const char *b1 = m_req->str("sources", "Sources");
    const float bw = 260, bh = 64, gap = 24;
    float x = W * 0.5f - (two ? bw + gap * 0.5f : bw * 0.5f);
    for (int i = 0; i < (two ? 2 : 1); i++) {
        const bool f = (m_focus == i) || (!two && i == 0);
        ui_fill_rrect(&c, x, y, bw, bh, 14, f ? col(0xffffff, a) : col(FOCUS_BG, a));
        ui_text_draw_aligned(&c, UI_BOLD, 24, x + bw * 0.5f, baseline(y, bh, 24, UI_BOLD), 1,
                             f ? col(0x000000, a) : col(0xffffff, a), i == 0 ? b0 : b1, bw - 24);
        x += bw + gap;
    }
}

/* ---- dialogs ---------------------------------------------------------------------- */

void NuvioOsd::draw_dialog(ui_canvas &c, const NuvioStatus &st, float a)
{
    /* .player-modal-backdrop (the side panels use a lighter dim). */
    const bool side = m_dialog == DLG_SOURCES || m_dialog == DLG_EPISODES;
    if (side) {
        ui_fill_rect(&c, 0, 0, W, H, col(0, 0.45f * a));
    } else {
        ui_fill_rect(&c, 0, 0, W, H, col(0, 0.34f * a));
        const float hp[2] = {0.0f, 1.0f};
        const ui_color hc[2] = {col(0, 0.88f * a), col(0, 0.0f)};
        ui_fill_hgradient(&c, 0, 0, W, H, hp, hc, 2);
        const float vp[4] = {0.0f, 0.3f, 0.6f, 1.0f};
        const ui_color vc[4] = {col(0, 0.6f * a), col(0, 0.4f * a), col(0, 0.2f * a), col(0, 0.0f)};
        ui_fill_vgradient(&c, 0, 0, W, H, vp, vc, 4);
    }
    switch (m_dialog) {
    case DLG_SUBTITLES: draw_subtitles_dialog(c, st, a); break;
    case DLG_AUDIO:     draw_audio_dialog(c, st, a); break;
    case DLG_SOURCES:   draw_sources_panel(c, st, a); break;
    case DLG_EPISODES:  draw_episodes_panel(c, st, a); break;
    default: break;
    }
}

void NuvioOsd::draw_subtitles_dialog(ui_canvas &c, const NuvioStatus &, float a)
{
    const float x0 = 88, w = 1448, h = 810, top = H - 56 - h;
    ui_text_draw(&c, UI_SEMIBOLD, 56, x0, baseline(top, 66, 56, UI_SEMIBOLD), col(0xffffff, a),
                 m_req->str("subtitles", "Subtitles"), w);
    const float ctop = top + 66 + 24, cbot = top + h;
    const float gap = 28, inner = w - 2 * gap;
    const float cw[3] = {inner * 0.8f / 2.95f, inner * 1.2f / 2.95f, inner * 0.95f / 2.95f};
    const float cx[3] = {x0, x0 + cw[0] + gap, x0 + cw[0] + cw[1] + 2 * gap};
    const char *heads[3] = {m_req->str("language", "Language"), m_req->str("track", "Track"),
                            m_req->str("style", "Style")};
    const int sel = nuvio_subs_selected();
    for (int k = 0; k < 3; k++)
        ui_text_draw(&c, UI_REGULAR, 28, cx[k] + 4, baseline(ctop, 34, 28, UI_REGULAR),
                     col(TEXT_3, a), heads[k], cw[k]);
    const float list_top = ctop + 34 + 12;
    const float list_h = cbot - list_top;

    /* Languages. */
    {
        const int n = (int)m_sub_langs.size();
        const float row_h = 70 + 8;
        const int vis = std::max(1, (int)(list_h / row_h));
        keep_visible(m_dlg_row[0], n, vis, &m_dlg_scroll[0]);
        ui_push_clip(&c, cx[0] - 6, list_top - 6, cw[0] + 12, list_h + 12);
        float y = list_top;
        for (int i = m_dlg_scroll[0]; i < n && y < cbot; i++) {
            const SubLang &l = m_sub_langs[i];
            bool is_sel = i == 0 ? sel < 0 : std::find(l.tracks.begin(), l.tracks.end(), sel) != l.tracks.end();
            std::string chip = i == 0 || l.tracks.size() <= 1 ? std::string() : std::to_string(l.tracks.size());
            /* The language whose tracks are listed, while browsing them. */
            if (m_dlg_col != 0 && m_dlg_row[0] == i && !is_sel)
                ui_fill_rrect(&c, cx[0], y, cw[0], 70, 24, col(0xffffff, 0.12f * a));
            y += dialog_item(c, cx[0], y, cw[0], l.name, "", m_dlg_col == 0 && m_dlg_row[0] == i, is_sel,
                             false, a, chip) + 8;
        }
        ui_pop_clip(&c);
    }

    /* Tracks of the focused language. */
    if (m_dlg_row[0] > 0 && m_dlg_row[0] < (int)m_sub_langs.size()) {
        const SubLang &l = m_sub_langs[m_dlg_row[0]];
        const int n = (int)l.tracks.size();
        const float row_h = 108 + 8;
        const int vis = std::max(1, (int)(list_h / row_h));
        keep_visible(m_dlg_row[1], n, vis, &m_dlg_scroll[1]);
        ui_push_clip(&c, cx[1] - 6, list_top - 6, cw[1] + 12, list_h + 12);
        float y = list_top;
        for (int i = m_dlg_scroll[1]; i < n && y < cbot; i++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(l.tracks[i], &t) != 0)
                continue;
            std::string main = t.title[0] ? t.title : nuvio_language_name(t.lang);
            if (main.empty())
                main = m_req->str("unknown_language", "Unknown");
            std::string sub = t.external ? m_req->str("addon", "Addon") : m_req->str("built_in", "Built-in");
            if (t.external && t.state == 0)
                sub += std::string("  \xC2\xB7  ") + m_req->str("loading", "Loading\xE2\x80\xA6");
            else if (t.external && t.state < 0)
                sub += std::string("  \xC2\xB7  ") + m_req->str("unavailable", "Unavailable");
            else if (t.codec[0] && !t.external)
                sub += std::string("  \xC2\xB7  ") + upper(t.codec[0] == 'h' && !std::strncmp(t.codec, "hdmv", 4) ? "PGS" : t.codec);
            if (t.forced)
                sub += std::string("  \xC2\xB7  ") + m_req->str("forced", "Forced");
            if (t.hearing_impaired)
                sub += "  \xC2\xB7  SDH";
            y += dialog_item(c, cx[1], y, cw[1], main, sub, m_dlg_col == 1 && m_dlg_row[1] == i,
                             l.tracks[i] == sel, t.external && t.state < 0, a) + 8;
        }
        ui_pop_clip(&c);
    } else {
        ui_text_draw_wrapped(&c, UI_REGULAR, 26, cx[1] + 24, baseline(list_top + 16, 34, 26, UI_REGULAR),
                             cw[1] - 48, 34, 3, col(TEXT_2, a),
                             nuvio_subs_count() == 0 ? m_req->str("no_subtitles", "No subtitles found for this stream")
                                                     : m_req->str("subtitles_off", "Subtitles are off"));
    }

    /* Style: steppers with focusable minus / plus, then toggles. */
    {
        nuvio_sub_style s;
        nuvio_subs_get_style(&s);
        char v[6][48];
        std::snprintf(v[0], sizeof v[0], "%+.1f s", nuvio_subs_delay_ms() / 1000.0);
        std::snprintf(v[1], sizeof v[1], "%d%%", s.size_pct);
        std::snprintf(v[2], sizeof v[2], "%.0f%%", s.offset_pct);
        if (s.background < 0.05f)
            std::snprintf(v[3], sizeof v[3], "%s", m_req->str("off", "Off"));
        else
            std::snprintf(v[3], sizeof v[3], "%.0f%%", s.background * 100.0f);
        std::snprintf(v[4], sizeof v[4], "%s", s.outline ? m_req->str("on", "On") : m_req->str("off", "Off"));
        std::snprintf(v[5], sizeof v[5], "%s", s.bold ? m_req->str("on", "On") : m_req->str("off", "Off"));
        const char *labels[6] = {m_req->str("delay", "Delay"), m_req->str("size", "Size"),
                                 m_req->str("position", "Position"), m_req->str("background", "Background"),
                                 m_req->str("outline", "Outline"), m_req->str("bold", "Bold")};
        float y = list_top;
        const float sw = 96, sh = 76;
        for (int i = 0; i < 6 && y < cbot; i++) {
            const bool focus_row = m_dlg_col == 2 && m_dlg_row[2] == i;
            const float rh = 96;
            if (i <= 3) {
                for (int k = 0; k < 2; k++) {
                    const float bx = k == 0 ? cx[2] : cx[2] + cw[2] - sw;
                    const bool f = focus_row && m_dlg_sub == k;
                    ui_fill_rrect(&c, bx, y + (rh - sh) * 0.5f, sw, sh, 24,
                                  f ? col(0xffffff, 0.14f * a) : col(0xffffff, 0.0f));
                    ui_stroke_rrect(&c, bx, y + (rh - sh) * 0.5f, sw, sh, 24, 4,
                                    f ? col(0xffffff, a) : col(0xffffff, 0.18f * a));
                    ui_text_draw_aligned(&c, UI_SEMIBOLD, 44, bx + sw * 0.5f,
                                         baseline(y + (rh - sh) * 0.5f, sh, 44, UI_SEMIBOLD), 1,
                                         col(0xffffff, a), k == 0 ? "\xE2\x88\x92" : "+", 0);
                }
                const float mid = cx[2] + cw[2] * 0.5f;
                ui_text_draw_aligned(&c, UI_REGULAR, 22, mid, baseline(y + 12, 28, 22, UI_REGULAR), 1,
                                     col(TEXT_2, a), labels[i], cw[2] - 2 * sw - 16);
                ui_text_draw_aligned(&c, UI_SEMIBOLD, 32, mid, baseline(y + 42, 40, 32, UI_SEMIBOLD), 1,
                                     col(0xffffff, a), v[i], cw[2] - 2 * sw - 16);
            } else {
                const bool on = i == 4 ? s.outline : s.bold;
                dialog_item(c, cx[2], y + 4, cw[2], labels[i], v[i], focus_row, on, false, a);
            }
            y += rh + 12;
        }
    }
}

void NuvioOsd::draw_audio_dialog(ui_canvas &c, const NuvioStatus &st, float a)
{
    const float x0 = 88, h = 778, top = H - 128 - h;
    ui_text_draw(&c, UI_SEMIBOLD, 56, x0, baseline(top, 66, 56, UI_SEMIBOLD), col(0xffffff, a),
                 m_req->str("audio", "Audio"), 1448);
    const float list_top = top + 66 + 24 + 16, lw = 888, bottom = top + h;
    const int n = (int)st.audio.size();
    if (n == 0) {
        ui_text_draw(&c, UI_REGULAR, 26, x0 + 24, baseline(list_top, 34, 26, UI_REGULAR), col(TEXT_2, a),
                     m_req->str("no_audio_tracks", "No other audio tracks"), lw);
        return;
    }
    const int vis = std::max(1, (int)((bottom - list_top) / (108 + 12)));
    keep_visible(m_dlg_row[0], n, vis, &m_dlg_scroll[0]);
    ui_push_clip(&c, x0 - 6, list_top - 6, lw + 12, bottom - list_top + 12);
    float y = list_top;
    for (int i = m_dlg_scroll[0]; i < n && y < bottom; i++) {
        const NuvioAudioTrack &t = st.audio[i];
        std::string main = nuvio_language_name(t.lang);
        if (main.empty()) {
            char b[32];
            std::snprintf(b, sizeof b, "%s %d", m_req->str("track", "Track"), i + 1);
            main = b;
        }
        std::string sub = t.codec;
        if (!t.channels.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + t.channels;
        if (!t.title.empty()) sub += (sub.empty() ? "" : "  \xC2\xB7  ") + t.title;
        if (t.is_default) sub += std::string(sub.empty() ? "" : "  \xC2\xB7  ") + m_req->str("default", "Default");
        y += dialog_item(c, x0, y, lw, main, sub, m_dlg_row[0] == i, i == st.audio_active, false, a) + 12;
    }
    ui_pop_clip(&c);
}

void NuvioOsd::draw_sources_panel(ui_canvas &c, const NuvioStatus &, float a)
{
    const float pw = 1040, pad = 48;
    const float x = W - pw * a;
    ui_fill_rrect(&c, x, 0, pw + 64, H, 32, col(ELEVATED, 1.0f));
    ui_text_draw(&c, UI_SEMIBOLD, 56, x + pad, baseline(pad, 66, 56, UI_SEMIBOLD), col(0xffffff, 1),
                 m_req->str("sources", "Sources"), pw - 2 * pad);
    float y = pad + 66 + 14;
    const std::string cur = m_req->stream_title.empty() ? m_req->header_title() : m_req->stream_title;
    ui_text_draw(&c, UI_REGULAR, 32, x + pad, baseline(y, 43, 32, UI_REGULAR), col(0xffffff, 0.9f),
                 cur.c_str(), pw - 2 * pad);
    y += 43 + 24;
    const int n = (int)m_req->sources.size();
    const float card_h = 16 * 2 + 2 * 38 + 4 + 2 * 34;
    const int vis = std::max(1, (int)((H - pad - y) / (card_h + 12)));
    keep_visible(m_dlg_row[0], n, vis, &m_dlg_scroll[0]);
    ui_push_clip(&c, x, y - 6, pw, H - pad - y + 12);
    for (int i = m_dlg_scroll[0]; i < n && y < H - pad; i++) {
        const NuvioSource &s = m_req->sources[i];
        const bool f = m_dlg_row[0] == i, playing = i == m_req->source_index;
        const float cx = x + pad, cw = pw - 2 * pad;
        if (f) {
            ui_fill_rrect(&c, cx, y, cw, card_h, 12, col(FOCUS_BG, 1));
            ui_stroke_rrect(&c, cx, y, cw, card_h, 12, 3, col(0xffffff, 1));
        } else {
            ui_fill_rrect(&c, cx, y, cw, card_h, 12, col(CARD, 1));
        }
        float right = cx + cw - 20;
        if (playing) {
            const char *pl = m_req->str("playing", "Playing");
            const float tw = ui_text_width(UI_MEDIUM, 20, pl) + 24;
            ui_fill_rrect(&c, right - tw, y + 18, tw, 32, 16, col(SECONDARY, 1));
            ui_text_draw_aligned(&c, UI_MEDIUM, 20, right - tw * 0.5f, baseline(y + 18, 32, 20, UI_MEDIUM), 1,
                                 col(ON_SECONDARY, 1), pl, 0);
            right -= tw + 12;
        }
        if (!s.addon.empty()) {
            ui_text_draw_aligned(&c, UI_REGULAR, 20, cx + cw - 20, baseline(y + card_h - 44, 28, 20, UI_REGULAR), 2,
                                 col(TEXT_3, 1), s.addon.c_str(), 300);
        }
        const std::string title = s.title.empty() ? s.addon : s.title;
        const int tl = ui_text_draw_wrapped(&c, UI_BOLD, 30, cx + 20, baseline(y + 16, 38, 30, UI_BOLD),
                                            right - cx - 32, 38, 2, col(0xffffff, 1), title.c_str());
        if (!s.description.empty())
            ui_text_draw_wrapped(&c, UI_REGULAR, 24, cx + 20,
                                 baseline(y + 16 + std::max(1, tl) * 38 + 6, 34, 24, UI_REGULAR),
                                 cw - 40 - 320, 34, 2, col(TEXT_2, 1), s.description.c_str());
        y += card_h + 12;
    }
    ui_pop_clip(&c);
    (void)a;
}

void NuvioOsd::draw_episodes_panel(ui_canvas &c, const NuvioStatus &, float a)
{
    const float pw = 1040, pad = 48;
    const float x = W - pw * a;
    ui_fill_rrect(&c, x, 0, pw + 64, H, 32, col(ELEVATED, 1.0f));
    ui_text_draw(&c, UI_SEMIBOLD, 56, x + pad, baseline(pad, 66, 56, UI_SEMIBOLD), col(0xffffff, 1),
                 m_req->header_title().c_str(), pw - 2 * pad);
    float y = pad + 66 + 18;
    const std::vector<int> ss = seasons();
    if (ss.size() > 1) {
        float tx = x + pad;
        for (size_t i = 0; i < ss.size() && tx < x + pw - pad; i++) {
            char b[48];
            if (ss[i] == 0)
                std::snprintf(b, sizeof b, "%s", m_req->str("specials", "Specials"));
            else
                std::snprintf(b, sizeof b, "%s %d", m_req->str("season", "Season"), ss[i]);
            const float tw = ui_text_width(UI_SEMIBOLD, 26, b) + 56;
            const bool sel = (int)i == m_season_tab, f = sel && m_season_focus;
            ui_fill_rrect(&c, tx, y, tw, 64, 32, f ? col(SECONDARY, 1) : col(CARD, 1));
            ui_stroke_rrect(&c, tx, y, tw, 64, 32, 2, f ? col(0xffffff, 1) : sel ? col(0x9e9e9e, 1) : col(0x333333, 1));
            ui_text_draw_aligned(&c, UI_SEMIBOLD, 26, tx + tw * 0.5f, baseline(y, 64, 26, UI_SEMIBOLD), 1,
                                 f ? col(ON_SECONDARY, 1) : sel ? col(0xffffff, 1) : col(TEXT_2, 1), b, 0);
            tx += tw + 20;
        }
        y += 64 + 24;
    }
    std::vector<int> eps;
    const int n = episodes_in_tab(&eps);
    const float item_h = 135 + 2 * 18;
    const int vis = std::max(1, (int)((H - pad - y) / (item_h + 10)));
    keep_visible(m_dlg_row[0], n, vis, &m_dlg_scroll[0]);
    /* Ask for the visible thumbnails first, then draw (requests can evict). */
    std::vector<int> handles;
    for (int i = m_dlg_scroll[0]; i < n && i < m_dlg_scroll[0] + vis + 1; i++) {
        const NuvioEpisode &e = m_req->episodes[eps[i]];
        handles.push_back(e.thumbnail.empty() ? -1 : ui_image_request(e.thumbnail.c_str(), 240, 135, 0));
    }
    ui_push_clip(&c, x, y - 6, pw, H - pad - y + 12);
    for (int i = m_dlg_scroll[0], k = 0; i < n && y < H - pad; i++, k++) {
        const NuvioEpisode &e = m_req->episodes[eps[i]];
        const bool f = !m_season_focus && m_dlg_row[0] == i;
        const bool current = e.season == m_req->season && e.episode == m_req->episode;
        const float ix = x + pad, iw = pw - 2 * pad;
        if (f) {
            ui_fill_rrect(&c, ix, y, iw, item_h, 22, col(FOCUS_BG, 1));
            ui_stroke_rrect(&c, ix, y, iw, item_h, 22, 3, col(0xffffff, 1));
        }
        const float tx0 = ix + 18, ty0 = y + 18;
        ui_push_clip(&c, tx0, ty0, 240, 135);
        ui_fill_rrect(&c, tx0, ty0, 240, 135, 14, col(0xffffff, 0.06f));
        if (k < (int)handles.size())
            if (const ui_image *img = ui_image_get(handles[k], nullptr))
                ui_draw_image_cover(&c, img, tx0, ty0, 240, 135, e.released ? 1.0f : 0.5f);
        ui_pop_clip(&c);
        if (e.watched)
            ui_fill_rect(&c, tx0, ty0 + 131, 240, 4, col(SECONDARY, 1));
        const float cx = tx0 + 240 + 22, cw = ix + iw - 18 - cx;
        char head[32];
        std::snprintf(head, sizeof head, "%d. ", e.episode);
        std::string title = head + (e.title.empty() ? std::string(m_req->str("episode", "Episode")) : e.title);
        float right = cx + cw;
        if (current) {
            const char *pl = m_req->str("playing", "Playing");
            const float tw = ui_text_width(UI_MEDIUM, 20, pl) + 24;
            ui_fill_rrect(&c, right - tw, ty0 + 4, tw, 32, 16, col(SECONDARY, 1));
            ui_text_draw_aligned(&c, UI_MEDIUM, 20, right - tw * 0.5f, baseline(ty0 + 4, 32, 20, UI_MEDIUM), 1,
                                 col(ON_SECONDARY, 1), pl, 0);
            right -= tw + 12;
        }
        ui_text_draw(&c, UI_BOLD, 30, cx, baseline(ty0, 40, 30, UI_BOLD),
                     col(0xffffff, e.released ? 1.0f : 0.6f), title.c_str(), right - cx);
        const std::string sub = !e.released ? e.released_label
                                : !e.overview.empty() ? e.overview : e.released_label;
        if (!sub.empty())
            ui_text_draw_wrapped(&c, UI_REGULAR, 24, cx, baseline(ty0 + 46, 32, 24, UI_REGULAR), cw, 32, 2,
                                 col(TEXT_2, 1), sub.c_str());
        y += item_h + 10;
    }
    ui_pop_clip(&c);
}
