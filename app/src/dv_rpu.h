/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/*
 * Dolby Vision profile 5 support: parse the per-frame RPU and turn it into
 * the parameters the GPU needs to reconstruct the picture.
 *
 * Profile 5 carries no HDR10 layer. Its base layer is IPT-PQ-c2 shaped by
 * per-scene reshaping curves, and only the RPU says how to undo that, so
 * without it the picture shows green and purple. The PS5 cannot output Dolby
 * Vision, so the player rebuilds the image the way a Dolby Vision decoder
 * would and hands the display ordinary HDR10 (BT.2020 PQ):
 *
 *   1. reshape each component of the base layer (polynomial or MMR pieces)
 *   2. IPT' -> L'M'S' with the RPU's ycc_to_rgb matrix and offset
 *   3. PQ EOTF -> linear LMS
 *   4. LMS -> linear BT.2020 RGB (fixed inverse times the RPU's matrix)
 *   5. PQ OETF -> R'G'B' for the display
 *
 * The parser follows FFmpeg's libavcodec/dovi_rpudec.c and the maths
 * follows libplacebo (src/shaders/colorspace.c, pl_map_dovi_metadata), both
 * LGPL-2.1+, so the result matches what mpv and FFmpeg render.
 */
#ifndef NUVIO_DV_RPU_H
#define NUVIO_DV_RPU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DV_MAX_PIECES 8

typedef struct {
    int   num_pivots;                     /* 2..9 */
    float pivots[DV_MAX_PIECES + 1];      /* normalised base-layer code values */
    int   method[DV_MAX_PIECES];          /* 0 polynomial, 1 MMR */
    float poly[DV_MAX_PIECES][3];
    int   mmr_order[DV_MAX_PIECES];       /* 1..3 */
    float mmr_const[DV_MAX_PIECES];
    float mmr[DV_MAX_PIECES][3][7];
} dv_curve;

typedef struct {
    int      valid;
    dv_curve comp[3];
    float    nonlinear[9];      /* ycc_to_rgb, row-major */
    float    offset[3];         /* ycc_to_rgb_offset */
    float    lms2rgb[9];        /* fixed LMS->RGB times the RPU's rgb_to_lms */
    uint32_t hash;              /* changes whenever any of the above does */
} dv_params;

typedef struct dv_parser dv_parser;

dv_parser *dv_parser_create(void);
void       dv_parser_destroy(dv_parser *p);
void       dv_parser_reset(dv_parser *p);

/*
 * Parse one RPU: the payload of an HEVC NAL unit of type 62, starting right
 * after its two-byte NAL header, emulation-prevention bytes still in place.
 * On success fills *out and returns 0; on any error returns -1 and leaves the
 * parser's previous state intact.
 */
int dv_rpu_parse(dv_parser *p, const uint8_t *nal, int size, dv_params *out);

/* Pack *dv into the coefficient texture the profile 5 shaders read: one float
 * per RG16 texel (layout in tools/shaders/gen_dv5_pipes.py). `dst` holds at
 * least DV_TEX_FLOATS floats. */
#define DV_TEX_FLOATS 704
void dv_pack_texture(const dv_params *dv, float *dst);

/*
 * The playing stream's parameters, by presentation time. The decoder stores
 * each frame's RPU result under that frame's PTS as it decodes (decode order);
 * the renderer looks it up when that frame is shown (display order). Active
 * only while a profile 5 stream plays.
 */
void dv_session_begin(void);           /* a profile 5 stream opened */
void dv_session_end(void);
int  dv_session_active(void);
void dv_store(int64_t pts_us, const dv_params *dv);
/* The parameters for the frame at pts_us (or the nearest earlier one). */
int  dv_lookup(int64_t pts_us, dv_params *out);
/* The decoder's own parser, for its RPUs. */
dv_parser *dv_session_parser(void);

/* Hardware path: find the RPU NAL in an Annex B access unit, parse it with
 * the session parser and store the result under pts_us. */
void dv_session_parse_au(const uint8_t *au, int size, int64_t pts_us);

/* Software path: FFmpeg's AVDOVIMetadata (frame side data) -> dv_params. */
int dv_from_avdovi(const void *avdovi_metadata, dv_params *out);

/* Reference implementation of the per-pixel maths (same as the shader):
 * base-layer code values normalised to [0,1] -> BT.2020 PQ R'G'B'. */
void dv_reconstruct(const dv_params *dv, const float ycc[3], float rgb_pq[3]);

#ifdef __cplusplus
}
#endif

#endif /* NUVIO_DV_RPU_H */
