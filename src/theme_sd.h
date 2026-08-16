#pragma once
#include <stddef.h>
#include <stdint.h>

// Shared "read a whole theme-art file into PSRAM" helper for every screen
// that can source its art from the SD card instead of a flash-baked PNG
// array (splash_art.cpp, custom_sprite.cpp, radar/menu/settings_sprite.cpp).
// Portable the same way roads_sd.cpp is: real SD.open() on the device, a
// local stand-in directory (sim/sdcard, same root roads_sd::set_root()
// points the desktop simulator at) when built for the native/desktop target
// — so "Preview in simulator" exercises this exact code path too.
//
// One fixed path per asset for now (e.g. "/theme/clock_plate.png"), not yet
// per-theme-slug folders — see the comment above SD_SPLASH_PATH in
// splash_art.cpp for why (there's no on-device theme *selection* yet to
// route to a slug). Read failures of any kind (no card, missing file, over
// the size ceiling, short read) return nullptr — every caller already has a
// flash-baked/stock fallback, so this never needs to be fatal.
namespace theme_sd {

// Returns a freshly-allocated PSRAM (device) / heap (sim) buffer, or nullptr
// on any failure. Caller must pass the result to free() when done with it —
// this only ever holds the raw compressed bytes briefly, for a decode call,
// never the decoded pixels.
uint8_t *read_whole(const char *path, size_t &outLen, size_t maxBytes);
void free(uint8_t *buf);

} // namespace theme_sd
