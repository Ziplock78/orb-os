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
#include <stdint.h>     // uint32_t / uintptr_t for the allocator counters below
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
// Direct FlightAware testing: 256 bytes keeps roughly 25 KB of internal RAM
// free with Steam Punk loaded. TLS buffers use a separate PSRAM allocator;
// the display draw buffer remains in internal DMA RAM. Radar measured 10-13 FPS.
#ifndef ORB_LV_BIG_ALLOC
#define ORB_LV_BIG_ALLOC 256
#endif

// --- what this allocator is holding, and where ------------------------------------
//
// Added 2026-10-06, because the threshold above has now been moved twice on a measurement
// of the SYMPTOM (internal free / largest block) and never once on a measurement of the
// CAUSE (what is actually in internal RAM). A themed Orb measures 2.3 KB free with a
// 628-byte largest block, 50x worse than the 67 KB / 31.7 KB this file calls healthy, and
// the honest answer to "is that LVGL?" was a shrug.
//
// Compile the whole thing in with -DORB_LV_STATS=1 if needed for debugging, deployments should
// leave this at 0.
// it costs a bucket index and a few increments per allocation, and one
// heap_caps_get_allocated_size() call per free.
#ifndef ORB_LV_STATS
#define ORB_LV_STATS 0
#endif

#define ORB_LV_NBUCKETS 8

#if ORB_LV_STATS
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t orb_lv_hist_count[ORB_LV_NBUCKETS];
extern uint32_t orb_lv_hist_bytes[ORB_LV_NBUCKETS];
extern uint32_t orb_lv_live_int_bytes, orb_lv_live_ext_bytes;
extern uint32_t orb_lv_live_int_count, orb_lv_live_ext_count;
extern uint32_t orb_lv_peak_int_bytes;
extern uint32_t orb_lv_fallback_to_int, orb_lv_fallback_to_ext;
#ifdef __cplusplus
}
#endif

// Bucket 0 is everything under 64 B; each one after covers twice the previous.
static inline int orb_lv_bucket(size_t n) {
    int b = 0;
    size_t edge = 64;
    while (b < ORB_LV_NBUCKETS - 1 && n >= edge) { edge <<= 1; ++b; }
    return b;
}

// Which pool a pointer actually came out of. The ESP32-S3 maps PSRAM into a known data
// range, which is a cheaper and more portable-across-IDF-versions test than reaching for
// esp_ptr_external_ram() — that header has moved between IDF releases and this one has to
// compile from LVGL's C as well as from our C++.
static inline int orb_lv_is_ext(const void *p) {
    const uintptr_t a = (uintptr_t)p;
    return a >= 0x3C000000u && a < 0x3E000000u;
}

// `size` is the REQUESTED size; what the heap actually set aside is a few bytes more.
// Record the allocated size, because that is what note_free() can recover later and the
// two have to agree or the running total drifts.
//
// It drifted. The first capture showed held-bytes falling by ~60 B every 15 s with the
// allocation COUNT dead flat at 1032 — alloc was booking the request and free was
// unbooking the rounded-up block, so every alloc/free pair silently lost the difference.
// Counts were always right; the byte totals were slowly under-reporting. Booking the same
// number on both sides fixes it, and makes the bucket edges exact on both sides too.
static inline void orb_lv_note_alloc(void *p, size_t requested) {
    if (!p) return;
    (void)requested;
    const size_t size = heap_caps_get_allocated_size(p);
    if (orb_lv_is_ext(p)) {
        orb_lv_live_ext_bytes += (uint32_t)size;
        ++orb_lv_live_ext_count;
    } else {
        const int b = orb_lv_bucket(size);
        ++orb_lv_hist_count[b];
        orb_lv_hist_bytes[b] += (uint32_t)size;
        orb_lv_live_int_bytes += (uint32_t)size;
        ++orb_lv_live_int_count;
        if (orb_lv_live_int_bytes > orb_lv_peak_int_bytes)
            orb_lv_peak_int_bytes = orb_lv_live_int_bytes;
    }
}

