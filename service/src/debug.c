/*
 * Test-loop helpers. The routes that reach these are loopback-only unless
 * /data/nuvio/lan-debug exists (see http.c), so a release console never
 * exposes them to the network.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nuvio.h"

typedef struct {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} launch_ctx_t;

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceLaunchApp(const char *title_id, char **argv, launch_ctx_t *ctx);
int sceUserServiceGetForegroundUser(uint32_t *user_id);

/*
 * Starts the Nuvio app, but only from the home screen: a launch over a
 * running big app (Nuvio itself included) is never attempted, because
 * stacking launches has panicked consoles running EVO's engine.
 */
char *nuvio_debug_launch_app(int *status) {
  launch_ctx_t ctx;
  int running = sceSystemServiceGetAppIdOfRunningBigApp();
  int rc;
  char *reply = NULL;

  if (running > 0) {
    *status = 409;
    if (asprintf(&reply, "{\"returnValue\":false,\"errorText\":\"an app is running\","
                         "\"appId\":%d}", running) < 0)
      reply = NULL;
    return reply;
  }
  memset(&ctx, 0, sizeof(ctx));
  if (sceUserServiceGetForegroundUser(&ctx.user_id) != 0) {
    *status = 500;
    return strdup("{\"returnValue\":false,\"errorText\":\"no foreground user\"}");
  }
  /* A positive result is the new app's id (0x18 on the first run). */
  rc = sceSystemServiceLaunchApp(NUVIO_APP_TITLE_ID, NULL, &ctx);
  nuvio_log("debug: launch %s -> 0x%08x", NUVIO_APP_TITLE_ID, (unsigned)rc);
  *status = rc >= 0 ? 200 : 500;
  if (asprintf(&reply, "{\"returnValue\":%s,\"result\":\"0x%08x\"}", rc >= 0 ? "true" : "false",
               (unsigned)rc) < 0)
    reply = NULL;
  return reply;
}
