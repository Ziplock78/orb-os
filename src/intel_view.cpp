#include "intel_view.h"
#include "intel.h"
#include "intel_client.h"
#include "config.h"
#include "theme_style.h"
#include "intel_sprite.h"
#include "curved_text.h"
#include "font_ladder.h"
#include "theme_font.h"
#include "app_shell.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#else
#include <cstdlib>
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
// The "there is more" mark: one small chevron, just past the end of the list.
//
// Two earlier attempts are worth remembering. A column of dots beside the headlines read
// as a colon someone had left on the end of the sentence — an indicator that sits inside
// the text's own line box stops being an indicator and becomes punctuation. Moving it to a
// row along the bottom fixed that but left it always-on, a permanent mark on a screen
// whose whole job is to be glanceable, so it was hidden except while scrolling; which then
// meant a resting screen said nothing at all about the story it was not showing.
//
// A chevron solves all three. It reads as "continues" rather than as a character, it is
// one mark rather than a row of them, and it carries DIRECTION, so the same element that
// says "there is more below" at rest says "you can go back up" once you are moving. Nothing
// else is needed: with at most five items, which way you can go is the whole story, and a
// dot per item was answering a question nobody asked.
constexpr int CHEV_W       = 15;   // px across
constexpr int CHEV_H       = 5;    // px deep
constexpr int CHEV_GAP     = 9;    // px between the list and the mark
constexpr uint32_t SCROLL_IDLE_MS = 6000;  // scroll mode lets go after this much stillness
// A headline too long for its rows fades out instead of ending in an ellipsis, and the
// fade runs off the RIGHT END OF THE LAST LINE, not along its underside.
//
// The underside was the first attempt and it was the wrong shape for the meaning. A
// horizontal band of dimming across the bottom of a line does not say "this sentence
// carries on", it says "this line failed to draw" — text that fades downward looks broken
// in a way text that fades rightward does not, because rightward is the direction reading
// already travels.
//
// It is a REAL ALPHA MASK on the glyphs, not background-coloured strips painted over them.
// Strips were the first working version and they carried a hidden assumption: that there
// is a flat known colour behind the text. The moment this screen learned to wear a
// background image that assumption became a bug, and the fade would have painted solid
// rectangles across somebody's photograph. Masking the text itself is correct over
// anything, which is the whole point of doing it this way round.
constexpr int FADE_FRAC    = 38;   // % of the row's width the fade reaches back across

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
const lv_font_t *font_for(int size);

// A themed face if the design shipped one for this slot, otherwise the compiled ladder at
// whatever size the design asked for. A loaded face is baked at ONE size by lv_font_conv,
// so it ignores the size slider by nature; Studio knows this and hides the slider when a
// custom face is chosen, which is the same bargain every other themed text slot makes.
const lv_font_t *slot_font(int slot, int size) {
    if (theme_font::intel_has_font(slot)) {
        switch (slot) {
            case 0: return theme_font::intel_title();
            case 1: return theme_font::intel_text();
            case 2: return theme_font::intel_source();
            default: return theme_font::intel_age();
        }
    }
    return font_for(size);
}

// The ladder itself now lives in font_ladder.h, because the splash wants it too and this
// file is not the right owner of a fact about the whole binary.
const lv_font_t *font_for(int size) { return font_ladder(size); }

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
lv_obj_t *s_rowBox[INTEL_MAX_ROWS] = {};
lv_obj_t *s_rows[INTEL_MAX_ROWS]   = {};
lv_obj_t *s_credit[INTEL_MAX_ROWS] = {};
// The age line's arc, when a theme asks for one. A canvas rather than a label, for the
// same reason the clock's banners and the scope's readouts are: LVGL has no curved text and
// no glyph rotation, so the glyphs are rotated by hand into a raster. 636 KB of PSRAM,
// taken on entry and given back on exit, and only when the theme actually curves this line.
lv_obj_t   *s_ageCanvas = nullptr;
lv_color_t *s_ageBuf    = nullptr;

