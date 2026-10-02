/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Renders the Nuvio Player's interface to PNGs on the Mac, from the same code
 * the app runs: each state (loading, controls, seeking, pause overlay, the
 * panels, next episode, skip intro, error, subtitles) over a video frame.
 *
 *   tools/preview/build.sh && tools/preview/osd_preview <frame.png> <media-dir>
 */
#include "nuvio_osd.h"
#include "nuvio_session.h"
#include "nuvio_subs.h"
#include "ui_canvas.h"
#include "ui_image.h"
#include "ui_text.h"

#include <png.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static ui_canvas g_osd, g_sub;
static ui_image g_frame;
static NuvioOsd g_ui;
static NuvioStatus g_st;
static NuvioRequest g_req;
static std::string g_out = "out";

static void apply(const std::vector<OsdCommand> &cmds)
{
    for (const OsdCommand &c : cmds) {
        switch (c.cmd) {
        case OsdCmd::TogglePause: g_st.paused = !g_st.paused; break;
        case OsdCmd::SeekTo: g_st.position = c.value; break;
        case OsdCmd::SelectSubtitle: nuvio_subs_select(c.index); break;
        case OsdCmd::SubtitleDelay: nuvio_subs_set_delay_ms((int)c.value); break;
        case OsdCmd::SelectAudio: g_st.audio_active = c.index; break;
        case OsdCmd::SetViewMode: g_st.view_mode = c.index; break;
        default: std::printf("  command %d\n", (int)c.cmd); break;
        }
    }
}

static void advance(double seconds)
{
    for (double t = 0; t < seconds; t += 1.0 / 60.0) {
        g_st.now += 1.0 / 60.0;
        if (!g_st.paused && g_st.started)
            g_st.position += 1.0 / 60.0;
        std::vector<OsdCommand> cmds;
        g_ui.tick(g_st, cmds);
        apply(cmds);
        g_ui.render(g_osd, g_st);
    }
}

static void press(uint32_t button, bool repeat = false)
{
    nuvio_input_state in;
    std::memset(&in, 0, sizeof in);
    in.pressed = button;
    if (repeat)
        in.repeats = button & NUVIO_BTN_DPAD;
    std::vector<OsdCommand> cmds;
    g_ui.input(in, g_st, cmds);
    apply(cmds);
    advance(0.3);
}

/* Controls up with the focus on button i, then Cross. */
static void open_button(int i)
{
    for (int k = 0; k < 3; k++) {
        nuvio_input_state in;
        std::memset(&in, 0, sizeof in);
        in.pressed = NUVIO_BTN_CIRCLE;   /* close / hide (a Stop is harmless here) */
        std::vector<OsdCommand> cmds;
        g_ui.input(in, g_st, cmds);
        advance(0.3);
    }
    press(NUVIO_BTN_UP);                 /* controls, focus on Play */
    for (int k = 0; k < i; k++)
        press(NUVIO_BTN_RIGHT);
    press(NUVIO_BTN_CROSS);
    advance(0.3);
}

static void shot(const char *name, bool subs = false, int64_t sub_pts = 0)
{
    ui_canvas out;
    ui_canvas_init(&out, 1920, 1080);
    if (g_st.started)
        ui_draw_image_cover(&out, &g_frame, 0, 0, 1920, 1080, 1.0f);
    else
        ui_fill_rect(&out, 0, 0, 1920, 1080, 0xff000000u);
    if (subs) {
        nuvio_rect v = {0, 0, 1920, 1080};
        for (int i = 0; i < 3; i++)
            nuvio_subs_render(&g_sub, sub_pts, v, g_ui.subtitle_lift());
        ui_image si = {g_sub.px, g_sub.w, g_sub.h};
        ui_draw_image(&out, &si, 0, 0, 1920, 1080, 1.0f);
    }
    g_ui.render(g_osd, g_st);
    ui_image oi = {g_osd.px, g_osd.w, g_osd.h};
    ui_draw_image(&out, &oi, 0, 0, 1920, 1080, 1.0f);
    png_image pi;
    std::memset(&pi, 0, sizeof pi);
    pi.version = PNG_IMAGE_VERSION;
    pi.width = 1920;
    pi.height = 1080;
    pi.format = PNG_FORMAT_RGBA;
    const std::string path = g_out + "/" + name + ".png";
    png_image_write_to_file(&pi, path.c_str(), 0, out.px, 1920 * 4, nullptr);
    std::printf("wrote %s\n", path.c_str());
    ui_canvas_free(&out);
}

