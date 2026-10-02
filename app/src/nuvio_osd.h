#pragma once
/*
 * The Nuvio Player's interface, drawn natively over the video in Nuvio's own
 * design (NuvioTVSmart's Android TV player port, css/components-29..37): the
 * loading screen, the controls (title, progress, buttons, clock), seeking,
 * the pause overlay, the subtitle / audio / sources / episodes panels, the
 * next-episode card, skip intro, buffering and errors.
 *
 * The player feeds it a status every frame and the controller's input; it
 * answers with commands (pause, seek, pick a track, ...) and draws itself on
 * a 1920x1080 canvas that is composited over the picture.
 */
#include "nuvio_input.h"
#include "nuvio_session.h"
#include "ui_canvas.h"

#include <string>
#include <vector>

struct NuvioAudioTrack {
    int stream = -1;
    std::string lang;        /* "eng" */
    std::string title;       /* stream title, e.g. "Commentary" */
    std::string codec;       /* "TrueHD", "E-AC-3", ... */
    std::string channels;    /* "7.1", "5.1", "Stereo" */
    bool is_default = false;
};

struct NuvioStatus {
    double now = 0;               /* monotonic seconds */
    bool started = false;         /* the first frame is on screen */
    float open_progress = 0;      /* 0..1 before the first frame */
    const char *open_stage = "";  /* what the open is doing, for the loading screen */
    bool buffering = false;       /* stalled after start */
    bool paused = false;
    double position = 0, duration = 0, buffered = 0;
    std::vector<NuvioAudioTrack> audio;
    int audio_active = -1;        /* index into audio */
    int view_mode = 0;            /* 0 fit, 1 fill, 2 stretch */
    std::string quality_line;     /* "2160p · HDR10 · HEVC · TrueHD 7.1" */
    std::string error;            /* non-empty: the stream failed */
    bool switching = false;       /* reopening for an audio / source change */
};

enum class OsdCmd {
    TogglePause,
    SeekTo,          /* value = seconds */
    Stop,            /* back to Nuvio */
    SelectAudio,     /* index into status.audio */
    SelectSubtitle,  /* index = track id, -1 off */
    SubtitleDelay,   /* value = ms */
    SubtitleStyle,   /* style changed (read nuvio_subs_get_style) */
    SwitchSource,    /* index into request.sources */
    PlayEpisode,     /* season, episode */
    PlayNext,
    SetViewMode,     /* index = mode */
};

struct OsdCommand {
    OsdCmd cmd;
    double value = 0;
    int index = -1;
    int season = 0, episode = 0;
};

class NuvioOsd {
public:
    void begin(const NuvioRequest *req, double now);
    void end();

    /* One poll of the controller. */
    void input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out);
    /* Timers: auto-hide, seek commit, autoplay countdown. Every frame. */
    void tick(const NuvioStatus &st, std::vector<OsdCommand> &out);
    /* Redraws c if anything visible changed; true when it did. */
    bool render(ui_canvas &c, const NuvioStatus &st);

    /* Whether anything is on the canvas (else it need not be composited). */
    bool visible() const { return m_drew_something; }
    /* Pixels text subtitles rise while the bottom controls are up. */
    float subtitle_lift() const;
    /* A message at the top of the screen for a moment. */
    void toast(const std::string &text, double now);
    /* The playback ended by itself (EOF). */
    void playback_ended(const NuvioStatus &st, std::vector<OsdCommand> &out);
    /* The subtitles' preferred choice was made for the viewer. */
    void note_subtitle_choice() { m_dirty = true; }
    bool post_play_active() const { return m_post_play; }

