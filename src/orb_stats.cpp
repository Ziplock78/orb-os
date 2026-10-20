// The Orb OS — measurements: the boot-time PSRAM / internal RAM ledger, the frame and
// load counters /health reports, the loop() phase timer, and the periodic [memdbg] line.
// Moved out of main.cpp unchanged.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "orb_state.h"
#include "orb_stats.h"
#include "display.h"
#include "audio.h"
#include "app_shell.h"
#include "theme_select.h"
#include "theme_style.h"
#include "theme_art.h"
#include "lv_psram_alloc.h"
#include "diag_log.h"

// PSRAM ledger. Free memory went from 592 KB (before disabling two unused apps) to
// 105 KB (after), which means something else absorbed the ~3.5 MB that was freed and
// then some. Printing the balance at each stage of boot shows WHERE it goes, instead of
// inferring it from whatever fails to allocate first. Cheap: a handful of prints, once.
void psram_mark(const char *stage) {
    static uint32_t s_prev = 0;
    const uint32_t now = (uint32_t)ESP.getFreePsram();
    const int32_t delta = s_prev ? (int32_t)((now - s_prev) / 1024) : 0;
    Serial.printf("[psram] %-26s free %6u KB", stage, (unsigned)(now / 1024));
    if (s_prev) Serial.printf("   (%+ld KB)", (long)delta);
    Serial.println();
    s_prev = now;

    // INTERNAL RAM at the same milestones, which is the memory that actually runs out.
    //
    // Every boot marker in this file has reported PSRAM, of which there are megabytes spare,
    // while the resource the networking stack starves for went unmeasured. Measured
    // 2026-08-23 on a clean power-on: 9,480 bytes free with a largest block of 3,316 — and
    // that is the HEALTHY state, before anything goes wrong. A TCP connect plus an HTTP
    // request needs more than that at peak, so ordinary dips take the feed down. It was
    // never a leak; the ceiling is simply too low, and nobody could see it because nobody
    // was printing this number.
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    static uint32_t s_prevInt = 0;
    const uint32_t nowInt = (uint32_t)hi.total_free_bytes;
    Serial.printf("[intram] %-25s free %6u B  largest %5u B  blocks %u",
                  stage, (unsigned)nowInt, (unsigned)hi.largest_free_block,
                  (unsigned)hi.allocated_blocks);
    if (s_prevInt) Serial.printf("   (%+ld B)", (long)((int32_t)nowInt - (int32_t)s_prevInt));
    Serial.println();
    s_prevInt = nowInt;

#if ORB_LV_STATS
    // ...and how much of that internal RAM is the LVGL allocator's, which is the half the
    // [intram] line above cannot answer. `held` against `free` says whether LVGL IS the
    // problem; the histogram says what a different ORB_LV_BIG_ALLOC would actually move,
    // since everything in the bands below the current threshold is what a lower one sends
    // to PSRAM. See the note over the threshold in lv_psram_alloc.h.
    // ONE line per milestone, not three. This printed a total, a size histogram and a
    // fallback count at each of the thirteen boot marks — thirty-nine lines of diagnostics
    // for a question (is LVGL holding internal RAM?) that has been answered. The histogram
    // is what sized ORB_LV_BIG_ALLOC and it still exists, on the runtime line below and in
    // ?orb mem, where it can be asked for rather than being printed at everyone.
    Serial.printf("[lvmem] %-25s held %6u B in %4u allocs (peak %6u B)  psram %u B%s\n",
                  stage, (unsigned)orb_lv_live_int_bytes, (unsigned)orb_lv_live_int_count,
                  (unsigned)orb_lv_peak_int_bytes, (unsigned)orb_lv_live_ext_bytes,
                  (orb_lv_fallback_to_int || orb_lv_fallback_to_ext) ? "  (FALLBACKS)" : "");
#endif
}


