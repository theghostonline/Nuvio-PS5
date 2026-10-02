/*
 * Installs the native Nuvio app (PPSA99176) into /data/homebrew. The app is
 * built from ../nuvio-ps5-app (ps5-native-app-boilerplate) and embedded here
 * as a tar.gz, so one payload sets everything up. ShadowMountPlus notices the
 * folder, mounts it at /system_ex/app/PPSA99176 and registers it in Media.
 */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "assets.h"
#include "nuvio.h"

NUVIO_INCASSET(app_tgz, "build/app.tar.gz");
NUVIO_INCASSET(app_id, "build/app.id");

#define APP_BUILD_MARKER NUVIO_APP_DIR "/.nuvio-build"

int sceSystemServiceGetAppIdOfRunningBigApp(void);

/* The loader refuses modules without the execute bit (EACCES on launch). */
static void make_executable(const char *dir) {
  DIR *handle = opendir(dir);
  struct dirent *entry;

  if (!handle)
    return;
  while ((entry = readdir(handle))) {
    char path[512];
    size_t len = strlen(entry->d_name);
    if (len > 4 && !strcmp(entry->d_name + len - 4, ".prx")) {
      snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
      chmod(path, 0777);
    }
  }
  closedir(handle);
}

int nuvio_app_install_if_needed(void) {
  char id[64] = {0};
  char staging[256];

  snprintf(id, sizeof(id), "%.*s", (int)(app_id_size < 63 ? app_id_size : 63),
           (const char *)app_id);
  id[strcspn(id, "\r\n ")] = '\0';

  if (nuvio_file_equals(APP_BUILD_MARKER, id, strlen(id))) {
    nuvio_log("app: %s build %s already installed", NUVIO_APP_TITLE_ID, id);
    return 0;
  }

  /* The app is EVO-based and runs as the foreground (big) app. Replacing its
   * files under a running instance can panic the console, so while any big
   * app is up, leave the update for the caller to retry. */
  if (sceSystemServiceGetAppIdOfRunningBigApp() > 0) {
    nuvio_log("app: update to build %s waits until the running app closes", id);
    return 1;
  }

  /* Unpack beside the target, then swap it in, so ShadowMountPlus never
   * scans a half-written app. */
  snprintf(staging, sizeof(staging), "%s.staging", NUVIO_APP_DIR);
  nuvio_rm_rf(staging);
  if (nuvio_mkdir_p(staging, 0777) != 0 || nuvio_extract_tgz(app_tgz, app_tgz_size, staging) != 0) {
    nuvio_log("app: unpack to %s failed", staging);
    nuvio_rm_rf(staging);
    return -1;
  }
  {
    char path[320];
    snprintf(path, sizeof(path), "%s/eboot.bin", staging);
    chmod(path, 0777);
    snprintf(path, sizeof(path), "%s/sce_module", staging);
    make_executable(path);
    snprintf(path, sizeof(path), "%s/.nuvio-build", staging);
    nuvio_write_file(path, id, strlen(id), 0644);
  }

  nuvio_rm_rf(NUVIO_APP_DIR);
  if (rename(staging, NUVIO_APP_DIR) != 0) {
    nuvio_log("app: moving %s into place failed", staging);
    return -1;
  }
  nuvio_log("app: installed %s build %s to %s", NUVIO_APP_TITLE_ID, id, NUVIO_APP_DIR);
  return 0;
}
