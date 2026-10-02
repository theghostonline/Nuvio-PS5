/*
 * evo_packet_queue.h — bounded AVPacket FIFO shared by the demux and decode
 * threads.
 *
 * A fixed-capacity ring of cloned AVPackets guarded by a single mutex. push
 * clones the packet in, pop hands ownership out, clear frees everything.
 * Pure leaf: no clocks, no threads of its own, FFmpeg the only dependency.
 */
#ifndef EVO_PACKET_QUEUE_H
#define EVO_PACKET_QUEUE_H

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <libavcodec/packet.h>

/*
 * Slots, not a budget: the demux caps a queue by the seconds and bytes it
 * holds (evo_demux.c), and this only has to be deep enough never to be the
 * limit first. TrueHD in Matroska runs at 1200 packets/s: 512 slots was
 * under half a second of audio, 8192 under seven - and a full audio queue
 * holds video read-ahead to the same span. 65536 covers the thirty-second
 * budget with room for the starvation overshoot.
 */
#define PACKET_QUEUE_SIZE 65536

struct PacketRing;

typedef struct {
    AVPacket *packets[PACKET_QUEUE_SIZE];
    int32_t   dur_us[PACKET_QUEUE_SIZE];   /* per slot, as pushed */
    int read;
    int write;
    int count;
    int64_t total_dur_us;                  /* sum of dur_us over queued slots */
    int64_t total_bytes;                   /* sum of packet sizes */
    struct PacketRing *ring;               /* packet data store, see below */
    pthread_mutex_t mutex;
} PacketQueue;

/*
 * Give the queue one contiguous store of `bytes` for the packet data it holds.
 * Allocated on the first call and kept for the life of the process (later
 * calls are no-ops); returns 1 if the queue has a ring.
 *
 * Without it every queued packet keeps the demuxer's own buffer alive, one
 * mapping per packet above the allocator's slab sizes - thousands of live
 * mappings once the queue holds tens of seconds of 4K. With it the demuxer's
 * buffer is released as soon as the packet is copied in.
 */
int packet_queue_use_ring(PacketQueue *q, size_t bytes);

/* Bytes the queue's ring holds; 0 without one. */
size_t packet_queue_ring_size(PacketQueue *q);

/* Packets that did not fit the ring and fell back to a plain reference. */
extern volatile unsigned long packet_queue_ring_fallbacks;

/* Free every queued packet and reset the ring to empty. */
void packet_queue_clear(PacketQueue *q);

/* Clone `pkt` into the queue. Returns 1 on success, 0 if the queue is full. */
int packet_queue_push(PacketQueue *q, AVPacket *pkt);

/* As packet_queue_push, recording the packet's duration (microseconds) so
 * the queue can report how much playback time it holds. */
int packet_queue_push_timed(PacketQueue *q, AVPacket *pkt, int64_t dur_us);

/* Count, held duration (us) and held bytes in one consistent read. */
void packet_queue_level(PacketQueue *q, int *count, int64_t *dur_us, int64_t *bytes);

/* Pop the oldest packet, transferring ownership to the caller. NULL if empty. */
AVPacket *packet_queue_pop(PacketQueue *q);

/* Current number of queued packets. */
int packet_queue_count(PacketQueue *q);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PACKET_QUEUE_H */
