/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * How far the console clock is from real time.
 *
 * A jailbroken PS5 kept away from PSN often has no time sync, and its clock
 * drifts or is simply wrong. Nuvio's sign-in compares server-issued expiry
 * times with the local clock, so a console running a few minutes ahead sees
 * every sign-in QR code as already expired (issue #1, firmware 10.01). The
 * page asks /api/time for this offset and corrects its clock before anything
 * compares times.
 *
 * The time comes from the Date header of a plain-HTTP request: HTTPS cannot be
 * used to fix a wrong clock, because certificate checks depend on that clock.
 * One-second resolution is plenty for expiry checks.
 */
#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "nuvio.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_known;
static long long g_offset_ms;
static char g_source[64];

static long long now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Days since 1970-01-01 for a proleptic Gregorian date. */
static long long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long long)doe - 719468;
}

/* "Thu, 02 Oct 2026 03:12:45 GMT" -> epoch seconds, or -1. */
static long long parse_http_date(const char *s) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int d, y, h, mi, se;
  if (sscanf(s, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6)
    return -1;
  const char *p = strstr(months, mon);
  if (!p || y < 2020 || d < 1 || d > 31)
    return -1;
  const int m = (int)(p - months) / 3 + 1;
  return days_from_civil(y, m, d) * 86400 + h * 3600 + mi * 60 + se;
}

static size_t on_header(char *buf, size_t size, size_t n, void *user) {
  long long *server = user;
  const size_t len = size * n;
  if (len > 5 && !strncasecmp(buf, "Date:", 5)) {
    char line[96];
    size_t k = len - 5 < sizeof line - 1 ? len - 5 : sizeof line - 1;
    memcpy(line, buf + 5, k);
    line[k] = 0;
    const char *v = line;
    while (*v == ' ')
      v++;
    *server = parse_http_date(v);
  }
  return len;
}

/* 0 on success, offset in *out_ms. */
static int measure(const char *url, long long *out_ms) {
  CURL *c = curl_easy_init();
  long long server = -1;
  if (!c)
    return -1;
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &server);
  const long long t0 = now_ms();
  const CURLcode rc = curl_easy_perform(c);
  const long long t1 = now_ms();
  curl_easy_cleanup(c);
  if (rc != CURLE_OK || server < 0 || t1 - t0 > 8000)
    return -1;
  /* The header names a whole second; its middle is the best estimate. */
  *out_ms = server * 1000 + 500 - (t0 + (t1 - t0) / 2);
  return 0;
}

static void *clock_thread(void *arg) {
  static const char *urls[] = {"http://api.nuvio.tv/", "http://www.google.com/generate_204",
                               "http://www.cloudflare.com/"};
  (void)arg;
  for (int round = 0;; round++) {
    int ok = 0;
    for (size_t i = 0; i < sizeof urls / sizeof *urls && !ok; i++) {
      long long off = 0;
      if (measure(urls[i], &off) == 0) {
        pthread_mutex_lock(&g_lock);
        const int first = !g_known;
        g_known = 1;
        g_offset_ms = off;
        snprintf(g_source, sizeof g_source, "%s", urls[i]);
        pthread_mutex_unlock(&g_lock);
        if (first)
          nuvio_log("clock: console clock is %+.1f s from real time (%s)", off / 1000.0, urls[i]);
        ok = 1;
      }
    }
    /* Until the network is up: every 10 s. Then twice an hour for drift. */
    sleep(ok ? 1800 : 10);
  }
  return NULL;
}

void nuvio_clock_start(void) {
  pthread_t t;
  if (pthread_create(&t, NULL, clock_thread, NULL) == 0)
    pthread_detach(t);
}

/* LAN debug only: pretend the console clock is off by this many ms, so the
 * page's correction can be tested without changing the console's clock. */
static long long test_offset_ms(void) {
  struct stat st;
  long long v = 0;
  if (stat(NUVIO_LAN_DEBUG_FLAG, &st) != 0)
    return 0;
  FILE *f = fopen(NUVIO_DATA_DIR "/clock-test-offset", "r");
  if (!f)
    return 0;
  if (fscanf(f, "%lld", &v) != 1)
    v = 0;
  fclose(f);
  return v;
}

char *nuvio_clock_json(void) {
  char buf[192];
  const long long test = test_offset_ms();
  pthread_mutex_lock(&g_lock);
  if (g_known)
    snprintf(buf, sizeof buf, "{\"returnValue\":true,\"known\":true,\"offsetMs\":%lld,\"source\":\"%s\"}",
             g_offset_ms + test, g_source);
  else
    snprintf(buf, sizeof buf, "{\"returnValue\":true,\"known\":false,\"offsetMs\":0}");
  pthread_mutex_unlock(&g_lock);
  return strdup(buf);
}
