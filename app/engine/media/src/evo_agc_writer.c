#include "evo_agc_writer.h"

#include <string.h>
#include <limits.h>

/* GFX10.3 / RDNA2 format codes */
enum {
    GFX10_FORMAT_8_UNORM = 1,
    GFX10_FORMAT_16_UNORM = 7,
    GFX10_FORMAT_8_8_UNORM = 14,
    GFX10_FORMAT_16_16_UNORM = 23,
    GFX10_FORMAT_8_8_8_8_UNORM = 56,
    GFX10_FORMAT_16_16_16_16_FLOAT = 71,

    SQ_SEL_0 = 0,
    SQ_SEL_1 = 1,
    SQ_SEL_X = 4,
    SQ_SEL_Y = 5,
    SQ_SEL_Z = 6,
    SQ_SEL_W = 7,

    SQ_RSRC_IMG_2D = 9,

    /* The swizzle mode every colour target here is rendered with
     * (CB_COLOR0_ATTRIB3.COLOR_SW_MODE = 27, attrib=0x4dc6c000 in evo.log). */
    SQ_SW_64KB_R_X = 27,
};

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static uint32_t g_out_of_space_calls;

static uint8_t cb_out_of_space(SceAgcCommandBuffer *writer, uint32_t requested, void *user_data)
{
    (void)writer;
    (void)requested;
    (void)user_data;
    /* There is nowhere to get more room from, so 0 it stays and sceAgc drops
     * the packet. Counting it is the only thing that makes a truncated frame
     * visible from a log - see evo_agc_writer_out_of_space_count(). */
    g_out_of_space_calls++;
    return 0;
}

uint32_t evo_agc_writer_out_of_space_count(void)
{
    return g_out_of_space_calls;
}

void evo_agc_writer_reset_out_of_space_count(void)
{
    g_out_of_space_calls = 0;
}

void evo_agc_writer_init(SceAgcCommandBuffer *cb, uint32_t *buffer, uint32_t capacity_dwords)
{
    if (!cb || !buffer || capacity_dwords == 0u)
        return;
    cb->bottom = buffer;
    cb->top = buffer + capacity_dwords;
    cb->up = buffer;
    cb->down = cb->top;
    cb->callback = (uintptr_t)cb_out_of_space;
    cb->user_data = NULL;
    cb->reserved_dwords = 0u;
    cb->padding = 0u;
}

uint32_t evo_agc_writer_dwords_written(const SceAgcCommandBuffer *cb)
{
    if (!cb || !cb->bottom || cb->up < cb->bottom)
        return 0u;
    return (uint32_t)(cb->up - cb->bottom);
}

/* -------------------------------------------------------------------------
 * Hardware Resource Descriptor Builders
 * ------------------------------------------------------------------------- */

int evo_agc_build_vsharp(uint32_t out[EVO_AGC_VSHARP_DWORDS], uint64_t gpu_address,
                         uint32_t stride, uint32_t records)
{
    if (!out || gpu_address == 0u || stride == 0u || stride > 0x3fffu || records == 0u)
        return -1;
    out[0] = (uint32_t)gpu_address;
    out[1] = (uint32_t)((gpu_address >> 32) & 0xffffu) | (stride << 16);
    out[2] = records;
    out[3] = UINT32_C(0x11014fac); /* standard buffer format */
    return 0;
}

int evo_agc_build_constant_vsharp(uint32_t out[EVO_AGC_VSHARP_DWORDS], uint64_t gpu_address,
                                  uint32_t bytes)
{
    if (!out || gpu_address == 0u || bytes == 0u)
        return -1;
    out[0] = (uint32_t)gpu_address;
    out[1] = (uint32_t)((gpu_address >> 32) & 0xffffu);
    out[2] = (bytes + 3u) & ~UINT32_C(3);
    out[3] = UINT32_C(0x31016fac);
    return 0;
}

