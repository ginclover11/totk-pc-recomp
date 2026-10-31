// Linux stand-ins for prof_win.c. The sampling profiler (TOTK_PROFILE) and the frame timing lines (TOTK_TRACE=perf) wait
// for a Linux port; everything here does nothing, so the callers need no conditions. Use perf(1) meanwhile.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

void prof_start(void) {}
void prof_write(void) {}
uint64_t perf_begin(void) { return 0; }
void perf_end(int kind, uint64_t t0) { (void)kind, (void)t0; }
void perf_replay(uint64_t t0) { (void)t0; }
void perf_replay_thread(void) {}
void perf_main_thread(void) {}
void perf_count(int kind) { (void)kind; }
void perf_present(uint64_t frame) { (void)frame; }
void perf_summary_flush(void) {}
__attribute__((weak)) void gfx_exit_log(void) {}
void perf_gpu_wait(uint64_t t0) { (void)t0; }
void perf_set_overlay(int mode) { (void)mode; }
int perf_gpu_timing(void) { return 0; }
void perf_gpu_busy(uint64_t ns) { (void)ns; }
uint64_t perf_gpu_busy_total(void) { return 0; }
void (*perf_gpu_stats)(PerfGpu *out);

// A/B switching of experiments, as prof_win.c does it.
int rt_ab(void) {
    static int64_t period = -1;   // us of world clock; 0: off
    if (period < 0) {
        const char *e = cfg_get("TOTK_AB");
        period = e && *e ? (int64_t)(atof(e) * 1e6) : 0;
    }
    int64_t w = period ? frame_step_world_us() : -1;
    if (w < 0) return 0;   // before the world clock: A
    int64_t k = w / period % 4;
    return k == 1 || k == 2;
}

int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "TOTK_%s", name);
    const char *e = cfg_get(key);
    if (!e || !*e) e = def;
    return !strcmp(e, "ab") ? 2 : strcmp(e, "0") != 0;
}
