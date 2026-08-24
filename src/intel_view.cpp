#include "intel_view.h"
#include "intel.h"
#include "intel_client.h"
#include "config.h"
#include "theme_style.h"
#include "app_shell.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#else
#include <cstdio>
#include <cstdarg>
#include <chrono>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
    void println(const char *s) const { std::printf("%s\n", s); }
} Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif
#include <stdio.h>
#include <string.h>
#include <math.h>

// The Intel screen: a title, a handful of headlines, and how old they are.
//
// The whole design rule here is that this is read from across a room, in passing. It is
// not a news reader and, at the default type size, there is nothing to scroll. Three
// headlines is the default because three fit on a 466 px circle at a size that can be
// read standing up; five is available for people who would rather have density than size.
//
// THEME_CAPS 11 made the screen composed rather than fixed: the title is its own movable
// text, the headline type size is the theme's to pick from the compiled set, the block
// has real margins, and the age line is placeable. A size big enough that the requested
// count no longer fits does not clip it: the view shows what fits and the knob scrolls,
// using the Flight Tracker's exact grammar — press to take the knob, turn to move,
// press again (or six idle seconds) to give it back.
namespace {

// A circle is a hostile place for text. At the vertical centre the dial is 466 px wide,
// but a line sitting 150 px above the middle only has about 350 px of glass under it, and
// text that overruns is clipped by the bezel rather than wrapped.
//
// Square mode (the original, and still the default) lays out every headline inside one
// fixed-width box, [marginLeft, 466 - marginRight] — the default margins reproduce the
// original 330 px column exactly. Curved mode (THEME_CAPS 10) intersects that box with
// the chord of theme_style::intel().curveRadius at each row's height, so a row near the
// middle keeps the box and a row near the top or bottom narrows to match the glass
// actually under it.
// SCREEN_W (466) comes from config.h, the same macro every other view measures against.
constexpr int MIN_ROW_W    = 60;   // never wrap narrower than this, whatever box and chord say
constexpr int TITLE_Y      = -168; // the fixed layout's title spot, kept as the default
constexpr int FIRST_ROW_Y  = -74;
constexpr int ROW_STEP_5   = 52;   // five headlines: tighter, smaller type (automatic mode)
constexpr int ROW_STEP_3   = 74;   // three headlines: room for two wrapped lines each
constexpr int AGE_Y        = 176;
constexpr int ROW_GAP      = 10;   // explicit-size mode: air between headline blocks
// The scroll indicator: a row along the BOTTOM, and only while the knob is scrolling.
//
// It started as a column of dots beside the headlines, 191 px out from the centre. On a
// dark dial, small and vertically stacked and level with the text, it did not read as an
// indicator at all — it read as a colon someone had left on the end of the sentence. That
// is the whole lesson: an indicator that sits in the text's own line box becomes
// punctuation. Along the bottom, horizontal, it reads as pagination, which is what it is.
constexpr int DOT_Y        = 196;  // centre-relative; below the age line's default 176
constexpr int DOT_STEP     = 14;   // px between dot centres
constexpr uint32_t SCROLL_IDLE_MS = 6000;  // scroll mode lets go after this much stillness
// A headline too long for its rows fades out instead of ending in an ellipsis. The fade is
// a stack of background-coloured strips over the bottom of the row, each more opaque than
// the one above it — LVGL 8's gradients interpolate colour but carry a single opacity, so
// a real alpha ramp has to be built out of steps. Six is enough that the banding is not
// visible at arm's length, and cheap enough to keep one set per row permanently.
//
// This is only ever drawn over the theme's own flat background colour, which is the same
// reason the Intel screen has no image background: there is nothing underneath for the
// strips to be wrong about.
constexpr int FADE_STEPS   = 6;

// Read live rather than cached at file scope: theme_style::load() runs during
// theme_select::init(), well before this screen is ever created (see intelview::init()'s
// call site in main.cpp), so every one of these already reflects the active theme by the
// time anything here asks. THEME_CAPS 9 is what taught this screen to have colours at all;
// below that level a theme carries none and these fall back to the same greys this screen
// has always drawn.
lv_color_t c_title()  { return lv_color_hex(theme_style::intel().titleColor); }
lv_color_t c_text()   { return lv_color_hex(theme_style::intel().textColor); }
lv_color_t c_source() { return lv_color_hex(theme_style::intel().sourceColor); }
lv_color_t c_stale()  { return lv_color_hex(theme_style::intel().staleColor); }

// The compiled Montserrat sizes this screen may use. theme_style's parser already snapped
// anything else back to a known size, so the default case here is belt and braces for a
// struct edited from code rather than from a theme file.
const lv_font_t *font_for(int size) {
    switch (size) {
        case 12: return &lv_font_montserrat_12;
        case 14: return &lv_font_montserrat_14;
        case 18: return &lv_font_montserrat_18;
        case 20: return &lv_font_montserrat_20;
        case 22: return &lv_font_montserrat_22;
        case 24: return &lv_font_montserrat_24;
        case 26: return &lv_font_montserrat_26;
        case 28: return &lv_font_montserrat_28;
        case 32: return &lv_font_montserrat_32;
        case 36: return &lv_font_montserrat_36;
        case 40: return &lv_font_montserrat_40;
        case 44: return &lv_font_montserrat_44;
        case 48: return &lv_font_montserrat_48;
        default: return &lv_font_montserrat_16;
    }
}

// Where a row of text may live at height y (centre-relative): the margin box, intersected
// with the circle's chord when the curved boundary is on. Returns width and the row's own
// centre as an offset from the screen centre — asymmetric margins move rows sideways, and
// the chord can clip one side of an off-centre box before the other.
struct RowBox { int w; int cx; };
RowBox row_box(int y) {
    const theme_style::Intel &cfg = theme_style::intel();
    int x0 = cfg.marginLeft;
    int x1 = SCREEN_W - cfg.marginRight;
    if (cfg.curvedBounds) {
        const int r  = cfg.curveRadius;
        const int rr = r * r - y * y;
        // A row past the circle's own edge has no chord at all; the floor below covers it.
        const int half = rr > 0 ? (int)sqrtf((float)rr) : 0;
        if (SCREEN_W / 2 - half > x0) x0 = SCREEN_W / 2 - half;
        if (SCREEN_W / 2 + half < x1) x1 = SCREEN_W / 2 + half;
    }
    int w = x1 - x0;
    int cx = (x0 + x1) / 2 - SCREEN_W / 2;
    if (w < MIN_ROW_W) w = MIN_ROW_W;
    return { w, cx };
}

lv_obj_t *s_screen = nullptr;
lv_obj_t *s_title  = nullptr;
lv_obj_t *s_age    = nullptr;
// The age line's glow: the clock banner's ring technique (draw the string again in the
// glow colour at a ring of offsets with falling opacity, sharp fill on top) rebuilt from
// labels, because this screen has no canvas and one 466x466 canvas for a one-line field
// would cost more PSRAM than the whole rest of the screen. Two rings of eight, the inner
// ring stronger, reads the same as the canvas blur at this size.
constexpr int AGE_GLOW_DIRS  = 8;
constexpr int AGE_GLOW_RINGS = 2;
lv_obj_t *s_ageGlow[AGE_GLOW_RINGS * AGE_GLOW_DIRS] = {};
// Each headline is a LABEL INSIDE A FIXED-HEIGHT BOX, not a bare label, and the box is
// the whole reason the fade works. LVGL's long modes do not offer "wrap, then clip": WRAP
// grows to fit the text and CLIP refuses to wrap at all (it draws one endless line and
// cuts it off at both edges, which is exactly what it did when this was tried the easy
// way). A plain object clips its children by default, so the label wraps freely inside a
// box that is exactly two lines tall, and everything past that is simply not drawn.
lv_obj_t *s_rowBox[INTEL_MAX_ITEMS] = {};
lv_obj_t *s_rows[INTEL_MAX_ITEMS]   = {};
lv_obj_t *s_credit[INTEL_MAX_ITEMS] = {};
lv_obj_t *s_dots[INTEL_MAX_ITEMS]   = {};   // one per item, shown only while scrolling
lv_obj_t *s_fade[INTEL_MAX_ITEMS][FADE_STEPS] = {};   // per row, shown only when it overflows
lv_obj_t *s_empty  = nullptr;       // the one line shown when there is nothing to show

uint32_t s_lastTryMs   = 0;
bool     s_everFetched = false;

// Scroll state. s_scroll is the first visible item; s_visible is how many fit at the
// theme's type size (recomputed on every render); s_lastCount is what the last snapshot
// held, so onPress can tell "scrollable" from "fits" without re-reading the store.
int      s_scroll     = 0;
int      s_visible    = INTEL_MAX_ITEMS;
int      s_lastCount  = 0;
bool     s_scrollMode = false;
uint32_t s_scrollActivityMs = 0;

void show(lv_obj_t *o, bool on) {
    if (!o) return;
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Say which thing is unwell, the same way the scope does. From the desk an empty Intel
// screen looks identical whether the WiFi dropped, the gateway is down, or it simply has
// not asked yet, and the device knows which it is.
const char *empty_reason() {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return "No WiFi\nYour Orb is fine";
#endif
    // Three full stops, not "…": Montserrat has no glyph at U+2026 and draws an empty box
    // for it. Same reason the gateway sends ASCII in the headlines themselves.
    if (!s_everFetched)                return "Getting the headlines...";
    return "Headlines unavailable\nWiFi is fine, the service is not answering";
}

void exit_scroll_mode() {
    if (!s_scrollMode) return;
    s_scrollMode = false;
    app_shell::setCaptured(false);
}

// The scroll indicator, along the bottom, and ONLY while the knob is actually scrolling.
//
// One dot per headline, the visible window in the headline colour and the rest dimmed, so
// a big type size showing three of five does not read as "there are only three". It stays
// hidden at rest: the previous version was always on whenever the list overflowed, which
// put a permanent mark on a screen whose entire job is to be read at a glance, and it was
// the thing being mistaken for punctuation. While scrolling it is feedback for a gesture
// the person is in the middle of, which is when an indicator earns its place.
void style_dots(int count) {
    const bool on = s_scrollMode && count > s_visible;
    const int  left = -((count - 1) * DOT_STEP) / 2;
    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        if (!s_dots[i]) continue;
        const bool vis = on && i < count;
        show(s_dots[i], vis);
        if (!vis) continue;
        const bool inWindow = i >= s_scroll && i < s_scroll + s_visible;
        const int  d        = inWindow ? 7 : 5;
        lv_obj_set_size(s_dots[i], d, d);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER, left + i * DOT_STEP, DOT_Y);
        lv_obj_set_style_bg_color(s_dots[i], inWindow ? c_text() : c_source(), 0);
        lv_obj_set_style_bg_opa(s_dots[i], inWindow ? LV_OPA_COVER : LV_OPA_50, 0);
    }
}

