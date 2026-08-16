// Decodes the Launch Kit "custom" Settings layers (PNG bytes embedded in
// custom_settings_plate.h / custom_settings_overlay.h) once into PSRAM buffers
// — same pattern as radar_sprite.h/menu_sprite.h. See settings_sprite.cpp.
#pragma once
#include <lvgl.h>

const lv_img_dsc_t *settings_custom_plate();     // 466x466 opaque RGB565 background, or nullptr
const lv_img_dsc_t *settings_custom_overlay();   // 466x466 RGB565+alpha CRT+glass layer, or nullptr
void settings_sprite_release();                  // free decoded PSRAM buffers; next call re-decodes
