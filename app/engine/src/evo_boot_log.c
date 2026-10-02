/* evo_boot_log.c — see evo_boot_log.h. The one diagnostic log. */
#include "evo_boot_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef EVO_APP_MODULE

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#ifdef EVO_BOOT_TRACE_POPUP
struct bl_note { char pad[45]; char msg[3075]; };
extern int sceKernelSendNotificationRequest(int, void *, unsigned long, int);
#endif

#define EVO_LOG_PATH "/mnt/usb0/evo.log"

/*
 * Two phases:
 *   1. before /mnt/usb0 is reachable (boot, pre-self-unjail) - lines are held
 *      in g_boot. A hard cap; overflow bumps g_dropped and is reported once
 *      the file opens.
 *   2. after evo_boot_log_flush() first opens the file - lines go into a
 *      memory queue and a writer thread puts them on the USB stick.
 *
 * Why a writer thread: the log lives on the same USB stick a movie streams
 * from. Writing it from the caller - fflush per line, plus the render loop's
 * once-a-second fsync - blocked the main loop for 33-83 ms whenever the stick
 * was busy, which is a visible hitch in 4K playback (hardware 2026-09-28,
 * `pace:` trace, LG "Art" demo). Now evo_boot_log() is a memcpy, the
 * periodic flush from the loop only wakes the writer (evo_boot_log_kick),
 * and only an explicit evo_boot_log_flush() - a breadcrumb before something
 * risky - still waits for the stick. The crash handler writes out whatever
 * is still queued (evo_boot_log_crash_drain), so the last lines survive.
 */
#define BL_BOOT_CAP  16384
#define BL_QUEUE_CAP (256 * 1024)
#define BL_SYNC_MS   2000

static char   g_boot[BL_BOOT_CAP];
static size_t g_boot_len;
static int    g_dropped;
static FILE  *g_fp;

static pthread_mutex_t g_q_lock  = PTHREAD_MUTEX_INITIALIZER;  /* the queue */
static pthread_mutex_t g_io_lock = PTHREAD_MUTEX_INITIALIZER;  /* g_fp writes */
static pthread_cond_t  g_q_cond  = PTHREAD_COND_INITIALIZER;
static char   g_q[BL_QUEUE_CAP];
static size_t g_q_len;
static int    g_q_dropped;
static int    g_want_sync;
static int    g_writer_up;

/* Take the queue and write it. Caller holds nothing; returns bytes written. */
static size_t bl_drain(int do_sync)
{
    static char out[BL_QUEUE_CAP];
    size_t n;
    int dropped;

    pthread_mutex_lock(&g_io_lock);
    pthread_mutex_lock(&g_q_lock);
    n = g_q_len;
    memcpy(out, g_q, n);
    g_q_len = 0;
    dropped = g_q_dropped;
    g_q_dropped = 0;
    pthread_mutex_unlock(&g_q_lock);

    if (g_fp) {
        if (dropped)
            fprintf(g_fp, "[log] %d line(s) dropped - log queue full\n", dropped);
        if (n)
            fwrite(out, 1, n, g_fp);
        fflush(g_fp);
        if (do_sync)
            fsync(fileno(g_fp));
    }
    pthread_mutex_unlock(&g_io_lock);
    return n;
}

static void *bl_writer(void *arg)
{
    (void)arg;
    struct timespec last_sync;
    clock_gettime(CLOCK_MONOTONIC, &last_sync);
    for (;;) {
        pthread_mutex_lock(&g_q_lock);
        if (g_q_len == 0 && !g_want_sync) {
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 250 * 1000000L;
            if (until.tv_nsec >= 1000000000L) {
                until.tv_sec++;
                until.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_q_cond, &g_q_lock, &until);
        }
        const int want_sync = g_want_sync;
        g_want_sync = 0;
        pthread_mutex_unlock(&g_q_lock);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        const long since_ms = (long)(now.tv_sec - last_sync.tv_sec) * 1000L +
                              (now.tv_nsec - last_sync.tv_nsec) / 1000000L;
        const int do_sync = want_sync || since_ms >= BL_SYNC_MS;
        bl_drain(do_sync);
        if (do_sync)
            last_sync = now;
    }
    return NULL;
}

#ifdef EVO_NUVIO_EDITION
/*
 * Nuvio edition: the app's users have no USB stick in, so the log also goes
 * to the nuvio-ps5 payload (POST 127.0.0.1:17600/api/log, read back from
 * GET /api/logs). Its own thread and queue: a loopback connect that is
 * refused (payload not loaded yet) just keeps the lines for the next try.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define NL_CAP (64 * 1024)
static char            g_nl[NL_CAP];
static size_t          g_nl_len;
static int             g_nl_dropped;
static int             g_nl_up;
static pthread_mutex_t g_nl_lock = PTHREAD_MUTEX_INITIALIZER;

static int nl_post(const char *body, size_t len)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(17600);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    char head[160];
    int hl = snprintf(head, sizeof head,
                      "POST /api/log HTTP/1.0\r\nContent-Type: text/plain\r\n"
                      "Content-Length: %zu\r\n\r\n", len);
    int ok = write(fd, head, (size_t)hl) == hl;
    for (size_t off = 0; ok && off < len;) {
        ssize_t w = write(fd, body + off, len - off);
        if (w <= 0) ok = 0; else off += (size_t)w;
    }
    char sink[256];
    while (ok && read(fd, sink, sizeof sink) > 0) {}
    close(fd);
    return ok ? 0 : -1;
}

static void *nl_thread(void *arg)
{
    static char out[NL_CAP + 64];
    (void)arg;
    for (;;) {
        usleep(500 * 1000);
        pthread_mutex_lock(&g_nl_lock);
        const int dropped = g_nl_dropped;
        const size_t pre = dropped
            ? (size_t)snprintf(out, 64, "[evo] [log] %d line(s) dropped\n", dropped) : 0;
        const size_t body = g_nl_len;
        memcpy(out + pre, g_nl, body);
        pthread_mutex_unlock(&g_nl_lock);
        if (pre + body == 0 || nl_post(out, pre + body) != 0)
            continue;
        /* Sent: drop what went out, keep anything queued meanwhile. */
        pthread_mutex_lock(&g_nl_lock);
        g_nl_dropped -= dropped;
        memmove(g_nl, g_nl + body, g_nl_len - body);
        g_nl_len -= body;
        pthread_mutex_unlock(&g_nl_lock);
    }
    return NULL;
}

