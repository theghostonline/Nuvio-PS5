#ifndef EVO_READDIR_H
#define EVO_READDIR_H

/*
 * evo_readdir - directory enumeration that works in the app-module sandbox.
 *
 * In the PPSA99039 app sandbox opendir() fails EPERM on every path (its
 * internal fstatfs step is not wired through the clean-room libc.prx shim),
 * but open(path, O_RDONLY|O_DIRECTORY) + getdents(fd, buf, n) return real
 * entries - proven on hardware 2026-09-02 (docs/evo-pro/phase-1b-app-module.md
 * section 5, Step A #2).
 *
 * This is a drop-in replacement for the opendir/readdir/closedir triple:
 * same call shape, same `struct dirent *` (d_name / d_type) result, so call
 * sites change only the three function names and the handle type.
 *
 * Only the EVO_APP_MODULE build takes the getdents path; the elfldr-payload
 * build forwards straight to libc opendir/readdir so `main` behaviour is
 * byte-for-byte unchanged.
 */

#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct evo_dir evo_dir_t;

/* NULL on failure (errno set by open()/opendir()). */
evo_dir_t *evo_opendir(const char *path);

/* Next entry, or NULL at end of directory / on error. The returned pointer is
 * owned by `d` and valid until the next evo_readdir()/evo_closedir() on `d`.
 * Entries are returned in raw filesystem order, "." and ".." included, exactly
 * like readdir(). */
struct dirent *evo_readdir(evo_dir_t *d);

void evo_closedir(evo_dir_t *d);

#ifdef __cplusplus
}
#endif

#endif /* EVO_READDIR_H */
