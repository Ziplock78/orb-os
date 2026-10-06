// Counters for the LVGL heap wrapper in include/lv_psram_alloc.h.
//
// They live in their own translation unit because that header is included by LVGL's
// lv_mem.c, which is C, so a C++17 `inline` variable is not available and a `static` one
// would give every includer its own private copy and count nothing.
//
// Why these exist: the Orb runs out of INTERNAL RAM, not PSRAM, and the measured state on
// a themed device is 2.3 KB free with a 628-byte largest block — against the 67 KB / 31.7 KB
// that lv_psram_alloc.h records as healthy. The boot [intram] marks in main.cpp say WHEN
// internal RAM goes; they cannot say WHAT holds it. ORB_LV_BIG_ALLOC keeps every LVGL
// allocation under 2 KB in internal RAM by design, which makes LVGL the first suspect, but
// suspicion is not a measurement and the threshold has already been moved twice on one.
//
// So: how many bytes does the LVGL allocator hold in internal RAM right now, and at what
// sizes. Against heap_caps_get_info's total that is the whole question — if LVGL holds most
// of the internal heap the threshold is the fix, and the histogram says where to put it; if
// it does not, the search moves somewhere else and nobody has spent a day on the wrong pool.
#include <stdint.h>
#include "lv_psram_alloc.h"   // the bucket count, so the two cannot drift apart

// Live allocations, by requested size. Bucket 0 is everything under 64 bytes and each
// bucket after it covers twice the span of the one before, so the eight read:
//   0:<64  1:<128  2:<256  3:<512  4:<1024  5:<2048  6:<4096  7:>=4096.
// Only INTERNAL-RAM allocations are counted here: PSRAM ones are not the scarce thing and
// a histogram of them would not change any decision.
uint32_t orb_lv_hist_count[ORB_LV_NBUCKETS];
uint32_t orb_lv_hist_bytes[ORB_LV_NBUCKETS];

// Totals, both regions, so "LVGL holds X of the internal heap" is answerable directly.
uint32_t orb_lv_live_int_bytes;
uint32_t orb_lv_live_ext_bytes;
uint32_t orb_lv_live_int_count;
uint32_t orb_lv_live_ext_count;

// Peak internal held, since the live number at the moment you ask is not the number that
// decided whether something failed an hour ago.
uint32_t orb_lv_peak_int_bytes;

// Times an allocation could not be served from its intended pool and fell back to the
// other. A non-zero int_fallback means PSRAM refused and the scarce pool absorbed it,
// which is the shape of the font boot-loop this wrapper was written for.
uint32_t orb_lv_fallback_to_int;
uint32_t orb_lv_fallback_to_ext;
