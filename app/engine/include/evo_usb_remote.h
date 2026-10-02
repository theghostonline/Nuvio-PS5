/*
 * evo_usb_remote.h — scriptable dev remote for the app module.
 *
 * When built with -DEVO_USB_REMOTE (scripts/package-app.sh --usb-remote) the
 * frame loop polls /mnt/usb0/evo_cmd for one-line commands and writes
 * /mnt/usb0/evo_status once a second. Lets tools/evo-remote.sh drive playback
 * (open a file, seek) and read state over FTP with no controller and no
 * rebuild between tests. Compiled out entirely otherwise.
 *
 * Commands (evo_cmd, consumed then deleted):
 *   play <path>        open <path> from the start
 *   seek <sec>         seek to absolute <sec>
 *   seek +<sec>        seek forward
 *   seek -<sec>        seek back
 *   stop               end playback, back to the browser (#8: flushes the
 *                      per-file `sweep` line, which the decoder close writes)
 *   upcompare          #103: pause, then capture the SAME frame with the
 *                      upscaler Off / Sharp / AI Standard / Large / Maximum
 *                      and no OSD, to
 *                      /mnt/usb0/evo_up_{off,sharp,ai,ai_large,ai_max}.bmp;
 *                      restores the
 *                      Settings mode and the pause state afterwards
 *
 * Status line: build=<id> t=<s> scr=<n> be=<0|1> pos=<s> dur=<s> fps=<n>
 *              fatal=<0|1> eof=<0|1> active=<0|1>
 */
#ifndef EVO_USB_REMOTE_H
#define EVO_USB_REMOTE_H

#ifdef __cplusplus
extern "C" {
#endif

#if defined(EVO_USB_REMOTE) && defined(EVO_APP_MODULE)
/* Call once per frame from main()'s loop. Cheap: a stat() on the cmd file and,
 * at most once a second, a status write. */
void evo_usb_remote_poll(void);

/*
 * One frame's worth of synthetic button presses, OR'd into the real pad mask
 * by the frame loop. Returns the pending mask and clears it, so a `key`
 * command produces exactly one press edge and one release - the same shape
 * the UI sees from a real controller tap.
 *
 * This exists so the whole interface can be driven without a human holding a
 * pad: screen tours for documentation and video capture, and reproducing a
 * navigation bug in the same order every time. `key l3` reaches the existing
 * screenshot handler, so captures come for free.
 *
 * Returns 0 in builds without the dev remote, so the frame loop's OR is a
 * no-op there.
 */
unsigned int evo_usb_remote_take_buttons(void);

/*
 * Go straight to a screen, or straight to a browser source, instead of
 * simulating the button presses that would get there.
 *
 * Synthetic input alone turned out not to be enough to drive the storage
 * browser: whether `left` reaches the Sources panel or carries on to the
 * navigation rail depends on state the remote cannot see, so "switch to
 * Internal Storage" was a guess that silently landed on the wrong source.
 * These call the same entry points the UI itself uses.
 *
 * screen: an evo::ScreenId value (2 Player, 21 TextReader, 28 SurroundTest,
 * 30 ImageViewer, ...).
 * source: the browser's sidebar index - 0 USB, 1 Internal, 2 Favorites,
 * 3 Recent. The category filters are no longer sources: they are per-folder
 * chips in the header, chosen with Up from the grid.
 */
void evo_remote_goto_screen(int screen_id);
void evo_remote_browser_source(int sidebar_index);

/*
 * Open a file in the image viewer / text reader.
 *
 * Navigating to those screens is not enough on its own - they render
 * "IMAGE COULD NOT BE DISPLAYED" / "NO DOCUMENT LOADED" until something
 * hands them a path, which is what BrowserScreen does before it navigates.
 * These mirror that: load, then show.
 */
void evo_remote_open_image(const char *path);
void evo_remote_open_text(const char *path);
#else
#define evo_usb_remote_poll() ((void)0)
#define evo_usb_remote_take_buttons() (0u)
#endif

/* Provided by the host (main.c): open <path> from the beginning, mirroring the
 * browse->select path (nav push, return screen, start_video_playback). */
void evo_open_media_path(const char *path);

/* Provided by the host (Application.cpp): start the #103 upscaler A/B/C
 * capture described above. No-op unless a video is playing. */
void evo_remote_upscale_compare(void);

/* Provided by the host (main.c): end playback and return to the browser. */
void evo_stop_media_playback(void);

/* Provided by the host (main.c): ABSOLUTE media position, seek base included.
 * evo_pb_position_s() is the raw clock and restarts at 0 on every seek — use
 * this for anything user-meaningful (relative seeks, the status line). */
double evo_player_position_s(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_USB_REMOTE_H */
