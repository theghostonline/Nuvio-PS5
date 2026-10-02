/*
 * evo_stream_io.h — High-Throughput Streaming I/O Engine & Read-Ahead Buffer.
 *
 * Implements high-throughput asynchronous AVIO streaming buffers with 8MB–16MB
 * read-ahead ring buffering, sequential disk prefetching (posix_fadvise),
 * and direct memory buffer management for 100+ Mbps 4K REMUX media streams.
 */
#ifndef EVO_STREAM_IO_H
#define EVO_STREAM_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavformat/avformat.h>
#include <libavformat/avio.h>

typedef struct {
    size_t ring_buffer_size;  /* default 8 MiB (8 * 1024 * 1024) */
    size_t io_block_size;     /* default 512 KiB (512 * 1024) */
    int    enable_fadvise;    /* POSIX_FADV_SEQUENTIAL / WILLNEED */
} evo_stream_io_config_t;

typedef struct evo_stream_io_ctx evo_stream_io_ctx_t;

/**
 * Arm a wall-clock deadline on every blocking libavformat read made through
 * this context, via AVFormatContext.interrupt_callback.
 *
 * libavformat has no internal bound on how long a demuxer may spend inside a
 * single call. A Matroska file whose first block desyncs sends the demuxer
 * into matroska_resync(), which scans forward for the next Cluster ID; on a
 * multi-gigabyte REMUX that scan reads the whole file without ever returning a
 * packet, so avformat_find_stream_info() never returns and the calling thread
 * is gone for good (hardware, 2026-09-28: a DV P7 UHD REMUX, 85 streams, still
 * scanning 32 s and 1.2 GB in when the log was pulled).
 *
 * The callback is polled on each avio buffer refill, so the deadline bounds
 * the scan rather than the individual read.
 *
 * @param io_ctx   context from evo_stream_io_open(); NULL is a no-op
 * @param seconds  budget from now, or <= 0 to disarm (the playback reads that
 *                 follow the probe must never be interrupted)
 */
void evo_stream_io_set_deadline(evo_stream_io_ctx_t *io_ctx, double seconds);

/* Fail every blocking I/O call on this context from now on (stop/quit). */
void evo_stream_io_abort(evo_stream_io_ctx_t *ctx);

/**
 * Non-zero once an armed deadline has passed - i.e. the last libavformat call
 * returned because it was interrupted rather than because it finished.
 * Distinguishes "this file is malformed" from "this file needs longer".
 */
int evo_stream_io_deadline_expired(const evo_stream_io_ctx_t *io_ctx);

/**
 * Does this URL name an HLS or DASH playlist / manifest (".m3u8", ".mpd",
 * ".ism/")? Judged from the text, case-insensitively, query string included -
 * the format is not known until the open has happened.
 */
int evo_stream_io_url_is_playlist(const char *url);

/**
 * The FFmpeg options a network open gets: bounded reconnects, 5 s timeouts, the
 * HLS segment allowlist widened - and reconnect_at_eof for a raw stream but NOT
 * for a playlist, where it turns every small playlist into a reconnect loop
 * (see the .c). Public so the policy can be tested without a network.
 */
void evo_stream_io_apply_network_options(AVDictionary **opts, const char *url);

/**
 * Open a media stream with High-Throughput I/O ring buffering and direct memory.
 * Sets up custom AVIOContext with sequential kernel readahead and enlarged buffer.
 *
 * @param path          File path (e.g. /mnt/usb0/movie.mkv) or URL (http://...)
 * @param out_fmt_ctx   Pointer to AVFormatContext* to be populated
 * @param cfg           Optional config overrides (pass NULL for defaults)
 * @param out_io_ctx    Pointer to receive stream I/O context handle
 * @return 0 on success, negative error code on failure
 */
int evo_stream_io_open(const char *path,
                       AVFormatContext **out_fmt_ctx,
                       const evo_stream_io_config_t *cfg,
                       evo_stream_io_ctx_t **out_io_ctx);

/**
 * Close stream I/O context and release buffers.
 */
void evo_stream_io_close(evo_stream_io_ctx_t *io_ctx);

/**
 * Apply kernel sequential read-ahead hints on open file descriptors.
 */
void evo_stream_io_hint_sequential(int fd);

#ifdef __cplusplus
}
#endif

#endif /* EVO_STREAM_IO_H */
