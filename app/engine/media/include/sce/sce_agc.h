#ifndef SCE_AGC_H
#define SCE_AGC_H

/* Clean-room declarations for Sony PS5 libSceAgc and libSceAgcDriver.
 * Suitable for both native title compilation and dynamic symbol binding. */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SceAgcRegister {
    uint32_t offset;
    uint32_t value;
} SceAgcRegister;

typedef struct SceAgcCommandBuffer {
    uint32_t *bottom;
    uint32_t *top;
    uint32_t *up;
    uint32_t *down;
    uintptr_t callback;
    void     *user_data;
    uint32_t  reserved_dwords;
    uint32_t  padding;
} SceAgcCommandBuffer;

typedef struct SceAgcSubmit {
    const uint32_t *words;
    uint32_t        count;
    uint8_t         flag;
    uint8_t         padding[3];
} SceAgcSubmit;

struct ps5_agc_linked_cx {
    SceAgcRegister spi_ps_input_cntl[32];
    SceAgcRegister vgt_shader_stages_en;
    SceAgcRegister vgt_gs_out_prim_type;
};

struct ps5_agc_linked_uc {
    SceAgcRegister ge_cntl;
    SceAgcRegister ge_user_vgpr_en;
    SceAgcRegister vgt_primitive_type;
};

struct ps5_agc_type_index {
    uint32_t key;
    uint32_t encoded_index;
};

struct ps5_agc_register_defaults {
    SceAgcRegister **table_cx;
    SceAgcRegister **table_sh;
    SceAgcRegister **table_uc;
    SceAgcRegister **table_3;
    uint64_t unknown_20;
    uint64_t unknown_28;
    struct ps5_agc_type_index *type_index_pairs;
    uint32_t count;
    uint32_t reserved_3c;
};

static inline uint32_t ps5_agc_type_index_bank(const struct ps5_agc_type_index *record) {
    return record->encoded_index & 3u;
}

static inline uint32_t ps5_agc_type_index_value(const struct ps5_agc_type_index *record) {
    return record->encoded_index >> 2;
}

/* Core AGC functions in libSceAgc.prx */
int32_t sceAgcInit(uint32_t ring_size);
int32_t sceAgcCreateShader(void **shader, void *header, void *code);
int32_t sceAgcLinkShaders(void *cx, void *uc, void *reserved,
                          void *pre_raster, void *pixel, uint32_t primitive);
void   *sceAgcGetRegisterDefaults(void);

uint32_t *sceAgcDcbSetCxRegistersIndirect(void *writer, const void *registers, uint32_t count);
uint32_t *sceAgcDcbSetUcRegistersIndirect(void *writer, const void *registers, uint32_t count);
uint32_t *sceAgcDcbSetShRegistersIndirect(void *writer, const void *registers, uint32_t count);
uint32_t *sceAgcCbSetShRegisterRangeDirect(void *writer, uint32_t compact_offset, const uint32_t *values, uint32_t count);

uint32_t *sceAgcDcbDrawIndexAuto(void *writer, uint32_t vertex_count, uint64_t modifier);
uint32_t *sceAgcDcbSetIndexSize(void *writer, uint8_t size, uint8_t pad);
uint32_t *sceAgcDcbSetIndexBuffer(void *writer, const void *gpu_indices);
uint32_t *sceAgcDcbSetIndexCount(void *writer, uint32_t count);
uint32_t *sceAgcDcbDrawIndex(void *writer, uint32_t index_count, const void *gpu_indices, uint64_t modifier);

uint32_t *sceAgcDcbSetFlip(void *writer, uint32_t video_handle, int32_t buffer_index, uint32_t flip_mode, int64_t flip_arg);
uint32_t *sceAgcDcbDmaData(void *writer, uint32_t arg2, uint32_t dst_select, uint32_t arg4,
                           uint64_t destination, uint32_t src_select, uint32_t arg7,
                           uint64_t source_or_immediate, uint32_t byte_count,
                           uint32_t raw_wait, uint32_t disable_write_confirm, uint32_t cp_sync);
uint32_t *sceAgcDcbAcquireMem(void *writer, uint8_t engine, uint32_t cb_db_op, uint32_t gcr_control,
                              const volatile void *base, uint64_t size_bytes, uint32_t poll_cycles);
uint32_t *sceAgcCbReleaseMem(void *writer, uint8_t engine, int16_t event_type, uint64_t event_data,
                             int8_t data_sel, void *address, uint32_t int_sel, uint64_t value,
                             uint16_t gcr_cntl, uint16_t reserved, int8_t sync_scope, int32_t poll_cycles);
int32_t   sceAgcSuspendPoint(void);

/* Driver functions in libSceAgcDriver.prx */
int32_t  sceAgcDriverSubmitDcb(void *description);
uint32_t sceAgcDriverGetWaitRenderingPacketSizeInDwords(void);
uint32_t sceAgcDriverWaitUntilSafeForRendering(uint32_t **command, uint32_t packet_size, uint32_t reserved,
                                              uint32_t video_handle, int32_t buffer_index);

#ifdef __cplusplus
}
#endif

#endif /* SCE_AGC_H */