lv_obj_t *s_chevDown = nullptr;   // there is more after the last row shown
lv_obj_t *s_chevUp   = nullptr;   // ...and more before the first, once you have moved
// LVGL keeps the pointer rather than copying, so these outlive the call that sets them.
lv_point_t s_chevDownPts[3] = { {0, 0}, {CHEV_W / 2, CHEV_H}, {CHEV_W, 0} };
lv_point_t s_chevUpPts[3]   = { {0, CHEV_H}, {CHEV_W / 2, 0}, {CHEV_W, CHEV_H} };
lv_obj_t *s_plateImg   = nullptr;   // the theme's background picture, behind everything
lv_obj_t *s_overlayImg = nullptr;   // its glass/CRT, in front of everything

// Where each row's fade sits, in that row box's own coordinates. Filled by render(),
// read by the draw hook below.
struct RowFade { bool on; lv_coord_t x0, y0, w, h; };
RowFade s_rowFade[INTEL_MAX_ROWS] = {};

// One shared ramp, because only one object is ever mid-draw at a time. Rebuilt only when
// the geometry it describes actually changes, which for a static screen is almost never.
lv_opa_t *s_fadeMap = nullptr;
int s_fadeMapW = 0, s_fadeMapH = 0;

const lv_opa_t *fade_map(int w, int h) {
    if (w <= 1 || h <= 0) return nullptr;
    if (s_fadeMap && s_fadeMapW == w && s_fadeMapH == h) return s_fadeMap;
    lv_opa_t *m = (lv_opa_t *)lv_mem_alloc((size_t)w * h);
    if (!m) return nullptr;
    if (s_fadeMap) lv_mem_free(s_fadeMap);
    s_fadeMap = m; s_fadeMapW = w; s_fadeMapH = h;
    // Fully opaque where the zone starts, fully clear at the right edge. Every row of the
    // ramp is identical: the gradient runs across, not down.
    for (int x = 0; x < w; ++x) {
        const lv_opa_t v = (lv_opa_t)(255 - (x * 255) / (w - 1));
        for (int y = 0; y < h; ++y) s_fadeMap[(size_t)y * w + x] = v;
    }
    return s_fadeMap;
}

lv_draw_mask_map_param_t s_maskParam;
int16_t s_maskId = -1;

// Masks live for the duration of one object's draw: added as its text is about to be
// painted, removed the moment it is done. Anything still on the stack afterwards would
// silently clip whatever drew next.
void row_draw_cb(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DRAW_MAIN_BEGIN) {
        if (i < 0 || i >= INTEL_MAX_ROWS || !s_rowFade[i].on) return;
        const lv_opa_t *map = fade_map(s_rowFade[i].w, s_rowFade[i].h);
        if (!map) return;
        lv_obj_t *obj = lv_event_get_target(e);
        lv_area_t a;
        a.x1 = obj->coords.x1 + s_rowFade[i].x0;
        a.y1 = obj->coords.y1 + s_rowFade[i].y0;
        a.x2 = a.x1 + s_rowFade[i].w - 1;
        a.y2 = a.y1 + s_rowFade[i].h - 1;
        lv_draw_mask_map_init(&s_maskParam, &a, map);
        s_maskId = lv_draw_mask_add(&s_maskParam, nullptr);
    } else if (code == LV_EVENT_DRAW_MAIN_END) {
        if (s_maskId >= 0) {
            lv_draw_mask_remove_id(s_maskId);
            lv_draw_mask_free_param(&s_maskParam);
            s_maskId = -1;
        }
    }
}
lv_obj_t *s_empty  = nullptr;       // the one line shown when there is nothing to show

uint32_t s_lastTryMs   = 0;
bool     s_everFetched = false;

// Scroll state. s_scroll is the first visible item; s_visible is how many fit at the
// theme's type size (recomputed on every render); s_lastCount is what the last snapshot
// held, so onPress can tell "scrollable" from "fits" without re-reading the store.
int      s_scroll     = 0;
int      s_visible    = INTEL_MAX_ROWS;
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
}

