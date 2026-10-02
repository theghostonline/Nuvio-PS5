/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
/* Dolby Vision RPU parsing and profile 5 reconstruction - see dv_rpu.h. */
#include "dv_rpu.h"

#include <math.h>
#include <pthread.h>

#include <libavutil/dovi_meta.h>

#ifndef DV_RPU_NO_LOG
#include "evo_boot_trace.h"
#else
#define evo_bt(...) ((void)0)    /* host test builds */
#endif
#include <stdlib.h>
#include <string.h>

#define DV_MAX_ID 15

typedef struct {
    int      present;
    dv_curve comp[3];
} dv_mapping;

typedef struct {
    int   present;
    float nonlinear[9];
    float offset[3];
    float rgb_to_lms[9];
} dv_color;

struct dv_parser {
    dv_mapping vdr[DV_MAX_ID + 1];
    dv_color   dm;
    int        cur_mapping;
    uint8_t   *buf;
    int        cap;
};

/* ---- bit reader ------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    int bits;       /* total */
    int pos;
    int err;
} bits_t;

static unsigned get1(bits_t *b)
{
    if (b->pos >= b->bits) {
        b->err = 1;
        return 0;
    }
    unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u;
    b->pos++;
    return v;
}

static uint32_t getn(bits_t *b, int n)
{
    uint32_t v = 0;
    while (n-- > 0)
        v = (v << 1) | get1(b);
    return v;
}

static uint32_t ue(bits_t *b)
{
    int lz = 0;
    while (!get1(b) && !b->err && lz < 32)
        lz++;
    if (lz >= 32) {
        b->err = 1;
        return 0;
    }
    return (uint32_t)((1ull << lz) - 1 + getn(b, lz));
}

static int32_t se(bits_t *b)
{
    uint32_t k = ue(b);
    return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
}

static int32_t sbits(bits_t *b, int n)
{
    uint32_t v = getn(b, n);
    if (v & (1u << (n - 1)))
        return (int32_t)v - (int32_t)(1u << n);
    return (int32_t)v;
}

/* A reshaping coefficient as a float: fixed point with `denom` fraction bits,
 * or an IEEE float. */
static float coef(bits_t *b, int float_type, int denom)
{
    if (float_type) {
        union { uint32_t u; float f; } v;
        v.u = getn(b, 32);
        return v.f;
    }
    const double ipart = (double)se(b);
    const double fpart = (double)getn(b, denom);
    return (float)(ipart + fpart / (double)(1ull << denom));
}

/* ---- parser --------------------------------------------------------------- */

dv_parser *dv_parser_create(void)
{
    return (dv_parser *)calloc(1, sizeof(dv_parser));
}

void dv_parser_destroy(dv_parser *p)
{
    if (!p)
        return;
    free(p->buf);
    free(p);
}

void dv_parser_reset(dv_parser *p)
{
    if (!p)
        return;
    uint8_t *buf = p->buf;
    int cap = p->cap;
    memset(p, 0, sizeof *p);
    p->buf = buf;
    p->cap = cap;
}

/* ff_dovi_color_default: used when an RPU carries no display-management data. */
static void default_color(dv_color *c)
{
    static const int ycc[9] = {9575, 0, 14742, 9575, 1754, 4383, 9575, 17372, 0};
    static const int lms[9] = {5845, 9702, 837, 2568, 12256, 1561, 0, 679, 15705};
    for (int i = 0; i < 9; i++) {
        c->nonlinear[i] = ycc[i] / 8192.0f;
        c->rgb_to_lms[i] = lms[i] / 16384.0f;
    }
    c->offset[0] = 0.25f;
    c->offset[1] = 2.0f;
    c->offset[2] = 2.0f;
}

static uint32_t fnv(uint32_t h, const void *data, size_t n)
{
    const uint8_t *d = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++)
        h = (h ^ d[i]) * 16777619u;
    return h;
}

