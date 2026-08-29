#include "ticker_view.h"
#include "ticker.h"
#include "theme_style.h"
#include "theme_font.h"
#include "curved_text.h"
#include "plate_sprite.h"
#include "config.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
    void println(const char *s) const { std::printf("%s\n", s); }
} Serial;
#define MALLOC_CAP_SPIRAM 0
static void *heap_caps_malloc(size_t n, int) { return malloc(n); }
static void  heap_caps_free(void *p) { free(p); }
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace tickerview {
namespace {

// SCREEN_W / SCREEN_H come from config.h, which defines them as macros. Declaring them
// again here as constexpr is a redefinition the preprocessor turns into nonsense.

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_plate  = nullptr;   // the theme's own background picture, when it ships one
plate_sprite::Plate s_plateArt { "ticker_plate.png", "ticker_plate" };
lv_obj_t *s_name   = nullptr;
lv_obj_t *s_price  = nullptr;
lv_obj_t *s_change = nullptr;
lv_obj_t *s_note   = nullptr;   // the honest empty state, and the "n unpriced" line
lv_obj_t *s_dots   = nullptr;   // which of the watchlist you are on
lv_obj_t *s_flat   = nullptr;   // the strip, when it is a straight line
lv_obj_t *s_arc    = nullptr;   // the strip, when it is bent round the bezel
uint8_t  *s_arcBuf = nullptr;

int   s_sel      = 0;
float s_offset   = 0.0f;        // px along the flat strip, or degrees around the curved one
uint32_t s_lastTickMs = 0;
char  s_strip[512] = "";        // the flat placements: the watchlist as one line

// The curved placement keeps its entries SEPARATE, because it does not draw a line, it
// places items around an arc. See draw_arc_strip() for why that distinction matters.
char  s_entry[theme_style::TICKER_MAX_SYMBOLS][56];
float s_entryDeg[theme_style::TICKER_MAX_SYMBOLS];   // angular width of each, at the current radius
float s_entryAt[theme_style::TICKER_MAX_SYMBOLS];    // where each sits along the loop, degrees
int   s_entries = 0;
float s_loopDeg = 360.0f;      // the whole watchlist laid end to end, in degrees

// How much of the dial the strip occupies, centred on the theme's angle. A band that goes
// all the way round cannot be read: glyphs are laid tangent to the circle, so by the bottom
// of the dial they are upside down, and the first version of this drew "AAPL" inverted
// every time it scrolled past six o'clock. An arc across the top reads left to right the
// whole way, which is what a ticker is for.
// 150 was too wide: glyphs are tangent to the circle, so an entry sitting 75 degrees off
// the top is rendered vertically, and reading it means tilting your head. Inside about 55
// degrees either way the tape still reads as a line of text.
constexpr float STRIP_WINDOW_DEG = 112.0f;

// Only the rows the arc can touch are cleared each frame. The alternative is memsetting
// 651 KB of canvas sixty times a minute to repaint a band a fifth of that tall, which is
// the sort of thing that does not look like a problem until the sweep on another screen
// starts stuttering and nobody can say why.
int s_arcTop = 0, s_arcBot = SCREEN_H;

const lv_font_t *size_font(int px) {
    // The compiled ladder, and nothing between its rungs. LVGL fonts are glyph bitmaps
    // rather than outlines, so a size this binary was not built with cannot be drawn at any
    // quality; asking for one and getting the nearest is how a theme silently redesigns
    // itself. Unknown values land on the default rather than the closest.
    switch (px) {
        case 12: return &lv_font_montserrat_12;
        case 14: return &lv_font_montserrat_14;
        case 16: return &lv_font_montserrat_16;
        case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24;
        case 28: return &lv_font_montserrat_28;
        case 32: return &lv_font_montserrat_32;
        case 40: return &lv_font_montserrat_40;
        case 48: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_16;
    }
}

// A theme's own face when it shipped one, otherwise the ladder. A loaded face is baked at
// one size and ignores the slider, which is why the predicate is asked first.
const lv_font_t *slot_font(int slot, int px) {
    if (theme_font::ticker_has_font(slot)) {
        switch (slot) {
            case 0: return theme_font::ticker_name();
            case 1: return theme_font::ticker_price();
            case 2: return theme_font::ticker_change();
            default: return theme_font::ticker_strip();
        }
    }
    return size_font(px);
}

void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_color_t dir_color(float change) {
    const theme_style::Ticker &t = theme_style::ticker();
    if (change > 0.0001f)  return lv_color_hex(t.upColor);
    if (change < -0.0001f) return lv_color_hex(t.downColor);
    return lv_color_hex(t.flatColor);
}

// Prices span the S&P at four figures and a penny stock at three decimals, and one format
// cannot serve both: "%.2f" turns 7711.76 into something that fits and 0.00042 into 0.00.
// Decimals are chosen from the magnitude, which is what a person reading it would expect.
void fmt_price(char *out, size_t cap, float v) {
    const float a = fabsf(v);
    if      (a >= 1000.0f) snprintf(out, cap, "%.2f", (double)v);
    else if (a >= 1.0f)    snprintf(out, cap, "%.2f", (double)v);
    else if (a >= 0.01f)   snprintf(out, cap, "%.4f", (double)v);
    else                   snprintf(out, cap, "%.6f", (double)v);
}

// The whole watchlist on one line, for the flat placements. Rebuilt when quotes land, not
// per frame: this is a string operation over eight items and it has no business happening
// sixty times a second.
void rebuild_strip() {
    const theme_style::Ticker &cfg = theme_style::ticker();
    s_strip[0] = '\0';
    size_t o = 0;
    const int n = ticker_count();
    for (int i = 0; i < n && o + 48 < sizeof(s_strip); ++i) {
        TickerQuote q;
        if (!ticker_get(i, q)) continue;
        const float ch = q.price - q.prev;
        const float pct = (q.prev > 0.0001f) ? (ch / q.prev) * 100.0f : 0.0f;
        char price[24];
        fmt_price(price, sizeof(price), q.price);
        // The arrow is ASCII on purpose. The converted fonts carry no fallback glyph, so a
        // real up-arrow would draw an empty box on any theme that did not happen to include
        // one, and an empty box next to a number reads as a fault rather than as direction.
        const char dir = ch > 0.0001f ? '+' : (ch < -0.0001f ? '-' : '=');
        o += snprintf(s_strip + o, sizeof(s_strip) - o, "%s %s %c%.2f%%     ",
                      q.sym, price, dir, (double)fabsf(pct));
    }
    if (!o) snprintf(s_strip, sizeof(s_strip), "NO QUOTES     ");

    // ...and the same content as separate items, for the curved placement.
    s_entries = 0;
    const lv_font_t *f = slot_font(3, cfg.stripSize);
    const float R = (float)cfg.stripRadius;
    float at = 0.0f;
    for (int i = 0; i < n && s_entries < theme_style::TICKER_MAX_SYMBOLS; ++i) {
        TickerQuote q;
        if (!ticker_get(i, q)) continue;
        const float ch  = q.price - q.prev;
        const float pct = (q.prev > 0.0001f) ? (ch / q.prev) * 100.0f : 0.0f;
        char price[24];
        fmt_price(price, sizeof(price), q.price);
        const char dir = ch > 0.0001f ? '+' : (ch < -0.0001f ? '-' : '=');
        // Symbol and move only, no price. The tape is read at a glance while the eye is on
        // something else, and the full price is already sitting in the middle of the screen
        // in forty-point type. Carrying it around the bezel as well made each entry half an
        // arc wide, so two of them filled the readable window and the rest were off-screen.
        snprintf(s_entry[s_entries], sizeof(s_entry[0]), "%s %c%.2f%%",
                 q.sym, dir, (double)fabsf(pct));
        (void)price;
        // Angular width from the real rendered width at this radius, not from a character
        // count: "^DJI 53559.99" and "AAPL 319.70" have the same number of glyphs and are
        // not the same width, and spacing them as though they were is what makes a ring of
        // text look hand-placed.
        const lv_coord_t px = lv_txt_get_width(s_entry[s_entries], (uint32_t)strlen(s_entry[s_entries]),
                                               f, 0, LV_TEXT_FLAG_NONE);
        s_entryDeg[s_entries] = (float)px / R * 57.29578f;
        s_entryAt[s_entries]  = at + s_entryDeg[s_entries] * 0.5f;
        at += s_entryDeg[s_entries] + 10.0f;   // a gap, so two quotes never touch
        ++s_entries;
    }
    // The loop is exactly the content: symbols plus the gaps between them, and nothing
    // else. Padding it out to a full turn left a short watchlist sitting in an empty
    // window most of the time, which looked like the app had lost its data rather than
    // like a tape with a gap in it.
    s_loopDeg = at > 1.0f ? at : 360.0f;
}

// One line saying which thing is unwell. Never a bare "unavailable": a wrong symbol, a
// dropped connection and a gateway that is down are three different problems and only one
// of them is worth touching the Orb over.
const char *state_note() {
    static char buf[64];
    switch (ticker_state()) {
        case TICKER_LOADING:    return "GETTING PRICES";
        case TICKER_NO_WIFI:    return "NO WIFI, SO NO PRICES";
        case TICKER_FAILED:     return "THE PRICE SERVICE IS NOT ANSWERING";
        case TICKER_NO_SYMBOLS: return "NO SYMBOLS IN THIS THEME'S WATCHLIST";
        case TICKER_STALE: {
            const uint32_t up = ticker_updated_ms();
            if (!up) return "PRICES ARE OUT OF DATE";
            const uint32_t mins = (lv_tick_get() - up) / 60000U;
            snprintf(buf, sizeof(buf), "LAST PRICED %lu MIN AGO", (unsigned long)mins);
            return buf;
        }
        default: {
            const int miss = ticker_missing();
            if (miss > 0) {
                snprintf(buf, sizeof(buf), "%d SYMBOL%s COULD NOT BE PRICED",
                         miss, miss == 1 ? "" : "S");
                return buf;
            }
            return "";
        }
    }
}

void style_all() {
    const theme_style::Ticker &t = theme_style::ticker();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(t.bg), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    lv_obj_set_style_text_font(s_name,   slot_font(0, t.nameSize), 0);
    lv_obj_set_style_text_color(s_name,  lv_color_hex(t.nameColor), 0);
    lv_obj_set_style_text_font(s_price,  slot_font(1, t.priceSize), 0);
    lv_obj_set_style_text_font(s_change, slot_font(2, t.changeSize), 0);

    lv_obj_align(s_name,   LV_ALIGN_TOP_MID, 0, t.nameY);
    lv_obj_align(s_price,  LV_ALIGN_TOP_MID, 0, t.priceY);
    lv_obj_align(s_change, LV_ALIGN_TOP_MID, 0, t.changeY);

    if (s_flat) {
        lv_obj_set_style_text_font(s_flat, slot_font(3, t.stripSize), 0);
        lv_obj_set_style_text_color(s_flat, lv_color_hex(t.stripColor), 0);
        lv_obj_set_style_text_opa(s_flat, (lv_opa_t)t.stripOpa, 0);
        lv_obj_align(s_flat, LV_ALIGN_TOP_MID, 0,
                     t.stripPlace == theme_style::Ticker::STRIP_TOP ? 40 : t.stripY);
    }
}

// Each entry in its own direction's colour, when the theme asks for that. A tape where the
// fallers are red is readable at a glance from across a room; one in a single colour has to
// be read.
lv_color_t stripEntryColor(int i) {
    const theme_style::Ticker &t = theme_style::ticker();
    if (!t.stripUpDown) return lv_color_hex(t.stripColor);
    TickerQuote q;
    if (!ticker_get(i, q)) return lv_color_hex(t.stripColor);
    return dir_color(q.price - q.prev);
}

// The curved strip. One band of the canvas cleared, and one arc call per visible entry.
void draw_arc_strip() {
    const theme_style::Ticker &t = theme_style::ticker();
    if (!s_arcBuf || !s_arc) return;

    const float R = (float)t.stripRadius;
    // The band the text can occupy: the arc's own extent plus a glyph's height either side.
    const float pad = (float)t.stripSize + 8.0f;
    const float cy  = SCREEN_H / 2.0f;
    int top = (int)(cy - R - pad), bot = (int)(cy + R + pad);
    if (top < 0) top = 0;
    if (bot > SCREEN_H) bot = SCREEN_H;
    s_arcTop = top; s_arcBot = bot;

    memset(s_arcBuf + (size_t)top * SCREEN_W * 3, 0, (size_t)(bot - top) * SCREEN_W * 3);

    curved_text::Target dst { s_arcBuf, SCREEN_W, SCREEN_H };
    const lv_font_t *f = slot_font(3, t.stripSize);
    const float half = STRIP_WINDOW_DEG * 0.5f;

    // Each entry placed at its own angle, and drawn only while it is inside the window.
    // Drawing the watchlist as ONE long string and rotating it was the first attempt, and
    // it put half the tape upside down along the bottom of the dial: glyphs are laid
    // tangent to the circle, so a band that goes all the way round cannot read the right
    // way up all the way round. A window across the top reads properly for its whole width.
    for (int i = 0; i < s_entries; ++i) {
        // Where this entry has scrolled to, folded into one loop, then centred so that zero
        // means "in the middle of the window". The fold is what makes the tape endless
        // without s_offset ever having to grow large enough to lose its precision.
        float d = fmodf(s_entryAt[i] + s_offset, s_loopDeg);
        if (d < 0) d += s_loopDeg;
        const float edge = half + s_entryDeg[i] * 0.5f;
        // Tried at three places, one loop apart. A watchlist shorter than the window has to
        // appear twice at once or the tape has a hole in it, and an entry halfway off the
        // end has to come back on at the other side in the same frame rather than the next
        // one. Both are the same wrap, so both are the same three tests.
        for (int rep = -1; rep <= 1; ++rep) {
            const float rel = d - s_loopDeg * 0.5f + (float)rep * s_loopDeg;
            if (rel < -edge || rel > edge) continue;
            curved_text::draw_arc(dst, f, s_entry[i],
                                  SCREEN_W / 2.0f, cy, R, (float)t.stripAngle + rel,
                                  stripEntryColor(i), 0, lv_color_black(),
                                  (lv_opa_t)t.stripOpa);
        }
    }
    lv_obj_invalidate(s_arc);
}

}  // namespace

void init() {
    s_screen = lv_obj_create(nullptr);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, SCREEN_W, SCREEN_H);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Behind everything, and created first so it is at the back of the stack without having
    // to be moved there afterwards.
    s_plate = lv_img_create(s_screen);
    lv_obj_center(s_plate);
    show(s_plate, false);

    s_name = lv_label_create(s_screen);
    lv_label_set_text(s_name, "");
    lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);

    s_price = lv_label_create(s_screen);
    lv_label_set_text(s_price, "");
    lv_obj_set_style_text_align(s_price, LV_TEXT_ALIGN_CENTER, 0);

    s_change = lv_label_create(s_screen);
    lv_label_set_text(s_change, "");
    lv_obj_set_style_text_align(s_change, LV_TEXT_ALIGN_CENTER, 0);

    s_note = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_note, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_note, lv_color_hex(0x8A94A6), 0);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_note, "");
    lv_obj_align(s_note, LV_ALIGN_TOP_MID, 0, 330);

    s_dots = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_dots, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_dots, lv_color_hex(0x5F6874), 0);
    lv_label_set_text(s_dots, "");
    lv_obj_align(s_dots, LV_ALIGN_TOP_MID, 0, 152);

    // The flat strip is a label with LVGL's own circular scroll. It costs one object and
    // moves more smoothly than anything this screen could re-render per frame, so the
    // canvas below exists only for the curved placement that LVGL cannot do at all.
    s_flat = lv_label_create(s_screen);
    lv_label_set_long_mode(s_flat, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(s_flat, SCREEN_W - 60);
    lv_label_set_text(s_flat, "");

    s_arc = lv_canvas_create(s_screen);
    lv_obj_center(s_arc);
    show(s_arc, false);

    style_all();
    Serial.println("[ticker] screen built");
}

