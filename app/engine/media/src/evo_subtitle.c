/*
 * evo_subtitle.c — subtitle engine: embedded (MKV text tracks) + external SRT.
 *
 * Verbatim move of the EMBEDDED_SUBTITLE_MODULE, SRT_MODULE and
 * SUBTITLE_CONTROLS regions from main.c (Track A step A4 of
 * docs/modularisation-plan.md). The only edits are `static` -> external
 * linkage on the handful of symbols main.c still touches (see evo_subtitle.h)
 * and the transitional extern block below.
 */
#include "evo_subtitle.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>

#include "evo_audio_out.h"   /* audio_handle, audio_clock_seconds */
#include "evo_subsync.h"     /* #102 auto-sync worker */

#ifndef SCREEN_PLAYER
#define SCREEN_PLAYER 2
#endif

/* ---------------------------------------------------------------------------
 * TRANSITIONAL: playback-core / resume state still owned by main.c.
 * Replaced by the evo_pb_*() façade at A8. The on-video caption is now drawn by
 * the RmlUi playback OSD (#81); main.c only resolves the active cue text here.
 * ------------------------------------------------------------------------ */
extern int              screen;
extern int              player_paused;
extern double           video_clock_seconds;
extern AVFormatContext *play_fmt;
extern AVCodecContext  *audio_ctx;
extern char             current_media_path[512];
extern double           resume_base_offset_seconds;
extern double           requested_resume_seek_pos;
extern long long        controls_last_used_ms;
extern int              audio_stream_index;

void      toast(const char *title, const char *msg);
long long now_ms(void);
int       start_video_playback_at(const char *path, double resume_seconds);

/* Defined in the SRT section below; used by the embedded section above it. */
static void prospero_subtitle_clean_line(const char *input, char *output,
                                         size_t output_size);
static void prospero_subtitle_append_text(char *destination,
                                          size_t destination_size,
                                          const char *line);

/* Was a standalone static in main.c (near the browser-preview globals). */
int prospero_subtitle_delay_ms = 0;

/* #102: subtitle seconds per media second for the external SRT. Auto-sync sets
 * it when the SRT was timed for another framerate (25 vs 23.976 fps). */
double prospero_subtitle_time_scale = 1.0;

/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_START */

#define PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES 256
#define PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE 768

typedef struct {
    double start_seconds;
    double end_seconds;

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];
} ProsperoEmbeddedSubtitleCue;


/*
 * Subtitle pipeline counters. The trace already reported "a stream is
 * selected" and "no cues exist", which is a gap wide enough to hold every
 * stage in between. These split it: demuxed -> reached the decoder -> text
 * survived cleaning -> stored.
 */
int dbg_sub_demuxed;   /* packets av_read_frame gave us on the track */
int dbg_sub_entered;   /* packets that got past the decoder's guard  */
int dbg_sub_blank;     /* decoded but the text cleaned away to empty */
int dbg_sub_added;     /* add_cue calls                              */
int dbg_sub_cid;       /* codec id actually selected                 */

AVCodecContext *
prospero_embedded_subtitle_ctx = NULL;

int
prospero_embedded_subtitle_stream_index = -1;

/* PROSPERO_DIRECT_SUBRIP_START */

/* The codec id a track decodes lives in its slot (#110), below. */

/* PROSPERO_DIRECT_SUBRIP_END */



/* PROSPERO_SUBTITLE_SELECTION_STATE_START */

/*
 * -2 = automatic
 * -1 = external matching SRT
 * >=0 = exact embedded MKV subtitle stream
 */
int prospero_subtitle_requested_stream = -2;

/*
 * Zero renders the active embedded stream.
 * One renders the matching external SRT.
 */
int prospero_subtitle_use_external = 0;

/* PROSPERO_SUBTITLE_SELECTION_STATE_END */




/*
 * #110 - dual subtitles.
 *
 * An embedded text track is a "slot": the decoder its codec needs, a ring of
 * the cues decoded so far, and the stream it follows. Slot 0 is the primary
 * track and keeps the globals the rest of the app already reads
 * (prospero_embedded_subtitle_ctx / _stream_index / _count). Slot 1 is the
 * secondary track and is only ever driven from this file.
 *
 * Two locks per slot, always taken state -> ring:
 *   state_mutex  the decoder context, stream index and codec id. The demux
 *                thread holds it for one packet decode; the UI thread takes it
 *                to switch or close the track, so a decoder is never freed
 *                under a decode in flight.
 *   ring_mutex   the cue ring. The render thread only ever takes this one.
 */
typedef struct {
    int             *stream_index;
    AVCodecContext **ctx;
    int             *count;
    enum AVCodecID   codec_id;
    int              head;
    ProsperoEmbeddedSubtitleCue cues[
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
    ];
    pthread_mutex_t  ring_mutex;
    pthread_mutex_t  state_mutex;
} ProsperoSubSlot;

int prospero_embedded_subtitle_count = 0;

/* -1 = no embedded track in the secondary slot. */
int prospero_secondary_subtitle_stream_index = -1;
static int             prospero_secondary_cue_count = 0;
static AVCodecContext *prospero_secondary_ctx = NULL;

static ProsperoSubSlot prospero_sub_slot[2] = {
    {
        .stream_index = &prospero_embedded_subtitle_stream_index,
        .ctx          = &prospero_embedded_subtitle_ctx,
        .count        = &prospero_embedded_subtitle_count,
        .codec_id     = AV_CODEC_ID_NONE,
        .ring_mutex   = PTHREAD_MUTEX_INITIALIZER,
        .state_mutex  = PTHREAD_MUTEX_INITIALIZER,
    },
    {
        .stream_index = &prospero_secondary_subtitle_stream_index,
        .ctx          = &prospero_secondary_ctx,
        .count        = &prospero_secondary_cue_count,
        .codec_id     = AV_CODEC_ID_NONE,
        .ring_mutex   = PTHREAD_MUTEX_INITIALIZER,
        .state_mutex  = PTHREAD_MUTEX_INITIALIZER,
    },
};

#define SUB_PRIMARY   (&prospero_sub_slot[0])
#define SUB_SECONDARY (&prospero_sub_slot[1])


int prospero_embedded_subtitle_supported(
    enum AVCodecID codec_id
) {
    return (
        codec_id == AV_CODEC_ID_SUBRIP ||
        codec_id == AV_CODEC_ID_ASS ||
        codec_id == AV_CODEC_ID_SSA ||
        codec_id == AV_CODEC_ID_WEBVTT
    );
}


static void prospero_sub_slot_reset(
    ProsperoSubSlot *slot
) {
    pthread_mutex_lock(
        &slot->ring_mutex
    );

    slot->head = 0;
    *slot->count = 0;

    pthread_mutex_unlock(
        &slot->ring_mutex
    );
}


/* A seek: every cue in the rings is from the wrong place. The primary decoder
 * is flushed by the seek path; the secondary decoder is ours to flush. */
