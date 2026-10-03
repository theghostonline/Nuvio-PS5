/*
 * evo_parallel_io.c - Nuvio PS5: parallel read-ahead for big network files.
 *
 * One HTTP connection is often the limit, not the line: a Wi-Fi link that
 * carries 31 Mbit/s per connection, a debrid host that caps each connection.
 * An 8K VR file needs 80+. So a big file served with byte ranges is fetched by
 * PIO_WORKERS connections in PIO_CHUNK pieces, kept up to PIO_WINDOW pieces
 * ahead of the reader, and FFmpeg reads from that cache through a custom
 * AVIOContext. A seek starts a new window where it lands.
 */
#include "evo_parallel_io.h"
#include "evo_boot_log.h"
#include "evo_thread.h"

#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

#include <pthread.h>
#include <sys/types.h>

/* The cache lives in direct memory, allocated from the system rather than the
 * app's 128 MB pool, because flexible (malloc) memory is the scarce one here. */
int32_t sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align,
                                      int type, int64_t *offset);
int32_t sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t offset,
                                 size_t align);
int32_t sceKernelReleaseDirectMemory(int64_t start, size_t len);
int32_t sceKernelMunmap(void *addr, size_t len);
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PIO_CHUNK    (4 * 1024 * 1024)
#define PIO_WINDOW   32                       /* chunks: 128 MB ahead */
#define PIO_WORKERS  6
#define PIO_MIN_SIZE (96LL * 1024 * 1024)     /* smaller files: one connection is fine */

typedef struct {
    int64_t index;                /* which chunk this slot holds, -1 none */
    int     state;                /* 0 free, 1 fetching, 2 ready, 3 failed */
    int     len;
    uint8_t *data;
} pio_slot;

