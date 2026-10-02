/*
 * evo_sweep.c — see evo_sweep.h.
 *
 * The record is single-writer by construction: begin/end run on the thread that
 * opens and closes a file, and the only hot hooks (note_present, probe_colour)
 * run on the render loop, which is the same thread. Nothing here locks.
 */
#include "evo_sweep.h"

#include "evo_boot_log.h"
#include "evo_agc_runtime.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SWEEP_PRESENT_RING 256
#define SWEEP_PROBE_PIXELS 16

/* Presents to let go by before the colour probe fires: enough for the first
 * real frames to have reached the panel and for any open-time flash / fade to
 * be over (OSD auto-hides after 3.5s), few enough that a short clip still gets probed. */
#define SWEEP_PROBE_AFTER  120

/*
 * evo_boot_log() formats into a 600-byte line buffer. The sweep line's fixed
 * part is ~440 bytes at its widest, so the file name — which goes last, so a
 * name with spaces still parses — gets a hard cap rather than silently taking
 * the tail of the line with it.
 */
#define SWEEP_NAME_MAX 120

typedef struct {
    int      active;
    char     path[SWEEP_NAME_MAX];
    char     codec[32];
    int      w, h;
    double   fps;
    double   duration_s;
    int      backend;
    int      open_result;

    uint64_t t_begin_us;

    uint64_t present_n;         /* swaps while the player screen was up   */
    uint64_t present_video_n;   /* of those, ones carrying a new frame    */
    uint64_t blit_us_total;     /* GPU work: upload + YUV->RGB + OSD      */
    uint64_t blit_us_max;
    uint64_t swap_us_total;     /* eglSwapBuffers: mostly the vblank wait */
    uint64_t present_ring[SWEEP_PRESENT_RING];   /* blit only */
    uint32_t present_ring_count;

    int      probe_done;
    uint32_t probe_sig;
    uint32_t probe_mean;        /* 0x00RRGGBB */
} sweep_record;

static sweep_record g_rec;

uint64_t evo_sweep_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)(ts.tv_nsec / 1000);
}

int evo_sweep_active(void)
{
    return g_rec.active;
}

static const char *sweep_basename(const char *p)
{
    const char *s;
    if (!p)
        return "";
    s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static uint64_t sweep_p95(const uint64_t *ring, uint32_t count)
{
    uint64_t tmp[SWEEP_PRESENT_RING];
    uint32_t n, i, j, idx;
    n = count;
    if (n == 0)
        return 0;
    if (n > SWEEP_PRESENT_RING)
        n = SWEEP_PRESENT_RING;
    memcpy(tmp, ring, n * sizeof(uint64_t));
    for (i = 1; i < n; i++) {
        uint64_t val = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > val) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = val;
    }
    idx = (n * 95u) / 100u;
    if (idx >= n)
        idx = n - 1;
    return tmp[idx];
}

void evo_sweep_file_begin(const char *path, const char *codec_name,
                          int w, int h, double fps, double duration_s,
                          int backend, int open_result)
{
    memset(&g_rec, 0, sizeof(g_rec));
    snprintf(g_rec.path, sizeof(g_rec.path), "%s", sweep_basename(path));
    snprintf(g_rec.codec, sizeof(g_rec.codec), "%s",
             (codec_name && codec_name[0]) ? codec_name : "?");
    g_rec.w = w;
    g_rec.h = h;
    g_rec.fps = fps;
    g_rec.duration_s = duration_s;
    g_rec.backend = backend;
    g_rec.open_result = open_result;
    g_rec.t_begin_us = evo_sweep_now_us();
    g_rec.active = 1;
}

