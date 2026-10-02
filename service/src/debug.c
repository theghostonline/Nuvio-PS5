/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Test-loop helpers. The routes that reach these are loopback-only unless
 * /data/nuvio/lan-debug exists (see http.c), so a release console never
 * exposes them to the network.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

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

/*
 * Process and thread table, for diagnosing a stuck app from the Mac: two
 * samples a few seconds apart show which thread is burning CPU (runtime) and
 * what the others are blocked on (wmesg / lock).
 */
static int s_proc_errno;

static uint8_t *proc_table(int op, int arg, size_t *size) {
  int mib[4] = {CTL_KERN, KERN_PROC, op, arg};
  uint8_t *buf;

  *size = 0;
  if (sysctl(mib, 4, NULL, size, NULL, 0) != 0 || *size == 0) {
    s_proc_errno = errno;
    return NULL;
  }
  *size += *size / 4; /* threads can appear between the two calls */
  if (!(buf = malloc(*size)))
    return NULL;
  if (sysctl(mib, 4, buf, size, NULL, 0) != 0) {
    s_proc_errno = errno;
    free(buf);
    return NULL;
  }
  return buf;
}

static const char *proc_state(char stat) {
  static const char *names[] = {"?", "idle", "run", "sleep", "stop", "zomb", "wait", "lock"};
  return (unsigned char)stat < sizeof names / sizeof *names ? names[(unsigned char)stat] : "?";
}

/*
 * pid <= 0: one line per process. Otherwise one line per thread of pid.
 * Entries are walked by their own ki_structsize: the console's is not the
 * SDK's sizeof, but the leading fields (up to ki_comm) share its layout.
 */
char *nuvio_debug_threads(int pid, size_t *len) {
  size_t size, cap = 4096, used = 0;
  /* Every thread on the console, filtered below: a per-pid query of another
   * process returns nothing here. */
  uint8_t *buf = NULL;
  int op = KERN_PROC_PROC;

  s_proc_errno = 0;
  if (pid > 0) {
    static const int ops[] = {KERN_PROC_PID | KERN_PROC_INC_THREAD, KERN_PROC_ALL | KERN_PROC_INC_THREAD,
                              KERN_PROC_PID};
    for (size_t i = 0; !buf && i < sizeof ops / sizeof *ops; i++)
      buf = proc_table(op = ops[i], (ops[i] & ~KERN_PROC_INC_THREAD) == KERN_PROC_PID ? pid : 0, &size);
  } else {
    buf = proc_table(op, 0, &size);
  }
  char *out = malloc(cap);

  if (!out) {
    free(buf);
    return NULL;
  }
  used = (size_t)snprintf(out, cap, "# op %d: %zu bytes, entry %d, errno %d\n", op, buf ? size : 0,
                          buf ? *(int *)buf : 0, s_proc_errno);
  for (uint8_t *ptr = buf; buf && ptr + offsetof(struct kinfo_proc, ki_comm) + COMMLEN < buf + size;) {
    const struct kinfo_proc *p = (const struct kinfo_proc *)ptr;
    char line[256];
    int m;

    if (p->ki_structsize <= 0)
      break;
    ptr += p->ki_structsize;
    if (pid > 0 && p->ki_pid != pid)
      continue;
    m = pid > 0 ? snprintf(line, sizeof line, "name=%.16s state=%s wait=%.8s lock=%.8s cpu_ms=%llu\n",
                           p->ki_tdname, proc_state(p->ki_stat),
                           p->ki_wmesg[0] ? p->ki_wmesg : "-",
                           p->ki_lockname[0] ? p->ki_lockname : "-",
                           (unsigned long long)p->ki_runtime / 1000)
                : snprintf(line, sizeof line, "pid=%d comm=%.19s cpu_ms=%llu\n", p->ki_pid,
                           p->ki_comm, (unsigned long long)p->ki_runtime / 1000);
    if (m <= 0)
      continue;
    if (used + (size_t)m + 1 > cap) {
      char *grown = realloc(out, cap * 2);
      if (!grown)
        break;
      out = grown;
      cap *= 2;
    }
    memcpy(out + used, line, (size_t)m + 1);
    used += (size_t)m;
  }
  free(buf);
  *len = used;
  return out;
}