static void finish(const dv_parser *p, dv_params *out)
{
    const dv_mapping *m = &p->vdr[p->cur_mapping];
    dv_color def;
    const dv_color *c = &p->dm;
    if (!p->dm.present) {
        default_color(&def);
        c = &def;
    }
    memcpy(out->comp, m->comp, sizeof out->comp);
    memcpy(out->nonlinear, c->nonlinear, sizeof out->nonlinear);
    memcpy(out->offset, c->offset, sizeof out->offset);

    /* libplacebo: Dolby Vision outputs BT.2020-referred HPE LMS; the fixed
     * inverse of that, times the RPU's matrix. */
    static const double fixed[9] = {
        3.06441879, -2.16597676,  0.10155818,
       -0.65612108,  1.78554118, -0.12943749,
        0.01736321, -0.04725154,  1.03004253,
    };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++)
                s += fixed[i * 3 + k] * c->rgb_to_lms[k * 3 + j];
            out->lms2rgb[i * 3 + j] = (float)s;
        }
    uint32_t h = 2166136261u;
    h = fnv(h, out->comp, sizeof out->comp);
    h = fnv(h, out->nonlinear, sizeof out->nonlinear);
    h = fnv(h, out->offset, sizeof out->offset);
    h = fnv(h, out->lms2rgb, sizeof out->lms2rgb);
    out->hash = h ? h : 1;
    out->valid = 1;
}

int dv_rpu_parse(dv_parser *p, const uint8_t *nal, int size, dv_params *out)
{
    if (!p || !nal || size < 6)
        return -1;
    if (size > p->cap) {
        uint8_t *nb = (uint8_t *)realloc(p->buf, (size_t)size);
        if (!nb)
            return -1;
        p->buf = nb;
        p->cap = size;
    }
    /* Remove emulation-prevention bytes (00 00 03 -> 00 00). */
    int n = 0, zeros = 0;
    for (int i = 0; i < size; i++) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        p->buf[n++] = nal[i];
        zeros = nal[i] == 0 ? zeros + 1 : 0;
    }
    if (n < 6 || p->buf[0] != 25)          /* rpu_nal_prefix */
        return -1;
    const uint8_t *rpu = p->buf + 1;
    int len = n - 1;
    while (len && rpu[len - 1] == 0)
        len--;
    if (!len || rpu[len - 1] != 0x80)
        return -1;

    bits_t b = {rpu, len * 8, 0, 0};
    if (getn(&b, 6) != 2)                  /* rpu_type */
        return -1;
    const int rpu_format = (int)getn(&b, 11);
    getn(&b, 4);                           /* vdr_rpu_profile */
    getn(&b, 4);                           /* vdr_rpu_level */
    if (!get1(&b) || (rpu_format & 0x700)) /* vdr_seq_info_present */
        return -1;
    get1(&b);                              /* chroma_resampling_explicit_filter */
    const int coef_type = (int)getn(&b, 2);
    if (coef_type > 1)
        return -1;
    int denom = 32;
    if (coef_type == 0) {
        denom = (int)ue(&b);
        if (denom < 13 || denom > 32)
            return -1;
    }
    getn(&b, 2);                           /* vdr_rpu_normalized_idc */
    get1(&b);                              /* bl_video_full_range */
    const int bl_depth = (int)ue(&b) + 8;
    const uint32_t el_minus8 = ue(&b);
    ue(&b);                                /* vdr_bit_depth_minus8 */
    get1(&b);                              /* spatial_resampling_filter */
    const int dm_compression = (int)getn(&b, 3);
    get1(&b);                              /* el_spatial_resampling_filter */
    const int disable_residual = (int)get1(&b);
    (void)el_minus8;
    if (bl_depth < 8 || bl_depth > 16 || b.err)
        return -1;

    const int dm_present = (int)get1(&b);
    if (dm_compression > 1 || (dm_compression && !dm_present))
        return -1;
    const int use_prev = (int)get1(&b);
    if (!disable_residual)                 /* NLQ: a profile 7 enhancement layer */
        return -1;

    int mapping_id;
    if (use_prev) {
        mapping_id = (int)ue(&b);
        if (mapping_id > DV_MAX_ID)
            return -1;
        if (!p->vdr[mapping_id].present)
            mapping_id = 0;
        if (!p->vdr[mapping_id].present)
            return -1;
    } else {
        mapping_id = (int)ue(&b);
        if (mapping_id > DV_MAX_ID)
            return -1;
        dv_mapping m;
        memset(&m, 0, sizeof m);
        ue(&b);                            /* mapping_color_space */
        ue(&b);                            /* mapping_chroma_format_idc */
        const float pscale = 1.0f / (float)((1 << bl_depth) - 1);
        for (int c = 0; c < 3; c++) {
            const int np = (int)ue(&b) + 2;
            if (np > DV_MAX_PIECES + 1)
                return -1;
            m.comp[c].num_pivots = np;
            int pivot = 0;
            for (int i = 0; i < np; i++) {
                pivot += (int)getn(&b, bl_depth);
                m.comp[c].pivots[i] = (float)(pivot > 65535 ? 65535 : pivot) * pscale;
            }
        }
        ue(&b);                            /* num_x_partitions_minus1 */
        ue(&b);                            /* num_y_partitions_minus1 */
        for (int c = 0; c < 3; c++) {
            dv_curve *cv = &m.comp[c];
            for (int i = 0; i < cv->num_pivots - 1; i++) {
                const int idc = (int)ue(&b);
                if (idc > 1)
                    return -1;
                cv->method[i] = idc;
                if (idc == 0) {
                    const int order = (int)ue(&b) + 1;
                    if (order > 2)
                        return -1;
                    if (order == 1 && get1(&b))   /* linear_interp_flag */
                        return -1;
                    for (int k = 0; k <= order; k++)
                        cv->poly[i][k] = coef(&b, coef_type, denom);
                } else {
                    const int order = (int)getn(&b, 2) + 1;
                    if (order > 3)
                        return -1;
                    cv->mmr_order[i] = order;
                    cv->mmr_const[i] = coef(&b, coef_type, denom);
                    for (int j = 0; j < order; j++)
                        for (int k = 0; k < 7; k++)
                            cv->mmr[i][j][k] = coef(&b, coef_type, denom);
                }
                if (b.err)
                    return -1;
            }
        }
        if (b.err)
            return -1;
        m.present = 1;
        p->vdr[mapping_id] = m;
    }

    if (dm_present) {
        const int affected = (int)ue(&b);
        const int current = (int)ue(&b);
        if (affected > DV_MAX_ID || affected != current)
            return -1;
        ue(&b);                            /* scene_refresh_flag */
        if (!dm_compression) {
            dv_color c;
            for (int i = 0; i < 9; i++)
                c.nonlinear[i] = (float)sbits(&b, 16) / 8192.0f;
            for (int i = 0; i < 3; i++)
                c.offset[i] = (float)((double)getn(&b, 32) / (double)(1 << 28));
            for (int i = 0; i < 9; i++)
                c.rgb_to_lms[i] = (float)sbits(&b, 16) / 16384.0f;
            if (b.err)
                return -1;
            c.present = 1;
            p->dm = c;
        }
    } else {
        p->dm.present = 0;
    }

    p->cur_mapping = mapping_id;
    finish(p, out);
    return 0;
}

