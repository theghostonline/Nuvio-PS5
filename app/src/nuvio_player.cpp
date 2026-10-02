/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * The Nuvio Player's frame loop.
 *
 * One playback: parse the page's request, show Nuvio's loading screen while
 * the stream opens on a worker thread, then every frame
 *
 *   input -> interface commands -> status -> timers
 *   GPU clear, video quad, subtitles layer, interface layer, present
 *
 * presenting only when something on screen changed. The page gets progress
 * every five seconds and the final result (position, what to do next, the
 * tracks chosen) before it reopens.
 */
#include "nuvio_player.h"

#include "nuvio_bridge.h"
#include "nuvio_control.h"
#include "nuvio_input.h"
#include "nuvio_osd.h"
#include "nuvio_session.h"
#include "nuvio_subs.h"
#include "ui_canvas.h"
#include "ui_image.h"
#include "ui_text.h"

#include "evo/Application.hpp"
#include "evo/services/PlaybackController.hpp"

#include "evo_agc_runtime.h"
#include "evo_boot_log.h"
#include "evo_boot_trace.h"
#include "evo_playback.h"
#include "evo_vdec.h"
#include "pp_playback.h"

#include "cJSON.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/dovi_meta.h>

extern pp_playback g_pp_pb;
extern char nuvio_stream_headers[4096];
extern char nuvio_stream_user_agent[512];
/* Which EVO screen is up. The engine's decode, audio and subtitle threads
 * park unless it is the player (2): EVO's screen manager set it on entering
 * its player screen, and nothing else does. */
extern int screen;
extern AVFormatContext *play_fmt;
extern int video_stream_index;
extern int audio_stream_index;
extern volatile int pb_prebuffer_hold;
extern int pb_prebuffer_packets;
int evo_demux_seek_busy(void);
void evo_demux_rebuffer(int64_t target_us, int max_ms);
float evo_demux_prebuffer_progress(void);
double evo_demux_buffered_s(void);
extern volatile int evo_demux_state;
extern volatile unsigned long packet_queue_ring_fallbacks;
extern int dbg_video_packets;
extern int g_vdec_force_ffmpeg;
extern char nuvio_vdec_conf[512];
extern volatile unsigned nuvio_control_beats;
extern volatile int nuvio_control_stage;
extern int dbg_video_frames;
void evo_log_alloc_state(const char *when);
}

namespace evo {
extern int DisplayWidth;
extern int DisplayHeight;
}

namespace {

constexpr int kScreenNone = 0;
constexpr int kScreenPlayer = 2;
constexpr int CW = 1920, CH = 1080;     /* overlay canvases */

evo::PlaybackController *s_pb = nullptr;
int s_user_id = -1;
ui_canvas s_osd_canvas, s_sub_canvas;
NuvioOsd s_osd;

/* The viewer's language preferences for the audio pick (set per playback). */
std::vector<std::string> s_audio_langs;

double now_s()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- names for the quality line and track lists ----------------------------------- */

std::string video_codec_name(enum AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_HEVC: return "HEVC";
    case AV_CODEC_ID_H264: return "H.264";
    case AV_CODEC_ID_AV1: return "AV1";
    case AV_CODEC_ID_VP9: return "VP9";
    case AV_CODEC_ID_VP8: return "VP8";
    case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2";
    case AV_CODEC_ID_MPEG4: return "MPEG-4";
    case AV_CODEC_ID_VC1: return "VC-1";
    default: return avcodec_get_name(id);
    }
}

std::string audio_codec_name(const AVCodecParameters *par)
{
    switch (par->codec_id) {
    case AV_CODEC_ID_TRUEHD: return "TrueHD";
    case AV_CODEC_ID_MLP: return "MLP";
    case AV_CODEC_ID_DTS:
        if (par->profile == AV_PROFILE_DTS_HD_MA) return "DTS-HD MA";
        if (par->profile == AV_PROFILE_DTS_HD_HRA) return "DTS-HD HRA";
        if (par->profile == AV_PROFILE_DTS_EXPRESS) return "DTS Express";
        return "DTS";
    case AV_CODEC_ID_EAC3:
        return par->profile == AV_PROFILE_EAC3_DDP_ATMOS ? "Dolby Digital+ Atmos" : "Dolby Digital+";
    case AV_CODEC_ID_AC3: return "Dolby Digital";
    case AV_CODEC_ID_AAC: return "AAC";
    case AV_CODEC_ID_FLAC: return "FLAC";
    case AV_CODEC_ID_OPUS: return "Opus";
    case AV_CODEC_ID_VORBIS: return "Vorbis";
    case AV_CODEC_ID_MP3: return "MP3";
    case AV_CODEC_ID_ALAC: return "ALAC";
    default:
        if (par->codec_id >= AV_CODEC_ID_PCM_S16LE && par->codec_id < AV_CODEC_ID_ADPCM_IMA_QT)
            return "PCM";
        return avcodec_get_name(par->codec_id);
    }
}

