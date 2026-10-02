/*
 * Screenshots for the test loop: the frame on the panel, as a 24-bit BMP
 * posted to the payload (GET /api/debug/shot reads it back). A 4K output is
 * halved to 1920x1080 so the file stays under the payload's 8 MB body limit.
 */
#include "nuvio_shot.h"

#include "nuvio_bridge.h"

#include "evo_agc_runtime.h"
#include "evo_boot_trace.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* PQ (SMPTE ST 2084) code value -> 8-bit SDR, 203-nit graphics white. */
static uint8_t pq_to_sdr(uint32_t code10)
{
    static uint8_t lut[1024];
    static int ready;
    if (!ready) {
        const double m1 = 0.1593017578125, m2 = 78.84375;
        const double c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        for (int i = 0; i < 1024; i++) {
            double e = pow(i / 1023.0, 1.0 / m2);
            double num = e - c1 > 0.0 ? e - c1 : 0.0;
            double nits = 10000.0 * pow(num / (c2 - c3 * e), 1.0 / m1);
            double v = nits / 203.0;
            v = v / (1.0 + v * 0.25) * 1.25;          /* soft shoulder */
            v = v > 1.0 ? 1.0 : v;
            lut[i] = (uint8_t)(pow(v, 1.0 / 2.2) * 255.0 + 0.5);
        }
        ready = 1;
    }
    return lut[code10 & 0x3ff];
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int nuvio_shot_post(void)
{
    int sw = 0, sh = 0;
    evo_agc_runtime_get_size(&sw, &sh);
    if (sw <= 0 || sh <= 0)
        return -1;

    const int f = sw >= 3000 ? 2 : 1;          /* 4K -> 1080p */
    const int ow = sw / f, oh = sh / f;
    const size_t row = (size_t)ow * 3u;        /* 1920*3 is a multiple of 4 */
    const size_t size = 54u + row * (size_t)oh;
    const int hdr = evo_agc_runtime_hdr_output_active();

    uint32_t *full = (uint32_t *)malloc((size_t)sw * (size_t)sh * 4u);
    uint8_t *bmp = (uint8_t *)malloc(size);
    if (!full || !bmp) {
        free(full);
        free(bmp);
        return -1;
    }
    evo_agc_runtime_read_scanout(full, sw, sh);

    memset(bmp, 0, 54);
    bmp[0] = 'B';
    bmp[1] = 'M';
    put_le32(bmp + 2, (uint32_t)size);
    put_le32(bmp + 10, 54);
    put_le32(bmp + 14, 40);
    put_le32(bmp + 18, (uint32_t)ow);
    put_le32(bmp + 22, (uint32_t)oh);
    bmp[26] = 1;
    bmp[28] = 24;
    put_le32(bmp + 34, (uint32_t)(row * (size_t)oh));

    for (int y = 0; y < oh; y++) {
        uint8_t *dst = bmp + 54 + (size_t)(oh - 1 - y) * row;   /* bottom-up */
        for (int x = 0; x < ow; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < f; dy++) {
                const uint32_t *src = full + (size_t)(y * f + dy) * (size_t)sw + (size_t)x * f;
                for (int dx = 0; dx < f; dx++) {
                    const uint32_t px = src[dx];
                    if (hdr) {
                        r += pq_to_sdr(px >> 20);
                        g += pq_to_sdr(px >> 10);
                        b += pq_to_sdr(px);
                    } else {
                        r += (px >> 16) & 0xff;
                        g += (px >> 8) & 0xff;
                        b += px & 0xff;
                    }
                }
            }
            const unsigned n = (unsigned)(f * f);
            dst[x * 3 + 0] = (uint8_t)(b / n);
            dst[x * 3 + 1] = (uint8_t)(g / n);
            dst[x * 3 + 2] = (uint8_t)(r / n);
        }
    }
    free(full);

    const int rc = nuvio_bridge_post_blob("/api/debug/shot", "image/bmp", bmp, size);
    evo_bt("shot: %dx%d%s -> %s", ow, oh, hdr ? " (hdr)" : "", rc == 0 ? "posted" : "failed");
    free(bmp);
    return rc;
}