static void wait_subs()
{
    for (int i = 0; i < 100; i++) {
        bool loading = false;
        for (int k = 0; k < nuvio_subs_count(); k++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(k, &t) == 0 && t.state == 0)
                loading = true;
        }
        if (!loading)
            return;
        usleep(50000);
    }
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <frame.png> <media-dir> [out-dir]\n", argv[0]);
        return 2;
    }
    const std::string media = argv[2];
    if (argc > 3)
        g_out = argv[3];
    ui_text_init();
    nuvio_subs_init();
    ui_canvas_init(&g_osd, 1920, 1080);
    ui_canvas_init(&g_sub, 1920, 1080);
    {
        FILE *f = std::fopen(argv[1], "rb");
        std::vector<uint8_t> d;
        if (f) {
            std::fseek(f, 0, SEEK_END);
            d.resize(std::ftell(f));
            std::fseek(f, 0, SEEK_SET);
            std::fread(d.data(), 1, d.size(), f);
            std::fclose(f);
        }
        if (ui_image_decode(d.data(), d.size(), 1920, 1080, &g_frame) != 0) {
            std::fprintf(stderr, "frame decode failed\n");
            return 1;
        }
    }

    const std::string frame = argv[1];
    std::string json = R"({
      "id": "preview-1", "url": "http://example/stream.mkv",
      "title": "The Midnight Garden", "episodeTitle": "A Light in the Greenhouse",
      "season": 2, "episode": 5, "year": "2024", "itemType": "series",
      "description": "When a storm knocks out the power across the valley, Mara follows a flickering light into the old greenhouse and finds something that has been waiting there for decades. Meanwhile Theo tries to keep the festival from falling apart.",
      "logo": "LOGO", "background": "FRAME",
      "stream": {"title": "The.Midnight.Garden.S02E05.2160p.WEB-DL.DV.HDR.DDP5.1.Atmos-GRP", "addon": "Torrentio RD"},
      "sources": [
        {"title": "4K DV HDR10 · DDP 5.1 Atmos", "description": "💾 18.2 GB · 👤 412 · WEB-DL", "addon": "Torrentio RD", "url": "http://a"},
        {"title": "4K HDR10 · TrueHD 7.1", "description": "💾 41.7 GB · REMUX", "addon": "Comet", "url": "http://b"},
        {"title": "1080p · DDP 5.1", "description": "💾 3.1 GB · WEBRip", "addon": "MediaFusion", "url": "http://c"}
      ],
      "sourceIndex": 0,
      "episodes": [
        {"season": 2, "episode": 3, "title": "Roots", "thumbnail": "FRAME", "overview": "Mara digs up the past.", "watched": true},
        {"season": 2, "episode": 4, "title": "Under Glass", "thumbnail": "FRAME", "overview": "Theo makes a promise he can't keep.", "watched": true},
        {"season": 2, "episode": 5, "title": "A Light in the Greenhouse", "thumbnail": "FRAME", "overview": "A storm, a light, and a secret."},
        {"season": 2, "episode": 6, "title": "Bloom", "thumbnail": "FRAME", "overview": "The festival begins."},
        {"season": 1, "episode": 1, "title": "Pilot", "thumbnail": "FRAME", "overview": "Everything starts here."}
      ],
      "nextEpisode": {"season": 2, "episode": 6, "title": "Bloom", "thumbnail": "FRAME", "released": true},
      "skipIntervals": [{"type": "intro", "start": 60, "end": 150}],
      "prefs": {"subtitleLanguages": ["en"], "audioLanguages": ["en"], "clock24h": true, "autoplayNext": true}
    })";
    auto put = [&](const std::string &key, const std::string &value) {
        size_t at;
        while ((at = json.find("\"" + key + "\"")) != std::string::npos)
            json.replace(at, key.size() + 2, "\"" + value + "\"");
    };
    put("FRAME", frame);
    put("LOGO", std::string(argv[0]).substr(0, std::string(argv[0]).rfind('/')) + "/../../assets/app_logo_wordmark.png");
    if (!nuvio_request_parse(json.c_str(), g_req)) {
        std::fprintf(stderr, "request parse failed\n");
        return 1;
    }

    g_st.duration = 2843;
    g_st.position = 1225;
    g_st.buffered = 1290;
    g_st.quality_line = "4K  \xC2\xB7  Dolby Vision  \xC2\xB7  HEVC  \xC2\xB7  Dolby Digital+ Atmos 5.1";
    g_st.audio = {
        {1, "eng", "", "Dolby Digital+ Atmos", "5.1", true},
        {2, "eng", "Commentary", "AAC", "Stereo", false},
        {3, "spa", "", "Dolby Digital", "5.1", false},
        {4, "jpn", "", "AAC", "Stereo", false},
    };
    g_st.audio_active = 0;

    nuvio_subs_add_external((media + "/sub_en.srt").c_str(), "eng", "OpenSubtitles", "");
    nuvio_subs_add_external((media + "/sub_es.srt").c_str(), "spa", "OpenSubtitles", "");
    nuvio_subs_add_external((media + "/ext_fr.srt").c_str(), "fre", "SubDL", "");
    nuvio_subs_add_external((media + "/sub_en.ass").c_str(), "eng", "Styled (ASS)", "");
    wait_subs();

    g_ui.begin(&g_req, 0.0);
    g_st.open_stage = "Opening stream\xE2\x80\xA6";
    advance(1.2);
    shot("01_loading");
    g_st.open_progress = 0.55f;
    g_st.open_stage = "Buffering\xE2\x80\xA6";
    advance(0.5);
    shot("02_loading_fill");

    g_st.started = true;
    advance(0.6);
    shot("03_controls");
    press(NUVIO_BTN_UP);
    press(NUVIO_BTN_RIGHT);
    press(NUVIO_BTN_RIGHT, true);
    shot("04_progress_seek");
    advance(1.6);
    press(NUVIO_BTN_DOWN);
    press(NUVIO_BTN_RIGHT);
    press(NUVIO_BTN_RIGHT);
    shot("05_button_focus");
    press(NUVIO_BTN_CROSS);
    advance(0.3);
    shot("06_subtitles_dialog");
    press(NUVIO_BTN_DOWN);
    press(NUVIO_BTN_RIGHT);
    shot("07_subtitles_tracks");
    press(NUVIO_BTN_RIGHT);
    press(NUVIO_BTN_DOWN);
    shot("08_subtitles_style");
    press(NUVIO_BTN_CIRCLE);
    press(NUVIO_BTN_SQUARE);
    shot("09_audio_dialog");
    press(NUVIO_BTN_CIRCLE);
    open_button(4);
    shot("10_sources");
    press(NUVIO_BTN_DOWN);
    shot("10b_sources_focus2");
    press(NUVIO_BTN_CIRCLE);
    open_button(5);
    shot("11_episodes");
    press(NUVIO_BTN_UP);
    press(NUVIO_BTN_UP);
    press(NUVIO_BTN_UP);
    shot("11b_episodes_seasons");
    press(NUVIO_BTN_CIRCLE);
    open_button(6);
    shot("12_more_actions");
    press(NUVIO_BTN_CIRCLE);
    advance(0.6);
    press(NUVIO_BTN_RIGHT);
    press(NUVIO_BTN_RIGHT, true);
    shot("13_seekbar_hidden");
    advance(2.5);

    /* Subtitles on, playing. */
    nuvio_subs_select(0);
    shot("14_subtitles_srt", true, 4000000);
    nuvio_subs_select(3);
    shot("15_subtitles_ass", true, 4000000);
    press(NUVIO_BTN_UP);
    shot("16_subtitles_with_controls", true, 4000000);
    press(NUVIO_BTN_CIRCLE);
    advance(0.5);

    g_st.paused = true;
    advance(6.0);
    shot("17_pause_overlay");
    press(NUVIO_BTN_CROSS);
    advance(0.5);

    g_st.position = 75;
    advance(0.5);
    shot("18_skip_intro");
    g_st.position = g_st.duration * 0.995;
    advance(0.5);
    shot("19_next_episode");

    g_st.error = "This stream could not be opened. The link may have expired, or the source is unavailable.";
    advance(0.5);
    shot("20_error");
    return 0;
}
