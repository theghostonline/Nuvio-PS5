/*
 * evo_boot_log.h — EVO's single diagnostic log: /mnt/usb0/evo.log
 *
 * Every diagnostic stream funnels here — the boot trace (evo_bt / GL context /
 * jailbreak result), the playback breadcrumbs (pp_stage_bc), the native
 * decoder notes, the per-file playback stats — one timestamped, append-only
 * file so there is a single place to look.
 *
 * evo_boot_log() timestamps the line and, before /mnt/usb0 is reachable,
 * buffers it in memory; evo_boot_log_flush() opens the file once the sandbox
 * is unjailed, drains the buffer, and thereafter every line is written
 * straight through - which is what makes the last line before a crash
 * survive, so keep anything emitted per-frame rare rather than deferring it.
 * Call flush right after evo_jailbreak_self(). With
 * EVO_BOOT_TRACE_POPUP (--breadcrumbs) each line also pops a notification.
 * No-op on host / payload builds.
 *
 * NOT funnelled here: evo_status (a live one-line state snapshot the dev
 * remote polls) and evo_compat_report.txt (a user-triggered report).
 */
#ifndef EVO_BOOT_LOG_H
#define EVO_BOOT_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

void evo_boot_log(const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 1, 2)))
#endif
    ;
/* Put everything logged so far on the USB stick and fsync it - a breadcrumb.
 * Blocks until the stick has it; the first call also opens the file. */
void evo_boot_log_flush(void);
/* The periodic flush from the render loop: wakes the writer thread and
 * returns at once. Never stalls a frame on a busy USB stick. */
void evo_boot_log_kick(void);
/* Crash-handler use only: write still-queued lines to `fd` (no locks). */
void evo_boot_log_crash_drain(int fd);

/* Preferred names for new code — the file carries far more than the boot. */
#define evo_log        evo_boot_log
#define evo_log_flush  evo_boot_log_flush

#ifdef __cplusplus
}
#endif

#endif /* EVO_BOOT_LOG_H */
