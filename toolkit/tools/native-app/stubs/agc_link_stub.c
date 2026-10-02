/*
 * ps5-native-app-boilerplate / ProsperoLight - AGC link stub.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host-link declarations for system libSceAgc; never included in the app.
 */

#include <stdint.h>

int32_t sceAgcInit(void *state, uint32_t size) { (void)state; (void)size; return -1; }
int32_t sceAgcCreateShader(void **shader, void *header, void *code) { (void)shader; (void)header; (void)code; return -1; }
int32_t sceAgcLinkShaders(void *cx, void *uc, void *reserved, void *vertex, void *pixel, uint32_t primitive) { (void)cx; (void)uc; (void)reserved; (void)vertex; (void)pixel; (void)primitive; return -1; }
void *sceAgcGetRegisterDefaults(void) { return 0; }
uint32_t *sceAgcDcbSetCxRegistersIndirect(void *command, const void *registers, uint32_t count) { (void)command; (void)registers; (void)count; return 0; }
uint32_t *sceAgcDcbSetShRegistersIndirect(void *command, const void *registers, uint32_t count) { (void)command; (void)registers; (void)count; return 0; }
uint32_t *sceAgcDcbSetUcRegistersIndirect(void *command, const void *registers, uint32_t count) { (void)command; (void)registers; (void)count; return 0; }
uint32_t *sceAgcCbSetShRegisterRangeDirect(void *command, uint32_t offset, const uint32_t *values, uint32_t count) { (void)command; (void)offset; (void)values; (void)count; return 0; }
uint32_t *sceAgcDcbDrawIndexAuto(void *command, uint32_t count, uint64_t modifier) { (void)command; (void)count; (void)modifier; return 0; }
uint32_t *sceAgcDcbSetFlip(void *command, uint32_t handle, int buffer, uint32_t mode, int64_t argument) { (void)command; (void)handle; (void)buffer; (void)mode; (void)argument; return 0; }
int32_t sceAgcSuspendPoint(void) { return -1; }
uint32_t *sceAgcDcbDmaData(void *writer, uint32_t arg2, uint32_t dst_select, uint32_t arg4,
                           uint64_t destination, uint32_t src_select, uint32_t arg7,
                           uint64_t source_or_immediate, uint32_t byte_count,
                           uint32_t raw_wait, uint32_t disable_write_confirm, uint32_t cp_sync) {
    (void)writer; (void)arg2; (void)dst_select; (void)arg4; (void)destination;
    (void)src_select; (void)arg7; (void)source_or_immediate; (void)byte_count;
    (void)raw_wait; (void)disable_write_confirm; (void)cp_sync;
    return 0;
}
uint32_t *sceAgcDcbSetIndexSize(void *writer, uint8_t size, uint8_t pad) { (void)writer; (void)size; (void)pad; return 0; }
uint32_t *sceAgcDcbSetIndexBuffer(void *writer, const void *gpu_indices) { (void)writer; (void)gpu_indices; return 0; }
uint32_t *sceAgcDcbSetIndexCount(void *writer, uint32_t count) { (void)writer; (void)count; return 0; }
uint32_t *sceAgcDcbDrawIndex(void *writer, uint32_t index_count, const void *gpu_indices, uint64_t modifier) { (void)writer; (void)index_count; (void)gpu_indices; (void)modifier; return 0; }
uint32_t *sceAgcCbReleaseMem(void *writer, uint8_t engine, int16_t event_type, uint64_t event_data,
                             int8_t data_sel, void *address, uint32_t int_sel, uint64_t value,
                             uint16_t gcr_cntl, uint16_t reserved, int8_t sync_scope, int32_t poll_cycles) {
    (void)writer; (void)engine; (void)event_type; (void)event_data; (void)data_sel; (void)address;
    (void)int_sel; (void)value; (void)gcr_cntl; (void)reserved; (void)sync_scope; (void)poll_cycles;
    return 0;
}
