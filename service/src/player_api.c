/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * The mailbox between Nuvio's page and the Nuvio app's native player.
 *
 * The app cannot listen on a socket (sandbox), and the page is gone while a
 * stream plays (the browser dialog is closed), so both talk to this instead:
 *
 *   POST /api/player/play    page: queue a stream {url, title, headers, startPosition, ...}
 *   GET  /api/player/next    app:  take the queued stream (204 when none)
 *   POST /api/player/state   app:  {id, state, position, duration, error}
 *   GET  /api/player/result  page: take the last finished playback (204 when none)
 *   GET  /api/player/status  page: is the native player there (the app polls
 *                            while the page is shown), and what it last said
 */
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "nuvio.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static json_t *g_pending;      /* queued request, with its id */
static json_t *g_last_state;   /* latest state the app reported */
static json_t *g_result;       /* latest final state, until the page takes it */
static time_t g_last_poll;     /* when the app last asked for work */
/* The page's own "context" from the play request (its route stack, profile
 * and progress identity). The app never sees it; it rides back on the final
 * result so the page, reloaded, can carry on where it was. */
static json_t *g_context;
static char g_context_id[32];
static unsigned g_counter;

static int app_present_locked(void) {
  return g_last_poll && time(NULL) - g_last_poll <= 3;
}

/* LAN debug only: keep the last request so a stream can be replayed from the
 * Mac. It holds the stream URLs, so it is written only while the debug flag
 * exists and served through the flag-gated /api/debug/ routes. */
#define LAST_REQUEST_PATH NUVIO_DATA_DIR "/debug-last-request.json"

static void keep_last_request(const char *body, size_t len) {
  struct stat st;
  if (stat(NUVIO_LAN_DEBUG_FLAG, &st) != 0)
    return;
  FILE *f = fopen(LAST_REQUEST_PATH, "wb");
  if (!f)
    return;
  fwrite(body, 1, len, f);
  fclose(f);
}

char *nuvio_debug_last_request(size_t *len) {
  FILE *f = fopen(LAST_REQUEST_PATH, "rb");
  char *buf = NULL;
  long n;
  *len = 0;
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
      (buf = malloc((size_t)n)) && fread(buf, 1, (size_t)n, f) == (size_t)n) {
    *len = (size_t)n;
  } else {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  return buf;
}

/*
 * The page's context for the playback in progress, and its finished result,
 * also live on disk. Both used to be memory only, so a payload restart while
 * a stream played (an update, a crash) lost them: the page reopened with no
 * route stack and fell through to the profile picker instead of going back
 * to the stream list.
 */
#define SESSION_PATH NUVIO_DATA_DIR "/player-session.json"
#define RESULT_PATH  NUVIO_DATA_DIR "/player-result.json"

static void session_save_locked(void) {
  json_t *o = json_object();
  json_object_set_new(o, "id", json_string(g_context_id));
  if (g_context)
    json_object_set(o, "context", g_context);
  json_dump_file(o, SESSION_PATH, JSON_COMPACT);
  json_decref(o);
}

/* The context for request `id`, from memory or from the file. */
static json_t *session_context_locked(const char *id) {
  if (!id)
    return NULL;
  if (g_context && !strcmp(id, g_context_id))
    return g_context;
  json_t *o = json_load_file(SESSION_PATH, 0, NULL);
  const char *sid = json_string_value(json_object_get(o, "id"));
  if (sid && !strcmp(sid, id) && json_object_get(o, "context")) {
    json_decref(g_context);
    g_context = json_incref(json_object_get(o, "context"));
    snprintf(g_context_id, sizeof(g_context_id), "%s", sid);
  }
  json_decref(o);
  return g_context && !strcmp(id, g_context_id) ? g_context : NULL;
}

char *nuvio_player_play(const char *body, size_t len, int *status) {
  json_error_t error;
  json_t *req = body && len ? json_loadb(body, len, 0, &error) : NULL;
  const char *url = json_string_value(json_object_get(req, "url"));
  char id[32];
  char *reply;
  int present;

  if (!json_is_object(req) || !url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) {
    json_decref(req);
    *status = 400;
    return strdup("{\"returnValue\":false,\"errorText\":\"An http(s) url is required\"}");
  }
  keep_last_request(body, len);
  pthread_mutex_lock(&g_lock);
  snprintf(id, sizeof(id), "%lx-%u", (unsigned long)time(NULL), ++g_counter);
  json_object_set_new(req, "id", json_string(id));
  json_decref(g_context);
  g_context = json_incref(json_object_get(req, "context"));
  snprintf(g_context_id, sizeof(g_context_id), "%s", id);
  json_object_del(req, "context");
  session_save_locked();
  remove(RESULT_PATH);
  json_decref(g_pending);
  g_pending = req;
  json_decref(g_result);
  g_result = NULL;
  present = app_present_locked();
  pthread_mutex_unlock(&g_lock);

  nuvio_log("player: queued %s %.120s", id, url);
  *status = 200;
  if (asprintf(&reply, "{\"returnValue\":true,\"id\":\"%s\",\"nativePlayer\":%s}", id,
               present ? "true" : "false") < 0)
    return NULL;
  return reply;
}

