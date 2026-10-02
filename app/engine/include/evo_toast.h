#ifndef EVO_TOAST_MODULE_H
#define EVO_TOAST_MODULE_H

/*
 * Module: evo_toast
 *
 * In-app toast notification system. Categorises messages as info,
 * technical, or error and suppresses technical noise unless the debug
 * profile or overlay is active.
 *
 * Public surface:
 *   toast(title, msg)          — fire-and-forget notification
 *   draw_prospero_toast(fb)    — called every frame by the main render loop
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fire a toast notification. title and msg may be NULL (safe defaults apply).
 * Technical messages are suppressed unless debug mode is active. */
void toast(const char *title, const char *msg);

/* Draw the current toast into the framebuffer. Call every frame. */
void draw_prospero_toast(uint32_t *fb);

/* 1 while a toast is on screen (including its fade-out). The GL video path uses
 * this to know the OSD scratch has content worth compositing over the frame. */
int evo_toast_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_TOAST_MODULE_H */