// Show or hide the two marks for the current window.
//
// At rest only the down chevron can appear, and only when the list genuinely overruns.
// While scrolling both are live, so the pair together says where in the list you are
// without needing a dot for every item.
void style_chevrons(int count, int lastRowBottom, int firstRowTop, int cxLast, int cxFirst) {
    const bool more   = count > s_scroll + s_visible;
    const bool before = s_scrollMode && s_scroll > 0;
    if (s_chevDown) {
        show(s_chevDown, more);
        if (more) {
            lv_obj_set_style_line_color(s_chevDown, c_source(), 0);
            lv_obj_align(s_chevDown, LV_ALIGN_CENTER, cxLast, lastRowBottom + CHEV_GAP);
        }
    }
    if (s_chevUp) {
        show(s_chevUp, before);
        if (before) {
            lv_obj_set_style_line_color(s_chevUp, c_source(), 0);
            lv_obj_align(s_chevUp, LV_ALIGN_CENTER, cxFirst, firstRowTop - CHEV_GAP);
        }
    }
}

// Park a row's fade strips over the bottom of its text box, or hide them.
//
// `cropped` comes from measuring the string rather than from guessing: lv_txt_get_size
// lays the text out at the row's real width and font and reports how tall it actually
// wants to be, so a headline that happens to fit gets no fade and one that does not gets
// exactly the fade its own overflow earned.
// Record where this row's fade belongs, in the row box's own coordinates. Nothing is drawn
// here: row_draw_cb turns it into a mask at paint time, which is the only moment the
// object's real screen position is known.
void set_fade(int row, bool cropped, int w, int lastLineTop, int lineH) {
    if (row < 0 || row >= INTEL_MAX_ROWS) return;
    const int fadeW = w * FADE_FRAC / 100;
    if (!cropped || fadeW < 2) { s_rowFade[row] = { false, 0, 0, 0, 0 }; return; }
    s_rowFade[row] = { true, (lv_coord_t)(w - fadeW), (lv_coord_t)lastLineTop,
                       (lv_coord_t)fadeW, (lv_coord_t)lineH };
}

