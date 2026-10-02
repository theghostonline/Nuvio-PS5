/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Full-screen launch: the tile deeplink hits /launch, and this starts
 * nuvio-shell (embedded) as the foreground BigApp through hbldr. The shell
 * opens the web app in the system browser dialog covering the whole screen.
 * If anything fails, the /launch page falls back to the windowed browser.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "assets.h"
#include "hbldr/hbldr.h"
#include "nuvio.h"

NUVIO_INCASSET(shell_elf, "build/nuvio-shell.elf");
NUVIO_INCASSET(host_icon0_png, "build/icon0.png");

#define SHELL_PATH NUVIO_DATA_DIR "/nuvio-shell.elf"
#define SHELL_STDIO_LOG NUVIO_DATA_DIR "/shell-stdio.log"
#define HOST_ICON "/system_ex/app/" NUVIO_HOST_TITLE_ID "/sce_sys/icon0.png"

/* hbldr polls this while a previous BigApp exits; the SDK has no stub for it,
 * so (as in PS Play) report "not running" and let hbldr continue. */
int sceKernelGetAppState(int app_id, int *a, int *b) __attribute__((weak));
int sceKernelGetAppState(int app_id, int *a, int *b) {
  (void)app_id;
  (void)a;
  (void)b;
  return -1;
}

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *g_state = "idle";
static pid_t g_shell_pid = -1;

static void set_state(const char *state) {
  pthread_mutex_lock(&g_lock);
  g_state = state;
  pthread_mutex_unlock(&g_lock);
}

const char *nuvio_launch_state(void) {
  const char *state;
  pthread_mutex_lock(&g_lock);
  state = g_state;
  if (!strcmp(state, "running") && (g_shell_pid <= 0 || kill(g_shell_pid, 0) != 0))
    state = g_state = "idle";
  pthread_mutex_unlock(&g_lock);
  return state;
}

static void *launch_thread(void *arg) {
  char *argv[] = {SHELL_PATH, NULL};
  char *envp[] = {NULL};
  int stdio;
  pid_t pid;
  (void)arg;

  /* hbldr reports problems with perror()/puts(); keep them in a file. */
  {
    static int redirected;
    int fd;
    if (!redirected && (fd = open(NUVIO_DATA_DIR "/hbldr.log", O_WRONLY | O_CREAT | O_APPEND,
                                  0644)) >= 0) {
      dup2(fd, 1);
      dup2(fd, 2);
      close(fd);
      redirected = 1;
    }
  }
  /* Builds up to 1.0.0 used the tile's own title ID for the host. */
  if (hbldr_remove_system_app(NUVIO_TITLE_ID) != 0)
    nuvio_log("launch: could not remove old host /system_ex/app/%s", NUVIO_TITLE_ID);

  if (hbldr_prepare_host() != 0) {
    nuvio_log("launch: system host prepare failed errno=%d", errno);
    set_state("failed");
    return NULL;
  }
  /* The host is what the system shows while Nuvio runs; give it the icon. */
  if (!nuvio_file_equals(HOST_ICON, host_icon0_png, host_icon0_png_size) &&
      nuvio_write_file(HOST_ICON, host_icon0_png, host_icon0_png_size, 0644) != 0)
    nuvio_log("launch: could not write host icon errno=%d", errno);

  /* Keep a copy on disk for inspection; hbldr runs the embedded image. */
  if (!nuvio_file_equals(SHELL_PATH, shell_elf, shell_elf_size))
    nuvio_write_file(SHELL_PATH, shell_elf, shell_elf_size, 0755);

  stdio = open(SHELL_STDIO_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
  nuvio_log("launch: starting nuvio-shell via hbldr");
  pid = hbldr_launch_buffer(NUVIO_DATA_DIR, SHELL_PATH, stdio, argv, envp, shell_elf);
  if (stdio >= 0)
    close(stdio);

  pthread_mutex_lock(&g_lock);
  g_shell_pid = pid;
  g_state = pid > 0 ? "running" : "failed";
  pthread_mutex_unlock(&g_lock);
  nuvio_log("launch: hbldr -> pid %d", (int)pid);
  return NULL;
}

int nuvio_launch_shell_async(void) {
  pthread_t thread;
  pthread_attr_t attr;
  int rc;

  pthread_mutex_lock(&g_lock);
  if (!strcmp(g_state, "launching")) {
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
  g_state = "launching";
  pthread_mutex_unlock(&g_lock);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&thread, &attr, launch_thread, NULL);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    set_state("failed");
    return -1;
  }
  return 0;
}

/* Asks a running shell to close its dialog (and so exit). */
int nuvio_launch_close_shell(void) {
  return nuvio_write_file(NUVIO_DATA_DIR "/shell.close", "1", 1, 0644);
}