// Park a row's fade strips over the bottom of its text box, or hide them.
//
// `cropped` comes from measuring the string rather than from guessing: lv_txt_get_size
// lays the text out at the row's real width and font and reports how tall it actually
// wants to be, so a headline that happens to fit gets no fade and one that does not gets
// exactly the fade its own overflow earned.
void place_fade(int row, bool cropped, int cx, int w, int rowBottomY, int lineH) {
    // The bottom third of the last visible line. The box already clips whatever follows,
    // so this ramp is not doing the hiding — it is only softening the cut, and it has to
    // stay shallow enough that the words it passes over are still readable. Two fifths was
    // tried first and swallowed the middle of the line.
    const int fadeH   = lineH / 3;
    const int stripH  = fadeH / FADE_STEPS + 1;   // +1 so rounding never leaves a seam
    const int fadeTop = rowBottomY - fadeH;
    for (int i = 0; i < FADE_STEPS; ++i) {
        lv_obj_t *f = s_fade[row][i];
        if (!f) continue;
        show(f, cropped);
        if (!cropped) continue;
        lv_obj_set_size(f, w, stripH);
        lv_obj_set_style_bg_color(f, lv_color_hex(theme_style::intel().bg), 0);
        // Transparent at the top, solid background at the bottom: the text does not stop,
        // it goes away, which is the honest picture of a headline that continues.
        lv_obj_set_style_bg_opa(f, (lv_opa_t)((i + 1) * 255 / FADE_STEPS), 0);
        // align() centres, so a strip meant to OCCUPY [top, top+stripH] is centred half a
        // strip below its own top. Getting this wrong shifts the whole ramp up by a strip.
        lv_obj_align(f, LV_ALIGN_CENTER, cx, fadeTop + i * (fadeH / FADE_STEPS) + stripH / 2);
        lv_obj_move_foreground(f);
    }
}

