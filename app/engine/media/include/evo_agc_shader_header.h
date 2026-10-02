#ifndef EVO_AGC_SHADER_HEADER_H
#define EVO_AGC_SHADER_HEADER_H

/*
 * Builds the AGC shader header that sceAgcCreateShader consumes, from the
 * register tables tools/build_agc_pipes.py derives out of amdllpc's PAL
 * metadata.
 *
 * This replaces the opengnm-psbc "package" path (an ELF with .shader_header /
 * .shader_text sections, parsed by extract_shader_sections()). That path was
 * driven by a hand-written wrapper that reimplemented ps5-opengl's Gallium
 * caller without its overrides and shipped two silent, hardware-only bugs - a
 * wrong VGT_ESGS_RING_ITEMSIZE that wedged the GPU on the first real draw, and
 * a vertex stage compiled with no descriptor binding for its own uniform block,
 * which drew exactly zero fragments while faulting nothing.
 *
 * Ported from ps5-xash3d-halflife (src/ps5_shader_header.{h,c},
 * include/ps5_shader.h, src/ps5_shader_pipeline_slot.h), which drives a full
 * 3D game on this firmware through this same construction.
 *
 * Every struct here is a hardware/firmware ABI; the static asserts in the .c
 * are load-bearing, not documentation.
 */

#include "sce/sce_agc.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef SceAgcRegister evo_agc_reg_t;

enum {
    EVO_AGC_SHADER_PIXEL      = 1,
    EVO_AGC_SHADER_PRE_RASTER = 2,
    EVO_AGC_SHADER_MAX_CX     = 10,
    /* The ISA blob is padded by this much and carries the "barefoot" marker the
     * PSSL toolchain emits; sceAgcCreateShader rejects code without it. */
    EVO_AGC_SHADER_FOOTER_BYTES = 0x30,
};

/* What build_agc_pipes.py emits per pipeline. */
typedef struct evo_agc_shader_metadata {
    const uint8_t *gs_isa;
    uint32_t       gs_isa_bytes;
    const uint8_t *ps_isa;
    uint32_t       ps_isa_bytes;
    uint32_t       gs_rsrc1, gs_rsrc2;
    uint32_t       ps_rsrc1, ps_rsrc2;
    uint32_t       ge_cntl, shader_stages_en, gs_out_prim_type;
    uint64_t       draw_modifier;
    const evo_agc_reg_t *pre_raster_cx;
    uint32_t             pre_raster_cx_count;
    const evo_agc_reg_t *pixel_cx;
    uint32_t             pixel_cx_count;
    /* SPI_PS_INPUT_CNTL_0..N. Kept out of pixel_cx because the shader-header
     * arena's CX array is a fixed console ABI of exactly 9 pixel registers;
     * these ride with the pipeline's bind-time context registers instead. */
    const evo_agc_reg_t *ps_input_cntl;
    uint32_t             ps_input_cntl_count;
    /* User-SGPR layout, derived from the PAL metadata's .user_data_reg_map
     * rather than assumed. These say which dword of the block written at SH
     * 0x8c (vertex) / 0x0c (pixel) holds which [ResourceMapping] pointer.
     * The psbc path hardcoded these to match whatever that compiler chose;
     * getting them wrong is silent - the shader reads a resource through an
     * unset pointer, so it draws nothing and faults nothing. */
    uint32_t vs_user_sgpr_count;
    uint32_t ps_user_sgpr_count;
    int32_t  vs_const_table_dword;
    int32_t  vs_vertex_table_dword;
    /* The const-buffer and texture tables can both be fragment-stage
     * (ui_backdrop_blur keeps its BlurConstants in the fragment shader), so
     * keep a PS const-table slot alongside the PS texture-table slot. */
    int32_t  ps_const_table_dword;
    int32_t  ps_texture_table_dword;
} evo_agc_shader_metadata_t;

typedef struct evo_agc_shader_user_data {
    uint16_t *direct_resource_offsets;
    void     *sharp_resource_offsets[4];
    uint16_t  eud_size_dw;
    uint16_t  srt_size_dw;
    uint16_t  direct_resource_count;
    uint16_t  sharp_resource_count[4];
} evo_agc_shader_user_data_t;

typedef struct evo_agc_shader_hdr {
    uint32_t        file_header;      /* '1234' = 0x34333231 */
    uint32_t        version;          /* 0x18 */
    evo_agc_shader_user_data_t *user_data;
    const void     *code;
    evo_agc_reg_t  *cx_registers;
    evo_agc_reg_t  *sh_registers;
    void           *specials;
    void           *input_semantics;
    void           *output_semantics;
    uint32_t        header_size;
    uint32_t        shader_size;
    uint32_t        embedded_constant_dqw;
    uint32_t        target;           /* 5 */
    uint32_t        num_input_semantics;
    uint16_t        scratch_dw_per_thread;
    uint16_t        num_output_semantics;
    uint16_t        special_sizes_bytes;
    uint8_t         type;
    uint8_t         num_cx_registers;
    uint8_t         num_sh_registers;
    uint8_t         reserved_5d[3];
} evo_agc_shader_hdr_t;

typedef struct evo_agc_shader_specials {
    evo_agc_reg_t ge_cntl;
    evo_agc_reg_t shader_stages_en;
    uint32_t      dispatch_modifier;
    uint16_t      user_data_range_start;
    uint16_t      user_data_range_end;
    uint64_t      draw_modifier;
    evo_agc_reg_t gs_out_prim_type;
    evo_agc_reg_t ge_user_vgpr_en;
} evo_agc_shader_specials_t;

typedef struct evo_agc_shader_arena {
    evo_agc_shader_hdr_t      header;
    evo_agc_shader_user_data_t user_data;
    evo_agc_shader_specials_t specials;
    evo_agc_reg_t             cx[EVO_AGC_SHADER_MAX_CX];
    evo_agc_reg_t             sh[6];
} evo_agc_shader_arena_t;

/* Fill `arena` for one stage. `shader_bytes` is the ISA size PLUS
 * EVO_AGC_SHADER_FOOTER_BYTES; the caller copies the ISA into GPU memory and
 * writes the footer. Returns 0 on success. */
int evo_agc_shader_header_build(evo_agc_shader_arena_t *arena, uint8_t type,
                                uint32_t shader_bytes,
                                const evo_agc_shader_metadata_t *metadata);

/* Copy `isa` into `dst` and append the "barefoot" footer. `dst` must have room
 * for isa_bytes + EVO_AGC_SHADER_FOOTER_BYTES. */
void evo_agc_shader_write_code(uint8_t *dst, const uint8_t *isa,
                               uint32_t isa_bytes);

#ifdef __cplusplus
}
#endif

#endif /* EVO_AGC_SHADER_HEADER_H */