struct evo_pio {
    char    url[2048];
    char    headers[2048];
    char    ua[256];
    int64_t size;
    int64_t pos;                  /* the reader's position */
    int64_t base;                 /* first chunk of the current window */
    pio_slot slot[PIO_WINDOW];
    pthread_t workers[PIO_WORKERS];
    int     nworkers;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    volatile int stop;
    volatile int aborted;
    AVIOContext *avio;
    uint64_t bytes, t0_us;        /* throughput, for the log */
    uint8_t *mem;                 /* the PIO_WINDOW chunks */
    int64_t  mem_off;
    int     logged;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static int open_http(evo_pio *p, AVIOContext **pb, int64_t offset)
{
    AVDictionary *o = NULL;
    if (p->headers[0])
        av_dict_set(&o, "headers", p->headers, 0);
    if (p->ua[0])
        av_dict_set(&o, "user_agent", p->ua, 0);
    av_dict_set(&o, "rw_timeout", "8000000", 0);
    av_dict_set(&o, "reconnect", "1", 0);
    av_dict_set(&o, "reconnect_on_network_error", "1", 0);
    av_dict_set(&o, "reconnect_delay_max", "2", 0);
    if (offset > 0) {
        char off[32];
        snprintf(off, sizeof off, "%lld", (long long)offset);
        av_dict_set(&o, "offset", off, 0);
    }
    int r = avio_open2(pb, p->url, AVIO_FLAG_READ, NULL, &o);
    av_dict_free(&o);
    return r;
}

/* The next chunk nobody is fetching, nearest the reader first. */
static pio_slot *claim(evo_pio *p, int64_t *idx)
{
    const int64_t nchunks = (p->size + PIO_CHUNK - 1) / PIO_CHUNK;
    for (int64_t k = p->base; k < p->base + PIO_WINDOW && k < nchunks; k++) {
        pio_slot *s = &p->slot[k % PIO_WINDOW];
        if (s->index == k && s->state != 0)
            continue;                         /* fetching or ready */
        if (s->index != k && s->state == 1)
            continue;                         /* a stale fetch still owns it */
        s->index = k;
        s->state = 1;
        s->len = 0;
        *idx = k;
        return s;
    }
    return NULL;
}

static void *worker(void *arg)
{
    evo_pio *p = arg;
    AVIOContext *pb = NULL;
    int64_t at = -1;                          /* where pb's next read lands */
    while (!p->stop) {
        pthread_mutex_lock(&p->mu);
        int64_t idx = -1;
        pio_slot *s;
        while (!p->stop && !(s = claim(p, &idx)))
            pthread_cond_wait(&p->cv, &p->mu);
        pthread_mutex_unlock(&p->mu);
        if (p->stop)
            break;
        const int64_t off = idx * PIO_CHUNK;
        const int want = (int)((p->size - off) < PIO_CHUNK ? (p->size - off) : PIO_CHUNK);
        int got = 0;
        for (int attempt = 0; attempt < 3 && got < want && !p->stop; attempt++) {
            if (!pb || at != off + got) {     /* a new range: (re)connect there */
                if (pb)
                    avio_closep(&pb);
                if (open_http(p, &pb, off + got) < 0) {
                    pb = NULL;
                    usleep(200000);
                    continue;
                }
                at = off + got;
            }
            while (got < want && !p->stop) {
                /* straight into the slot: it is ours while state is 1 */
                const int n = avio_read(pb, s->data + got, want - got);
                if (n <= 0)
                    break;
                got += n;
                at += n;
            }
            if (got < want) {
                avio_closep(&pb);
                at = -1;
            }
        }
        pthread_mutex_lock(&p->mu);
        if (s->index == idx && s->state == 1) {
            if (got == want) {
                s->len = got;
                s->state = 2;
                p->bytes += (uint64_t)got;
            } else {
                s->state = 3;
            }
        }
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
    if (pb)
        avio_closep(&pb);
    return NULL;
}

static int pio_read(void *opaque, uint8_t *dst, int len)
{
    evo_pio *p = opaque;
    if (p->pos >= p->size)
        return AVERROR_EOF;
    const int64_t idx = p->pos / PIO_CHUNK;
    pthread_mutex_lock(&p->mu);
    if (idx < p->base || idx >= p->base + PIO_WINDOW / 2) {
        p->base = idx;                        /* moved (seek or ran ahead): a new window */
        pthread_cond_broadcast(&p->cv);
    }
    pio_slot *s = &p->slot[idx % PIO_WINDOW];
    const uint64_t give_up = now_us() + 30000000u;   /* 30 s without the bytes: an error */
    while (!p->stop && !p->aborted && !(s->index == idx && (s->state == 2 || s->state == 3))) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&p->cv, &p->mu, &ts);
        if (now_us() > give_up)
            break;
    }
    if (p->aborted || p->stop) {
        pthread_mutex_unlock(&p->mu);
        return AVERROR_EXIT;
    }
    if (!(s->index == idx && s->state == 2)) {
        if (s->index == idx && s->state == 3) {
            s->state = 0;                     /* failed: a worker fetches it again */
            pthread_cond_broadcast(&p->cv);
        }
        pthread_mutex_unlock(&p->mu);
        return AVERROR(EIO);
    }
    const int in = (int)(p->pos - idx * PIO_CHUNK);
    int n = s->len - in;
    if (n > len)
        n = len;
    memcpy(dst, s->data + in, (size_t)n);
    p->pos += n;
    /* finished with a chunk: free its slot for one further ahead */
    if (p->pos / PIO_CHUNK != idx) {
        s->state = 0;
        s->index = -1;
        p->base = p->pos / PIO_CHUNK;
        pthread_cond_broadcast(&p->cv);
    }
    pthread_mutex_unlock(&p->mu);
    if (!p->logged && p->bytes > 128u * 1024 * 1024) {
        p->logged = 1;
        const double s_ = (now_us() - p->t0_us) / 1e6;
        evo_boot_log("pio: first 128 MB at %.0f Mbit/s over %d connections",
                     (double)p->bytes * 8 / 1e6 / s_, p->nworkers);
    }
    return n;
}

static int64_t pio_seek(void *opaque, int64_t offset, int whence)
{
    evo_pio *p = opaque;
    if (whence == AVSEEK_SIZE)
        return p->size;
    whence &= ~AVSEEK_FORCE;
    int64_t to = whence == SEEK_SET ? offset : whence == SEEK_CUR ? p->pos + offset
               : whence == SEEK_END ? p->size + offset : -1;
    if (to < 0 || to > p->size)
        return AVERROR(EINVAL);
    p->pos = to;
    return to;
}

