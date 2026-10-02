/*
 * evo_provider_log.h — one diagnostic line, two destinations.
 *
 * The provider seam originally logged with fprintf(stderr, ...), which was
 * wrong twice over on the app module:
 *
 *   - stderr does not reach /mnt/usb0/evo.log. That file is written by
 *     evo_boot_log / evo_bt, so every provider diagnostic went nowhere and
 *     #90's "confirm the load lines in evo.log" was unsatisfiable.
 *   - nothing in the app module has ever written to stderr, and on the
 *     native-app CRT touching it dereferences null. The FFmpeg av_log fix
 *     landed earlier the same day for exactly this reason; the provider code
 *     walked straight back into it.
 *
 * So: evo_bt on the device (durable log + klog, and a popup only with
 * --breadcrumbs), plain stderr on the host, where the uiview harness and the
 * test runner both want to see it and stderr is a real stream.
 */
#ifndef EVO_PROVIDER_LOG_H
#define EVO_PROVIDER_LOG_H

#ifdef EVO_APP_MODULE

#include "evo_boot_trace.h"
/* evo_bt already prefixes "EVO boot: "; the tag keeps provider lines greppable
 * in a log that carries the whole boot and playback trace. */
#define PROV_LOG(fmt, ...) evo_bt("provider: " fmt, ##__VA_ARGS__)

#else /* host / payload */

#include <stdio.h>
#define PROV_LOG(fmt, ...) fprintf(stderr, "[EVO provider] " fmt "\n", ##__VA_ARGS__)

#endif

#endif /* EVO_PROVIDER_LOG_H */
