#pragma once
#include <lvgl.h>

// Inter, compiled in, for the three splash lines that cannot be rasterised.
//
// Every other piece of text on the Orb is baked by Orb Studio in whatever face the designer
// chose, and the live ones load that face off the SD card at runtime (theme_font.cpp). These
// three cannot do either: the firmware version, the config address and the data credits are
// not known at bake time, so they are drawn live — and there is no splash slot on the card
// for them to load a face from, so they fell through to whatever was compiled in. What was
// compiled in is LVGL's stock Montserrat, which nobody ever chose. On a device wearing a
// theme set in Inter, those three lines were the only Montserrat anybody actually saw.
//
// A splash font SLOT was the obvious alternative and was checked before this was built: it
// is cheaper in flash and it would have been dormant. No theme on the card ships a splash
// font, because Studio does not export one, so the slot would have loaded nothing and the
// lines would have stayed Montserrat until the Studio half shipped. Compiled is what changes
// what is on the glass today.
//
// FIVE SIZES, not the full fourteen-rung ladder. 12 and 14 are what the three lines default
// to; the rungs above are headroom for a design that wants them larger. It stops at 20
// because the lines sit at y=353, 385 and 419 — 32 px apart — so a face much beyond that
// overlaps its neighbours whatever font it is in, and compiling sizes that cannot be laid
// out would be paying flash for something unusable.
LV_FONT_DECLARE(font_inter_12)
LV_FONT_DECLARE(font_inter_14)
LV_FONT_DECLARE(font_inter_16)
LV_FONT_DECLARE(font_inter_18)
LV_FONT_DECLARE(font_inter_20)

// Nearest rung at or below the asked-for size, and NEVER a fall back to Montserrat.
//
// This is the one place that deliberately differs from font_ladder(), which returns 16 for
// an unknown size so a design asking for 30 is not silently redrawn at 28. Here the whole
// point is that these lines are Inter, so a size this binary does not carry is served in the
// nearest Inter rather than being served exactly in the wrong typeface. Getting the size
// approximately right and the face exactly right is the correct trade for three footer
// lines whose sizes are bounded by the layout anyway.
inline const lv_font_t *splash_font(int size) {
    if (size >= 20) return &font_inter_20;
    if (size >= 18) return &font_inter_18;
    if (size >= 16) return &font_inter_16;
    if (size >= 14) return &font_inter_14;
    return &font_inter_12;
}