// The size is recovered from the heap rather than remembered, so nothing here needs a side
// table. Both sides book heap_caps_get_allocated_size(), so an allocation lands in and
// leaves the same bucket and the totals do not drift (see the note over note_alloc).
//
// The one thing to keep in mind reading the histogram: the bands are therefore ALLOCATED
// sizes, a little above what LVGL asked for, so a band boundary is a few bytes generous.
// It does not change which side of ORB_LV_BIG_ALLOC anything falls on, because that
// threshold is applied to the request in orb_lv_malloc before any of this runs.
static inline void orb_lv_note_free(void *p) {
    if (!p) return;
    const size_t size = heap_caps_get_allocated_size(p);
    if (orb_lv_is_ext(p)) {
        if (orb_lv_live_ext_bytes >= size) orb_lv_live_ext_bytes -= (uint32_t)size;
        if (orb_lv_live_ext_count)         --orb_lv_live_ext_count;
    } else {
        const int b = orb_lv_bucket(size);
        if (orb_lv_hist_count[b])          --orb_lv_hist_count[b];
        if (orb_lv_hist_bytes[b] >= size)  orb_lv_hist_bytes[b] -= (uint32_t)size;
        if (orb_lv_live_int_bytes >= size) orb_lv_live_int_bytes -= (uint32_t)size;
        if (orb_lv_live_int_count)         --orb_lv_live_int_count;
    }
}
#else
#define orb_lv_note_alloc(p, size) ((void)0)
#define orb_lv_note_free(p)        ((void)0)
// Readable as zero when the census is compiled out, so /health and the [lvmem] prints do
// not have to be wrapped in #if at every use site. Without these, -DORB_LV_STATS=0 breaks
// the build in three other files, which is a poor reward for taking an option the comment
// above offers.
#define orb_lv_hist_count          ((const uint32_t *)orb_lv_zero_bucket)
#define orb_lv_hist_bytes          ((const uint32_t *)orb_lv_zero_bucket)
#define orb_lv_live_int_bytes      0u
#define orb_lv_live_ext_bytes      0u
#define orb_lv_live_int_count      0u
#define orb_lv_live_ext_count      0u
#define orb_lv_peak_int_bytes      0u
#define orb_lv_fallback_to_int     0u
#define orb_lv_fallback_to_ext     0u
static const uint32_t orb_lv_zero_bucket[ORB_LV_NBUCKETS] = { 0 };
#endif  // ORB_LV_STATS

// Leave internal headroom for newlib file locks and TCP while Settings builds
// its many small objects. Waiting until internal malloc fails is too late:
// newlib aborts rather than returning an error when its lock allocation fails.
#define ORB_LV_INTERNAL_RESERVE (16u * 1024u)
static inline int orb_lv_prefer_external(size_t size) {
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return size >= ORB_LV_BIG_ALLOC || available < ORB_LV_INTERNAL_RESERVE ||
           size > available - ORB_LV_INTERNAL_RESERVE;
}
static inline void *orb_lv_malloc(size_t size) {
    void *p;
    if (orb_lv_prefer_external(size)) {
        p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if ORB_LV_STATS
            if (p) ++orb_lv_fallback_to_int;
#endif
        }
    } else {
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) {
            p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if ORB_LV_STATS
            if (p) ++orb_lv_fallback_to_ext;
#endif
        }
    }
    orb_lv_note_alloc(p, size);
    return p;
}
static inline void *orb_lv_realloc(void *p, size_t size) {
    // heap_caps_realloc honours the requested caps and copies across regions when the
    // block has to move, so a small buffer growing past the threshold migrates to PSRAM.
    //
    // The old block is accounted for BEFORE the call: realloc may free it, after which the
    // pointer cannot be asked how big it was.
    orb_lv_note_free(p);
    void *q;
    if (orb_lv_prefer_external(size)) {
        q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!q) q = heap_caps_realloc(p, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    } else {
        q = heap_caps_realloc(p, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!q) q = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    // A failed realloc leaves the original block alive, so put it back on the books.
    // p may be NULL here (realloc(NULL, n) is a plain malloc), and asking the heap the
    // size of a null pointer is not a question with an answer.
    if (!q) { if (p) orb_lv_note_alloc(p, 0); }   // note_alloc asks the heap itself now
    else    orb_lv_note_alloc(q, size);
    return q;
}
static inline void orb_lv_free(void *p) { orb_lv_note_free(p); heap_caps_free(p); }

#else   // desktop simulator: plain libc, no PSRAM to speak of
static inline void *orb_lv_malloc(size_t size) { return malloc(size); }
static inline void *orb_lv_realloc(void *p, size_t size) { return realloc(p, size); }
static inline void orb_lv_free(void *p) { free(p); }
#endif
