#include "menu_text.h"
#include "config.h"          // SCREEN_W / SCREEN_H
#include "custom_menu.h"     // CUSTOM_HAS_MENU* (compile-time show/hide gates) + CUSTOM_MENU_*_FONT
#include "theme_style.h"     // per-theme position/color/glow/format/align — see theme_style.h
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

// Substitute every "{name}" in fmt with name, into a fixed buffer — mirrors
// radar_view.cpp's radar_fmt token substitution, single token here.
void format_name(const char *fmt, const char *name, char *out, size_t outSz) {
    if (!fmt) { out[0] = 0; return; }
    const char *n = name ? name : "";
    size_t w = 0;
    for (const char *p = fmt; *p && w + 1 < outSz; ) {
        if (!strncmp(p, "{name}", 6)) {
            for (const char *q = n; *q && w + 1 < outSz; ++q) out[w++] = *q;
            p += 6;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = 0;
}

// --- Straight-line glyph blit + glow, copied from radar_view.cpp's rtext_*
// functions (same technique clock_view.cpp's draw_baked_text uses): reads a
// 4-bpp glyph alpha bitmap (lv_font_conv --bpp 4 --no-compress layout, same as
// the LVGL builtin fonts) and blends it into this module's own RGB565+alpha
// canvas buffer. No curve needed here (the menu overlay is flat, not a scope),
// so only the straight-layout half of radar's renderer is needed.
inline float glyph_alpha4(const uint8_t *bmp, int bw, int x, int y) {
    const int bit = (y * bw + x) * 4;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t nib = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17.0f;
}
// overwrite=true means "this is the sharp text, it sits on top". Without it the
// max-alpha rule below let a saturated glow block the very letters it was drawn for:
// the glow reached alpha 255, the text also wanted 255, 255 is not greater than 255, so
// the text was skipped entirely and all you saw was a white fuzzy blob.
void blit_glyph(const uint8_t *bmp, int bw, int bh, float destCx, float destCy,
                lv_color_t col, lv_opa_t maxOpa, bool overwrite = false) {
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
            if (!overwrite && (uint8_t)a <= buf[px + 2]) continue;
            buf[px] = (uint8_t)(col.full & 0xFF);
            buf[px + 1] = (uint8_t)(col.full >> 8);
            buf[px + 2] = (uint8_t)a;
        }
    }
}
// ---- glow -------------------------------------------------------------------
// Glow used to be 24 offset copies of every glyph (3 rings x 8 directions), each one a
// full bilinear float resample. Measured cost: 358-829 ms for a single menu refresh,
// which is what made every knob detent take about a second.
//
// It is now drawn the way blurs are actually done: rasterise the whole string ONCE into
// a one-byte-per-pixel alpha stencil, blur that stencil, then paint the blurred shape in
// the glow colour. Two properties make it cheap:
//
//   - Separable. Blurring horizontally then vertically is mathematically the same as a
//     2D blur but costs radius+radius lookups per pixel instead of radius squared.
//   - Running sum. Sliding the window one pixel right means adding the pixel entering
//     and subtracting the one leaving: two operations, no matter how wide the blur.
//
// So the cost no longer depends on the glow radius at all. A glow of 40 costs what a
// glow of 4 costs, which turns glow back into a free design choice.
// Ceiling on how opaque the halo may get. The old stamped version peaked around 90 of
// 255 (rings at opacity 90/45/30), which is the "aged, bleeding into the page" density
// worth preserving. Left uncapped, a blurred stroke centre reaches full opacity and the
// glow stops being a glow and becomes a blob. Raise for a heavier bleed, lower for a
// subtler one: this is the one number to tune by eye.
constexpr int GLOW_MAX_ALPHA = 115;

struct Stencil {
    uint8_t *a = nullptr;          // alpha only, 1 byte per pixel
    int x0 = 0, y0 = 0, w = 0, h = 0;   // placement in canvas coordinates
};

