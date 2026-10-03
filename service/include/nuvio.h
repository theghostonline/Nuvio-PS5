/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Service version; bump with every payload release. */
#define NUVIO_PS5_VERSION "1.7.3"

/* Must match PS5_SERVICE_PORT in the web app (js/platform/ps5/ps5Service.js)
 * and the deeplink in assets/param.json. Never change it: the browser keys
 * Nuvio's saved settings and sign-in to this origin. */
#define NUVIO_PORT 17600

#define NUVIO_TITLE_ID "NUVI00001"
/* System host the full-screen shell runs in (hbldr). Distinct from the tile. */
#define NUVIO_HOST_TITLE_ID "NUVS00001"
#define NUVIO_THREAD_NAME "nuvio-ps5.elf"

#define NUVIO_DATA_DIR "/data/nuvio"
#define NUVIO_WWW_DIR NUVIO_DATA_DIR "/www"
#define NUVIO_LOG_PATH NUVIO_DATA_DIR "/nuvio-service.log"
#define NUVIO_APP_LOG_PATH NUVIO_DATA_DIR "/nuvio.log"
#define NUVIO_STORAGE_PATH NUVIO_DATA_DIR "/storage.json"
/* Drop a web build here (index.html at its root) to override the embedded one. */
#define NUVIO_WWW_OVERRIDE_DIR NUVIO_DATA_DIR "/www-dev"

/* log.c */
void nuvio_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Console clock vs real time (clock.c). */
void nuvio_clock_start(void);
char *nuvio_clock_json(void);
void nuvio_app_log_append(const char *line, size_t len);

/* notify.c */
void nuvio_notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* fsutil.c */
int nuvio_mkdir_p(const char *path, int mode);
int nuvio_write_file(const char *path, const void *data, size_t size, int mode);
int nuvio_file_equals(const char *path, const void *data, size_t size);
int nuvio_rm_rf(const char *path);
/* Reads a whole file into a malloc'd buffer (used by hbldr). */
uint8_t *fs_readfile(const char *path, size_t *size);

/* www.c: unpacks the embedded web app; returns the directory to serve. */
const char *nuvio_www_prepare(void);
int nuvio_extract_tgz(const uint8_t *data, size_t size, const char *dest);

/* app_install.c: the Nuvio app (PPSA99176, ../nuvio-app) for ShadowMountPlus.
 * Returns 0 installed or current, 1 deferred while an app runs, -1 failed. */
#define NUVIO_APP_TITLE_ID "PPSA99176"
#define NUVIO_APP_DIR "/data/homebrew/" NUVIO_APP_TITLE_ID
int nuvio_app_install_if_needed(void);

/* player_api.c: mailbox between Nuvio's page and the app's native player.
 * Returned strings are malloc'd JSON; NULL means "nothing" (204). */
char *nuvio_player_play(const char *body, size_t len, int *status);
char *nuvio_debug_last_request(size_t *len);
char *nuvio_player_next(void);
int nuvio_player_state(const char *body, size_t len);
char *nuvio_player_result(void);
char *nuvio_player_status(void);
int nuvio_player_control_post(const char *body, size_t len);
char *nuvio_player_control_take(void);
void nuvio_debug_shot_store(const void *data, size_t len);
void *nuvio_debug_shot_copy(size_t *len);

/* debug.c: test-loop helpers, reachable from the LAN only with the flag. */
char *nuvio_debug_launch_app(int *status);
/* Process list (pid <= 0) or one process's threads, as text lines. */
char *nuvio_debug_threads(int pid, size_t *len);

/* tile.c: removes the web-link tile (NUVI00001) used up to 1.0.2. */
int nuvio_tile_uninstall(void);

/* plugin_fetch.c */
int nuvio_plugin_init(void);
/* Handles /plugin/<route>. Returns a malloc'd JSON body and sets *status. */
char *nuvio_plugin_handle(const char *route, const char *method, const char *body,
                          size_t body_len, int *status);

/* launch.c: full-screen shell (BigApp via hbldr). Disabled: on FW 13.60
 * sceSystemServiceLaunchApp refuses an unregistered host (0x18). */
#define NUVIO_FULLSCREEN_SHELL 0
int nuvio_launch_shell_async(void);
const char *nuvio_launch_state(void);
int nuvio_launch_close_shell(void);

/* stream.c: repackages direct video URLs as fMP4 HLS for the browser. */
void nuvio_stream_init_module(void);
char *nuvio_stream_open(const char *url, const char *const *header_names,
                        const char *const *header_values, int header_count, int *status);
char *nuvio_stream_playlist(const char *id, size_t *len);
uint8_t *nuvio_stream_init(const char *id, size_t *len);
uint8_t *nuvio_stream_segment(const char *id, int index, size_t *len);
void nuvio_stream_close(const char *id);
/* Present = LAN clients may open streams too (for testing from a PC). */
#define NUVIO_LAN_DEBUG_FLAG NUVIO_DATA_DIR "/lan-debug"

char *nuvio_speed_test(const char *url, long buffer_bytes);

/* http.c */
int nuvio_http_start(const char *www_dir);
void nuvio_http_stop(void);
extern volatile int nuvio_keep_running;