std::string channel_name(int ch)
{
    switch (ch) {
    case 1: return "Mono";
    case 2: return "Stereo";
    case 3: return "2.1";
    case 6: return "5.1";
    case 7: return "6.1";
    case 8: return "7.1";
    default: return ch > 0 ? std::to_string(ch) + " ch" : std::string();
    }
}

/* How good an audio stream is, for the default pick. */
int audio_rank(const AVStream *st)
{
    const AVCodecParameters *p = st->codecpar;
    int r = 0;
    switch (p->codec_id) {
    case AV_CODEC_ID_TRUEHD: case AV_CODEC_ID_MLP: r = 90; break;
    case AV_CODEC_ID_FLAC: case AV_CODEC_ID_ALAC: r = 85; break;
    case AV_CODEC_ID_DTS: r = p->profile == AV_PROFILE_DTS_HD_MA ? 88 : 60; break;
    case AV_CODEC_ID_EAC3: r = 55; break;
    case AV_CODEC_ID_AC3: r = 45; break;
    case AV_CODEC_ID_OPUS: r = 40; break;
    case AV_CODEC_ID_AAC: r = 35; break;
    default:
        r = (p->codec_id >= AV_CODEC_ID_PCM_S16LE && p->codec_id < AV_CODEC_ID_ADPCM_IMA_QT) ? 86 : 20;
    }
    return r * 10 + std::min(8, p->ch_layout.nb_channels);
}

bool is_commentary(const AVStream *st)
{
    if (st->disposition & (AV_DISPOSITION_COMMENT | AV_DISPOSITION_VISUAL_IMPAIRED))
        return true;
    const AVDictionaryEntry *t = av_dict_get(st->metadata, "title", nullptr, 0);
    return t && t->value && (strcasestr(t->value, "commentary") || strcasestr(t->value, "description"));
}

std::string stream_lang(const AVStream *st)
{
    const AVDictionaryEntry *l = av_dict_get(st->metadata, "language", nullptr, 0);
    return l && l->value ? l->value : "";
}

} // namespace

/*
 * PlaybackController asks this when a file opens (NUVIO_APP hook): the first
 * stream in the viewer's preferred languages, and within the language the
 * best one (lossless before lossy, more channels first), never commentary.
 */
extern "C" int nuvio_pick_audio_stream(AVFormatContext *fmt, int current)
{
    if (!fmt)
        return -1;
    auto decodable = [&](unsigned i) {
        const AVStream *st = fmt->streams[i];
        return st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(st->codecpar->codec_id);
    };
    auto best_in = [&](const std::string &key) {
        int best = -1, best_rank = -1;
        for (unsigned i = 0; i < fmt->nb_streams; i++) {
            if (!decodable(i) || is_commentary(fmt->streams[i]))
                continue;
            if (!key.empty() && nuvio_language_key(stream_lang(fmt->streams[i])) != key)
                continue;
            int rank = audio_rank(fmt->streams[i]);
            if (fmt->streams[i]->disposition & AV_DISPOSITION_DEFAULT)
                rank += 1;
            if (rank > best_rank) {
                best_rank = rank;
                best = (int)i;
            }
        }
        return best;
    };
    for (const std::string &lang : s_audio_langs) {
        const int pick = best_in(nuvio_language_key(lang));
        if (pick >= 0)
            return pick;
    }
    /* No preference matched: the best track in the file's own default language. */
    if (current >= 0 && current < (int)fmt->nb_streams) {
        const int pick = best_in(nuvio_language_key(stream_lang(fmt->streams[current])));
        if (pick >= 0)
            return pick;
    }
    return current;
}

