#pragma once
// LVGL's heap, routed to PSRAM on the device.
//
// LVGL's own pool is internal RAM and was 64 KB. That is fine for objects and styles, but
// lv_font_load() parses a font by allocating its whole glyph bitmap out of that pool, and
// one 71 px menu font is 44 KB. The allocation failed, LVGL did not check the result, and
// load_glyph() wrote through the null pointer: a StoreProhibited panic in a boot loop,
// before any screen drew. (lv_font_loader.c:451.)
//
// Internal RAM is the scarce pool on this board — the largest free block sits around
// 30 KB after boot — while PSRAM has megabytes spare. Fonts are the first thing to want a
// large LVGL allocation, and they will not be the last, so the pool moves rather than
// growing: a bigger internal pool would just relocate the same failure and take memory
// from the TLS handshakes that already fragment there.
//
// Draw buffers were already allocated straight from PSRAM in display.cpp and are
// unaffected by this.
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>

// Hybrid, not all-PSRAM. Routing EVERYTHING to PSRAM fixed the font boot-loop but put
// LVGL's per-draw scratch buffers (glyph masks, blend lines) behind the slower external
// bus, which surfaced as a sluggish first knob interaction while those buffers warmed up.
// Small allocations are the hot path and go to internal RAM, exactly where LVGL's own
// 64 KB pool always lived; only big ones (a parsed font is ~44 KB, the old pool's whole
// size) go to PSRAM. Each side falls back to the other, so an allocation can degrade to
// the slow pool or the scarce one, but never to the unchecked null that caused the loop.
//
// The threshold is a build flag because it is a genuine tradeoff with no obvious right
// answer, and the two ends fail in different ways. Too high and LVGL's many medium
// allocations live in internal RAM and chop it into pieces too small for a TLS handshake,
// which needs two ~16 KB contiguous buffers: that is what left the radar and weather feeds
// dead with 70 KB free but a 17 KB largest block. Too low and LVGL's per-draw scratch sits
// behind the slower external bus. Measure before moving it; see the boot log's
// heap/largest numbers on any [adsb] or [weather] line.
// 2 KB, measured 2026-08-23. This sat at 16 KB, and the comment above ends "Measure before
// moving it" — so it was measured, on the device, by printing INTERNAL free at every boot
// milestone (the [intram] lines) instead of only PSRAM, which is all anything here had ever
// reported. The scarce pool was invisible.
//
// What 16 KB was costing, per boot:
//                        internal free   largest block   feed
//     16384 (before)          19 KB          7.1 KB      0 of 14 polls
//      2048 (now)             67 KB         31.7 KB     14 of 15 polls
//      1024 (also tried)      75 KB         31.7 KB     no better where it counts
//
// Display and radar alone were holding 137 KB of internal RAM in 641 small allocations,
// every one of them under the old threshold and so kept in the pool the network stack needs.
// A TCP connect plus an HTTP request could not fit in what was left, which is why the feed
// died in a different-looking way every time and why two days of fixes downstream of it kept
// helping a little and never holding.
//
// 1024 frees more total but does NOT raise the largest contiguous block, which is the number
// that decides whether a connection can be made, so it buys nothing here and pushes more of
// LVGL's per-draw scratch behind the slow bus for free. 2048 is the knee.
//
// Frame rate did not regress: 9 fps measured, against a sweep timer deliberately paced at
// 100 ms (10 fps ceiling). The scratch buffers that matter are still internal.
#ifndef ORB_LV_BIG_ALLOC
#define ORB_LV_BIG_ALLOC (2 * 1024)
#endif
static inline void *orb_lv_malloc(size_t size) {
    if (size >= ORB_LV_BIG_ALLOC) {
        void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return p ? p : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void *orb_lv_realloc(void *p, size_t size) {
    // heap_caps_realloc honours the requested caps and copies across regions when the
    // block has to move, so a small buffer growing past the threshold migrates to PSRAM.
    if (size >= ORB_LV_BIG_ALLOC) {
        void *q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        return q ? q : heap_caps_realloc(p, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    void *q = heap_caps_realloc(p, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void orb_lv_free(void *p) { heap_caps_free(p); }

#else   // desktop simulator: plain libc, no PSRAM to speak of
static inline void *orb_lv_malloc(size_t size) { return malloc(size); }
static inline void *orb_lv_realloc(void *p, size_t size) { return realloc(p, size); }
static inline void orb_lv_free(void *p) { free(p); }
#endif