static int build_tsharp_2d_internal(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                                    uint32_t width, uint32_t height, uint32_t pitch_bytes,
                                    uint32_t format, uint32_t bpp,
                                    uint32_t sel_x, uint32_t sel_y, uint32_t sel_z, uint32_t sel_w)
{
    /*
     * The descriptor stores the base as gpu_address >> 8 and the pitch in
     * texels, so a base that is not 256-byte aligned silently loses its low
     * bits and the GPU samples from up to 255 bytes before the texture. That
     * is not a crash or a fault - it is glyph atlases rendering as garbage
     * while any texture that happened to land aligned looks perfect. Reject it
     * here rather than encode something that cannot be right; ps5-opengl's
     * builder applies the same check.
     */
    if (!out || gpu_address == 0u || (gpu_address & 255u) != 0u ||
        width == 0u || height == 0u ||
        width > 16384u || height > 16384u || pitch_bytes < width * bpp ||
        (pitch_bytes % bpp) != 0u)
        return -1;

    const uint32_t width_minus_one = width - 1u;
    const uint32_t pitch_minus_one = (pitch_bytes / bpp) - 1u;

    memset(out, 0, EVO_AGC_TSHARP_DWORDS * sizeof(uint32_t));
    out[0] = (uint32_t)(gpu_address >> 8);
    out[1] = (uint32_t)(gpu_address >> 40) | (format << 20) | ((width_minus_one & 3u) << 30);
    out[2] = ((width_minus_one >> 2) & 0xfffu) | ((height - 1u) << 14);
    out[3] = sel_x | (sel_y << 3) | (sel_z << 6) | (sel_w << 9) | ((uint32_t)SQ_RSRC_IMG_2D << 28);
    if ((pitch_bytes / bpp) != width) {
        out[4] = (pitch_minus_one & 0x1fffu) | (((pitch_minus_one >> 13) & 1u) << 13);
    }
    out[5] = (7u << 20);
    return 0;
}

int evo_agc_build_tsharp_rgba8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                               uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_8_8_8_8_UNORM, 4u,
                                    SQ_SEL_X, SQ_SEL_Y, SQ_SEL_Z, SQ_SEL_W);
}

int evo_agc_build_tsharp_bgra8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                               uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    /* Same 8_8_8_8 format, swizzle swap (Z,Y,X,W): memory order B,G,R,A -> the
     * shader's R,G,B,A. See the header's comment for when this is needed. */
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_8_8_8_8_UNORM, 4u,
                                    SQ_SEL_Z, SQ_SEL_Y, SQ_SEL_X, SQ_SEL_W);
}

int evo_agc_build_tsharp_r8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                            uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_8_UNORM, 1u,
                                    SQ_SEL_X, SQ_SEL_0, SQ_SEL_0, SQ_SEL_1);
}

int evo_agc_build_tsharp_rg8(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                             uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_8_8_UNORM, 2u,
                                    SQ_SEL_X, SQ_SEL_Y, SQ_SEL_0, SQ_SEL_1);
}

int evo_agc_build_tsharp_r16(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                             uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_16_UNORM, 2u,
                                    SQ_SEL_X, SQ_SEL_0, SQ_SEL_0, SQ_SEL_1);
}

int evo_agc_build_tsharp_rg16(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                              uint32_t width, uint32_t height, uint32_t pitch_bytes)
{
    return build_tsharp_2d_internal(out, gpu_address, width, height, pitch_bytes,
                                    GFX10_FORMAT_16_16_UNORM, 4u,
                                    SQ_SEL_X, SQ_SEL_Y, SQ_SEL_0, SQ_SEL_1);
}

int evo_agc_build_tsharp_render_target(uint32_t out[EVO_AGC_TSHARP_DWORDS], uint64_t gpu_address,
                                       uint32_t width, uint32_t height, int fp16)
{
    /* Row pitch is implied by the tiling, so pass the unpadded row size and
     * leave the linear pitch field empty. */
    const uint32_t bpp = fp16 ? 8u : 4u;
    if ((gpu_address & 0xffffu) != 0u)
        return -1;
    int rc = build_tsharp_2d_internal(out, gpu_address, width, height, width * bpp,
                                      fp16 ? GFX10_FORMAT_16_16_16_16_FLOAT
                                           : GFX10_FORMAT_8_8_8_8_UNORM, bpp,
                                      SQ_SEL_X, SQ_SEL_Y, SQ_SEL_Z, SQ_SEL_W);
    if (rc == 0)
        out[3] |= (uint32_t)SQ_SW_64KB_R_X << 20;   /* SQ_IMG_RSRC_WORD3.SW_MODE */
    return rc;
}