namespace {

/* ---- background work -------------------------------------------------------------- */

struct OpenJob {
    pthread_t thread{};
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    bool ok = false;
    int kind = 0;                 /* 0 open, 1 audio switch */
    evo::PlaybackSource src;
    double at = 0;
    int audio_stream = -1;
};

void *open_thread(void *arg)
{
    OpenJob *j = static_cast<OpenJob *>(arg);
    if (j->kind == 1)
        j->ok = s_pb->switchAudioTrack(j->audio_stream);
    else
        j->ok = s_pb->startPlaybackSource(j->src, j->at);
    j->done = true;
    return nullptr;
}

bool start_job(OpenJob &j)
{
    j.done = false;
    j.ok = false;
    j.running = true;
    if (pthread_create(&j.thread, nullptr, open_thread, &j) != 0) {
        j.running = false;
        return false;
    }
    return true;
}

void finish_job(OpenJob &j)
{
    if (j.running) {
        pthread_join(j.thread, nullptr);
        j.running = false;
    }
}

/* Addon subtitles: asks each Stremio subtitle URL the page computed and adds
 * what comes back (preferred languages first). Runs beside the playback. */
struct SubFetch {
    std::vector<NuvioRequest::SubtitleRequest> requests;
    std::vector<std::string> langs;
    int session = 0;
};
std::atomic<int> s_session{0};

std::string fetch_text(const std::string &url, int timeout_s)
{
    AVIOContext *io = nullptr;
    AVDictionary *o = nullptr;
    std::string out;
    av_dict_set(&o, "rw_timeout", std::to_string(timeout_s * 1000000).c_str(), 0);
    av_dict_set(&o, "user_agent", "Mozilla/5.0 (PlayStation 5) NuvioPS5", 0);
    if (avio_open2(&io, url.c_str(), AVIO_FLAG_READ, nullptr, &o) >= 0) {
        unsigned char buf[16384];
        int n;
        while ((n = avio_read(io, buf, sizeof buf)) > 0 && out.size() < 4 * 1024 * 1024)
            out.append((const char *)buf, (size_t)n);
        avio_closep(&io);
    }
    av_dict_free(&o);
    return out;
}

void *subtitle_fetch_thread(void *arg)
{
    SubFetch *f = static_cast<SubFetch *>(arg);
    struct Found { std::string url, lang, label, headers; int pref; };
    std::vector<Found> found;
    for (const auto &q : f->requests) {
        if (s_session != f->session)
            break;
        const std::string body = fetch_text(q.url, 12);
        cJSON *root = cJSON_Parse(body.c_str());
        const cJSON *arr = cJSON_GetObjectItem(root, "subtitles");
        const cJSON *it;
        int n = 0;
        cJSON_ArrayForEach(it, arr) {
            const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(it, "url"));
            const char *lang = cJSON_GetStringValue(cJSON_GetObjectItem(it, "lang"));
            if (!url || !*url)
                continue;
            Found x;
            x.url = url;
            x.lang = lang ? lang : "";
            x.label = q.addon;
            const cJSON *h = cJSON_GetObjectItem(it, "headers");
            if (!h) {
                const cJSON *bh = cJSON_GetObjectItem(it, "behaviorHints");
                h = cJSON_GetObjectItem(cJSON_GetObjectItem(bh, "proxyHeaders"), "request");
            }
            if (h)
                x.headers = nuvio_headers_from_json(h, nullptr);
            x.pref = 1000;
            for (size_t k = 0; k < f->langs.size(); k++)
                if (nuvio_language_key(x.lang) == nuvio_language_key(f->langs[k]))
                    x.pref = (int)k;
            found.push_back(x);
            n++;
        }
        cJSON_Delete(root);
        evo_bt("subs: addon '%s' gave %d", q.addon.c_str(), n);
    }
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.pref < b.pref; });
    int added = 0;
    for (const Found &x : found) {
        if (s_session != f->session || added >= 40)
            break;
        nuvio_subs_add_external(x.url.c_str(), x.lang.c_str(), x.label.c_str(), x.headers.c_str());
        added++;
    }
    delete f;
    return nullptr;
}

/* ---- per-playback state ------------------------------------------------------------ */

struct Session {
    NuvioRequest req;
    NuvioResult res;
    OpenJob job;
    bool opened = false;          /* the stream is open (job finished ok) */
    bool started = false;         /* a frame is on screen */
    bool failed = false;
    std::string error;
    bool cancel = false;
    bool done = false;
    bool user_picked_subs = false;
    double open_started = 0;
    double last_frame_at = 0;
    double last_pos = 0, last_pos_change = 0;
    double wd_since = 0;              /* hardware decoder watchdog */
    int wd_frames = -1;
    int wd_consumed = 0;
        double vq_empty_since = 0;        /* underrun watch */
    int rebuffers = 0;
    double last_rebuffer_at = -1e9;
    int64_t last_pts = INT64_MIN;
    NuvioStatus st;
    std::vector<int> audio_streams;   /* status.audio index -> stream */
};

void update_hdr(bool playing)
{
    static int s_want = 0;
    static bool s_refused = false;
    const int trc = evo_agc_runtime_last_video_trc();
    const int want = (playing && (trc == 16 || trc == 18)) ? 1 : 0;
    if (!playing)
        s_refused = false;
    if (want == s_want || (want && s_refused))
        return;
    evo_bt("nuvio: hdr10 %s (trc=%d)", want ? "on" : "off", trc);
    if (evo_agc_runtime_set_hdr_output(want) == 0)
        s_want = want;
    else if (want)
        s_refused = true;   /* the display said no: stay tone-mapped */
}

/* Dolby Vision profile of a stream, 0 when it carries none. */
int dolby_vision_profile(const AVCodecParameters *p)
{
    for (int i = 0; i < p->nb_coded_side_data; i++)
        if (p->coded_side_data[i].type == AV_PKT_DATA_DOVI_CONF &&
            p->coded_side_data[i].size >= sizeof(AVDOVIDecoderConfigurationRecord))
            return ((const AVDOVIDecoderConfigurationRecord *)p->coded_side_data[i].data)->dv_profile;
    return 0;
}

