#pragma once
/*
 * The app's line to the nuvio-ps5 payload on 127.0.0.1:17600.
 *
 * The app sandbox cannot listen on a socket, but it can connect out, so the
 * payload keeps a small mailbox between Nuvio's page and the app:
 *
 *   page -> POST /api/player/play     (payload stores the request)
 *   app  -> GET  /api/player/next     (takes it; 204 when there is none)
 *   app  -> POST /api/player/state    (progress while playing, then the end)
 *   page -> GET  /api/player/result   (after the page reopens)
 *
 * Every call blocks for at most a couple of seconds and is only made from
 * places where that is fine (between frames on a loopback socket it is ~1 ms).
 */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUVIO_SERVICE_PORT 17600

/* 1 if the payload answers on its port. */
int nuvio_service_up(void);

/* 1 and *json (malloc'd; the caller frees it) when the page queued a stream,
 * 0 when nothing is waiting, -1 when the payload could not be reached. */
int nuvio_bridge_next(char **json);

/* Progress while playing: {id, state: "playing", position, duration}. */
void nuvio_bridge_state(const char *id, const char *state, double position,
                        double duration, const char *error);

/* The final result, as built by nuvio_result_json (state, action, tracks). */
void nuvio_bridge_state_json(const char *json);

/* A command queued for the app (GET /api/player/control): from the page, or
 * from a test machine while the console has /data/nuvio/lan-debug.
 *   key   button = cross|circle|square|triangle|up|down|left|right|options|
 *                  l1|r1|l2|r2, value = hold time in ms (0 = a tap)
 *   stop  end the playback as if Circle had been pressed on the controls
 *   quit  close the app cleanly (playback stopped, GPU idle, then exit)
 *   shot  post a screenshot of what is on screen to /api/debug/shot */
typedef struct nuvio_command {
    char cmd[16];
    char button[16];
    int  value;
} nuvio_command;

/* Fills up to max commands; returns how many (0 when none or unreachable). */
/* GET a payload path; returns the HTTP status, *body malloc'd (may be NULL). */
int nuvio_bridge_get(const char *path, char **body);
int nuvio_bridge_commands(nuvio_command *out, int max);

/* POSTs a binary body to the payload. 0 on success. */
int nuvio_bridge_post_blob(const char *path, const char *type, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
