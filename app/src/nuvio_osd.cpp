/*
 * The Nuvio Player's interface: state and input. Drawing is in
 * nuvio_osd_draw.cpp. Behaviour follows NuvioTVSmart's player screen
 * (js/ui/screens/player/playerScreenMethods-71-on-key-down.js and friends):
 *
 *   controls hidden   Left/Right seek (10 s steps, faster when held), Up/Down
 *                     show the controls, Cross pauses and shows them, Circle
 *                     leaves; Cross takes "Skip intro" / "Next episode" when
 *                     those are up
 *   progress bar      Left/Right seek, Down to the buttons, Up hides, Cross
 *                     pauses
 *   buttons           Left/Right move, Up to the progress bar, Down hides,
 *                     Cross acts
 *
 * The controls hide after 4.2 s without input while playing; five seconds
 * paused brings the "You're watching" overlay. Seeks commit one second after
 * the last press.
 */
#include "nuvio_osd.h"

#include "nuvio_subs.h"
#include "ui_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

constexpr double kAutoHide = 4.2;
constexpr double kPauseOverlayAfter = 5.0;
constexpr double kSeekCommit = 1.0;
constexpr double kSkipShowFor = 8.0;

/* Nuvio's scrub steps (core/player/playerScrubRates.js). */
double scrub_step(int repeat)
{
    if (repeat >= 15) return 60.0;
    if (repeat >= 8) return 30.0;
    if (repeat >= 3) return 20.0;
    return 10.0;
}

float ease_out(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

} // namespace

/* ---- helpers shared with the drawing code ----------------------------------- */

std::string nuvio_format_time(double s)
{
    if (!(s > 0)) s = 0;
    const long t = (long)s;
    char b[32];
    if (t >= 3600)
        std::snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else
        std::snprintf(b, sizeof b, "%ld:%02ld", t / 60, t % 60);
    return b;
}

std::string nuvio_language_key(const std::string &code)
{
    static const struct { const char *three, *two; } k[] = {
        {"eng", "en"}, {"spa", "es"}, {"fre", "fr"}, {"fra", "fr"}, {"ger", "de"}, {"deu", "de"},
        {"ita", "it"}, {"por", "pt"}, {"rus", "ru"}, {"jpn", "ja"}, {"kor", "ko"}, {"chi", "zh"},
        {"zho", "zh"}, {"ara", "ar"}, {"hin", "hi"}, {"tur", "tr"}, {"pol", "pl"}, {"dut", "nl"},
        {"nld", "nl"}, {"swe", "sv"}, {"nor", "no"}, {"nob", "nb"}, {"dan", "da"}, {"fin", "fi"},
        {"gre", "el"}, {"ell", "el"}, {"heb", "he"}, {"cze", "cs"}, {"ces", "cs"}, {"hun", "hu"},
        {"rum", "ro"}, {"ron", "ro"}, {"bul", "bg"}, {"hrv", "hr"}, {"srp", "sr"}, {"slv", "sl"},
        {"slo", "sk"}, {"slk", "sk"}, {"ukr", "uk"}, {"tha", "th"}, {"vie", "vi"}, {"ind", "id"},
        {"may", "ms"}, {"msa", "ms"}, {"per", "fa"}, {"fas", "fa"}, {"urd", "ur"}, {"ben", "bn"},
        {"tam", "ta"}, {"tel", "te"}, {"mal", "ml"}, {"kan", "kn"}, {"mar", "mr"}, {"cat", "ca"},
        {"baq", "eu"}, {"eus", "eu"}, {"glg", "gl"}, {"ice", "is"}, {"isl", "is"}, {"est", "et"},
        {"lav", "lv"}, {"lit", "lt"}, {"fil", "tl"}, {"tgl", "tl"}, {"alb", "sq"}, {"sqi", "sq"},
        {"mac", "mk"}, {"mkd", "mk"}, {"bos", "bs"}, {"geo", "ka"}, {"kat", "ka"}, {"arm", "hy"},
        {"hye", "hy"}, {"aze", "az"}, {"kaz", "kk"}, {"uzb", "uz"}, {"mon", "mn"}, {"khm", "km"},
        {"lao", "lo"}, {"bur", "my"}, {"mya", "my"}, {"nep", "ne"}, {"sin", "si"}, {"swa", "sw"},
        {"afr", "af"}, {"amh", "am"}, {"wel", "cy"}, {"cym", "cy"}, {"gle", "ga"}, {"lat", "la"},
        {"pob", "pt-br"}, {"pt-BR", "pt-br"}, {"es-419", "es-419"},
    };
    std::string c;
    for (char ch : code)
        c += (char)(ch == '_' ? '-' : std::tolower((unsigned char)ch));
    for (const auto &e : k)
        if (c == e.three)
            return e.two;
    return c;
}