// Lay out and fill the visible window. This is layout() and the old onHeadlinesReady
// merged: with scrolling, which items the row widgets hold and where the rows sit are
// one decision, not two.
void render() {
    const theme_style::Intel &cfg = theme_style::intel();
    IntelSnapshot s;
    const bool have = intel_get(s) && s.valid && s.count > 0;
    s_lastCount = have ? s.count : 0;

    show(s_empty, !have);
    if (!have) {
        lv_label_set_text(s_empty, empty_reason());
        for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
            show(s_rowBox[i], false); show(s_credit[i], false); show(s_dots[i], false);
            for (int f = 0; f < FADE_STEPS; ++f) show(s_fade[i][f], false);
        }
        lv_label_set_text(s_age, "");
        exit_scroll_mode();
        return;
    }

    const bool autoSize = cfg.textSize == 0;
    const lv_font_t *font   = autoSize ? (s.count > 3 ? &lv_font_montserrat_14
                                                      : &lv_font_montserrat_16)
                                       : font_for(cfg.textSize);
    const int lineH   = lv_font_get_line_height(font);
    const int creditH = lv_font_get_line_height(&lv_font_montserrat_12);

    // How many fit. Automatic mode is the original fixed layout and always shows the whole
    // count — that is the pixel-identical promise every THEME_CAPS default keeps. An
    // explicit size gets the honest computation instead: the space between the title and
    // the age line (or the bezel, when either is hidden or moved), divided by a block of
    // two wrapped lines plus its credit.
    int step, firstCenter;
    int blockH = 2 * lineH + 2 + creditH;
    if (autoSize) {
        s_visible   = s.count;
        step        = s.count > 3 ? ROW_STEP_5 : ROW_STEP_3;
        firstCenter = s.count > 3 ? FIRST_ROW_Y - 16
                                  : FIRST_ROW_Y + ((3 - s.count) * step) / 2;
    } else {
        const int titleH  = lv_font_get_line_height(font_for(cfg.titleSize));
        const int ageH    = lv_font_get_line_height(font_for(cfg.ageSize));
        const int topB    = cfg.titleShow ? (cfg.titleY - 233) + titleH / 2 + 8 : -180;
        const int botB    = cfg.ageShow   ? (cfg.ageY   - 233) - ageH / 2 - 6 :  180;
        step = blockH + ROW_GAP;
        const int avail = botB - topB;
        s_visible = avail >= blockH ? (avail + ROW_GAP) / step : 1;
        if (s_visible > s.count) s_visible = s.count;
        if (s_visible < 1)       s_visible = 1;
        // All fit: centre the block in the space, the way the fixed layout always has.
        // Overflowing: fill from the top so the reading order and the scroll agree.
        const int span = s_visible * step - ROW_GAP;
        firstCenter = (s_visible >= s.count ? (topB + botB) / 2 - span / 2
                                            : topB)
                      + lineH;   // first headline label's own centre, not the block top
    }
    // The theme's own nudge, applied to both layouts: it moves the headlines and nothing
    // else, which is exactly what "move the block down a bit" should mean when the title
    // and the age line have positions of their own.
    firstCenter += cfg.blockOffsetY;

    // Never strand the window past the end when a shorter set arrives mid-scroll.
    const int maxScroll = s.count - s_visible;
    if (s_scroll > maxScroll) s_scroll = maxScroll < 0 ? 0 : maxScroll;
    if (s_scroll < 0)         s_scroll = 0;
    if (s.count <= s_visible) exit_scroll_mode();

    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        const bool on = i < s_visible && (s_scroll + i) < s.count;
        show(s_rowBox[i], on);
        show(s_credit[i], on);
        if (!on) { place_fade(i, false, 0, 0, 0, lineH); continue; }
        const int item = s_scroll + i;
        const int yCen = firstCenter + i * step;             // headline label centre
        const RowBox box = row_box(autoSize ? yCen : yCen - lineH + blockH / 2);
        lv_obj_set_style_text_font(s_rows[i], font, 0);
        lv_obj_set_width(s_rows[i], box.w);
        lv_obj_set_width(s_rowBox[i], box.w);
        lv_label_set_text(s_rows[i], s.items[item].text);
        lv_label_set_text(s_credit[i], s.items[item].source);
        bool cropped = false;
        if (autoSize) {
            // The original behaviour, untouched: the box takes whatever height the text
            // wants, so nothing is ever cut and nothing ever fades.
            lv_obj_set_height(s_rowBox[i], LV_SIZE_CONTENT);
        } else {
            // A chosen size gets exactly two lines. Ask the text how tall it really wants
            // to be first: a headline that fits gets no fade at all, and one that does not
            // dissolves at the bottom instead of ending in an ellipsis. An ellipsis is a
            // punctuation mark sitting where the sentence stops, and at 40 px it reads as
            // part of the headline rather than as a note about it.
            lv_point_t want;
            lv_txt_get_size(&want, s.items[item].text, font, 0, 0, box.w, LV_TEXT_FLAG_NONE);
            cropped = want.y > 2 * lineH;
            lv_obj_set_height(s_rowBox[i], 2 * lineH);
        }
        lv_obj_align(s_rowBox[i], LV_ALIGN_CENTER, box.cx, yCen);
        place_fade(i, cropped, box.cx, box.w, yCen + lineH, lineH);
        // The credit rides just under its own headline. Aligning it to the BOX rather than
        // to the label keeps it put: the label inside may be three lines tall and clipped,
        // and a credit chasing the label's real height would sit under text nobody can see.
        lv_obj_align_to(s_credit[i], s_rowBox[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }

    style_dots(s.count);   // positions itself along the bottom; hidden unless scrolling
}

} // namespace

