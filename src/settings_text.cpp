#include "settings_text.h"
#include "config.h"             // SCREEN_W / SCREEN_H
#include "custom_settings.h"    // CUSTOM_HAS_SETTINGS / CUSTOM_SETTINGS_FONT
#include "theme_style.h"        // per-theme glow/glowColor — see theme_style.h
#include <math.h>
#include <string.h>
#include <stdio.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <stdlib.h>
#endif

namespace {

lv_obj_t   *s_canvas = nullptr;
lv_color_t *s_buf    = nullptr;

// Straight-line glyph blit + glow — same technique as menu_text.cpp (itself
// copied from radar_view.cpp's selection-banner renderer): reads a 4-bpp
// glyph alpha bitmap (lv_font_conv --bpp 4 --no-compress layout, same as the
// LVGL builtin fonts) and blends it into this module's own RGB565+alpha
// canvas buffer, centered rather than left-aligned (every wheel item is
// centered on its own x).
inline float glyph_alpha4(const uint8_t *bmp, int bw, int x, int y) {
    const int bit = (y * bw + x) * 4;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t nib = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17.0f;
}
void blit_glyph(const uint8_t *bmp, int bw, int bh, float destCx, float destCy,
                lv_color_t col, lv_opa_t maxOpa) {
    if (!bmp || bw <= 0 || bh <= 0 || !s_buf) return;
    uint8_t *buf = (uint8_t *)s_buf;
    const float reach = sqrtf((bw * 0.5f) * (bw * 0.5f) + (bh * 0.5f) * (bh * 0.5f)) + 1.0f;
    const int x0 = (int)fmaxf(0.0f, destCx - reach), x1 = (int)fminf((float)SCREEN_W - 1, destCx + reach);
    const int y0 = (int)fmaxf(0.0f, destCy - reach), y1 = (int)fminf((float)SCREEN_H - 1, destCy + reach);
    const float pivotX = bw * 0.5f, pivotY = bh * 0.5f;
    for (int dy = y0; dy <= y1; ++dy) {
        const float sy = dy - destCy + pivotY;
        for (int dx = x0; dx <= x1; ++dx) {
            const float sx = dx - destCx + pivotX;
            const int ix = (int)floorf(sx), iy = (int)floorf(sy);
            if (ix < -1 || iy < -1 || ix >= bw || iy >= bh) continue;
            const float fx = sx - ix, fy = sy - iy;
            const float a00 = (ix >= 0 && iy >= 0 && ix < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix, iy) : 0.0f;
            const float a10 = (ix + 1 >= 0 && iy >= 0 && ix + 1 < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy) : 0.0f;
            const float a01 = (ix >= 0 && iy + 1 >= 0 && ix < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix, iy + 1) : 0.0f;
            const float a11 = (ix + 1 >= 0 && iy + 1 >= 0 && ix + 1 < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy + 1) : 0.0f;
            float a = a00 * (1 - fx) * (1 - fy) + a10 * fx * (1 - fy) + a01 * (1 - fx) * fy + a11 * fx * fy;
            a = a * (float)maxOpa / 255.0f;
            if (a < 8.0f) continue;
            const int px = (dy * SCREEN_W + dx) * 3;
            if ((uint8_t)a <= buf[px + 2]) continue;
            buf[px] = (uint8_t)(col.full & 0xFF);
            buf[px + 1] = (uint8_t)(col.full >> 8);
            buf[px + 2] = (uint8_t)a;
        }
    }
}
void blit_glyph_glow(const uint8_t *bmp, int bw, int bh, float destCx, float destCy,
                     lv_color_t glowCol, int glow, lv_opa_t itemOpa) {
    if (glow <= 0) return;
    static const float dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f} };
    const int rings = 3;
    for (int ri = 1; ri <= rings; ++ri) {
        const int r = (int)lroundf((float)glow * ri / rings);
        if (r <= 0) continue;
        const lv_opa_t opa = (lv_opa_t)((90 / ri) * itemOpa / 255);
        for (int di = 0; di < 8; ++di)
            blit_glyph(bmp, bw, bh, destCx + dirs[di][0] * r, destCy + dirs[di][1] * r, glowCol, opa);
    }
}

} // namespace