std::string nuvio_language_name(const std::string &code)
{
    static const struct { const char *k, *name; } n[] = {
        {"en", "English"}, {"es", "Spanish"}, {"es-419", "Spanish (Latin America)"},
        {"es-mx", "Spanish (Mexico)"}, {"fr", "French"}, {"fr-ca", "French (Canada)"},
        {"de", "German"}, {"it", "Italian"}, {"pt", "Portuguese"}, {"pt-br", "Portuguese (Brazil)"},
        {"pt-pt", "Portuguese (Portugal)"}, {"ru", "Russian"}, {"ja", "Japanese"}, {"ko", "Korean"},
        {"zh", "Chinese"}, {"zh-cn", "Chinese (Simplified)"}, {"zh-tw", "Chinese (Traditional)"},
        {"zh-hk", "Chinese (Hong Kong)"}, {"ar", "Arabic"}, {"hi", "Hindi"}, {"tr", "Turkish"},
        {"pl", "Polish"}, {"nl", "Dutch"}, {"sv", "Swedish"}, {"no", "Norwegian"}, {"nb", "Norwegian"},
        {"da", "Danish"}, {"fi", "Finnish"}, {"el", "Greek"}, {"he", "Hebrew"}, {"cs", "Czech"},
        {"hu", "Hungarian"}, {"ro", "Romanian"}, {"bg", "Bulgarian"}, {"hr", "Croatian"},
        {"sr", "Serbian"}, {"sl", "Slovenian"}, {"sk", "Slovak"}, {"uk", "Ukrainian"}, {"th", "Thai"},
        {"vi", "Vietnamese"}, {"id", "Indonesian"}, {"ms", "Malay"}, {"fa", "Persian"}, {"ur", "Urdu"},
        {"bn", "Bengali"}, {"ta", "Tamil"}, {"te", "Telugu"}, {"ml", "Malayalam"}, {"kn", "Kannada"},
        {"mr", "Marathi"}, {"ca", "Catalan"}, {"eu", "Basque"}, {"gl", "Galician"}, {"is", "Icelandic"},
        {"et", "Estonian"}, {"lv", "Latvian"}, {"lt", "Lithuanian"}, {"tl", "Filipino"},
        {"sq", "Albanian"}, {"mk", "Macedonian"}, {"bs", "Bosnian"}, {"ka", "Georgian"},
        {"hy", "Armenian"}, {"az", "Azerbaijani"}, {"kk", "Kazakh"}, {"uz", "Uzbek"}, {"mn", "Mongolian"},
        {"km", "Khmer"}, {"lo", "Lao"}, {"my", "Burmese"}, {"ne", "Nepali"}, {"si", "Sinhala"},
        {"sw", "Swahili"}, {"af", "Afrikaans"}, {"am", "Amharic"}, {"cy", "Welsh"}, {"ga", "Irish"},
        {"la", "Latin"}, {"und", ""}, {"unknown", ""}, {"mul", "Multiple"}, {"zxx", ""},
    };
    const std::string key = nuvio_language_key(code);
    for (const auto &e : n)
        if (key == e.k)
            return e.name;
    /* "en-gb" and similar: the base language. */
    const size_t dash = key.find('-');
    if (dash != std::string::npos) {
        const std::string base = key.substr(0, dash);
        for (const auto &e : n)
            if (base == e.k)
                return e.name;
    }
    if (code.empty())
        return "";
    std::string out = code;
    if (out.size() <= 3)
        for (auto &ch : out)
            ch = (char)std::toupper((unsigned char)ch);
    return out;
}

/* ---- animation --------------------------------------------------------------- */

bool NuvioOsd::Anim::step(float dt)
{
    if (v == target)
        return false;
    const float d = dur > 0.001f ? dt / dur : 1.0f;
    if (v < target) v = std::min(target, v + d);
    else v = std::max(target, v - d);
    return true;
}

float NuvioOsd::Anim::eased() const
{
    return ease_out(v);
}

/* ---- lifecycle ---------------------------------------------------------------- */

void NuvioOsd::begin(const NuvioRequest *req, double now)
{
    *this = NuvioOsd();
    m_req = req;
    m_last_now = now;
    m_load_since = now;
    a_loading.v = a_loading.target = 1.0f;
    m_dirty = true;
    /* Artwork: drawn at exactly these sizes, so fetched at them. */
    if (!req->backdrop.empty())
        m_img_backdrop = ui_image_request(req->backdrop.c_str(), 1920, 1080, 0);
    if (!req->logo.empty()) {
        m_img_logo = ui_image_request(req->logo.c_str(), 640, 230, 0);
        m_img_logo_small = ui_image_request(req->logo.c_str(), 520, 120, 0);
    }
    if (req->has_next && !req->next.thumbnail.empty())
        m_img_next = ui_image_request(req->next.thumbnail.c_str(), 180, 102, 0);
}

void NuvioOsd::end()
{
    m_req = nullptr;
}

