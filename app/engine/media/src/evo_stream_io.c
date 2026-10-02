/*
 * evo_stream_io.c — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput 8MB–16MB streaming buffers with sequential disk
 * prefetching (posix_fadvise) and optimized dictionary options for 100+ Mbps 4K media.
 */
#include "evo_stream_io.h"
#include "evo_direct_mem.h"

#ifdef EVO_APP_MODULE
extern void pp_stage_bc(const char *stage_id, const char *detail);
#  define SIO_BC(id, d) pp_stage_bc((id), (d))
#else
#  define SIO_BC(id, d) ((void)0)
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EVO_STREAM_DEFAULT_RING_SIZE (8 * 1024 * 1024) /* 8 MiB */

/*
 * How long avformat_open_input() may spend before the interrupt callback
 * unwinds it. Generous: a 4 MB probe over USB is well inside a second, and a
 * network open has its own 5 s "timeout" option, so anything approaching this
 * is a demuxer that has stopped making progress rather than slow storage.
 */
#define EVO_STREAM_IO_OPEN_DEADLINE_SEC 20.0

/*
 * ...but a raw network stream (not a playlist) gets less. Hardware, 2026-09-30: a live channel whose
 * server answered every request with the same 1665 bytes and then closed the
 * connection sat in FFmpeg's reconnect loop until this deadline - 20 s of a
 * spinner for a channel that was never going to play. Each reconnect there
 * succeeded (it got its 1665 bytes), so no retry limit inside FFmpeg ever trips;
 * only a wall clock bounds it. A healthy IPTV open (DNS, TLS, two playlists) is
 * about three seconds, so twelve is generous and still ends a dead channel in
 * a fraction of the time.
 */
#define EVO_STREAM_IO_NET_OPEN_DEADLINE_SEC 12.0

struct evo_stream_io_ctx {
    int     is_network;
    char    media_path[512];
    /* Wall-clock deadline for blocking libavformat reads, or 0 when disarmed.
     * See evo_stream_io_set_deadline() in the header for why this exists. */
    double  deadline_at;
    int     deadline_hit;
    volatile int aborted;      /* evo_stream_io_abort: fail every blocking call */
};

static double sio_now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/*
 * avio polls this on every buffer refill. Returning non-zero makes the read
 * fail with AVERROR_EXIT, which unwinds the demuxer out of whatever loop it is
 * in - including matroska_resync()'s byte-at-a-time forward scan.
 */
static int sio_interrupt_cb(void *opaque)
{
    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)opaque;
    if (ctx && ctx->aborted)
        return 1;
    if (!ctx || ctx->deadline_at <= 0.0)
        return 0;
    if (ctx->deadline_hit)
        return 1;
    if (sio_now_seconds() >= ctx->deadline_at) {
        ctx->deadline_hit = 1;
        return 1;
    }
    return 0;
}

/* Stop is waiting on the demux thread: make the read it is blocked in (or the
 * reconnect loop it is sitting out) return now instead of at rw_timeout. */
void evo_stream_io_abort(evo_stream_io_ctx_t *ctx)
{
    if (ctx)
        ctx->aborted = 1;
}

void evo_stream_io_set_deadline(evo_stream_io_ctx_t *ctx, double seconds)
{
    if (!ctx) return;
    if (seconds <= 0.0) {
        /* Disarm only. deadline_hit survives, because the caller reads it
         * after disarming to find out why the call it just made returned. */
        ctx->deadline_at = 0.0;
        return;
    }
    ctx->deadline_at  = sio_now_seconds() + seconds;
    ctx->deadline_hit = 0;
}

int evo_stream_io_deadline_expired(const evo_stream_io_ctx_t *ctx)
{
    return ctx ? ctx->deadline_hit : 0;
}

