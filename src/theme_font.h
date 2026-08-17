#pragma once
#include <lvgl.h>

// Per-theme typography: fonts that travel with a theme instead of being compiled in.
//
// Fonts were the last welded thing. Everything else a theme owns — artwork, colours,
// positions, hand geometry, the app roster, display names — travels as data on the SD
// card and reaches the device over WiFi. Fonts were real compiled LVGL glyph bitmaps
// baked into the firmware binary, which had two consequences, both of which cost real
// time this week:
//
//   1. Only ONE theme's fonts existed at a time, whichever was flashed last. Switching
//      themes in Settings > Design gave you the new theme's artwork wearing the previous
//      theme's typography, with nothing to explain it. That produced a genuine
//      Modern-fonts-over-Steam-Punk hybrid on the bench.
//   2. Every theme push had to rebuild and reflash the firmware over USB (~48 s and a
//      cable), even when the only change was a background image, because Launch Kit had
//      no way to know whether a font had moved.
//
// Launch Kit now emits each font as an lv_font_conv binary and ships it like any other
// theme asset. The device stores it in the `themeart` flash partition (theme_art.h) and
// loads it through an lv_fs driver that reads straight out of the memory map, so no card
// access is involved at draw time.
//
// Everything degrades to the old behaviour: a theme that ships no font, a font that fails
// to load, or a device with no themeart partition all fall back to the compiled
// CUSTOM_*_FONT the firmware was built with. A missing font must never mean no text.
namespace theme_font {

// Register the lv_fs driver and load whatever fonts the active theme ships. Call once at
// boot, after theme_art::begin() (it reads from the mapped partition) and before any view
// asks for a font. Safe to call twice.
void begin();

// One accessor per text slot. Each returns the theme's font when it loaded, and the
// compiled fallback otherwise, so callers never need a null check or a fallback of their
// own — which is what keeps this from leaking into every renderer.
const lv_font_t *clock_text1();
const lv_font_t *clock_text2();
const lv_font_t *menu_current();
const lv_font_t *menu_prev();
const lv_font_t *menu_next();
const lv_font_t *settings_item();
const lv_font_t *radar_text(int idx);      // idx 0..3, clamped

// How many of this theme's fonts actually loaded from flash. 0 means everything is
// running on compiled fallbacks, which is the honest "nothing changed yet" state rather
// than a failure.
int loaded_count();

} // namespace theme_font