float NuvioOsd::subtitle_lift() const
{
    /* The bottom controls (title, progress, buttons) are ~330 px tall. */
    return a_controls.eased() * 300.0f + a_seek.eased() * 70.0f * (1.0f - a_controls.eased());
}

void NuvioOsd::toast(const std::string &text, double now)
{
    m_toast = text;
    m_toast_until = now + std::min(6.0, 2.0 + text.size() / 25.0);   /* time to read it */
    a_toast.to(1.0f, 0.15f);
    m_dirty = true;
}

/* ---- controls ----------------------------------------------------------------- */

std::vector<NuvioOsd::Button> NuvioOsd::buttons(const NuvioStatus &) const
{
    std::vector<Button> b{BTN_PLAY};
    if (m_req && m_req->has_next && m_req->next.released)
        b.push_back(BTN_NEXT);
    b.push_back(BTN_SUBS);
    b.push_back(BTN_AUDIO);
    if (m_req && !m_req->sources.empty())
        b.push_back(BTN_SOURCES);
    if (m_req && !m_req->episodes.empty())
        b.push_back(BTN_EPISODES);
    if (m_more) {
        b.push_back(BTN_ASPECT);
        b.push_back(BTN_LESS);
    } else {
        b.push_back(BTN_MORE);
    }
    return b;
}

void NuvioOsd::touch(double now)
{
    m_hide_at = now + kAutoHide;
    if (m_paused_since >= 0)
        m_paused_since = now;
    m_dirty = true;
}

void NuvioOsd::show_controls(double now, Zone zone)
{
    if (!m_controls) {
        m_zone = zone;
        if (zone == ZONE_BUTTONS)
            m_focus = 0;
    }
    m_controls = true;
    a_controls.to(1.0f, 0.2f);
    if (m_pause_overlay) {
        m_pause_overlay = false;
        a_pause.to(0.0f, 0.18f);
    }
    touch(now);
}

void NuvioOsd::hide_controls()
{
    m_controls = false;
    m_more = false;
    a_controls.to(0.0f, 0.2f);
    m_dirty = true;
}

void NuvioOsd::begin_seek(int dir, bool repeat, const NuvioStatus &st)
{
    if (st.duration <= 0)
        return;
    if (dir != m_seek_dir || !repeat)
        m_seek_repeat = 0;
    m_seek_dir = dir;
    const double base = m_seeking ? m_seek_target : st.position;
    double t = base + dir * scrub_step(m_seek_repeat++);
    t = std::max(0.0, std::min(t, std::max(0.0, st.duration - 1.0)));
    m_seek_target = t;
    m_seeking = true;
    m_seek_commit_at = st.now + kSeekCommit;
    if (!m_controls)
        a_seek.to(1.0f, 0.15f);
    m_dirty = true;
}

void NuvioOsd::commit_seek(std::vector<OsdCommand> &out)
{
    if (!m_seeking)
        return;
    out.push_back({OsdCmd::SeekTo, m_seek_target});
    m_seeking = false;
    m_seek_shown_until = m_last_now + 1.2;   /* the bar stays while the seek lands */
    m_dirty = true;
}

int NuvioOsd::current_skip(const NuvioStatus &st) const
{
    if (!m_req || !m_req->prefs.skip_intro)
        return -1;
    for (size_t i = 0; i < m_req->skips.size() && i < 16; i++) {
        const NuvioSkip &k = m_req->skips[i];
        if (k.type == "outro" || k.type == "credits" || k.type == "ed")
            continue;              /* the next-episode card covers the end */
        if (st.position >= k.start && st.position < k.end - 1.0 && !m_skip_done[i])
            return (int)i;
    }
    return -1;
}

bool NuvioOsd::card_visible(const NuvioStatus &st) const
{
    if (!m_req || !m_req->has_next || !st.started || st.duration <= 60 || m_card_dismissed)
        return false;
    if (m_post_play)
        return true;
    const NuvioPrefs &p = m_req->prefs;
    double outro_start = -1, outro_end = -1;
    for (const NuvioSkip &k : m_req->skips)
        if (k.type == "outro" || k.type == "credits" || k.type == "ed") {
            outro_start = outro_start < 0 ? k.start : std::min(outro_start, k.start);
            outro_end = std::max(outro_end, k.end);
        }
    const double user_s = p.next_by_minutes ? p.next_minutes * 60.0
                                            : (100.0 - p.next_percent) / 100.0 * st.duration;
    auto by_threshold = [&]() {
        return p.next_by_minutes ? st.duration - st.position <= p.next_minutes * 60.0
                                 : st.position / st.duration >= p.next_percent / 100.0;
    };
    if (outro_start >= 0)
        return st.duration - outro_end > user_s ? by_threshold() : st.position >= outro_start;
    return by_threshold();
}