lv_obj_t *intelview::screen() { return s_screen; }

// A knob press: toggle scroll mode when there is anything to scroll, otherwise ask now
// instead of waiting out the poll. The fetch itself belongs to the network task, so the
// refresh half only clears the timer that task is watching.
void intelview::onPress() {
    if (s_scrollMode) {
        exit_scroll_mode();
        style_dots(s_lastCount);
        Serial.println("[intel] scroll mode released");
        return;
    }
    if (s_lastCount > s_visible) {
        s_scrollMode = true;
        s_scrollActivityMs = millis();
        app_shell::setCaptured(true);
        style_dots(s_lastCount);
        Serial.println("[intel] scroll mode: turn to move, press to release");
        return;
    }
    s_lastTryMs = 0;
    Serial.println("[intel] refresh requested from the knob");
}

// A detent in scroll mode: move the window. Clamped in render(), so spinning past the
// end just holds the last page rather than wrapping — wrap-around on a five-item list
// reads as a glitch, not a feature.
void intelview::onTurn(int delta) {
    if (!s_scrollMode) return;
    s_scroll += delta;
    s_scrollActivityMs = millis();
    render();
}

// Entering from the switcher: top of the list, knob released. Same reset-on-entry the
// Flight Tracker does, and for the same reason — stale mode from a prior visit must not
// leak into this one.
void intelview::onEnter() {
    s_scroll = 0;
    exit_scroll_mode();
    render();
}

