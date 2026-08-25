#pragma once
#include <lvgl.h>

// The Headlines screen's own art: a full-dial background plate and the shared glass/CRT
// overlay. Same three-rung source order every other screen uses (pre-baked flash, then the
// SD PNG, then nothing), and the same reason for it: a plate decoded from the card costs
// ~430 ms and ~424 KB of PSRAM every time the screen is shown, and a plate already baked
// into the themeart partition costs nothing at all. See theme_art.h.
//
// Unlike the clock and the menu there is no compiled-in fallback: this screen was colour
// only until THEME_CAPS 14, so no firmware ever baked a CUSTOM_INTEL_* PNG into itself.
// A theme that ships neither file simply gets the flat background colour it always had,
// which is what makes this addition free for every design made before it.
namespace intelview {

const lv_img_dsc_t *plate();     // 466x466 opaque RGB565, or nullptr
const lv_img_dsc_t *overlay();   // 466x466 RGB565+alpha glass/CRT, or nullptr

// Give the decoded buffers back. Called when the screen is left, so a theme's artwork is
// not held while another app is on the dial.
void sprite_release();

}  // namespace intelview