Stencil s_sten;

bool stencil_alloc(int x0, int y0, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    const size_t bytes = (size_t)w * h;
#if defined(ESP_PLATFORM)
    s_sten.a = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
    s_sten.a = (uint8_t *)malloc(bytes);
#endif
    if (!s_sten.a) return false;
    memset(s_sten.a, 0, bytes);
    s_sten.x0 = x0; s_sten.y0 = y0; s_sten.w = w; s_sten.h = h;
    return true;
}

void stencil_free() {
    if (!s_sten.a) return;
#if defined(ESP_PLATFORM)
    heap_caps_free(s_sten.a);
#else
    free(s_sten.a);
#endif
    s_sten.a = nullptr; s_sten.w = s_sten.h = 0;
}

// Stamp one glyph into the stencil. Integer placement on purpose: this is the blurred
// layer, so sub-pixel precision is invisible once it has been smeared, and dropping the
// bilinear filter here removes four lookups and ~10 float ops from every pixel. The
// sharp text on top still uses the filtered path, which is where precision shows.
void stencil_glyph(const uint8_t *bmp, int bw, int bh, float destCx, float destCy) {
    if (!s_sten.a || !bmp || bw <= 0 || bh <= 0) return;
    const int gx = (int)lroundf(destCx - bw * 0.5f) - s_sten.x0;
    const int gy = (int)lroundf(destCy - bh * 0.5f) - s_sten.y0;
    for (int sy = 0; sy < bh; ++sy) {
        const int dy = gy + sy;
        if (dy < 0 || dy >= s_sten.h) continue;
        uint8_t *row = s_sten.a + (size_t)dy * s_sten.w;
        for (int sx = 0; sx < bw; ++sx) {
            const int dx = gx + sx;
            if (dx < 0 || dx >= s_sten.w) continue;
            const uint8_t a = (uint8_t)glyph_alpha4(bmp, bw, sx, sy);
            if (a > row[dx]) row[dx] = a;
        }
    }
}

// One separable box-blur pass in each axis, using a sliding window sum so the per-pixel
// cost is constant regardless of `r`. Two passes approximate a Gaussian closely enough
// for a glow, and match the shape of the browser's shadowBlur that Launch Kit's editor
// has always previewed.
void stencil_blur(int r) {
    if (!s_sten.a || r <= 0) return;
    const int w = s_sten.w, h = s_sten.h;
    const int win = 2 * r + 1;
    uint8_t *tmp = nullptr;
#if defined(ESP_PLATFORM)
    tmp = (uint8_t *)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
#else
    tmp = (uint8_t *)malloc((size_t)w * h);
#endif
    if (!tmp) return;   // no scratch: leave the stencil sharp rather than dropping glow

    for (int y = 0; y < h; ++y) {            // horizontal
        const uint8_t *src = s_sten.a + (size_t)y * w;
        uint8_t *dst = tmp + (size_t)y * w;
        int sum = 0;
        for (int x = -r; x <= r; ++x) sum += src[x < 0 ? 0 : (x >= w ? w - 1 : x)];
        for (int x = 0; x < w; ++x) {
            dst[x] = (uint8_t)(sum / win);
            const int add = x + r + 1, sub = x - r;
            sum += src[add >= w ? w - 1 : add];
            sum -= src[sub < 0 ? 0 : sub];
        }
    }
    for (int x = 0; x < w; ++x) {            // vertical
        int sum = 0;
        for (int y = -r; y <= r; ++y) sum += tmp[(size_t)(y < 0 ? 0 : (y >= h ? h - 1 : y)) * w + x];
        for (int y = 0; y < h; ++y) {
            s_sten.a[(size_t)y * w + x] = (uint8_t)(sum / win);
            const int add = y + r + 1, sub = y - r;
            sum += tmp[(size_t)(add >= h ? h - 1 : add) * w + x];
            sum -= tmp[(size_t)(sub < 0 ? 0 : sub) * w + x];
        }
    }
#if defined(ESP_PLATFORM)
    heap_caps_free(tmp);
#else
    free(tmp);
#endif
}

