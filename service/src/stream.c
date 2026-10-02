/*
 * Stream repackaging for the PS5 browser.
 *
 * The PS5's WebKit decodes H.264/HEVC video and AAC/AC-3/E-AC-3 audio through
 * Media Source Extensions but cannot read Matroska (most debrid files) or DTS,
 * TrueHD, FLAC and Opus audio. This turns any direct video URL into a VOD HLS
 * stream of fragmented-MP4 segments that Nuvio plays with hls.js:
 *
 *   POST /api/stream/open {url, headers}   probe + keyframe index -> session
 *   GET  /stream/<id>/index.m3u8           segments cut on keyframes (~6 s)
 *   GET  /stream/<id>/init.mp4             ftyp+moov from FFmpeg's mp4 muxer
 *   GET  /stream/<id>/<n>.m4s              moof+mdat built here, per request
 *
 * Video packets are copied. Audio is copied when the browser supports it and
 * otherwise re-encoded to AC-3 (surround) or AAC (stereo). Segments are cut
 * at keyframes from the source's own index (Matroska Cues, MP4 sample
 * tables), so every segment decodes on its own and seeking is exact.
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "nuvio.h"

#define MAX_SESSIONS 4
#define SESSION_IDLE_SECONDS 300
#define TARGET_SEGMENT_SECONDS 6.0
/* Keeps decode timestamps positive for B-frame streams. */
#define TIMESTAMP_SHIFT_SECONDS 10
/* After a segment's last video packet, keep reading this much for audio. */
#define AUDIO_TAIL_SECONDS 2.0

typedef struct {
  uint8_t *data;
  size_t len, cap;
} buf_t;

typedef struct {
  int64_t pts;   /* source video stream time base */
  double seconds;
} keyframe_t;

typedef struct {
  int64_t start_pts, end_pts; /* video stream time base; end exclusive */
  double duration;
} segment_t;

typedef struct {
  uint32_t duration, size, flags;
  int32_t cto;
} sample_t;

typedef struct {
  sample_t *samples;
  int count, cap;
  buf_t data;
  int64_t first_dts; /* output time scale, shifted */
  int have_first;
} track_out_t;

typedef struct session {
  int used;
  char id[17];
  time_t last_access;
  pthread_mutex_t lock;

  AVFormatContext *in;
  int vidx, aidx;
  int transcode_audio;
  enum AVCodecID out_audio_codec;

  AVCodecContext *adec, *aenc;
  SwrContext *swr;
  AVAudioFifo *fifo;

  /* Packets read past the end of the last segment, for the next one. */
  AVPacket **pending;
  int pending_count, pending_head, pending_cap;
  int next_index;
  int64_t next_audio_pts; /* converted audio timeline, output time scale */

  keyframe_t *keyframes;
  int keyframe_count;
  segment_t *segments;
  int segment_count;
  double duration;

  buf_t init;
  AVRational vtb_out, atb_out; /* output track time scales */
  char codecs[96];
} session_t;

static session_t g_sessions[MAX_SESSIONS];

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}
static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- small helpers ------------------------------------------------------- */

static int buf_reserve(buf_t *b, size_t extra) {
  if (b->len + extra <= b->cap)
    return 0;
  size_t cap = b->cap ? b->cap : 65536;
  while (cap < b->len + extra)
    cap *= 2;
  uint8_t *grown = realloc(b->data, cap);
  if (!grown)
    return -1;
  b->data = grown;
  b->cap = cap;
  return 0;
}

static int buf_put(buf_t *b, const void *data, size_t len) {
  if (buf_reserve(b, len))
    return -1;
  memcpy(b->data + b->len, data, len);
  b->len += len;
  return 0;
}

static void put32(buf_t *b, uint32_t v) {
  uint8_t x[4] = {v >> 24, v >> 16, v >> 8, v};
  buf_put(b, x, 4);
}

static void put64(buf_t *b, uint64_t v) {
  put32(b, (uint32_t)(v >> 32));
  put32(b, (uint32_t)v);
}

static void patch32(buf_t *b, size_t at, uint32_t v) {
  b->data[at] = v >> 24;
  b->data[at + 1] = v >> 16;
  b->data[at + 2] = v >> 8;
  b->data[at + 3] = v;
}

static size_t box_begin(buf_t *b, const char *type) {
  size_t at = b->len;
  put32(b, 0);
  buf_put(b, type, 4);
  return at;
}

static void box_end(buf_t *b, size_t at) {
  patch32(b, at, (uint32_t)(b->len - at));
}

static void free_buf(buf_t *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}

static void track_reset(track_out_t *t) {
  free(t->samples);
  free_buf(&t->data);
  memset(t, 0, sizeof(*t));
}

static int track_add(track_out_t *t, const uint8_t *data, int size, int64_t dts, int64_t pts,
                     int64_t duration, int keyframe) {
  if (t->count == t->cap) {
    int cap = t->cap ? t->cap * 2 : 256;
    sample_t *grown = realloc(t->samples, (size_t)cap * sizeof(sample_t));
    if (!grown)
      return -1;
    t->samples = grown;
    t->cap = cap;
  }
  if (!t->have_first) {
    t->first_dts = dts;
    t->have_first = 1;
  }
  sample_t *s = &t->samples[t->count++];
  s->size = (uint32_t)size;
  s->duration = duration > 0 ? (uint32_t)duration : 0;
  s->cto = (int32_t)(pts - dts);
  /* sample_depends_on=2 for sync samples, else depends_on=1|non_sync. */
  s->flags = keyframe ? 0x02000000u : 0x01010000u;
  return buf_put(&t->data, data, (size_t)size);
}

