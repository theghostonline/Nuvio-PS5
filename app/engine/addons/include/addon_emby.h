/*
 * addon_emby.h — Emby & Jellyfin media server client for EVO Player.
 */
#ifndef ADDON_EMBY_H
#define ADDON_EMBY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "evo_addon.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EMBY_MAX_ITEMS 64

typedef struct emby_config {
    char host[128];
    int  port;
    char username[64];
    char password[64];
    char token[128];
    char user_id[64];
    char server_name[64];
    char server_version[32];
    /* https rather than http for every request, including the stream URL that
     * FFmpeg opens. A reverse-proxied Emby is the common deployment and was
     * simply unreachable before this existed (#90). */
    bool use_https;
    bool is_connected;
} emby_config_t;

typedef void (*emby_auth_cb)(int success, const char *msg, void *userdata);
typedef void (*emby_items_cb)(int success, const evo_media_entry_t *items, int count, void *userdata);

/* Initialize Emby client and load persistent credentials */
int  emby_init(void);

/* Save Emby configuration to disk */
int  emby_save_config(void);

/* Retrieve active configuration pointer */
emby_config_t *emby_get_config(void);

/* Update server connection settings */
void emby_set_server(const char *host, int port, const char *username, const char *password);

/* Authenticate with Emby server asynchronously */
int  emby_connect_async(emby_auth_cb callback, void *userdata);

/* Disconnect from current server */
void emby_disconnect(void);

/* Fetch root libraries (Movies, TV Shows, Collections) asynchronously */
int  emby_fetch_libraries_async(emby_items_cb callback, void *userdata);

/* Fetch items in a library or folder asynchronously */
int  emby_fetch_items_async(const char *parent_id, emby_items_cb callback, void *userdata);

/* Construct direct stream playback URL for an item */
int  emby_build_stream_url(const char *item_id, char *out_url, size_t max_len);

/* Playback session reporting */
void emby_report_playback_start(const char *item_id);
void emby_report_playback_progress(const char *item_id, int64_t pos_sec, int64_t dur_sec);
void emby_report_playback_stop(const char *item_id, int64_t pos_sec);

#ifdef __cplusplus
}
#endif

#endif /* ADDON_EMBY_H */
