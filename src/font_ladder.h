#pragma once
#include <lvgl.h>

// The sizes this binary actually contains.
//
// LVGL fonts are compiled glyph bitmaps, not scalable outlines, so a size not linked into
// the firmware cannot be drawn at any quality: there are no glyphs to draw. Every screen
// that offers a size control therefore offers exactly this ladder and nothing between the
// rungs, and Studio's slider walks the same list.
//
// Shared rather than copied. intel_view.cpp grew the original; the splash is the second
// screen to want it, which by this repo's own rule (see curved_text.cpp, extracted on the
// third copy) is one copy too early to start duplicating.
//
// Unknown sizes fall back to 16 rather than to the nearest rung. Nearest sounds friendlier
// and quietly redesigns the theme: a design asking for 30 would silently become 28 or 32
// and the person who wrote 30 would never be told.
inline const lv_font_t *font_ladder(int size) {
    switch (size) {
        case 12: return &lv_font_montserrat_12;
        case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16;
        case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24;
        case 26: return &lv_font_montserrat_26;
        case 28: return &lv_font_montserrat_28;
        case 32: return &lv_font_montserrat_32;
        case 36: return &lv_font_montserrat_36;
        case 40: return &lv_font_montserrat_40;
        case 44: return &lv_font_montserrat_44;
        case 48: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_16;
    }
}
