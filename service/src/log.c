/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nuvio.h"

/* Logs are capped so a chatty web app can never fill /data. */
#define LOG_MAX_BYTES (2 * 1024 * 1024)

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void append_capped(const char *path, const char *line, size_t len) {
  struct stat st;
  int fd;

  if (stat(path, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
    char old[256];
    snprintf(old, sizeof(old), "%s.1", path);
    rename(path, old);
  }
  fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0)
    return;
  (void)write(fd, line, len);
  close(fd);
}

void nuvio_log(const char *fmt, ...) {
  char body[1536];
  char line[1664];
  struct tm tm;
  time_t now = time(NULL);
  va_list ap;
  int n;

  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  localtime_r(&now, &tm);
  n = snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d [nuvio] %s\n",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
               tm.tm_min, tm.tm_sec, body);
  if (n <= 0)
    return;
  if ((size_t)n >= sizeof(line))
    n = (int)sizeof(line) - 1;

  fputs(line, stdout);
  fflush(stdout);

  pthread_mutex_lock(&g_log_lock);
  nuvio_mkdir_p(NUVIO_DATA_DIR, 0755);
  append_capped(NUVIO_LOG_PATH, line, (size_t)n);
  pthread_mutex_unlock(&g_log_lock);
}

void nuvio_app_log_append(const char *line, size_t len) {
  char stamp[32];
  struct tm tm;
  time_t now = time(NULL);
  int n;

  localtime_r(&now, &tm);
  n = snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d ",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
               tm.tm_min, tm.tm_sec);

  pthread_mutex_lock(&g_log_lock);
  nuvio_mkdir_p(NUVIO_DATA_DIR, 0755);
  {
    struct stat st;
    int fd;
    if (stat(NUVIO_APP_LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES)
      rename(NUVIO_APP_LOG_PATH, NUVIO_APP_LOG_PATH ".1");
    fd = open(NUVIO_APP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
      (void)write(fd, stamp, (size_t)n);
      (void)write(fd, line, len);
      if (len == 0 || line[len - 1] != '\n')
        (void)write(fd, "\n", 1);
      close(fd);
    }
  }
  pthread_mutex_unlock(&g_log_lock);
}
