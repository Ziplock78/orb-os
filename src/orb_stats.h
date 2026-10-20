// The Orb OS — measurements for the serial log and /health. See orb_stats.cpp.
#pragma once
#include <stdint.h>

void psram_mark(const char *stage);   // boot ledger: PSRAM and internal RAM at each stage

// loop()'s phase timer. loop() stamps micros() at each boundary into a LP_N + 1 array.
enum { LP_INPUT = 0, LP_LVGL, LP_NET, LP_WORK, LP_DELAY, LP_N };
extern uint32_t g_loopDelayMs;        // the delay() at the bottom of loop(); /rdbg can change it
void loop_phase_report(const uint32_t *t);

// Per-second rates since the previous call, for /health.
unsigned host_fps();
unsigned host_lvgl_load();
unsigned host_flush_load();
unsigned host_te_load();
unsigned host_pushed_load();
unsigned host_screens_per_s();
unsigned host_loop_per_s();
int      host_bg_frames_baked();
unsigned host_internal_alloc_blocks();
unsigned host_internal_free_blocks();

void stats_status_tick();             // loop()'s 5-second status tick, the diagnostics half