std::string quality_line()
{
    if (!play_fmt)
        return "";
    const char *dot = "  \xC2\xB7  ";
    std::string q;
    if (video_stream_index >= 0 && video_stream_index < (int)play_fmt->nb_streams) {
        const AVCodecParameters *p = play_fmt->streams[video_stream_index]->codecpar;
        const int h = p->height;
        if (h >= 2000) q = "4K";
        else if (h >= 1400) q = "1440p";
        else if (h >= 1000) q = "1080p";
        else if (h >= 700) q = "720p";
        else if (h > 0) q = std::to_string(h) + "p";
        /* The PS5 cannot output Dolby Vision: profile 8 shows its HDR10/HLG
         * base layer, and that is what the label says. */
        const int dv = dolby_vision_profile(p);
        const char *hdr = p->color_trc == AVCOL_TRC_SMPTE2084 ? (dv ? "HDR10 (Dolby Vision)" : "HDR10")
                          : p->color_trc == AVCOL_TRC_ARIB_STD_B67 ? (dv ? "HLG (Dolby Vision)" : "HLG")
                          : dv == 5 ? "Dolby Vision 5" : nullptr;
        if (hdr)
            q += std::string(q.empty() ? "" : dot) + hdr;
        q += std::string(q.empty() ? "" : dot) + video_codec_name(p->codec_id);
    }
    if (audio_stream_index >= 0 && audio_stream_index < (int)play_fmt->nb_streams) {
        const AVCodecParameters *p = play_fmt->streams[audio_stream_index]->codecpar;
        q += std::string(q.empty() ? "" : dot) + audio_codec_name(p);
        const std::string ch = channel_name(p->ch_layout.nb_channels);
        if (!ch.empty())
            q += " " + ch;
    }
    return q;
}

void refresh_audio(Session &s)
{
    s.st.audio.clear();
    s.audio_streams.clear();
    s.st.audio_active = -1;
    for (const auto &t : s_pb->getAudioTracks()) {
        NuvioAudioTrack a;
        a.stream = t.streamIndex;
        a.lang = (t.language == "UND" || t.language == "und") ? "" : t.language;
        a.title = t.title;
        if (play_fmt && t.streamIndex >= 0 && t.streamIndex < (int)play_fmt->nb_streams) {
            const AVStream *st = play_fmt->streams[t.streamIndex];
            a.codec = audio_codec_name(st->codecpar);
            a.is_default = (st->disposition & AV_DISPOSITION_DEFAULT) != 0;
        } else {
            a.codec = t.codecName;
        }
        a.channels = channel_name(t.channels);
        if (t.streamIndex == s_pb->getActiveAudioStream())
            s.st.audio_active = (int)s.st.audio.size();
        s.st.audio.push_back(a);
        s.audio_streams.push_back(t.streamIndex);
    }
}

/* The viewer's subtitle preference: their language, else a forced track in
 * the audio's language when subtitles are off. Re-run as addon tracks arrive
 * until the viewer picks one themselves. */
void auto_select_subtitles(Session &s)
{
    if (s.user_picked_subs || nuvio_subs_selected() >= 0)
        return;
    const NuvioPrefs &p = s.req.prefs;
    const int n = nuvio_subs_count();
    auto find = [&](const std::string &lang, bool forced_only) {
        const std::string key = nuvio_language_key(lang);
        int best = -1, best_score = -1;
        for (int i = 0; i < n; i++) {
            nuvio_sub_track t;
            if (nuvio_subs_track(i, &t) != 0 || t.state != 1)
                continue;
            if (nuvio_language_key(t.lang) != key || forced_only != (t.forced != 0))
                continue;
            const int score = (t.external ? 0 : 20) + (t.hearing_impaired ? 0 : 5) +
                              (t.is_default ? 2 : 0) + (t.bitmap ? 0 : 1);
            if (score > best_score) {
                best_score = score;
                best = i;
            }
        }
        return best;
    };
    int pick = -1;
    if (p.subtitles_enabled) {
        for (const std::string &l : p.subtitle_langs)
            if ((pick = find(l, false)) >= 0)
                break;
    } else if (p.forced_only_when_off && s.st.audio_active >= 0) {
        pick = find(s.st.audio[s.st.audio_active].lang, true);
    }
    if (pick >= 0) {
        nuvio_subs_select(pick);
        s_osd.note_subtitle_choice();
    }
}

nuvio_rect video_rect(const pp_video_frame &f)
{
    nuvio_rect r = {0, 0, (float)CW, (float)CH};
    float dw = (float)(f.disp_w ? f.disp_w : 16), dh = (float)(f.disp_h ? f.disp_h : 9);
    if (play_fmt && video_stream_index >= 0 && video_stream_index < (int)play_fmt->nb_streams) {
        const AVRational sar = play_fmt->streams[video_stream_index]->codecpar->sample_aspect_ratio;
        if (sar.num > 0 && sar.den > 0)
            dw *= (float)sar.num / (float)sar.den;
    }
    const int mode = (int)s_pb->getViewMode();
    if (mode == 2)
        return r;
    const float sx = CW / dw, sy = CH / dh;
    const float k = mode == 1 ? std::max(sx, sy) : std::min(sx, sy);
    r.w = dw * k;
    r.h = dh * k;
    r.x = (CW - r.w) * 0.5f;
    r.y = (CH - r.h) * 0.5f;
    return r;
}