/* Durations come from the next sample's dts; the last one keeps its own. */
static void track_fix_durations(track_out_t *t, const int64_t *dts_list) {
  for (int i = 0; i + 1 < t->count; i++) {
    int64_t d = dts_list[i + 1] - dts_list[i];
    if (d > 0)
      t->samples[i].duration = (uint32_t)d;
  }
  if (t->count > 1 && t->samples[t->count - 1].duration == 0)
    t->samples[t->count - 1].duration = t->samples[t->count - 2].duration;
}

/* ---- init segment -------------------------------------------------------- */

typedef struct {
  buf_t *out;
} avio_sink_t;

static int sink_write(void *opaque, const uint8_t *data, int size) {
  avio_sink_t *sink = opaque;
  return buf_put(sink->out, data, (size_t)size) == 0 ? size : AVERROR(ENOMEM);
}

/* Offset of the first top-level box of this type, or -1. */
static long find_top_box(const buf_t *b, const char *type) {
  size_t at = 0;
  while (at + 8 <= b->len) {
    uint32_t size = (uint32_t)b->data[at] << 24 | b->data[at + 1] << 16 | b->data[at + 2] << 8 |
                    b->data[at + 3];
    if (!memcmp(b->data + at + 4, type, 4))
      return (long)at;
    if (size < 8)
      return -1;
    at += size;
  }
  return -1;
}

/* Builds ftyp+moov with FFmpeg's mp4 muxer. AC-3/E-AC-3 sample entries need
 * a real packet, so a first keyframe and audio packet are muxed and only the
 * bytes before the first moof are kept. */
static int build_init(session_t *s, AVCodecParameters *apar_out, AVPacket *vpkt, AVPacket *apkt,
                      AVRational apkt_tb) {
  AVFormatContext *mux = NULL;
  AVIOContext *pb = NULL;
  AVDictionary *opts = NULL;
  buf_t out = {0};
  avio_sink_t sink = {&out};
  uint8_t *io_buf = av_malloc(65536);
  AVStream *vst, *ast = NULL;
  int rc = -1;
  long moov;
  size_t moov_end;

  if (!io_buf || avformat_alloc_output_context2(&mux, NULL, "mp4", NULL) < 0)
    goto out;
  pb = avio_alloc_context(io_buf, 65536, 1, &sink, NULL, sink_write, NULL);
  if (!pb)
    goto out;
  io_buf = NULL;
  mux->pb = pb;

  vst = avformat_new_stream(mux, NULL);
  avcodec_parameters_copy(vst->codecpar, s->in->streams[s->vidx]->codecpar);
  vst->codecpar->codec_tag = vst->codecpar->codec_id == AV_CODEC_ID_HEVC
                                 ? MKTAG('h', 'v', 'c', '1')
                                 : MKTAG('a', 'v', 'c', '1');
  vst->time_base = s->in->streams[s->vidx]->time_base;
  if (s->aidx >= 0 && apar_out) {
    ast = avformat_new_stream(mux, NULL);
    avcodec_parameters_copy(ast->codecpar, apar_out);
    ast->codecpar->codec_tag = 0;
    ast->time_base = (AVRational){1, apar_out->sample_rate};
  }

  av_dict_set(&opts, "movflags", "frag_custom+empty_moov+default_base_moof+delay_moov", 0);
  if (avformat_write_header(mux, &opts) < 0) {
    nuvio_log("stream: init header failed");
    goto out;
  }
  s->vtb_out = vst->time_base;
  if (ast)
    s->atb_out = ast->time_base;

  if (vpkt) {
    AVPacket *p = av_packet_clone(vpkt);
    p->stream_index = 0;
    av_packet_rescale_ts(p, s->in->streams[s->vidx]->time_base, vst->time_base);
    if (p->dts == AV_NOPTS_VALUE)
      p->dts = p->pts;
    av_write_frame(mux, p);
    av_packet_free(&p);
  }
  if (ast && apkt) {
    AVPacket *p = av_packet_clone(apkt);
    p->stream_index = 1;
    av_packet_rescale_ts(p, apkt_tb, ast->time_base);
    if (p->dts == AV_NOPTS_VALUE)
      p->dts = p->pts;
    av_write_frame(mux, p);
    av_packet_free(&p);
  }
  av_write_frame(mux, NULL); /* flush the fragment, which emits the moov */
  avio_flush(pb);

  /* The init segment is everything up to the end of the moov box. */
  moov = find_top_box(&out, "moov");
  if (moov < 0) {
    nuvio_log("stream: muxer produced no moov (len %zu)", out.len);
    goto out;
  }
  moov_end = (size_t)moov + ((uint32_t)out.data[moov] << 24 | out.data[moov + 1] << 16 |
                             out.data[moov + 2] << 8 | out.data[moov + 3]);
  if (moov_end > out.len)
    goto out;
  free_buf(&s->init);
  buf_put(&s->init, out.data, moov_end);
  rc = 0;

out:
  if (mux) {
    mux->pb = NULL;
    avformat_free_context(mux);
  }
  if (pb) {
    av_freep(&pb->buffer);
    avio_context_free(&pb);
  }
  av_free(io_buf);
  av_dict_free(&opts);
  free_buf(&out);
  return rc;
}

