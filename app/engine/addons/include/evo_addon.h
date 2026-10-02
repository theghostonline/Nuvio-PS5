/*
 * evo_addon.h — Addon subsystem data structures and interface definitions.
 */
#ifndef EVO_ADDON_H
#define EVO_ADDON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVO_ADDON_EMBY = 0,
    EVO_ADDON_JELLYFIN,
    EVO_ADDON_NUVIO,
    EVO_ADDON_CUSTOM
} evo_addon_type_t;

typedef enum {
    EVO_MEDIA_FOLDER = 0,
    EVO_MEDIA_VIDEO,
    EVO_MEDIA_AUDIO,
    EVO_MEDIA_STREAM
} evo_media_kind_t;

typedef struct evo_media_entry {
    char id[128];
    char title[160];
    char detail[160];
    char stream_url[512];
    char overview[512];
    int64_t duration_sec;
    int64_t resume_pos_sec;
    evo_media_kind_t kind;
    int is_folder;
} evo_media_entry_t;

#ifdef __cplusplus
}
#endif

#endif /* EVO_ADDON_H */