lv_obj_t *screen() { return s_screen; }

void onEnter() {
    const theme_style::Ticker &t = theme_style::ticker();
    s_sel = 0;
    s_offset = 0.0f;
    s_lastTickMs = lv_tick_get();

    // Take the canvas ONLY when the design actually curves its strip. 651 KB is not a thing
    // to hold for a feature the theme has switched off, and every screen on this device now
    // takes its memory on the way in and gives it back on the way out.
    if (t.stripShow && t.stripPlace == theme_style::Ticker::STRIP_CURVED && !s_arcBuf) {
        s_arcBuf = (uint8_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 3, MALLOC_CAP_SPIRAM);
        if (s_arcBuf) {
            memset(s_arcBuf, 0, (size_t)SCREEN_W * SCREEN_H * 3);
            lv_canvas_set_buffer(s_arc, s_arcBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
        } else {
            Serial.println("[ticker] no PSRAM for the curved strip; falling back to the flat one");
        }
    }
    // The plate is decoded HERE rather than at init, so a screen nobody visits costs no
    // PSRAM, and a theme changed while another app was up is picked up on the way in.
    const lv_img_dsc_t *art = plate_sprite::get(s_plateArt);
    if (art && s_plate) {
        lv_img_set_src(s_plate, art);
        lv_obj_move_background(s_plate);
    }
    show(s_plate, art != nullptr);

    style_all();
    rebuild_strip();
    onQuotesReady();
}

void onExit() {
    // The picture goes back with everything else. 424 KB is not a thing to hold for a screen
    // nobody is looking at.
    if (s_plate) { lv_img_set_src(s_plate, nullptr); show(s_plate, false); }
    plate_sprite::release(s_plateArt);
    // Delete the object rather than handing it a null buffer. LVGL does not accept the
    // latter and wedges the UI thread on the next app switch, which cost two evenings and
    // three flashes to learn on the clock.
    if (s_arc) { lv_obj_del(s_arc); s_arc = nullptr; }
    if (s_arcBuf) { heap_caps_free(s_arcBuf); s_arcBuf = nullptr; }
    s_arc = lv_canvas_create(s_screen);
    lv_obj_center(s_arc);
    show(s_arc, false);
}

void onTurn(int delta) {
    const int n = ticker_count();
    if (n <= 0) return;
    s_sel += delta;
    // Wraps, because a watchlist is a ring and stopping at the end of eight symbols would
    // mean turning the knob backwards to see the first one again.
    while (s_sel < 0)  s_sel += n;
    while (s_sel >= n) s_sel -= n;
    onQuotesReady();
}

void onPress() {
    // Ask now rather than waiting out the poll, the same as the News screen's press. The
    // network task owns the fetch; this only moves its next-due time forward.
    Serial.println("[ticker] refresh asked for");
    ticker_store_begin();
}

void onQuotesReady() {
    if (!s_screen) return;
    const theme_style::Ticker &t = theme_style::ticker();
    const int n = ticker_count();
    if (s_sel >= n) s_sel = n > 0 ? n - 1 : 0;

    TickerQuote q;
    const bool have = n > 0 && ticker_get(s_sel, q);

    show(s_name,   have && t.nameShow);
    show(s_price,  have && t.priceShow);
    show(s_change, have && t.changeShow);
    show(s_dots,   have && n > 1);

    if (have) {
        char line[80];
        // Symbol and name together: the symbol is what was asked for and the name is what
        // it turned out to be, and on an index those are "^GSPC" and "S&P 500", neither of
        // which is much use without the other.
        snprintf(line, sizeof(line), "%s  %s", q.sym, q.name);
        lv_label_set_text(s_name, line);

        char price[24];
        fmt_price(price, sizeof(price), q.price);
        lv_label_set_text(s_price, price);

        const float ch  = q.price - q.prev;
        const float pct = (q.prev > 0.0001f) ? (ch / q.prev) * 100.0f : 0.0f;
        char chg[48];
        if (t.changePct) snprintf(chg, sizeof(chg), "%+.2f  (%+.2f%%)", (double)ch, (double)pct);
        else             snprintf(chg, sizeof(chg), "%+.2f", (double)ch);
        lv_label_set_text(s_change, chg);

        const lv_color_t dc = dir_color(ch);
        lv_obj_set_style_text_color(s_change, dc, 0);
        lv_obj_set_style_text_color(s_price, t.priceColorOn ? lv_color_hex(t.priceColor) : dc, 0);

        // Where you are in the watchlist, as marks rather than "3 of 8": it is read at a
        // glance from across a room and a fraction is not.
        char dots[theme_style::TICKER_MAX_SYMBOLS * 2 + 1] = "";
        for (int i = 0; i < n && i < theme_style::TICKER_MAX_SYMBOLS; ++i)
            strlcat(dots, i == s_sel ? "O " : ". ", sizeof(dots));
        lv_label_set_text(s_dots, dots);
    }

    const char *note = state_note();
    lv_label_set_text(s_note, note);
    show(s_note, note[0] != '\0');

    rebuild_strip();
    const bool curved = t.stripShow && t.stripPlace == theme_style::Ticker::STRIP_CURVED && s_arcBuf;
    show(s_flat, t.stripShow && !curved);
    show(s_arc,  curved);
    if (!curved && t.stripShow) lv_label_set_text(s_flat, s_strip);
    if (curved) draw_arc_strip();
}

void tick() {
    const theme_style::Ticker &t = theme_style::ticker();
    if (!t.stripShow) return;
    const uint32_t now = lv_tick_get();
    const uint32_t dt  = now - s_lastTickMs;
    if (dt < 40) return;                  // ~25 fps ceiling; the strip does not need more
    s_lastTickMs = now;

    // Only the curved strip is driven from here. The flat one is LVGL's own circular scroll
    // animation, which runs on the library's clock and stays smooth without this function
    // knowing anything about it.
    if (t.stripPlace != theme_style::Ticker::STRIP_CURVED || !s_arcBuf) return;

    // Advanced by REAL elapsed time, not by a fixed step per call. A fixed step ties the
    // speed to the frame rate, so the strip slows down whenever anything else on the device
    // is busy, which is exactly the stutter the sweep took so long to get rid of.
    s_offset -= (float)t.stripSpeed * (float)dt / 1000.0f;
    if (s_offset <= -360.0f) s_offset += 360.0f;
    draw_arc_strip();
}

}  // namespace tickerview