void prospero_embedded_subtitle_reset(void) {
    prospero_sub_slot_reset(SUB_PRIMARY);
    prospero_sub_slot_reset(SUB_SECONDARY);

    pthread_mutex_lock(
        &SUB_SECONDARY->state_mutex
    );

    if (*SUB_SECONDARY->ctx) {
        avcodec_flush_buffers(
            *SUB_SECONDARY->ctx
        );
    }

    pthread_mutex_unlock(
        &SUB_SECONDARY->state_mutex
    );
}


static void prospero_sub_slot_close(
    ProsperoSubSlot *slot
) {
    prospero_sub_slot_reset(slot);

    pthread_mutex_lock(
        &slot->state_mutex
    );

    if (*slot->ctx) {
        avcodec_free_context(
            slot->ctx
        );
    }

    *slot->stream_index = -1;
    slot->codec_id = AV_CODEC_ID_NONE;

    pthread_mutex_unlock(
        &slot->state_mutex
    );
}


void prospero_embedded_subtitle_close(void) {
    prospero_sub_slot_close(SUB_PRIMARY);
    prospero_sub_slot_close(SUB_SECONDARY);

    prospero_secondary_use_external = 0;
}


/*
 * How many cues a subtitle track claims to hold, or -1 when it does not say.
 *
 * mkvmerge writes per-track STATISTICS tags, and the key is suffixed with the
 * language - NUMBER_OF_FRAMES-eng - so the lookup has to ignore the suffix.
 *
 * This matters more than it looks. Release groups ship a vanity track that is
 * flagged "default", is tagged English, and contains two cues forty minutes
 * in. It wins every metadata-based contest against the real subtitle track
 * and then displays nothing, which is indistinguishable from subtitles being
 * broken. The count is the only field that tells them apart before playback.
 */
int prospero_subtitle_declared_cues(
    AVStream *stream
) {
    if (!stream) {
        return -1;
    }

    AVDictionaryEntry *frames =
        av_dict_get(
            stream->metadata,
            "NUMBER_OF_FRAMES",
            NULL,
            AV_DICT_IGNORE_SUFFIX
        );

    if (
        !frames ||
        !frames->value ||
        !frames->value[0]
    ) {
        return -1;
    }

    long parsed =
        strtol(
            frames->value,
            NULL,
            10
        );

    if (
        parsed < 0 ||
        parsed > 1000000
    ) {
        return -1;
    }

    return (int)parsed;
}




static int prospero_embedded_subtitle_score_stream(
    AVStream *stream
) {
    if (
        !stream ||
        !stream->codecpar ||
        !prospero_embedded_subtitle_supported(
            stream->codecpar->codec_id
        )
    ) {
        return -100000;
    }

    int score = 10;

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (language && language->value) {
        if (
            strcasecmp(
                language->value,
                "eng"
            ) == 0 ||
            strcasecmp(
                language->value,
                "en"
            ) == 0 ||
            strcasecmp(
                language->value,
                "english"
            ) == 0
        ) {
            score += 100;
        }
    }

    if (
        stream->disposition &
        AV_DISPOSITION_DEFAULT
    ) {
        score += 40;
    }

    /*
     * Cue count outranks every metadata signal, because it is the only one
     * that reports what the track actually contains rather than what it
     * claims to be. A near-empty track loses even when it is English and
     * flagged default; a rich one gets a modest tie-break bonus.
     */
    {
        int cues =
            prospero_subtitle_declared_cues(
                stream
            );

        if (
            cues >= 0 &&
            cues < PROSPERO_SUBTITLE_MIN_USEFUL_CUES
        ) {
            score -= 1000;
        } else if (cues > 0) {
            score += cues > 50 ? 20 : 5;
        }
    }

    if (
        stream->disposition &
        AV_DISPOSITION_FORCED
    ) {
        score += 25;
    }

    return score;
}


/*
 * Point `slot` at embedded text stream `stream_index` of `format`: open the
 * decoder its codec needs and start following the stream. Returns 1, or 0 with
 * the slot left closed. `title` heads the toast.
 */
static int prospero_sub_slot_start(
    ProsperoSubSlot *slot,
    AVFormatContext *format,
    int stream_index,
    const char *title
) {
    AVStream *stream =
        format->streams[stream_index];

    enum AVCodecID codec_id =
        stream->codecpar->codec_id;

    prospero_sub_slot_close(slot);

    pthread_mutex_lock(
        &slot->state_mutex
    );

    *slot->stream_index = stream_index;
    slot->codec_id = codec_id;

    if (slot == SUB_PRIMARY) {
        dbg_sub_cid = (int)codec_id;
    }

    /*
     * Matroska SubRip packets already contain plain subtitle text.
     * Decode them directly when the PS5 FFmpeg build does not include
     * a registered SubRip decoder.
     */
    if (codec_id != AV_CODEC_ID_SUBRIP) {
        const AVCodec *decoder =
            avcodec_find_decoder(
                codec_id
            );

        AVCodecContext *context = NULL;
        const char *failure = NULL;

        if (!decoder) {
            failure = "TEXT DECODER NOT FOUND";
        } else if (
            !(context = avcodec_alloc_context3(decoder))
        ) {
            failure = "TEXT DECODER FAILED";
        } else if (
            avcodec_parameters_to_context(
                context,
                stream->codecpar
            ) < 0
        ) {
            failure = "TEXT DECODER FAILED";
        } else {
            context->pkt_timebase =
                stream->time_base;

            if (
                avcodec_open2(
                    context,
                    decoder,
                    NULL
                ) < 0
            ) {
                failure = "TEXT DECODER FAILED";
            }
        }

        if (failure) {
            if (context) {
                avcodec_free_context(
                    &context
                );
            }

            *slot->stream_index = -1;
            slot->codec_id = AV_CODEC_ID_NONE;

            pthread_mutex_unlock(
                &slot->state_mutex
            );

            toast(
                "SUBTITLES",
                failure
            );

            return 0;
        }

        *slot->ctx = context;
    }

    pthread_mutex_unlock(
        &slot->state_mutex
    );

    const char *codec_name =
        avcodec_get_name(
            codec_id
        );

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name =
            language->value;
    }

    char message[128];

    snprintf(
        message,
        sizeof(message),
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );

    toast(
        title,
        message
    );

    return 1;
}


int prospero_embedded_subtitle_open(
    AVFormatContext *format
) {
    prospero_embedded_subtitle_close();

    /*
     * A matching external SRT is treated as the selected track.
     */
    if (!format) {
        return 0;
    }

    int best_stream = -1;
    int best_score = -100000;
    int found_pgs = 0;

    for (
        unsigned int index = 0;
        index < format->nb_streams;
        index++
    ) {
        AVStream *stream =
            format->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            stream->codecpar->codec_id ==
            AV_CODEC_ID_HDMV_PGS_SUBTITLE
        ) {
            found_pgs = 1;
            continue;
        }

        int score =
            prospero_embedded_subtitle_score_stream(
                stream
            );

        if (score > best_score) {
            best_score = score;
            best_stream = (int)index;
        }
    }

    
    /*
     * D-pad track cycling may request a specific embedded stream.
     */
    if (
        prospero_subtitle_requested_stream >= 0
    ) {
        int requested =
            prospero_subtitle_requested_stream;

        if (
            requested <
            (int)format->nb_streams
        ) {
            AVStream *requested_stream =
                format->streams[requested];

            if (
                requested_stream &&
                requested_stream->codecpar &&
                requested_stream->codecpar->codec_type ==
                    AVMEDIA_TYPE_SUBTITLE &&
                prospero_embedded_subtitle_supported(
                    requested_stream->codecpar->codec_id
                )
            ) {
                best_stream = requested;
            }
        }
    }