static void nl_queue(const char *stamped, size_t m)
{
    pthread_mutex_lock(&g_nl_lock);
    if (!g_nl_up) {
        pthread_t t;
        if (pthread_create(&t, NULL, nl_thread, NULL) == 0) {
            pthread_detach(t);
            g_nl_up = 1;
        }
    }
    if (g_nl_len + 6 + m <= NL_CAP) {
        memcpy(g_nl + g_nl_len, "[evo] ", 6);
        memcpy(g_nl + g_nl_len + 6, stamped, m);
        g_nl_len += 6 + m;
    } else {
        g_nl_dropped++;
    }
    pthread_mutex_unlock(&g_nl_lock);
}
#endif

void evo_boot_log(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    printf("EVO boot: %s\n", line);
    fflush(stdout);

#ifdef EVO_BOOT_TRACE_POPUP
    /* #51: on-screen popup — opt-in (--breadcrumbs). Off by default; the
     * EVO_LOG_PATH file is the durable channel and is always written. */
    struct bl_note nt;
    memset(&nt, 0, sizeof nt);
    snprintf(nt.msg, sizeof nt.msg, "%s", line);
    sceKernelSendNotificationRequest(0, &nt, sizeof nt, 0);
#endif

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char stamped[680];
    int m = snprintf(stamped, sizeof stamped, "[%lld.%03ld] %s\n",
                     (long long)ts.tv_sec, ts.tv_nsec / 1000000L, line);
    if (m < 0)
        return;
    if ((size_t)m >= sizeof stamped)
        m = (int)sizeof stamped - 1;

#ifdef EVO_NUVIO_EDITION
    nl_queue(stamped, (size_t)m);
#endif

    pthread_mutex_lock(&g_q_lock);
    if (g_fp) {
        if (g_q_len + (size_t)m <= BL_QUEUE_CAP) {
            memcpy(g_q + g_q_len, stamped, (size_t)m);
            g_q_len += (size_t)m;
        } else {
            g_q_dropped++;
        }
        pthread_cond_signal(&g_q_cond);
    } else if (g_boot_len + (size_t)m < BL_BOOT_CAP) {
        memcpy(g_boot + g_boot_len, stamped, (size_t)m);
        g_boot_len += (size_t)m;
    } else {
        g_dropped++;
    }
    pthread_mutex_unlock(&g_q_lock);
}

void evo_boot_log_flush(void)
{
    if (!g_fp) {
        FILE *fp = fopen(EVO_LOG_PATH, "a");
        if (!fp)
            return;   /* /mnt/usb0 not reachable yet — try again next call */
        pthread_mutex_lock(&g_io_lock);
        pthread_mutex_lock(&g_q_lock);
        if (g_dropped) {
            fprintf(fp, "[log] %d pre-mount line(s) dropped\n", g_dropped);
            g_dropped = 0;
        }
        if (g_boot_len) {
            fwrite(g_boot, 1, g_boot_len, fp);
            g_boot_len = 0;
        }
        g_fp = fp;
        pthread_mutex_unlock(&g_q_lock);
        pthread_mutex_unlock(&g_io_lock);

        if (!g_writer_up) {
            pthread_t t;
            if (pthread_create(&t, NULL, bl_writer, NULL) == 0) {
                pthread_detach(t);
                g_writer_up = 1;
            }
        }
    }
    /* An explicit flush is a breadcrumb: it waits until the line is on the
     * stick. */
    bl_drain(1);
}

void evo_boot_log_kick(void)
{
    if (!g_fp) {
        evo_boot_log_flush();   /* still in phase 1: try to open the file */
        return;
    }
    if (!g_writer_up) {
        bl_drain(1);            /* no writer thread: behave as before */
        return;
    }
    pthread_mutex_lock(&g_q_lock);
    g_want_sync = 1;
    pthread_cond_signal(&g_q_cond);
    pthread_mutex_unlock(&g_q_lock);
}

void evo_boot_log_crash_drain(int fd)
{
    /* Signal context: no locks, no stdio. Best effort - a line being queued
     * at the instant of the crash may be torn. */
    if (fd < 0)
        return;
    if (g_q_len && g_q_len <= BL_QUEUE_CAP)
        (void)write(fd, g_q, g_q_len);
    else if (!g_fp && g_boot_len && g_boot_len <= BL_BOOT_CAP)
        (void)write(fd, g_boot, g_boot_len);
}

#else  /* host / payload */

void evo_boot_log(const char *fmt, ...) { (void)fmt; }
void evo_boot_log_flush(void) {}
void evo_boot_log_kick(void) {}
void evo_boot_log_crash_drain(int fd) { (void)fd; }

#endif