int evo_agc_build_ssharp(uint32_t out[EVO_AGC_SSHARP_DWORDS], int clamp_to_edge, int bilinear)
{
    if (!out)
        return -1;
    memset(out, 0, EVO_AGC_SSHARP_DWORDS * sizeof(uint32_t));
    const uint32_t addr_mode = clamp_to_edge ? 2u : 0u; /* 2 = CLAMP_LAST_TEXEL, 0 = REPEAT */
    out[0] = addr_mode | (addr_mode << 3) | (addr_mode << 6);
    out[1] = 0x00fff000u; /* maxLod = 4095 */
    if (bilinear) {
        /* kNativeAgcBilinearSamplerWord = (1u<<27) | (1u<<24) | (1u<<22) | (1u<<20) = 0x09500000u */
        out[2] = 0x09500000u;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * DCB Command Packet Emitters
 * ------------------------------------------------------------------------- */

int evo_agc_writer_set_target(SceAgcCommandBuffer *cb, const SceAgcRegister *mrt, uint32_t count)
{
    if (!cb || !mrt || count == 0u)
        return -1;
    sceAgcDcbSetCxRegistersIndirect(cb, mrt, count);
    return 0;
}

int evo_agc_writer_set_viewport(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs,
                                float x, float y, float width, float height)
{
    if (!cb || !gpu_regs || width <= 0.0f || height <= 0.0f)
        return -1;

    gpu_regs[0]  = (SceAgcRegister){0x10f, float_bits(width * 0.5f)};
    gpu_regs[1]  = (SceAgcRegister){0x110, float_bits(x + width * 0.5f)};
    gpu_regs[2]  = (SceAgcRegister){0x111, float_bits(height * -0.5f)};
    gpu_regs[3]  = (SceAgcRegister){0x112, float_bits(y + height * 0.5f)};
    gpu_regs[4]  = (SceAgcRegister){0x113, float_bits(1.0f)};
    gpu_regs[5]  = (SceAgcRegister){0x114, 0u};
    gpu_regs[6]  = (SceAgcRegister){0x0b4, 0u};
    gpu_regs[7]  = (SceAgcRegister){0x0b5, float_bits(1.0f)};
    gpu_regs[8]  = (SceAgcRegister){0x2fa, float_bits(1.0f)};
    gpu_regs[9]  = (SceAgcRegister){0x2fb, float_bits(1.0f)};
    gpu_regs[10] = (SceAgcRegister){0x2fc, float_bits(1.0f)};
    gpu_regs[11] = (SceAgcRegister){0x2fd, float_bits(1.0f)};

    sceAgcDcbSetCxRegistersIndirect(cb, gpu_regs, 12);
    return 0;
}

int evo_agc_writer_set_scissor(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs,
                               uint32_t left, uint32_t top,
                               uint32_t right, uint32_t bottom)
{
    if (!cb || !gpu_regs)
        return -1;

    gpu_regs[0] = (SceAgcRegister){0x090, (left & 0x3fffu) | ((top & 0x3fffu) << 16u) | UINT32_C(0x80000000)};
    gpu_regs[1] = (SceAgcRegister){0x091, (right & 0x3fffu) | ((bottom & 0x3fffu) << 16u)};

    sceAgcDcbSetCxRegistersIndirect(cb, gpu_regs, 2);
    return 0;
}

int evo_agc_writer_set_blend(SceAgcCommandBuffer *cb, SceAgcRegister *gpu_regs, int blend_mode)
{
    if (!cb || !gpu_regs)
        return -1;

    uint32_t blend_ctrl = 0u;
    switch (blend_mode) {
    case EVO_AGC_BLEND_ALPHA:
        blend_ctrl = UINT32_C(0x40000504); /* SRC_ALPHA, ONE_MINUS_SRC_ALPHA */
        break;
    case EVO_AGC_BLEND_PREMULTIPLIED:
        blend_ctrl = UINT32_C(0x40000501); /* ONE, ONE_MINUS_SRC_ALPHA */
        break;
    case EVO_AGC_BLEND_ADDITIVE:
        blend_ctrl = UINT32_C(0x40000101); /* ONE, ONE */
        break;
    case EVO_AGC_BLEND_NONE:
    default:
        blend_ctrl = 0u;
        break;
    }

    gpu_regs[0] = (SceAgcRegister){0x01e0, blend_ctrl};
    gpu_regs[1] = (SceAgcRegister){0x008e, UINT32_C(0x0000000f)}; /* Target mask RGBA */

    sceAgcDcbSetCxRegistersIndirect(cb, gpu_regs, 2);
    return 0;
}

int evo_agc_writer_set_cx_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count)
{
    if (!cb || !registers || count == 0u)
        return -1;
    sceAgcDcbSetCxRegistersIndirect(cb, registers, count);
    return 0;
}

int evo_agc_writer_set_sh_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count)
{
    if (!cb || !registers || count == 0u)
        return -1;
    sceAgcDcbSetShRegistersIndirect(cb, registers, count);
    return 0;
}

