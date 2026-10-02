#ifndef EVO_FAVORITES_H
#define EVO_FAVORITES_H

/*
 * Module: evo_favorites
 *
 * Favorites database. Persists to /data/evoplayer/evo_favorites.txt.
 * Stores up to 100 entries with path, title and duration.
 *
 * Public surface:
 *   favorites_clear()
 *   favorites_find(path)               -- index or -1
 *   favorites_is_favorite(path)        -- bool
 *   favorites_add(path, title, dur)
 *   favorites_remove(path)
 *   favorites_save()
 *   favorites_load()
 *   favorites_toggle_current_media()   -- add/remove currently playing file
 */

#include "evo_data_path.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAVORITES_FILE_DB evo_data_path("evo_favorites.txt")
#define MAX_FAVORITES     100

typedef struct {
    char   path[512];
    char   title[128];
    double duration;
} FavoriteEntry;

extern FavoriteEntry favorite_files[MAX_FAVORITES];
extern int           favorite_count;
extern int           favorite_selected;

void favorites_clear(void);
int  favorites_find(const char *path);
int  favorites_is_favorite(const char *path);
void favorites_add(const char *path, const char *title, double duration);
void favorites_remove(const char *path);
void favorites_save(void);
void favorites_load(void);
void favorites_toggle_current_media(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_FAVORITES_H */
