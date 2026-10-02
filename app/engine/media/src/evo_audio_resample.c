/*
 * evo_audio_resample.c — libswresample configuration for the audio-out path.
 *
 * Verbatim move of the PROSPERO_AUDIO_RESAMPLER_STATE region from main.c
 * (Track A step A2 of docs/modularisation-plan.md). No behaviour change: the
 * two globals the configure path used to read (`evo_audio_channels`,
 * `audio_ctx`) are now passed in as `out_channels` / `audio_ctx`, and the
 * output rate is the PROSPERO_AUDIO_OUTPUT_RATE macro instead of a file-scope
 * `static const int`. Everything else is byte-for-byte the original.
 */
#include "evo_audio_resample.h"

#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>

/* ---------------------------------------------------------------------------
 * EVO: surround output.
 *
 * Upstream hardcoded stereo everywhere - the AudioOut port was opened with
 * S16_STEREO and libswresample downmixed every layout to 2.0, so a 5.1 or 7.1
 * source lost its surround channels entirely.
 *
 * Verified on hardware (12.70, 5.1 system) by projects/audioout_test:
 *   - sceAudioOutOpen accepts S16_8CH (format 2) from a homebrew app slot
 *   - the PS5's channel interleave order matches FFmpeg's 7.1 order exactly:
 *         FL FR FC LFE BL BR SL SR
 *     confirmed one speaker at a time and with a clockwise rotation test
 *
 * The open port channel count (2 or 8) arrives as `out_channels`.
 * ------------------------------------------------------------------------ */

static SwrContext *prospero_audio_swr = NULL;

static AVChannelLayout prospero_audio_input_layout;
static int prospero_audio_input_layout_valid = 0;

static enum AVSampleFormat prospero_audio_input_format =
    AV_SAMPLE_FMT_NONE;

static int prospero_audio_input_rate = 0;
static const int prospero_audio_output_rate = PROSPERO_AUDIO_OUTPUT_RATE;


void prospero_audio_resampler_destroy(void) {
    if (prospero_audio_swr) {
        swr_free(&prospero_audio_swr);
    }

    if (prospero_audio_input_layout_valid) {
        av_channel_layout_uninit(
            &prospero_audio_input_layout
        );

        prospero_audio_input_layout_valid = 0;
    }

    prospero_audio_input_format =
        AV_SAMPLE_FMT_NONE;

    prospero_audio_input_rate = 0;
}


void prospero_audio_resampler_reset(void) {
    if (!prospero_audio_swr) {
        return;
    }

    swr_close(prospero_audio_swr);

    if (swr_init(prospero_audio_swr) < 0) {
        prospero_audio_resampler_destroy();
    }
}


int prospero_audio_resampler_configure(
    const AVFrame *frame,
    int out_channels,
    const AVCodecContext *audio_ctx
) {
    if (!frame) {
        return 0;
    }

    int input_rate =
        frame->sample_rate > 0
            ? frame->sample_rate
            : (
                audio_ctx &&
                audio_ctx->sample_rate > 0
                    ? audio_ctx->sample_rate
                    : 48000
            );

    AVChannelLayout input_layout;

    memset(
        &input_layout,
        0,
        sizeof(input_layout)
    );

    if (frame->ch_layout.nb_channels > 0) {
        if (
            av_channel_layout_copy(
                &input_layout,
                &frame->ch_layout
            ) < 0
        ) {
            return 0;
        }
    } else if (
        audio_ctx &&
        audio_ctx->ch_layout.nb_channels > 0
    ) {
        if (
            av_channel_layout_copy(
                &input_layout,
                &audio_ctx->ch_layout
            ) < 0
        ) {
            return 0;
        }
    } else {
        av_channel_layout_default(
            &input_layout,
            2
        );
    }

    int unchanged =
        prospero_audio_swr &&
        prospero_audio_input_layout_valid &&
        prospero_audio_input_format ==
            (enum AVSampleFormat)frame->format &&
        prospero_audio_input_rate ==
            input_rate &&
        av_channel_layout_compare(
            &prospero_audio_input_layout,
            &input_layout
        ) == 0;

    if (unchanged) {
        av_channel_layout_uninit(
            &input_layout
        );

        return 1;
    }

    prospero_audio_resampler_destroy();

    /* EVO: match the resampler's output layout to the port that is actually
     * open. Upstream always used AV_CHANNEL_LAYOUT_STEREO here, which is what
     * discarded the surround channels before they ever reached AudioOut.
     *
     * FFmpeg's 7.1 order (FL FR FC LFE BL BR SL SR) matches the PS5's
     * interleave order exactly - verified per speaker on hardware - so no
     * channel remapping is needed. */
    AVChannelLayout output_layout =
        (out_channels == 8)
            ? (AVChannelLayout)AV_CHANNEL_LAYOUT_7POINT1
            : (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;

    if (
        swr_alloc_set_opts2(
            &prospero_audio_swr,
            &output_layout,
            AV_SAMPLE_FMT_S16,
            prospero_audio_output_rate,
            &input_layout,
            (enum AVSampleFormat)frame->format,
            input_rate,
            0,
            NULL
        ) < 0
    ) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    av_opt_set_double(
        prospero_audio_swr,
        "rematrix_maxval",
        1.0,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "center_mix_level",
        0.70710678,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "surround_mix_level",
        0.70710678,
        0
    );

    av_opt_set_double(
        prospero_audio_swr,
        "lfe_mix_level",
        0.5,
        0
    );

    if (swr_init(prospero_audio_swr) < 0) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    if (
        av_channel_layout_copy(
            &prospero_audio_input_layout,
            &input_layout
        ) < 0
    ) {
        av_channel_layout_uninit(
            &input_layout
        );

        prospero_audio_resampler_destroy();
        return 0;
    }

    prospero_audio_input_layout_valid = 1;

    prospero_audio_input_format =
        (enum AVSampleFormat)frame->format;

    prospero_audio_input_rate =
        input_rate;

    av_channel_layout_uninit(
        &input_layout
    );

    return 1;
}


SwrContext *prospero_audio_resampler_ctx(void) {
    return prospero_audio_swr;
}


int prospero_audio_resampler_input_rate(void) {
    return prospero_audio_input_rate;
}
