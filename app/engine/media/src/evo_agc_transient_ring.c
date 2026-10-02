#include "evo_agc_transient_ring.h"

#include <limits.h>
#include <string.h>

static int is_power_of_two(size_t value)
{
    return value && (value & (value - 1u)) == 0u;
}

int evo_agc_transient_ring_init(evo_agc_transient_ring_t *ring, void *base,
                                uint64_t gpu_base, size_t bytes,
                                uint32_t slot_count, size_t slot_alignment)
{
    if (!ring || !base || bytes == 0u || slot_count == 0u ||
        slot_count > EVO_AGC_TRANSIENT_MAX_SLOTS ||
        !is_power_of_two(slot_alignment) ||
        ((uintptr_t)base & (slot_alignment - 1u)) != 0u)
        return EVO_AGC_TRANSIENT_PRECONDITION;

    const size_t raw_slot_bytes = bytes / slot_count;
    const size_t slot_bytes = raw_slot_bytes & ~(slot_alignment - 1u);
    if (slot_bytes == 0u || slot_bytes > SIZE_MAX / slot_count)
        return EVO_AGC_TRANSIENT_PRECONDITION;

    memset(ring, 0, sizeof(*ring));
    ring->base = (uint8_t *)base;
    ring->gpu_base = gpu_base ? gpu_base : (uint64_t)(uintptr_t)base;
    ring->bytes = slot_bytes * slot_count;
    ring->slot_count = slot_count;

    for (uint32_t i = 0; i < slot_count; ++i) {
        ring->slots[i].offset = (size_t)i * slot_bytes;
        ring->slots[i].bytes = slot_bytes;
        ring->slots[i].used = 0u;
        ring->slots[i].retire_token = 0u;
        ring->slots[i].state = EVO_AGC_TRANSIENT_EMPTY;
    }

    return EVO_AGC_TRANSIENT_OK;
}

int evo_agc_transient_ring_begin(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                 uint64_t completed_token, int completion_proven)
{
    if (!ring || !ring->base || slot_index >= ring->slot_count)
        return EVO_AGC_TRANSIENT_PRECONDITION;

    evo_agc_transient_slot_t *slot = &ring->slots[slot_index];
    if (slot->state == EVO_AGC_TRANSIENT_OPEN)
        return EVO_AGC_TRANSIENT_SLOT_BUSY;

    if (slot->state == EVO_AGC_TRANSIENT_SEALED) {
        if (!completion_proven)
            return EVO_AGC_TRANSIENT_SLOT_BUSY;
        if (completed_token == 0u || completed_token != slot->retire_token)
            return EVO_AGC_TRANSIENT_TOKEN_MISMATCH;
    }

    slot->used = 0u;
    slot->retire_token = 0u;
    slot->state = EVO_AGC_TRANSIENT_OPEN;
    return EVO_AGC_TRANSIENT_OK;
}

int evo_agc_transient_ring_alloc(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                 size_t bytes, size_t alignment,
                                 evo_agc_transient_slice_t *slice)
{
    if (!ring || !ring->base || !slice || slot_index >= ring->slot_count ||
        bytes == 0u || !is_power_of_two(alignment))
        return EVO_AGC_TRANSIENT_PRECONDITION;

    evo_agc_transient_slot_t *slot = &ring->slots[slot_index];
    if (slot->state != EVO_AGC_TRANSIENT_OPEN)
        return EVO_AGC_TRANSIENT_SLOT_BUSY;

    const uintptr_t slot_base = (uintptr_t)ring->base + slot->offset;
    if (slot_base > UINTPTR_MAX - slot->used ||
        slot_base + slot->used > UINTPTR_MAX - (alignment - 1u))
        return EVO_AGC_TRANSIENT_EXHAUSTED;

    const uintptr_t aligned_address =
        (slot_base + slot->used + alignment - 1u) &
        ~(uintptr_t)(alignment - 1u);
    if (aligned_address < slot_base)
        return EVO_AGC_TRANSIENT_EXHAUSTED;

    const size_t local_offset = (size_t)(aligned_address - slot_base);
    if (local_offset > slot->bytes || bytes > slot->bytes - local_offset)
        return EVO_AGC_TRANSIENT_EXHAUSTED;

    slot->used = local_offset + bytes;
    slice->cpu = (void *)aligned_address;
    slice->offset = slot->offset + local_offset;
    slice->gpu_addr = ring->gpu_base + slice->offset;
    slice->bytes = bytes;

    return EVO_AGC_TRANSIENT_OK;
}

int evo_agc_transient_ring_seal(evo_agc_transient_ring_t *ring, uint32_t slot_index,
                                uint64_t retire_token)
{
    if (!ring || !ring->base || slot_index >= ring->slot_count ||
        retire_token == 0u)
        return EVO_AGC_TRANSIENT_PRECONDITION;

    evo_agc_transient_slot_t *slot = &ring->slots[slot_index];
    if (slot->state != EVO_AGC_TRANSIENT_OPEN)
        return EVO_AGC_TRANSIENT_SLOT_BUSY;

    slot->retire_token = retire_token;
    slot->state = EVO_AGC_TRANSIENT_SEALED;
    return EVO_AGC_TRANSIENT_OK;
}

int evo_agc_transient_ring_abort(evo_agc_transient_ring_t *ring, uint32_t slot_index)
{
    if (!ring || !ring->base || slot_index >= ring->slot_count)
        return EVO_AGC_TRANSIENT_PRECONDITION;

    evo_agc_transient_slot_t *slot = &ring->slots[slot_index];
    if (slot->state != EVO_AGC_TRANSIENT_OPEN)
        return EVO_AGC_TRANSIENT_SLOT_BUSY;

    slot->used = 0u;
    slot->state = EVO_AGC_TRANSIENT_EMPTY;
    return EVO_AGC_TRANSIENT_OK;
}

size_t evo_agc_transient_ring_used(const evo_agc_transient_ring_t *ring, uint32_t slot_index)
{
    if (!ring || slot_index >= ring->slot_count)
        return 0;
    return ring->slots[slot_index].used;
}
