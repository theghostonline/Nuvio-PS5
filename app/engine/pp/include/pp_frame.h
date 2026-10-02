/*
 * Neutral decoded-frame structure — no FFmpeg types.
 * FFmpeg (or any decoder) adapts *into* this shape; videoout never sees AVFrame.
 */
#ifndef PP_FRAME_H
#define PP_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum pp_frame_format {
    PP_FRAME_NV12 = 0,
    PP_FRAME_YUV420P = 1,
    PP_FRAME_RGBA = 2,
    PP_FRAME_BGRA = 3,
    /* GL-5 (#81): planar 4:2:0, 16-bit little-endian samples with 10 significant
     * bits in the low bits — what FFmpeg's software HEVC/VP9 decoders hand back
     * as yuv420p10le. Same plane layout as PP_FRAME_YUV420P; the strides are in
     * bytes (= 2 * samples). The GL video shader samples it as GL_R16 and skips
     * the old CPU pack-to-8-bit step. */
    PP_FRAME_YUV420P10 = 4,
    /* #41 Phase D: two-plane semi-planar 4:2:0 NV12 with 10-bit little-endian
     * samples low-aligned in 16-bit words — what native sceVideodec2 HEVC Main10
     * and VP9 Profile 2 decoders output.
     * Plane 0: Luma (Y), 16-bit words, stride in bytes (= 2 * samples).
     * Plane 1: Interleaved chroma (UV), 16-bit words, stride in bytes (= 2 * samples). */
    PP_FRAME_NV12_10 = 5
} pp_frame_format;

/*
 * How the frame is fitted to the panel. Since GL-4 (#80) this is a scale on the
 * video quad's clip-space corners (evo_gl_blit_yuv), not a CPU rescale — it
 * lives here because it describes the frame's presentation, and the converter
 * header that used to own it is gone.
 */
typedef enum pp_aspect_mode {
    PP_ASPECT_FIT = 0,     /* letterbox / pillarbox, preserve AR */
    PP_ASPECT_FILL = 1,    /* crop source to fill, preserve AR */
    PP_ASPECT_STRETCH = 2  /* ignore AR */
} pp_aspect_mode;

typedef struct pp_frame {
    pp_frame_format format;
    uint32_t width;
    uint32_t height;
    /* MB-padded luma height for NV12 — the row count at which the interleaved
     * UV plane starts (planes[1] == planes[0] + strides[0]*coded_height).
     * 0 means "same as height". Only the native NV12 decode path sets it;
     * the GPU (sceAgc) present path needs it, the CPU converters ignore it. */
    uint32_t coded_height;
    const uint8_t *planes[4];
    int strides[4];
    int64_t pts_us;
    /* Raw AVColorTransferCharacteristic of the source (0 = unspecified). Carried
     * for the HDR badge and #41's PQ/HLG tone-map tail; GL-5 does not act on it —
     * 10-bit is presented as SDR-with-more-precision. */
    int color_trc;
} pp_frame;

#ifdef __cplusplus
}
#endif

#endif /* PP_FRAME_H */
