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

static inline void *orb_lv_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void *orb_lv_realloc(void *p, size_t size) {
    return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static inline void orb_lv_free(void *p) { heap_caps_free(p); }

#else   // desktop simulator: plain libc, no PSRAM to speak of
static inline void *orb_lv_malloc(size_t size) { return malloc(size); }
static inline void *orb_lv_realloc(void *p, size_t size) { return realloc(p, size); }
static inline void orb_lv_free(void *p) { free(p); }
#endif
