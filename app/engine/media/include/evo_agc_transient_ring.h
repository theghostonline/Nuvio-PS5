#ifndef EVO_AGC_TRANSIENT_RING_H
#define EVO_AGC_TRANSIENT_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EVO_AGC_TRANSIENT_MAX_SLOTS = 3,
};

enum evo_agc_transient_slot_state {
    EVO_AGC_TRANSIENT_EMPTY = 0,
    EVO_AGC_TRANSIENT_OPEN = 1,
    EVO_AGC_TRANSIENT_SEALED = 2,
};

typedef struct evo_agc_transient_slice {
    void    *cpu;
    uint64_t gpu_addr;
    size_t   offset;
    size_t   bytes;
} evo_agc_transient_slice_t;

typedef struct evo_agc_transient_slot {
    size_t   offset;
    size_t   bytes;
    size_t   used;
    uint64_t retire_token;
    uint8_t  state;
} evo_agc_transient_slot_t;

typedef struct evo_agc_transient_ring {
    uint8_t                  *base;
    uint64_t                  gpu_base;
    size_t                    bytes;
    uint32_t                  slot_count;
    evo_agc_transient_slot_t  slots[EVO_AGC_TRANSIENT_MAX_SLOTS];
} evo_agc_transient_ring_t;

enum evo_agc_transient_result {
    EVO_AGC_TRANSIENT_OK = 0,
    EVO_AGC_TRANSIENT_PRECONDITION = -1,
    EVO_AGC_TRANSIENT_SLOT_BUSY = -2,
    EVO_AGC_TRANSIENT_TOKEN_MISMATCH = -3,
    EVO_AGC_TRANSIENT_EXHAUSTED = -4,
};

int evo_agc_transient_ring_init(evo_agc_transient_ring_t *ring, void *base,
                                uint64_t gpu_base, size_t bytes,
                                uint32_t slot_count, size_t slot_alignment);

int evo_agc_transient_ring_begin(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                 uint64_t completed_token, int completion_proven);

int evo_agc_transient_ring_alloc(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                 size_t bytes, size_t alignment,
                                 evo_agc_transient_slice_t *slice);

int evo_agc_transient_ring_seal(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                uint64_t retire_token);

int evo_agc_transient_ring_abort(evo_agc_transient_ring_t *ring, uint32_t slot_index);

size_t evo_agc_transient_ring_used(const evo_agc_transient_ring_t *ring, uint32_t slot_index);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AGC_TRANSIENT_RING_H */