/* ---- audio conversion ---------------------------------------------------- */

static int audio_copy_ok(enum AVCodecID id) {
  return id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_AC3 || id == AV_CODEC_ID_EAC3;
}

static void audio_close(session_t *s) {
  avcodec_free_context(&s->adec);
  avcodec_free_context(&s->aenc);
  swr_free(&s->swr);
  if (s->fifo)
    av_audio_fifo_free(s->fifo);
  s->fifo = NULL;
}

static int audio_open_transcoder(session_t *s) {
  AVStream *st = s->in->streams[s->aidx];
  const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
  int surround = st->codecpar->ch_layout.nb_channels > 2;
  const AVCodec *enc = avcodec_find_encoder(surround ? AV_CODEC_ID_AC3 : AV_CODEC_ID_AAC);

  if (!dec || !enc)
    return -1;
  s->adec = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(s->adec, st->codecpar);
  s->adec->pkt_timebase = st->time_base;
  if (avcodec_open2(s->adec, dec, NULL) < 0)
    return -1;

  s->aenc = avcodec_alloc_context3(enc);
  s->aenc->sample_rate = s->adec->sample_rate > 48000 ? 48000 : s->adec->sample_rate;
  if (surround)
    av_channel_layout_from_mask(&s->aenc->ch_layout, AV_CH_LAYOUT_5POINT1);
  else
    av_channel_layout_default(&s->aenc->ch_layout, 2);
  s->aenc->sample_fmt = enc->sample_fmts ? enc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
  s->aenc->bit_rate = surround ? 640000 : 192000;
  s->aenc->time_base = (AVRational){1, s->aenc->sample_rate};
  if (avcodec_open2(s->aenc, enc, NULL) < 0)
    return -1;

  if (swr_alloc_set_opts2(&s->swr, &s->aenc->ch_layout, s->aenc->sample_fmt, s->aenc->sample_rate,
                          &s->adec->ch_layout, s->adec->sample_fmt, s->adec->sample_rate, 0,
                          NULL) < 0 ||
      swr_init(s->swr) < 0)
    return -1;
  s->fifo = av_audio_fifo_alloc(s->aenc->sample_fmt, s->aenc->ch_layout.nb_channels,
                                s->aenc->frame_size * 4);
  s->out_audio_codec = enc->id;
  return s->fifo ? 0 : -1;
}

/* Feeds one source packet (NULL = flush) and appends encoded packets. */
static int audio_transcode(session_t *s, AVPacket *pkt, track_out_t *track, int64_t *next_pts,
                           int64_t **dts_list, int *dts_cap) {
  AVFrame *frame = av_frame_alloc();
  AVFrame *out = av_frame_alloc();
  AVPacket *enc_pkt = av_packet_alloc();
  int rc = 0;

  if (avcodec_send_packet(s->adec, pkt) < 0 && pkt)
    goto done;
  while (avcodec_receive_frame(s->adec, frame) == 0) {
    int out_samples = swr_get_out_samples(s->swr, frame->nb_samples);
    uint8_t **conv = NULL;
    if (*next_pts == AV_NOPTS_VALUE && frame->pts != AV_NOPTS_VALUE)
      *next_pts = av_rescale_q(frame->pts, s->in->streams[s->aidx]->time_base, s->atb_out);
    av_samples_alloc_array_and_samples(&conv, NULL, s->aenc->ch_layout.nb_channels, out_samples,
                                       s->aenc->sample_fmt, 0);
    out_samples = swr_convert(s->swr, conv, out_samples, (const uint8_t **)frame->extended_data,
                              frame->nb_samples);
    if (out_samples > 0)
      av_audio_fifo_write(s->fifo, (void **)conv, out_samples);
    if (conv)
      av_freep(&conv[0]);
    av_freep(&conv);
    av_frame_unref(frame);
  }

  while (av_audio_fifo_size(s->fifo) >= s->aenc->frame_size ||
         (!pkt && av_audio_fifo_size(s->fifo) > 0)) {
    int n = FFMIN(av_audio_fifo_size(s->fifo), s->aenc->frame_size);
    out->nb_samples = s->aenc->frame_size;
    out->format = s->aenc->sample_fmt;
    out->sample_rate = s->aenc->sample_rate;
    av_channel_layout_copy(&out->ch_layout, &s->aenc->ch_layout);
    if (av_frame_get_buffer(out, 0) < 0)
      break;
    av_samples_set_silence(out->extended_data, 0, out->nb_samples, out->ch_layout.nb_channels,
                           out->format);
    av_audio_fifo_read(s->fifo, (void **)out->extended_data, n);
    out->pts = *next_pts;
    *next_pts += out->nb_samples;
    if (avcodec_send_frame(s->aenc, out) == 0) {
      while (avcodec_receive_packet(s->aenc, enc_pkt) == 0) {
        int64_t dts = enc_pkt->pts + (int64_t)TIMESTAMP_SHIFT_SECONDS * s->atb_out.den;
        if (track->count >= *dts_cap) {
          *dts_cap = *dts_cap ? *dts_cap * 2 : 512;
          *dts_list = realloc(*dts_list, (size_t)*dts_cap * sizeof(int64_t));
        }
        (*dts_list)[track->count] = dts;
        track_add(track, enc_pkt->data, enc_pkt->size, dts, dts, enc_pkt->duration, 1);
        av_packet_unref(enc_pkt);
      }
    }
    av_frame_unref(out);
  }

done:
  av_frame_free(&frame);
  av_frame_free(&out);
  av_packet_free(&enc_pkt);
  return rc;
}

