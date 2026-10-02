/*
 * Nuvio on the home screen.
 *
 * Current builds install the native Nuvio app (PPSA99176, built from
 * ../nuvio-ps5-app) into /data/homebrew, where ShadowMountPlus mounts and
 * registers it; the app shows the web app full screen. Builds up to 1.0.2
 * registered a web-link tile (NUVI00001) instead, which is removed here.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nuvio.h"

#define APP_DIR "/user/app/" NUVIO_TITLE_ID

int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppUnInstall(const char *, void *, void *);

int nuvio_tile_uninstall(void) {
  struct stat st;
  int rc;

  if (stat(APP_DIR, &st) != 0)
    return 0;
  if ((rc = sceAppInstUtilInitialize()) != 0)
    return rc;
  rc = sceAppInstUtilAppUnInstall(NUVIO_TITLE_ID, NULL, NULL);
  sceAppInstUtilTerminate();
  nuvio_log("tile: uninstall -> 0x%08x", (unsigned)rc);
  nuvio_rm_rf(APP_DIR);
  return rc;
}
