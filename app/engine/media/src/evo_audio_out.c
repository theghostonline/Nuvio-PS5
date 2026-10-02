/*
 * evo_audio_out.c — media audio output path: decode → resample → AudioOut.
 *
 * Verbatim move of the audio-out ring / output thread, the audio sample mix,
 * the audio decode thread, and the AUDIO_TRACK_SWITCH region from main.c
 * (Track A step A3 of docs/modularisation-plan.md). No behaviour change: the
 * only edits are `static` → external linkage on the state main.c still touches
 * and the transitional extern block below for the playback-core / subtitle /
 * resume globals this code reads. All of that collapses into the evo_pb_*()
 * façade at A8.
 */
#include "evo_audio_out.h"

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "evo_packet_queue.h"
#include "evo_audio_resample.h"
#include "evo_adec.h"
#include "pp_playback.h"
#include "evo_boot_log.h"
#include "evo_speaker_cal.h"

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

int sceAudioOutOutput(int handle, const void *ptr);

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core, subtitle and resume state still owned by
 * main.c. Replaced by evo_pb_*() / evo_subtitle_*() accessors at A8 / A4.
 * ------------------------------------------------------------------------ */
extern int               screen;
extern pp_playback       g_pp_pb;
extern int               player_paused;
/* Start-of-stream pre-buffer hold - see the note in Bridge.cpp. */
extern volatile int      pb_prebuffer_hold;
extern volatile int      pb_scrub_hold;
extern double            video_clock_seconds;
extern double            first_video_pts_seconds;
extern int               video_stream_index;
extern int               video_decode_ready;
extern int               video_decode_done;
extern int               audio_stream_index;
extern int               evo_audio_channels;
extern AVFormatContext  *play_fmt;
extern AVCodecContext   *audio_ctx;
extern PacketQueue       audio_packet_queue;
extern double            resume_base_offset_seconds;
extern double            media_duration_sec;
extern double            requested_resume_seek_pos;
extern long long         controls_last_used_ms;
extern int               prospero_subtitle_enabled;
extern int               prospero_subtitle_use_external;
extern int               prospero_subtitle_count;
extern int               prospero_embedded_subtitle_stream_index;
extern int               prospero_subtitle_requested_stream;
extern char              current_media_path[512];

void     toast(const char *title, const char *msg);
long long now_ms(void);

/* ---------------------------------------------------------------------------
 * Audio-out session state (exported via evo_audio_out.h).
 * ------------------------------------------------------------------------ */
int audio_handle = -1;
static evo_pcm_t audio_accum[2048 * EVO_AUDIO_MAX_CH];
int audio_accum_pos = 0;