/* ---- sessions -------------------------------------------------------------- */

static void pending_clear(session_t *s);

static void session_close_locked(session_t *s) {
  pending_clear(s);
  free(s->pending);
  s->pending = NULL;
  s->pending_cap = 0;
  s->next_index = -1;
  audio_close(s);
  avformat_close_input(&s->in);
  free(s->keyframes);
  free(s->segments);
  free_buf(&s->init);
  s->keyframes = NULL;
  s->segments = NULL;
  s->used = 0;
}

/* Locks a session and confirms it still belongs to this id; another request
 * may have recycled the slot between lookup and lock. */
static int session_lock_owned(session_t *s, const char *id) {
  pthread_mutex_lock(&s->lock);
  if (s->used && !strcmp(s->id, id))
    return 1;
  pthread_mutex_unlock(&s->lock);
  return 0;
}

static session_t *session_find(const char *id) {
  session_t *found = NULL;
  pthread_mutex_lock(&g_table_lock);
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (g_sessions[i].used && !strcmp(g_sessions[i].id, id)) {
      found = &g_sessions[i];
      found->last_access = time(NULL);
      break;
    }
  }
  pthread_mutex_unlock(&g_table_lock);
  return found;
}

static session_t *session_alloc(void) {
  session_t *oldest = NULL;
  time_t now = time(NULL);

  pthread_mutex_lock(&g_table_lock);
  for (int i = 0; i < MAX_SESSIONS; i++) {
    session_t *s = &g_sessions[i];
    if (s->used && now - s->last_access > SESSION_IDLE_SECONDS) {
      pthread_mutex_lock(&s->lock);
      session_close_locked(s);
      pthread_mutex_unlock(&s->lock);
    }
    if (!s->used) {
      oldest = s;
      break;
    }
    if (!oldest || s->last_access < oldest->last_access)
      oldest = s;
  }
  if (oldest->used) {
    pthread_mutex_lock(&oldest->lock);
    session_close_locked(oldest);
    pthread_mutex_unlock(&oldest->lock);
  }
  oldest->used = 1;
  oldest->last_access = now;
  snprintf(oldest->id, sizeof(oldest->id), "%08lx%08lx", (unsigned long)random(),
           (unsigned long)(now ^ (time_t)(uintptr_t)oldest));
  pthread_mutex_unlock(&g_table_lock);
  return oldest;
}

static void pending_clear(session_t *s) {
  for (int i = s->pending_head; i < s->pending_count; i++)
    av_packet_free(&s->pending[i]);
  s->pending_count = s->pending_head = 0;
}

static void pending_push(session_t *s, AVPacket *pkt) {
  if (s->pending_count == s->pending_cap) {
    if (s->pending_head > 0) {
      memmove(s->pending, s->pending + s->pending_head,
              (size_t)(s->pending_count - s->pending_head) * sizeof(AVPacket *));
      s->pending_count -= s->pending_head;
      s->pending_head = 0;
    }
    if (s->pending_count == s->pending_cap) {
      int cap = s->pending_cap ? s->pending_cap * 2 : 256;
      AVPacket **grown = realloc(s->pending, (size_t)cap * sizeof(AVPacket *));
      if (!grown)
        return;
      s->pending = grown;
      s->pending_cap = cap;
    }
  }
  s->pending[s->pending_count++] = av_packet_clone(pkt);
}

/* Moves the oldest carried-over packet into pkt; 0 when none are left. */
static int pending_pop(session_t *s, AVPacket *pkt) {
  if (s->pending_head >= s->pending_count) {
    s->pending_count = s->pending_head = 0;
    return 0;
  }
  AVPacket *next = s->pending[s->pending_head++];
  av_packet_move_ref(pkt, next);
  av_packet_free(&next);
  return 1;
}

static void collect_keyframes(session_t *s) {
  AVStream *st = s->in->streams[s->vidx];
  int n = avformat_index_get_entries_count(st);
  int cap = 0;

  s->keyframe_count = 0;
  for (int i = 0; i < n; i++) {
    const AVIndexEntry *e = avformat_index_get_entry(st, i);
    if (!e || !(e->flags & AVINDEX_KEYFRAME) || e->timestamp == AV_NOPTS_VALUE)
      continue;
    if (s->keyframe_count == cap) {
      cap = cap ? cap * 2 : 1024;
      s->keyframes = realloc(s->keyframes, (size_t)cap * sizeof(keyframe_t));
    }
    keyframe_t *k = &s->keyframes[s->keyframe_count++];
    k->pts = e->timestamp;
    k->seconds = e->timestamp * av_q2d(st->time_base);
  }
}

/* Groups keyframes into segments of about TARGET_SEGMENT_SECONDS. */
static void plan_segments(session_t *s) {
  AVStream *st = s->in->streams[s->vidx];
  int cap = 0;
  int64_t end_pts = (int64_t)(s->duration / av_q2d(st->time_base)) + 1;

  s->segment_count = 0;
  for (int i = 0; i < s->keyframe_count;) {
    int j = i + 1;
    while (j < s->keyframe_count &&
           s->keyframes[j].seconds - s->keyframes[i].seconds < TARGET_SEGMENT_SECONDS)
      j++;
    if (s->segment_count == cap) {
      cap = cap ? cap * 2 : 512;
      s->segments = realloc(s->segments, (size_t)cap * sizeof(segment_t));
    }
    segment_t *seg = &s->segments[s->segment_count++];
    seg->start_pts = s->keyframes[i].pts;
    seg->end_pts = j < s->keyframe_count ? s->keyframes[j].pts : end_pts;
    seg->duration = (seg->end_pts - seg->start_pts) * av_q2d(st->time_base);
    if (seg->duration <= 0)
      seg->duration = 0.001;
    i = j;
  }
}

