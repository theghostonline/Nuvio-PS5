#ifndef EVO_AGC_WRITER_H
#define EVO_AGC_WRITER_H

#include "sce/sce_agc.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    EVO_AGC_VSHARP_DWORDS = 4,
    EVO_AGC_TSHARP_DWORDS = 8,
    EVO_AGC_SSHARP_DWORDS = 4,
    EVO_AGC_COMBINED_DESCRIPTOR_DWORDS = 12,
};

enum evo_agc_blend_mode {
    EVO_AGC_BLEND_NONE = 0,
    EVO_AGC_BLEND_ALPHA = 1,
    EVO_AGC_BLEND_PREMULTIPLIED = 2,
    EVO_AGC_BLEND_ADDITIVE = 3,
};

/* GFX10.3 hardware descriptor builders */
int evo_agc_build_vsharp(uint32_t out[EVO_AGC_VSHARP_DWORDS], uint64_t gpu_address,
                         uint32_t stride, uint32_t records);

int evo_agc_build_constant_vsharp(uint32_t out[EVO_AGC_VSHARP_DWORDS], uint64_t gpu_address,
                                  uint32_t bytes);

int evo_agc_build_tsharp_rgba8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                               uint32_t width, uint32_t height, uint32_t pitch_bytes);

int evo_agc_build_tsharp_r8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                            uint32_t width, uint32_t height, uint32_t pitch_bytes);

int evo_agc_build_tsharp_rg8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                             uint32_t width, uint32_t height, uint32_t pitch_bytes);

int evo_agc_build_tsharp_r16(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                             uint32_t width, uint32_t height, uint32_t pitch_bytes);

int evo_agc_build_tsharp_rg16(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                               uint32_t width, uint32_t height, uint32_t pitch_bytes);

/* Sample something the GPU RENDERED through setup_color_target(): those
 * colour targets are 64KB_R_X tiled (CB_COLOR0_ATTRIB3.COLOR_SW_MODE = 27), so
 * a linear T# reads them as scrambled blocks - which is what #103's first
 * hardware run showed. `width`/`height` must be the colour target's own, and
 * the base 64 KB aligned. RGBA8 (standard swap) or RGBA16F. */
int evo_agc_build_tsharp_render_target(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                                       uint32_t width, uint32_t height, int fp16);

/* BGRA8: FORMAT_8_8_8_8 but with swizzle (Z,Y,X,W), so a texture stored in
 * memory as B,G,R,A bytes (the scanout backbuffer, written BGRA via
 * COMP_SWAP=ALT, and the 0xAABBGGRR OSD buffer) is sampled as correct R,G,B,A.
 * Sampling those surfaces with the plain rgba8 builder silently swaps red and
 * blue - visible the moment the UI composites from a BGRA source (backdrop
 * blur sampling the scanout). */
int evo_agc_build_tsharp_bgra8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                               uint32_t width, uint32_t height, uint32_t pitch_bytes);

int evo_agc_build_ssharp(uint32_t out[EVO_AGC_SSHARP_DWORDS], int clamp_to_edge, int bilinear);

/* DCB packet writers.
 *
 * GFX10's *RegistersIndirect packets (set_target / set_viewport / set_scissor
 * / set_blend / set_{cx,sh,uc}_indirect below) tell the GPU Command Processor
 * to DMA-read the register words from the given pointer AT SUBMIT TIME, not to
 * copy them into the DCB immediately - so every pointer passed to one of these
 * MUST be GPU-mapped (PROT_GPU_RW), cache-flushed memory that stays valid until
 * the GPU actually executes the packet, never a caller's stack array or a
 * plain CPU-heap/.bss struct field. set_viewport/set_scissor/set_blend take an
 * explicit `gpu_regs` destination for exactly this reason - the caller (which
 * knows the frame's GPU-visible transient allocation) supplies it. set_target
 * and set_{cx,sh,uc}_indirect take data that is computed once and reused every
 * frame (color targets, compiled pipeline registers), so their fix is at the
 * storage site instead: evo_agc_runtime.c allocates that storage from
 * direct_mem_base (also GPU-mapped) rather than embedding it in g_agc_dev. */
