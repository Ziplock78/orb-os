// Decodes the Launch Kit "custom" clock layers (PNG bytes embedded in
// custom_plate.h / custom_overlay.h / custom_hands.h) once into PSRAM buffers the
// FACE_CUSTOM compositor blits/rotates. See custom_sprite.cpp.
#pragma once
#include <stdint.h>

struct CustomSprite { const uint8_t *data; int w, h; };  // RGB565+alpha, 3 bytes/px (lo,hi,alpha)

const uint16_t *custom_plate();     // 466x466 RGB565 opaque background, or nullptr
const uint8_t  *custom_overlay();   // 466x466 RGB565+alpha (3 B/px) over-hands layer, or nullptr
// The SPLASH's glass. Deliberately a different file from custom_overlay(): that one is the
// clock's, and Studio bakes the hand-pivot hub into it. See the note on the definition.
const uint8_t  *splash_overlay();   // 466x466 RGB565+alpha, or nullptr if the theme ships none
CustomSprite    custom_hand(int kind);  // kind 0=hour,1=minute,2=second,3=static1,4=static2; {nullptr,0,0} if absent
// The pre-blurred, pre-coloured silhouette a hand casts, or {nullptr,0,0} if the theme
// ships none. Same pivot and size as its hand, so it rotates identically; the offset that
// makes the light look fixed is applied to the centre by the caller.
CustomSprite    custom_shadow(int hand);  // hand 0=hour, 1=minute, 2=second
// The wind screen's two, THEME_CAPS 44. Both carry alpha, so both come back in the same
// 3-bytes-per-pixel shape a hand does, which is also what LVGL rotates natively.
const uint16_t *wind_background(int &w, int &h);   // wind_bg.png, OPAQUE RGB565: the wind screen covers the clock
CustomSprite    wind_crank();        // wind_crank.png, turned by the knob about its own pivot
CustomSprite    wind_crank_shadow(); // wind_crank_shadow.png, same size and pivot as the crank
void            custom_sprite_release();  // free all decoded PSRAM buffers; next call re-decodes