static void describe_codecs(session_t *s) {
  AVCodecParameters *v = s->in->streams[s->vidx]->codecpar;
  const char *audio = s->aidx < 0 ? NULL
                      : s->out_audio_codec == AV_CODEC_ID_AC3  ? "ac-3"
                      : s->out_audio_codec == AV_CODEC_ID_EAC3 ? "ec-3"
                                                                : "mp4a.40.2";
  const char *video = v->codec_id == AV_CODEC_ID_HEVC ? "hvc1.1.6.L150.B0" : "avc1.640028";
  if (v->codec_id == AV_CODEC_ID_HEVC && v->profile == 2)
    video = "hvc1.2.4.L150.B0";
  snprintf(s->codecs, sizeof(s->codecs), "%s%s%s", video, audio ? "," : "", audio ? audio : "");
}

static char *json_error(int *status, int code, const char *message) {
  char *out = malloc(512);
  *status = code;
  snprintf(out, 512, "{\"returnValue\":false,\"errorText\":\"%s\"}", message);
  return out;
}

static void append_header(char *headers, size_t size, const char *name, const char *value) {
  size_t len = strlen(headers);
  if (!value || !value[0] || strchr(value, '\r') || strchr(value, '\n') || strchr(name, '\r'))
    return;
  snprintf(headers + len, size - len, "%s: %s\r\n", name, value);
}

