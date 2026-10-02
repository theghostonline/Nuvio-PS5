/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * EVO Player features the vendored engine still calls but the Nuvio Player
 * has no use for. Each is the "not there" answer the engine already handles.
 */
#include "evo_provider.h"

#include <stddef.h>

/* Providers (Emby, IPTV, ...) are EVO's; Nuvio streams carry no provider
 * object, so progress reporting and link resolution are skipped. */
const evo_provider_t *evo_provider_find(const char *id)
{
    (void)id;
    return NULL;
}

/* Folder thumbnails are part of EVO's file browser, which Nuvio has none of. */
void prospero_thumbnail_close_context(void)
{
}

/* EVO's "recently played" list. Nuvio keeps watch progress itself (the
 * player reports the final position to the page), so nothing is stored. */
#include "evo_recent.h"

void recent_add_or_update(const char *path, const char *title, double last_pos, double duration)
{
    (void)path;
    (void)title;
    (void)last_pos;
    (void)duration;
}

void recent_save(void)
{
}

/* Whether the process can see the real /data, which decides where the engine
 * keeps its few files (evo_data_path.c). The same probe EVO uses. */
#include <fcntl.h>
#include <unistd.h>

int evo_jailbreak_is_open(void)
{
    int fd = open("/data", O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}