void evo_sweep_file_failed(const char *path, const char *codec_name,
                           int w, int h, int open_result, const char *note)
{
    char name[SWEEP_NAME_MAX];
    g_rec.active = 0;
    snprintf(name, sizeof(name), "%s", sweep_basename(path));
    evo_boot_log("sweep v=1 open=%s codec=%s res=%dx%d verdict=%s note=%s file=%s",
                 evo_vdec_open_result_name((evo_vdec_open_result)open_result),
                 (codec_name && codec_name[0]) ? codec_name : "?",
                 w, h,
                 (open_result == EVO_VDEC_OPEN_NO_DECODER) ? "no_decoder" : "failed",
                 (note && note[0]) ? note : "-",
                 name);
    evo_boot_log_flush();
}

void evo_sweep_note_present(uint64_t begin_us, uint64_t blit_done_us,
                            uint64_t swap_done_us, int video)
{
    uint64_t blit_us, swap_us;
    uint32_t i;
    if (!g_rec.active || blit_done_us < begin_us || swap_done_us < blit_done_us)
        return;
    blit_us = blit_done_us - begin_us;
    swap_us = swap_done_us - blit_done_us;

    /* Index from the count BEFORE this sample, so a full ring overwrites the
     * oldest entry rather than skipping slot 0 forever. */
    if (g_rec.present_ring_count < SWEEP_PRESENT_RING) {
        i = g_rec.present_ring_count;
        g_rec.present_ring_count++;
    } else {
        i = (uint32_t)(g_rec.present_n % SWEEP_PRESENT_RING);
    }
    g_rec.present_ring[i] = blit_us;

    g_rec.present_n++;
    if (video)
        g_rec.present_video_n++;
    g_rec.blit_us_total += blit_us;
    g_rec.swap_us_total += swap_us;
    if (blit_us > g_rec.blit_us_max)
        g_rec.blit_us_max = blit_us;
}

void evo_sweep_probe_colour(void)
{
    uint8_t rgb[SWEEP_PROBE_PIXELS * 3];
    uint32_t sum_r = 0, sum_g = 0, sum_b = 0;
    uint32_t hash = 2166136261u;   /* FNV-1a */
    int n, i;

    if (!g_rec.active || g_rec.probe_done)
        return;
    if (g_rec.present_video_n < SWEEP_PROBE_AFTER)
        return;

    n = evo_agc_probe_rgb(rgb, SWEEP_PROBE_PIXELS);
    if (n <= 0) {
        g_rec.probe_done = 1;   /* probe not supported or empty: don't retry per frame */
        return;
    }
    for (i = 0; i < n * 3; i++) {
        hash ^= rgb[i];
        hash *= 16777619u;
    }
    for (i = 0; i < n; i++) {
        sum_r += rgb[i * 3 + 0];
        sum_g += rgb[i * 3 + 1];
        sum_b += rgb[i * 3 + 2];
    }
    g_rec.probe_sig  = hash;
    g_rec.probe_mean = ((sum_r / (uint32_t)n) << 16) |
                       ((sum_g / (uint32_t)n) << 8)  |
                        (sum_b / (uint32_t)n);
    g_rec.probe_done = 1;
}