// Rendered frames per second, sampled between /health calls. The sweep advances a fixed
// step per timer tick rather than by elapsed time, so it visibly runs slow whenever the
// render loop cannot keep its 30 ms cadence — Zion spotted exactly that by comparing the
// device against Launch Kit's preview. This turns that observation into a number.
// Milliseconds of CPU spent per wall-clock second, sampled between /health calls.
// lvgl_ms_per_s near 1000 means the render loop is saturated; flush_ms_per_s says how
// much of that was the QSPI push rather than compositing.
unsigned host_lvgl_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t us = display_lvgl_us(), now = millis();
    unsigned v = 0;
    if (lastMs && now > lastMs) v = (unsigned)(((us - last) / 1000UL) * 1000UL / (now - lastMs));
    last = us; lastMs = now;
    return v;
}
// How many of the theme's declared background frames are actually baked into themeart.
// Read-only: theme_art::has() is an index lookup, it decodes and allocates nothing.
int host_bg_frames_baked() {
    const int want = theme_style::clock().bgAnim.frames;
    const char *slug = theme_select::activeSlug();
    int got = 0;
    for (int i = 1; i <= want; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "clock_plate_%02d.png", i);
        if (theme_art::has(slug, name)) ++got;
    }
    return got;
}

// Internal-heap block counts, for the leak hunt. A rising allocated-block count with a
// falling free total is a leak; a steady count with a falling total is fragmentation.
// They are different faults with different fixes, and "free KB" cannot tell them apart.
unsigned host_internal_alloc_blocks() {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    return (unsigned)hi.allocated_blocks;
}
unsigned host_internal_free_blocks() {
    multi_heap_info_t hi;
    heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
    return (unsigned)hi.free_blocks;
}
// ---- where the second goes outside LVGL -----------------------------------------------
// Measured 2026-10-06: LVGL owned ~640 ms of every second on the Flight Tracker while the frame
// rate sat well below what the per-frame cost implied, and the missing ~360 ms was neither the
// web server nor the ADS-B fetch -- both ruled out by suppressing each and watching the figure
// refuse to move. This splits the rest of loop() by name rather than guessing again.
//
// It found the delay() at the bottom of this function: CONFIG_FREERTOS_HZ is 1000, so delay(5)
// is a real five-millisecond vTaskDelay every pass. Cutting it to 1 ms raised passes from 53 to
// 128 a second and changed the frame rate not at all, which is how we know frames were never
// what the loop was short of. Left at 5 for that reason -- spending the CPU buys nothing.
uint32_t g_loopDelayMs = 5;
static const char *LP_NAME[LP_N] = { "input", "lvgl", "net", "work", "delay" };
static uint32_t g_lp[LP_N] = { 0 };
static uint32_t g_lpPasses = 0, g_lpAt = 0;
static volatile uint32_t g_lpPerS = 0;

void loop_phase_report(const uint32_t *t) {
    for (int i = 0; i < LP_N; ++i) g_lp[i] += t[i + 1] - t[i];
    ++g_lpPasses;
    const uint32_t now = millis();
    if (now - g_lpAt <= 10000) return;
    if (g_lpAt && g_lpPasses) {
        const uint32_t span = now - g_lpAt;
        g_lpPerS = g_lpPasses * 1000UL / span;
        // ms per SECOND, not per pass: a 40 us phase run 200 times a second is not a small cost.
        Serial.printf("[loop] %lu passes/s:", (unsigned long)g_lpPerS);
        for (int i = 0; i < LP_N; ++i)
            Serial.printf(" %s %lu ms/s", LP_NAME[i], (unsigned long)(g_lp[i] / 1000UL * 1000UL / span));
        Serial.printf(" (delay=%lu ms)\n", (unsigned long)g_loopDelayMs);
    }
    g_lpAt = now; g_lpPasses = 0;
    for (int i = 0; i < LP_N; ++i) g_lp[i] = 0;
}
unsigned host_loop_per_s() { return (unsigned)g_lpPerS; }

