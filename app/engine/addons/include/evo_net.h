/*
 * evo_net.h — Non-blocking HTTP/REST client and network task queue for EVO Player.
 *
 * Implements a lightweight, thread-safe HTTP/REST client over standard BSD sockets.
 * All asynchronous requests execute on a background worker thread and pump their
 * completion callbacks on the main UI thread via evo_net_poll(), preventing network
 * latency from dropping frames on the 60fps UI loop.
 *
 * Sizing Limits and Boundaries:
 *   - EVO_NET_MAX_HOST (256): Maximum host name length including null terminator.
 *   - EVO_NET_MAX_PATH (2048): Maximum path and query string buffer size. Provider
 *     catalog queries (e.g. Emby /Items with item fields and tokens) routinely
 *     exceed 256 characters; truncation is strictly treated as an error (-13).
 *   - EVO_NET_MAX_URL (2048): Maximum complete URL length.
 *   - EVO_NET_MAX_REDIRECTS (5): Maximum number of redirect hops followed.
 *   - EVO_NET_MAX_BODY (8 MiB): Hard response body cap to protect memory on
 *     constrained platforms (PS5). Provider media streams NEVER route through
 *     evo_net; this networking client is strictly for JSON metadata, M3U8/M3U
 *     playlists, and UI bundle files. Responses exceeding this cap return -17.
 *
 * Redirect Behavior:
 *   - Follows up to EVO_NET_MAX_REDIRECTS (5) HTTP redirects (301, 302, 303, 307, 308).
 *   - Location header parsed case-insensitively.
 *   - Supports absolute (http://, https://) and root-relative (/path) targets.
 *   - Status 303, and status 301/302 on a POST request, automatically switch the method
 *     to GET and drop the request payload. Status 307 and 308 preserve method and body.
 *   - Insecure downgrades from HTTPS to HTTP are strictly rejected (-15) to prevent
 *     accidental credential/token leakage.
 *
 * Chunked Transfer Decoding:
 *   - Responses with 'Transfer-Encoding: chunked' are transparently decoded in-place.
 *   - Chunk extensions and trailing headers are handled according to RFC specifications.
 *   - Any malformed chunk stream is treated as an error (-16) rather than returning a
 *     partial, corrupted payload to the caller.
 *
 * Connection Lifecycle:
 *   - Requests specify 'Connection: close'. To prevent rogue or misconfigured servers
 *     from stalling the client for the full timeout window (6 seconds), response reading
 *     terminates as soon as 'Content-Length' bytes have been received.
 *
 * Asynchronous Return Codes (evo_net_request_async):
 *    0 (EVO_NET_ASYNC_OK)        : Request successfully enqueued onto worker queue.
 *   -1 (EVO_NET_ASYNC_ERR_INIT)  : Network worker thread initialization failed.
 *   -2 (EVO_NET_ASYNC_ERR_MEM)   : Memory allocation failed for request or headers.
 *   -3 (EVO_NET_ASYNC_ERR_FULL)  : Request queue is full (EVO_NET_MAX_QUEUE reached);
 *                                  rejected immediately to prevent stalling caller thread.
 *   -4 (EVO_NET_ASYNC_ERR_ARG)   : Invalid argument (NULL parameter, or URL/method too long).
 *
 * Synchronous Return Codes (evo_net_http_get_sync, evo_net_http_post_sync):
 *    0 (EVO_NET_OK)              : Success (HTTP status 2xx or 3xx resolved).
 *   -1 (EVO_NET_ERR_INVALID_URL) : Invalid or unsupported URL scheme (not http/https).
 *   -2 (EVO_NET_ERR_DNS)         : DNS host resolution failed.
 *   -3 (EVO_NET_ERR_CONNECT)     : Socket connection to host failed.
 *   -4 (EVO_NET_ERR_SEND_HDR)    : Failed to transmit HTTP request headers.
 *   -5 (EVO_NET_ERR_SEND_BODY)   : Failed to transmit HTTP request body.
 *   -6 (EVO_NET_ERR_MEM)         : Memory allocation failed for response buffer.
 *   -7 (EVO_NET_ERR_REALLOC)     : Memory reallocation failed expanding response buffer.
 *   -8 (EVO_NET_ERR_HTTP)        : HTTP error status received (status < 200 or status >= 400).
 *  -10 (EVO_NET_ERR_SSL_CTX)     : SSL context initialization failed or OpenSSL unavailable.
 *  -11 (EVO_NET_ERR_SSL_NEW)     : SSL session allocation failed.
 *  -12 (EVO_NET_ERR_SSL_CONN)    : SSL handshake failed.
 *  -13 (EVO_NET_ERR_TOO_LONG)    : URL, host, or path exceeded maximum allowed size.
 *  -14 (EVO_NET_ERR_REDIRECTS)   : Exceeded maximum allowed redirect hops (5).
 *  -15 (EVO_NET_ERR_DOWNGRADE)   : Insecure redirect downgrade (HTTPS to HTTP) rejected.
 *  -16 (EVO_NET_ERR_CHUNKED)     : Malformed HTTP chunked-transfer encoding stream.
 *  -17 (EVO_NET_ERR_BODY_LIMIT)  : Response body exceeded hard limit (EVO_NET_MAX_BODY).
 */