static evo_pcm_t audio_queue[AUDIO_QUEUE_BLOCKS][AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
int evo_audio_port_float = 1;
volatile int audio_queue_read = 0;
volatile int audio_queue_write = 0;
volatile int audio_queue_count = 0;
volatile long long audio_samples_played = 0;
volatile long long audio_samples_decoded = 0;
volatile double audio_clock_seconds = 0.0;
volatile double audio_pts_seconds = 0.0;
double first_audio_pts_seconds = -1.0;
/* Set by the resume path (PlaybackController::open): that seek lands on the
 * keyframe before the saved position, not on it, and nothing drops the run-up
 * the way the in-place seek's gates do. The audio clock counts from the first
 * decoded frame, so that frame's PTS is where playback really restarted -
 * re-anchor the base to it, or every readout (OSD, subtitle cues) runs ahead
 * by the keyframe gap. Holds the resume position the base was seeded with. */
volatile double resume_base_anchor_pending = -1.0;

static void anchor_resume_base(void)
{
    const double want = resume_base_anchor_pending;
    if (want < 0.0)
        return;
    resume_base_anchor_pending = -1.0;
    const double landed = first_audio_pts_seconds;
    /* A keyframe lands at or a few seconds before the resume point; anything
     * else is a timestamp origin we don't understand - keep the seeded base. */
    if (landed > want + 0.5 || landed < want - 30.0) {
        evo_boot_log("resume: first audio at %.3fs, resume point %.3fs - keeping base",
                     landed, want);
        return;
    }
    resume_base_offset_seconds = landed;
    evo_boot_log("resume: re-anchored base %.3fs -> %.3fs (keyframe run-up %.2fs)",
                 want, landed, want - landed);
}
volatile double audio_seek_discard_until = -1.0;

int detected_audio_rate = 48000;
volatile int audio_thread_running = 0;
pthread_t audio_thread;

volatile int audio_decode_thread_running = 0;
pthread_t audio_decode_thread;


static void audio_queue_push(evo_pcm_t *buf) {
    int spins = 0;
    /* Wait for space instead of dropping — drops cause choppy audio on UHD */
    while (audio_queue_count >= AUDIO_QUEUE_BLOCKS &&
           audio_decode_thread_running && spins < 200) {
        usleep(500);
        spins++;
    }
    if (audio_queue_count >= AUDIO_QUEUE_BLOCKS)
        return;

    memcpy(audio_queue[audio_queue_write], buf,
           (size_t)AUDIO_BLOCK_SAMPLES * evo_audio_channels * sizeof(evo_pcm_t));
    audio_queue_write = (audio_queue_write + 1) % AUDIO_QUEUE_BLOCKS;
    audio_queue_count++;
}

void *audio_output_thread(void *arg) {
    static evo_pcm_t silence[AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
    static int16_t s16_block[AUDIO_BLOCK_SAMPLES * EVO_AUDIO_MAX_CH];
    /*
     * State for the "video is not advancing" escape on the throttle below.
     * Video's own audio-master wait has the mirror of this (stuck_iters in
     * decode_next_video_frame); without it here, the two waits can hold each
     * other forever.
     */
    double video_rel_at_wait = -1.0;
    int    video_stuck_iters = 0;
    while (audio_thread_running) {
        /*
         * Hold output while a seek is still discarding video.
         *
         * The audio side is gated to the seek target, so it is ready to play
         * the instant the seek lands - but video still has to decode from the
         * keyframe to that target, which on a long-GOP 4K stream is nearly two
         * seconds. Letting audio run through that put it 0.9 s ahead by the
         * time the picture came back, which read as "video is late", tripped
         * the badly-late branch in decode_next_video_frame, and set it draining
         * queued packets every frame - so video could never catch up and
         * playback crawled at a few frames a second. Starting both at the
         * target together is the whole point of the gate; this is the other
         * half of it.
         */
        if ((g_pp_pb.active && g_pp_pb.seek_discarding) || pb_scrub_hold) {
            /* The clocks restart at the seek target; so does the stall
             * detector, so a pre-seek reading cannot leak past the seek.
             * pb_scrub_hold parks output for the same reason it parks the two
             * decode threads: while the seek bar is being dragged the picture
             * is frozen, so audio running on would only build a lead it has to
             * give back the moment the scrub commits. */
            video_rel_at_wait = -1.0;
            video_stuck_iters = 0;
            usleep(2000);
            continue;
        }
        if (screen == 2 && !player_paused && audio_handle >= 1) {
            /*
             * Compare like-for-like: audio_clock is from t=0 of this session,
             * video must be relative to first video PTS (not absolute PTS).
             * Absolute compare freezes easy 720p/YouTube when clocks diverge.
             */
            /*
             * No anchor yet means nothing has been presented since the last
             * open or seek, so there is no video position to hold audio back
             * against. Treating it as 0 disables the throttle below instead of
             * comparing the audio clock with a stale absolute PTS.
             */
            double video_rel = 0.0;
            if (first_video_pts_seconds >= 0.0)
                video_rel = video_clock_seconds - first_video_pts_seconds;
            if (video_rel < 0.0)
                video_rel = 0.0;

            if (video_stream_index >= 0 && first_video_pts_seconds < 0.0 &&
                video_decode_ready && !video_decode_done) {
                usleep(2000);
                continue;
            }

            /*
             * Hold audio back when it gets more than half a second ahead of
             * the picture - but only while the picture is actually moving.
             *
             * This throttle waits on video, and video's audio-master wait in
             * decode_next_video_frame() waits on audio. On hardware, after a
             * seek into 4K HEVC + E-AC-3, that pair latched (2026-09-28):
             * audio parked here, its decoded-PCM ring stayed full, the audio
             * decode thread stopped popping, the audio packet queue capped
             * out, the demux thread parked on it, the video packet queue
             * drained, video stopped decoding - so video_rel froze and this
             * test stayed true forever. video_frames=0 for two minutes.
             *
             * demux_wait_for_room() in evo_demux.c cuts that cycle at the
             * demux end; this cuts it here too. ~250 ms with no video progress
             * and audio plays regardless, exactly as video presents regardless
             * after ~120 ms with no audio progress.
             *
             * Any movement counts, in either direction: a seek re-anchors
             * first_video_pts_seconds, so video_rel drops back to ~0 and a
             * forward-only test would never see it move again - leaving the
             * throttle switched off for the rest of the file.
             */
            if (video_rel < video_rel_at_wait - 0.0005 ||
                video_rel > video_rel_at_wait + 0.0005) {
                video_rel_at_wait = video_rel;
                video_stuck_iters = 0;
            }
            if (video_rel >= EVO_AV_SYNC_SETTLE_SEC &&
                audio_clock_seconds > video_rel + 0.50 &&
                video_stuck_iters < 125) {
                video_stuck_iters++;
                usleep(2000);
                continue;
            }
            if (audio_queue_count > 0) {
                const evo_pcm_t *blk = audio_queue[audio_queue_read];
                if (evo_audio_port_float) {
                    sceAudioOutOutput(audio_handle, blk);
                } else {
                    /* S16 port fallback: convert this block. 32768 is the
                     * inverse of the decoders' s16 -> float scale, so 16-bit
                     * sources come back bit for bit; rounded, not truncated. */
                    const int n = AUDIO_BLOCK_SAMPLES * evo_audio_channels;
                    for (int i = 0; i < n; i++) {
                        const float v = blk[i] * 32768.0f;
                        s16_block[i] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768
                                     : (int16_t)(v + (v >= 0.0f ? 0.5f : -0.5f));
                    }
                    sceAudioOutOutput(audio_handle, s16_block);
                }
                audio_samples_played += AUDIO_BLOCK_SAMPLES;
                audio_clock_seconds = (double)audio_samples_played / 48000.0;
                audio_queue_read = (audio_queue_read + 1) % AUDIO_QUEUE_BLOCKS;
                audio_queue_count--;
            } else {
                /*
                 * Soft underrun: silence without advancing media clock.
                 * Video wait loop must break if audio stays stuck (see decode).
                 */
                sceAudioOutOutput(audio_handle, silence);
            }
        } else {
            usleep(200);
        }
    }
    return NULL;
}

static float audio_get_sample(AVFrame *af, int ch, int i) {
    int channels = af->ch_layout.nb_channels;
    if (channels <= 0) channels = 2;

    if (ch >= channels) ch = channels - 1;
    if (ch < 0) ch = 0;

    if (af->format == AV_SAMPLE_FMT_FLTP) {
        float *d = (float*)af->data[ch];
        return d ? d[i] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_FLT) {
        float *d = (float*)af->data[0];
        return d ? d[i * channels + ch] : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16P) {
        int16_t *d = (int16_t*)af->data[ch];
        return d ? ((float)d[i] / 32768.0f) : 0.0f;
    }

    if (af->format == AV_SAMPLE_FMT_S16) {
        int16_t *d = (int16_t*)af->data[0];
        return d ? ((float)d[i * channels + ch] / 32768.0f) : 0.0f;
    }

    return 0.0f;
}

static int16_t audio_float_to_s16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 30000.0f);
}