private:
    enum Zone { ZONE_PROGRESS, ZONE_BUTTONS, ZONE_SKIP, ZONE_NEXT };
    enum Dialog { DLG_NONE, DLG_SUBTITLES, DLG_AUDIO, DLG_SOURCES, DLG_EPISODES };
    enum Button { BTN_PLAY, BTN_NEXT, BTN_SUBS, BTN_AUDIO, BTN_SOURCES, BTN_EPISODES,
                  BTN_MORE, BTN_ASPECT, BTN_LESS };

    struct Anim {
        float v = 0, target = 0, dur = 0.2f;
        void to(float t, float d) { target = t; dur = d; }
        bool step(float dt);
        float eased() const;
    };

    /* Subtitle dialog rows. */
    struct SubLang { std::string key, name; std::vector<int> tracks; };
    struct StyleRow { int kind; };

    const NuvioRequest *m_req = nullptr;
    double m_last_now = 0;
    double m_load_since = 0;      /* when the loading screen appeared */
    bool m_dirty = true;
    bool m_drew_something = false;

    /* controls */
    bool m_controls = false;
    Zone m_zone = ZONE_BUTTONS;
    int m_focus = 0;
    bool m_more = false;
    double m_hide_at = 0;
    Anim a_controls, a_loading, a_seek, a_pause, a_dialog, a_spinner, a_card, a_skip, a_toast,
         a_error;

    /* seeking */
    bool m_seeking = false;
    double m_seek_target = 0;
    int m_seek_dir = 0;
    int m_seek_repeat = 0;
    double m_seek_commit_at = 0;
    double m_seek_shown_until = 0;

    /* pause overlay */
    double m_paused_since = -1;
    bool m_pause_overlay = false;

    /* dialogs */
    Dialog m_dialog = DLG_NONE;
    bool m_controls_before_dialog = false;
    int m_dlg_col = 0;            /* subtitles: 0 languages, 1 tracks, 2 style */
    int m_dlg_row[3] = {0, 0, 0};
    int m_dlg_scroll[3] = {0, 0, 0};
    int m_dlg_sub = 0;            /* style steppers: 0 minus, 1 plus */
    int m_season_tab = 0;
    bool m_season_focus = false;
    std::vector<SubLang> m_sub_langs;

    /* next episode / skip */
    bool m_card_dismissed = false;
    bool m_post_play = false;
    double m_post_play_until = 0;
    int m_skip_index = -1;
    double m_skip_shown_at = 0;
    bool m_skip_done[16] = {};

    /* toast */
    std::string m_toast;
    double m_toast_until = 0;

    /* last drawn values, to know when to redraw */
    int m_drawn_second = -1;
    int m_drawn_minute = -1;
    unsigned m_drawn_images = 0;
    int m_drawn_sub_state = -1;
    bool m_was_buffering = false, m_was_paused = false, m_was_started = false;
    std::string m_was_error;
    float m_spin = 0;

    /* images */
    int m_img_backdrop = -1, m_img_logo = -1, m_img_logo_small = -1, m_img_next = -1;

    /* helpers */
    std::vector<Button> buttons(const NuvioStatus &st) const;
    void show_controls(double now, Zone zone);
    void hide_controls();
    void touch(double now);
    void begin_seek(int dir, bool repeat, const NuvioStatus &st);
    void commit_seek(std::vector<OsdCommand> &out);
    void open_dialog(Dialog d, const NuvioStatus &st);
    void close_dialog();
    void activate(Button b, const NuvioStatus &st, std::vector<OsdCommand> &out);
    void build_sub_langs();
    int current_skip(const NuvioStatus &st) const;
    bool card_visible(const NuvioStatus &st) const;
    void dialog_input(const nuvio_input_state &in, const NuvioStatus &st, std::vector<OsdCommand> &out);
    void subtitles_input(uint32_t p, std::vector<OsdCommand> &out);
    void style_adjust(int row, int dir, std::vector<OsdCommand> &out);
    int style_rows() const;
    int episodes_in_tab(std::vector<int> *out) const;
    std::vector<int> seasons() const;
    std::string clock_text(double now, double add_seconds = 0) const;

    /* drawing (nuvio_osd_draw.cpp) */
    void draw_loading(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_controls(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_seekbar(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_pause(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_spinner(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_card(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_skip(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_toast(ui_canvas &c, float a);
    void draw_error(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_dialog(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_subtitles_dialog(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_audio_dialog(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_sources_panel(ui_canvas &c, const NuvioStatus &st, float a);
    void draw_episodes_panel(ui_canvas &c, const NuvioStatus &st, float a);
};

/* "English" for "eng"/"en"/"en-US"; the code itself when unknown. */
std::string nuvio_language_name(const std::string &code);
/* ISO 639-1 form of a 2- or 3-letter code ("eng" -> "en"), lower case. */
std::string nuvio_language_key(const std::string &code);
/* "1:02:03" / "2:03". */
std::string nuvio_format_time(double seconds);