void NuvioOsd::playback_ended(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    if (m_req && m_req->has_next && m_req->next.released && !m_card_dismissed) {
        const NuvioPrefs &p = m_req->prefs;
        const bool still_watching = p.still_watching_episodes > 0 &&
                                    m_req->autoplay_count + 1 >= p.still_watching_episodes;
        if (p.autoplay_next && !still_watching) {
            out.push_back({OsdCmd::PlayNext});
            return;
        }
        /* Wait for the viewer: the card stays, focused. */
        m_post_play = true;
        m_post_play_until = st.now + 30.0;
        m_zone = ZONE_NEXT;
        a_card.to(1.0f, 0.18f);
        hide_controls();
        m_dirty = true;
        return;
    }
    out.push_back({OsdCmd::Stop});
}

void NuvioOsd::open_dialog(Dialog d, const NuvioStatus &st)
{
    m_dialog = d;
    m_dlg_col = 0;
    std::memset(m_dlg_row, 0, sizeof m_dlg_row);
    std::memset(m_dlg_scroll, 0, sizeof m_dlg_scroll);
    m_season_focus = false;
    if (d == DLG_SUBTITLES) {
        build_sub_langs();
        /* Start on the language of the track in use (or Off). */
        const int sel = nuvio_subs_selected();
        for (size_t i = 0; i < m_sub_langs.size(); i++)
            for (size_t k = 0; k < m_sub_langs[i].tracks.size(); k++)
                if (m_sub_langs[i].tracks[k] == sel) {
                    m_dlg_row[0] = (int)i;
                    m_dlg_row[1] = (int)k;
                }
        if (sel >= 0)
            m_dlg_col = 1;
    } else if (d == DLG_AUDIO) {
        m_dlg_row[0] = std::max(0, st.audio_active);
    } else if (d == DLG_SOURCES) {
        m_dlg_row[0] = std::max(0, m_req->source_index);
    } else if (d == DLG_EPISODES) {
        const std::vector<int> ss = seasons();
        m_season_tab = 0;
        for (size_t i = 0; i < ss.size(); i++)
            if (ss[i] == m_req->season)
                m_season_tab = (int)i;
        std::vector<int> eps;
        episodes_in_tab(&eps);
        for (size_t i = 0; i < eps.size(); i++)
            if (m_req->episodes[eps[i]].episode == m_req->episode &&
                m_req->episodes[eps[i]].season == m_req->season)
                m_dlg_row[0] = (int)i;
    }
    a_dialog.to(1.0f, 0.2f);
    /* The panel replaces the controls, as in Nuvio; they return on close. */
    m_controls_before_dialog = m_controls;
    if (m_controls) {
        m_controls = false;
        a_controls.to(0.0f, 0.15f);
    }
    m_dirty = true;
}

void NuvioOsd::close_dialog()
{
    m_dialog = DLG_NONE;
    a_dialog.to(0.0f, 0.18f);
    if (m_controls_before_dialog) {
        m_controls = true;
        a_controls.to(1.0f, 0.2f);
        touch(m_last_now);
    }
    m_dirty = true;
}

void NuvioOsd::build_sub_langs()
{
    /* "Off", then one row per language, built-in tracks before addon ones. */
    m_sub_langs.clear();
    m_sub_langs.push_back({"off", m_req ? m_req->str("off", "Off") : "Off", {}});
    const int n = nuvio_subs_count();
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(i, &t) != 0 || t.external != pass)
                continue;
            std::string key = nuvio_language_key(t.lang);
            if (key.empty() || key == "und")
                key = "und";
            auto it = std::find_if(m_sub_langs.begin(), m_sub_langs.end(),
                                   [&](const SubLang &l) { return l.key == key; });
            if (it == m_sub_langs.end()) {
                std::string name = nuvio_language_name(t.lang);
                if (name.empty())
                    name = m_req ? m_req->str("unknown_language", "Unknown") : "Unknown";
                m_sub_langs.push_back({key, name, {}});
                it = m_sub_langs.end() - 1;
            }
            it->tracks.push_back(i);
        }
}

std::vector<int> NuvioOsd::seasons() const
{
    std::vector<int> s;
    if (!m_req)
        return s;
    for (const NuvioEpisode &e : m_req->episodes)
        if (std::find(s.begin(), s.end(), e.season) == s.end())
            s.push_back(e.season);
    std::sort(s.begin(), s.end(), [](int a, int b) {
        /* Specials (season 0) last, like Nuvio's season tabs. */
        if ((a == 0) != (b == 0)) return b == 0;
        return a < b;
    });
    return s;
}

int NuvioOsd::episodes_in_tab(std::vector<int> *out) const
{
    out->clear();
    const std::vector<int> ss = seasons();
    if (ss.empty())
        return 0;
    const int season = ss[std::min<size_t>(m_season_tab, ss.size() - 1)];
    for (size_t i = 0; i < m_req->episodes.size(); i++)
        if (m_req->episodes[i].season == season)
            out->push_back((int)i);
    return (int)out->size();
}

int NuvioOsd::style_rows() const
{
    return 6;   /* delay, size, position, background, outline, bold */
}