namespace settings_text {

// See the note in menu_text.cpp: this ~868 KB PSRAM canvas used to be allocated at boot
// and never released, alongside an identical one for the app-switcher menu. Two screens
// that are on-screen for seconds at a time were holding ~1.7 MB permanently, which is
// what made the second allocation fail once theme art got detailed.
lv_obj_t *s_parent = nullptr;

void init(lv_obj_t *parent) {
#if CUSTOM_HAS_SETTINGS
    s_parent = parent;
#else
    (void)parent;
#endif
}

void acquire() {
#if CUSTOM_HAS_SETTINGS
    lv_obj_t *parent = s_parent;
    if (s_canvas || !parent) return;
    // The ONLY thing this canvas buys over plain LVGL labels is glow. If the active
    // theme asks for none, do not allocate at all: available() then returns false and
    // wheel_layout() keeps the ordinary labels, which cost a few KB instead of ~868.
    if (theme_style::settings().glow <= 0) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
    s_buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    s_buf = (lv_color_t *)malloc(sz);
#endif
    if (!s_buf) {
        // Same silent failure menu_text had: with no canvas, draw_item() below does
        // nothing, wheel_layout() has already forced every label transparent, and
        // Settings renders as a bare highlight strip with no readable text at all.
#ifdef ARDUINO
        Serial.printf("[settings_text] canvas alloc FAILED (%u bytes) - falling back to plain labels\n", (unsigned)sz);
#else
        printf("[settings_text] canvas alloc FAILED (%u bytes) - falling back to plain labels\n", (unsigned)sz);
#endif
        return;
    }
    s_canvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_center(s_canvas);
#endif
}

void release() {
#if CUSTOM_HAS_SETTINGS
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_buf) {
#if defined(ESP_PLATFORM)
        heap_caps_free(s_buf);
#else
        free(s_buf);
#endif
        s_buf = nullptr;
    }
#endif
}

void begin_frame() {
#if CUSTOM_HAS_SETTINGS
    if (!s_canvas) return;
    // Same fix as menu_text: lv_canvas_fill_bg writes through LVGL's per-pixel API and
    // was measured at 143 ms to blank a 651 KB buffer. TRUE_COLOR_ALPHA's "transparent
    // black" is all-zero bytes, so a plain wipe is identical output at memory speed.
    // This runs on every knob turn inside Settings, so it was costing the same there.
    memset(s_buf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H));
    lv_obj_invalidate(s_canvas);   // fill_bg used to mark it dirty; do that ourselves now
#endif
}

void draw_item(const char *str, float x, float y, lv_color_t color, lv_opa_t opa) {
#if CUSTOM_HAS_SETTINGS
    if (!s_canvas || !str || !str[0] || opa == 0) return;
    const lv_font_t *font = CUSTOM_SETTINGS_FONT;
    const int n = (int)strlen(str), cap = n < 40 ? n : 40;
    float w[40], total = 0.0f;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        w[i] = lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0) ? (float)g.adv_w : 0.0f;
        total += w[i];
    }
    const float startX = x - total / 2.0f;
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;
    const lv_color_t glowCol = lv_color_hex(theme_style::settings().glowColor);
    float cx = startX;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
            if (bmp && g.box_w && g.box_h) {
                const float destCx = cx + (float)g.ofs_x + (float)g.box_w * 0.5f;
                const float destCy = y + halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
                blit_glyph_glow(bmp, g.box_w, g.box_h, destCx, destCy, glowCol, theme_style::settings().glow, opa);
                blit_glyph(bmp, g.box_w, g.box_h, destCx, destCy, color, opa);
            }
        }
        cx += w[i];
    }
    lv_obj_invalidate(s_canvas);
#else
    (void)str; (void)x; (void)y; (void)color; (void)opa;
#endif
}

bool available() { return s_canvas != nullptr; }

} // namespace settings_text
