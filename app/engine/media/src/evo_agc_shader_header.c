#include "evo_agc_shader_header.h"

#include <string.h>

/* Load-bearing ABI checks, ported verbatim from ps5-xash3d-halflife's
 * src/ps5_shader_header.c. sceAgcCreateShader reads these structures by offset;
 * a layout drift here is a silent wrong-pointer, not a compile error. */
_Static_assert(sizeof(evo_agc_shader_user_data_t) == 0x38, "user-data ABI");
_Static_assert(sizeof(evo_agc_shader_hdr_t) == 0x60, "shader header ABI");
_Static_assert(offsetof(evo_agc_shader_hdr_t, code) == 0x10, "code placement");
_Static_assert(offsetof(evo_agc_shader_hdr_t, type) == 0x5a, "type placement");
_Static_assert(sizeof(evo_agc_shader_specials_t) == 0x30, "specials ABI");
_Static_assert(offsetof(evo_agc_shader_arena_t, user_data) == 0x60,
               "user-data placement");
_Static_assert(offsetof(evo_agc_shader_arena_t, specials) == 0x98,
               "specials placement");
_Static_assert(offsetof(evo_agc_shader_arena_t, cx) == 0xc8, "CX placement");
_Static_assert(offsetof(evo_agc_shader_arena_t, sh) == 0x118, "SH placement");
_Static_assert(sizeof(evo_agc_shader_arena_t) == 0x148, "arena ABI");

/* The header's pointer fields are stored SELF-RELATIVE: each holds the distance
 * from its own address to its target, so the whole arena can be memcpy'd into
 * GPU memory and stay internally consistent. sceAgcCreateShader resolves them
 * to absolute pointers in place - which is why evo_agc_runtime.c can read
 * cx_registers/sh_registers straight off the object afterwards. */
static void *self_relative(void *field, void *target)
{
    return (void *)((uintptr_t)target - (uintptr_t)field);
}

int evo_agc_shader_header_build(evo_agc_shader_arena_t *arena, uint8_t type,
                                uint32_t shader_bytes,
                                const evo_agc_shader_metadata_t *metadata)
{
    if (!arena || !metadata || shader_bytes < EVO_AGC_SHADER_FOOTER_BYTES ||
        (shader_bytes & 3u) ||
        (type != EVO_AGC_SHADER_PRE_RASTER && type != EVO_AGC_SHADER_PIXEL) ||
        metadata->pre_raster_cx_count != 10u ||
        metadata->pixel_cx_count != 9u ||
        !metadata->pre_raster_cx || !metadata->pixel_cx)
        return -1;

    memset(arena, 0, sizeof(*arena));
    arena->header.file_header = UINT32_C(0x34333231);   /* '1234' */
    arena->header.version = UINT32_C(0x18);
    arena->header.user_data =
        self_relative(&arena->header.user_data, &arena->user_data);
    arena->header.cx_registers =
        self_relative(&arena->header.cx_registers, arena->cx);
    arena->header.sh_registers =
        self_relative(&arena->header.sh_registers, arena->sh);
    arena->header.specials =
        self_relative(&arena->header.specials, &arena->specials);
    arena->header.header_size = sizeof(*arena);
    arena->header.shader_size = shader_bytes;
    arena->header.target = 5u;
    arena->header.special_sizes_bytes = sizeof(arena->specials);
    arena->header.type = type;
    arena->header.num_sh_registers = 6u;

    /* The specials block is the pre-raster/NGG linkage sceAgcLinkShaders reads;
     * a pixel shader zeroes the values but must still carry the offsets. */
    arena->specials.ge_cntl = (evo_agc_reg_t){
        0x25b, type == EVO_AGC_SHADER_PRE_RASTER ? metadata->ge_cntl : 0u};
    arena->specials.shader_stages_en = (evo_agc_reg_t){
        0x2d5, type == EVO_AGC_SHADER_PRE_RASTER ? metadata->shader_stages_en : 0u};
    arena->specials.gs_out_prim_type = (evo_agc_reg_t){
        0x2ce, type == EVO_AGC_SHADER_PRE_RASTER ? metadata->gs_out_prim_type : 0u};
    arena->specials.ge_user_vgpr_en.offset = 0x25c;
    arena->specials.draw_modifier = metadata->draw_modifier;

    if (type == EVO_AGC_SHADER_PIXEL) {
        arena->header.num_cx_registers = 9u;
        memcpy(arena->cx, metadata->pixel_cx, 9u * sizeof(evo_agc_reg_t));
        /* SPI_SHADER_PGM_LO/HI_PS are left zero for sceAgcCreateShader to fill
         * with the code address; RSRC1/RSRC2 come from the PAL metadata. */
        arena->sh[0].offset = arena->sh[1].offset = 0x006;
        arena->sh[2].offset = 0x008;
        arena->sh[3].offset = 0x009;
        arena->sh[4] = (evo_agc_reg_t){0x00a, metadata->ps_rsrc1};
        arena->sh[5] = (evo_agc_reg_t){0x00b, metadata->ps_rsrc2};
    } else {
        arena->header.num_cx_registers = 10u;
        memcpy(arena->cx, metadata->pre_raster_cx, 10u * sizeof(evo_agc_reg_t));
        arena->sh[0].offset = arena->sh[1].offset = 0x080;
        arena->sh[2] = (evo_agc_reg_t){0x08a, metadata->gs_rsrc1};
        arena->sh[3] = (evo_agc_reg_t){0x08b, metadata->gs_rsrc2};
        arena->sh[4].offset = 0x0c8;
        arena->sh[5].offset = 0x0c9;
    }
    return 0;
}

void evo_agc_shader_write_code(uint8_t *dst, const uint8_t *isa,
                               uint32_t isa_bytes)
{
    if (!dst || !isa)
        return;
    memcpy(dst, isa, isa_bytes);
    memset(dst + isa_bytes, 0, EVO_AGC_SHADER_FOOTER_BYTES);
    /* sceAgcCreateShader looks for this marker, which only the PSSL toolchain
     * emits; amdllpc's raw ISA has no trailer, so synthesise it the way the
     * reference implementation does. */
    memcpy(dst + isa_bytes, "barefoot", 8);
}