void evo_agc_writer_init(SceAgcCommandBuffer *cb, uint32_t *buffer, uint32_t capacity_dwords);

uint32_t evo_agc_writer_dwords_written(const SceAgcCommandBuffer *cb);

/*
 * How many times the command buffer ran out of room since the last reset.
 *
 * Non-zero means a frame was TRUNCATED: the out-of-space callback has no way to
 * hand sceAgc more room, so whatever it was about to write is dropped and the
 * frame goes to the panel with a piece of the UI missing. That used to be
 * completely silent, which is what made a 184-row provider level so hard to
 * diagnose - it is reported in the `agc health` line as dcb_full.
 */
uint32_t evo_agc_writer_out_of_space_count(void);
void     evo_agc_writer_reset_out_of_space_count(void);

int evo_agc_writer_set_target(SceAgcCommandBuffer *cb, const SceAgcRegister *mrt, uint32_t count);

int evo_agc_writer_set_viewport(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs,
                                float x, float y, float width, float height);

int evo_agc_writer_set_scissor(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs,
                               uint32_t left, uint32_t top,
                               uint32_t right, uint32_t bottom);

int evo_agc_writer_set_blend(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs, int blend_mode);

int evo_agc_writer_set_cx_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count);
int evo_agc_writer_set_sh_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count);
int evo_agc_writer_set_uc_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count);

int evo_agc_writer_set_user_data_gs(SceAgcCommandBuffer *cb, const uint32_t *values, uint32_t count);
int evo_agc_writer_set_user_data_ps(SceAgcCommandBuffer *cb, const uint32_t *values, uint32_t count);

int evo_agc_writer_draw_index(SceAgcCommandBuffer *cb, uint32_t index_count, const uint16_t *gpu_indices);
int evo_agc_writer_draw_index_modifier(SceAgcCommandBuffer *cb, uint32_t index_count, const uint16_t *gpu_indices, uint64_t modifier);
int evo_agc_writer_draw_auto(SceAgcCommandBuffer *cb, uint32_t vertex_count);

int evo_agc_writer_set_flip(SceAgcCommandBuffer *cb, uint32_t video_handle, int32_t buffer_index,
                            uint32_t flip_mode, int64_t flip_arg);

/* End-of-frame cache protocol. BOTH are mandatory, in this order, around the
 * SetFlip - this is the sequence ps5-opengl's hardware-verified native AGC
 * runtime emits (src/platform/ps5_agc_native_runtime.c, runtime_gpu_present_*
 * and the AGC_RUNTIME_PACKAGES submit path) and the piece this bare-metal
 * rewrite was missing entirely:
 *
 *   flush_color_target()  event 45 = FLUSH_AND_INV_CB_DATA_TS, GCR control 12.
 *       Pushes the colour-block caches out to memory. Without it the pixels a
 *       draw produced are sitting in the CB cache; the GPU reports no fault,
 *       the submit returns 0, and the display controller - which is not a
 *       coherent client and reads DRAM directly - scans out the untouched
 *       buffer. That is exactly "GPU runs, submits succeed, nothing is ever
 *       drawn", and why a CPU-written test pattern (CPU -> clflush -> DRAM)
 *       reached the panel while no GPU draw ever did.
 *
 *   release_mem()         event 40 = CACHE_FLUSH_AND_INV_TS, GCR control 0x30c
 *       = GLV_INV | GL1_INV | GL2_INV | GL2_WB. The GL2_WB is the other half of
 *       the same problem: it writes the GPU's L2 back to DRAM. It also writes
 *       `marker` to fence_address at end-of-pipe, which is the frame's
 *       retirement signal. The previous hand-rolled PM4 blob here carried GCR
 *       control 0xc0 (no L2 writeback) and no CB flush at all.
 *
 * fence_address must be 8-byte aligned and GPU-mapped; the GPU writes the
 * 32-bit `marker` there. `marker` must be non-zero and must differ from the
 * value already in the slot, or the waiter cannot tell old from new. */
int evo_agc_writer_flush_color_target(SceAgcCommandBuffer *cb);
int evo_agc_writer_release_mem(SceAgcCommandBuffer *cb, uint64_t fence_address,
                               uint32_t marker);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AGC_WRITER_H */