static void mix_audio_frame_to_queue(
    AVFrame *frame
) {
    if (
        !frame ||
        frame->nb_samples <= 0
    ) {
        return;
    }

    if (
        !prospero_audio_resampler_configure(
            frame,
            evo_audio_channels,
            audio_ctx
        )
    ) {
        return;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : prospero_audio_resampler_input_rate();

    int64_t delay =
        swr_get_delay(
            prospero_audio_resampler_ctx(),
            input_rate
        );

    int output_capacity =
        (int)av_rescale_rnd(
            delay + frame->nb_samples,
            PROSPERO_AUDIO_OUTPUT_RATE,
            input_rate,
            AV_ROUND_UP
        );

    if (output_capacity <= 0) {
        return;
    }

    /*
     * Grow-only scratch instead of av_samples_alloc/av_freep per packet. This
     * runs on every audio packet - about 43 a second for AAC - and the
     * capacity only ever changes when the stream layout does, so the malloc
     * and free were pure churn on the decode thread.
     */
    static uint8_t *output_buffer;
    static size_t   output_buffer_cap;

    size_t need = (size_t)output_capacity * (size_t)evo_audio_channels *
                  sizeof(evo_pcm_t);
    if (need > output_buffer_cap) {
        uint8_t *grown = (uint8_t *)av_realloc(output_buffer, need);
        if (!grown) {
            return;
        }
        output_buffer = grown;
        output_buffer_cap = need;
    }

    uint8_t *output_planes[1] = {
        output_buffer
    };

    int converted =
        swr_convert(
            prospero_audio_resampler_ctx(),
            output_planes,
            output_capacity,
            (const uint8_t * const *)
                frame->extended_data,
            frame->nb_samples
        );

    if (converted > 0) {
        const evo_pcm_t *samples = (const evo_pcm_t *)output_buffer;
        const int ch = evo_audio_channels;
        int index = 0;

        /*
         * Both buffers are interleaved with the same channel count, so this is
         * a straight copy - it just has to stop at each accumulator block
         * boundary to hand the block off. It used to run sample by sample and
         * channel by channel, re-reading the volatile thread flag on every
         * iteration, which also stopped the compiler vectorising it. Copy in
         * runs instead and check the flag once per run.
         */
        while (index < converted && audio_decode_thread_running) {
            int run = converted - index;
            const int space = AUDIO_BLOCK_SAMPLES - audio_accum_pos;
            if (run > space)
                run = space;

            memcpy(&audio_accum[(size_t)audio_accum_pos * ch],
                   &samples[(size_t)index * ch],
                   (size_t)run * (size_t)ch * sizeof(evo_pcm_t));

            audio_accum_pos += run;
            audio_samples_decoded += run;
            index += run;

            if (audio_accum_pos >= AUDIO_BLOCK_SAMPLES) {
                while (
                    audio_decode_thread_running &&
                    audio_queue_count >=
                        AUDIO_QUEUE_BLOCKS - 2
                ) {
                    usleep(1000);
                }

                if (!audio_decode_thread_running) {
                    break;
                }

                audio_queue_push(audio_accum);
                audio_accum_pos = 0;
            }
        }
    }

}


/* The live native audio decoder, or NULL when this stream is on FFmpeg.
 * Owned by the playback controller; read only on the decode thread. */
evo_adec *g_adec = NULL;
/* One AAC frame is at most 2048 samples per channel; 64 KB covers stereo S16
 * with room to spare and matches the capacity the reference PoC uses. */
static int16_t g_adec_pcm[32 * 1024];

void *audio_decode_thread_func(void *arg) {
    AVFrame *af = av_frame_alloc();
    if (!af) return NULL;

    while (audio_decode_thread_running) {
        /*
         * pb_prebuffer_hold parks audio alongside video at open. It has to be
         * both or neither: audio is the master clock, so letting it run while
         * video is held would advance the clock against frames that have not
         * been decoded yet, and every one of them would arrive late.
         */
        if (player_paused || pb_prebuffer_hold || pb_scrub_hold || screen != 2) {
            /* Parked - see the note on the video decode thread. */
            usleep(5000);
            continue;
        }

        while (audio_decode_thread_running && audio_queue_count > 10) {
            usleep(1000);
        }

        AVPacket *pkt = packet_queue_pop(&audio_packet_queue);
        if (pkt && audio_seek_discard_until >= 0.0) {
            /*
             * Seek discard window. av_seek_frame lands on the keyframe at or
             * before the target, so everything up to the target is run-up the
             * viewer has already heard. The video side drops those frames in
             * pp_playback_push_frame; drop the matching audio here so both
             * clocks restart from the target together. Audio packets are
             * independently decodable and the decoder was just flushed, so the
             * first kept packet primes it exactly as a fresh open would.
             * A packet with no PTS clears the gate rather than being dropped.
             */
            double pkt_seconds = -1.0;
            if (pkt->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0)
                pkt_seconds = (double)pkt->pts *
                    av_q2d(play_fmt->streams[audio_stream_index]->time_base);

            /*
             * 25 ms of slack keeps the packet that straddles the target.
             * In MPEG-TS, PES packets carry PTS only every few packets; intermediate
             * packets have AV_NOPTS_VALUE. Discard them while the seek gate is active
             * rather than prematurely clearing the gate.
             * The cap is the safety net for a container whose audio PTS does
             * not share an origin with the seek target (a non-zero
             * start_time): rather than mute the track, give up on the gate
             * after a GOP's worth of packets and let everything through.
             */
            static double s_last_discard_target = -1.0;
            static int s_dropped = 0;
            if (audio_seek_discard_until != s_last_discard_target) {
                s_last_discard_target = audio_seek_discard_until;
                s_dropped = 0;
            }

            if (pkt_seconds >= 0.0) {
                if (pkt_seconds < audio_seek_discard_until - 0.025 &&
                    s_dropped < 2000) {
                    s_dropped++;
                    av_packet_free(&pkt);
                    continue;
                }
                s_dropped = 0;
                s_last_discard_target = -1.0;
                audio_seek_discard_until = -1.0;
            } else {
                if (s_dropped < 2000) {
                    s_dropped++;
                    av_packet_free(&pkt);
                    continue;
                }
                s_dropped = 0;
                s_last_discard_target = -1.0;
                audio_seek_discard_until = -1.0;
            }
        }
        if (!pkt) {
            /*
             * Do not terminate at EOF. The demux thread stays alive
             * so an in-place backward seek can refill this queue.
             */
            usleep(
                video_decode_done
                    ? 5000
                    : 1000
            );

            continue;
        }

        /*
         * Native decode (libSceAudiodec, AAC/MP3) produces interleaved S16 at
         * the stream rate. Wrap it in an AVFrame and hand it to the same
         * mix_audio_frame_to_queue() the FFmpeg path uses, so resampling to the
         * 48 kHz port, channel mapping and the accumulator are all unchanged.
         * g_adec is NULL for every other codec and after a native fatal, which
         * is what makes the fallback a one-line branch rather than a mode.
         */
        int handled_natively = 0;
        if (g_adec && evo_adec_active(g_adec) == EVO_ADEC_BACKEND_NATIVE) {
            int produced = evo_adec_decode(g_adec, pkt->data, pkt->size,
                                           g_adec_pcm, (int)sizeof(g_adec_pcm));
            if (produced < 0) {
                /* Latched fatal: drop to FFmpeg for the rest of the session. */
                evo_adec_close(g_adec);
                g_adec = NULL;
            } else {
                handled_natively = 1;
                int ch = evo_adec_channels(g_adec);
                int rate = evo_adec_rate(g_adec);
                if (produced > 0 && ch > 0 && rate > 0) {
                    av_frame_unref(af);
                    af->format = AV_SAMPLE_FMT_S16;
                    af->sample_rate = rate;
                    av_channel_layout_default(&af->ch_layout, ch);
                    af->nb_samples = produced / (int)sizeof(int16_t) / ch;
                    if (af->nb_samples > 0 &&
                        av_frame_get_buffer(af, 0) == 0) {
                        memcpy(af->data[0], g_adec_pcm, (size_t)produced);
                        if (pkt->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                            audio_pts_seconds = pkt->pts *
                                av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                            if (first_audio_pts_seconds < 0.0) {
                                first_audio_pts_seconds = audio_pts_seconds;
                                anchor_resume_base();
                            }
                        }
                        mix_audio_frame_to_queue(af);
                    }
                    av_frame_unref(af);
                }
            }
        }

        if (!handled_natively && avcodec_send_packet(audio_ctx, pkt) == 0) {
            while (audio_decode_thread_running && avcodec_receive_frame(audio_ctx, af) == 0) {
                if (af->pts != AV_NOPTS_VALUE && play_fmt && audio_stream_index >= 0) {
                    audio_pts_seconds = af->pts * av_q2d(play_fmt->streams[audio_stream_index]->time_base);
                    if (first_audio_pts_seconds < 0.0) {
                        first_audio_pts_seconds = audio_pts_seconds;
                        anchor_resume_base();
                    }
                }

                mix_audio_frame_to_queue(af);
                av_frame_unref(af);
            }
        }

        av_packet_free(&pkt);
    }

    av_frame_free(&af);
    return NULL;
}


/* PROSPERO_AUDIO_TRACK_SWITCH_START */

#define PROSPERO_AUDIO_TRACK_LIMIT 32

int prospero_audio_requested_stream = -1;
char prospero_audio_active_label[128] = {0};


static int prospero_audio_collect_streams(
    AVFormatContext *format,
    int *indexes,
    int maximum
) {
    if (!format || !indexes || maximum <= 0) {
        return 0;
    }

    int count = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams &&
        count < maximum;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_AUDIO
        ) {
            continue;
        }

        if (
            !avcodec_find_decoder(
                stream->codecpar->codec_id
            )
        ) {
            continue;
        }

        indexes[count++] =
            (int)index;
    }

    return count;
}


