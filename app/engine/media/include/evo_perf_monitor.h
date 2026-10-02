/*
 * evo_perf_monitor — rolling telemetry for the playback diagnostic HUD (#63).
 *
 * GL-5 (#81) first pass: GPU frame-budget and direct-memory pool usage, each a
 * short rolling history the RmlUi #stats-hud draws as a sparkline. CPU% is
 * best-effort (CLOCK_PROCESS_CPUTIME_ID; 0 when the platform has no such clock).
 *
 * Sampled from the render loop via evo_perf_monitor_tick() every frame; the
 * monitor rate-limits itself to ~2 Hz.
 */
#ifndef EVO_PERF_MONITOR_H
#define EVO_PERF_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#define EVO_PERF_HIST 48   /* samples kept per metric (~24 s at 2 Hz) */

typedef struct {
    float cpu_hist[EVO_PERF_HIST];   /* 0..1, index 0 oldest .. count-1 newest */
    float gpu_hist[EVO_PERF_HIST];
    float ram_hist[EVO_PERF_HIST];
    int   count;                     /* valid samples, <= EVO_PERF_HIST */

    float cpu_pct,  cpu_peak_pct;    /* 0..100+ */
    float gpu_pct,  gpu_peak_pct;    /* present cost as % of the 16.67 ms budget */
    float ram_mb,   ram_peak_mb;     /* direct-memory pool bytes in use */
    float ram_total_mb;
} evo_perf_snapshot_t;

/* Call once per rendered frame. gpu_present_ms is the measured blit+swap cost of
 * the last present (0 if unknown). Cheap; only samples every ~500 ms. */
void evo_perf_monitor_tick(double gpu_present_ms);

/* Latest snapshot (thread: render loop only). */
void evo_perf_monitor_get(evo_perf_snapshot_t *out);

/* Drop the history (e.g. on file open) so a graph is not half old data. */
void evo_perf_monitor_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_PERF_MONITOR_H */