void NuvioOsd::style_adjust(int row, int dir, std::vector<OsdCommand> &out)
{
    nuvio_sub_style s;
    nuvio_subs_get_style(&s);
    switch (row) {
    case 0: {
        int d = nuvio_subs_delay_ms() + dir * 100;
        d = std::max(-30000, std::min(30000, d));
        out.push_back({OsdCmd::SubtitleDelay, (double)d});
        return;
    }
    case 1:
        s.size_pct = std::max(50, std::min(250, s.size_pct + dir * 10));
        break;
    case 2:
        s.offset_pct = std::max(0.0f, std::min(40.0f, s.offset_pct + dir * 2.0f));
        break;
    case 3: {
        static const float steps[] = {0.0f, 0.4f, 0.6f, 0.8f, 1.0f};
        int i = 0;
        while (i < 4 && steps[i] + 0.05f < s.background)
            i++;
        i = std::max(0, std::min(4, i + dir));
        s.background = steps[i];
        break;
    }
    case 4:
        s.outline = !s.outline;
        break;
    case 5:
        s.bold = !s.bold;
        break;
    default:
        return;
    }
    nuvio_subs_set_style(&s);
    out.push_back({OsdCmd::SubtitleStyle});
}

/* ---- input ------------------------------------------------------------------------ */

void NuvioOsd::activate(Button b, const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    switch (b) {
    case BTN_PLAY:
        out.push_back({OsdCmd::TogglePause});
        break;
    case BTN_NEXT:
        out.push_back({OsdCmd::PlayNext});
        break;
    case BTN_SUBS:
        open_dialog(DLG_SUBTITLES, st);
        break;
    case BTN_AUDIO:
        open_dialog(DLG_AUDIO, st);
        break;
    case BTN_SOURCES:
        open_dialog(DLG_SOURCES, st);
        break;
    case BTN_EPISODES:
        open_dialog(DLG_EPISODES, st);
        break;
    case BTN_MORE:
        m_more = true;
        m_focus = (int)buttons(st).size() - 2;   /* onto the first extra action */
        break;
    case BTN_LESS:
        m_more = false;
        m_focus = (int)buttons(st).size() - 1;
        break;
    case BTN_ASPECT: {
        const int next = (st.view_mode + 1) % 3;
        out.push_back({OsdCmd::SetViewMode, 0, next});
        static const char *const keys[] = {"aspect_fit", "aspect_fill", "aspect_stretch"};
        static const char *const names[] = {"Fit", "Fill", "Stretch"};
        toast(m_req->str(keys[next], names[next]), st.now);
        break;
    }
    }
    m_dirty = true;
}

