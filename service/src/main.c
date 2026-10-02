/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * nuvio-ps5: resident companion payload for Nuvio TV on a jailbroken PS5.
 *
 *  - serves the PS5 build of Nuvio TV (NuvioMedia/NuvioTVSmart) on
 *    http://127.0.0.1:17600/
 *  - installs the native Nuvio app (PPSA99176, Media) that shows it full screen
 *  - provides the plugin network service Nuvio's scrapers need
 *
 * Load it after the jailbreak (autoload.txt), then open Nuvio from Media.
 */

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "hbldr/hbldr.h"
#include "nuvio.h"

int sceUserServiceInitialize(void *);

/* Finds another process with our thread name (a previous instance). */
static pid_t find_pid(const char *name) {
  int mib[4] = {1, 14, 8, 0}; /* CTL_KERN, KERN_PROC, KERN_PROC_PROC */
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) != 0)
    return -1;
  if (!(buf = malloc(buf_size)))
    return -1;
  if (sysctl(mib, 4, buf, &buf_size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  /* struct kinfo_proc offsets on the PS5 kernel: ki_pid at 72, ki_tdname at
   * 447 (same layout as the payload SDK and the WebKit Autoloader use). */
  for (uint8_t *ptr = buf; ptr < buf + buf_size;) {
    int size = *(int *)ptr;
    pid_t ki_pid = *(pid_t *)&ptr[72];
    const char *ki_tdname = (const char *)&ptr[447];
    if (size <= 0)
      break;
    ptr += size;
    if (!strcmp(name, ki_tdname) && ki_pid != mypid)
      pid = ki_pid;
  }
  free(buf);
  return pid;
}

static void replace_previous_instance(void) {
  pid_t pid;
  int attempts = 0;

  while ((pid = find_pid(NUVIO_THREAD_NAME)) > 0 && attempts++ < 10) {
    nuvio_log("stopping previous instance pid=%d", (int)pid);
    kill(pid, SIGKILL);
    sleep(1);
  }
}

int main(void) {
  const char *www_dir;
  int app_rc;
  int user_prio = 256;

  syscall(SYS_thr_set_name, -1, NUVIO_THREAD_NAME);
  signal(SIGPIPE, SIG_IGN);

  replace_previous_instance();
  nuvio_mkdir_p(NUVIO_DATA_DIR, 0755);
  nuvio_log("nuvio-ps5 %s starting (pid %d)", NUVIO_PS5_VERSION, (int)getpid());

  sceUserServiceInitialize(&user_prio);
  nuvio_plugin_init();
  nuvio_stream_init_module();

  if (!(www_dir = nuvio_www_prepare())) {
    nuvio_notify("Nuvio: could not unpack the app to %s", NUVIO_WWW_DIR);
    return 1;
  }

  if (nuvio_http_start(www_dir) != 0) {
    nuvio_notify("Nuvio: port %d is busy, service not started", NUVIO_PORT);
    return 2;
  }

  /* The native app is what the home screen shows; ShadowMountPlus registers it.
   * 1 = deferred because an app is running; retried in the loop below. */
  app_rc = nuvio_app_install_if_needed();
  if (app_rc < 0)
    nuvio_notify("Nuvio: could not install the app to %s", NUVIO_APP_DIR);
  else if (app_rc > 0)
    nuvio_notify("Nuvio %s: close the running game or app\nto finish updating Nuvio",
                 NUVIO_PS5_VERSION);
  nuvio_tile_uninstall();

  if (!NUVIO_FULLSCREEN_SHELL) {
    /* Remove hosts created by the 1.0.0/1.0.1 full-screen attempts. */
    hbldr_remove_system_app(NUVIO_TITLE_ID);
    hbldr_remove_system_app(NUVIO_HOST_TITLE_ID);
  }

  nuvio_notify("Nuvio %s ready\nOpen Nuvio from the home screen", NUVIO_PS5_VERSION);

  for (int tick = 0; nuvio_keep_running; tick++) {
    sleep(1);
    if (app_rc > 0 && tick % 10 == 9 && (app_rc = nuvio_app_install_if_needed()) == 0)
      nuvio_notify("Nuvio %s installed\nOpen Nuvio from the home screen", NUVIO_PS5_VERSION);
  }

  nuvio_log("shutting down");
  nuvio_http_stop();
  return 0;
}