void evo_stream_io_hint_sequential(int fd)
{
    if (fd < 0) return;
    /* posix_fadvise is not wired on the PS5 kernel from the app-module
     * process - it faults (SIGSYS-class, same as exit()). It is only a
     * read-ahead hint, so skip it there. Hardware-confirmed 2026-09-02:
     * the crash on file-open was here, between P8_01c and P8_01d. */
#ifndef EVO_APP_MODULE
#if defined(POSIX_FADV_SEQUENTIAL)
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#if defined(POSIX_FADV_WILLNEED)
    posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
#endif
#endif
}

static void prefetch_file_sequential(const char *path)
{
    if (!path || strncmp(path, "http://", 7) == 0 || strncmp(path, "https://", 8) == 0)
        return;
#ifndef EVO_APP_MODULE
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        evo_stream_io_hint_sequential(fd);
        close(fd);
    }
#endif
}

/*
 * The frame rate a raw .obu's file name states, as an AVRational string, or 0.
 * Takes the digits before "fps" ("2397fps", "23.976fps", "24fps"). Four
 * undotted digits are hundredths (Netflix's 2397 / 2997 / 5994), and anything
 * within 0.02 of an NTSC rate snaps to its exact n*1000/1001 form.
 */
static int raw_av1_rate_from_name(const char *path, char *out, size_t out_len)
{
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".obu") != 0)
        return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    for (const char *p = strstr(base, "fps"); p; p = strstr(p + 1, "fps")) {
        const char *q = p;
        while (q > base && ((q[-1] >= '0' && q[-1] <= '9') || q[-1] == '.'))
            q--;
        if (q == p)
            continue;
        char num[16];
        size_t n = (size_t)(p - q);
        if (n >= sizeof num)
            continue;
        memcpy(num, q, n);
        num[n] = 0;
        double v = atof(num);
        if (!strchr(num, '.') && n == 4)
            v /= 100.0;
        if (v < 1.0 || v > 240.0)
            continue;
        static const int ntsc[] = { 24, 30, 48, 60, 120 };
        for (size_t i = 0; i < sizeof ntsc / sizeof ntsc[0]; i++) {
            double r = ntsc[i] * 1000.0 / 1001.0;
            if (v > r - 0.02 && v < r + 0.02) {
                snprintf(out, out_len, "%d/1001", ntsc[i] * 1000);
                return 1;
            }
        }
        snprintf(out, out_len, "%d/1000", (int)(v * 1000.0 + 0.5));
        return 1;
    }
    return 0;
}

/* Case-insensitive substring search. (libc's strcasestr is one of the calls the
 * native-app libc leaves as a NULL import, so it is not used.) */
static int sio_contains_ci(const char *hay, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0) return 1;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0)
            return 1;
    }
    return 0;
}

int evo_stream_io_url_is_playlist(const char *url)
{
    if (!url) return 0;
    return sio_contains_ci(url, ".m3u8") || sio_contains_ci(url, ".mpd") ||
           sio_contains_ci(url, ".ism/");
}

#ifdef NUVIO_APP
/* Nuvio app: request headers the page sent with the stream (an addon's
 * Referer / User-Agent / Authorization), "Name: value\r\n" each. Set by the
 * player before it opens the URL; empty for a plain link. */
char nuvio_stream_headers[4096];
char nuvio_stream_user_agent[512];
#endif

