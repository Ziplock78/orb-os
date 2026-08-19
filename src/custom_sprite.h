// Decodes the Launch Kit "custom" clock layers (PNG bytes embedded in
// custom_plate.h / custom_overlay.h / custom_hands.h) once into PSRAM buffers the
// FACE_CUSTOM compositor blits/rotates. See custom_sprite.cpp.
#pragma once
#include <stdint.h>

struct CustomSprite { const uint8_t *data; int w, h; };  // RGB565+alpha, 3 bytes/px (lo,hi,alpha)

const uint16_t *custom_plate();     // 466x466 RGB565 opaque background, or nullptr
const uint8_t  *custom_overlay();   // 466x466 RGB565+alpha (3 B/px) over-hands layer, or nullptr
CustomSprite    custom_hand(int kind);  // kind 0=hour,1=minute,2=second,3=static1,4=static2; {nullptr,0,0} if absent
// The pre-blurred, pre-coloured silhouette a hand casts, or {nullptr,0,0} if the theme
// ships none. Same pivot and size as its hand, so it rotates identically; the offset that
// makes the light look fixed is applied to the centre by the caller.
CustomSprite    custom_shadow(int hand);  // hand 0=hour, 1=minute, 2=second
void            custom_sprite_release();  // free all decoded PSRAM buffers; next call re-decodes