// Time per second parked in the panel's vertical blanking. Real latency, so it belongs in the
// same ledger as render and transfer cost.
unsigned host_te_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t us = display_te_wait_us(), now = millis();
    unsigned v = 0;
    if (lastMs && now > lastMs) v = (unsigned)(((us - last) / 1000UL) * 1000UL / (now - lastMs));
    last = us; lastMs = now;
    return v;
}
// Hundredths of a screen per second ACTUALLY sent to the panel. Paired with screens_per_s
// (what LVGL rendered) this says whether the box being pushed is bigger than the area that
// changed -- the mistake a full-width band made before the box was tightened.
unsigned host_pushed_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t px = display_pushed_px(), now = millis();
    unsigned v = 0;
    const uint32_t perScreen = (uint32_t)SCREEN_W * SCREEN_H;
    if (lastMs && now > lastMs) v = (unsigned)(((px - last) * 100UL / perScreen) * 1000UL / (now - lastMs));
    last = px; lastMs = now;
    return v;
}
unsigned host_flush_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t us = display_flush_us(), now = millis();
    unsigned v = 0;
    if (lastMs && now > lastMs) v = (unsigned)(((us - last) / 1000UL) * 1000UL / (now - lastMs));
    last = us; lastMs = now;
    return v;
}

// Full screens' worth of pixels repainted per second (466x466 = 1 screen). A value near
// the frame rate means every frame repaints essentially the whole display; a value far
// below it means LVGL really is honouring small dirty rectangles. This distinguishes
// "too much area" from "too many layers" — halving the sweep's redraw rate moved the
// frame cost by 0.2%, so one of those two assumptions is wrong.
unsigned host_screens_per_s() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t px = display_flushed_px(), now = millis();
    unsigned v = 0;
    const uint32_t perScreen = (uint32_t)SCREEN_W * SCREEN_H;
    if (lastMs && now > lastMs) v = (unsigned)(((px - last) * 100UL / perScreen) * 1000UL / (now - lastMs));
    last = px; lastMs = now;
    return v;   // hundredths of a screen per second
}

unsigned host_fps() {
    static uint32_t lastFrames = 0, lastMs = 0;
    const uint32_t frames = display_frames();
    const uint32_t now = millis();
    unsigned fps = 0;
    if (lastMs && now > lastMs) fps = (unsigned)((frames - lastFrames) * 1000UL / (now - lastMs));
    lastFrames = frames; lastMs = now;
    return fps;
}

