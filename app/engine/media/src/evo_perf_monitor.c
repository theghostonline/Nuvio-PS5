/* evo_perf_monitor — see evo_perf_monitor.h. */
#include "evo_perf_monitor.h"
#include "evo_direct_mem.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#define SAMPLE_PERIOD_NS  500000000ll   /* 2 Hz */

static evo_perf_snapshot_t g_snap;

static long long s_last_sample_ns;
static long long s_last_cpu_ns;      /* CLOCK_PROCESS_CPUTIME_ID, 0 = unsupported */
static long long s_last_wall_ns;
static int       s_cpu_ok = 1;
static int       s_ncpu;

static double    s_gpu_ms_accum;
static int       s_gpu_ms_n;

static long long mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static long long proc_cpu_ns(void)
{
#ifdef CLOCK_PROCESS_CPUTIME_ID
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
        return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
#endif
    s_cpu_ok = 0;
    return 0;
}

static void ring_push(float *hist, float v)
{
    memmove(hist, hist + 1, (EVO_PERF_HIST - 1) * sizeof(float));
    hist[EVO_PERF_HIST - 1] = v;
}

void evo_perf_monitor_reset(void)
{
    memset(&g_snap, 0, sizeof(g_snap));
    s_last_sample_ns = 0;
    s_last_cpu_ns = 0;
    s_last_wall_ns = 0;
    s_gpu_ms_accum = 0.0;
    s_gpu_ms_n = 0;
}

void evo_perf_monitor_tick(double gpu_present_ms)
{
    long long now = mono_ns();

    if (gpu_present_ms > 0.0) {
        s_gpu_ms_accum += gpu_present_ms;
        s_gpu_ms_n++;
    }

    if (s_last_sample_ns == 0) {
        s_last_sample_ns = now;
        s_last_wall_ns = now;
        s_last_cpu_ns = proc_cpu_ns();
        if (s_ncpu <= 0) {
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            s_ncpu = (n > 0 && n <= 16) ? (int)n : 6;   /* PS5: 8 cores, ~6-7 to games */
        }
        return;
    }
    if (now - s_last_sample_ns < SAMPLE_PERIOD_NS)
        return;

    /* --- CPU: process CPU time / (wall * ncores) --- */
    float cpu = 0.0f;
    if (s_cpu_ok) {
        long long cpu_ns  = proc_cpu_ns();
        long long wall_ns = now - s_last_wall_ns;
        if (s_cpu_ok && wall_ns > 0) {
            double busy = (double)(cpu_ns - s_last_cpu_ns) /
                          ((double)wall_ns * (double)s_ncpu);
            cpu = (float)(busy * 100.0);
            if (cpu < 0.0f) cpu = 0.0f;
        }
        s_last_cpu_ns = cpu_ns;
    }
    s_last_wall_ns = now;

    /* --- GPU: mean present cost as % of a 60 Hz frame budget --- */
    float gpu = 0.0f;
    if (s_gpu_ms_n > 0)
        gpu = (float)((s_gpu_ms_accum / s_gpu_ms_n) / 16.667 * 100.0);
    s_gpu_ms_accum = 0.0;
    s_gpu_ms_n = 0;

    /* --- RAM: direct-memory pool usage --- */
    evo_direct_mem_stats_t dm;
    memset(&dm, 0, sizeof(dm));
    evo_direct_mem_get_stats(&dm);
    float ram_mb    = (float)((double)dm.allocated_bytes / (1024.0 * 1024.0));
    float ram_total = (float)((double)dm.total_bytes     / (1024.0 * 1024.0));
    if (ram_total < 1.0f) ram_total = 1.0f;

    g_snap.cpu_pct = cpu;
    g_snap.gpu_pct = gpu;
    g_snap.ram_mb  = ram_mb;
    g_snap.ram_total_mb = ram_total;
    if (cpu     > g_snap.cpu_peak_pct) g_snap.cpu_peak_pct = cpu;
    if (gpu     > g_snap.gpu_peak_pct) g_snap.gpu_peak_pct = gpu;
    if (ram_mb  > g_snap.ram_peak_mb)  g_snap.ram_peak_mb  = ram_mb;

    ring_push(g_snap.cpu_hist, cpu / 100.0f);
    ring_push(g_snap.gpu_hist, gpu / 100.0f);
    ring_push(g_snap.ram_hist, ram_mb / ram_total);
    if (g_snap.count < EVO_PERF_HIST) g_snap.count++;

    s_last_sample_ns = now;
}

void evo_perf_monitor_get(evo_perf_snapshot_t *out)
{
    if (out) *out = g_snap;
}