if (best_stream < 0) {
        if (found_pgs) {
            toast(
                "SUBTITLES",
                "PGS NOT SUPPORTED YET"
            );
        }

        return 0;
    }

    return prospero_sub_slot_start(
        SUB_PRIMARY,
        format,
        best_stream,
        "EMBEDDED SUBTITLES"
    );
}


static const char *
prospero_embedded_subtitle_ass_payload(
    const char *ass
) {
    if (!ass) {
        return NULL;
    }

    int fields_to_skip =
        strncmp(
            ass,
            "Dialogue:",
            9
        ) == 0
            ? 9
            : 8;

    const char *cursor = ass;
    int comma_count = 0;

    while (*cursor) {
        if (*cursor == ',') {
            comma_count++;

            if (
                comma_count >=
                fields_to_skip
            ) {
                return cursor + 1;
            }
        }

        cursor++;
    }

    return ass;
}


static void prospero_embedded_subtitle_extract_text(
    AVSubtitle *subtitle,
    char *output,
    size_t output_size
) {
    if (
        !subtitle ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    output[0] = 0;

    for (
        unsigned int index = 0;
        index < subtitle->num_rects;
        index++
    ) {
        AVSubtitleRect *rectangle =
            subtitle->rects[index];

        if (!rectangle) {
            continue;
        }

        const char *raw_text = NULL;

        if (
            rectangle->text &&
            rectangle->text[0]
        ) {
            raw_text =
                rectangle->text;
        } else if (
            rectangle->ass &&
            rectangle->ass[0]
        ) {
            raw_text =
                prospero_embedded_subtitle_ass_payload(
                    rectangle->ass
                );
        }

        if (!raw_text || !raw_text[0]) {
            continue;
        }

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            prospero_subtitle_append_text(
                output,
                output_size,
                cleaned
            );
        }
    }
}