/* ---- per-pixel reference -------------------------------------------------- */

static float pq_eotf(float v)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float e = powf(fmaxf(v, 0.0f), 1.0f / m2);
    return powf(fmaxf(e - c1, 0.0f) / (c2 - c3 * e), 1.0f / m1);
}

static float pq_oetf(float l)
{
    const float m1 = 0.1593017578125f, m2 = 78.84375f;
    const float c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float y = powf(fmaxf(l, 0.0f), m1);
    return powf((c1 + c2 * y) / (1.0f + c3 * y), m2);
}

static float reshape(const dv_curve *cv, const float sig[3], float s)
{
    if (cv->num_pivots < 2)
        return s;
    int k = 0;
    for (int i = 1; i < cv->num_pivots - 1; i++)
        if (s >= cv->pivots[i])
            k = i;
    float r;
    if (cv->method[k] == 0) {
        r = (cv->poly[k][2] * s + cv->poly[k][1]) * s + cv->poly[k][0];
    } else {
        const float x[7] = {sig[0], sig[1], sig[2], sig[0] * sig[1], sig[0] * sig[2],
                            sig[1] * sig[2], sig[0] * sig[1] * sig[2]};
        r = cv->mmr_const[k];
        float p[7];
        for (int t = 0; t < 7; t++)
            p[t] = x[t];
        for (int j = 0; j < cv->mmr_order[k]; j++) {
            for (int t = 0; t < 7; t++)
                r += cv->mmr[k][j][t] * p[t];
            for (int t = 0; t < 7; t++)
                p[t] *= x[t];
        }
    }
    const float lo = cv->pivots[0], hi = cv->pivots[cv->num_pivots - 1];
    return r < lo ? lo : r > hi ? hi : r;
}

