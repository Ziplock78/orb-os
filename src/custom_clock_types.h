// Committed schema for a Launch Kit "custom" clock. The visual layers (plate,
// overlay, hand sprites) are the editor's own rendered pixels, shipped as PNGs in
// custom_plate.h / custom_overlay.h / custom_hands.h. This descriptor carries only
// what the firmware still draws itself: the two live text banners and a fallback
// background color. Kept in sync with the generator in the Launch Kit server
// (server.js customClockHeader).
#pragma once
#include <stdint.h>

// A text banner (time or date), drawn centred at (x,y). fmt is a strftime() string
// the push already translated from the editor tokens (e.g. "hh:mm" -> "%I:%M").
struct CustomText {
    bool     show;
    int      x, y;        // centre, px (466x466 canvas, centre 233,233)
    int      font;        // nearest built-in Montserrat px: 12/14/16/18/20/28/48
    uint32_t color;       // 0xRRGGBB
    char     fmt[24];     // strftime format
};

struct CustomClockDesign {
    bool     active;      // false = FACE_CUSTOM is ignored entirely
    uint32_t bg;          // fallback solid background if there is no plate
    CustomText text1, text2;   // Montserrat fallback used only when a banner has no atlas
};
