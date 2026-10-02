/*
 * evo_packet_queue.c — bounded AVPacket FIFO shared by the demux and decode
 * threads.
 *
 * Verbatim move of the PacketQueue helpers from main.c (Track A step A1 of
 * docs/modularisation-plan.md). No behaviour change: the demux still owns the
 * two queue instances until step A5.
 */
#include "evo_packet_queue.h"

#include <stdlib.h>
#include <string.h>
#include <libavcodec/defs.h>
#include <libavutil/buffer.h>

/* ---------------------------------------------------------------------------
 * Packet data ring.
 *
 * Regions are carved at the head and retired at the tail. Packets are freed
 * roughly in order - each queue has one consumer - but a decoder may hold one
 * a little longer than its successor, so a region is only marked free and the
 * tail advances over every consecutive freed region. A region that does not
 * fit before the end of the store leaves a freed pad behind and starts again
 * at offset 0. Every region is a multiple of RING_ALIGN, so a pad is always
 * big enough for its header.
 * ------------------------------------------------------------------------ */
#define RING_HDR   64
#define RING_ALIGN 64

struct PacketRing {
    uint8_t *base;
    size_t   size;
    size_t   head;          /* next region starts here */
    size_t   tail;          /* oldest live region */
    size_t   used;          /* bytes from tail to head, pads included */
    pthread_mutex_t mu;
};

typedef struct {
    uint32_t size;          /* whole region, header included */
    uint32_t freed;
    struct PacketRing *ring;
} ring_hdr_t;

volatile unsigned long packet_queue_ring_fallbacks = 0;

static uint8_t *ring_alloc(struct PacketRing *r, size_t n)
{
    const size_t need = (RING_HDR + n + AV_INPUT_BUFFER_PADDING_SIZE + RING_ALIGN - 1) &
                        ~(size_t)(RING_ALIGN - 1);
    if (need > UINT32_MAX || need > r->size)
        return NULL;

    pthread_mutex_lock(&r->mu);
    if (r->used == 0)
        r->head = r->tail = 0;

    size_t off = (size_t)-1;
    if (r->used == 0 || r->head > r->tail) {
        /* Free space is [head, size) then [0, tail). */
        if (r->size - r->head >= need) {
            off = r->head;
        } else if (r->tail >= need) {
            ring_hdr_t *pad = (ring_hdr_t *)(r->base + r->head);
            pad->size  = (uint32_t)(r->size - r->head);
            pad->freed = 1;
            pad->ring  = r;
            r->used += r->size - r->head;
            off = 0;
        }
    } else if (r->head < r->tail) {
        if (r->tail - r->head >= need)
            off = r->head;
    }
    /* head == tail with used > 0: full. */

    if (off == (size_t)-1) {
        pthread_mutex_unlock(&r->mu);
        return NULL;
    }

    ring_hdr_t *h = (ring_hdr_t *)(r->base + off);
    h->size  = (uint32_t)need;
    h->freed = 0;
    h->ring  = r;
    r->head = off + need;
    if (r->head == r->size)
        r->head = 0;
    r->used += need;
    pthread_mutex_unlock(&r->mu);
    return (uint8_t *)h + RING_HDR;
}

static void ring_release(void *opaque, uint8_t *data)
{
    (void)opaque;
    ring_hdr_t *h = (ring_hdr_t *)(data - RING_HDR);
    struct PacketRing *r = h->ring;

    pthread_mutex_lock(&r->mu);
    h->freed = 1;
    while (r->used > 0) {
        ring_hdr_t *t = (ring_hdr_t *)(r->base + r->tail);
        if (!t->freed)
            break;
        r->used -= t->size;
        r->tail += t->size;
        if (r->tail >= r->size)
            r->tail = 0;
    }
    if (r->used == 0)
        r->head = r->tail = 0;
    pthread_mutex_unlock(&r->mu);
}