/* NULL when nothing is queued. */
char *nuvio_player_next(void) {
  char *out = NULL;

  pthread_mutex_lock(&g_lock);
  g_last_poll = time(NULL);
  if (g_pending) {
    out = json_dumps(g_pending, JSON_COMPACT);
    nuvio_log("player: app took %s", json_string_value(json_object_get(g_pending, "id")));
    json_decref(g_pending);
    g_pending = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return out;
}

int nuvio_player_state(const char *body, size_t len) {
  json_error_t error;
  json_t *state = body && len ? json_loadb(body, len, 0, &error) : NULL;
  const char *name = json_string_value(json_object_get(state, "state"));

  if (!json_is_object(state) || !name) {
    json_decref(state);
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  json_decref(g_last_state);
  g_last_state = json_incref(state);
  if (strcmp(name, "playing")) {
    const char *sid = json_string_value(json_object_get(state, "id"));
    json_t *context = session_context_locked(sid);
    json_decref(g_result);
    g_result = json_copy(state);
    if (g_result && context)
      json_object_set(g_result, "context", context);
    if (g_result)
      json_dump_file(g_result, RESULT_PATH, JSON_COMPACT);
    nuvio_log("player: %s %s at %.1f/%.1f%s%s", json_string_value(json_object_get(state, "id")),
              name, json_number_value(json_object_get(state, "position")),
              json_number_value(json_object_get(state, "duration")),
              json_object_get(state, "error") ? " - " : "",
              json_object_get(state, "error") ? json_string_value(json_object_get(state, "error")) : "");
  }
  pthread_mutex_unlock(&g_lock);
  json_decref(state);
  return 0;
}

/* NULL when there is no unread result. */
char *nuvio_player_result(void) {
  char *out = NULL;

  pthread_mutex_lock(&g_lock);
  if (!g_result) {
    /* Posted before a restart - but only a fresh one: a result nobody read
     * must not turn tomorrow's launch into a "return from the player". */
    struct stat st;
    if (stat(RESULT_PATH, &st) == 0 && time(NULL) - st.st_mtime < 600)
      g_result = json_load_file(RESULT_PATH, 0, NULL);
  }
  remove(RESULT_PATH);
  if (g_result) {
    out = json_dumps(g_result, JSON_COMPACT);
    json_decref(g_result);
    g_result = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  return out;
}

char *nuvio_player_status(void) {
  json_t *o = json_object();
  char *out;

  pthread_mutex_lock(&g_lock);
  json_object_set_new(o, "returnValue", json_true());
  json_object_set_new(o, "nativePlayer", json_boolean(app_present_locked()));
  json_object_set_new(o, "pending", json_boolean(g_pending != NULL));
  if (g_last_state)
    json_object_set(o, "last", g_last_state);
  pthread_mutex_unlock(&g_lock);
  out = json_dumps(o, JSON_COMPACT);
  json_decref(o);
  return out;
}

/*
 * Commands for the app, queued by the page (or, with the lan-debug flag, by a
 * test machine): {"cmd":"key","button":"cross","hold_ms":0}, {"cmd":"stop"},
 * {"cmd":"quit"}, {"cmd":"shot"}. The app takes them with GET
 * /api/player/control, every few frames while it plays and every poll while
 * the page is up.
 */
static json_t *g_commands;

int nuvio_player_control_post(const char *body, size_t len) {
  json_error_t error;
  json_t *cmd = body && len ? json_loadb(body, len, 0, &error) : NULL;

  if (!json_is_object(cmd) || !json_string_value(json_object_get(cmd, "cmd"))) {
    json_decref(cmd);
    return -1;
  }
  pthread_mutex_lock(&g_lock);
  if (!g_commands)
    g_commands = json_array();
  if (json_array_size(g_commands) < 64)
    json_array_append(g_commands, cmd);
  pthread_mutex_unlock(&g_lock);
  nuvio_log("player: command %s", json_string_value(json_object_get(cmd, "cmd")));
  json_decref(cmd);
  return 0;
}

/* NULL when nothing is queued; otherwise a JSON array, and the queue is empty. */
char *nuvio_player_control_take(void) {
  char *out = NULL;

  pthread_mutex_lock(&g_lock);
  g_last_poll = time(NULL);
  if (g_commands && json_array_size(g_commands)) {
    out = json_dumps(g_commands, JSON_COMPACT);
    json_array_clear(g_commands);
  }
  pthread_mutex_unlock(&g_lock);
  return out;
}

/* The latest screenshot the app posted (debug builds of the test loop). */
static void *g_shot;
static size_t g_shot_len;

void nuvio_debug_shot_store(const void *data, size_t len) {
  void *copy = len ? malloc(len) : NULL;

  if (len && !copy)
    return;
  if (copy)
    memcpy(copy, data, len);
  pthread_mutex_lock(&g_lock);
  free(g_shot);
  g_shot = copy;
  g_shot_len = copy ? len : 0;
  pthread_mutex_unlock(&g_lock);
  nuvio_log("debug: screenshot stored (%zu bytes)", len);
}

void *nuvio_debug_shot_copy(size_t *len) {
  void *out = NULL;

  pthread_mutex_lock(&g_lock);
  if (g_shot && (out = malloc(g_shot_len)))
    memcpy(out, g_shot, g_shot_len);
  *len = out ? g_shot_len : 0;
  pthread_mutex_unlock(&g_lock);
  return out;
}