static void prospero_embedded_subtitle_add_cue(
    ProsperoSubSlot *slot,
    double start_seconds,
    double end_seconds,
    const char *text
) {
    if (
        !text ||
        !text[0] ||
        end_seconds <= start_seconds
    ) {
        return;
    }

    pthread_mutex_lock(
        &slot->ring_mutex
    );

    if (
        *slot->count >=
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES
    ) {
        slot->head =
            (
                slot->head +
                1
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        (*slot->count)--;
    }

    int write_index =
        (
            slot->head +
            *slot->count
        ) %
        PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

    ProsperoEmbeddedSubtitleCue *cue =
        &slot->cues[
            write_index
        ];

    cue->start_seconds =
        start_seconds;

    cue->end_seconds =
        end_seconds;

    snprintf(
        cue->text,
        sizeof(cue->text),
        "%s",
        text
    );

    (*slot->count)++;

    pthread_mutex_unlock(
        &slot->ring_mutex
    );
}


/* Caller holds slot->state_mutex. */
static void prospero_sub_slot_decode_locked(
    ProsperoSubSlot *slot,
    AVPacket *packet
) {
    const int primary =
        slot == SUB_PRIMARY;

    const int stream_index =
        *slot->stream_index;

    if (
        !play_fmt ||
        stream_index < 0
    ) {
        return;
    }

    if (primary) {
        dbg_sub_entered++;
    }

    AVStream *stream =
        play_fmt->streams[
            stream_index
        ];

    int64_t timestamp =
        packet->pts != AV_NOPTS_VALUE
            ? packet->pts
            : packet->dts;

    double base_seconds = 0.0;

    if (timestamp != AV_NOPTS_VALUE) {
        base_seconds =
            timestamp *
            av_q2d(
                stream->time_base
            );
    }

    double packet_duration = 0.0;

    if (packet->duration > 0) {
        packet_duration =
            packet->duration *
            av_q2d(
                stream->time_base
            );
    }

    if (packet_duration <= 0.0) {
        packet_duration = 5.0;
    }

    /*
     * Direct Matroska SubRip path.
     */
    if (
        slot->codec_id ==
        AV_CODEC_ID_SUBRIP
    ) {
        if (
            !packet->data ||
            packet->size <= 0
        ) {
            return;
        }

        size_t copy_size =
            (size_t)packet->size;

        if (
            copy_size >=
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ) {
            copy_size =
                PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE - 1;
        }

        char raw_text[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        memcpy(
            raw_text,
            packet->data,
            copy_size
        );

        raw_text[copy_size] = 0;

        char cleaned[
            PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
        ];

        prospero_subtitle_clean_line(
            raw_text,
            cleaned,
            sizeof(cleaned)
        );

        if (cleaned[0]) {
            if (primary) {
                dbg_sub_added++;
            }

            prospero_embedded_subtitle_add_cue(
                slot,
                base_seconds,
                base_seconds + packet_duration,
                cleaned
            );
        } else if (primary) {
            dbg_sub_blank++;
        }

        return;
    }

    if (!*slot->ctx) {
        return;
    }

    AVSubtitle subtitle;

    memset(
        &subtitle,
        0,
        sizeof(subtitle)
    );

    subtitle.pts =
        AV_NOPTS_VALUE;

    int got_subtitle = 0;

    int result =
        avcodec_decode_subtitle2(
            *slot->ctx,
            &subtitle,
            &got_subtitle,
            packet
        );

    if (
        result < 0 ||
        !got_subtitle
    ) {
        avsubtitle_free(
            &subtitle
        );

        return;
    }

    if (
        subtitle.pts !=
        AV_NOPTS_VALUE
    ) {
        base_seconds =
            (double)subtitle.pts /
            (double)AV_TIME_BASE;
    }

    double start_seconds =
        base_seconds +
        subtitle.start_display_time /
            1000.0;

    double end_seconds =
        base_seconds +
        subtitle.end_display_time /
            1000.0;

    if (
        end_seconds <= start_seconds ||
        end_seconds - start_seconds >
            120.0
    ) {
        end_seconds =
            start_seconds +
            packet_duration;
    }

    if (end_seconds <= start_seconds) {
        end_seconds =
            start_seconds + 5.0;
    }

    char text[
        PROSPERO_EMBEDDED_SUBTITLE_TEXT_SIZE
    ];

    prospero_embedded_subtitle_extract_text(
        &subtitle,
        text,
        sizeof(text)
    );

    if (text[0]) {
        prospero_embedded_subtitle_add_cue(
            slot,
            start_seconds,
            end_seconds,
            text
        );
    }

    avsubtitle_free(
        &subtitle
    );
}


static void prospero_sub_slot_decode(
    ProsperoSubSlot *slot,
    AVPacket *packet
) {
    /* Read unlocked on purpose: a stale value only costs one needless lock,
     * or one packet left for the next demux pass after a switch. */
    if (packet->stream_index != *slot->stream_index) {
        return;
    }

    pthread_mutex_lock(
        &slot->state_mutex
    );

    prospero_sub_slot_decode_locked(
        slot,
        packet
    );

    pthread_mutex_unlock(
        &slot->state_mutex
    );
}


void prospero_embedded_subtitle_decode_packet(
    AVPacket *packet
) {
    if (!packet) {
        return;
    }

    prospero_sub_slot_decode(SUB_PRIMARY, packet);
    prospero_sub_slot_decode(SUB_SECONDARY, packet);
}


static int prospero_sub_slot_text_at(
    ProsperoSubSlot *slot,
    double position,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    output[0] = 0;

    pthread_mutex_lock(
        &slot->ring_mutex
    );

    const ProsperoEmbeddedSubtitleCue *
        selected = NULL;

    for (
        int offset = 0;
        offset <
            *slot->count;
        offset++
    ) {
        int index =
            (
                slot->head +
                offset
            ) %
            PROSPERO_EMBEDDED_SUBTITLE_MAX_CUES;

        const ProsperoEmbeddedSubtitleCue *cue =
            &slot->cues[
                index
            ];

        if (
            position >= cue->start_seconds &&
            position <= cue->end_seconds
        ) {
            if (
                !selected ||
                cue->start_seconds >
                    selected->start_seconds
            ) {
                selected = cue;
            }
        }
    }

    if (selected) {
        snprintf(
            output,
            output_size,
            "%s",
            selected->text
        );
    }

    pthread_mutex_unlock(
        &slot->ring_mutex
    );

    return output[0] != 0;
}


int prospero_embedded_subtitle_text_at(
    double position,
    char *output,
    size_t output_size
) {
    return prospero_sub_slot_text_at(
        SUB_PRIMARY,
        position,
        output,
        output_size
    );
}

/* PROSPERO_EMBEDDED_SUBTITLE_MODULE_END */
/* PROSPERO_SRT_MODULE_START */

#define PROSPERO_SUBTITLE_MAX_CUES 2048
/* PROSPERO_SUBTITLE_{TEXT_SIZE,MAX_LINES,LINE_SIZE} and the ProsperoSubtitleCue
 * type are in evo_subtitle.h now (main.c reads cues via prospero_subtitle_active_cue). */

static ProsperoSubtitleCue prospero_subtitle_cues[
    PROSPERO_SUBTITLE_MAX_CUES
];

int prospero_subtitle_count = 0;
int prospero_subtitle_enabled = 1;
int prospero_subtitle_face = 2;  /* 1 SUB (SMALL), 2 MENU (MEDIUM/DEFAULT), 3 TITLE (LARGE) */

static char prospero_subtitle_path[512] = {0};


void prospero_subtitle_clear(void) {
    /* Media change or stop: an analysis of the old file must not land on the
     * new one, and its offset/ratio belong to the old SRT too. */
    evo_subsync_cancel();
    prospero_subtitle_count = 0;
    prospero_subtitle_path[0] = 0;
    prospero_subtitle_delay_ms = 0;
    prospero_subtitle_time_scale = 1.0;
    prospero_secondary_delay_ms = 0;
}


void prospero_subtitle_trim(
    char *text
) {
    if (!text) {
        return;
    }

    size_t length =
        strlen(text);

    while (
        length > 0 &&
        (
            text[length - 1] == '\r' ||
            text[length - 1] == '\n' ||
            text[length - 1] == ' ' ||
            text[length - 1] == '\t'
        )
    ) {
        text[--length] = 0;
    }

    size_t start = 0;

    while (
        text[start] == ' ' ||
        text[start] == '\t'
    ) {
        start++;
    }

    if (start > 0) {
        memmove(
            text,
            text + start,
            strlen(text + start) + 1
        );
    }
}


static int prospero_subtitle_parse_timing(
    const char *line,
    double *start_seconds,
    double *end_seconds
) {
    int start_hour = 0;
    int start_minute = 0;
    int start_second = 0;
    int start_millisecond = 0;

    int end_hour = 0;
    int end_minute = 0;
    int end_second = 0;
    int end_millisecond = 0;

    char start_separator = 0;
    char end_separator = 0;

    int matched =
        sscanf(
            line,
            "%d:%d:%d%c%d --> %d:%d:%d%c%d",
            &start_hour,
            &start_minute,
            &start_second,
            &start_separator,
            &start_millisecond,
            &end_hour,
            &end_minute,
            &end_second,
            &end_separator,
            &end_millisecond
        );

    if (matched != 10) {
        return 0;
    }

    if (
        (
            start_separator != ',' &&
            start_separator != '.'
        ) ||
        (
            end_separator != ',' &&
            end_separator != '.'
        )
    ) {
        return 0;
    }

    *start_seconds =
        start_hour * 3600.0 +
        start_minute * 60.0 +
        start_second +
        start_millisecond / 1000.0;

    *end_seconds =
        end_hour * 3600.0 +
        end_minute * 60.0 +
        end_second +
        end_millisecond / 1000.0;

    return (
        *end_seconds >=
        *start_seconds
    );
}


static void prospero_subtitle_clean_line(
    const char *input,
    char *output,
    size_t output_size
) {
    if (
        !input ||
        !output ||
        output_size == 0
    ) {
        return;
    }

    size_t write_index = 0;
    int inside_angle_tag = 0;
    int inside_brace_tag = 0;

    for (
        size_t index = 0;
        input[index] &&
        write_index + 1 < output_size;
        index++
    ) {
        unsigned char value =
            (unsigned char)input[index];

        if (value == '<') {
            inside_angle_tag = 1;
            continue;
        }

        if (
            inside_angle_tag &&
            value == '>'
        ) {
            inside_angle_tag = 0;
            continue;
        }

        if (value == '{') {
            inside_brace_tag = 1;
            continue;
        }

        if (
            inside_brace_tag &&
            value == '}'
        ) {
            inside_brace_tag = 0;
            continue;
        }

        if (
            inside_angle_tag ||
            inside_brace_tag
        ) {
            continue;
        }

        if (
            value == '\\' &&
            (
                input[index + 1] == 'N' ||
                input[index + 1] == 'n'
            )
        ) {
            output[write_index++] = '\n';
            index++;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&amp;",
                5
            ) == 0
        ) {
            output[write_index++] = '&';
            index += 4;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&lt;",
                4
            ) == 0
        ) {
            output[write_index++] = '<';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&gt;",
                4
            ) == 0
        ) {
            output[write_index++] = '>';
            index += 3;
            continue;
        }

        if (
            strncmp(
                input + index,
                "&nbsp;",
                6
            ) == 0
        ) {
            output[write_index++] = ' ';
            index += 5;
            continue;
        }

        /*
         * Normalize common UTF-8 punctuation to characters supported
         * by the current UI font.
         */
        if (
            value == 0xE2 &&
            (unsigned char)input[index + 1] == 0x80
        ) {
            unsigned char third =
                (unsigned char)input[index + 2];

            if (
                third == 0x98 ||
                third == 0x99
            ) {
                output[write_index++] = '\'';
                index += 2;
                continue;
            }

            if (
                third == 0x9C ||
                third == 0x9D
            ) {
                output[write_index++] = '"';
                index += 2;
                continue;
            }

            if (
                third == 0x93 ||
                third == 0x94
            ) {
                output[write_index++] = '-';
                index += 2;
                continue;
            }

            if (third == 0xA6) {
                if (
                    write_index + 3 <
                    output_size
                ) {
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                    output[write_index++] = '.';
                }

                index += 2;
                continue;
            }
        }

        if (
            value == 0xC2 &&
            (unsigned char)input[index + 1] == 0xA0
        ) {
            output[write_index++] = ' ';
            index++;
            continue;
        }

        /*
         * #81: pass UTF-8 through untouched. The old code folded every byte
         * >= 128 to '?' because the bitmap caption font was ASCII-only; the
         * RmlUi caption overlay renders real Unicode (Noto / DejaVu fallback
         * faces). Continuation bytes fall through to the copy below, so the
         * whole sequence is preserved. The curly-quote / dash / ellipsis
         * normalisation above still runs (harmless, and keeps lines compact).
         */

        if (value == '\t') {
            value = ' ';
        }

        output[write_index++] =
            (char)value;
    }

    output[write_index] = 0;
    prospero_subtitle_trim(output);
}


