/*
 * evo_hw.h - which console EVO is running on (#103).
 *
 * The only question asked today is "is this a PS5 Pro" (Sony's code name for
 * the Pro is "Trinity"). Two libkernel calls answer it:
 *
 *   sceKernelHasTrinityMode      NID yu17wG8L5FI - the title is running in
 *                                Pro mode (a Pro running a title in base-PS5
 *                                compatibility mode reports 0)
 *   sceKernelIsAuthenticTrinity  NID X0HkB92+NRE - the hardware is a Pro
 *
 * (third_party/findings_nid.txt section 1A.)
 *
 * Both are resolved at runtime with sceKernelDlsym, never imported. A name the
 * SDK stub or the console's libkernel lacks would be a null import, and a null
 * import crashes the app module at load with no log. An unresolved symbol just
 * leaves the answer "unknown", which every caller treats as a base PS5.
 *
 * evo_hw_probe() runs once at boot; everything else returns the cached result.
 */
#ifndef EVO_HW_H
#define EVO_HW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Tri-state results: 1 = yes, 0 = no, -1 = unknown (symbol did not resolve,
 * or the probe has not run). */
enum { EVO_HW_UNKNOWN = -1 };

/* Resolve and call the Trinity queries, cache the answers and log
 * "hw: ps5 pro=<0|1|?> trinity_mode=<0|1|?> authentic=<0|1|?>". Idempotent. */
void evo_hw_probe(void);

/* 1 only when the console is positively identified as a PS5 Pro; 0 for a base
 * PS5 AND for "unknown", so a failed probe can never enable a Pro-only path. */
int  evo_hw_is_ps5_pro(void);

/* The raw tri-states, for the log and the diagnostics screens. */
int  evo_hw_trinity_mode(void);
int  evo_hw_authentic_trinity(void);

/* 1 when at least one query answered. A PS5 Pro the probe cannot identify
 * reports 0 here - "PS5" would be a false claim, so say "unknown". */
int  evo_hw_model_known(void);

/* "PS5 Pro", "PS5", or "PS5 model unknown". */
const char *evo_hw_model_name(void);

#ifdef __cplusplus
}
#endif

#endif /* EVO_HW_H */
