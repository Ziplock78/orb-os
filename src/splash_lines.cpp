#include "splash_lines.h"
#include "config.h"          // SCREEN_W / SCREEN_H, FW_VERSION
#include "theme_style.h"     // the placement and styling these three lines are allowed
#include "curved_text.h"     // straight AND arc, one code path, glow included
#include "font_ladder.h"     // the sizes this binary actually contains
#include "custom_sprite.h"   // custom_overlay() — the glass, decoded flash-then-SD
#include <stdio.h>
#include <string.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdlib>
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif

namespace splash_lines {
namespace {

constexpr int W = SCREEN_W;
constexpr int H = SCREEN_H;
constexpr size_t CANVAS_BYTES = (size_t)W * H * 3;   // RGB565 + alpha, what curved_text wants

lv_obj_t *s_canvas  = nullptr;
lv_obj_t *s_glass   = nullptr;
uint8_t  *s_buf     = nullptr;
lv_img_dsc_t s_glassDsc{};

// The address line. Held here rather than read from elsewhere because it arrives late: the
// splash is on screen well before WiFi has an IP, and About can be opened before or after.
char s_net[64] = "capsuleradar.local";

lv_color_t rgb(uint32_t v) {
    return lv_color_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

void one_line(const theme_style::SplashText &t, const char *text) {
    if (!text || !*text || !s_buf) return;
    const curved_text::Target dst{ s_buf, W, H };
    const lv_font_t *f = font_ladder(t.size);
    const lv_color_t col = rgb(t.color);
    const lv_color_t glowCol = rgb(t.glowColor);
    if (t.curved && t.curveR > 0) {
        curved_text::draw_arc(dst, f, text, (float)(W / 2), (float)(H / 2),
                              (float)t.curveR, t.arcDeg, col, t.glow, glowCol);
    } else {
        curved_text::draw_straight(dst, f, text, (float)t.x, (float)t.y,
                                   col, t.glow, glowCol, t.align);
    }
}

// Repaint all three. Cheap enough to do whole rather than tracking which line changed: it
// happens on entry and when the address arrives, not per frame.
void repaint() {
    if (!s_buf) return;
    memset(s_buf, 0, CANVAS_BYTES);          // fully transparent; the picture shows through
    const theme_style::Splash &sp = theme_style::splash();
    char ver[48];
    snprintf(ver, sizeof(ver), "Capsule Radar v%s", FW_VERSION);
    one_line(sp.version, ver);
    one_line(sp.network, s_net);
    // Two sources, two lines. draw_straight lays one line, so the newline is walked here
    // rather than teaching the glyph code about paragraphs for the sake of one caller.
    const char *credits[] = { "Aircraft data: adsb.lol", "Map data: OpenStreetMap" };
    theme_style::SplashText second = sp.credits;
    second.y += (int)lv_font_get_line_height(font_ladder(sp.credits.size)) + 4;
    one_line(sp.credits, credits[0]);
    one_line(second, credits[1]);
    if (s_canvas) lv_obj_invalidate(s_canvas);
}

} // namespace

void attach(lv_obj_t *parent) {
    release();
    if (!parent) return;
    // The TEXT is drawn for every theme, styled or not. Its compiled defaults are the exact
    // offsets settings_view.cpp used to hardcode, so an older theme is not losing three
    // lines here, it is getting the same three from a different place. Gating this on
    // `styled` would have deleted the firmware version off every theme already on a card.
    s_buf = (uint8_t *)heap_caps_malloc(CANVAS_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
#ifdef ARDUINO
        Serial.println("[splash_lines] no PSRAM for the text canvas - splash text skipped");
#endif
        return;
    }
    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_buf, W, H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_obj_center(s_canvas);
    repaint();

    // The glass, last, over everything. See the header: this is the whole reason the bake
    // stopped including it.
    //
    // ONLY for themes that ship splash_style.json. An older theme's splash.png already has
    // the glass painted in by the browser, and compositing it again would show it twice.
    if (theme_style::splash().styled)
    if (const uint8_t *ov = custom_overlay()) {
        s_glassDsc.header.always_zero = 0;
        s_glassDsc.header.w  = W;
        s_glassDsc.header.h  = H;
        s_glassDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        s_glassDsc.data_size = CANVAS_BYTES;
        s_glassDsc.data      = ov;
        s_glass = lv_img_create(parent);
        lv_img_set_src(s_glass, &s_glassDsc);
        lv_obj_center(s_glass);
        lv_obj_move_foreground(s_glass);
    }
}

void setNetwork(const char *line) {
    if (!line || !*line) return;
    snprintf(s_net, sizeof(s_net), "%s", line);
    repaint();
}

void release() {
    if (s_glass)  { lv_obj_del(s_glass);  s_glass = nullptr; }
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_buf)    { free(s_buf); s_buf = nullptr; }
}

} // namespace splash_lines