void intelview::scrollState(int &first, int &visible, int &count) {
    first = s_scroll; visible = s_visible; count = s_lastCount;
}

// ---- network step (core 0) --------------------------------------------------
// Called from the same task that polls aircraft. One request, then it is done: this is a
// sub-kilobyte fetch against a cache, so there is no burst to spread out the way the
// weather tiles need.
bool intelview::fetchStep() {
    const theme_style::Intel &cfg = theme_style::intel();
    const uint32_t now = millis();
    if (s_lastTryMs != 0) {
        IntelSnapshot cur;
        const bool have = intel_get(cur) && cur.valid;
        const uint32_t due = have ? (uint32_t)cfg.pollMinutes * 60000UL : INTEL_RETRY_MS;
        if (now - s_lastTryMs < due) return false;
    }
    s_lastTryMs = now ? now : 1;   // never leave it at 0, which means "ask immediately"

    IntelSnapshot snap;
    if (!intel_fetch(cfg.topic, cfg.source, cfg.count, snap)) return false;
    intel_store(snap);
    s_everFetched = true;
    return true;
}

// ---- UI (core 1) ------------------------------------------------------------
void intelview::onHeadlinesReady() {
    render();
    intelview::tick();
}

// The age line, refreshed on a timer rather than only on arrival: headlines that stopped
// updating should look stale, not current. The poll is the theme's own interval, so
// anything past about twice it means something is wrong and the colour says so.
void intelview::tick() {
    if (!s_age) return;
    const theme_style::Intel &cfg = theme_style::intel();
    IntelSnapshot s;
    if (!intel_get(s) || !s.valid || s.count == 0) {
        lv_label_set_text(s_age, "");
        for (auto *g : s_ageGlow) if (g) lv_label_set_text(g, "");
        return;
    }
    const uint32_t ageS = (millis() - s.fetchedMs) / 1000;
    char buf[32];
    if (ageS < 90)        snprintf(buf, sizeof(buf), "just now");
    else if (ageS < 3600) snprintf(buf, sizeof(buf), "%lu min ago", (unsigned long)(ageS / 60));
    else                  snprintf(buf, sizeof(buf), "%lu hr ago", (unsigned long)(ageS / 3600));
    lv_label_set_text(s_age, buf);
    const uint32_t staleS = (uint32_t)cfg.pollMinutes * 60U * 2U;
    lv_obj_set_style_text_color(s_age, ageS > staleS ? c_stale() : lv_color_hex(cfg.ageColor), 0);
    for (auto *g : s_ageGlow) if (g) lv_label_set_text(g, buf);
}