/* Copy `src` into the ring. NULL when the ring has no room. */
static AVPacket *ring_clone(struct PacketRing *r, const AVPacket *src)
{
    uint8_t *d = ring_alloc(r, (size_t)src->size);
    if (!d)
        return NULL;
    AVPacket *p = av_packet_alloc();
    if (!p) {
        ring_release(NULL, d);
        return NULL;
    }
    p->buf = av_buffer_create(d, (size_t)src->size + AV_INPUT_BUFFER_PADDING_SIZE,
                              ring_release, NULL, 0);
    if (!p->buf) {
        ring_release(NULL, d);
        av_packet_free(&p);
        return NULL;
    }
    if (av_packet_copy_props(p, src) < 0) {
        av_packet_free(&p);            /* releases the region with the buffer */
        return NULL;
    }
    memcpy(d, src->data, (size_t)src->size);
    memset(d + src->size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    p->data = d;
    p->size = src->size;
    return p;
}

size_t packet_queue_ring_size(PacketQueue *q)
{
    return q->ring ? q->ring->size : 0;
}

int packet_queue_use_ring(PacketQueue *q, size_t bytes)
{
    if (q->ring)
        return 1;
    bytes &= ~(size_t)(RING_ALIGN - 1);
    struct PacketRing *r = (struct PacketRing *)calloc(1, sizeof *r);
    if (!r)
        return 0;
    /* One allocation, so one mapping; the shim serves it from direct memory.
     * Halve on refusal rather than go without. */
    for (;;) {
        r->base = (uint8_t *)malloc(bytes);
        if (r->base || bytes < ((size_t)64 << 20))
            break;
        bytes = (bytes / 2) & ~(size_t)(RING_ALIGN - 1);
    }
    if (!r->base) {
        free(r);
        return 0;
    }
    r->size = bytes;
    pthread_mutex_init(&r->mu, NULL);
    pthread_mutex_lock(&q->mutex);
    q->ring = r;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void packet_queue_clear(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    for (int i = 0; i < PACKET_QUEUE_SIZE; i++) {
        if (q->packets[i]) {
            av_packet_free(&q->packets[i]);
            q->packets[i] = NULL;
        }
    }

    q->read = 0;
    q->write = 0;
    q->count = 0;
    q->total_dur_us = 0;
    q->total_bytes = 0;

    pthread_mutex_unlock(&q->mutex);
}

int packet_queue_push(PacketQueue *q, AVPacket *pkt) {
    return packet_queue_push_timed(q, pkt, 0);
}

int packet_queue_push_timed(PacketQueue *q, AVPacket *pkt, int64_t dur_us) {
    if (dur_us < 0) dur_us = 0;
    if (dur_us > INT32_MAX) dur_us = INT32_MAX;

    /* Copy outside the lock: the decode thread pops from this queue at frame
     * rate. The ring takes the data when there is one with room; otherwise a
     * plain reference to the demuxer's buffer. */
    AVPacket *clone = NULL;
    if (q->ring && pkt->size > 0 && pkt->data) {
        clone = ring_clone(q->ring, pkt);
        if (!clone)
            packet_queue_ring_fallbacks++;
    }
    if (!clone)
        clone = av_packet_clone(pkt);
    if (!clone)
        return 0;

    pthread_mutex_lock(&q->mutex);

    if (q->count >= PACKET_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        av_packet_free(&clone);
        return 0;
    }

    q->packets[q->write] = clone;
    q->dur_us[q->write] = (int32_t)dur_us;
    q->write = (q->write + 1) % PACKET_QUEUE_SIZE;
    q->count++;
    q->total_dur_us += dur_us;
    q->total_bytes += clone->size;

    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void packet_queue_level(PacketQueue *q, int *count, int64_t *dur_us, int64_t *bytes) {
    pthread_mutex_lock(&q->mutex);
    if (count)  *count  = q->count;
    if (dur_us) *dur_us = q->total_dur_us;
    if (bytes)  *bytes  = q->total_bytes;
    pthread_mutex_unlock(&q->mutex);
}

AVPacket *packet_queue_pop(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);

    if (q->count <= 0) {
        pthread_mutex_unlock(&q->mutex);
        return NULL;
    }

    AVPacket *pkt = q->packets[q->read];
    q->packets[q->read] = NULL;
    q->total_dur_us -= q->dur_us[q->read];
    if (pkt) q->total_bytes -= pkt->size;
    q->read = (q->read + 1) % PACKET_QUEUE_SIZE;
    q->count--;
    if (q->count == 0) {
        q->total_dur_us = 0;
        q->total_bytes = 0;
    }

    pthread_mutex_unlock(&q->mutex);
    return pkt;
}

int packet_queue_count(PacketQueue *q) {
    pthread_mutex_lock(&q->mutex);
    int c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}
