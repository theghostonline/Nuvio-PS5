#include "pp_stage_breadcrumb.h"

#include <stdio.h>
#include <string.h>

#include "evo_boot_log.h"   /* the one diagnostic log — /mnt/usb0/evo.log */

/*
 * Playback-stage breadcrumbs for 4K crash isolation. Every checkpoint goes to
 * /mnt/usb0/evo.log (via evo_boot_log — timestamped, append-only, shared with
 * the boot trace and the decoder notes) and, in the app module, to the kernel
 * log. The system notification is opt-in — it shares evo_bt()'s
 * EVO_BOOT_TRACE_POPUP gate (scripts/package-app.sh --breadcrumbs) because it
 * fires routinely during playback (P8_AVLOG / SEEK_AVFRAME / P8_VDEC_FATAL),
 * not just at boot.
 */
#if defined(EVO_APP_MODULE)
extern int sceKernelDebugOutText(int, const char *);
#if defined(EVO_BOOT_TRACE_POPUP)
struct pp_bc_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
#endif

static void pp_bc_notify(const char *msg)
{
#if defined(EVO_BOOT_TRACE_POPUP)
    struct pp_bc_note n;
    memset(&n, 0, sizeof n);
    snprintf(n.msg, sizeof n.msg, "%s", msg);
    sceKernelSendNotificationRequest(0, &n, sizeof n, 0);
#endif
    char line[512];
    snprintf(line, sizeof line, "%s\n", msg);
    sceKernelDebugOutText(0, line);
}
#else
#define pp_bc_notify(m) ((void)0)
#endif

void pp_stage_bc(const char *stage_id, const char *detail)
{
    if (!stage_id)
        return;
    char msg[512];
    snprintf(msg, sizeof msg, "bc %s%s%s", stage_id,
             (detail && detail[0]) ? " " : "", detail ? detail : "");
    evo_boot_log("%s", msg);
    pp_bc_notify(msg);
}

void pp_stage_bc_checkpoint(const char *stage_id, const char *detail)
{
    pp_stage_bc(stage_id, detail);
}