/* Open source `index` of the request at `at` seconds, on the worker thread. */
void open_source(Session &s, int index, double at)
{
    const NuvioSource src = s.req.sources[index];
    nuvio_subs_close();
    for (const NuvioSubtitleRef &r : s.req.subtitles)
        nuvio_subs_add_external(r.url.c_str(), r.lang.c_str(), r.label.c_str(), r.headers.c_str());
    s.req.url = src.url;
    s.req.headers = src.headers;
    s.req.user_agent = src.user_agent;
    s.req.source_index = index;
    s.req.stream_title = src.title;
    s.req.stream_description = src.description;
    s.req.stream_addon = src.addon;
    s.req.start_position = at;
    std::snprintf(nuvio_stream_headers, sizeof nuvio_stream_headers, "%s", src.headers.c_str());
    std::snprintf(nuvio_stream_user_agent, sizeof nuvio_stream_user_agent, "%s", src.user_agent.c_str());
    s.opened = s.started = s.failed = false;
    s.error.clear();
    s.last_pts = INT64_MIN;
    s.user_picked_subs = false;
    s.job.kind = 0;
    s.job.src = evo::PlaybackSource();
    s.job.src.url = src.url;
    s.job.src.title = s.req.header_title();
    s.job.src.provider = "nuvio";
    s.job.at = at;
    s.open_started = now_s();
    s_osd.begin(&s.req, s.open_started);
    start_job(s.job);
    evo_bt("nuvio: opening source %d at %.1f s", index, at);
}

void apply(Session &s, const OsdCommand &c)
{
    switch (c.cmd) {
    case OsdCmd::TogglePause:
        s_pb->togglePause();
        break;
    case OsdCmd::SeekTo:
        s_pb->seekTo(std::max(0.0, c.value));
        break;
    case OsdCmd::Stop:
        s.done = true;
        if (s.res.state.empty())
            s.res.state = s.failed ? "error" : "stopped";
        break;
    case OsdCmd::PlayNext:
        s.done = true;
        s.res.state = "ended";
        s.res.action = "next";
        s.res.season = s.req.next.season;
        s.res.episode = s.req.next.episode;
        break;
    case OsdCmd::PlayEpisode:
        s.done = true;
        s.res.state = "stopped";
        s.res.action = "episode";
        s.res.season = c.season;
        s.res.episode = c.episode;
        break;
    case OsdCmd::SelectAudio:
        if (c.index >= 0 && c.index < (int)s.audio_streams.size() && !s.job.running) {
            s.job.kind = 1;
            s.job.audio_stream = s.audio_streams[c.index];
            s.st.switching = true;
            start_job(s.job);
        }
        break;
    case OsdCmd::SelectSubtitle:
        s.user_picked_subs = true;
        nuvio_subs_select(c.index);
        break;
    case OsdCmd::SubtitleDelay:
        nuvio_subs_set_delay_ms((int)c.value);
        break;
    case OsdCmd::SubtitleStyle:
        break;
    case OsdCmd::SetViewMode:
        s_pb->setViewMode((evo::ViewMode)c.index);
        break;
    case OsdCmd::SwitchSource:
        if (c.index >= 0 && c.index < (int)s.req.sources.size() && !s.job.running) {
            const double at = s.started ? s_pb->getPositionSeconds() : s.req.start_position;
            s_pb->stopPlayback();
            open_source(s, c.index, at);
        }
        break;
    }
}

} // namespace

/* ---- public -------------------------------------------------------------------------- */

extern "C" void nuvio_player_init(int user_id)
{
    s_user_id = user_id;
    s_pb = new evo::PlaybackController();
    evo::Application::getInstance().setPlaybackController(s_pb);
    avformat_network_init();
    pp_playback_init(&g_pp_pb);
    pp_playback_set_output(&g_pp_pb, evo::DisplayWidth, evo::DisplayHeight, PP_ASPECT_FIT);
    evo_vdec_prefer_nv12(1);
    if (ui_canvas_init(&s_osd_canvas, CW, CH) != 0 || ui_canvas_init(&s_sub_canvas, CW, CH) != 0)
        evo_bt("nuvio: overlay canvases could not be allocated");
    if (ui_text_init() != 0)
        evo_bt("nuvio: fonts failed");
    if (nuvio_subs_init() != 0)
        evo_bt("nuvio: libass failed");
    evo_bt("nuvio: player ready (%dx%d)", evo::DisplayWidth, evo::DisplayHeight);
}

/*
 * The hardware decoder is taking the stream but returning no pictures (or
 * gave up): reopen the same source at the same point on the software decoder.
 * One-way for this playback - g_vdec_force_ffmpeg stays set until the next
 * request - so a seek cannot land back on the decoder that failed.
 */