// Paint the blurred stencil into the canvas in the glow colour.
void stencil_composite(lv_color_t glowCol, int strength) {
    if (!s_sten.a || !s_buf) return;
    uint8_t *buf = (uint8_t *)s_buf;
    const uint8_t lo = (uint8_t)(glowCol.full & 0xFF), hi = (uint8_t)(glowCol.full >> 8);
    for (int y = 0; y < s_sten.h; ++y) {
        const int dy = s_sten.y0 + y;
        if (dy < 0 || dy >= SCREEN_H) continue;
        const uint8_t *srow = s_sten.a + (size_t)y * s_sten.w;
        for (int x = 0; x < s_sten.w; ++x) {
            const int dx = s_sten.x0 + x;
            if (dx < 0 || dx >= SCREEN_W) continue;
            int a = srow[x] * strength / 255;
            if (a < 8) continue;
            if (a > GLOW_MAX_ALPHA) a = GLOW_MAX_ALPHA;
            const int px = (dy * SCREEN_W + dx) * 3;
            if ((uint8_t)a <= buf[px + 2]) continue;
            buf[px] = lo; buf[px + 1] = hi; buf[px + 2] = (uint8_t)a;
        }
    }
}
void draw_straight(const lv_font_t *font, const char *str, float bx, float by,
                   lv_color_t col, int glow, lv_color_t glowCol, int align) {
    if (!font || !str || !str[0]) return;
    const int n = (int)strlen(str), cap = n < 80 ? n : 80;
    float w[80], total = 0.0f;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        w[i] = lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0) ? (float)g.adv_w : 0.0f;
        total += w[i];
    }
    const float startX = (align == 1) ? (bx - total / 2.0f) : (align == 2) ? (bx - total) : bx;
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;
    // Glow first, for the WHOLE string at once: one stencil, one blur, one composite,
    // instead of 24 resampled copies per glyph. Then the sharp text goes on top.
    if (glow > 0) {
        const int pad = glow * 2 + 4;   // room for the blur to spread past the glyphs
        const int sx0 = (int)startX - pad;
        const int sy0 = (int)(by + halfMid - lineH) - pad;
        const int sw  = (int)total + 2 * pad;
        const int sh  = (int)lineH * 2 + 2 * pad;
        if (stencil_alloc(sx0, sy0, sw, sh)) {
            float gx = startX;
            for (int i = 0; i < cap; ++i) {
                lv_font_glyph_dsc_t g;
                if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) {
                    const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
                    if (bmp && g.box_w && g.box_h)
                        stencil_glyph(bmp, g.box_w, g.box_h,
                                      gx + (float)g.ofs_x + (float)g.box_w * 0.5f,
                                      by + halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f);
                }
                gx += w[i];
            }
            stencil_blur(glow);
            // No boost: a blurred stroke centre already reaches full alpha, and
            // GLOW_MAX_ALPHA is what keeps it from swamping the text.
            stencil_composite(glowCol, 255);
            stencil_free();
        }
    }

    float x = startX;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
            if (bmp && g.box_w && g.box_h) {
                const float destCx = x + (float)g.ofs_x + (float)g.box_w * 0.5f;
                const float destCy = by + halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
                blit_glyph(bmp, g.box_w, g.box_h, destCx, destCy, col, 255, true);
            }
        }
        x += w[i];
    }
}

} // namespace

