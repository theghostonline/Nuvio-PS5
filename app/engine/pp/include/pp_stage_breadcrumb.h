/*
 * Playback-stage breadcrumbs for 4K crash isolation — written straight to
 * /mnt/usb0/evo.log (the one diagnostic log), not only on clean exit.
 */
#ifndef PP_STAGE_BREADCRUMB_H
#define PP_STAGE_BREADCRUMB_H

#ifdef __cplusplus
extern "C" {
#endif

/** Append one stage line to the log. Safe to call frequently. */
void pp_stage_bc(const char *stage_id, const char *detail);

/** Alias of pp_stage_bc (the separate "last alive" file is gone). */
void pp_stage_bc_checkpoint(const char *stage_id, const char *detail);

#ifdef __cplusplus
}
#endif

#endif