void evo_stream_io_apply_network_options(AVDictionary **opts, const char *url)
{
#ifdef NUVIO_APP
    if (nuvio_stream_headers[0])
        av_dict_set(opts, "headers", nuvio_stream_headers, 0);
    if (nuvio_stream_user_agent[0])
        av_dict_set(opts, "user_agent", nuvio_stream_user_agent, 0);
#endif
    av_dict_set(opts, "reconnect", "1", 0);
    av_dict_set(opts, "reconnect_streamed", "1", 0);
    av_dict_set(opts, "reconnect_on_network_error", "1", 0);
    av_dict_set(opts, "reconnect_delay_max", "2", 0);
    av_dict_set(opts, "rw_timeout", "5000000", 0);
    av_dict_set(opts, "timeout", "5000000", 0);

    /*
     * Bounded retries. FFmpeg's default is to keep going for 256 s. These cap
     * one failing read at three reconnects and six seconds of waiting; the
     * network open deadline above caps the case they cannot (every reconnect
     * succeeds, and gets the same data again).
     */
    av_dict_set(opts, "reconnect_max_retries", "3", 0);
    av_dict_set(opts, "reconnect_delay_total_max", "6", 0);

    /*
     * Reconnect at EOF only for a raw stream.
     *
     * For a live MPEG-TS over HTTP an EOF is a dropped connection, and
     * reconnecting is right. For an HLS or DASH playlist it is the end of a
     * small file that the server never said the length of: the http layer
     * "reconnects", fetches the same bytes again, hits EOF again, and repeats
     * with a growing delay - a full request and TLS handshake each time, on
     * every playlist the demuxer reads. libavformat's hls demuxer hands these
     * options to every playlist and segment it opens. Hardware, 2026-09-30, from
     * the log:
     *
     *   Will reconnect at 182 in 0 second(s), error=End of file.   (x6, 2 s)
     *   ... open took 10.8 s   (a 182-byte master playlist)
     *   Will reconnect at 1023 in 0 second(s), error=End of file.  (x18, 18 s)
     *   ... open took 21.4 s and delivered no packets
     *
     * The hls demuxer already reloads a playlist and retries a segment itself.
     */
    if (!evo_stream_io_url_is_playlist(url))
        av_dict_set(opts, "reconnect_at_eof", "1", 0);

    /*
     * HLS: accept every segment URL the playlist names.
     *
     * libavformat's hls demuxer refuses a segment whose URL does not end
     * in an extension on its allowlist, and the default list is short.
     * Plenty of live CDNs serve segments from extensionless, signed or
     * query-string URLs, and the refusal is not survivable: the segment
     * fetch fails, the media playlist behind it fails to parse, and the
     * open completes with nb_streams=0 - a channel that sits on a black
     * screen with no error. Hardware, 2026-09-28:
     *
     *   URL .../v1/segment/<token>/0/186944902 is not in allowed_extensions
     *   parse_playlist error Invalid data found when processing input
     *   P8_02b_FIND_INFO_RC rc=0 timeout=0 nb_streams=0
     *
     * The allowlist is a guard against a playlist naming a local path; a
     * media player asked to open a URL the user chose has already made
     * that decision, and every other player ships with this widened.
     */
    av_dict_set(opts, "allowed_extensions", "ALL", 0);

    /*
     * ...and stop checking the segment's extension against its detected format.
     *
     * "ALL" clears only the first of two gates in hls.c's test_segment(). The
     * second - `extension_picky`, on by default - compares the URL's extension
     * with what the segment turned out to be, and an extensionless URL has none
     * to compare, so a segment that is plainly MPEG-TS is refused anyway:
     *
     *   detected format mpegts extension none mismatches allowed extensions in
     *   url https://.../v1/segment/<token>/3/186971110
     *   Error when loading first segment ...
     *
     * Hardware, 2026-09-30: an Akamai channel whose variant playlist opened with
     * AVERROR_INVALIDDATA after 8 s. Same class of channel as the one above.
     */
    av_dict_set(opts, "extension_picky", "0", 0);
}