// Lay out and fill the visible window. This is layout() and the old onHeadlinesReady
// merged: with scrolling, which items the row widgets hold and where the rows sit are
// one decision, not two.
void render() {
    const theme_style::Intel &cfg = theme_style::intel();
    uint32_t fetchedMs = 0;
    int total = 0;
    const bool have = intel_meta(fetchedMs, total) && total > 0;
    s_lastCount = have ? total : 0;

    show(s_empty, !have);
    if (!have) {
        lv_label_set_text(s_empty, empty_reason());
        show(s_chevDown, false);
        show(s_chevUp, false);
        for (int i = 0; i < INTEL_MAX_ROWS; ++i) {
            show(s_rowBox[i], false); show(s_credit[i], false);
            set_fade(i, false, 0, 0, 0);
        }
        lv_label_set_text(s_age, "");
        exit_scroll_mode();
        return;
    }

    const bool themed   = theme_font::intel_has_font(1);
    const bool autoSize = cfg.textSize == 0 && !themed;
    const lv_font_t *font = themed ? theme_font::intel_text()
                          : autoSize ? (total > 3 ? &lv_font_montserrat_14
                                                  : &lv_font_montserrat_16)
                          : font_for(cfg.textSize);
    const lv_font_t *creditFont = slot_font(2, cfg.sourceSize);
    const int lineH   = lv_font_get_line_height(font);
    const int creditH = lv_font_get_line_height(creditFont);
    // A headline is two lines plus whatever leading the theme asked for between them.
    const int gap     = cfg.lineGap;
    const int textH   = 2 * lineH + gap;

    // How many fit. Automatic mode is the original fixed layout and always shows the whole
    // count — that is the pixel-identical promise every THEME_CAPS default keeps. An
    // explicit size gets the honest computation instead: the space between the title and
    // the age line (or the bezel, when either is hidden or moved), divided by a block of
    // two wrapped lines plus its credit.
    int step, firstCenter;
    int blockH = textH + cfg.sourceGap + creditH;
    if (autoSize) {
        s_visible   = total;
        step        = total > 3 ? ROW_STEP_5 : ROW_STEP_3;
        firstCenter = total > 3 ? FIRST_ROW_Y - 16
                                : FIRST_ROW_Y + ((3 - total) * step) / 2;
    } else {
        const int titleH  = lv_font_get_line_height(slot_font(0, cfg.titleSize));
        const int ageH    = lv_font_get_line_height(slot_font(3, cfg.ageSize));
        // An explicit margin wins over the worked-out bound: a design that states where the
        // band is knows something about its own artwork that this arithmetic cannot.
        const int topB    = cfg.marginTop > 0    ? cfg.marginTop - 233
                          : cfg.titleShow        ? (cfg.titleY - 233) + titleH / 2 + 8
                                                 : -180;
        const int botB    = cfg.marginBottom > 0 ? 233 - cfg.marginBottom
                          : cfg.ageShow          ? (cfg.ageY - 233) - ageH / 2 - 6
                                                 :  180;
        step = blockH + ROW_GAP;
        const int avail = botB - topB;
        s_visible = avail >= blockH ? (avail + ROW_GAP) / step : 1;
        // The theme's own ceiling, when it set one. Never a floor: asking for six rows on a
        // dial that fits three would put half of them past the glass.
        if (cfg.onScreen > 0 && s_visible > cfg.onScreen) s_visible = cfg.onScreen;
        if (s_visible > total)          s_visible = total;
        if (s_visible > INTEL_MAX_ROWS) s_visible = INTEL_MAX_ROWS;
        if (s_visible < 1)              s_visible = 1;
        // All fit: centre the block in the space, the way the fixed layout always has.
        // Overflowing: fill from the top so the reading order and the scroll agree.
        const int span = s_visible * step - ROW_GAP;
        firstCenter = (s_visible >= total ? (topB + botB) / 2 - span / 2
                                          : topB)
                      + lineH;   // first headline label's own centre, not the block top
    }
    if (s_visible > INTEL_MAX_ROWS) s_visible = INTEL_MAX_ROWS;
    // The theme's own nudge, applied to both layouts: it moves the headlines and nothing
    // else, which is exactly what "move the block down a bit" should mean when the title
    // and the age line have positions of their own.
    firstCenter += cfg.blockOffsetY;

    // Never strand the window past the end when a shorter set arrives mid-scroll.
    const int maxScroll = total - s_visible;
    if (s_scroll > maxScroll) s_scroll = maxScroll < 0 ? 0 : maxScroll;
    if (s_scroll < 0)         s_scroll = 0;
    if (total <= s_visible) exit_scroll_mode();

    // Only the window, never the whole set: at twenty items the full struct is over 2 KB
    // and this runs on the LVGL task's stack.
    IntelItem win[INTEL_MAX_ROWS];
    int dummy = 0;
    const int got = intel_window(s_scroll, s_visible, win, dummy);

    int firstTop = 0, lastBottom = 0, cxFirst = 0, cxLast = 0;
    for (int i = 0; i < INTEL_MAX_ROWS; ++i) {
        const bool on = i < got;
        show(s_rowBox[i], on);
        show(s_credit[i], on);
        if (!on) { set_fade(i, false, 0, 0, 0); continue; }
        const int yCen = firstCenter + i * step;             // headline label centre
        const RowBox box = row_box(autoSize ? yCen : yCen - lineH + blockH / 2);
        lv_obj_set_style_text_font(s_rows[i], font, 0);
        lv_obj_set_style_text_line_space(s_rows[i], gap, 0);
        lv_obj_set_width(s_rows[i], box.w);
        lv_obj_set_width(s_rowBox[i], box.w);
        lv_label_set_text(s_rows[i], win[i].text);
        lv_label_set_text(s_credit[i], win[i].source);
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
            lv_txt_get_size(&want, win[i].text, font, 0, gap, box.w, LV_TEXT_FLAG_NONE);
            cropped = want.y > textH;
            lv_obj_set_height(s_rowBox[i], textH);
        }
        lv_obj_align(s_rowBox[i], LV_ALIGN_CENTER, box.cx, yCen);
        if (i == 0) { firstTop = yCen - textH / 2; cxFirst = box.cx; }
        // The credit hangs below the box, so the list's real bottom is past it.
        lastBottom = yCen + textH / 2 + cfg.sourceGap + creditH;
        cxLast     = box.cx;
        // The second line occupies the bottom lineH of a box textH tall, so within the box
        // its top edge is textH - lineH down from the top.
        set_fade(i, cropped, box.w, textH - lineH, lineH);
        // The credit rides just under its own headline. Aligning it to the BOX rather than
        // to the label keeps it put: the label inside may be three lines tall and clipped,
        // and a credit chasing the label's real height would sit under text nobody can see.
        lv_obj_set_style_text_font(s_credit[i], creditFont, 0);
        lv_obj_align_to(s_credit[i], s_rowBox[i], LV_ALIGN_OUT_BOTTOM_MID, 0, cfg.sourceGap);
    }

    style_chevrons(total, lastBottom, firstTop, cxLast, cxFirst);
}