namespace menu_text {

// The canvas is a ~868 KB PSRAM buffer, and it used to be allocated once at boot and
// held for the life of the device. Settings holds an identical one, so between them
// they claimed ~1.7 MB permanently for two screens that are each on-screen for a couple
// of seconds at a time. On a card with detailed theme art that was enough to push the
// second allocation over the edge, and it failed silently.
//
// Now it follows the same discipline as the theme art: acquire on show, release on
// hide. init() only remembers where the canvas should live.
lv_obj_t *s_parent = nullptr;

void init(lv_obj_t *parent) {
#if CUSTOM_HAS_MENU
    s_parent = parent;
#else
    (void)parent;
#endif
}

void acquire() {
#if CUSTOM_HAS_MENU
    lv_obj_t *parent = s_parent;
    if (s_canvas || !parent) return;
    // Same rule as settings_text: glow is the only reason this canvas exists, so a theme
    // that asks for none falls back to the plain label and costs nothing.
    const theme_style::Menu &mn = theme_style::menu();
    if (mn.current.glow <= 0 && mn.prev.glow <= 0 && mn.next.glow <= 0) return;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
    s_buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    s_buf = (lv_color_t *)malloc(sz);
#endif
    if (!s_buf) {
        // Silent until now, and it looked exactly like "the menu is broken": with no
        // canvas, refresh() below is a no-op, so the switcher overlay appeared with no
        // app names on it and there was no way to tell which app you were on.
#ifdef ARDUINO
        Serial.printf("[menu_text] canvas alloc FAILED (%u bytes) - falling back to a plain label\n",
                      (unsigned)sz);
#else
        printf("[menu_text] canvas alloc FAILED (%u bytes) - falling back to a plain label\n",
               (unsigned)sz);
#endif
        return;
    }
#ifdef ARDUINO
    Serial.printf("[menu_text] canvas acquired (%u bytes), %u KB PSRAM free after\n",
                  (unsigned)sz, (unsigned)(ESP.getFreePsram() / 1024));
#endif
    s_canvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(s_canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_center(s_canvas);
#endif
}

void release() {
#if CUSTOM_HAS_MENU
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

// Width of a substring in the given font, in pixels.
static float measure(const lv_font_t *font, const char *s, int len) {
    float total = 0.0f;
    for (int i = 0; i < len; ++i) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)s[i], 0)) total += (float)g.adv_w;
    }
    return total;
}

// Draw a name, wrapping on spaces when it is wider than wrapWidth, and centring the
// resulting stack as a block on `by`.
//
// draw_straight() puts one line's visual middle on `by`. Stacking naively from there
// would hang extra lines below the anchor and make a two-word name look dropped rather
// than deliberate; instead the whole block's middle lands on `by`, so "Clock" and
// "Flight Tracker" sit at the same optical centre. wrapWidth 0 keeps the old
// single-line behaviour exactly, which is what every existing theme gets.
void draw_wrapped(const lv_font_t *font, const char *str, float bx, float by,
                  lv_color_t col, int glow, lv_color_t glowCol, int align,
                  int wrapWidth, int lineGap, int lineStep) {
    if (!font || !str || !str[0]) return;
    char wrapped[160];
    const int lines = wrap_text(font, str, wrapWidth, wrapped, sizeof(wrapped));
    if (lines <= 1) { draw_straight(font, str, bx, by, col, glow, glowCol, align); return; }

    // Use the editor's exact step when it sent one. Deriving it here from
    // lv_font_get_line_height() while Studio derived it from size*1.2 put the two a few
    // pixels apart, which is enough to make a two-line name look right in the preview and
    // slightly off on the dial. The fallback only serves themes pushed before lineStep
    // existed.
    const float step = (lineStep > 0) ? (float)lineStep
                                      : ((float)lv_font_get_line_height(font) + (float)lineGap);
    // Same expression Studio uses: centre the line CENTRES about by, so a one-line and a
    // two-line name share an optical centre.
    const float firstY = by - (lines - 1) * step * 0.5f;

    char line[80];
    const char *p = wrapped;
    for (int i = 0; i < lines; ++i) {
        const char *nl = strchr(p, '\n');
        const size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const size_t cap = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, p, cap);
        line[cap] = '\0';
        draw_straight(font, line, bx, firstY + i * step, col, glow, glowCol, align);
        if (!nl) break;
        p = nl + 1;
    }
}

