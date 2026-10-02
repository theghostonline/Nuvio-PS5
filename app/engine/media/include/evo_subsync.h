/*
 * evo_subsync.h - auto-sync an external SRT to the film's audio (#102).
 *
 * ffsubsync-style, on device: a background worker opens its OWN
 * AVFormatContext on the media file (playback demux is never touched),
 * decodes three ~5 min windows of the selected audio stream (start / middle /
 * end), turns each into a 10 ms speech/no-speech bit vector, and slides the
 * cue timeline over it (bit covariance via popcount) to find the offset. A framerate
 * mismatch (a 25 fps SRT on a 23.976 fps release) is found by repeating the
 * search under each standard ratio and keeping the one whose windows agree.
 *
 * Embedded text tracks work too: pass the track's stream index and the
 * worker gathers its cues from the packets it reads around each window
 * (offset only - no ratio search for a track muxed with the video).
 *
 * Result model, matching what PlayerScreen does with it:
 *     subtitle_time = media_time * scale - delay
 * so a positive delay shows the subtitles later.
 *
 * This file knows nothing about the subtitle engine or the UI - evo_subtitle.c
 * snapshots the cues, starts the run, and applies the result on the UI thread
 * (prospero_subtitle_autosync_*). That keeps it linkable into the host harness
 * (tools/subsync_host.sh), which calls evo_subsync_analyse() directly.
 */
#ifndef EVO_SUBSYNC_H
#define EVO_SUBSYNC_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EVO_SUBSYNC_OK = 0,        /* confident result in delay_s / scale        */
    EVO_SUBSYNC_LOW_CONF,      /* analysed fine, no trustworthy match         */
    EVO_SUBSYNC_ERROR,         /* could not open / decode the audio           */
    EVO_SUBSYNC_CANCELLED
};

typedef struct {
    int    status;             /* EVO_SUBSYNC_*                               */
    double delay_s;            /* + = subtitles shown later                   */
    double scale;              /* subtitle seconds per media second, 1.0 = none */
    double confidence;         /* peak sharpness, in SDs of the curve (>= 0.35) */
    int    windows_used;       /* windows that agreed on the answer           */
    double elapsed_s;
} evo_subsync_result_t;

/* Synchronous analysis. audio_stream < 0 picks the best audio stream.
 * sub_stream >= 0 analyses that embedded subtitle track and ignores the cue
 * arrays; sub_stream < 0 uses cue_start/cue_end (subtitle-timeline seconds,
 * e.g. an external SRT). cancel is polled (non-zero aborts); progress (0-100)
 * is written if non-NULL. Returns out->status. */
int evo_subsync_analyse(const char *media_path, int audio_stream,
                        int sub_stream,
                        const double *cue_start, const double *cue_end,
                        int cue_count, volatile int *cancel,
                        volatile int *progress, evo_subsync_result_t *out);

/* ---- background worker (one run at a time) ---------------------------- */

/* Starts the worker, copying the cue times when sub_stream < 0. 0 if one is
 * already running, the path is not a local file, or there are too few cues. */
int  evo_subsync_start(const char *media_path, int audio_stream,
                       int sub_stream,
                       const double *cue_start, const double *cue_end,
                       int cue_count);
/* Asks the worker to stop and joins it. Drops any result not yet taken.
 * Safe to call when nothing is running. */
void evo_subsync_cancel(void);
int  evo_subsync_running(void);
int  evo_subsync_progress(void);       /* 0-100 while running                */
/* 1 once, with the finished run's result. UI thread only. */
int  evo_subsync_take_result(evo_subsync_result_t *out);

/* 1 for a path the worker can open itself (no "scheme://"). */
int  evo_subsync_path_supported(const char *media_path);

/* "25->23.976 fps" for a standard ratio, NULL for 1.0 / anything else. */
const char *evo_subsync_ratio_label(double scale);

#ifdef __cplusplus
}
#endif

#endif /* EVO_SUBSYNC_H */