void NuvioOsd::input(const nuvio_input_state &in, const NuvioStatus &st,
                     std::vector<OsdCommand> &out)
{
    const uint32_t p = in.pressed;
    if (!p || !m_req)
        return;
    const double now = st.now;
    m_dirty = true;
    if (m_paused_since >= 0)
        m_paused_since = now;

    /* A failed stream: Go Back, or try another source. */
    if (!st.error.empty()) {
        const bool can_switch = m_req->sources.size() > 1;
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT))
            m_focus = can_switch ? (m_focus ^ 1) : 0;
        if (p & NUVIO_BTN_CIRCLE)
            out.push_back({OsdCmd::Stop});
        else if (p & NUVIO_BTN_CROSS) {
            if (m_focus == 1 && can_switch) {
                open_dialog(DLG_SOURCES, st);
                a_error.to(0.0f, 0.15f);
            } else {
                out.push_back({OsdCmd::Stop});
            }
        }
        if (m_dialog == DLG_NONE)
            return;
    }

    if (m_dialog != DLG_NONE) {
        dialog_input(in, st, out);
        return;
    }

    /* Still opening: only leaving makes sense. */
    if (!st.started) {
        if (p & NUVIO_BTN_CIRCLE)
            out.push_back({OsdCmd::Stop});
        return;
    }

    /* The episode is over and the next one waits for a press. */
    if (m_post_play) {
        if (p & NUVIO_BTN_CROSS)
            out.push_back({OsdCmd::PlayNext});
        else if (p & NUVIO_BTN_CIRCLE)
            out.push_back({OsdCmd::Stop});
        return;
    }

    if (m_pause_overlay) {
        m_pause_overlay = false;
        a_pause.to(0.0f, 0.18f);
        if (p & NUVIO_BTN_CROSS) {
            out.push_back({OsdCmd::TogglePause});
            return;
        }
        if (p & NUVIO_BTN_CIRCLE)
            return;
        show_controls(now, ZONE_BUTTONS);
        return;
    }

    if (p & NUVIO_BTN_CIRCLE) {
        if (m_seeking) {
            m_seeking = false;
            a_seek.to(0.0f, 0.15f);
        } else if (m_controls) {
            hide_controls();
        } else {
            out.push_back({OsdCmd::Stop});
        }
        return;
    }
    if (p & (NUVIO_BTN_OPTIONS | NUVIO_BTN_TOUCHPAD)) {
        if (m_controls) hide_controls();
        else show_controls(now, ZONE_BUTTONS);
        return;
    }
    if (p & NUVIO_BTN_TRIANGLE) {
        open_dialog(DLG_SUBTITLES, st);
        return;
    }
    if (p & NUVIO_BTN_SQUARE) {
        open_dialog(DLG_AUDIO, st);
        return;
    }
    if (p & (NUVIO_BTN_L1 | NUVIO_BTN_R1 | NUVIO_BTN_L2 | NUVIO_BTN_R2)) {
        const double d = (p & NUVIO_BTN_L1) ? -10 : (p & NUVIO_BTN_R1) ? 10 : (p & NUVIO_BTN_L2) ? -60 : 60;
        double t = (m_seeking ? m_seek_target : st.position) + d;
        t = std::max(0.0, std::min(t, std::max(0.0, st.duration - 1.0)));
        m_seek_target = t;
        m_seeking = true;
        m_seek_commit_at = now + 0.6;
        if (!m_controls)
            a_seek.to(1.0f, 0.15f);
        return;
    }

    const int skip = current_skip(st);
    const bool skip_up = skip >= 0 && (m_controls || now - m_skip_shown_at < kSkipShowFor);
    const bool card = card_visible(st);

    if (!m_controls) {
        if ((p & NUVIO_BTN_CROSS) && skip_up) {
            m_skip_done[skip] = true;
            out.push_back({OsdCmd::SeekTo, m_req->skips[skip].end});
            return;
        }
        if ((p & NUVIO_BTN_CROSS) && card) {
            out.push_back({OsdCmd::PlayNext});
            return;
        }
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
            begin_seek((p & NUVIO_BTN_LEFT) ? -1 : 1, (in.repeats & NUVIO_BTN_DPAD) != 0, st);
            return;
        }
        if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
            show_controls(now, ZONE_BUTTONS);
            return;
        }
        if (p & NUVIO_BTN_CROSS) {
            if (m_seeking) {
                commit_seek(out);
                a_seek.to(0.0f, 0.15f);
            }
            out.push_back({OsdCmd::TogglePause});
            show_controls(now, ZONE_BUTTONS);
        }
        return;
    }

    touch(now);
    const std::vector<Button> bs = buttons(st);
    if (m_focus >= (int)bs.size())
        m_focus = (int)bs.size() - 1;

    if (m_zone == ZONE_SKIP) {
        if ((p & NUVIO_BTN_CROSS) && skip >= 0) {
            m_skip_done[skip] = true;
            out.push_back({OsdCmd::SeekTo, m_req->skips[skip].end});
            m_zone = ZONE_PROGRESS;
        } else if (p & NUVIO_BTN_DOWN) {
            m_zone = ZONE_PROGRESS;
        } else if ((p & NUVIO_BTN_RIGHT) && card) {
            m_zone = ZONE_NEXT;
        }
        if (skip < 0 && m_zone == ZONE_SKIP)
            m_zone = ZONE_PROGRESS;
        return;
    }
    if (m_zone == ZONE_NEXT) {
        if ((p & NUVIO_BTN_CROSS) && card)
            out.push_back({OsdCmd::PlayNext});
        else if (p & NUVIO_BTN_DOWN)
            m_zone = ZONE_PROGRESS;
        else if ((p & NUVIO_BTN_LEFT) && skip >= 0)
            m_zone = ZONE_SKIP;
        if (!card && m_zone == ZONE_NEXT)
            m_zone = ZONE_PROGRESS;
        return;
    }
    if (m_zone == ZONE_PROGRESS) {
        if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
            begin_seek((p & NUVIO_BTN_LEFT) ? -1 : 1, (in.repeats & NUVIO_BTN_DPAD) != 0, st);
        } else if (p & NUVIO_BTN_UP) {
            if (skip >= 0) m_zone = ZONE_SKIP;
            else if (card) m_zone = ZONE_NEXT;
            else hide_controls();
        } else if (p & NUVIO_BTN_DOWN) {
            m_zone = ZONE_BUTTONS;
        } else if (p & NUVIO_BTN_CROSS) {
            if (m_seeking)
                commit_seek(out);
            out.push_back({OsdCmd::TogglePause});
        }
        return;
    }
    /* ZONE_BUTTONS */
    if (p & NUVIO_BTN_LEFT)
        m_focus = std::max(0, m_focus - 1);
    else if (p & NUVIO_BTN_RIGHT)
        m_focus = std::min((int)bs.size() - 1, m_focus + 1);
    else if (p & NUVIO_BTN_UP)
        m_zone = ZONE_PROGRESS;
    else if (p & NUVIO_BTN_DOWN)
        hide_controls();
    else if (p & NUVIO_BTN_CROSS)
        activate(bs[m_focus], st, out);
}