void dv_reconstruct(const dv_params *dv, const float ycc[3], float rgb[3])
{
    float sig[3], c[3], lms[3];
    for (int i = 0; i < 3; i++)
        sig[i] = ycc[i] < 0.0f ? 0.0f : ycc[i] > 1.0f ? 1.0f : ycc[i];
    for (int i = 0; i < 3; i++)
        c[i] = reshape(&dv->comp[i], sig, sig[i]);
    /* libplacebo scales the offsets like the samples: 2^10 / (2^10 - 1). */
    for (int i = 0; i < 3; i++)
        c[i] -= dv->offset[i] * (1024.0f / 1023.0f);
    for (int i = 0; i < 3; i++)
        lms[i] = pq_eotf(dv->nonlinear[i * 3 + 0] * c[0] + dv->nonlinear[i * 3 + 1] * c[1] +
                         dv->nonlinear[i * 3 + 2] * c[2]);
    for (int i = 0; i < 3; i++)
        rgb[i] = pq_oetf(dv->lms2rgb[i * 3 + 0] * lms[0] + dv->lms2rgb[i * 3 + 1] * lms[1] +
                         dv->lms2rgb[i * 3 + 2] * lms[2]);
}

/* ---- texture packing ------------------------------------------------------ */

void dv_pack_texture(const dv_params *dv, float *t)
{
    memset(t, 0, sizeof(float) * DV_TEX_FLOATS);
    for (int i = 0; i < 3; i++)
        t[i] = dv->offset[i];
    for (int i = 0; i < 9; i++) {
        t[3 + i] = dv->nonlinear[i];
        t[12 + i] = dv->lms2rgb[i];
    }
    for (int c = 0; c < 3; c++) {
        const dv_curve *cv = &dv->comp[c];
        float *b = t + 24 + c * 218;
        b[0] = (float)cv->num_pivots;
        for (int i = 0; i < cv->num_pivots && i < DV_MAX_PIECES + 1; i++)
            b[1 + i] = cv->pivots[i];
        for (int k = 0; k < cv->num_pivots - 1 && k < DV_MAX_PIECES; k++) {
            float *pc = b + 10 + k * 26;
            if (cv->method[k] == 0) {
                pc[0] = 0.0f;
                pc[1] = cv->poly[k][0];
                pc[2] = cv->poly[k][1];
                pc[3] = cv->poly[k][2];
            } else {
                pc[0] = (float)cv->mmr_order[k];
                pc[4] = cv->mmr_const[k];
                for (int j = 0; j < cv->mmr_order[k]; j++)
                    for (int q = 0; q < 7; q++)
                        pc[5 + j * 7 + q] = cv->mmr[k][j][q];
            }
        }
    }
}

/* ---- per-stream store ----------------------------------------------------- */

#define DV_RING 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_active;
static dv_parser *g_parser;
static struct { int64_t pts; dv_params p; int used; } g_ring[DV_RING];
static int g_next;
static int g_parsed, g_failed, g_logged_fail;

void dv_session_begin(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_parser)
        g_parser = dv_parser_create();
    dv_parser_reset(g_parser);
    memset(g_ring, 0, sizeof g_ring);
    g_next = 0;
    g_parsed = g_failed = g_logged_fail = 0;
    g_active = 1;
    pthread_mutex_unlock(&g_lock);
}

void dv_session_end(void)
{
    pthread_mutex_lock(&g_lock);
    g_active = 0;
    pthread_mutex_unlock(&g_lock);
}

int dv_session_active(void)
{
    return g_active;
}

dv_parser *dv_session_parser(void)
{
    return g_parser;
}

void dv_store(int64_t pts_us, const dv_params *dv)
{
    if (!dv || !dv->valid)
        return;
    pthread_mutex_lock(&g_lock);
    g_ring[g_next].pts = pts_us;
    g_ring[g_next].p = *dv;
    g_ring[g_next].used = 1;
    g_next = (g_next + 1) % DV_RING;
    pthread_mutex_unlock(&g_lock);
}

