/*
 * evo_hls_variants.h - the quality variants inside an HLS master playlist.
 *
 * An HLS channel is usually one URL that names a MASTER playlist: a short file
 * listing the same programme at several resolutions and bitrates
 * (#EXT-X-STREAM-INF), each pointing at its own media playlist. EVO's player
 * picks one of them itself (the best it can see). This reads that list so the
 * user can pick instead - see the stream picker on ProviderHostScreen.
 *
 * The parser is pure: text in, evo_stream_choice_t out, no network and no
 * allocation, so it is tested on the host (tools/hls_host.sh). The fetch is the
 * only part that touches evo_net.
 */
#ifndef EVO_HLS_VARIANTS_H
#define EVO_HLS_VARIANTS_H

#include <stddef.h>

#include "evo_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The most variants read from one master. The stream picker lists nine rows in
 * all: the URL as listed, these, and the extension guess. */
#define EVO_HLS_MAX_VARIANTS 6

/*
 * Resolves `ref` against `base` (RFC 3986, the parts a playlist uses):
 * an absolute URL is kept, "//host/x" takes base's scheme, "/x" takes its
 * scheme and authority, "?q" replaces the query, anything else is relative to
 * base's directory, and "." / ".." segments are removed.
 * Returns 0, or -1 if `ref` is relative and `base` is not a URL, or the result
 * does not fit `out_size`.
 */
int evo_hls_join_url(const char *base, const char *ref, char *out, size_t out_size);

/*
 * Reads the variants out of a master playlist. `base_url` is the playlist's own
 * URL, which its relative variant URIs are resolved against.
 *
 * Fills up to `max` choices, best first (highest resolution, then bitrate;
 * audio-only last), with url, label ("1080p", "720p", "2.4 Mbps", "Audio
 * only"), container "hls", width, height, bitrate_bps and the video / audio
 * codec names when CODECS says. Duplicate URLs are dropped and I-frame-only
 * playlists are skipped.
 *
 * Returns how many. 0 means this is not a master playlist - a media playlist
 * (#EXTINF, no #EXT-X-STREAM-INF) has nothing to choose - or nothing parsed.
 */
int evo_hls_parse_master(const char *body, size_t len, const char *base_url,
                         evo_stream_choice_t *out, int max);

/*
 * Delivered on the main thread from evo_net_poll(). `count` is 0 when the fetch
 * failed, the server refused (HTTP 403 is common on signed channels), or the
 * file is not a master playlist. `variants` is valid only during the call.
 */
typedef void (*evo_hls_variants_cb)(int count, const evo_stream_choice_t *variants, void *ud);

/*
 * Fetches `master_url` and parses it. Returns 0 if queued - the callback then
 * fires exactly once - or negative, in which case it never fires.
 */
int evo_hls_variants_fetch(const char *master_url, evo_hls_variants_cb cb, void *ud);

#ifdef __cplusplus
}
#endif

#endif /* EVO_HLS_VARIANTS_H */
