// Decodes the Launch Kit "custom" Menu (app-switcher overlay) layers (PNG bytes
// embedded in custom_menu_plate.h / custom_menu_overlay.h) once into PSRAM
// buffers — same pattern as radar_sprite.h, kept as its own module since the
// two screens' baked PNGs are separate blobs. See menu_sprite.cpp.
#pragma once
#include <lvgl.h>

const lv_img_dsc_t *menu_custom_plate();     // 466x466 opaque RGB565 background, or nullptr
const lv_img_dsc_t *menu_custom_overlay();   // 466x466 RGB565+alpha CRT+glass layer, or nullptr
void menu_sprite_release();                  // free decoded PSRAM buffers; next call re-decodes