int dv_lookup(int64_t pts_us, dv_params *out)
{
    int best = -1, newest = -1;
    int64_t best_pts = INT64_MIN;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < DV_RING; i++) {
        if (!g_ring[i].used)
            continue;
        if (g_ring[i].pts == pts_us) {
            best = i;
            break;
        }
        /* Fallback: the latest frame at or before this one (a scene's
         * parameters rarely change frame to frame). */
        if (g_ring[i].pts <= pts_us && g_ring[i].pts > best_pts) {
            best_pts = g_ring[i].pts;
            best = i;
        }
        newest = i;
    }
    if (best < 0)
        best = newest;
    if (best >= 0)
        *out = g_ring[best].p;
    pthread_mutex_unlock(&g_lock);
    return best >= 0;
}

void dv_session_parse_au(const uint8_t *au, int size, int64_t pts_us)
{
    if (!g_active || !g_parser || !au)
        return;
    for (int i = 0; i + 3 < size; i++) {
        if (au[i] || au[i + 1] || au[i + 2] != 1)
            continue;
        const int nal = i + 3;
        int end = size;
        for (int j = nal; j + 2 < size; j++)
            if (!au[j] && !au[j + 1] && (au[j + 2] == 1 || (au[j + 2] == 0 && j + 3 < size && au[j + 3] == 1))) {
                end = j;
                break;
            }
        if (((au[nal] >> 1) & 0x3f) == 62 && end - nal > 2) {
            dv_params p;
            if (dv_rpu_parse(g_parser, au + nal + 2, end - nal - 2, &p) == 0) {
                dv_store(pts_us, &p);
                if (g_parsed++ == 0)
                    evo_bt("dv5: first RPU parsed - pivots %d/%d/%d, %s chroma",
                           p.comp[0].num_pivots, p.comp[1].num_pivots, p.comp[2].num_pivots,
                           p.comp[1].method[0] ? "MMR" : "polynomial");
            } else if (g_failed++ == 0 || (g_failed % 500) == 0) {
                evo_bt("dv5: RPU parse failed (%d so far, %d parsed)", g_failed, g_parsed);
            }
            return;
        }
        i = end - 1;
    }
}

int dv_from_avdovi(const void *data, dv_params *out)
{
    const AVDOVIMetadata *md = (const AVDOVIMetadata *)data;
    if (!md)
        return -1;
    const AVDOVIRpuDataHeader *h = av_dovi_get_header(md);
    const AVDOVIDataMapping *m = av_dovi_get_mapping(md);
    const AVDOVIColorMetadata *c = av_dovi_get_color(md);
    if (!h || !m || !c || h->bl_bit_depth < 8)
        return -1;
    dv_parser tmp;
    memset(&tmp, 0, sizeof tmp);
    const float ps = 1.0f / (float)((1 << h->bl_bit_depth) - 1);
    const float cs = 1.0f / (float)(1ull << h->coef_log2_denom);
    dv_mapping *dm = &tmp.vdr[0];
    for (int ci = 0; ci < 3; ci++) {
        const AVDOVIReshapingCurve *src = &m->curves[ci];
        dv_curve *cv = &dm->comp[ci];
        cv->num_pivots = src->num_pivots > DV_MAX_PIECES + 1 ? DV_MAX_PIECES + 1 : src->num_pivots;
        for (int i = 0; i < cv->num_pivots; i++)
            cv->pivots[i] = src->pivots[i] * ps;
        for (int i = 0; i < cv->num_pivots - 1; i++) {
            cv->method[i] = src->mapping_idc[i];
            if (src->mapping_idc[i] == AV_DOVI_MAPPING_POLYNOMIAL) {
                for (int k = 0; k <= src->poly_order[i] && k < 3; k++)
                    cv->poly[i][k] = src->poly_coef[i][k] * cs;
            } else {
                cv->mmr_order[i] = src->mmr_order[i];
                cv->mmr_const[i] = src->mmr_constant[i] * cs;
                for (int j = 0; j < src->mmr_order[i]; j++)
                    for (int k = 0; k < 7; k++)
                        cv->mmr[i][j][k] = src->mmr_coef[i][j][k] * cs;
            }
        }
    }
    dm->present = 1;
    for (int i = 0; i < 9; i++) {
        tmp.dm.nonlinear[i] = (float)av_q2d(c->ycc_to_rgb_matrix[i]);
        tmp.dm.rgb_to_lms[i] = (float)av_q2d(c->rgb_to_lms_matrix[i]);
    }
    for (int i = 0; i < 3; i++)
        tmp.dm.offset[i] = (float)av_q2d(c->ycc_to_rgb_offset[i]);
    tmp.dm.present = 1;
    finish(&tmp, out);
    return 0;
}