static void prospero_subtitle_append_text(
    char *destination,
    size_t destination_size,
    const char *line
) {
    if (
        !destination ||
        !line ||
        destination_size == 0
    ) {
        return;
    }

    size_t used =
        strlen(destination);

    if (
        used > 0 &&
        used + 1 < destination_size
    ) {
        destination[used++] = '\n';
        destination[used] = 0;
    }

    if (used + 1 >= destination_size) {
        return;
    }

    snprintf(
        destination + used,
        destination_size - used,
        "%s",
        line
    );
}


static int prospero_subtitle_sidecar_path(
    const char *media_path,
    char *output,
    size_t output_size,
    const char *extension
) {
    if (
        !media_path ||
        !media_path[0] ||
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    snprintf(
        output,
        output_size,
        "%s",
        media_path
    );

    char *last_slash =
        strrchr(output, '/');

    char *last_dot =
        strrchr(output, '.');

    if (
        last_dot &&
        (
            !last_slash ||
            last_dot > last_slash
        )
    ) {
        *last_dot = 0;
    }

    size_t used =
        strlen(output);

    if (
        used + strlen(extension) + 1 >
        output_size
    ) {
        return 0;
    }

    strcat(
        output,
        extension
    );

    return 1;
}


static int prospero_subtitle_load_file(
    const char *path
) {
    FILE *file =
        fopen(
            path,
            "rb"
        );

    if (!file) {
        return 0;
    }

    prospero_subtitle_count = 0;

    char line[1024];
    int first_line = 1;

    while (
        prospero_subtitle_count <
            PROSPERO_SUBTITLE_MAX_CUES &&
        fgets(
            line,
            sizeof(line),
            file
        )
    ) {
        if (
            first_line &&
            (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF
        ) {
            memmove(
                line,
                line + 3,
                strlen(line + 3) + 1
            );
        }

        first_line = 0;
        prospero_subtitle_trim(line);

        if (!line[0]) {
            continue;
        }

        char timing_line[1024];

        if (strstr(line, "-->")) {
            snprintf(
                timing_line,
                sizeof(timing_line),
                "%s",
                line
            );
        } else {
            if (
                !fgets(
                    timing_line,
                    sizeof(timing_line),
                    file
                )
            ) {
                break;
            }

            prospero_subtitle_trim(
                timing_line
            );

            if (
                !strstr(
                    timing_line,
                    "-->"
                )
            ) {
                continue;
            }
        }

        double start_seconds = 0.0;
        double end_seconds = 0.0;

        if (
            !prospero_subtitle_parse_timing(
                timing_line,
                &start_seconds,
                &end_seconds
            )
        ) {
            continue;
        }

        ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[
                prospero_subtitle_count
            ];

        cue->start_seconds =
            start_seconds;

        cue->end_seconds =
            end_seconds;

        cue->text[0] = 0;

        while (
            fgets(
                line,
                sizeof(line),
                file
            )
        ) {
            prospero_subtitle_trim(line);

            if (!line[0]) {
                break;
            }

            char cleaned[
                PROSPERO_SUBTITLE_TEXT_SIZE
            ];

            prospero_subtitle_clean_line(
                line,
                cleaned,
                sizeof(cleaned)
            );

            if (cleaned[0]) {
                prospero_subtitle_append_text(
                    cue->text,
                    sizeof(cue->text),
                    cleaned
                );
            }
        }

        if (cue->text[0]) {
            prospero_subtitle_count++;
        }
    }

    fclose(file);

    if (prospero_subtitle_count <= 0) {
        return 0;
    }

    snprintf(
        prospero_subtitle_path,
        sizeof(prospero_subtitle_path),
        "%s",
        path
    );

    return prospero_subtitle_count;
}


int prospero_subtitle_load_for_media(
    const char *media_path
) {
    prospero_subtitle_clear();

    char sidecar_path[512];

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".srt"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    if (
        prospero_subtitle_sidecar_path(
            media_path,
            sidecar_path,
            sizeof(sidecar_path),
            ".SRT"
        )
    ) {
        int count =
            prospero_subtitle_load_file(
                sidecar_path
            );

        if (count > 0) {
            char message[96];

            snprintf(
                message,
                sizeof(message),
                "Loaded %d cues",
                count
            );

            toast(
                "SUBTITLES",
                message
            );

            return count;
        }
    }

    prospero_subtitle_clear();
    return 0;
}


const ProsperoSubtitleCue *
prospero_subtitle_active_cue(
    double position
) {
    int low = 0;
    int high =
        prospero_subtitle_count - 1;

    while (low <= high) {
        int middle =
            low + (high - low) / 2;

        const ProsperoSubtitleCue *cue =
            &prospero_subtitle_cues[middle];

        if (
            position <
            cue->start_seconds
        ) {
            high = middle - 1;
        } else if (
            position >
            cue->end_seconds
        ) {
            low = middle + 1;
        } else {
            return cue;
        }
    }

    return NULL;
}

/* PROSPERO_SRT_MODULE_END */
/* PROSPERO_SUBTITLE_CONTROLS_START */

void prospero_subtitle_toggle(void) {
    int available =
        prospero_subtitle_count > 0 ||
        prospero_embedded_subtitle_stream_index >= 0;

    if (!available) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    prospero_subtitle_enabled =
        !prospero_subtitle_enabled;

    toast(
        "SUBTITLES",
        prospero_subtitle_enabled
            ? "ON"
            : "OFF"
    );

    controls_last_used_ms = now_ms();
}


static int prospero_subtitle_collect_tracks(
    int *tracks,
    int capacity
) {
    if (
        !tracks ||
        capacity <= 0 ||
        !play_fmt
    ) {
        return 0;
    }

    int count = 0;

    /*
     * -1 represents the matching external SRT.
     */
    if (
        prospero_subtitle_count > 0 &&
        count < capacity
    ) {
        tracks[count++] = -1;
    }

    for (
        unsigned int index = 0;
        index < play_fmt->nb_streams &&
        count < capacity;
        index++
    ) {
        AVStream *stream =
            play_fmt->streams[index];

        if (
            !stream ||
            !stream->codecpar ||
            stream->codecpar->codec_type !=
                AVMEDIA_TYPE_SUBTITLE
        ) {
            continue;
        }

        if (
            prospero_embedded_subtitle_supported(
                stream->codecpar->codec_id
            )
        ) {
            tracks[count++] = (int)index;
        }
    }

    return count;
}


static void prospero_subtitle_track_label(
    int track,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return;
    }

    if (track < 0) {
        snprintf(
            output,
            output_size,
            "EXTERNAL SRT"
        );

        return;
    }

    if (
        !play_fmt ||
        track >= (int)play_fmt->nb_streams
    ) {
        snprintf(
            output,
            output_size,
            "EMBEDDED TEXT"
        );

        return;
    }

    AVStream *stream =
        play_fmt->streams[track];

    const char *language_name =
        "UNSPECIFIED";

    AVDictionaryEntry *language =
        av_dict_get(
            stream->metadata,
            "language",
            NULL,
            0
        );

    if (
        language &&
        language->value &&
        language->value[0]
    ) {
        language_name = language->value;
    }

    const char *codec_name =
        avcodec_get_name(
            stream->codecpar->codec_id
        );

    snprintf(
        output,
        output_size,
        "%s / %s",
        language_name,
        codec_name
            ? codec_name
            : "TEXT"
    );
}


/*
 * Switch to `track` (-1 meaning the external SRT) and carry on from where we
 * are. Selecting a subtitle stream is a demuxer-open decision, so the file is
 * reopened and seeked back to the current position rather than switched in
 * place; the pause state survives so this does not silently start playing a
 * paused film.
 *
 * Split out of the D-pad cycler so the picker performs the identical switch.
 * Cycling made sense when a file had two tracks. Retail rips carry thirty-odd
 * and cycling through them one restart at a time is unusable, so the cycler
 * is now the fallback path and the picker is the one people will use.
 */
void prospero_subtitle_apply_track(int track)
{
    char label[128];

    if (
        !play_fmt ||
        !current_media_path[0]
    ) {
        return;
    }

    prospero_subtitle_track_label(
        track,
        label,
        sizeof(label)
    );

    double position =
        resume_base_offset_seconds +
        (
            audio_ctx &&
            audio_handle >= 1
                ? audio_clock_seconds
                : video_clock_seconds
        );

    if (position < 0.0) {
        position = 0.0;
    }

    int restore_paused = player_paused;

    char playback_path[512];

    snprintf(
        playback_path,
        sizeof(playback_path),
        "%s",
        current_media_path
    );

    prospero_subtitle_requested_stream = track;

    /* The track about to become the primary cannot also be the secondary. */
    if (
        prospero_secondary_subtitle_requested == track
    ) {
        prospero_secondary_subtitle_requested =
            PROSPERO_SECONDARY_NONE;
    }

    requested_resume_seek_pos = position;
    resume_base_offset_seconds = position;

    /* Re-open at the position the picture is at, not the start of the file:
     * requested_resume_seek_pos is no longer read by the open path. */
    if (
        !start_video_playback_at(
            playback_path, position
        )
    ) {
        prospero_subtitle_requested_stream = -2;

        toast(
            "SUBTITLES",
            "TRACK CHANGE FAILED"
        );

        return;
    }

    if (restore_paused) {
        player_paused = 1;
    }

    prospero_subtitle_enabled = 1;

    toast(
        "SUBTITLES",
        label
    );

    controls_last_used_ms = now_ms();
}


static void prospero_subtitle_cycle_track(void) {
    if (
        screen != SCREEN_PLAYER ||
        !play_fmt ||
        !current_media_path[0]
    ) {
        return;
    }

    int tracks[64];

    int track_count =
        prospero_subtitle_collect_tracks(
            tracks,
            64
        );

    if (track_count <= 0) {
        prospero_subtitle_enabled = 0;

        toast(
            "SUBTITLES",
            "NO SUPPORTED TEXT TRACK"
        );

        return;
    }

    int current_track =
        prospero_subtitle_use_external
            ? -1
            : prospero_embedded_subtitle_stream_index;

    int current_slot = -1;

    for (
        int index = 0;
        index < track_count;
        index++
    ) {
        if (tracks[index] == current_track) {
            current_slot = index;
            break;
        }
    }

    int next_slot =
        current_slot < 0
            ? 0
            : (current_slot + 1) % track_count;

    int next_track =
        tracks[next_slot];

    char label[128];

    prospero_subtitle_track_label(
        next_track,
        label,
        sizeof(label)
    );

    if (
        track_count == 1 &&
        current_slot == 0
    ) {
        prospero_subtitle_enabled = 1;
        toast("SUBTITLES", label);
        return;
    }

    prospero_subtitle_apply_track(
        next_track
    );
}

/* PROSPERO_SUBTITLE_CONTROLS_END */
void prospero_subtitle_nudge_delay(int delta_ms)
{
    char msg[64];
    prospero_subtitle_delay_ms += delta_ms;
    if (prospero_subtitle_delay_ms < -PROSPERO_SUBTITLE_MAX_DELAY_MS)
        prospero_subtitle_delay_ms = -PROSPERO_SUBTITLE_MAX_DELAY_MS;
    if (prospero_subtitle_delay_ms > PROSPERO_SUBTITLE_MAX_DELAY_MS)
        prospero_subtitle_delay_ms = PROSPERO_SUBTITLE_MAX_DELAY_MS;

    if (prospero_subtitle_delay_ms == 0)
        snprintf(msg, sizeof(msg), "0 ms (sync)");
    else
        snprintf(
            msg,
            sizeof(msg),
            "%+d ms",
            prospero_subtitle_delay_ms);
    toast("SUB DELAY", msg);
    controls_last_used_ms = now_ms();
}

double prospero_subtitle_position(double clock_seconds)
{
    /* The ratio is measured against the external SRT only; embedded tracks are
     * muxed with the video and never drift. */
    double scale = prospero_subtitle_use_external
                 ? prospero_subtitle_time_scale : 1.0;
    double pos = clock_seconds * scale -
                 (double)prospero_subtitle_delay_ms / 1000.0;
    return pos < 0.0 ? 0.0 : pos;
}

/* PROSPERO_SECONDARY_SUBTITLE_START (#110) */

/*
 * The secondary track: a second line of dialogue drawn with the first, for
 * bilingual viewing. It is the external SRT or one embedded text track, never
 * the same source as the primary. What was chosen is kept per file, so a
 * reopen of the same file (audio change, primary change) keeps it and a
 * different file starts without one.
 */
int prospero_secondary_subtitle_requested = PROSPERO_SECONDARY_NONE;
int prospero_secondary_use_external = 0;
int prospero_secondary_delay_ms = 0;
int prospero_secondary_position = PROSPERO_SECONDARY_POS_STACKED;
int prospero_secondary_color = 0;

static char prospero_secondary_media[768] = {0};


int prospero_secondary_subtitle_active(void) {
    if (prospero_secondary_use_external) {
        /* One SRT cannot be both tracks. */
        return !prospero_subtitle_use_external &&
               prospero_subtitle_count > 0;
    }

    return prospero_secondary_subtitle_stream_index >= 0;
}


int prospero_subtitle_wants_stream(int stream_index) {
    return stream_index >= 0 &&
           (
               stream_index == prospero_embedded_subtitle_stream_index ||
               stream_index == prospero_secondary_subtitle_stream_index
           );
}


static int prospero_secondary_stream_usable(int stream_index) {
    if (
        !play_fmt ||
        stream_index < 0 ||
        stream_index >= (int)play_fmt->nb_streams
    ) {
        return 0;
    }

    AVStream *stream =
        play_fmt->streams[stream_index];

    return stream &&
           stream->codecpar &&
           stream->codecpar->codec_type ==
               AVMEDIA_TYPE_SUBTITLE &&
           prospero_embedded_subtitle_supported(
               stream->codecpar->codec_id
           );
}


/* The secondary gives up its track without a word: the primary is taking it. */
static void prospero_secondary_drop(void) {
    prospero_secondary_subtitle_requested =
        PROSPERO_SECONDARY_NONE;
    prospero_secondary_use_external = 0;
    prospero_secondary_delay_ms = 0;
    prospero_sub_slot_close(SUB_SECONDARY);
}


int prospero_secondary_subtitle_open(
    AVFormatContext *format
) {
    prospero_secondary_use_external = 0;

    if (
        strcmp(
            prospero_secondary_media,
            current_media_path
        ) != 0
    ) {
        /* A different file: the choice was made for the last one. */
        prospero_secondary_subtitle_requested =
            PROSPERO_SECONDARY_NONE;
        prospero_secondary_media[0] = 0;
    }

    int wanted =
        prospero_secondary_subtitle_requested;

    if (
        !format ||
        wanted == PROSPERO_SECONDARY_NONE
    ) {
        return 0;
    }

    if (wanted < 0) {
        if (
            prospero_subtitle_count > 0 &&
            !prospero_subtitle_use_external
        ) {
            prospero_secondary_use_external = 1;
            return 1;
        }

        return 0;
    }

    if (
        !prospero_secondary_stream_usable(wanted) ||
        (
            !prospero_subtitle_use_external &&
            wanted == prospero_embedded_subtitle_stream_index
        )
    ) {
        return 0;
    }

    return prospero_sub_slot_start(
        SUB_SECONDARY,
        format,
        wanted,
        "SECONDARY SUBTITLES"
    );
}


int prospero_secondary_subtitle_select(int track) {
    if (track == PROSPERO_SECONDARY_NONE) {
        prospero_secondary_drop();

        toast(
            "SECONDARY SUBTITLES",
            "OFF"
        );

        return 1;
    }

    if (
        !play_fmt ||
        !current_media_path[0]
    ) {
        return 0;
    }

    if (track == -1) {
        if (prospero_subtitle_count <= 0) {
            toast(
                "SECONDARY SUBTITLES",
                "NO EXTERNAL SRT"
            );

            return 0;
        }

        if (prospero_subtitle_use_external) {
            toast(
                "SECONDARY SUBTITLES",
                "THE SRT IS THE PRIMARY TRACK"
            );

            return 0;
        }

        prospero_sub_slot_close(SUB_SECONDARY);
        prospero_secondary_use_external = 1;

        toast(
            "SECONDARY SUBTITLES",
            "EXTERNAL SRT"
        );
    } else {
        if (!prospero_secondary_stream_usable(track)) {
            toast(
                "SECONDARY SUBTITLES",
                "NOT A TEXT TRACK"
            );

            return 0;
        }

        if (
            !prospero_subtitle_use_external &&
            track == prospero_embedded_subtitle_stream_index
        ) {
            toast(
                "SECONDARY SUBTITLES",
                "THAT IS THE PRIMARY TRACK"
            );

            return 0;
        }

        prospero_secondary_use_external = 0;

        if (
            !prospero_sub_slot_start(
                SUB_SECONDARY,
                play_fmt,
                track,
                "SECONDARY SUBTITLES"
            )
        ) {
            return 0;
        }
    }

    prospero_secondary_subtitle_requested = track;

    snprintf(
        prospero_secondary_media,
        sizeof(prospero_secondary_media),
        "%s",
        current_media_path
    );

    /* A different track has its own timing. */
    prospero_secondary_delay_ms = 0;
    prospero_subtitle_enabled = 1;
    controls_last_used_ms = now_ms();

    return 1;
}


void prospero_secondary_nudge_delay(int delta_ms) {
    char msg[64];

    prospero_secondary_delay_ms += delta_ms;

    if (prospero_secondary_delay_ms < -PROSPERO_SUBTITLE_MAX_DELAY_MS)
        prospero_secondary_delay_ms = -PROSPERO_SUBTITLE_MAX_DELAY_MS;
    if (prospero_secondary_delay_ms > PROSPERO_SUBTITLE_MAX_DELAY_MS)
        prospero_secondary_delay_ms = PROSPERO_SUBTITLE_MAX_DELAY_MS;

    if (prospero_secondary_delay_ms == 0)
        snprintf(msg, sizeof(msg), "0 ms (sync)");
    else
        snprintf(
            msg,
            sizeof(msg),
            "%+d ms",
            prospero_secondary_delay_ms);

    toast("2ND SUB DELAY", msg);
    controls_last_used_ms = now_ms();
}


int prospero_secondary_subtitle_text_at(
    double clock_seconds,
    char *output,
    size_t output_size
) {
    if (
        !output ||
        output_size == 0
    ) {
        return 0;
    }

    output[0] = 0;

    if (!prospero_secondary_subtitle_active()) {
        return 0;
    }

    /* The SRT's framerate ratio applies to whichever track is the SRT. */
    double scale = prospero_secondary_use_external
                 ? prospero_subtitle_time_scale : 1.0;
    double position = clock_seconds * scale -
                      (double)prospero_secondary_delay_ms / 1000.0;

    if (position < 0.0) {
        position = 0.0;
    }

    if (prospero_secondary_use_external) {
        const ProsperoSubtitleCue *cue =
            prospero_subtitle_active_cue(position);

        if (cue && cue->text[0]) {
            snprintf(
                output,
                output_size,
                "%s",
                cue->text
            );
        }
    } else {
        prospero_sub_slot_text_at(
            SUB_SECONDARY,
            position,
            output,
            output_size
        );
    }

    return output[0] != 0;
}

/*
 * Make `track` the primary: -1 for the external SRT, or an embedded stream
 * index. In place, exactly as the secondary switches: the demuxer already
 * delivers every subtitle stream, and the decoder for the new track's codec
 * is opened under the slot's lock. (The picker used to only write the stream
 * index, which left the old track's decoder and cues behind - harmless for two
 * SubRip tracks, wrong the moment a file mixes ASS and SubRip.) The same
 * source cannot be both tracks, so a secondary that held it is dropped.
 */
int prospero_subtitle_select_primary(int track) {
    if (track == -1) {
        if (prospero_subtitle_count <= 0) {
            return 0;
        }

        if (prospero_secondary_use_external) {
            prospero_secondary_drop();
        }

        prospero_subtitle_use_external = 1;
        prospero_subtitle_enabled = 1;

        return 1;
    }

    if (
        !play_fmt ||
        !prospero_secondary_stream_usable(track)
    ) {
        return 0;
    }

    if (track == prospero_secondary_subtitle_stream_index) {
        prospero_secondary_drop();
    }

    prospero_subtitle_use_external = 0;
    prospero_subtitle_enabled = 1;

    if (track == prospero_embedded_subtitle_stream_index) {
        return 1;
    }

    return prospero_sub_slot_start(
        SUB_PRIMARY,
        play_fmt,
        track,
        "EMBEDDED SUBTITLES"
    );
}

/* PROSPERO_SECONDARY_SUBTITLE_END */

/* PROSPERO_SUBTITLE_AUTOSYNC_START (#102) */

/* What the running/last run analysed: -1 the external SRT, >= 0 that
 * embedded stream. A result is only applied to the track it measured. */
static int prospero_subtitle_autosync_track = -1;

int prospero_subtitle_autosync_available(void)
{
    if (!prospero_subtitle_enabled ||
        !evo_subsync_path_supported(current_media_path))
        return 0;
    if (prospero_subtitle_use_external)
        return prospero_subtitle_count > 0;

    int index = prospero_embedded_subtitle_stream_index;
    return play_fmt && index >= 0 && index < (int)play_fmt->nb_streams &&
           prospero_embedded_subtitle_supported(
               play_fmt->streams[index]->codecpar->codec_id);
}

int prospero_subtitle_autosync_running(void)
{
    return evo_subsync_running();
}

void prospero_subtitle_autosync_toggle(void)
{
    if (evo_subsync_running()) {
        evo_subsync_cancel();
        toast("AUTO-SYNC", "Cancelled");
        return;
    }
    if (!prospero_subtitle_autosync_available())
        return;

    if (!prospero_subtitle_use_external) {
        /* Embedded: the worker reads the track's cues from the file itself;
         * the ring the player keeps only holds what has streamed past. */
        prospero_subtitle_autosync_track = prospero_embedded_subtitle_stream_index;
        if (!evo_subsync_start(current_media_path, audio_stream_index,
                               prospero_subtitle_autosync_track, NULL, NULL, 0))
            toast("AUTO-SYNC", "Couldn't auto-sync - adjust manually");
        return;
    }

    prospero_subtitle_autosync_track = -1;
    int n = prospero_subtitle_count;
    double *starts = (double *)malloc(sizeof(double) * (size_t)n);
    double *ends = (double *)malloc(sizeof(double) * (size_t)n);
    int started = 0;
    if (starts && ends) {
        for (int i = 0; i < n; i++) {
            starts[i] = prospero_subtitle_cues[i].start_seconds;
            ends[i] = prospero_subtitle_cues[i].end_seconds;
        }
        started = evo_subsync_start(current_media_path, audio_stream_index,
                                    -1, starts, ends, n);
    }
    free(starts);
    free(ends);
    if (!started)
        toast("AUTO-SYNC", "Couldn't auto-sync - adjust manually");
}

void prospero_subtitle_autosync_pump(void)
{
    evo_subsync_result_t r;
    if (!evo_subsync_take_result(&r))
        return;
    if (r.status == EVO_SUBSYNC_CANCELLED)
        return;
    int current_track = prospero_subtitle_use_external
                      ? -1 : prospero_embedded_subtitle_stream_index;
    if (current_track != prospero_subtitle_autosync_track)
        return;                    /* the track changed under the run */
    if (r.status != EVO_SUBSYNC_OK ||
        (current_track < 0 && !prospero_subtitle_count)) {
        toast("AUTO-SYNC", "Couldn't auto-sync - adjust manually");
        return;
    }

    double ms = r.delay_s * 1000.0;
    int delay = (int)(ms + (ms >= 0.0 ? 0.5 : -0.5));
    if (delay < -PROSPERO_SUBTITLE_MAX_DELAY_MS) delay = -PROSPERO_SUBTITLE_MAX_DELAY_MS;
    if (delay > PROSPERO_SUBTITLE_MAX_DELAY_MS) delay = PROSPERO_SUBTITLE_MAX_DELAY_MS;
    prospero_subtitle_delay_ms = delay;
    if (current_track < 0)         /* ratios are measured for the SRT only */
        prospero_subtitle_time_scale = r.scale;

    char msg[96];
    const char *ratio = evo_subsync_ratio_label(r.scale);
    if (ratio)
        snprintf(msg, sizeof(msg), "%+.1f s, %s", delay / 1000.0, ratio);
    else if (delay > -100 && delay < 100)
        snprintf(msg, sizeof(msg), "Already in sync");
    else
        snprintf(msg, sizeof(msg), "Subtitles shifted %+.1f s", delay / 1000.0);
    toast("AUTO-SYNC", msg);
    controls_last_used_ms = now_ms();
}

const char *prospero_subtitle_autosync_detail(char *buf, size_t size)
{
    if (evo_subsync_running()) {
        snprintf(buf, size, "ANALYSING... %d%%", evo_subsync_progress());
    } else if (!evo_subsync_path_supported(current_media_path)) {
        snprintf(buf, size, "LOCAL FILES ONLY");
    } else if (!prospero_subtitle_autosync_available()) {
        snprintf(buf, size, "SELECT A TRACK");
    } else if (prospero_subtitle_use_external && prospero_subtitle_time_scale != 1.0 &&
               evo_subsync_ratio_label(prospero_subtitle_time_scale)) {
        snprintf(buf, size, "%s", evo_subsync_ratio_label(prospero_subtitle_time_scale));
    } else {
        snprintf(buf, size, "MATCH TO AUDIO");
    }
    return buf;
}

/* PROSPERO_SUBTITLE_AUTOSYNC_END */