void prospero_audio_build_label(
    AVFormatContext *format,
    int selected_stream,
    char *output,
    size_t output_size
) {
    if (!output || output_size == 0) {
        return;
    }

    snprintf(
        output,
        output_size,
        "UNKNOWN AUDIO"
    );

    if (
        !format ||
        selected_stream < 0 ||
        selected_stream >=
            (int)format->nb_streams
    ) {
        return;
    }

    int indexes[
        PROSPERO_AUDIO_TRACK_LIMIT
    ];

    int count =
        prospero_audio_collect_streams(
            format,
            indexes,
            PROSPERO_AUDIO_TRACK_LIMIT
        );

    int ordinal = 0;

    for (int index = 0; index < count; index++) {
        if (indexes[index] == selected_stream) {
            ordinal = index + 1;
            break;
        }
    }

    AVStream *stream =
        format->streams[selected_stream];

    AVCodecParameters *parameters =
        stream->codecpar;

    const AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    const char *language_text =
        (
            language &&
            language->value &&
            language->value[0]
        )
            ? language->value
            : "und";

    const char *codec_name =
        avcodec_get_name(
            parameters->codec_id
        );

    int channels =
        parameters->ch_layout.nb_channels;

    snprintf(
        output,
        output_size,
        "%d/%d  %s  %s  %dCH",
        ordinal,
        count,
        language_text,
        codec_name
            ? codec_name
            : "unknown",
        channels
    );
}


/*
 * prospero_audio_cycle_track() lived here. It cycled to the next audio stream
 * by calling start_video_playback() to re-open the file - a main.c entry point
 * that the C++ migration removed, so the function could not have worked.
 * Audio track selection is now evo::PlaybackController::switchAudioTrack() and
 * the AudioTrackPickerScreen: a picker rather than a cycle, because each step
 * costs a reopen.
 */
