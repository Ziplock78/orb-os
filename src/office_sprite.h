#pragma once
// Decodes the Office clock's hand sprites (real photo/render crops, not procedural
// drawing — see tools/bake_alpha_sprite.py) into PSRAM LV_IMG_CF_TRUE_COLOR_ALPHA
// buffers, once each, for hand-rolled rotate+blend (see draw_office_minute_sprite() /
// draw_office_hour_sprite() in clock_view.cpp — not lv_img_set_angle() directly; see
// that file for why). The minute hand's glow is baked into the same image as the hand
// itself so they can never drift apart as they rotate — see office_minute_img_meta.h /
// office_hour_img_meta.h for each sprite's pivot and baseline angle.
#include <lvgl.h>

// Each returns a stable pointer to its decoded sprite (cached after the first
// successful call), or nullptr on decode failure.
const lv_img_dsc_t *office_minute_sprite();
const lv_img_dsc_t *office_hour_sprite();