static void tick_cb(lv_timer_t * /*t*/) {
    if (lv_scr_act() != intelview::screen()) return;
    intelview::tick();
}

// Scroll mode's idle watchdog, the manual release's automatic twin. One second is plenty:
// the timeout is six, and half-second precision on "you stopped turning a while ago" is
// not something a person can perceive.
static void scroll_idle_cb(lv_timer_t * /*t*/) {
    if (!s_scrollMode) return;
    if (millis() - s_scrollActivityMs < SCROLL_IDLE_MS) return;
    exit_scroll_mode();
    style_dots(s_lastCount);
    Serial.println("[intel] scroll mode idle release");
}

void intelview::init() {
    const theme_style::Intel &cfg = theme_style::intel();
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(cfg.bg), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = lv_label_create(s_screen);
    lv_label_set_text(s_title, cfg.title);
    lv_obj_set_style_text_color(s_title, c_title(), 0);
    lv_obj_set_style_text_font(s_title, font_for(cfg.titleSize), 0);
    // Letter-spaced, because a short word in small caps at the top of a dial reads as a
    // label rather than as another headline.
    lv_obj_set_style_text_letter_space(s_title, 4, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, cfg.titleX - 233, cfg.titleY - 233);
    show(s_title, cfg.titleShow);

    for (int i = 0; i < INTEL_MAX_ITEMS; ++i) {
        s_rowBox[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_rowBox[i]);
        lv_obj_clear_flag(s_rowBox[i], LV_OBJ_FLAG_SCROLLABLE);
        show(s_rowBox[i], false);

        s_rows[i] = lv_label_create(s_rowBox[i]);
        lv_label_set_long_mode(s_rows[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(s_rows[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(s_rows[i], c_text(), 0);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_16, 0);
        lv_label_set_text(s_rows[i], "");
        // Top of its box, so a headline that overruns grows downward into the clip rather
        // than pushing its own first line up out of the layout.
        lv_obj_align(s_rows[i], LV_ALIGN_TOP_MID, 0, 0);

        s_credit[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_credit[i], c_source(), 0);
        lv_obj_set_style_text_font(s_credit[i], &lv_font_montserrat_12, 0);
        lv_label_set_text(s_credit[i], "");
        show(s_credit[i], false);

        s_dots[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        show(s_dots[i], false);

        // Created after the row they cover so they start above it in the sibling order,
        // and re-raised on every placement in case anything else has been moved since.
        for (int f = 0; f < FADE_STEPS; ++f) {
            lv_obj_t *o = lv_obj_create(s_screen);
            s_fade[i][f] = o;
            lv_obj_remove_style_all(o);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
            show(o, false);
        }
    }

    s_empty = lv_label_create(s_screen);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    {
        const RowBox box = row_box(0);
        lv_obj_set_width(s_empty, box.w);
        lv_obj_align(s_empty, LV_ALIGN_CENTER, box.cx, 0);
    }
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_empty, c_source(), 0);
    lv_obj_set_style_text_font(s_empty, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_empty, empty_reason());

    // Glow ring first, sharp fill after: LVGL paints siblings in creation order, and the
    // glow has to sit under the line it haloes. All hidden when the theme asks for none.
    {
        static const float dirs[AGE_GLOW_DIRS][2] = {
            {1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f}
        };
        const lv_font_t *ageFont = font_for(cfg.ageSize);
        for (int ri = 1; ri <= AGE_GLOW_RINGS; ++ri) {
            const int r = cfg.ageGlow * ri / AGE_GLOW_RINGS;
            for (int di = 0; di < AGE_GLOW_DIRS; ++di) {
                lv_obj_t *g = lv_label_create(s_screen);
                s_ageGlow[(ri - 1) * AGE_GLOW_DIRS + di] = g;
                lv_obj_set_style_text_color(g, lv_color_hex(cfg.ageGlowColor), 0);
                lv_obj_set_style_text_font(g, ageFont, 0);
                lv_obj_set_style_text_opa(g, (lv_opa_t)(90 / ri), 0);   // fainter further out
                lv_label_set_text(g, "");
                lv_obj_align(g, LV_ALIGN_CENTER,
                             cfg.ageX - 233 + (int)lroundf(dirs[di][0] * r),
                             cfg.ageY - 233 + (int)lroundf(dirs[di][1] * r));
                show(g, cfg.ageShow && cfg.ageGlow > 0);
            }
        }
    }

    s_age = lv_label_create(s_screen);
    lv_obj_set_style_text_color(s_age, lv_color_hex(cfg.ageColor), 0);
    lv_obj_set_style_text_font(s_age, font_for(cfg.ageSize), 0);
    lv_label_set_text(s_age, "");
    lv_obj_align(s_age, LV_ALIGN_CENTER, cfg.ageX - 233, cfg.ageY - 233);
    show(s_age, cfg.ageShow);

    render();
    lv_timer_create(tick_cb, 30000, nullptr);
    lv_timer_create(scroll_idle_cb, 1000, nullptr);
}