// Attach the theme's plate and glass, if it ships them.
//
// Lazily, on entry, and given back on the way out: a decoded 466x466 plate is 424 KB of
// PSRAM and the glass is 651 KB, which is not something to hold while the user is looking
// at the clock. A theme baked into flash pays neither, but the same attach/detach applies
// so the two paths cannot drift.
void attach_age_canvas() {
    const theme_style::Intel &cfg = theme_style::intel();
    const bool want = cfg.ageShow && cfg.ageCurved;
    if (want && !s_ageBuf) {
        const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
        s_ageBuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
        s_ageBuf = (lv_color_t *)malloc(sz);
#endif
        if (!s_ageBuf) {
            // Say so rather than silently falling back: a curved line that quietly comes
            // back straight is the class of lie the caps ledger exists to prevent.
            Serial.printf("[intel] age canvas alloc FAILED (%u bytes) - drawing it straight\n",
                          (unsigned)sz);
        } else {
            s_ageCanvas = lv_canvas_create(s_screen);
            lv_canvas_set_buffer(s_ageCanvas, s_ageBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
            lv_obj_center(s_ageCanvas);
            lv_canvas_fill_bg(s_ageCanvas, lv_color_black(), LV_OPA_TRANSP);
            lv_obj_clear_flag(s_ageCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }
    }
    const bool curving = want && s_ageBuf;
    show(s_ageCanvas, curving);
    // The straight line and its glow ring stand down while the arc has the job.
    show(s_age, cfg.ageShow && !curving);
    for (auto *g : s_ageGlow) show(g, cfg.ageShow && !curving && cfg.ageGlow > 0);
}

void release_age_canvas() {
    if (s_ageCanvas) { lv_obj_del(s_ageCanvas); s_ageCanvas = nullptr; }
    if (s_ageBuf) {
#if defined(ESP_PLATFORM)
        heap_caps_free(s_ageBuf);
#else
        free(s_ageBuf);
#endif
        s_ageBuf = nullptr;
    }
}

void attach_art() {
    if (const lv_img_dsc_t *p = intelview::plate()) {
        if (!s_plateImg) {
            s_plateImg = lv_img_create(s_screen);
            lv_obj_clear_flag(s_plateImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }
        lv_img_set_src(s_plateImg, p);
        lv_obj_center(s_plateImg);
        lv_obj_move_background(s_plateImg);   // behind the title, the rows, everything
        show(s_plateImg, true);
    } else if (s_plateImg) {
        show(s_plateImg, false);
    }

    if (const lv_img_dsc_t *o = intelview::overlay()) {
        if (!s_overlayImg) {
            s_overlayImg = lv_img_create(s_screen);
            lv_obj_clear_flag(s_overlayImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }
        lv_img_set_src(s_overlayImg, o);
        lv_obj_center(s_overlayImg);
        lv_obj_move_foreground(s_overlayImg);   // glass is composited over the lot
        show(s_overlayImg, true);
    } else if (s_overlayImg) {
        show(s_overlayImg, false);
    }
}

} // namespace

lv_obj_t *intelview::screen() { return s_screen; }

void intelview::onExit() { intelview::sprite_release(); release_age_canvas(); }

// A knob press: toggle scroll mode when there is anything to scroll, otherwise ask now
// instead of waiting out the poll. The fetch itself belongs to the network task, so the
// refresh half only clears the timer that task is watching.
// A press asks for the headlines again. Nothing else: scrolling is what a TURN does now,
// so the button is not needed to reach any of it.
void intelview::onPress() {
    s_lastTryMs = 0;
    Serial.println("[intel] refresh requested from the knob");
}

// A detent in scroll mode: move the window. Clamped in render(), so spinning past the
// end just holds the last page rather than wrapping — wrap-around on a five-item list
// reads as a glitch, not a feature.
// A turn scrolls, straight in, with no press to arm it first. The window is clamped in
// render(), so turning past either end simply holds there rather than wrapping — wrap-around
// on a short list reads as a glitch.
//
// s_scrollMode now means only "the marks are showing", not "the knob has been taken". The
// knob is never taken on this screen: the Rock gesture is what leaves, so there is nothing
// to hand back.
void intelview::onTurn(int delta) {
    if (s_lastCount <= s_visible) return;   // nothing to scroll
    s_scroll += delta;
    s_scrollMode = true;
    s_scrollActivityMs = millis();
    render();
}

// Entering from the switcher: top of the list, knob released. Same reset-on-entry the
// Flight Tracker does, and for the same reason — stale mode from a prior visit must not
// leak into this one.
void intelview::onEnter() {
    s_scroll = 0;
    exit_scroll_mode();
    attach_art();
    attach_age_canvas();
    render();
    intelview::tick();
    // render() may have raised a fade or a dot; the glass belongs over all of it, and
    // re-asserting is cheaper than reasoning about who moved what.
    if (s_overlayImg) lv_obj_move_foreground(s_overlayImg);
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
        uint32_t seen = 0; int held = 0;
        const bool have = intel_meta(seen, held);
        const uint32_t due = have ? (uint32_t)cfg.pollMinutes * 60000UL : INTEL_RETRY_MS;
        if (now - s_lastTryMs < due) return false;
    }
    s_lastTryMs = now ? now : 1;   // never leave it at 0, which means "ask immediately"

    // Static, not a local: at twenty items this is over 2 KB, and the network task's stack
    // is not the place to find that out. Only this task ever touches it.
    static IntelSnapshot snap;
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
    uint32_t fetchedMs = 0; int held = 0;
    if (!intel_meta(fetchedMs, held) || held == 0) {
        lv_label_set_text(s_age, "");
        for (auto *g : s_ageGlow) if (g) lv_label_set_text(g, "");
        return;
    }
    const uint32_t ageS = (millis() - fetchedMs) / 1000;
    char phrase[32];
    if (ageS < 90)        snprintf(phrase, sizeof(phrase), "just now");
    else if (ageS < 3600) snprintf(phrase, sizeof(phrase), "%lu min ago", (unsigned long)(ageS / 60));
    else                  snprintf(phrase, sizeof(phrase), "%lu hr ago", (unsigned long)(ageS / 3600));

    // Substitute the phrase into the theme's own wording. Written by hand rather than with
    // snprintf's %s because the token may appear anywhere, more than once, or not at all,
    // and the surrounding words are the theme's to choose.
    char buf[80];
    {
        const char *src = cfg.ageFmt;
        size_t o = 0;
        while (*src && o + 1 < sizeof(buf)) {
            if (!strncmp(src, "{t}", 3)) {
                for (const char *p = phrase; *p && o + 1 < sizeof(buf); ++p) buf[o++] = *p;
                src += 3;
            } else {
                buf[o++] = *src++;
            }
        }
        buf[o] = '\0';
    }
    const uint32_t staleS = (uint32_t)cfg.pollMinutes * 60U * 2U;
    const lv_color_t col = ageS > staleS ? c_stale() : lv_color_hex(cfg.ageColor);

    if (cfg.ageCurved && s_ageBuf && s_ageCanvas) {
        // Repainted whole each time rather than diffed: this runs on a 30 s timer over a
        // dozen glyphs, so the clear costs less than working out what changed.
        lv_canvas_fill_bg(s_ageCanvas, lv_color_black(), LV_OPA_TRANSP);
        const curved_text::Target dst = { (uint8_t *)s_ageBuf, SCREEN_W, SCREEN_H };
        curved_text::draw_arc(dst, slot_font(3, cfg.ageSize), buf,
                              (float)(SCREEN_W / 2), (float)(SCREEN_H / 2),
                              (float)cfg.ageCurveR, cfg.ageArcDeg,
                              col, cfg.ageGlow, lv_color_hex(cfg.ageGlowColor));
        lv_obj_invalidate(s_ageCanvas);
        return;
    }

    lv_label_set_text(s_age, buf);
    lv_obj_set_style_text_color(s_age, col, 0);
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
    render();
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
    lv_obj_set_style_text_font(s_title, slot_font(0, cfg.titleSize), 0);
    // Letter-spaced, because a short word in small caps at the top of a dial reads as a
    // label rather than as another headline.
    lv_obj_set_style_text_letter_space(s_title, 4, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, cfg.titleX - 233, cfg.titleY - 233);
    show(s_title, cfg.titleShow);

    for (int i = 0; i < INTEL_MAX_ROWS; ++i) {
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
        lv_obj_add_event_cb(s_rows[i], row_draw_cb, LV_EVENT_DRAW_MAIN_BEGIN, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s_rows[i], row_draw_cb, LV_EVENT_DRAW_MAIN_END,   (void *)(intptr_t)i);

        s_credit[i] = lv_label_create(s_screen);
        lv_obj_set_style_text_color(s_credit[i], c_source(), 0);
        lv_obj_set_style_text_font(s_credit[i], &lv_font_montserrat_12, 0);
        lv_label_set_text(s_credit[i], "");
        show(s_credit[i], false);



    }

    // Drawn rather than set in a font: LVGL's built-in symbols are a fixed weight and size
    // that would not follow the theme's own line work, and two lines cost nothing.
    for (int k = 0; k < 2; ++k) {
        lv_obj_t *c = lv_line_create(s_screen);
        (k == 0 ? s_chevDown : s_chevUp) = c;
        lv_line_set_points(c, k == 0 ? s_chevDownPts : s_chevUpPts, 3);
        lv_obj_set_style_line_width(c, 2, 0);
        lv_obj_set_style_line_rounded(c, true, 0);
        lv_obj_set_style_line_color(c, c_source(), 0);
        show(c, false);
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
        const lv_font_t *ageFont = slot_font(3, cfg.ageSize);
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
    lv_obj_set_style_text_font(s_age, slot_font(3, cfg.ageSize), 0);
    lv_label_set_text(s_age, "");
    lv_obj_align(s_age, LV_ALIGN_CENTER, cfg.ageX - 233, cfg.ageY - 233);
    show(s_age, cfg.ageShow);

    attach_art();
    attach_age_canvas();
    render();
    if (s_overlayImg) lv_obj_move_foreground(s_overlayImg);
    lv_timer_create(tick_cb, 30000, nullptr);
    lv_timer_create(scroll_idle_cb, 1000, nullptr);
}
