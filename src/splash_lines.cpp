#include "splash_lines.h"
#include "config.h"          // SCREEN_W / SCREEN_H, FW_VERSION
#include "theme_style.h"     // the placement and styling these three lines are allowed
#include "curved_text.h"     // straight AND arc, one code path, glow included
#include "font_ladder.h"     // the sizes this binary actually contains
#include "splash_font.h"     // ...and Inter, for these three lines specifically
#include "custom_sprite.h"   // splash_overlay() — the glass, decoded flash-then-SD
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
//
// 112 to match the two buffers this is copied FROM - main.cpp's net[112] and
// settings_view.cpp's s_netInfo[112]. At 64 it was the short one in the chain and silently
// clipped the tail: "Configure at\ntheorb.local\n192.168.1.42  |  28.53830, -81.37920" is 62
// characters and only fit by luck, and a three-digit octet or a three-digit longitude - a
// stranger in Denver, say - pushed it over.
char s_net[112] = ORB_MDNS_ADDR;

lv_color_t rgb(uint32_t v) {
    return lv_color_make((uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v);
}

void one_line(const theme_style::SplashText &t, const char *text, int lineGap = 0) {
    if (!text || !*text || !s_buf) return;
    const curved_text::Target dst{ s_buf, W, H };
    const lv_font_t *f = splash_font(t.size);   // Inter, not the compiled stock face
    const lv_color_t col = rgb(t.color);
    const lv_color_t glowCol = rgb(t.glowColor);
    if (t.curved && t.curveR > 0) {
        curved_text::draw_arc(dst, f, text, (float)(W / 2), (float)(H / 2),
                              (float)t.curveR, t.arcDeg, col, t.glow, glowCol, (lv_opa_t)t.opa);
    } else {
        curved_text::draw_straight(dst, f, text, (float)t.x, (float)t.y,
                                   col, t.glow, glowCol, t.align, (lv_opa_t)t.opa,
                                   curved_text::pill_of(t), lineGap);
    }
}

// Repaint all three. Cheap enough to do whole rather than tracking which line changed: it
// happens on entry and when the address arrives, not per frame.
void repaint() {
    if (!s_buf) return;
    memset(s_buf, 0, CANVAS_BYTES);          // fully transparent; the picture shows through
    const theme_style::Splash &sp = theme_style::splash();
    char ver[48];
    snprintf(ver, sizeof(ver), "The Orb OS v%s", FW_VERSION);
    one_line(sp.version, ver);
    // The theme's name and who made it, THEME_CAPS 35. UX-028 puts these beside the version
    // and the credits, and CUT-07 says so by number; the splash carried neither. Live like
    // the other three because they come from theme.json on the card, which Studio writes
    // at export rather than baking into the picture. ASCII only, "by" rather than an
    // interpunct: the compiled Inter covers 0x20-0x7F and anything outside it draws nothing.
    char who[112];
    const char *author = theme_style::themeAuthor();
    if (author && *author) snprintf(who, sizeof(who), "%s by %s", theme_style::themeLabel(), author);
    else                   snprintf(who, sizeof(who), "%s", theme_style::themeLabel());
    one_line(sp.theme, who);
    // Three lines: "Configure at", the mDNS name, and the IP with the active centre point.
    // They arrive as one string with newlines in it and draw_straight lays them, which is
    // the whole of the fix - see its header for why that had to move into the shared path.
    one_line(sp.network, s_net);
    // Two sources, and DELIBERATELY still two calls now that draw_straight could lay them
    // from one string. Orb Studio previews these as two independently placed lines - the
    // second at `y + size + 4` (studio.tsx's SplashPreview) - so they are two lines in the
    // designer's head and two anchors in the file. Handing them over as one block would
    // centre the pair on the credits anchor and slide both up ~9 px away from the preview
    // somebody positioned them against, which is a worse fault than the duplication.
    //
    // The two steps do not agree and this is worth fixing on the Studio side rather than
    // here: Studio steps by size + 4 (16 px), this steps by the font's line height + 4
    // (19 px). menu_text hit exactly this and settled it by having Studio send an explicit
    // lineStep; these lines have no such field yet.
    const char *credits[] = { "Aircraft data: adsb.lol", "Map data: OpenStreetMap" };
    theme_style::SplashText second = sp.credits;
    second.y += (int)lv_font_get_line_height(splash_font(sp.credits.size)) + 4;
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
    // splash_overlay(), NOT custom_overlay(). custom_overlay() is the CLOCK's glass and
    // Studio bakes the hand-pivot hub into it, so borrowing it painted a white dot in the
    // middle of the startup screen and of Settings > About for every theme that reaches
    // here. Found on the glass by Zion, traced by pulling the file off his own card.
    //
    // A theme baked before Studio exports splash_overlay.png returns nullptr and gets no
    // glass on its splash, which is the right way to fail: a plainer screen, not a dot.
    //
    // ONLY for themes that ship splash_style.json. An older theme's splash.png already has
    // the glass painted in by the browser, and compositing it again would show it twice.
    if (theme_style::splash().styled)
    if (const uint8_t *ov = splash_overlay()) {
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