int evo_agc_writer_set_uc_indirect(SceAgcCommandBuffer *cb, const SceAgcRegister *registers, uint32_t count)
{
    if (!cb || !registers || count == 0u)
        return -1;
    sceAgcDcbSetUcRegistersIndirect(cb, registers, count);
    return 0;
}

int evo_agc_writer_set_user_data_gs(SceAgcCommandBuffer *cb, const uint32_t *values, uint32_t count)
{
    if (!cb || !values || count == 0u)
        return -1;
    sceAgcCbSetShRegisterRangeDirect(cb, 0x8c, values, count);
    return 0;
}

int evo_agc_writer_set_user_data_ps(SceAgcCommandBuffer *cb, const uint32_t *values, uint32_t count)
{
    if (!cb || !values || count == 0u)
        return -1;
    sceAgcCbSetShRegisterRangeDirect(cb, 0x0c, values, count);
    return 0;
}

int evo_agc_writer_draw_index_modifier(SceAgcCommandBuffer *cb, uint32_t index_count, const uint16_t *gpu_indices, uint64_t modifier)
{
    if (!cb || !gpu_indices || index_count == 0u)
        return -1;
    sceAgcDcbSetIndexSize(cb, 0, 0); /* 16-bit indices */
    sceAgcDcbSetIndexBuffer(cb, (void *)gpu_indices);
    sceAgcDcbSetIndexCount(cb, index_count);
    sceAgcDcbDrawIndex(cb, index_count, (void *)gpu_indices, modifier);
    return 0;
}

int evo_agc_writer_draw_index(SceAgcCommandBuffer *cb, uint32_t index_count, const uint16_t *gpu_indices)
{
    return evo_agc_writer_draw_index_modifier(cb, index_count, gpu_indices, 0);
}

int evo_agc_writer_draw_auto(SceAgcCommandBuffer *cb, uint32_t vertex_count)
{
    if (!cb || vertex_count == 0u)
        return -1;
    sceAgcDcbDrawIndexAuto(cb, vertex_count, 2);
    return 0;
}

int evo_agc_writer_set_flip(SceAgcCommandBuffer *cb, uint32_t video_handle, int32_t buffer_index,
                            uint32_t flip_mode, int64_t flip_arg)
{
    if (!cb || buffer_index < 0)
        return -1;
    sceAgcDcbSetFlip(cb, video_handle, buffer_index, flip_mode, flip_arg);
    return 0;
}

/* Both of these hand the packet to libSceAgc rather than memcpy'ing a PM4 blob:
 * the GCR-control encoding inside RELEASE_MEM is what actually decides whether
 * the frame's pixels reach DRAM, and hand-assembling it is how this path ended
 * up emitting a fence with no cache writeback. Argument positions are taken
 * verbatim from ps5-opengl's native runtime; see evo_agc_writer.h. */
int evo_agc_writer_flush_color_target(SceAgcCommandBuffer *cb)
{
    if (!cb)
        return -1;
    /* event 45 = FLUSH_AND_INV_CB_DATA_TS, GCR control 12, no memory write. */
    return sceAgcCbReleaseMem(cb, 45, 12, 1, 0, NULL, 0, 0, 0, 1, 0, 0) ? 0 : -1;
}

int evo_agc_writer_release_mem(SceAgcCommandBuffer *cb, uint64_t fence_address,
                               uint32_t marker)
{
    if (!cb || !fence_address || (fence_address & 7u) != 0u || !marker)
        return -1;
    /* event 40 = CACHE_FLUSH_AND_INV_TS, GCR control 0x30c (GLV/GL1/GL2 inv +
     * GL2 writeback), int_sel 1 = write `marker` to fence_address at EOP. */
    return sceAgcCbReleaseMem(cb, 40, 0x30c, 0, 0,
                              (void *)(uintptr_t)fence_address, 1,
                              marker, 0, 0, 0, 0) ? 0 : -1;
}