/* body: {"url": "...", "headers": {"User-Agent": "...", ...}} */
char *nuvio_stream_open(const char *url, const char *const *header_names,
                        const char *const *header_values, int header_count, int *status) {
  AVDictionary *opts = NULL;
  AVPacket *pkt = NULL, *first_video = NULL, *first_audio = NULL;
  AVCodecParameters *apar_out = NULL;
  session_t *s;
  char headers[4096] = "";
  char *reply;
  int rc;

  if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
    return json_error(status, 400, "Only http and https streams can be opened");

  for (int i = 0; i < header_count; i++) {
    if (!strcasecmp(header_names[i], "user-agent"))
      av_dict_set(&opts, "user_agent", header_values[i], 0);
    else if (!strcasecmp(header_names[i], "referer"))
      av_dict_set(&opts, "referer", header_values[i], 0);
    else
      append_header(headers, sizeof(headers), header_names[i], header_values[i]);
  }
  if (headers[0])
    av_dict_set(&opts, "headers", headers, 0);
  av_dict_set(&opts, "reconnect", "1", 0);
  av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
  av_dict_set(&opts, "reconnect_delay_max", "8", 0);
  av_dict_set(&opts, "rw_timeout", "30000000", 0);
  av_dict_set(&opts, "seekable", "1", 0);
  /* Large socket buffer: the default window caps throughput near 1 MB/s at
   * internet latencies. Keep-alive avoids a TLS handshake per seek. */
  av_dict_set(&opts, "recv_buffer_size", "8388608", 0);
  av_dict_set(&opts, "multiple_requests", "1", 0);

  s = session_alloc();
  pthread_mutex_lock(&s->lock);
  s->vidx = s->aidx = -1;
  s->next_index = -1;
  s->next_audio_pts = AV_NOPTS_VALUE;

  rc = avformat_open_input(&s->in, url, NULL, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    char err[128];
    av_strerror(rc, err, sizeof(err));
    nuvio_log("stream: open failed: %s", err);
    reply = json_error(status, 502, "The stream could not be opened");
    goto fail;
  }
  s->in->probesize = 8 * 1024 * 1024;
  s->in->max_analyze_duration = 5 * AV_TIME_BASE;
  if (avformat_find_stream_info(s->in, NULL) < 0) {
    reply = json_error(status, 502, "The stream format could not be read");
    goto fail;
  }

  s->vidx = av_find_best_stream(s->in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  s->aidx = av_find_best_stream(s->in, AVMEDIA_TYPE_AUDIO, -1, s->vidx, NULL, 0);
  if (s->vidx < 0) {
    reply = json_error(status, 415, "The stream has no video");
    goto fail;
  }
  {
    AVCodecParameters *v = s->in->streams[s->vidx]->codecpar;
    const AVPacketSideData *dovi = av_packet_side_data_get(
        v->coded_side_data, v->nb_coded_side_data, AV_PKT_DATA_DOVI_CONF);
    if (v->codec_id != AV_CODEC_ID_H264 && v->codec_id != AV_CODEC_ID_HEVC) {
      nuvio_log("stream: unsupported video codec %s", avcodec_get_name(v->codec_id));
      reply = json_error(status, 415, "This video codec needs the native player");
      goto fail;
    }
    if (dovi && dovi->size >= 4 && (dovi->data[2] >> 1) == 5) {
      reply = json_error(status, 415, "Dolby Vision profile 5 needs the native player");
      goto fail;
    }
  }
  for (unsigned i = 0; i < s->in->nb_streams; i++)
    if ((int)i != s->vidx && (int)i != s->aidx)
      s->in->streams[i]->discard = AVDISCARD_ALL;

  s->duration = s->in->duration > 0 ? s->in->duration / (double)AV_TIME_BASE : 0;

  /* Matroska reads its Cues on the first seek; load them now. */
  avformat_seek_file(s->in, s->vidx, INT64_MIN, 0, 0, 0);
  collect_keyframes(s);
  if (s->keyframe_count < 2 || s->duration <= 0) {
    nuvio_log("stream: no keyframe index (%d entries)", s->keyframe_count);
    reply = json_error(status, 422, "This file has no seek index; use the native player");
    goto fail;
  }
  plan_segments(s);

  if (s->aidx >= 0) {
    AVCodecParameters *a = s->in->streams[s->aidx]->codecpar;
    s->transcode_audio = !audio_copy_ok(a->codec_id);
    if (s->transcode_audio) {
      if (audio_open_transcoder(s) < 0) {
        nuvio_log("stream: audio %s cannot be converted; video only",
                  avcodec_get_name(a->codec_id));
        audio_close(s);
        s->aidx = -1;
      } else {
        apar_out = avcodec_parameters_alloc();
        avcodec_parameters_from_context(apar_out, s->aenc);
      }
    } else {
      s->out_audio_codec = a->codec_id;
      apar_out = avcodec_parameters_alloc();
      avcodec_parameters_copy(apar_out, a);
    }
  }

  /* First video keyframe and audio packet, for the init segment. */
  avformat_seek_file(s->in, s->vidx, INT64_MIN, s->segments[0].start_pts,
                     s->segments[0].start_pts, 0);
  pkt = av_packet_alloc();
  for (int reads = 0; reads < 2000 && (!first_video || (s->aidx >= 0 && !first_audio)); reads++) {
    if (av_read_frame(s->in, pkt) < 0)
      break;
    if (pkt->stream_index == s->vidx && !first_video && (pkt->flags & AV_PKT_FLAG_KEY))
      first_video = av_packet_clone(pkt);
    else if (pkt->stream_index == s->aidx && !first_audio && !s->transcode_audio)
      first_audio = av_packet_clone(pkt);
    av_packet_unref(pkt);
  }
  if (s->transcode_audio && s->aidx >= 0) {
    /* The encoder's own first packet describes the output stream. */
    AVFrame *silence = av_frame_alloc();
    AVPacket *enc = av_packet_alloc();
    silence->nb_samples = s->aenc->frame_size;
    silence->format = s->aenc->sample_fmt;
    silence->sample_rate = s->aenc->sample_rate;
    av_channel_layout_copy(&silence->ch_layout, &s->aenc->ch_layout);
    av_frame_get_buffer(silence, 0);
    av_samples_set_silence(silence->extended_data, 0, silence->nb_samples,
                           silence->ch_layout.nb_channels, silence->format);
    silence->pts = 0;
    avcodec_send_frame(s->aenc, silence);
    avcodec_send_frame(s->aenc, NULL);
    if (avcodec_receive_packet(s->aenc, enc) == 0)
      first_audio = av_packet_clone(enc);
    av_frame_free(&silence);
    av_packet_free(&enc);
    /* Flushed encoders cannot be reused: reopen for real audio. */
    avcodec_free_context(&s->aenc);
    audio_close(s);
    audio_open_transcoder(s);
  }
  if (!first_video ||
      build_init(s, apar_out, first_video, first_audio,
                 s->transcode_audio || s->aidx < 0 ? (AVRational){1, apar_out ? apar_out->sample_rate : 1}
                                                   : s->in->streams[s->aidx]->time_base) != 0) {
    reply = json_error(status, 500, "The stream could not be repackaged");
    goto fail;
  }
  describe_codecs(s);

  reply = malloc(1024);
  *status = 200;
  snprintf(reply, 1024,
           "{\"returnValue\":true,\"id\":\"%s\",\"playlist\":\"/stream/%s/index.m3u8\","
           "\"duration\":%.3f,\"segments\":%d,\"video\":\"%s\",\"audio\":\"%s\","
           "\"audioConverted\":%s,\"codecs\":\"%s\"}",
           s->id, s->id, s->duration, s->segment_count,
           avcodec_get_name(s->in->streams[s->vidx]->codecpar->codec_id),
           s->aidx >= 0 ? avcodec_get_name(s->in->streams[s->aidx]->codecpar->codec_id) : "none",
           s->transcode_audio ? "true" : "false", s->codecs);
  nuvio_log("stream: session %s %.0fs %d segments video=%s audio=%s%s", s->id, s->duration,
            s->segment_count, avcodec_get_name(s->in->streams[s->vidx]->codecpar->codec_id),
            s->aidx >= 0 ? avcodec_get_name(s->in->streams[s->aidx]->codecpar->codec_id) : "none",
            s->transcode_audio ? " (converted)" : "");
  av_packet_free(&pkt);
  av_packet_free(&first_video);
  av_packet_free(&first_audio);
  avcodec_parameters_free(&apar_out);
  pthread_mutex_unlock(&s->lock);
  return reply;

fail:
  av_packet_free(&pkt);
  av_packet_free(&first_video);
  av_packet_free(&first_audio);
  avcodec_parameters_free(&apar_out);
  session_close_locked(s);
  pthread_mutex_unlock(&s->lock);
  return reply;
}

char *nuvio_stream_playlist(const char *id, size_t *len) {
  session_t *s = session_find(id);
  buf_t out = {0};
  char line[160];
  double longest = 0;

  if (!s)
    return NULL;
  if (!session_lock_owned(s, id))
    return NULL;
  for (int i = 0; i < s->segment_count; i++)
    if (s->segments[i].duration > longest)
      longest = s->segments[i].duration;
  snprintf(line, sizeof(line),
           "#EXTM3U\n#EXT-X-VERSION:7\n#EXT-X-TARGETDURATION:%d\n#EXT-X-PLAYLIST-TYPE:VOD\n"
           "#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-INDEPENDENT-SEGMENTS\n#EXT-X-MAP:URI=\"init.mp4\"\n",
           (int)(longest + 0.999));
  buf_put(&out, line, strlen(line));
  for (int i = 0; i < s->segment_count; i++) {
    int n = snprintf(line, sizeof(line), "#EXTINF:%.6f,\n%d.m4s\n", s->segments[i].duration, i);
    buf_put(&out, line, (size_t)n);
  }
  buf_put(&out, "#EXT-X-ENDLIST\n", 15);
  pthread_mutex_unlock(&s->lock);
  *len = out.len;
  return (char *)out.data;
}

uint8_t *nuvio_stream_init(const char *id, size_t *len) {
  session_t *s = session_find(id);
  uint8_t *copy = NULL;
  if (!s)
    return NULL;
  if (!session_lock_owned(s, id))
    return NULL;
  if (s->init.len && (copy = malloc(s->init.len))) {
    memcpy(copy, s->init.data, s->init.len);
    *len = s->init.len;
  }
  pthread_mutex_unlock(&s->lock);
  return copy;
}

static void write_traf(buf_t *b, uint32_t track_id, const track_out_t *t, size_t *data_offset_at,
                       int with_cto) {
  size_t traf = box_begin(b, "traf");
  size_t tfhd = box_begin(b, "tfhd");
  put32(b, 0x00020000); /* default-base-is-moof */
  put32(b, track_id);
  box_end(b, tfhd);
  size_t tfdt = box_begin(b, "tfdt");
  put32(b, 0x01000000);
  put64(b, (uint64_t)t->first_dts);
  box_end(b, tfdt);
  size_t trun = box_begin(b, "trun");
  uint32_t flags = 0x000001 | 0x000100 | 0x000200 | 0x000400 | (with_cto ? 0x000800 : 0);
  put32(b, (with_cto ? 0x01000000u : 0) | flags);
  put32(b, (uint32_t)t->count);
  *data_offset_at = b->len;
  put32(b, 0);
  for (int i = 0; i < t->count; i++) {
    put32(b, t->samples[i].duration);
    put32(b, t->samples[i].size);
    put32(b, t->samples[i].flags);
    if (with_cto)
      put32(b, (uint32_t)t->samples[i].cto);
  }
  box_end(b, trun);
  box_end(b, traf);
}

uint8_t *nuvio_stream_segment(const char *id, int index, size_t *len) {
  session_t *s = session_find(id);
  track_out_t video = {0}, audio = {0};
  int64_t *vdts = NULL, *adts = NULL;
  int vdts_cap = 0, adts_cap = 0;
  AVPacket *pkt = NULL;
  buf_t out = {0};
  uint8_t *result = NULL;

  if (!s)
    return NULL;
  if (!session_lock_owned(s, id))
    return NULL;
  if (index < 0 || index >= s->segment_count || !s->in)
    goto done;

  {
    const segment_t *seg = &s->segments[index];
    AVStream *vst = s->in->streams[s->vidx];
    AVStream *ast = s->aidx >= 0 ? s->in->streams[s->aidx] : NULL;
    int64_t vshift = (int64_t)TIMESTAMP_SHIFT_SECONDS * s->vtb_out.den / s->vtb_out.num;
    int64_t ashift = ast ? (int64_t)TIMESTAMP_SHIFT_SECONDS * s->atb_out.den / s->atb_out.num : 0;
    double seg_start = seg->start_pts * av_q2d(vst->time_base);
    double seg_end = seg->end_pts * av_q2d(vst->time_base);
    int in_segment = 0, video_done = 0;
    double video_done_at = 0;
    /* hls.js asks for segments in order; keep reading from where the last
     * one stopped (with its carried-over packets) and only seek on jumps. */
    int sequential = index == s->next_index && s->pending_count > 0;

    double t_begin = now_seconds(), t_seeked;
    int64_t bytes_before;
    if (!sequential) {
      pending_clear(s);
      if (avformat_seek_file(s->in, s->vidx, INT64_MIN, seg->start_pts, seg->start_pts, 0) < 0) {
        nuvio_log("stream: seek to segment %d failed", index);
        goto done;
      }
      if (s->transcode_audio) {
        avcodec_flush_buffers(s->adec);
        av_audio_fifo_reset(s->fifo);
        swr_init(s->swr);
      }
      s->next_audio_pts = AV_NOPTS_VALUE;
    }
    s->next_index = -1;

    t_seeked = now_seconds();
    bytes_before = avio_tell(s->in->pb);
    pkt = av_packet_alloc();
    for (;;) {
      if (!pending_pop(s, pkt) && av_read_frame(s->in, pkt) < 0)
        break;
      if (pkt->stream_index == s->vidx) {
        int key = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
        if (!in_segment) {
          /* Start at this segment's keyframe (the seek may land earlier). */
          if (key && pkt->pts != AV_NOPTS_VALUE && pkt->pts >= seg->start_pts)
            in_segment = 1;
          else {
            av_packet_unref(pkt);
            continue;
          }
        }
        if (!video_done && key && pkt->pts >= seg->end_pts) {
          video_done = 1;
          video_done_at = pkt->pts * av_q2d(vst->time_base);
        }
        if (video_done) {
          pending_push(s, pkt); /* first packets of the next segment */
        } else {
          int64_t src_pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
          int64_t pts = av_rescale_q(src_pts, vst->time_base, s->vtb_out) + vshift;
          int64_t dts = pkt->dts == AV_NOPTS_VALUE
                            ? pts
                            : av_rescale_q(pkt->dts, vst->time_base, s->vtb_out) + vshift;
          if (video.count >= vdts_cap) {
            vdts_cap = vdts_cap ? vdts_cap * 2 : 512;
            vdts = realloc(vdts, (size_t)vdts_cap * sizeof(int64_t));
          }
          vdts[video.count] = dts;
          track_add(&video, pkt->data, pkt->size, dts, pts,
                    av_rescale_q(pkt->duration, vst->time_base, s->vtb_out), key);
        }
      } else if (ast && pkt->stream_index == s->aidx && pkt->pts != AV_NOPTS_VALUE) {
        double t = pkt->pts * av_q2d(ast->time_base);
        if (t >= seg_end) {
          pending_push(s, pkt);
        } else if (t >= seg_start) {
          if (s->transcode_audio) {
            audio_transcode(s, pkt, &audio, &s->next_audio_pts, &adts, &adts_cap);
          } else {
            int64_t apts = av_rescale_q(pkt->pts, ast->time_base, s->atb_out) + ashift;
            if (audio.count >= adts_cap) {
              adts_cap = adts_cap ? adts_cap * 2 : 512;
              adts = realloc(adts, (size_t)adts_cap * sizeof(int64_t));
            }
            adts[audio.count] = apts;
            track_add(&audio, pkt->data, pkt->size, apts, apts,
                      av_rescale_q(pkt->duration, ast->time_base, s->atb_out), 1);
          }
        }
      }
      {
        /* Stop once video is complete and audio has reached the segment end
         * (or the tail allowance for interleaving has passed). */
        AVStream *pst = s->in->streams[pkt->stream_index];
        double t = pkt->pts == AV_NOPTS_VALUE ? 0 : pkt->pts * av_q2d(pst->time_base);
        int stop = video_done && (!ast || (pkt->stream_index == s->aidx && t >= seg_end) ||
                                  t > video_done_at + AUDIO_TAIL_SECONDS);
        av_packet_unref(pkt);
        if (stop)
          break;
      }
    }
    if (video_done)
      s->next_index = index + 1;
    else if (s->transcode_audio && ast)
      audio_transcode(s, NULL, &audio, &s->next_audio_pts, &adts, &adts_cap); /* end of file */

    nuvio_log("stream: segment %d %s %.2fs read %.2fs %.1f MB (%.1f MB/s) v=%d a=%d", index,
              sequential ? "continued" : "seek",
              t_seeked - t_begin, now_seconds() - t_seeked,
              (avio_tell(s->in->pb) - bytes_before) / 1e6,
              (avio_tell(s->in->pb) - bytes_before) / 1e6 / (now_seconds() - t_seeked + 1e-6),
              video.count, audio.count);
    if (!video.count) {
      nuvio_log("stream: segment %d produced no video", index);
      goto done;
    }
    track_fix_durations(&video, vdts);
    if (audio.count)
      track_fix_durations(&audio, adts);

    /* moof + mdat. Video data first, then audio, in one mdat. */
    {
      size_t moof = box_begin(&out, "moof");
      size_t mfhd = box_begin(&out, "mfhd");
      size_t voff_at = 0, aoff_at = 0;
      put32(&out, 0);
      put32(&out, (uint32_t)index + 1);
      box_end(&out, mfhd);
      write_traf(&out, 1, &video, &voff_at, 1);
      if (audio.count)
        write_traf(&out, 2, &audio, &aoff_at, 0);
      box_end(&out, moof);
      size_t moof_size = out.len - moof;
      patch32(&out, voff_at, (uint32_t)(moof_size + 8));
      if (audio.count)
        patch32(&out, aoff_at, (uint32_t)(moof_size + 8 + video.data.len));
      put32(&out, (uint32_t)(8 + video.data.len + audio.data.len));
      buf_put(&out, "mdat", 4);
      buf_put(&out, video.data.data, video.data.len);
      if (audio.count)
        buf_put(&out, audio.data.data, audio.data.len);
    }
    result = out.data;
    *len = out.len;
    out.data = NULL;
  }

done:
  pthread_mutex_unlock(&s->lock);
  av_packet_free(&pkt);
  track_reset(&video);
  track_reset(&audio);
  free(vdts);
  free(adts);
  free_buf(&out);
  return result;
}

void nuvio_stream_close(const char *id) {
  session_t *s = session_find(id);
  if (!s || !session_lock_owned(s, id))
    return;
  session_close_locked(s);
  pthread_mutex_unlock(&s->lock);
}

void nuvio_stream_init_module(void) {
  for (int i = 0; i < MAX_SESSIONS; i++)
    pthread_mutex_init(&g_sessions[i].lock, NULL);
  srandom((unsigned)time(NULL));
  avformat_network_init();
}
