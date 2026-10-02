#ifndef EVO_DATA_PATH_H
#define EVO_DATA_PATH_H

/*
 * evo_data_path - where EVO's persistent data lives.
 *
 * As an elfldr payload EVO runs unsandboxed and writes to /data/evoplayer/.
 * As the PPSA99039 app module the process launches into a fresh per-title
 * sandbox: /data is ENOENT until evo_jailbreak_self() lifts it. Once lifted,
 * /data/evoplayer/ is the durable home - the same as the payload.
 * /download0/evoplayer/ is only a fallback for the window before the self-unjail
 * lands (or if it never does); it is a savedata-relative mount with no
 * sceSaveDataMount2/commit behind it, so writes there do NOT survive a
 * relaunch (issue #46).
 *
 * The root therefore has to be picked at RUNTIME, not compile time:
 *   - payload / host build:            always /data/evoplayer
 *   - app module, sandbox open:        /data/evoplayer   (cached once resolved)
 *   - app module, sandbox still shut:  /download0/evoplayer (transient, not cached)
 *
 * evo_data_dir() / evo_data_path() do that resolution. Nothing else should
 * reference EVO_DATA_DIR - it survives only as the fallback literal inside
 * evo_data_path.c.
 */

#ifdef EVO_APP_MODULE
#define EVO_DATA_DIR "/download0/evoplayer"   /* fallback only - see above */
#else
#define EVO_DATA_DIR "/data/evoplayer"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The data root, no trailing slash. Resolved once, lazily; on the app module
 * it stays on the /download0 fallback (uncached) until the sandbox opens, then
 * pins to /data/evoplayer. */
const char *evo_data_dir(void);

/*
 * Join `leaf` onto the data root: evo_data_path("emby.conf") ->
 * "/data/evoplayer/emby.conf". A leading '/' on `leaf` is ignored.
 *
 * Returns a pointer to a per-thread static buffer - use it immediately (as an
 * fopen()/open() argument, say); do not stash it or pass two results of this
 * call into the same expression. On overflow it returns evo_data_dir().
 */
const char *evo_data_path(const char *leaf);

/*
 * Drop the cached root so the next evo_data_dir()/evo_data_path() re-resolves.
 * Call after a late evo_jailbreak_ensure() flips the sandbox open - config
 * loaded from the /download0 fallback then needs to move to /data (issue #46).
 */
void evo_data_path_rebind(void);

/*
 * mkdir() that works in the app-module sandbox. The clean-room libc.prx surface
 * exposes sceKernelMkdir, not the POSIX name (same gap that made evo_readdir.c
 * wrap getdents(2) directly), so the app build goes straight to the libkernel
 * export. Returns 0 on success or if the directory already exists.
 */
int evo_mkdir(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* EVO_DATA_PATH_H */