#ifndef EVO_NET_H
#define EVO_NET_H

#include <stddef.h>
#include <netdb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_NET_MAX_HOST        256
#define EVO_NET_MAX_PATH        2048
#define EVO_NET_MAX_URL         2048
#define EVO_NET_MAX_REDIRECTS   5
#define EVO_NET_MAX_BODY        (64 * 1024 * 1024)

/* evo_net_request_async return codes */
#define EVO_NET_ASYNC_OK         0
#define EVO_NET_ASYNC_ERR_INIT  -1
#define EVO_NET_ASYNC_ERR_MEM   -2
#define EVO_NET_ASYNC_ERR_FULL  -3
#define EVO_NET_ASYNC_ERR_ARG   -4

/* Synchronous HTTP return codes */
#define EVO_NET_OK               0
#define EVO_NET_ERR_INVALID_URL -1
#define EVO_NET_ERR_DNS         -2
#define EVO_NET_ERR_CONNECT     -3
#define EVO_NET_ERR_SEND_HDR    -4
#define EVO_NET_ERR_SEND_BODY   -5
#define EVO_NET_ERR_MEM         -6
#define EVO_NET_ERR_REALLOC     -7
#define EVO_NET_ERR_HTTP        -8
#define EVO_NET_ERR_SSL_CTX    -10
#define EVO_NET_ERR_SSL_NEW    -11
#define EVO_NET_ERR_SSL_CONN   -12
#define EVO_NET_ERR_TOO_LONG   -13
#define EVO_NET_ERR_REDIRECTS  -14
#define EVO_NET_ERR_DOWNGRADE  -15
#define EVO_NET_ERR_CHUNKED    -16
#define EVO_NET_ERR_BODY_LIMIT -17

typedef void (*evo_net_cb)(int success, int status_code, const char *body, size_t body_len, void *user_data);

/*
 * Connects to the first address in `list` that answers, and returns the socket
 * (blocking, with EVO_NET_TIMEOUT_SEC read/write timeouts) or -1.
 *
 * Each address gets at most `per_addr_timeout_ms` (a non-blocking connect
 * bounded by poll). A plain blocking connect() to an address that silently drops
 * packets does not honour SO_SNDTIMEO here: it sits in the kernel's SYN retry
 * for about 75 s. That stalled evo_net's single worker thread - and every
 * request queued behind it - whenever a host had one dead address, which
 * raw.githubusercontent.com does from some networks (hardware, 2026-09-30).
 *
 * An address that timed out is remembered for five minutes and tried after the
 * ones that have not, so only the first request to a host pays for it.
 */
int evo_net_connect_list(const struct addrinfo *list, int per_addr_timeout_ms);

/* Initialize background network worker and request queue. */
int  evo_net_init(void);

/* Shutdown background network worker and clean up. */
void evo_net_shutdown(void);

/*
 * Pump completed network callbacks on the calling (main/UI) thread.
 * Call this once per frame in the main render/event loop.
 */
void evo_net_poll(void);

/*
 * Asynchronous HTTP/REST request. Returns 0 on queued, negative on error.
 * Callback is invoked on the main thread during evo_net_poll().
 */
int  evo_net_request_async(const char *method,
                           const char *url,
                           const char *post_data,
                           const char **headers,
                           int header_count,
                           evo_net_cb callback,
                           void *user_data);

/*
 * Synchronous HTTP GET request. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_get_sync(const char *url,
                           const char **headers,
                           int header_count,
                           char **out_body,
                           size_t *out_len,
                           int *out_status);

/*
 * Synchronous HTTP POST request with JSON or form payload. Blocks calling thread.
 * Caller must free(*out_body) if allocated.
 */
int  evo_net_http_post_sync(const char *url,
                            const char *post_data,
                            const char **headers,
                            int header_count,
                            char **out_body,
                            size_t *out_len,
                            int *out_status);

#ifdef __cplusplus
}
#endif

#endif /* EVO_NET_H */