void evo_sweep_file_end(const evo_vdec *v, const pp_playback_stats *ps)
{
    evo_vdec_stats vs;
    const char *verdict;
    double play_s, dec_avg_ms, dec_p95_ms, dec_max_ms;
    double gpu_avg_ms, gpu_p95_ms, gpu_max_ms, swap_avg_ms, budget_ms, dec_fps;
    uint64_t pub, drop_late, drop_seek;

    if (!g_rec.active)
        return;
    g_rec.active = 0;

    memset(&vs, 0, sizeof(vs));
    if (v)
        evo_vdec_get_stats(v, &vs);

    play_s = (double)(evo_sweep_now_us() - g_rec.t_begin_us) / 1e6;

    dec_avg_ms = vs.frames_out ? (double)vs.decode_us_total / (double)vs.frames_out / 1000.0 : 0.0;
    dec_p95_ms = v ? ((double)evo_vdec_decode_p95_us(v) / 1000.0) : 0.0;
    dec_max_ms = (double)vs.decode_us_max / 1000.0;

    gpu_avg_ms  = g_rec.present_n
                      ? (double)g_rec.blit_us_total / (double)g_rec.present_n / 1000.0
                      : 0.0;
    gpu_p95_ms  = (double)sweep_p95(g_rec.present_ring, g_rec.present_ring_count) / 1000.0;
    gpu_max_ms  = (double)g_rec.blit_us_max / 1000.0;
    swap_avg_ms = g_rec.present_n
                      ? (double)g_rec.swap_us_total / (double)g_rec.present_n / 1000.0
                      : 0.0;

    /* The real-time budget is one frame period. Decode and present both have to
     * fit inside it, on the same thread they run on today. */
    budget_ms = (g_rec.fps > 1.0) ? (1000.0 / g_rec.fps) : 0.0;
    dec_fps   = (play_s > 0.1) ? (double)vs.frames_out / play_s : 0.0;

    pub       = ps ? ps->frames_published      : 0;
    drop_late = ps ? ps->frames_late_dropped   : 0;
    drop_seek = ps ? ps->frames_discarded_seek : 0;

    /*
     * "No decoder" vs "too slow" — the distinction this sweep exists to make.
     * An open that never produced a frame is a decoder problem; a file that
     * decoded but could not hold its own frame period is a throughput problem,
     * and it is a throughput problem whether that shows up as decode cost over
     * budget or as the clock dropping late frames.
     */
    if (g_rec.open_result == EVO_VDEC_OPEN_NO_DECODER)
        verdict = "no_decoder";
    else if (!v && play_s >= 0.5)
        verdict = "realtime";
    else if (vs.frames_out == 0 && v)
        verdict = "no_frames";
    else if (vs.fatal_errors > 0)
        verdict = "decode_error";
    else if (budget_ms > 0.0 && dec_p95_ms > budget_ms)
        verdict = "slow_decode";
    else if (pub > 0 && drop_late * 20 > pub)   /* >5% of published frames late */
        verdict = "slow_pipeline";
    else
        verdict = "realtime";

    evo_boot_log("sweep v=1 codec=%s res=%dx%d fps=%.3f dur=%.1f be=%s open=%s "
                 "play_s=%.1f dec_n=%llu dec_ms_avg=%.2f dec_ms_p95=%.2f dec_ms_max=%.2f "
                 "dec_fps=%.1f dec_fatal=%llu sw_map=%llu budget_ms=%.2f "
                 "pres_n=%llu pres_vid=%llu gpu_ms_avg=%.2f gpu_ms_p95=%.2f "
                 "gpu_ms_max=%.2f swap_ms_avg=%.2f "
                 "pub=%llu drop_late=%llu drop_seek=%llu "
                 "sig=%08x rgb=%06x verdict=%s file=%s",
                 g_rec.codec, g_rec.w, g_rec.h, g_rec.fps, g_rec.duration_s,
                 (vs.backend == EVO_VDEC_BACKEND_NATIVE) ? "native" : "ffmpeg",
                 evo_vdec_open_result_name((evo_vdec_open_result)g_rec.open_result),
                 play_s,
                 (unsigned long long)vs.frames_out, dec_avg_ms, dec_p95_ms, dec_max_ms,
                 dec_fps,
                 (unsigned long long)vs.fatal_errors,
                 (unsigned long long)vs.frames_sw_mapped,
                 budget_ms,
                 (unsigned long long)g_rec.present_n,
                 (unsigned long long)g_rec.present_video_n,
                 gpu_avg_ms, gpu_p95_ms, gpu_max_ms, swap_avg_ms,
                 (unsigned long long)pub,
                 (unsigned long long)drop_late,
                 (unsigned long long)drop_seek,
                 g_rec.probe_done ? g_rec.probe_sig : 0u,
                 g_rec.probe_done ? g_rec.probe_mean : 0u,
                 verdict, g_rec.path);
    evo_boot_log_flush();
}