// The every-~15-s half of loop()'s 5-second status tick: heap trend to the diag ring,
// [memdbg] and (with ORB_LV_STATS) [lvmem] to serial. Called from loop() every 5 s.
void stats_status_tick() {
    static int diagTick = 0;
    if (++diagTick >= 3) {   // every ~15s — a heap trend without flooding the ring buffer
        diagTick = 0;
        diag::log("heap %u min %u app %s", (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), app_shell::name());
        // Direct to Serial, not the diag ring buffer: this is instrumentation for the
        // 2026-08-22 memory investigation, wanted live over the cable rather than
        // competing for room in the compact RTC history. Fragmentation is the thing
        // "heap %u" above cannot show — a device can report 8 KB free and still be
        // unable to satisfy a 2 KB request if that 8 KB is forty scattered crumbs, which
        // is exactly the gap between free_bytes and largest_free_block below.
        {
            multi_heap_info_t hi;
            heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
            // Frames since the previous [memdbg], i.e. a real fps over a known 15 s
            // window. host_fps() cannot serve this: it measures frames since ITS last
            // call and is only called by /health, so a single request always reads 0 —
            // which is exactly why this looked like a dead renderer earlier today.
            static uint32_t s_dbgFrames = 0, s_dbgMs = 0;
            const uint32_t nowFr = display_frames(), nowMsFr = millis();
            unsigned realFps = 0;
            if (s_dbgMs && nowMsFr > s_dbgMs)
                realFps = (unsigned)((nowFr - s_dbgFrames) * 1000UL / (nowMsFr - s_dbgMs));
            s_dbgFrames = nowFr; s_dbgMs = nowMsFr;
            Serial.printf("[memdbg] internal free=%u largest=%u blocks(free/alloc)=%u/%u "
                          "stacks(adsb/audio/loop)=%u/%u/%u radar=%d fps=%u\n",
                          (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
                          (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
                          (unsigned)(g_adsbTaskHandle ? uxTaskGetStackHighWaterMark(g_adsbTaskHandle) : 0),
                          (unsigned)audio_stack_free_bytes(),
                          (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                          (int)g_radarViewActive, realFps);
#if ORB_LV_STATS
            // Every FOURTH [memdbg], i.e. once a minute, and only when a number has
            // actually moved.
            //
            // At 115200 baud these three lines are about 300 bytes, and Serial.printf
            // blocks the render loop while they go out: measured at roughly 50 ms of
            // every second in the windows they landed in, which showed up as a real fps
            // dip and briefly looked like the animation burst I was hunting. A
            // diagnostic that perturbs the thing it measures has to be rare.
            static int      s_lvEvery = 0;
            static uint32_t s_lvPrevInt = 0;
            static uint32_t s_lvPrevCount = 0;
            const bool lvMoved = (orb_lv_live_int_bytes != s_lvPrevInt) ||
                                 (orb_lv_live_int_count != s_lvPrevCount);
            s_lvPrevInt   = orb_lv_live_int_bytes;
            s_lvPrevCount = orb_lv_live_int_count;
            if (++s_lvEvery >= 4 && lvMoved) {
                s_lvEvery = 0;
                // The interesting state is the one AFTER a theme is up and apps have been
                // switched, which no boot mark can reach.
                Serial.printf("[lvmem] runtime: held %u B internal in %u allocs (peak %u), "
                              "psram %u B in %u; bands <64:%u <128:%u <256:%u <512:%u "
                              "<1k:%u <2k:%u <4k:%u 4k+:%u\n",
                              (unsigned)orb_lv_live_int_bytes, (unsigned)orb_lv_live_int_count,
                              (unsigned)orb_lv_peak_int_bytes,
                              (unsigned)orb_lv_live_ext_bytes, (unsigned)orb_lv_live_ext_count,
                              (unsigned)orb_lv_hist_count[0], (unsigned)orb_lv_hist_count[1],
                              (unsigned)orb_lv_hist_count[2], (unsigned)orb_lv_hist_count[3],
                              (unsigned)orb_lv_hist_count[4], (unsigned)orb_lv_hist_count[5],
                              (unsigned)orb_lv_hist_count[6], (unsigned)orb_lv_hist_count[7]);
                // BYTES per band as well as counts. The first capture printed only counts,
                // and a count cannot answer "how much would a lower threshold move?" — 1612
                // allocations under 64 bytes could be 30 KB or 60 KB and the two lead to
                // different decisions. This is the line that settles it.
                Serial.printf("[lvmem] runtime bytes: <64:%u <128:%u <256:%u <512:%u "
                              "<1k:%u <2k:%u <4k:%u 4k+:%u\n",
                              (unsigned)orb_lv_hist_bytes[0], (unsigned)orb_lv_hist_bytes[1],
                              (unsigned)orb_lv_hist_bytes[2], (unsigned)orb_lv_hist_bytes[3],
                              (unsigned)orb_lv_hist_bytes[4], (unsigned)orb_lv_hist_bytes[5],
                              (unsigned)orb_lv_hist_bytes[6], (unsigned)orb_lv_hist_bytes[7]);
            }
#endif
        }
    }
#if DEBUG_MEM
    static uint32_t lastFrames = 0;
    const uint32_t fr = display_frames();
    const unsigned fps = (fr - lastFrames) / 5;
    lastFrames = fr;
    Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                  (int)g_snap.size(), fps);
#endif
}