int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx)
{
    if (!path || !out_fmt_ctx) return -1;

    SIO_BC("P8_01a_SIO_ENTER", path);
    evo_stream_io_ctx_t *ctx = (evo_stream_io_ctx_t *)evo_direct_mem_calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) ctx = (evo_stream_io_ctx_t *)calloc(1, sizeof(evo_stream_io_ctx_t));
    if (!ctx) return -1;
    SIO_BC("P8_01b_SIO_CTX", "ctx alloc ok");

    ctx->is_network = (strncmp(path, "http://", 7) == 0 ||
                       strncmp(path, "https://", 8) == 0 ||
                       strncmp(path, "ftp://", 6) == 0 ||
                       strncmp(path, "smb://", 6) == 0);
    snprintf(ctx->media_path, sizeof(ctx->media_path), "%s", path);

    size_t ring_size = (cfg && cfg->ring_buffer_size > 0) ? cfg->ring_buffer_size : EVO_STREAM_DEFAULT_RING_SIZE;
    char buf_size_str[32];
    snprintf(buf_size_str, sizeof(buf_size_str), "%zu", ring_size);

    AVDictionary *opts = NULL;

    /*
     * Fast probe & high-throughput streaming buffer options.
     *
     * analyzeduration is 4 s, not the 2 s this file used while it had no
     * callers: PlaybackController's own open (the one that actually shipped)
     * used 4 s, and #90 routes that open through here. Halving it would change
     * stream detection on awkward MPEG-TS - the files where a second audio
     * track appears late - as a side effect of a refactor, which is the kind of
     * regression that gets blamed on the codec pass rather than on this line.
     */
    av_dict_set(&opts, "probesize", "4194304", 0);
    av_dict_set(&opts, "analyzeduration", "4000000", 0);
    av_dict_set(&opts, "buffer_size", buf_size_str, 0);

    if (ctx->is_network) {
        evo_stream_io_apply_network_options(&opts, path);
    } else {
        /* Prime the kernel storage controller for sequential read-ahead */
        SIO_BC("P8_01c_PREFETCH", "open+fadvise");
        prefetch_file_sequential(path);
    }

    /* A raw AV1 .obu carries no timestamps, and the obu demuxer invents them
     * from its `framerate` option - 25 by default. 23.976 fps film then plays
     * 4% fast (#94, Chimera on hardware 2026-09-25). The sequence header's
     * timing_info is optional and Netflix's Open Content streams omit it; the
     * name is the one place the rate is written down ("...-2397fps-..."). */
    if (!ctx->is_network) {
        char rate[16];
        if (raw_av1_rate_from_name(path, rate, sizeof rate)) {
            av_dict_set(&opts, "framerate", rate, 0);
            SIO_BC("P8_01c2_OBU_RATE", rate);
        }
    }

    SIO_BC("P8_01d_PRE_OPEN", "-> avformat_open_input");

    /*
     * Allocate the context here rather than letting avformat_open_input do it,
     * so interrupt_callback is in place before the first byte is read. The
     * open itself gets the same deadline as the probe: a header parse that
     * desyncs scans just as far as a probe that does.
     */
    AVFormatContext *fmt = avformat_alloc_context();
    if (!fmt) {
        av_dict_free(&opts);
        evo_direct_mem_free(ctx);
        return -1;
    }
    fmt->interrupt_callback.callback = sio_interrupt_cb;
    fmt->interrupt_callback.opaque   = ctx;
    /* A playlist open is genuinely longer - the master, a variant, and the first
     * segment all load before it returns - and a slow origin can spend ten seconds
     * on them. It keeps the full budget; the reconnect loop that used to burn it
     * is gone (see evo_stream_io_apply_network_options). Only a raw stream, whose
     * failure mode is looping on the same few bytes, gets the short one. */
    evo_stream_io_set_deadline(ctx, (ctx->is_network && !evo_stream_io_url_is_playlist(path))
                                        ? EVO_STREAM_IO_NET_OPEN_DEADLINE_SEC
                                        : EVO_STREAM_IO_OPEN_DEADLINE_SEC);

    int rc = avformat_open_input(&fmt, path, NULL, &opts);
    /* avformat_open_input frees and NULLs *fmt on failure, including on an
     * interrupted open, so there is nothing left to clean up here. */
    evo_stream_io_set_deadline(ctx, 0.0);
    {
        char d[32]; snprintf(d, sizeof d, "rc=%d", rc);
        SIO_BC("P8_01e_OPEN_RC", d);
    }
    av_dict_free(&opts);

    if (rc < 0) {
        evo_direct_mem_free(ctx);
        return rc;
    }

    *out_fmt_ctx = fmt;
    if (out_io_ctx) *out_io_ctx = ctx;
    return 0;
}

void evo_stream_io_close(evo_stream_io_ctx_t *ctx)
{
    if (!ctx) return;
    evo_direct_mem_free(ctx);
}