evo_pio *evo_pio_open(const char *url, const char *headers, const char *user_agent)
{
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)))
        return NULL;
    evo_pio *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    snprintf(p->url, sizeof p->url, "%s", url);
    if (headers)
        snprintf(p->headers, sizeof p->headers, "%s", headers);
    if (user_agent)
        snprintf(p->ua, sizeof p->ua, "%s", user_agent);
    /* size, and whether ranges work: open at an offset and see where it lands */
    AVIOContext *pb = NULL;
    if (open_http(p, &pb, 0) < 0) {
        free(p);
        return NULL;
    }
    p->size = avio_size(pb);
    const int seekable = (pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    avio_closep(&pb);
    if (p->size < PIO_MIN_SIZE || !seekable) {
        evo_boot_log("pio: not used (size %lld, ranges %s)", (long long)p->size, seekable ? "yes" : "no");
        free(p);
        return NULL;
    }
    const size_t bytes = (size_t)PIO_WINDOW * PIO_CHUNK;
    p->mem_off = -1;
    void *va = NULL;
    if (sceKernelAllocateDirectMemory(0, (int64_t)16 << 30, bytes, 0x200000, 12, &p->mem_off) != 0 ||
        sceKernelMapDirectMemory(&va, bytes, 0x33, 0, p->mem_off, 0x200000) != 0) {
        evo_boot_log("pio: no direct memory for the cache");
        evo_pio_close(p);
        return NULL;
    }
    p->mem = va;
    for (int i = 0; i < PIO_WINDOW; i++) {
        p->slot[i].index = -1;
        p->slot[i].data = p->mem + (size_t)i * PIO_CHUNK;
    }
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->cv, NULL);
    p->t0_us = now_us();
    /* evo_thread_create, not pthread_create: these workers run FFmpeg's HTTP
     * reader, and the console's default thread stack is too small for it. That
     * is the same overflow that closed the app with CE-108255-1 in 1.7.2. */
    for (int i = 0; i < PIO_WORKERS; i++)
        if (evo_thread_create(&p->workers[i], worker, p) == 0)
            p->nworkers++;
    uint8_t *iobuf = av_malloc(1 << 20);
    p->avio = iobuf ? avio_alloc_context(iobuf, 1 << 20, 0, p, pio_read, NULL, pio_seek) : NULL;
    if (!p->avio) {
        av_free(iobuf);
        evo_pio_close(p);
        return NULL;
    }
    p->avio->seekable = AVIO_SEEKABLE_NORMAL;
    evo_boot_log("pio: %lld MB file, %d connections, %d MB read-ahead", (long long)(p->size >> 20),
                 p->nworkers, PIO_WINDOW * PIO_CHUNK >> 20);
    return p;
}

void evo_pio_abort(evo_pio *p)
{
    if (!p)
        return;
    p->aborted = 1;
    if (p->nworkers) {
        pthread_mutex_lock(&p->mu);
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
    }
}

AVIOContext *evo_pio_avio(evo_pio *p)
{
    return p ? p->avio : NULL;
}

void evo_pio_close(evo_pio *p)
{
    if (!p)
        return;
    if (p->nworkers) {
        pthread_mutex_lock(&p->mu);
        p->stop = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
        for (int i = 0; i < p->nworkers; i++)
            pthread_join(p->workers[i], NULL);
        pthread_mutex_destroy(&p->mu);
        pthread_cond_destroy(&p->cv);
    }
    if (p->avio) {
        av_freep(&p->avio->buffer);
        avio_context_free(&p->avio);
    }
    if (p->mem)
        sceKernelMunmap(p->mem, (size_t)PIO_WINDOW * PIO_CHUNK);
    if (p->mem_off >= 0 && p->mem)
        sceKernelReleaseDirectMemory(p->mem_off, (size_t)PIO_WINDOW * PIO_CHUNK);
    free(p);
}