int wrap_text(const lv_font_t *font, const char *in, int wrapWidth, char *out, size_t cap) {
    (void)font; (void)wrapWidth;
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!in || !in[0]) return 0;
    // Explicit breaks, not measured ones. Auto-wrapping meant three separate width
    // calculations (Studio's canvas metrics, LVGL's font metrics, and this renderer's)
    // agreeing on where a line ends, which they did not: the same name broke in different
    // places in the preview and on the dial. The designer types '|' where the break
    // belongs and every consumer just honours it.
    size_t w = 0;
    int lines = 1;
    for (const char *p = in; *p && w + 1 < cap; ++p) {
        if (*p == '|' || *p == '\n') { out[w++] = '\n'; ++lines; }
        else out[w++] = *p;
    }
    out[w] = '\0';
    return lines;
}

void refresh(const char *prevName, const char *curName, const char *nextName) {
#if CUSTOM_HAS_MENU
    if (!s_canvas) return;
    // Phase 0 instrumentation (see orb-performance-architecture-plan.md). Every detent
    // of the knob lands here, so this is the number that decides whether the menu feels
    // instant or takes a second. Split clear-vs-draw because they have entirely
    // different fixes: the clear is 651 KB of memory traffic, the draw is the glow.
#ifdef ARDUINO
    const uint32_t t0 = micros();
#endif
    // Was lv_canvas_fill_bg(black, LV_OPA_TRANSP), measured at 143 ms to blank 651 KB
    // (~4.5 MB/s), because it writes through LVGL's per-pixel API. The buffer format is
    // TRUE_COLOR_ALPHA, 3 bytes per pixel, and "transparent black" is all zero bytes, so
    // a straight wipe produces an identical result at memory speed.
    memset(s_buf, 0, LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H));
    lv_obj_invalidate(s_canvas);   // fill_bg used to mark it dirty; do that ourselves now
#ifdef ARDUINO
    const uint32_t t1 = micros();
#endif
    char out[96];
    // CUSTOM_HAS_MENU_* (whether this slot exists at all) and each FONT stay
    // compile-time (see theme_style.h); position/color/glow/format/align now
    // follow the active SD theme.
#if CUSTOM_HAS_MENU_PREV
    if (prevName && prevName[0]) {
        const theme_style::MenuText &t = theme_style::menu().prev;
        format_name(t.fmt, prevName, out, sizeof(out));
        draw_straight(CUSTOM_MENU_PREV_FONT, out, (float)t.x, (float)t.y,
                     lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor), t.align);
    }
#endif
#if CUSTOM_HAS_MENU_NEXT
    if (nextName && nextName[0]) {
        const theme_style::MenuText &t = theme_style::menu().next;
        format_name(t.fmt, nextName, out, sizeof(out));
        draw_straight(CUSTOM_MENU_NEXT_FONT, out, (float)t.x, (float)t.y,
                     lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor), t.align);
    }
#endif
#if CUSTOM_HAS_MENU_CURRENT
    if (curName && curName[0]) {
        const theme_style::MenuText &t = theme_style::menu().current;
        format_name(t.fmt, curName, out, sizeof(out));
        draw_wrapped(CUSTOM_MENU_CURRENT_FONT, out, (float)t.x, (float)t.y,
                     lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor), t.align,
                     t.wrapWidth, t.lineGap, t.lineStep);
    }
#endif
    lv_obj_invalidate(s_canvas);
#ifdef ARDUINO
    const uint32_t t2 = micros();
    Serial.printf("[perf] menu refresh: clear %lu us, draw %lu us, total %lu us (%lu ms)\n",
                  (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
                  (unsigned long)(t2 - t0), (unsigned long)((t2 - t0) / 1000));
#endif
#else
    (void)prevName; (void)curName; (void)nextName;
#endif
}

bool available() { return s_canvas != nullptr; }

} // namespace menu_text