void NuvioOsd::subtitles_input(uint32_t p, std::vector<OsdCommand> &out)
{
    /* Three columns: languages | that language's tracks | style. Style rows
     * 0-3 are steppers with a focusable minus (sub 0) and plus (sub 1), as in
     * Nuvio's subtitle style rail; rows 4-5 are toggles. */
    const int col = m_dlg_col;
    const int nlang = (int)m_sub_langs.size();
    const SubLang *lang = (m_dlg_row[0] >= 0 && m_dlg_row[0] < nlang) ? &m_sub_langs[m_dlg_row[0]] : nullptr;
    const int ntracks = lang ? (int)lang->tracks.size() : 0;
    const bool stepper = col == 2 && m_dlg_row[2] <= 3;

    if (p & NUVIO_BTN_CIRCLE) {
        close_dialog();
        return;
    }
    if (p & NUVIO_BTN_LEFT) {
        if (stepper && m_dlg_sub == 1)
            m_dlg_sub = 0;
        else if (col == 2)
            m_dlg_col = ntracks > 0 ? 1 : 0;
        else if (col == 1)
            m_dlg_col = 0;
        return;
    }
    if (p & NUVIO_BTN_RIGHT) {
        if (stepper && m_dlg_sub == 0)
            m_dlg_sub = 1;
        else if (col == 0)
            m_dlg_col = ntracks > 0 ? 1 : 2;
        else if (col == 1) {
            m_dlg_col = 2;
            m_dlg_sub = 0;
        }
        return;
    }
    if (p & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) {
        const int d = (p & NUVIO_BTN_UP) ? -1 : 1;
        const int n = col == 0 ? nlang : col == 1 ? std::max(1, ntracks) : style_rows();
        m_dlg_row[col] = std::max(0, std::min(n - 1, m_dlg_row[col] + d));
        if (col == 0)
            m_dlg_row[1] = 0;
        return;
    }
    if (p & NUVIO_BTN_CROSS) {
        if (col == 0) {
            if (m_dlg_row[0] == 0)
                out.push_back({OsdCmd::SelectSubtitle, 0, -1});
            else if (ntracks == 1)
                out.push_back({OsdCmd::SelectSubtitle, 0, lang->tracks[0]});
            else if (ntracks > 1)
                m_dlg_col = 1;
        } else if (col == 1 && ntracks > 0) {
            out.push_back({OsdCmd::SelectSubtitle, 0, lang->tracks[std::min(m_dlg_row[1], ntracks - 1)]});
        } else if (col == 2) {
            style_adjust(m_dlg_row[2], stepper ? (m_dlg_sub ? 1 : -1) : 0, out);
        }
    }
}

void NuvioOsd::dialog_input(const nuvio_input_state &in, const NuvioStatus &st,
                            std::vector<OsdCommand> &out)
{
    const uint32_t p = in.pressed;
    if (m_dialog == DLG_SUBTITLES) {
        subtitles_input(p, out);
        return;
    }
    if (p & NUVIO_BTN_CIRCLE) {
        close_dialog();
        return;
    }
    int n = 0;
    if (m_dialog == DLG_AUDIO)
        n = (int)st.audio.size();
    else if (m_dialog == DLG_SOURCES)
        n = (int)m_req->sources.size();
    else if (m_dialog == DLG_EPISODES) {
        std::vector<int> eps;
        n = episodes_in_tab(&eps);
        const int ns = (int)seasons().size();
        if (m_season_focus) {
            if (p & NUVIO_BTN_LEFT) m_season_tab = std::max(0, m_season_tab - 1);
            if (p & NUVIO_BTN_RIGHT) m_season_tab = std::min(ns - 1, m_season_tab + 1);
            if (p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) {
                m_dlg_row[0] = 0;
                m_dlg_scroll[0] = 0;
            }
            if (p & (NUVIO_BTN_DOWN | NUVIO_BTN_CROSS))
                m_season_focus = false;
            return;
        }
        if ((p & NUVIO_BTN_UP) && m_dlg_row[0] == 0 && ns > 1) {
            m_season_focus = true;
            return;
        }
        if ((p & NUVIO_BTN_CROSS) && n > 0) {
            const NuvioEpisode &e = m_req->episodes[eps[std::min(m_dlg_row[0], n - 1)]];
            if (e.season == m_req->season && e.episode == m_req->episode) {
                close_dialog();
            } else if (e.released) {
                out.push_back({OsdCmd::PlayEpisode, 0, -1, e.season, e.episode});
                close_dialog();
            }
            return;
        }
    }
    if (p & NUVIO_BTN_UP)
        m_dlg_row[0] = std::max(0, m_dlg_row[0] - 1);
    if (p & NUVIO_BTN_DOWN)
        m_dlg_row[0] = std::min(std::max(0, n - 1), m_dlg_row[0] + 1);
    if ((p & NUVIO_BTN_CROSS) && n > 0) {
        if (m_dialog == DLG_AUDIO) {
            if (m_dlg_row[0] != st.audio_active)
                out.push_back({OsdCmd::SelectAudio, 0, m_dlg_row[0]});
            close_dialog();
        } else if (m_dialog == DLG_SOURCES) {
            if (m_dlg_row[0] != m_req->source_index || !st.error.empty())
                out.push_back({OsdCmd::SwitchSource, 0, m_dlg_row[0]});
            close_dialog();
        }
    }
}