static void reopen_software(Session &s, const char *why)
{
    const double at = s.started ? s_pb->getPositionSeconds() : s.req.start_position;
    evo_bt("nuvio: hardware decoder %s - reopening at %.1f s on the software decoder", why, at);
    g_vdec_force_ffmpeg = 1;
    s_pb->stopPlayback();
    nuvio_subs_close();
    for (const NuvioSubtitleRef &r : s.req.subtitles)
        nuvio_subs_add_external(r.url.c_str(), r.lang.c_str(), r.label.c_str(), r.headers.c_str());
    s.req.start_position = at;
    s.opened = s.started = s.failed = false;
    s.error.clear();
    s.last_pts = INT64_MIN;
    s.job.kind = 0;
    s.job.src = evo::PlaybackSource();
    s.job.src.url = s.req.url;
    s.job.src.title = s.req.header_title();
    s.job.src.provider = "nuvio";
    s.job.at = at;
    s.open_started = now_s();
    s.wd_frames = -1;
    s_osd.begin(&s.req, s.open_started);
    start_job(s.job);
}

extern "C" void nuvio_player_run(const char *json)
{
    static Session s_storage;
    Session &s = s_storage;
    s.~Session();
    new (&s) Session();
    if (!nuvio_request_parse(json, s.req)) {
        evo_bt("nuvio: unusable play request");
        return;
    }
    g_vdec_force_ffmpeg = 0;      /* each playback starts on the hardware decoder */
    {
        /* Decoder test overrides (LAN debug builds of the payload serve them). */
        char *conf = nullptr;
        nuvio_vdec_conf[0] = 0;
        if (nuvio_bridge_get("/api/debug/vdec-conf", &conf) == 200 && conf)
            std::snprintf(nuvio_vdec_conf, sizeof nuvio_vdec_conf, "%s", conf);
        free(conf);
    }
        const int session = ++s_session;
    s_audio_langs = s.req.prefs.audio_langs;
    std::snprintf(nuvio_stream_headers, sizeof nuvio_stream_headers, "%s", s.req.headers.c_str());
    std::snprintf(nuvio_stream_user_agent, sizeof nuvio_stream_user_agent, "%s", s.req.user_agent.c_str());

    nuvio_input_open(s_user_id);
    nuvio_control_set_playing(1);
    nuvio_subs_close();
    nuvio_subs_set_style(&s.req.prefs.style);
    nuvio_subs_set_delay_ms(0);
    for (const NuvioSubtitleRef &r : s.req.subtitles)
        nuvio_subs_add_external(r.url.c_str(), r.lang.c_str(), r.label.c_str(), r.headers.c_str());
    if (!s.req.subtitle_requests.empty()) {
        SubFetch *f = new SubFetch{s.req.subtitle_requests, s.req.prefs.subtitle_langs, session};
        pthread_t t;
        if (pthread_create(&t, nullptr, subtitle_fetch_thread, f) == 0)
            pthread_detach(t);
        else
            delete f;
    }

    screen = kScreenPlayer;
    evo_agc_runtime_set_player_mode(1);
    s.open_started = now_s();
    s_osd.begin(&s.req, s.open_started);

    s.job.kind = 0;
    s.job.src.url = s.req.url;
    s.job.src.title = s.req.header_title();
    s.job.src.provider = "nuvio";   /* a network stream: no local resume file */
    s.job.at = s.req.start_position;
    evo_bt("nuvio: open '%s' at %.1f s", s.req.header_title().c_str(), s.req.start_position);
    start_job(s.job);

    double last_report = 0, last_diag = 0, last_auto = 0;
    bool seen_active = false;
    std::vector<OsdCommand> cmds;

    while (!s.done) {
        const double now = now_s();
        s.st.now = now;
        cmds.clear();

        /* Background open / audio switch finished? */
        if (s.job.running && s.job.done) {
            finish_job(s.job);
            s.st.switching = false;
            if (s.job.kind == 0) {
                if (s.cancel) {
                    s.done = true;
                    s.res.state = "stopped";
                } else if (!s.job.ok) {
                    s.failed = true;
                    s.error = s.req.str("open_failed", "This stream could not be opened. The link may have "
                                                       "expired, or the source is unavailable.");
                    evo_bt("nuvio: open failed");
                } else {
                    s.opened = true;
                    nuvio_subs_open(play_fmt, video_stream_index);
                    refresh_audio(s);
                    auto_select_subtitles(s);
                    s.st.quality_line = quality_line();
                    seen_active = false;
                    evo_bt("nuvio: open ok - %s", s.st.quality_line.c_str());
                    if (play_fmt && video_stream_index >= 0 &&
                        dolby_vision_profile(play_fmt->streams[video_stream_index]->codecpar) == 5)
                        s_osd.toast(s.req.str("dv5_unsupported",
                                              "Dolby Vision profile 5 can't be shown on PS5 - colours may be off. Try another source."),
                                    now);
                }
            } else {
                refresh_audio(s);
                s.st.quality_line = quality_line();
                if (!s.job.ok)
                    s_osd.toast(s.req.str("audio_switch_failed", "That audio track could not be played"), now);
            }
            if (s.done)
                break;
        }
        const bool engine_ready = s.opened && !s.job.running;

        /* Input. */
        nuvio_input_state in;
        nuvio_input_poll(&in);
        if (nuvio_control_take_stop() || nuvio_control_quit_requested()) {
            if (s.job.running && !s.opened) {
                s.cancel = true;
            } else {
                s.done = true;
                s.res.state = "stopped";
                break;
            }
        }

        /* Status. */
        s.st.started = s.started;
        s.st.error = s.error;
        if (engine_ready && !s.failed) {
            const bool active = s_pb->isActive();
            if (active)
                seen_active = true;
            s.st.paused = s_pb->isPaused();
            s.st.position = s_pb->getPositionSeconds();
            s.st.duration = s_pb->getDurationSeconds();
            int vq = 0, aq = 0, ab = 0;
            evo_pb_queue_depth(&vq, &aq, &ab);
            s.st.buffered = s.st.position + evo_demux_buffered_s();
            if (!s.started) {
                s.st.open_progress = evo_demux_prebuffer_progress();
                s.st.open_stage = s.req.str("buffering", "Buffering\xE2\x80\xA6");
            }
            if (std::fabs(s.st.position - s.last_pos) > 0.05) {
                s.last_pos = s.st.position;
                s.last_pos_change = now;
            }
            s.st.buffering = s.started && !s.st.paused && !evo_pb_is_eof() &&
                             (pb_prebuffer_hold || (vq == 0 && now - s.last_pos_change > 0.7));
            /*
             * Underrun: the network fell behind and the video queue ran dry.
             * Left alone the decoders limp on frame by frame while audio
             * drifts ahead - a stutter for as long as the dip lasts. Hold
             * instead and refill a few seconds, longer each time it recurs
             * (a link that cannot sustain the bitrate wants more cushion per
             * stop, not more stops).
             */
            const bool starving = s.started && !s.st.paused && !pb_prebuffer_hold &&
                                  !evo_pb_is_eof() && video_stream_index >= 0 && vq == 0 &&
                                  !evo_demux_seek_busy() && !s_osd.post_play_active();
            if (!starving) {
                s.vq_empty_since = 0;
            } else if (s.vq_empty_since == 0) {
                s.vq_empty_since = now;
            } else if (now - s.vq_empty_since >= 0.15) {
                static const int64_t kRefillUs[] = {3000000, 6000000, 10000000};
                if (now - s.last_rebuffer_at > 120)
                    s.rebuffers = 0;
                evo_demux_rebuffer(kRefillUs[std::min(s.rebuffers, 2)], 20000);
                s.rebuffers++;
                s.last_rebuffer_at = now;
                s.vq_empty_since = 0;
            }
            /* Hardware decoder fed but silent - packets go in, no picture comes
             * out (6 s and three dozen packets, outside seeks and pauses) - or
             * given up outright: software decoder from the same point. */
            {
                const int consumed = dbg_video_packets - vq;
                const bool watch = !g_vdec_force_ffmpeg && video_stream_index >= 0 &&
                                   evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE &&
                                   !s.st.paused && !evo_demux_seek_busy();
                if (!watch || dbg_video_frames != s.wd_frames) {
                    s.wd_frames = dbg_video_frames;
                    s.wd_consumed = consumed;
                    s.wd_since = now;
                }
                if (!g_vdec_force_ffmpeg && evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE &&
                    (evo_pb_decode_fatal() ||
                     (watch && now - s.wd_since > 6.0 && consumed - s.wd_consumed > 36))) {
                    reopen_software(s, evo_pb_decode_fatal() ? "gave up" : "returns no pictures");
                    continue;
                }
            }
            if (evo_pb_decode_fatal()) {
                s.failed = true;
                s.error = s.req.str("decode_failed", "This video could not be decoded.");
            }
            /* Ended: the engine stopped by itself, or the last frame has shown. */
            if (!s.failed && s.started && !s_osd.post_play_active() &&
                ((seen_active && !active) ||
                 (evo_pb_is_eof() && vq == 0 &&
                  (now - s.last_frame_at > 0.8 || (s.st.duration > 0 && s.st.position >= s.st.duration - 0.3))))) {
                s.res.state = "ended";
                s_osd.playback_ended(s.st, cmds);
            }
        } else if (!s.started) {
            s.st.open_stage = s.req.str("opening", "Opening stream\xE2\x80\xA6");
            s.st.open_progress = 0;
        }
        if (!s.started && !s.failed && s.opened && now - s.open_started > 30.0) {
            s.failed = true;
            s.error = s.req.str("did_not_start", "Playback did not start. The source may be too slow or unavailable.");
        }
        s.st.error = s.error;

        s_osd.input(in, s.st, cmds);
        s_osd.tick(s.st, cmds);
        for (const OsdCommand &c : cmds) {
            if (c.cmd == OsdCmd::Stop && s.job.running && !s.opened) {
                s.cancel = true;     /* the open finishes, then we leave */
                continue;
            }
            apply(s, c);
            if (s.done)
                break;
        }
        if (s.done)
            break;

        /* Subtitles appear as addon tracks arrive. */
        if (s.opened && now - last_auto > 1.0) {
            last_auto = now;
            auto_select_subtitles(s);
        }

        /* ---- draw ---- */
        update_hdr(s.started && engine_ready && !s.failed);
        pp_video_frame f;
        std::memset(&f, 0, sizeof f);
        bool have = false, new_frame = false;
        int64_t pts = s.last_pts;
        if (engine_ready && !s.failed) {
            have = pp_playback_get_video_frame(&g_pp_pb, &f) && f.ready;
            pts = g_pp_pb.display_pts_us;
            new_frame = have && (pts != s.last_pts || g_pp_pb.seek_discarding);
            if (have && !s.started) {
                s.started = true;
                s.st.started = true;
                evo_bt("nuvio: first frame %ux%u ten_bit=%d trc=%d after %.2f s", f.disp_w, f.disp_h,
                       f.ten_bit, f.color_trc, now - s.open_started);
            }
            if (new_frame)
                s.last_frame_at = now;
        }

        bool sub_changed = false;
        if (s.started && have)
            sub_changed = nuvio_subs_render(&s_sub_canvas, pts, video_rect(f), s_osd.subtitle_lift()) != 0;
        const bool osd_changed = s_osd.render(s_osd_canvas, s.st);
        const bool stale = have && evo_agc_runtime_video_slot_stale(pts);

        if (new_frame || stale || sub_changed || osd_changed) {
            evo_agc_runtime_frame_begin();
            evo_agc_runtime_clear_black();
            if (have) {
                const int is_direct =
                    (evo_pb_active_backend() == EVO_VDEC_BACKEND_NATIVE && !f.held && f.uv != nullptr) ? 1 : 0;
                evo_agc_blit_yuv(f.y, f.y_pitch, f.uv, f.uv_pitch, f.u, f.u_pitch, f.v, f.v_pitch,
                                 (int)f.coded_w, (int)f.coded_h, (int)f.disp_w, (int)f.disp_h,
                                 (int)s_pb->getViewMode(), f.ten_bit, f.color_trc, is_direct, pts);
                s.last_pts = pts;
            }
            if (have && (nuvio_subs_visible() || sub_changed))
                evo_agc_composite_overlay(0, s_sub_canvas.px, CW, CH, sub_changed ? 1 : 0, 1.0f);
            if (s_osd.visible())
                evo_agc_composite_overlay(1, s_osd_canvas.px, CW, CH, osd_changed ? 1 : 0, 1.0f);
            evo_agc_runtime_present();
        } else {
            usleep(2000);
        }

        if (now - last_report >= 1.0 && s.started) {
            last_report = now;
            nuvio_control_report(s.req.id.c_str(), s.st.position, s.st.duration);
        }
        if (now - last_diag >= 5.0 && engine_ready) {
            last_diag = now;
            int vq = 0, aq = 0, ab = 0;
            evo_pb_queue_depth(&vq, &aq, &ab);
            evo_bt("nuvio: pos=%.1f/%.1f paused=%d buffering=%d vq=%d aq=%d ab=%d buf=%.1fs demux=%d eof=%d ringfb=%lu vpk=%d vfr=%d ctl=%u/%d vclk=%.2f aclk=%.2f",
                   s.st.position, s.st.duration, (int)s.st.paused, (int)s.st.buffering, vq, aq, ab,
                   evo_demux_buffered_s(), evo_demux_state, evo_pb_is_eof(),
                   (unsigned long)packet_queue_ring_fallbacks,
                   dbg_video_packets, dbg_video_frames, nuvio_control_beats, nuvio_control_stage,
                   evo_pb_video_clock_s(), evo_pb_audio_clock_s());
            evo_log_alloc_state("play");
        }
    }

    /* ---- leave ---- */
    s_session++;
    finish_job(s.job);
    s.res.position = s.started ? s_pb->getPositionSeconds() : s.req.start_position;
    s.res.duration = s_pb->getDurationSeconds();
    if (s.failed && s.res.error.empty())
        s.res.error = s.error;
    if (s.res.state.empty())
        s.res.state = s.failed ? "error" : "stopped";
    if (!s.st.audio.empty() && s.st.audio_active >= 0)
        s.res.audio_lang = s.st.audio[s.st.audio_active].lang;
    {
        nuvio_sub_track t;
        const int sel = nuvio_subs_selected();
        s.res.subtitles_on = sel >= 0;
        if (sel >= 0 && nuvio_subs_track(sel, &t) == 0)
            s.res.subtitle_lang = t.lang;
        s.res.subtitle_delay_ms = nuvio_subs_delay_ms();
    }
    evo_bt("nuvio: %s at %.1f / %.1f s%s%s%s%s", s.res.state.c_str(), s.res.position, s.res.duration,
           s.res.action.empty() ? "" : " -> ", s.res.action.c_str(), s.res.error.empty() ? "" : " - ",
           s.res.error.c_str());
    s_pb->stopPlayback();
    screen = kScreenNone;
    nuvio_subs_close();
    ui_image_clear();
    s_osd.end();
    update_hdr(false);
    evo_agc_runtime_set_player_mode(0);
    evo_agc_runtime_frame_begin();
    evo_agc_runtime_clear_black();
    evo_agc_runtime_present();
    nuvio_control_set_playing(0);
    nuvio_input_close();
    evo_boot_log_flush();
    nuvio_bridge_state_json(nuvio_result_json(s.req, s.res).c_str());
}
