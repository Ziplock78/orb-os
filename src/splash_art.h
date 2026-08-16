#pragma once
// Decodes the boot-splash artwork into a PSRAM RGB565 buffer at the moment it's
// needed. Tries, in order: a theme splash on the microSD card (/theme/splash.png —
// see the SD-hosted-theme comment in splash_art.cpp for why it's one fixed path
// for now, not yet per-theme folders), then a flash-baked Launch Kit push
// (CUSTOM_HAS_SPLASH), then the stock office/default art baked at
// splash_png_default.h / splash_png_office.h. A raw baked RGB565 array costs
// ~217KB of flash per 466x466 image regardless of content; PNG-decoding on
// demand (the same approach wx_radar_client.cpp already uses for weather tiles)
// is what makes the flash-baked fallback affordable at all.
#include <lvgl.h>

// Decodes into an internal PSRAM buffer (reused across calls — the result is only valid
// until the next call) and fills `out` to point at it. Returns false on decode failure.
bool splash_art_decode(bool office, lv_img_dsc_t *out);