/* ---- timers ------------------------------------------------------------------------- */

void NuvioOsd::tick(const NuvioStatus &st, std::vector<OsdCommand> &out)
{
    const double now = st.now;
    const float dt = (float)std::min(0.1, std::max(0.0, now - m_last_now));
    m_last_now = now;
    if (!m_req)
        return;

    /* Loading screen until the first frame; the error screen when it fails. */
    a_loading.to(st.started || !st.error.empty() ? 0.0f : 1.0f, 0.25f);
    a_error.to(!st.error.empty() && m_dialog == DLG_NONE ? 1.0f : 0.0f, 0.18f);
    if (st.error != m_was_error) {
        m_was_error = st.error;
        m_focus = 0;
        m_dirty = true;
    }

    /* The controls come up for a moment when the picture first appears. */
    if (st.started && !m_was_started) {
        m_was_started = true;
        show_controls(now, ZONE_BUTTONS);
    }

    if (m_controls && !st.paused && m_dialog == DLG_NONE && !m_seeking && now >= m_hide_at)
        hide_controls();

    if (m_seeking && now >= m_seek_commit_at) {
        commit_seek(out);
        if (!m_controls)
            a_seek.to(1.0f, 0.15f);
    }
    if (!m_seeking && a_seek.target > 0 && now >= m_seek_shown_until)
        a_seek.to(0.0f, 0.2f);

    /* Pause overlay after five quiet seconds paused. */
    if (st.paused && st.started) {
        if (m_paused_since < 0)
            m_paused_since = now;
        if (!m_pause_overlay && m_dialog == DLG_NONE && !m_seeking &&
            now - m_paused_since >= kPauseOverlayAfter) {
            m_pause_overlay = true;
            a_pause.to(1.0f, 0.18f);
            hide_controls();
        }
    } else {
        m_paused_since = -1;
        if (m_pause_overlay) {
            m_pause_overlay = false;
            a_pause.to(0.0f, 0.18f);
        }
    }

    /* Skip intro: shown for a while on entering an interval, and with the controls. */
    const int skip = current_skip(st);
    if (skip != m_skip_index) {
        m_skip_index = skip;
        m_skip_shown_at = now;
        m_dirty = true;
    }
    const bool skip_visible = skip >= 0 && st.started && st.error.empty() &&
                              (m_controls || now - m_skip_shown_at < kSkipShowFor) && !m_pause_overlay;
    a_skip.to(skip_visible ? 1.0f : 0.0f, 0.18f);

    const bool card = card_visible(st) && st.error.empty() && !m_pause_overlay;
    a_card.to(card ? 1.0f : 0.0f, 0.18f);
    if (m_post_play && now >= m_post_play_until)
        out.push_back({OsdCmd::Stop});

    a_spinner.to(st.started && (st.buffering || st.switching) && st.error.empty() ? 1.0f : 0.0f, 0.18f);
    if (a_spinner.v > 0) {
        m_spin += dt * 4.4f;
        m_dirty = true;
    }

    if (m_toast_until > 0 && now >= m_toast_until) {
        m_toast_until = 0;
        a_toast.to(0.0f, 0.2f);
    }

    Anim *anims[] = {&a_controls, &a_loading, &a_seek, &a_pause, &a_dialog, &a_spinner,
                     &a_card, &a_skip, &a_toast, &a_error};
    for (Anim *a : anims)
        if (a->step(dt))
            m_dirty = true;

    if (st.buffering != m_was_buffering || st.paused != m_was_paused) {
        m_was_buffering = st.buffering;
        m_was_paused = st.paused;
        m_dirty = true;
    }
    /* The loading screen pulses until the stream starts filling. */
    if (a_loading.v > 0)
        m_dirty = true;
}

std::string NuvioOsd::clock_text(double /*now*/, double add_seconds) const
{
    time_t t = time(nullptr) + (time_t)add_seconds;
    struct tm tm;
    if (m_req && m_req->prefs.has_tz) {
        t += (time_t)m_req->prefs.tz_offset_min * 60;
        gmtime_r(&t, &tm);
    } else {
        localtime_r(&t, &tm);
    }
    char b[16];
    if (!m_req || m_req->prefs.clock_24h)
        std::snprintf(b, sizeof b, "%02d:%02d", tm.tm_hour, tm.tm_min);
    else
        std::snprintf(b, sizeof b, "%d:%02d %s", (tm.tm_hour + 11) % 12 + 1, tm.tm_min,
                      tm.tm_hour < 12 ? "AM" : "PM");
    return b;
}
