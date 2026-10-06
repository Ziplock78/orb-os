// Clock app for the shell. Three faces, cycled by pushing the knob:
//   IMPERIAL — blue Imperial Signal dial, silver dauphine hands, center seconds, date.
//   AVIATOR  — cream WWII aviator dial, dark hands, small seconds in the 6-o'clock sub-dial.
//   DIGITAL  — big hand-drawn 24-hour readout (seven-segment style) + the date.
//
// Time comes from the system clock (RTC-seeded, NTP-synced; see main.cpp). TZ is
// applied at boot, so getLocalTime() returns local time.
#include "clock_view.h"
#include "display.h"      // orb_screen_covered(): do not redraw under a cover
#include "theme_audio.h"  // the theme's tick bank
#ifdef ARDUINO
#include "audio.h"
#else
// The simulator builds no audio module. Compiled out rather than faked, the same way
// theme_audio and wind_notice do it.
#define audio_play_pcm(p, n, m, t) ((void)0)
#define audio_tick_level() 100
#endif
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
// Desktop/native build (no ESP32 core): shim the two Arduino-only calls this
// file uses so it can run in the LVGL simulator for real screenshots.
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <ctime>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } void println(const char *s) const { puts(s); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void  heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <lvgl.h>
#include <time.h>
#include <sys/time.h>   // gettimeofday: the sweeping hand needs the fraction of a second
#include <math.h>
#include <string.h>
#include <ctype.h>
#include "config.h"
#include "app_theme.h"
#include "office_sprite.h"
#include "office_minute_img_meta.h"
#include "office_hour_img_meta.h"

// NO TIME YET. Until the RTC or NTP has set the clock, getLocalTime() says no, and this
// screen used to draw nothing at all: a black disc, on a theme whose dial is drawn here.
// The first stranger to power-cycle an Orb without a coin cell in the RTC read that as a
// broken clock (CanadianAvenger, 2.16.17), and it did look like one. A clock that has not
// been set shows its face at twelve, which every oven and microwave has taught people to
// read correctly, so that is what this draws: the dial, the hands at 12:00 with the seconds
// running, and no date, because a date would be an invented one. The real time replaces it
// on the tick after it arrives.
#include "orb_time.h"   // one honest read of the wall clock; NOT Arduino's retrying getLocalTime
#include "orb_text_case.h"   // ALL CAPS on a finished line, THEME_CAPS 53

static bool s_noTime = false;

// Reads the clock ONCE, through orb_local_time(). It used to call Arduino's
// getLocalTime(ti, 0), which returns false without reading anything at all if the
// millisecond counter ticks between its two adjacent millis() calls; orb_time.h has the
// whole story. That false is what put the hands at twelve for a frame on every theme.
static void time_for_face(struct tm *ti) {
    if (orb_local_time(ti)) { s_noTime = false; return; }
    ti->tm_hour = 0; ti->tm_min = 0;   // tm_sec keeps running from the system clock
    s_noTime = true;
}
#include "dial_img.h"      // DIAL_IMG  — Imperial Signal (blue)
#include "dial_avi.h"      // DIAL_AVI  — Aviator (cream), AVI_SUB_X/Y sub-dial centre
#include "hand_hour_img.h" // HAND_HOUR_IMG — owner's real hour hand (trefoil tip), rotated at runtime
#include "hand_min_img.h"  // HAND_MIN_IMG  — owner's real minute hand (lance tip), rotated at runtime
#include "hand_hour_shadow_img.h" // HAND_HOUR_SHADOW_IMG — pre-blurred black silhouette of the hour hand
#include "hand_min_shadow_img.h"  // HAND_MIN_SHADOW_IMG  — pre-blurred black silhouette of the minute hand
#include "custom_clock.h"   // CUSTOM_CLOCK — text banners + fallback bg for the pushed design
#include "custom_hands.h"   // CUSTOM_HAS_* / CUSTOM_*_PIVOT_* / CUSTOM_*_BLEND / CUSTOM_HAND_ORDER
#include "custom_text.h"    // CUSTOM_HAS_TEXT* (compile-time show/hide gate) / CUSTOM_TEXT*_FONT (compiled glyphs, not per-theme — see theme_style.h)
#include "custom_sprite.h"  // custom_plate()/custom_overlay()/custom_hand()
#include "theme_style.h"
#include "theme_font.h"   // per-theme fonts, with the compiled font as fallback    // per-theme bg/text position/color/format — the runtime half of custom_text.h's macros (theme_style.h explains what stays compile-time and why)

// ---- palette ----------------------------------------------------------------
static const lv_color_t COL_HAND      = LV_COLOR_MAKE(0xE4, 0xE9, 0xF0);  // imperial silver
static const lv_color_t COL_HAND_EDGE = LV_COLOR_MAKE(0x0A, 0x16, 0x28);  // imperial hand outline
static const lv_color_t COL_DATE      = LV_COLOR_MAKE(0xF2, 0xF5, 0xF9);
static const lv_color_t COL_LUME      = LV_COLOR_MAKE(0xDA, 0xCF, 0xA6);  // aged cream lume fill
static const lv_color_t COL_LUME_EDGE = LV_COLOR_MAKE(0x38, 0x2E, 0x18);  // dark sepia outline
static const lv_color_t COL_BRASS     = LV_COLOR_MAKE(0x9C, 0x7B, 0x44);  // brass centre boss
static const lv_color_t COL_GOLD      = LV_COLOR_MAKE(0xCB, 0xA5, 0x54);  // polished gold Breguet hands
static const lv_color_t COL_RED       = LV_COLOR_MAKE(0xB2, 0x3A, 0x2C);  // red seconds hand
static const lv_color_t COL_DATE_DARK = LV_COLOR_MAKE(0x2A, 0x24, 0x18);  // date text on cream
static const lv_color_t COL_DIGIT     = LV_COLOR_MAKE(0xE8, 0xEC, 0xF1);
static const lv_color_t COL_BLACK     = LV_COLOR_MAKE(0x00, 0x00, 0x00);
// DIGITAL face: cool cyan-white lit segments over faint "ghost" off-segments, like a real
// backlit seven-segment LCD/VFD. Weekday strip dims every day except today.
static const lv_color_t COL_SEG_ON    = LV_COLOR_MAKE(0xDE, 0xEE, 0xFF);  // lit segment (cool white, faint cyan)
static const lv_color_t COL_SEG_OFF   = LV_COLOR_MAKE(0x11, 0x18, 0x22);  // unlit ghost segment
static const lv_color_t COL_WK_ON     = LV_COLOR_MAKE(0xDE, 0xEE, 0xFF);  // today
static const lv_color_t COL_WK_OFF    = LV_COLOR_MAKE(0x39, 0x45, 0x52);  // other weekdays

static constexpr float CX = SCREEN_CX;   // 233 (main dial centre)
static constexpr float CY = SCREEN_CY;   // 233
static constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

static constexpr int DATE_WIN_X = 233;   // Imperial date-window centre
static constexpr int DATE_WIN_Y = 328;
static constexpr float AVI_DATE_R    = 184.0f;  // date banner arc radius from the centre
static constexpr float AVI_DATE_MID  = 180.0f;  // centred at 6 o'clock
static constexpr float AVI_DATE_STEP = 4.0f;    // degrees between characters

// FACE_OFFICE is a light-background
// face and the other three are all dark dial/bitmap art, so mixing it in would look broken
// either way round. The Office app theme (see app_theme.h) always shows it instead, exactly
// like radar_view.cpp forces its own scope skin when Office is active.
// FACE_CUSTOM is a design pushed from Launch Kit (see custom_clock.h). Like Office it's
// outside the knob push-cycle: when CUSTOM_CLOCK.active it's forced and shown on its own,
// so the sim always displays exactly the design that was pushed.
enum Face { FACE_AVIATOR, FACE_IMPERIAL, FACE_DIGITAL, FACE_OFFICE, FACE_CUSTOM, FACE_COUNT };

static Face        s_face   = FACE_AVIATOR;   // WWII aviator is the default face
static lv_obj_t   *s_screen = nullptr;
static lv_obj_t   *s_canvas = nullptr;
static lv_color_t *s_buf    = nullptr;
static lv_obj_t   *s_hourImg = nullptr;   // AVIATOR: rotated gold Breguet hands (HAND_IMG sprite)
static lv_obj_t   *s_minImg  = nullptr;
static lv_obj_t   *s_hourShadow = nullptr;   // soft drop shadow of each hand, offset toward 7 o'clock
static lv_obj_t   *s_minShadow  = nullptr;   // (fixed light direction, so it doesn't rotate with the hand)

// Shadow offset: a fixed screen-space translation (not rotated with the hand), simulating
// a light source raising the hand slightly off the dial. Points toward the 7-o'clock mark
// (210 deg clockwise from 12): dx = sin(210deg), dy = -cos(210deg).
static constexpr float HAND_SHADOW_DX = -4.0f;
static constexpr float HAND_SHADOW_DY =  7.0f;

// ---- drawing helpers --------------------------------------------------------
static inline lv_point_t P(float x, float y) {
    lv_point_t p;
    p.x = (lv_coord_t)lroundf(x);
    p.y = (lv_coord_t)lroundf(y);
    return p;
}

// Tapered "dauphine" hand pivoting at (px_c, py_c).
static void draw_hand_at(float pxc, float pyc, float angDeg, float len, float tail,
                         float hw, lv_color_t col) {
    const float a  = angDeg * DEG2RAD;
    const float dx = sinf(a),  dy = -cosf(a);
    const float qx = cosf(a),  qy =  sinf(a);
    const float sx = pxc + len*0.16f*dx, sy = pyc + len*0.16f*dy;
    lv_point_t pts[4] = {
        P(pxc + len*dx,  pyc + len*dy),
        P(sx + hw*qx,    sy + hw*qy),
        P(pxc - tail*dx, pyc - tail*dy),
        P(sx - hw*qx,    sy - hw*qy),
    };
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa   = LV_OPA_COVER;
    lv_canvas_draw_polygon(s_canvas, pts, 4, &d);
}

// Hand with a thin contrasting outline (fill drawn over a slightly larger edge).
static void draw_hand_edged(float pxc, float pyc, float angDeg, float len, float tail,
                            float hw, lv_color_t fill, lv_color_t edge) {
    draw_hand_at(pxc, pyc, angDeg, len + 1.5f, tail + 1.5f, hw + 1.4f, edge);
    draw_hand_at(pxc, pyc, angDeg, len,        tail,        hw,        fill);
}

static void draw_disc(float ccx, float ccy, float r, lv_color_t col) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = LV_RADIUS_CIRCLE;
    lv_canvas_draw_rect(s_canvas, (lv_coord_t)lroundf(ccx - r), (lv_coord_t)lroundf(ccy - r),
                        (lv_coord_t)lroundf(2*r), (lv_coord_t)lroundf(2*r), &d);
}

static void draw_round_rect(float x, float y, float w, float h, float radius, lv_color_t col) {
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = col;
    d.bg_opa   = LV_OPA_COVER;
    d.radius   = (lv_coord_t)lroundf(radius);
    lv_canvas_draw_rect(s_canvas, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y),
                        (lv_coord_t)lroundf(w), (lv_coord_t)lroundf(h), &d);
}

// A thin needle (line) pivoting at (pxc,pyc), for the sub-seconds hand.
static void draw_needle_at(float pxc, float pyc, float angDeg, float len, float tail,
                           float width, lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    const float a  = angDeg * DEG2RAD;
    const float dx = sinf(a), dy = -cosf(a);
    lv_point_t sp[2] = { P(pxc - tail*dx, pyc - tail*dy), P(pxc + len*dx, pyc + len*dy) };
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.opa   = opa;
    ld.width = (lv_coord_t)lroundf(width);
    ld.round_start = 1;
    ld.round_end   = 1;
    lv_canvas_draw_line(s_canvas, sp, 2, &ld);
}

// ---- IMPERIAL face ----------------------------------------------------------
static void draw_imperial(const struct tm *ti) {
    memcpy(s_buf, DIAL_IMG, sizeof(DIAL_IMG));

    if (!s_noTime) {
        char ds[4];
        snprintf(ds, sizeof(ds), "%d", ti->tm_mday);
        lv_draw_label_dsc_t ld;
        lv_draw_label_dsc_init(&ld);
        ld.color = COL_DATE;
        ld.font  = &lv_font_montserrat_20;
        ld.align = LV_TEXT_ALIGN_CENTER;
        lv_canvas_draw_text(s_canvas, DATE_WIN_X - 24, DATE_WIN_Y - 12, 48, &ld, ds);
    }

    const float sec  = ti->tm_sec;
    const float mins = ti->tm_min + sec / 60.0f;
    const float hrs  = (ti->tm_hour % 12) + mins / 60.0f;

    draw_hand_edged(CX, CY, hrs  * 30.0f, 116, 20, 7.0f, COL_HAND, COL_HAND_EDGE);
    draw_hand_edged(CX, CY, mins * 6.0f,  190, 26, 5.5f, COL_HAND, COL_HAND_EDGE);

    draw_needle_at(CX, CY, sec * 6.0f, 196, 48, 4, COL_HAND_EDGE);
    draw_needle_at(CX, CY, sec * 6.0f, 196, 48, 2, COL_HAND);
    draw_disc(CX, CY, 8, COL_HAND);
    draw_disc(CX, CY, 3, COL_BLACK);
}

// Draw text curved along an arc centred at (cx,cy), radius R, centred on midDeg
// (clock angle: 0 = 12 o'clock, 180 = 6). Characters stay upright along the curve.
static void draw_arc_text(float cx, float cy, float R, float midDeg, float stepDeg,
                          const char *txt, const lv_font_t *font, lv_color_t col) {
    const int n = (int)strlen(txt);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = col;
    ld.font  = font;
    ld.align = LV_TEXT_ALIGN_CENTER;
    const float halfH = lv_font_get_line_height(font) * 0.5f;
    for (int i = 0; i < n; ++i) {
        const float a = (midDeg + ((n - 1) * 0.5f - i) * stepDeg) * DEG2RAD;
        const float x = cx + R * sinf(a);
        const float y = cy - R * cosf(a);
        char c[2] = { txt[i], 0 };
        lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(x - 12), (lv_coord_t)lroundf(y - halfH), 24, &ld, c);
    }
}

// ---- AVIATOR face -----------------------------------------------------------
static void draw_aviator(const struct tm *ti) {
    memcpy(s_buf, DIAL_AVI, sizeof(DIAL_AVI));

    // date curved along the banner at the bottom ("Mon 27th")
    if (!s_noTime) {
        int day = ti->tm_mday;
        const char *suf = "th";
        if (day < 11 || day > 13) {
            switch (day % 10) { case 1: suf = "st"; break; case 2: suf = "nd"; break; case 3: suf = "rd"; break; }
        }
        char wd[8]; strftime(wd, sizeof(wd), "%a", ti);
        char ds[16]; snprintf(ds, sizeof(ds), "%s %d%s", wd, day, suf);
        draw_arc_text(CX, CY, AVI_DATE_R, AVI_DATE_MID, AVI_DATE_STEP, ds, &lv_font_montserrat_18, COL_DATE_DARK);
    }

    const float sec  = ti->tm_sec;
    const float mins = ti->tm_min + sec / 60.0f;
    const float hrs  = (ti->tm_hour % 12) + mins / 60.0f;

    // red small seconds in the sub-dial — drawn first so the hour/minute hands sit on top
    draw_needle_at(AVI_SUB_X, AVI_SUB_Y, sec * 6.0f, 44, 10, 2, COL_RED);
    draw_disc(AVI_SUB_X, AVI_SUB_Y, 3, COL_RED);

    // centre boss on the canvas, under the hand sprites — it shows through the ring holes
    // as the centre pin
    draw_disc(CX, CY, 9, COL_LUME_EDGE);
    draw_disc(CX, CY, 5, COL_GOLD);

    // gold Breguet hour + minute hands: the owner's real hands (two distinct cropped
    // shapes — trefoil-tip hour, lance-tip minute — not one shape scaled), each rotated
    // around its own pivot ring. LVGL angle is 0.1-degree units, clockwise, 0 = tip up.
    if (s_hourImg && s_minImg) {
        const int16_t hourAngle = (int16_t)lroundf(hrs  * 300.0f);   // 30 deg/hr * 10
        const int16_t minAngle  = (int16_t)lroundf(mins * 60.0f);    // 6 deg/min * 10
        lv_obj_clear_flag(s_hourImg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_minImg,  LV_OBJ_FLAG_HIDDEN);
        lv_img_set_angle(s_hourImg, hourAngle);
        lv_img_set_angle(s_minImg,  minAngle);
        if (s_hourShadow && s_minShadow) {
            lv_obj_clear_flag(s_hourShadow, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_minShadow,  LV_OBJ_FLAG_HIDDEN);
            lv_img_set_angle(s_hourShadow, hourAngle);   // same rotation as the hand, fixed offset
            lv_img_set_angle(s_minShadow,  minAngle);    // does the rest — see HAND_SHADOW_DX/DY
            lv_obj_move_foreground(s_hourShadow);
            lv_obj_move_foreground(s_minShadow);
        }
        lv_obj_move_foreground(s_hourImg);
        lv_obj_move_foreground(s_minImg);
    }
}

// ---- DIGITAL face (seven-segment) -------------------------------------------
// segment bits: a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40
static const uint8_t SEG[10] = { 0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F };

static void draw_digit(float ox, float oy, float w, float h, float t, uint8_t mask, lv_color_t col) {
    const float r  = t * 0.5f;
    const float hh = h * 0.5f;
    if (mask & 0x01) draw_round_rect(ox,         oy,               w, t,  r, col);
    if (mask & 0x40) draw_round_rect(ox,         oy + hh - t*0.5f, w, t,  r, col);
    if (mask & 0x08) draw_round_rect(ox,         oy + h - t,       w, t,  r, col);
    if (mask & 0x20) draw_round_rect(ox,         oy,               t, hh, r, col);
    if (mask & 0x02) draw_round_rect(ox + w - t, oy,               t, hh, r, col);
    if (mask & 0x10) draw_round_rect(ox,         oy + hh,          t, hh, r, col);
    if (mask & 0x04) draw_round_rect(ox + w - t, oy + hh,          t, hh, r, col);
}

// Draw one seven-segment cell with the real-display look: every segment faintly lit as a
// dark "ghost", then the active segments drawn bright on top.
static void draw_seg_cell(float ox, float oy, float w, float h, float t, uint8_t mask) {
    draw_digit(ox, oy, w, h, t, 0x7F, COL_SEG_OFF);   // ghost: all seven segments, dim
    draw_digit(ox, oy, w, h, t, mask, COL_SEG_ON);    // lit segments, bright
}

static void draw_digital(const struct tm *ti) {
    lv_canvas_fill_bg(s_canvas, COL_BLACK, LV_OPA_COVER);

    // --- time HH:MM (24-hour), the hero element, upper-centre -----------------
    const int digits[4] = { ti->tm_hour/10, ti->tm_hour%10, ti->tm_min/10, ti->tm_min%10 };
    const float w = 72, h = 150, t = 16, gap = 12, colonW = 24;
    const float totalW = 4 * w + 4 * gap + colonW;
    float x  = (SCREEN_W - totalW) * 0.5f;
    const float oy = 108;

    draw_seg_cell(x, oy, w, h, t, SEG[digits[0]]); x += w + gap;
    draw_seg_cell(x, oy, w, h, t, SEG[digits[1]]); x += w + gap;
    draw_disc(x + colonW*0.5f, oy + h*0.36f, t*0.55f, COL_SEG_ON);
    draw_disc(x + colonW*0.5f, oy + h*0.64f, t*0.55f, COL_SEG_ON);
    x += colonW + gap;
    draw_seg_cell(x, oy, w, h, t, SEG[digits[2]]); x += w + gap;
    draw_seg_cell(x, oy, w, h, t, SEG[digits[3]]);

    if (s_noTime) return;   // no weekday and no date until there is a real one to show

    // --- weekday strip MO..SU, today lit and underlined, the rest dim ----------
    static const char *WD[7] = { "MO", "TU", "WE", "TH", "FR", "SA", "SU" };
    const int today = (ti->tm_wday + 6) % 7;   // tm_wday: 0=Sun; strip is Monday-first
    const float wy = 296, cellW = 52, stripW = cellW * 7;
    const float sx = (SCREEN_W - stripW) * 0.5f;
    lv_draw_label_dsc_t wl;
    lv_draw_label_dsc_init(&wl);
    wl.font  = &lv_font_montserrat_16;
    wl.align = LV_TEXT_ALIGN_CENTER;
    for (int i = 0; i < 7; ++i) {
        wl.color = (i == today) ? COL_WK_ON : COL_WK_OFF;
        lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(sx + i * cellW),
                            (lv_coord_t)lroundf(wy), (lv_coord_t)lroundf(cellW), &wl, WD[i]);
    }
    draw_round_rect(sx + today * cellW + 10, wy + 24, cellW - 20, 3, 1.5f, COL_WK_ON);

    // --- date: DD (seven-segment) + month abbreviation (bright caps) -----------
    const int dd[2] = { ti->tm_mday/10, ti->tm_mday%10 };
    char mon[8];
    strftime(mon, sizeof(mon), "%b", ti);
    for (char *p = mon; *p; ++p) *p = (char)toupper((unsigned char)*p);

    const float dw = 40, dh = 66, dt = 9, dgap = 8, groupGap = 22;
    lv_point_t msz;
    lv_txt_get_size(&msz, mon, &lv_font_montserrat_28, 0, 0, LV_COORD_MAX, 0);
    const float dnumW = 2 * dw + dgap;
    const float groupW = dnumW + groupGap + msz.x;
    float gx = (SCREEN_W - groupW) * 0.5f;
    const float gy = 352;

    draw_seg_cell(gx, gy, dw, dh, dt, SEG[dd[0]]); gx += dw + dgap;
    draw_seg_cell(gx, gy, dw, dh, dt, SEG[dd[1]]); gx += dw;

    lv_draw_label_dsc_t md;
    lv_draw_label_dsc_init(&md);
    md.font  = &lv_font_montserrat_28;
    md.color = COL_SEG_ON;
    md.align = LV_TEXT_ALIGN_LEFT;
    const float monY = gy + (dh - lv_font_get_line_height(&lv_font_montserrat_28)) * 0.5f;
    lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(gx + groupGap),
                        (lv_coord_t)lroundf(monY), (lv_coord_t)lroundf(msz.x + 8), &md, mon);
}

// ---- OFFICE face (modern/light — see app_theme.h) ---------------------------
// Big 24-hour numerals + date, both sitting entirely ABOVE the hand pivot (which sits at
// the dial's true centre, matching the reference — a prior version had the pivot cutting
// through the middle of the time digits instead). Both hands are baked sprites (see
// office_sprite.h / office_minute_img_meta.h / office_hour_img_meta.h) — real photo/
// render crops, not procedural drawing. The minute hand's rim glow is baked into its
// same image so the two can never drift apart as they rotate. No tick marks and no
// seconds hand — the reference shows neither. lv_font_montserrat_48 is the largest font
// baked into this build (see lv_conf.h); the reference's numerals run bigger than that,
// but adding a larger baked font is a separate asset job.
//
// Both hands are now real photo/render crops (see office_sprite.h), not procedural
// drawing — the hour hand's own blurred taper comes from the source art, same as the
// minute hand's glow does.
//
// The bake crop assumes each source's full frame maps 1:1 to the panel's full diameter.
// Zion's second minute-hand crop (the current one) was already recropped tight to the
// glow, reaching to within a few percent of its own frame edge, so this needs little to
// no extra scaling — unlike the original wide-margin crop, which needed 1.3x to close
// the gap to the bezel. The hour hand's own pivot-to-tip reach already lands at a
// sensible fraction of the minute hand's length straight out of the bake, so it stays
// at 1.0 unless that changes.
static constexpr float OFFICE_MINUTE_IMG_ZOOM = 1.0f;
static constexpr float OFFICE_HOUR_IMG_ZOOM   = 1.0f;

static inline void unpack565(uint16_t v, uint8_t &r, uint8_t &g, uint8_t &b) {
    r = (uint8_t)(((v >> 11) & 0x1F) << 3);
    g = (uint8_t)(((v >> 5)  & 0x3F) << 2);
    b = (uint8_t)((v & 0x1F) << 3);
}

// Rotates+scales a baked hand sprite by hand and alpha-blends it straight into the
// canvas's own pixel buffer — see the comment at this function's call sites in
// draw_office() for why this bypasses LVGL's normal lv_img-over-lv_canvas compositing.
// Bilinear (4-sample) rather than nearest-neighbor: at zoom > 1 the source is being
// upscaled, and nearest-neighbor made curves visibly stair-step next to the crisp
// antialiased digits. Each sample's color is weighted by its own alpha (premultiplied-
// style) so the fully-transparent pixels bordering the shape — stored as plain
// (0,0,0,0) — don't drag a dark fringe into the blend at the edges.
static void blend_office_sprite(const lv_img_dsc_t *spr, int pivotX, int pivotY,
                                float baselineDeg, float zoom, float angleDeg) {
    if (!spr || !s_buf) return;
    const uint8_t *src = (const uint8_t *)spr->data;
    const int sw = (int)spr->header.w, sh = (int)spr->header.h;

    const float th = (angleDeg - baselineDeg) * DEG2RAD;
    const float ct = cosf(th), st = sinf(th);
    const float invZoom = 1.0f / zoom;

    const float reachX = fmaxf((float)pivotX, (float)(sw - pivotX));
    const float reachY = fmaxf((float)pivotY, (float)(sh - pivotY));
    const float reach  = sqrtf(reachX * reachX + reachY * reachY) * zoom;
    const int x0 = (int)fmaxf(0.0f, CX - reach), x1 = (int)fminf((float)SCREEN_W - 1, CX + reach);
    const int y0 = (int)fmaxf(0.0f, CY - reach), y1 = (int)fminf((float)SCREEN_H - 1, CY + reach);

    for (int dy = y0; dy <= y1; ++dy) {
        const float oy = dy - CY;
        for (int dx = x0; dx <= x1; ++dx) {
            const float ox = dx - CX;
            // inverse-rotate + inverse-scale the destination offset into the sprite's own frame
            const float sxf = (ox * ct + oy * st) * invZoom + pivotX;
            const float syf = (-ox * st + oy * ct) * invZoom + pivotY;

            const int sx0 = (int)floorf(sxf), sy0 = (int)floorf(syf);
            const int sx1 = sx0 + 1, sy1 = sy0 + 1;
            if (sx0 < 0 || sy0 < 0 || sx1 >= sw || sy1 >= sh) continue;
            const float fx = sxf - sx0, fy = syf - sy0;

            uint8_t r00, g00, b00, r10, g10, b10, r01, g01, b01, r11, g11, b11;
            const uint8_t *p00 = src + ((size_t)sy0 * sw + sx0) * 3;
            const uint8_t *p10 = src + ((size_t)sy0 * sw + sx1) * 3;
            const uint8_t *p01 = src + ((size_t)sy1 * sw + sx0) * 3;
            const uint8_t *p11 = src + ((size_t)sy1 * sw + sx1) * 3;
            unpack565((uint16_t)(p00[0] | (p00[1] << 8)), r00, g00, b00);
            unpack565((uint16_t)(p10[0] | (p10[1] << 8)), r10, g10, b10);
            unpack565((uint16_t)(p01[0] | (p01[1] << 8)), r01, g01, b01);
            unpack565((uint16_t)(p11[0] | (p11[1] << 8)), r11, g11, b11);
            const uint8_t a00 = p00[2], a10 = p10[2], a01 = p01[2], a11 = p11[2];

            const float w00 = (1-fx)*(1-fy), w10 = fx*(1-fy), w01 = (1-fx)*fy, w11 = fx*fy;
            const float aF = a00*w00 + a10*w10 + a01*w01 + a11*w11;
            if (aF < 8) continue;   // skip the faintest antialiasing fringe

            const float aw00 = a00*w00, aw10 = a10*w10, aw01 = a01*w01, aw11 = a11*w11;
            const float aSum = aw00 + aw10 + aw01 + aw11;
            const float rF = (r00*aw00 + r10*aw10 + r01*aw01 + r11*aw11) / aSum;
            const float gF = (g00*aw00 + g10*aw10 + g01*aw01 + g11*aw11) / aSum;
            const float bF = (b00*aw00 + b10*aw10 + b01*aw01 + b11*aw11) / aSum;

            lv_color_t srcCol = LV_COLOR_MAKE((uint8_t)rF, (uint8_t)gF, (uint8_t)bF);
            lv_color_t *dstPx = &s_buf[dy * SCREEN_W + dx];
            *dstPx = lv_color_mix(srcCol, *dstPx, (lv_opa_t)lroundf(aF));
        }
    }
}

static void draw_office_minute_sprite(float minAngle) {
    blend_office_sprite(office_minute_sprite(), OFFICE_MINUTE_IMG_PIVOT_X, OFFICE_MINUTE_IMG_PIVOT_Y,
                        OFFICE_MINUTE_IMG_BASELINE_DEG_X10 / 10.0f, OFFICE_MINUTE_IMG_ZOOM, minAngle);
}

static void draw_office_hour_sprite(float hourAngle) {
    blend_office_sprite(office_hour_sprite(), OFFICE_HOUR_IMG_PIVOT_X, OFFICE_HOUR_IMG_PIVOT_Y,
                        OFFICE_HOUR_IMG_BASELINE_DEG_X10 / 10.0f, OFFICE_HOUR_IMG_ZOOM, hourAngle);
}

static void draw_office(const struct tm *ti) {
    const AppPalette &pal = app_theme::palette();
    lv_canvas_fill_bg(s_canvas, pal.bg, LV_OPA_COVER);

    char hh[3]; snprintf(hh, sizeof(hh), "%02d", ti->tm_hour);
    char mm[3]; snprintf(mm, sizeof(mm), "%02d", ti->tm_min);
    lv_point_t hsz, msz;
    lv_txt_get_size(&hsz, hh, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, 0);
    lv_txt_get_size(&msz, mm, &lv_font_montserrat_48, 0, 0, LV_COORD_MAX, 0);
    const float handGap = 34.0f;   // room for the hands' pivot dot between HH and MM

    // Pivot at the dial's TRUE centre — the reference's dot sits right there, not offset.
    // Time, then date, then the pivot, strictly stacked top-to-bottom with no overlap.
    const float handY     = CY;
    const float pivotGap  = 30.0f;   // date line -> pivot
    const float dateGap   = 14.0f;   // time digits -> date line
    const float dateLineH = lv_font_get_line_height(&lv_font_montserrat_20);
    const float dateY     = handY - pivotGap - dateLineH;
    const float textY     = dateY - dateGap - hsz.y;

    const float totalW = hsz.x + handGap + msz.x;
    const float startX = CX - totalW * 0.5f;

    const float sec  = ti->tm_sec;
    const float mins = ti->tm_min + sec / 60.0f;
    const float hrs  = (ti->tm_hour % 12) + mins / 60.0f;
    const float minAngle  = mins * 6.0f;
    const float hourAngle = hrs  * 30.0f;

    lv_draw_label_dsc_t td;
    lv_draw_label_dsc_init(&td);
    td.font  = &lv_font_montserrat_48;
    td.color = pal.ink;
    td.align = LV_TEXT_ALIGN_LEFT;
    lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(startX), (lv_coord_t)lroundf(textY),
                        (lv_coord_t)lroundf(hsz.x + 4), &td, hh);
    lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(startX + hsz.x + handGap), (lv_coord_t)lroundf(textY),
                        (lv_coord_t)lroundf(msz.x + 4), &td, mm);

    // Both hands: baked sprites (see office_sprite.h), rotated and alpha-blended directly
    // into the canvas buffer — NOT separate lv_img objects. LVGL's normal lv_img-over-
    // lv_canvas compositing (two sibling objects) turned a sprite's whole bounding box
    // opaque wherever it overlapped canvas-drawn content, for reasons that didn't trace
    // back to the image data itself (verified: correct alpha bytes, correct header,
    // matches the working Aviator sprite's format exactly, reproducible with antialiasing
    // off / angle=0 / off-screen position all isolating the SAME cause: any overlap with
    // the canvas). This sidesteps whatever that was by doing the rotation and blending by
    // hand straight into the same buffer the text draws into. Hour drawn first so the
    // minute hand's glow, which reaches much farther, sits on top at the pivot.
    draw_office_hour_sprite(hourAngle);
    draw_office_minute_sprite(minAngle);

    if (s_noTime) return;   // the date line would be an invented one
    char wd[16]; strftime(wd, sizeof(wd), "%A", ti);
    char mo[16]; strftime(mo, sizeof(mo), "%B", ti);
    char dateStr[40];
    snprintf(dateStr, sizeof(dateStr), "%s, %s %d", wd, mo, ti->tm_mday);
    lv_draw_label_dsc_t dd;
    lv_draw_label_dsc_init(&dd);
    dd.font  = &lv_font_montserrat_20;
    dd.color = pal.soft;
    dd.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(CX - 200), (lv_coord_t)lroundf(dateY), 400, &dd, dateStr);
}

// ---- CUSTOM face (pushed from Launch Kit) -----------------------------------
// One live text banner, rendered with the design's real typeface baked into the
// firmware (a proper LVGL font in custom_font*.c) rather than a shipped glyph
// image. The glow the editor draws with canvas shadowBlur is reproduced here as
// a firmware effect: the string is drawn several times in the glow colour at a
// ring of offsets with falling opacity, then the sharp fill goes on top. Centred
// at (bx,by) to match the editor (textAlign centre, textBaseline middle).
// One live text banner in the design's real baked typeface, hard-left anchored at
// (bx,by): the string always starts at the same x, laid out with each glyph's own
// natural advance width. A digit that's narrower or wider than its predecessor
// (e.g. "1" -> "8") only pushes the tail end of the string further right — the
// start never moves, so there's no left-right wobble as the seconds tick. Glow is
// a few rings of the same layout at falling opacity, offset outward, under the
// sharp fill on top.
// align: 0 left (bx is the start — a digit changing width only shifts the tail,
// so a live value never wobbles), 1 center (bx is the middle), 2 right (bx is
// the end). Mirrors the editor's alignedStartX().
// Why a banner drew nothing, said once.
//
// Both banner painters bail on a null font or a format strftime will not take, and both
// used to do it in silence. A theme asking for "%a %b %-d" — the GNU no-padding flag, which
// this newlib does not have — therefore lost its whole date line with no symptom anywhere:
// not on the screen, not on the wire, not in this log. Finding that cost an afternoon.
//
// Rate-limited to one line per distinct reason, because this runs inside a once-a-second
// redraw and a fault that repeats 3600 times an hour is noise, not a diagnosis.
static void banner_silent(const char *which, const char *why, const char *fmt) {
#ifdef ARDUINO
    static char s_said[2][40] = { "", "" };
    const int slot = (which[5] == '2') ? 1 : 0;
    char now[40];
    snprintf(now, sizeof(now), "%s:%s", why, fmt ? fmt : "");
    if (!strcmp(s_said[slot], now)) return;
    snprintf(s_said[slot], sizeof(s_said[slot]), "%s", now);
    Serial.printf("[clock] %s drew nothing: %s (fmt \"%s\")\n", which, why, fmt ? fmt : "");
#else
    (void)which; (void)why; (void)fmt;
#endif
}

// A rounded rectangle blended into the canvas in one pass at one opacity. Corner
// coverage comes from the distance to the corner's circle centre, with a one-pixel ramp
// so the curve is smooth rather than stepped. Radius is clamped to half the shorter side.
static void fill_plate(int x, int y, int w, int h, int radius, lv_color_t col, lv_opa_t opa) {
    if (!s_buf || w <= 0 || h <= 0 || opa == 0) return;
    float r = (float)radius;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0) r = 0;
    const int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    const int x1 = (x + w > SCREEN_W) ? SCREEN_W : x + w;
    const int y1 = (y + h > SCREEN_H) ? SCREEN_H : y + h;
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            float cov = 1.0f;
            if (r > 0.5f) {
                // Which corner, if any, this pixel sits in; centre of that corner's arc.
                const float cx = (px < x + r) ? x + r : (px >= x + w - r) ? x + w - r : -1.0f;
                const float cy = (py < y + r) ? y + r : (py >= y + h - r) ? y + h - r : -1.0f;
                if (cx >= 0 && cy >= 0) {
                    const float dx = (px + 0.5f) - cx, dy = (py + 0.5f) - cy;
                    const float d = sqrtf(dx * dx + dy * dy);
                    cov = r + 0.5f - d;            // 1 inside, 0 outside, a one-pixel ramp between
                    if (cov <= 0.0f) continue;
                    if (cov > 1.0f) cov = 1.0f;
                }
            }
            const lv_opa_t a = (lv_opa_t)lroundf(opa * cov);
            if (!a) continue;
            lv_color_t *dst = &s_buf[py * SCREEN_W + px];
            *dst = lv_color_mix(col, *dst, a);
        }
    }
}

static void draw_baked_text(const lv_font_t *font, const char *fmt, int bx, int by,
                            uint32_t color, int glow, uint32_t glowColor, int align,
                            const struct tm *ti, const char *which, lv_opa_t opa = LV_OPA_COVER,
                            uint32_t bg = 0, int bgOpa = 0, int bgRadius = 0, bool upper = false) {
    if (!font)          { banner_silent(which, "no font loaded for this slot", fmt); return; }
    if (!fmt || !fmt[0]) { banner_silent(which, "empty format", fmt); return; }
    if (s_noTime)        return;   // nothing true to print yet; see time_for_face()
    char buf[48];
    // 0 means strftime refused the format outright — almost always a flag or a conversion
    // this libc does not implement, since 48 bytes is ample for anything a banner shows.
    if (strftime(buf, sizeof(buf), fmt, ti) == 0) {
        banner_silent(which, "strftime rejected the format, or it produced nothing", fmt);
        return;
    }
    // AFTER strftime, not before: %A is Wednesday by now, and uppercasing the format would
    // have missed it. THEME_CAPS 53.
    if (upper) orb_upper(buf);
    const int n = (int)strlen(buf);
    float w[48], total = 0.0f;
    for (int i = 0; i < n && i < 48; ++i) {
        lv_font_glyph_dsc_t g;
        w[i] = lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)buf[i], 0) ? (float)g.adv_w : 0.0f;
        total += w[i];
    }
    const float startX = (align == 1) ? (bx - total / 2.0f) : (align == 2) ? (bx - total) : (float)bx;
    const int y0 = (int)lroundf(by - lv_font_get_line_height(font) * 0.5f);

    // The plate behind the words, THEME_CAPS 33. This screen draws into an LVGL canvas
    // rather than through curved_text, so it gets LVGL's own rounded rectangle instead of
    // the raster filler that serves the other screens. Same padding, 8 across and 2 down, so
    // a design that moves a line between screens keeps the shape it drew against.
    if (bgOpa > 0) {
        // Drawn by hand, not with lv_canvas_draw_rect. LVGL 8.4's rounded rectangle with a
        // background opacity under 253 paints its corner rows and its middle block down two
        // different paths, and on this canvas the middle came out wrong: missing entirely in
        // the simulator, and on the glass a dark seam across the words (canoejohn, a date box
        // at 99% opacity, 2026-09-18). Studio's preview had no such line, because a browser
        // draws a rounded rectangle in one pass. So does this: one coverage value per pixel,
        // rounded corners included, blended once.
        const lv_coord_t lh = (lv_coord_t)lv_font_get_line_height(font);
        fill_plate((int)lroundf(startX) - 8, y0 - 2, (int)lroundf(total) + 16, lh + 4,
                   bgRadius, lv_color_hex(bg), (lv_opa_t)bgOpa);
    }
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font  = font;
    ld.align = LV_TEXT_ALIGN_LEFT;
    const auto paint = [&](int ox, int oy, lv_color_t col, lv_opa_t opa) {
        ld.color = col; ld.opa = opa;
        float x = startX;
        for (int i = 0; i < n && i < 48; ++i) {
            char c[2] = { buf[i], 0 };
            lv_canvas_draw_text(s_canvas, (lv_coord_t)lroundf(x) + ox, y0 + oy, (lv_coord_t)lroundf(w[i] + 4), &ld, c);
            x += w[i];
        }
    };
    if (glow > 0) {
        static const float dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f} };
        const lv_color_t gc = lv_color_hex(glowColor);
        const int rings = 3;
        for (int ri = 1; ri <= rings; ++ri) {
            const int r = (int)lroundf((float)glow * ri / rings);
            if (r <= 0) continue;
            const lv_opa_t opa = (lv_opa_t)(90 / ri);   // fainter the further out
            for (int di = 0; di < 8; ++di)
                paint((int)lroundf(dirs[di][0] * r), (int)lroundf(dirs[di][1] * r), gc, opa);
        }
    }
    paint(0, 0, lv_color_hex(color), opa);
}

// Read a 4-bpp (16-level) glyph alpha bitmap (as lv_font_conv --bpp 4 --no-compress
// emits it): continuous bitstream, MSB-first, box_w px per row, no row padding.
static inline float glyph_alpha4(const uint8_t *bmp, int bw, int x, int y) {
    const int bit = (y * bw + x) * 4;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t nib = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17.0f;   // 0..15 -> 0..255
}

// Rotate one glyph's alpha bitmap around its own centre by angleDeg (clockwise,
// screen space) and alpha-blend it into the canvas in a solid colour, with its
// box centred at (destCx,destCy). Bilinear sampled so rotated edges stay smooth.
static void blit_glyph_rot(const uint8_t *bmp, int bw, int bh, float destCx, float destCy, float angleDeg, lv_color_t col, lv_opa_t opa = LV_OPA_COVER) {
    if (!bmp || bw <= 0 || bh <= 0) return;
    const float th = angleDeg * DEG2RAD, ct = cosf(th), st = sinf(th);
    const float pivotX = bw * 0.5f, pivotY = bh * 0.5f;
    const float reach = sqrtf(pivotX * pivotX + pivotY * pivotY) + 1.0f;
    const int x0 = (int)fmaxf(0.0f, destCx - reach), x1 = (int)fminf((float)SCREEN_W - 1, destCx + reach);
    const int y0 = (int)fmaxf(0.0f, destCy - reach), y1 = (int)fminf((float)SCREEN_H - 1, destCy + reach);
    for (int dy = y0; dy <= y1; ++dy) {
        const float oy = dy - destCy;
        for (int dx = x0; dx <= x1; ++dx) {
            const float ox = dx - destCx;
            const float sxf = ox * ct + oy * st + pivotX;
            const float syf = -ox * st + oy * ct + pivotY;
            const int ix = (int)floorf(sxf), iy = (int)floorf(syf);
            if (ix < -1 || iy < -1 || ix >= bw || iy >= bh) continue;
            const float fx = sxf - ix, fy = syf - iy;
            const float a00 = (ix >= 0 && iy >= 0 && ix < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix, iy) : 0.0f;
            const float a10 = (ix + 1 >= 0 && iy >= 0 && ix + 1 < bw && iy < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy) : 0.0f;
            const float a01 = (ix >= 0 && iy + 1 >= 0 && ix < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix, iy + 1) : 0.0f;
            const float a11 = (ix + 1 >= 0 && iy + 1 >= 0 && ix + 1 < bw && iy + 1 < bh) ? glyph_alpha4(bmp, bw, ix + 1, iy + 1) : 0.0f;
            const float a = (a00 * (1 - fx) * (1 - fy) + a10 * fx * (1 - fy) + a01 * (1 - fx) * fy + a11 * fx * fy)
                          * (float)opa / 255.0f;
            if (a < 8.0f) continue;
            lv_color_t *d = &s_buf[dy * SCREEN_W + dx];
            *d = lv_color_mix(col, *d, (lv_opa_t)lroundf(fminf(255.0f, a)));
        }
    }
}

// A curved banner: lay each glyph along an arc of radius R centred on arcDeg (a
// clock angle, 0 = 12 o'clock), advancing by the glyph's own width AND tilting
// each glyph tangent to the arc — the same geometry as the editor's
// drawCurvedText (textAlign centre, textBaseline middle). Glow isn't applied on
// the curve.
static void draw_baked_arc_text(const lv_font_t *font, const char *fmt, float R, float arcDeg,
                                uint32_t color, const struct tm *ti, const char *which,
                                lv_opa_t opa = LV_OPA_COVER, bool upper = false) {
    if (!font)           { banner_silent(which, "no font loaded for this slot", fmt); return; }
    if (!fmt || !fmt[0])  { banner_silent(which, "empty format", fmt); return; }
    if (R < 1.0f)        { banner_silent(which, "curved, but sitting on the dial centre", fmt); return; }
    if (s_noTime)        return;
    char buf[48];
    if (strftime(buf, sizeof(buf), fmt, ti) == 0) {
        banner_silent(which, "strftime rejected the format, or it produced nothing", fmt);
        return;
    }
    if (upper) orb_upper(buf);   // THEME_CAPS 53, after strftime for the same reason
    const int n = (int)strlen(buf);
    float w[48]; float total = 0.0f;
    for (int i = 0; i < n && i < 48; ++i) {
        char c[2] = { buf[i], 0 }; lv_point_t s;
        lv_txt_get_size(&s, c, font, 0, 0, LV_COORD_MAX, 0);
        w[i] = s.x; total += s.x;
    }
    const float norm = fmodf(fmodf(arcDeg, 360.0f) + 360.0f, 360.0f);
    const bool bottom = (norm > 90.0f && norm < 270.0f);
    const float dir = bottom ? -1.0f : 1.0f;                 // read L->R at the bottom
    const float base = arcDeg * DEG2RAD;
    const lv_color_t col = lv_color_hex(color);
    // Baseline "middle": vertical centre of the em box sits on the arc point.
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;      // (ascent - descent)/2
    float cursor = -total / 2.0f;
    for (int i = 0; i < n && i < 48; ++i) {
        const float mid = cursor + w[i] / 2.0f, ang = base + dir * mid / R;
        const float ax = CX + sinf(ang) * R, ay = CY - cosf(ang) * R;   // arc anchor
        const float rot = ang + (bottom ? 3.14159265358979f : 0.0f);    // glyph tilt (clockwise)
        cursor += w[i];
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)buf[i], 0)) continue;
        const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)buf[i]);
        if (!bmp || g.box_w == 0 || g.box_h == 0) continue;
        // Glyph box centre offset from the arc anchor in the upright (unrotated)
        // text frame, then rotated by the tilt into screen space.
        const float offY = halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
        const float cr = cosf(rot), sr = sinf(rot);
        const float destCx = ax - offY * sr, destCy = ay + offY * cr;
        blit_glyph_rot(bmp, g.box_w, g.box_h, destCx, destCy, rot / DEG2RAD, col, opa);
    }
}

// A shadow is one flat colour behind a shape, so it needs none of the colour machinery a
// hand needs. blend_custom_hand reconstructs each pixel from four RGB565 neighbours: four
// unpacks and nine multiplies per pixel, over a box as wide as the sprite's reach. For
// Aviator's 429x429 minute hand that is the whole 466x466 screen, and running it a SECOND
// time for the shadow doubled the most expensive loop on the clock.
//
// That is what turned the dial black. Not the art — the shadow sprites are clean
// silhouettes covering 3% of their box — but a frame that no longer finished inside its own
// tick, so the canvas was never completely composited before it was flushed.
//
// This samples alpha only, nearest-neighbour, and mixes one constant colour. A blurred
// silhouette has no detail for bilinear to preserve, so nothing is lost, and it costs
// roughly a third of the full path.
// The box everything on this dial is allowed to touch.
//
// Full screen except while a smooth second hand is sweeping, when only the hand's own
// rectangle is repainted. Every full-screen pass on this face costs real time — the plate
// copy is 28.7 ms and the overlay 26.1 — and both scale straight down with the area, so a
// hand covering a quarter of the dial costs a quarter of them.
//
// A file-static rather than a parameter on nine functions: it is the same box for every
// layer in one pass, and threading it through by hand is how one helper ends up drawing
// outside it and smearing the frame.
static int s_clipX0 = 0, s_clipY0 = 0, s_clipX1 = SCREEN_W - 1, s_clipY1 = SCREEN_H - 1;
// Per-row runs, when the box is not tight enough.
//
// A sweep frame wipes only the runs the second hand and its shadow actually cover, not the
// rectangle around them. Anything redrawn afterwards — a hand the design orders ABOVE the
// second hand — must go back over exactly those runs and no further, or it would be blended
// a second time onto pixels that still had it from the last frame, and a semi-transparent
// hand would darken a little more every frame.
static const int *s_runLo = nullptr, *s_runHi = nullptr;
static inline void clip_reset() {
    s_clipX0 = 0; s_clipY0 = 0; s_clipX1 = SCREEN_W - 1; s_clipY1 = SCREEN_H - 1;
    s_runLo = nullptr; s_runHi = nullptr;
}
// The x range this row may touch, box and run together.
static inline void clip_row(int dy, int &lo, int &hi) {
    if (lo < s_clipX0) lo = s_clipX0;
    if (hi > s_clipX1) hi = s_clipX1;
    if (s_runLo && dy >= 0 && dy < SCREEN_H) {
        if (lo < s_runLo[dy]) lo = s_runLo[dy];
        if (hi > s_runHi[dy]) hi = s_runHi[dy];
    }
}

// ---- the hands layer cache --------------------------------------------------
//
// WHY. Measured on Steam Punk with [compose]: a full face is 527 ms, of which the HANDS are
// 472 — 90% of it. The plate is 29 ms and the glass 27. An animated background has to do one
// full face per frame, because the background is the bottom layer and everything above was
// composited onto it, so at 8 fps the theme was asking for 4.2 seconds of work per second
// and getting about one frame.
//
// The hands do not move during a burst. Six frames at 8 fps is 750 ms, in which the minute
// hand turns 0.075 degrees and the hour hand 0.006. So their contribution is computed once
// and kept: two RGB565+alpha layers, blitted in place of four bilinear sprite passes.
//
// TWO layers, not one, and this is the whole reason the order works out. The pipeline lays
// every shadow down before any hand, deliberately — "interleaving them would let the minute
// hand's shadow fall across the hour hand drawn below it" — and this theme's draw order puts
// the second hand UNDER the minute and hour. So the true sequence is
//
//     plate -> text -> S2 -> S1 -> S0 -> H2 -> H1 -> H0 -> glass
//
// with S=shadow, H=hand, 2=second, 1=minute, 0=hour. The second hand's two pieces come
// first in each group and must stay live, because it is the one thing that does move. One
// combined layer would have to be drawn either side of them and would reorder something.
// Splitting at exactly those two points reproduces the sequence with nothing moved:
//
//     plate -> text -> S2 -> [shadow layer] -> H2 -> [hand layer] -> glass
//
// 3 bytes a pixel, 651 KB each, PSRAM, taken on first use and given back in onExit() like
// every other screen's art.
static uint8_t *s_layShadow = nullptr;   // minute + hour shadows, composited
static uint8_t *s_layHand   = nullptr;   // minute + hour hands, composited
// When set, the two sprite blits accumulate into this instead of painting the canvas.
static uint8_t *s_layTarget = nullptr;
// What the layers are a picture of. A rebuild is needed when either hand has turned enough
// to matter; the thresholds are the ones the caches above already use.
static float s_layMinAng = 1e9f, s_layHrAng = 1e9f;
static bool  s_layValid  = false;
// A REBUILD SPREAD OVER FRAMES, because doing it in one go is a visible stop.
//
// A rebuild is ~470 ms and the minute hand goes stale every ~3.5 s, so the first version
// stalled the sweep for half a second every three and a half: "every ~3.5 seconds it stops
// then resumes". The threshold cannot simply be raised — it is bounded by the minute hand
// visibly lagging, about a pixel at the tip — so the work is sliced instead. A band of rows
// per frame, with the previous contents still standing in the rows not yet redone, so the
// seam between them is the 0.35 degrees the tolerance already allows and it closes within a
// second. The FIRST build is done whole, because there is nothing behind it to show.
static int   s_layBuildY   = -1;      // next row to redo; <0 = not rebuilding
static float s_layWantMin  = 0.0f;    // angles the in-progress rebuild is for
static float s_layWantHr   = 0.0f;
static const int LAY_BAND  = SCREEN_H / 8;   // ~58 rows, so ~60 ms a frame instead of 470
// Set for one compose by the caller that knows the hands have not moved. compose_custom
// then blits each layer at the exact point in the sequence its contents belong, instead of
// running the four bilinear sprite passes. Anything else about the frame is unchanged.
static bool  s_layUse    = false;

// One source pixel into a layer, source-over, non-premultiplied.
//
// The canvas path can mix straight onto an opaque destination because there is always
// something underneath. A layer starts empty, so alpha has to accumulate as well as colour:
// two hands overlap near the hub and the second one must not erase the first's coverage.
static inline void lay_put(uint8_t *lay, int dx, int dy, lv_color_t sc, uint8_t a) {
    uint8_t *L = lay + ((size_t)dy * SCREEN_W + dx) * 3;
    const uint8_t da = L[2];
    if (!da) {                      // empty: the source is the answer
        L[0] = (uint8_t)(sc.full & 0xFF);
        L[1] = (uint8_t)(sc.full >> 8);
        L[2] = a;
        return;
    }
    const uint16_t outA = (uint16_t)(a + (uint16_t)da * (255 - a) / 255);
    if (!outA) return;
    lv_color_t dc; dc.full = (uint16_t)(L[0] | (L[1] << 8));
    // The source's share of the result. lv_color_mix(c1, c2, w) is c1*w + c2*(255-w).
    const uint8_t w = (uint8_t)((uint32_t)a * 255u / outA);
    const lv_color_t mixed = lv_color_mix(sc, dc, w);
    L[0] = (uint8_t)(mixed.full & 0xFF);
    L[1] = (uint8_t)(mixed.full >> 8);
    L[2] = (uint8_t)(outA > 255 ? 255 : outA);
}

// A finished layer onto the canvas: the same operation the glass pass does, over the same
// clip, so a sweep frame pays only for its own rows.
static void lay_blit(const uint8_t *lay) {
    if (!lay || !s_buf) return;
    for (int dy = s_clipY0; dy <= s_clipY1; ++dy) {
        int lo = s_clipX0, hi = s_clipX1;
        clip_row(dy, lo, hi);
        if (hi < lo) continue;
        const int base = dy * SCREEN_W;
        for (int dx = lo; dx <= hi; ++dx) {
            const uint8_t *L = lay + ((size_t)base + dx) * 3;
            const uint8_t a = L[2];
            if (!a) continue;
            lv_color_t sc; sc.full = (uint16_t)(L[0] | (L[1] << 8));
            s_buf[base + dx] = lv_color_mix(sc, s_buf[base + dx], a);
        }
    }
}

// Build both layers for the hand angles given. Forward-declared blits, called below.
static void blend_shadow(const uint8_t *src, int sw, int sh, int pivotX, int pivotY,
                         float cx, float cy, float angleDeg);
static void blend_custom_hand(const uint8_t *src, int sw, int sh, int pivotX, int pivotY,
                              float cx, float cy, float angleDeg, int blend);
CustomSprite custom_hand(int kind);     // not static: defined elsewhere in the tree
CustomSprite custom_shadow(int kind);

static void layers_free() {
    if (s_layShadow) { heap_caps_free(s_layShadow); s_layShadow = nullptr; }
    if (s_layHand)   { heap_caps_free(s_layHand);   s_layHand   = nullptr; }
    s_layValid = false;
}

// Both layers, for the minute and hour hands only, in the theme's own draw order.
//
// Deliberately NOT the second hand: it is the one piece that moves between frames of a
// background burst, and it is cheap (a 32x107 shadow and a small hand) so it stays live.
// Returns false and leaves s_layValid clear if the memory is not there, and every caller
// then takes the ordinary path — slow, correct, and exactly what it did before.
static bool layers_build_rows(float minAng, float hrAng, int y0, int y1);
static bool layers_usable(float minAng, float hrAng);
static bool layers_build(float minAng, float hrAng) {
    if (!s_layShadow) s_layShadow = (uint8_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 3,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_layHand)   s_layHand   = (uint8_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * 3,
                                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_layShadow || !s_layHand) {
#if defined(ESP_PLATFORM)
        static bool told = false;
        if (!told) {
            told = true;
            Serial.printf("[layers] no PSRAM for the hand layers (need 2 x %u KB, %u KB free) "
                          "- animated backgrounds stay on the slow path\n",
                          (unsigned)((size_t)SCREEN_W * SCREEN_H * 3 / 1024),
                          (unsigned)(ESP.getFreePsram() / 1024));
        }
#endif
        layers_free();
        return false;
    }
    return layers_build_rows(minAng, hrAng, 0, SCREEN_H - 1);
}

// The rows [y0..y1] of both layers, for the given angles.
static bool layers_build_rows(float minAng, float hrAng, int y0, int y1) {
    if (!s_layShadow || !s_layHand) return false;
    const theme_style::Clock &cs = theme_style::clock();
    const float ang[5] = { hrAng, minAng, 0.0f, 0.0f, 0.0f };
    if (y0 < 0) y0 = 0;
    if (y1 > SCREEN_H - 1) y1 = SCREEN_H - 1;
    if (y1 < y0) return false;
    const size_t rowBytes = (size_t)SCREEN_W * 3;
    const size_t off = (size_t)y0 * rowBytes, len = (size_t)(y1 - y0 + 1) * rowBytes;
    memset(s_layShadow + off, 0, len);
    memset(s_layHand   + off, 0, len);
    // The clip is the band, and the full width of it: the layer is reused by frames whose
    // clip is narrower, and anything left unpainted now would be a hole then.
    const int cx0 = s_clipX0, cy0 = s_clipY0, cx1 = s_clipX1, cy1 = s_clipY1;
    const int *rl = s_runLo, *rh = s_runHi;
    clip_reset();
    s_clipY0 = y0; s_clipY1 = y1;

    if (cs.shadowOn) {
        s_layTarget = s_layShadow;
        for (int i = 0; i < cs.orderN; ++i) {
            const int k = cs.order[i];
            if (k != 0 && k != 1) continue;            // minute and hour only
            if (!cs.hand[k].show) continue;
            const theme_style::Hand &hd = cs.hand[k];
            CustomSprite sh = custom_shadow(k);
            if (sh.data) blend_shadow(sh.data, sh.w, sh.h, hd.pivotX, hd.pivotY,
                                      (float)(hd.centerX + cs.shadowDX),
                                      (float)(hd.centerY + cs.shadowDY), ang[k]);
        }
    }
    s_layTarget = s_layHand;
    for (int i = 0; i < cs.orderN; ++i) {
        const int k = cs.order[i];
        if (k != 0 && k != 1) continue;
        if (!cs.hand[k].show) continue;
        const theme_style::Hand &hd = cs.hand[k];
        CustomSprite spr = custom_hand(k);
        if (spr.data) blend_custom_hand(spr.data, spr.w, spr.h, hd.pivotX, hd.pivotY,
                                        (float)hd.centerX, (float)hd.centerY, ang[k], hd.blend);
    }
    s_layTarget = nullptr;

    s_clipX0 = cx0; s_clipY0 = cy0; s_clipX1 = cx1; s_clipY1 = cy1;
    s_runLo = rl; s_runHi = rh;
    return true;
}

// Keep a rebuild moving, or start one. Call once a frame.
//
// Returns whether there is anything worth blitting: after the first build there always is,
// even mid-rebuild, which is the whole point of slicing it.
static bool layers_tick(float minAng, float hrAng) {
    if (!s_layShadow || !s_layHand) {
        if (!layers_build(minAng, hrAng)) return false;   // allocates, or gives up for good
        s_layMinAng = minAng; s_layHrAng = hrAng; s_layValid = true; s_layBuildY = -1;
        return true;
    }
    if (s_layBuildY >= 0) {                                // a slice of the rebuild in flight
        layers_build_rows(s_layWantMin, s_layWantHr, s_layBuildY, s_layBuildY + LAY_BAND - 1);
        s_layBuildY += LAY_BAND;
        if (s_layBuildY >= SCREEN_H) {                     // done: it is now a picture of those
            s_layMinAng = s_layWantMin; s_layHrAng = s_layWantHr;
            s_layValid = true; s_layBuildY = -1;
        }
        return s_layValid;
    }
    if (!layers_usable(minAng, hrAng)) {                   // gone stale: start slicing
        s_layWantMin = minAng; s_layWantHr = hrAng;
        s_layBuildY = 0;
    }
    return s_layValid;
}

// Are the layers still a picture of where the hands are now?
//
// Same question the dial caches already ask, and the same answer: hold until the hand's TIP
// has travelled about a pixel. The minute hand buys roughly three seconds at this reach and
// the hour hand forty, which is far longer than any background burst, so a burst rebuilds
// them at most once.
static bool layers_usable(float minAng, float hrAng) {
    if (!s_layValid || !s_layShadow || !s_layHand) return false;
    float dm = minAng - s_layMinAng; if (dm < 0) dm = -dm; if (dm > 180.0f) dm = 360.0f - dm;
    float dh = hrAng  - s_layHrAng;  if (dh < 0) dh = -dh; if (dh > 180.0f) dh = 360.0f - dh;
    return dm < 0.35f && dh < 0.35f;
}

// The run of dx, within one row, whose source coordinates land inside the sprite.
//
// Both rotating blits need this and only one of them had it. blend_custom_hand got the
// treatment first and went from 234 ms to 16; blend_shadow kept sweeping the whole square,
// and on a design with shadows switched on it then cost more than every hand put together —
// 38 ms of a 57 ms sweep frame, for one hand's shadow. Shared now, so they cannot diverge
// again.
//
// sxf and syf are linear in dx within a row, so each axis clips the run to an interval and
// the answer is the intersection. Callers keep their own per-pixel guard: this narrows the
// loop, it does not decide what is drawn.
static inline void row_span(float a, float b, float L, float &lo, float &hi, bool &dead) {
    if (fabsf(a) < 1e-6f) { if (b < 0.0f || b > L) dead = true; return; }
    float t0 = (0.0f - b) / a, t1 = (L - b) / a;
    if (t0 > t1) { const float t = t0; t0 = t1; t1 = t; }
    if (t0 > lo) lo = t0;
    if (t1 < hi) hi = t1;
}

static void blend_shadow(const uint8_t *src, int sw, int sh, int pivotX, int pivotY,
                         float cx, float cy, float angleDeg) {
    if (!src || !s_buf) return;
    const float th = angleDeg * DEG2RAD, ct = cosf(th), st = sinf(th);
    const float reach = sqrtf(fmaxf((float)pivotX, (float)(sw - pivotX)) * fmaxf((float)pivotX, (float)(sw - pivotX))
                            + fmaxf((float)pivotY, (float)(sh - pivotY)) * fmaxf((float)pivotY, (float)(sh - pivotY)));
    int x0 = (int)fmaxf(0.0f, cx - reach), x1 = (int)fminf((float)SCREEN_W - 1, cx + reach);
    int y0 = (int)fmaxf(0.0f, cy - reach), y1 = (int)fminf((float)SCREEN_H - 1, cy + reach);
    if (x0 < s_clipX0) x0 = s_clipX0;
    if (y0 < s_clipY0) y0 = s_clipY0;
    if (x1 > s_clipX1) x1 = s_clipX1;
    if (y1 > s_clipY1) y1 = s_clipY1;
    for (int dy = y0; dy <= y1; ++dy) {
        const float oy = dy - cy;
        const float ax = oy * st + pivotX, ay = oy * ct + pivotY;
        float uLo = (float)(x0 - cx), uHi = (float)(x1 - cx);
        bool dead = false;
        row_span(ct,  ax, (float)(sw - 1), uLo, uHi, dead);
        row_span(-st, ay, (float)(sh - 1), uLo, uHi, dead);
        if (dead || uHi < uLo) continue;
        int rx0 = (int)floorf(cx + uLo) - 1, rx1 = (int)ceilf(cx + uHi) + 1;
        if (rx0 < x0) rx0 = x0;
        if (rx1 > x1) rx1 = x1;
        clip_row(dy, rx0, rx1);
        for (int dx = rx0; dx <= rx1; ++dx) {
            const float ox = dx - cx;
            const int sx = (int)(ox * ct + oy * st + pivotX);
            const int sy = (int)(-ox * st + oy * ct + pivotY);
            if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
            const uint8_t *p = src + ((size_t)sy * sw + sx) * 3;
            const uint8_t a = p[2];
            if (a < 8) continue;                       // same floor the hand blit uses
            lv_color_t sc; sc.full = (uint16_t)(p[0] | (p[1] << 8));
            if (s_layTarget) { lay_put(s_layTarget, dx, dy, sc, a); continue; }
            lv_color_t *dst = &s_buf[dy * SCREEN_W + dx];
            *dst = lv_color_mix(sc, *dst, a);
        }
    }
}

// Rotate a hand sprite (RGB565+alpha, 3 B/px) around the dial centre by angleDeg
// and composite it into the canvas with the given blend (0 normal, 1 multiply,
// 2 screen) — the same rotation math as blend_office_sprite, generalised to raw
// sprite data + a blend mode so a pushed hand lands exactly where the editor drew it.
// A layer that never turns is a straight copy: one source pixel onto one screen pixel.
//
// Worth its own path because the layers that use it are FULL SCREEN. Sending 217k pixels
// through the rotating blit below costs four texel fetches and a dozen floats each, which
// is the same full-screen bilinear pass that once left the Aviator dial black. This is the
// overlay loop's cost instead, and the frame budget already carries one of those.
static void blit_upright(const uint8_t *src, int sw, int sh, int pivotX, int pivotY,
                         int cx, int cy, int blend) {
    const int offX = cx - pivotX, offY = cy - pivotY;
    int x0 = offX < 0 ? 0 : offX, y0 = offY < 0 ? 0 : offY;
    int x1 = (offX + sw < SCREEN_W ? offX + sw : SCREEN_W);
    int y1 = (offY + sh < SCREEN_H ? offY + sh : SCREEN_H);
    if (x0 < s_clipX0) x0 = s_clipX0;
    if (y0 < s_clipY0) y0 = s_clipY0;
    if (x1 > s_clipX1 + 1) x1 = s_clipX1 + 1;
    if (y1 > s_clipY1 + 1) y1 = s_clipY1 + 1;
    for (int dy = y0; dy < y1; ++dy) {
        const uint8_t *row = src + ((size_t)(dy - offY) * sw) * 3;
        lv_color_t *dstRow = &s_buf[dy * SCREEN_W];
        for (int dx = x0; dx < x1; ++dx) {
            const uint8_t *p = row + (size_t)(dx - offX) * 3;
            const uint8_t a = p[2];
            if (a < 8) continue;                       // same floor the rotating blit uses
            lv_color_t sc; sc.full = (uint16_t)(p[0] | (p[1] << 8));
            lv_color_t *dst = &dstRow[dx];
            if (blend) {
                uint8_t sr, sg, sb, dr, dg, db;
                unpack565(sc.full, sr, sg, sb);
                unpack565(dst->full, dr, dg, db);
                if (blend == 1) { sr = (uint8_t)(sr * dr / 255); sg = (uint8_t)(sg * dg / 255); sb = (uint8_t)(sb * db / 255); }
                else if (blend == 2) { sr = (uint8_t)(255 - (255 - sr) * (255 - dr) / 255); sg = (uint8_t)(255 - (255 - sg) * (255 - dg) / 255); sb = (uint8_t)(255 - (255 - sb) * (255 - db) / 255); }
                sc = LV_COLOR_MAKE(sr, sg, sb);
            }
            *dst = lv_color_mix(sc, *dst, a);
        }
    }
}

static void blend_custom_hand(const uint8_t *src, int sw, int sh, int pivotX, int pivotY, float cx, float cy, float angleDeg, int blend) {
    if (!src || !s_buf) return;
    // The static layers (kinds 3 and 4) are always here, and a hand passing 12 lands here
    // for one frame, which is free.
    if (fabsf(angleDeg) < 0.01f && cx == floorf(cx) && cy == floorf(cy)) {
        blit_upright(src, sw, sh, pivotX, pivotY, (int)cx, (int)cy, blend);
        return;
    }
    const float th = angleDeg * DEG2RAD, ct = cosf(th), st = sinf(th);
    const float reach = sqrtf(fmaxf((float)pivotX, (float)(sw - pivotX)) * fmaxf((float)pivotX, (float)(sw - pivotX))
                            + fmaxf((float)pivotY, (float)(sh - pivotY)) * fmaxf((float)pivotY, (float)(sh - pivotY)));
    int x0 = (int)fmaxf(0.0f, cx - reach), x1 = (int)fminf((float)SCREEN_W - 1, cx + reach);
    int y0 = (int)fmaxf(0.0f, cy - reach), y1 = (int)fminf((float)SCREEN_H - 1, cy + reach);
    if (x0 < s_clipX0) x0 = s_clipX0;
    if (y0 < s_clipY0) y0 = s_clipY0;
    if (x1 > s_clipX1) x1 = s_clipX1;
    if (y1 > s_clipY1) y1 = s_clipY1;
    // ONLY THE PIXELS THE HAND ACTUALLY LANDS ON.
    //
    // The box above is a square as wide as the hand's whole swing, because `reach` is the
    // distance from the pivot to the furthest corner. A hand is a long thin rectangle, so
    // most of that square is empty: a 60 x 230 hand pivoting near one end sweeps a 460 px
    // square, 212,000 pixels, to cover about 14,000 of them. Every one of the other 198,000
    // still paid for two multiplies, a floor, and a bounds test before being skipped.
    //
    // Measured on the Beige face before this: 234 ms of a 290 ms redraw was the three hands,
    // 81% of the whole dial. Nothing about a smooth second hand is possible at that price.
    //
    // Within one row the source coordinates are LINEAR in dx, so the range of dx that lands
    // inside the sprite is just two intervals intersected. Solve, clamp, and walk only that.
    // The original per-pixel test is kept below as the authority: this narrows the loop, it
    // does not decide what gets drawn, so an off-by-one here costs a wasted iteration rather
    // than a wrong pixel.
    const float lx = (float)(sw - 1), ly = (float)(sh - 1);
    for (int dy = y0; dy <= y1; ++dy) {
        const float oy = dy - cy;
        // sxf(u) = u*ct + ax and syf(u) = -u*st + ay, for u = dx - cx.
        const float ax = oy * st + pivotX, ay = oy * ct + pivotY;
        float uLo = (float)(x0 - cx), uHi = (float)(x1 - cx);
        bool empty = false;
        row_span(ct,  ax, lx, uLo, uHi, empty);
        row_span(-st, ay, ly, uLo, uHi, empty);
        if (empty || uHi < uLo) continue;
        // One pixel of slack each way, because the bounds above are on the sampled point and
        // the guard below tests the texel pair around it.
        int rx0 = (int)floorf(cx + uLo) - 1, rx1 = (int)ceilf(cx + uHi) + 1;
        if (rx0 < x0) rx0 = x0;
        if (rx1 > x1) rx1 = x1;
        clip_row(dy, rx0, rx1);
        if (rx1 < rx0) continue;
        // Stepped, not recomputed. sxf advances by ct and syf by -st for every pixel across
        // a row, which turns four multiplies and four adds per pixel into two adds.
        float sxf = (rx0 - cx) * ct + ax;
        float syf = -(rx0 - cx) * st + ay;
        for (int dx = rx0; dx <= rx1; ++dx, sxf += ct, syf -= st) {
            const int sx0 = (int)floorf(sxf), sy0 = (int)floorf(syf), sx1 = sx0 + 1, sy1 = sy0 + 1;
            if (sx0 < 0 || sy0 < 0 || sx1 >= sw || sy1 >= sh) continue;
            const float fx = sxf - sx0, fy = syf - sy0;
            const uint8_t *p00 = src + ((size_t)sy0 * sw + sx0) * 3, *p10 = src + ((size_t)sy0 * sw + sx1) * 3;
            const uint8_t *p01 = src + ((size_t)sy1 * sw + sx0) * 3, *p11 = src + ((size_t)sy1 * sw + sx1) * 3;
            const float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
            const float aF = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
            if (aF < 8) continue;
            const float aw00 = p00[2] * w00, aw10 = p10[2] * w10, aw01 = p01[2] * w01, aw11 = p11[2] * w11;
            const float aSum = aw00 + aw10 + aw01 + aw11;
            // ONE reciprocal, not three divides. Colour is weighted by alpha and then
            // normalised on all three channels, and a float division is the most expensive
            // arithmetic on this chip by a distance: three of them per pixel, over the forty
            // thousand a sweeping hand touches, is the difference between a hand that glides
            // and one Zion can see stepping.
            const float invA = 1.0f / aSum;
            uint8_t r, g, b, r2, g2, b2, r3, g3, b3, r4, g4, b4;
            unpack565((uint16_t)(p00[0] | (p00[1] << 8)), r, g, b);
            unpack565((uint16_t)(p10[0] | (p10[1] << 8)), r2, g2, b2);
            unpack565((uint16_t)(p01[0] | (p01[1] << 8)), r3, g3, b3);
            unpack565((uint16_t)(p11[0] | (p11[1] << 8)), r4, g4, b4);
            float rF = (r * aw00 + r2 * aw10 + r3 * aw01 + r4 * aw11) * invA;
            float gF = (g * aw00 + g2 * aw10 + g3 * aw01 + g4 * aw11) * invA;
            float bF = (b * aw00 + b2 * aw10 + b3 * aw01 + b4 * aw11) * invA;
            lv_color_t *dst = &s_buf[dy * SCREEN_W + dx];
            uint8_t dr, dg, db; unpack565(dst->full, dr, dg, db);
            if (blend == 1) { rF = rF * dr / 255.0f; gF = gF * dg / 255.0f; bF = bF * db / 255.0f; }        // multiply
            else if (blend == 2) { rF = 255 - (255 - rF) * (255 - dr) / 255.0f; gF = 255 - (255 - gF) * (255 - dg) / 255.0f; bF = 255 - (255 - bF) * (255 - db) / 255.0f; } // screen
            lv_color_t sc = LV_COLOR_MAKE((uint8_t)rF, (uint8_t)gF, (uint8_t)bF);
            if (s_layTarget) lay_put(s_layTarget, dx, dy, sc, (uint8_t)lroundf(aF));
            else             *dst = lv_color_mix(sc, *dst, (lv_opa_t)lroundf(aF));
        }
    }
}


// Composite the editor's exact pixels: plate (background) -> live text -> hand
// sprites (rotated, in the editor's draw order/blend) -> overlay (hub, rim, glass).
// Copy the plate rotated about the screen centre. Used only by themes whose background
// "rotates with" a hand (Launch Kit's Rotate-with control): the crescent border in the
// Modern theme is meant to trail the minute hand, and a static plate made it line up once
// an hour by coincidence.
//
// Nearest-neighbour on purpose. The artwork this exists for is a soft gradient, where
// bilinear buys nothing visible, and the same choice on the radar sweep earlier roughly
// doubled that screen's frame rate. Full-screen, so it is worth not paying for.
// The rotated plate, kept between frames. Rotating is ~217k pixel lookups; a clock with a
// second hand redraws ~33x a second, and the minute-follow angle moves 0.1 deg in that
// time — so all but one of those rotations reproduced the previous image exactly.
// Measured cost of getting this wrong: 993 ms of LVGL time per second, i.e. the CPU
// pinned inside the graphics library, which starved knob input and read as a sluggish
// encoder. Now the rotation happens only when the angle has actually moved, and every
// other frame is a memcpy.
static void blit_plate_rot_slow(const uint16_t *src, float angleDeg);

static uint16_t *s_rotCache      = nullptr;
static float     s_rotCacheAngle = 1e9f;    // no cached angle yet
static const uint16_t *s_rotCacheSrc = nullptr;

static void blit_plate_rot(const uint16_t *src, float angleDeg) {
    const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t);
    if (!s_rotCache) {
#ifdef ESP_PLATFORM
        s_rotCache = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_rotCache = (uint16_t *)malloc(bytes);
#endif
    }
    if (s_rotCache) {
        // A quarter of a degree is well under one pixel of movement at this radius, so
        // re-rotating below that threshold buys nothing visible. At minute-follow it means
        // a real rotation roughly every 2.5 s instead of 33 times a second.
        if (s_rotCacheSrc == src && fabsf(angleDeg - s_rotCacheAngle) < 0.25f) {
            memcpy(s_buf, s_rotCache, bytes);
            return;
        }
        blit_plate_rot_slow(src, angleDeg);
        memcpy(s_rotCache, s_buf, bytes);
        s_rotCacheAngle = angleDeg;
        s_rotCacheSrc   = src;
        return;
    }
    blit_plate_rot_slow(src, angleDeg);   // no cache buffer: correct, just slower
}

static void blit_plate_rot_slow(const uint16_t *src, float angleDeg) {
    const float th = angleDeg * DEG2RAD, ct = cosf(th), st = sinf(th);
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    uint16_t *dst = (uint16_t *)s_buf;
    for (int dy = 0; dy < SCREEN_H; ++dy) {
        const float oy = dy - cy;
        for (int dx = 0; dx < SCREEN_W; ++dx) {
            const float ox = dx - cx;
            const int sx = (int)(ox * ct + oy * st + cx);
            const int sy = (int)(-ox * st + oy * ct + cy);
            dst[(size_t)dy * SCREEN_W + dx] =
                (sx < 0 || sy < 0 || sx >= SCREEN_W || sy >= SCREEN_H) ? 0 : src[(size_t)sy * SCREEN_W + sx];
        }
    }
}

// Compose the dial.
//
// Two flags, and both exist for the smooth second hand. `skipSecond` leaves the sweeping
// hand out, which is what makes a cached face possible: everything that only changes once a
// minute is composited once and kept. `withOverlay` leaves the glass off, because the
// ── the moving background, THEME_CAPS 55 ──────────────────────────────────────
//
// Zion's design, and it is the reason this is affordable at all. A background that changes
// every frame invalidates s_under, the cache of everything beneath the second hand, and that
// cache is the only reason a sweep is possible: rebuilding it costs a full compose, which is
// 72 ms against the 26 ms a cached frame costs, and the hand falls from about thirteen a
// second to four and a half. Measured; see the note over sweep_period().
//
// So the default is to HOLD on frame nought and play now and then. While it holds, the
// background is still, the cache is valid, and the hand runs exactly as fast as it does on a
// theme with no animation at all: the cost is not reduced, it is absent. A play spends that
// cost for a second or two, which is the moment somebody is looking at the picture rather
// than the hand. A theme may also ask to loop, and then it pays the whole time, which is its
// choice to make and is why it is not the default.
static uint32_t s_bgPlayStart = 0;     // ms, when the current play began; 0 when holding
static uint32_t s_bgLastPlay  = 0;     // ms, when the last play ended
static int      s_bgFrame     = 0;     // the frame now on screen, so a change can be noticed

// Is a play running right now? The tick uses this to keep time with the animation rather
// than with the second hand, which may otherwise be a whole second apart.
static bool bg_anim_playing() { return s_bgPlayStart != 0; }

// Which frame belongs on the dial at this moment.
static int bg_anim_frame() {
    const theme_style::Clock::BgAnim &a = theme_style::clock().bgAnim;
    if (a.frames <= 0) return 0;
    const uint32_t now  = lv_tick_get();
    const uint32_t step = 1000u / (uint32_t)(a.fps < 1 ? 1 : a.fps);
    const int      last = a.frames;            // frames counts the EXTRA ones, so 0..last

    if (a.loop) {
        s_bgPlayStart = 0;                     // a loop is never "a play"; it just runs
        return (int)((now / (step ? step : 1)) % (uint32_t)(last + 1));
    }

    if (!bg_anim_playing()) {
        // Hold on frame nought until it is time. s_bgLastPlay starts at 0, which would fire
        // immediately at boot; the theme is applied before the first tick, so instead the
        // first interval is measured from whenever this theme became the live one.
        if (s_bgLastPlay == 0) { s_bgLastPlay = now; return 0; }
        if (now - s_bgLastPlay < (uint32_t)a.everySec * 1000u) return 0;
        s_bgPlayStart = now ? now : 1;
        // Two lines an interval, and an interval is minutes by default. Worth having: "does
        // the background actually fire?" is otherwise only answerable by watching the glass.
        Serial.printf("[bg_anim] play starts: %d frames at %d fps\n", a.frames, a.fps);
    }
    const uint32_t into = now - s_bgPlayStart;
    const int      idx  = (int)(into / (step ? step : 1));
    if (idx > last) {                          // played through: back to nought and hold
        Serial.printf("[bg_anim] play ends after %lu ms, holding %d s\n",
                      (unsigned long)into, a.everySec);
        s_bgPlayStart = 0;
        s_bgLastPlay  = now;
        return 0;
    }
    return idx;
}

// Start this theme's clock over. Called when a theme is applied, so the first play is one
// full interval after the theme arrives rather than at some moment inherited from the last.
void clock_view_reset_bg_anim() {
    // A theme change replaces the hand art and its geometry, so whatever the layers hold is
    // a picture of the previous design. This is the one hook every theme application runs
    // through, which is why the reset lives here rather than beside the art loader.
    layers_free();
    s_bgPlayStart = 0;
    s_bgLastPlay  = lv_tick_get();
    s_bgFrame     = 0;
}

// How far through the minute hand's step we are, 0 to about 1.1, or -1 when it is not
// stepping. Only a railway dial ever sets it. See minute_step_ease().
static float s_stepEase = -1.0f;

// What a full compose spends its time on, summed since the last report. See the stopwatches
// in compose_custom.
static float s_phPlate = 0.0f, s_phText = 0.0f, s_phHands = 0.0f, s_phRest = 0.0f;

// Where the minute hand belongs, in minutes-of-the-hour, for a given wall clock reading.
//
// Two rules, and the gap between them is the whole of what Jean-Paul Stringaro reported.
// A railway dial's minute hand sits ON a mark, so it takes whole minutes and no seconds
// fraction: it cannot come to rest partway between two marks just because that is where
// the Orb happened to be switched on. Every other sweeping dial creeps, so it takes the
// fraction. Shared with the simulator self-test so the rule is checked, not just written.
static float minute_hand_mins(bool railway, int min, int sec) {
    return railway ? (float)min : (float)min + (float)sec / 60.0f;
}

// overlay has to go back on TOP of the second hand and so cannot be baked into that cache.
// `stopKind` is the hand this compose stops BELOW: that hand, and everything above it in the
// draw order, is left out and redrawn later by whoever owns the cache being built.
//
//   -1  draw the whole face (the plain redraw path)
//    2  stop below the second hand  -> s_under, which a sweep frame restores from
//    1  stop below the minute hand  -> s_noMin, which a minute move restores from
//
// It was a bool called skipSecond. Two caches need two stop points, and the second one is what
// stops the whole dial being recomposed every few seconds just to creep one hand.
static void compose_custom(const struct tm *ti, int stopKind, bool withOverlay) {
    // Decode the plate first: it's the whole visible dial and the largest buffer,
    // so it gets first claim on PSRAM. (Text is now a baked font, not a giant
    // atlas, so the old "overlay first" ordering is no longer needed.) The overlay
    // is decoded next and blitted at the end (over the hands).
    // The frame the player chose, falling back to the plate itself when a theme ships no
    // animation or its frames never baked. Both answers are the same pointer for frame 0.
    const uint16_t *plate = custom_plate_frame(s_bgFrame);
    if (!plate) plate = custom_plate();
    const uint8_t *overlay = custom_overlay();
    // Angles are needed before the plate now: a theme can ask the plate to rotate with a
    // hand, in which case the straight copy below becomes a rotated one.
    const float p_sec = ti->tm_sec, p_min = ti->tm_min + p_sec / 60.0f;
    const float p_hr = (ti->tm_hour % 12) + p_min / 60.0f;
    const float followAng[4] = { 0.0f, p_hr * 30.0f, p_min * 6.0f, p_sec * 6.0f };
#if defined(ESP_PLATFORM)
    // WHERE the 85 ms goes. A recompose stops a sweeping hand dead, so the only question
    // worth asking about it is which part is expensive, and that cannot be guessed: the plate
    // is a memcpy, the banners are text, the hands are rotations and the glass is a blend over
    // every pixel on the screen. Measured per phase, reported with the interval in tick_cb.
    const uint32_t ph0 = micros();
#endif
    const int pf = theme_style::clock().plateFollow;
    if (plate) {
        if (pf > 0 && pf < 4) blit_plate_rot(plate, followAng[pf]);
        else memcpy(s_buf, plate, (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t));
    }
    else lv_canvas_fill_bg(s_canvas, lv_color_hex(theme_style::clock().bg), LV_OPA_COVER);

    // Live text banners in the design's real baked font (+ firmware glow). A curved banner
    // arcs along the rim instead of sitting on a straight baseline.
    //
    // Used to stay behind CUSTOM_HAS_TEXT{1,2}, a compile-time gate baked in by whichever
    // Launch Kit push happened to run last. `show` is the runtime gate now, same as every
    // other layer here.
    //
    // A LAMBDA because the design chooses which side of the hands these fall on. Drawn before
    // them the hands sweep over the words, which is what a watch does and what this firmware
    // has always done; drawn after, the words sit on top, which a date window or a signature
    // across the dial wants. THEME_CAPS 43.
    auto draw_banners = [&]() {
        {
            const theme_style::ClockText &t = theme_style::clock().text1;
            if (t.show) {
                if (t.curved) draw_baked_arc_text(theme_font::clock_text1(), t.fmt, (float)t.curveR, t.arcDeg, t.color, ti, "text1", (lv_opa_t)t.opa, t.upper);
                else draw_baked_text(theme_font::clock_text1(), t.fmt, t.x, t.y, t.color, t.glow, t.glowColor, t.align, ti, "text1", (lv_opa_t)t.opa, t.bg, t.bgOpa, t.radius, t.upper);
            }
        }
        {
            const theme_style::ClockText &t = theme_style::clock().text2;
            if (t.show) {
                if (t.curved) draw_baked_arc_text(theme_font::clock_text2(), t.fmt, (float)t.curveR, t.arcDeg, t.color, ti, "text2", (lv_opa_t)t.opa, t.upper);
                else draw_baked_text(theme_font::clock_text2(), t.fmt, t.x, t.y, t.color, t.glow, t.glowColor, t.align, ti, "text2", (lv_opa_t)t.opa, t.bg, t.bgOpa, t.radius, t.upper);
            }
        }
    };
#if defined(ESP_PLATFORM)
    s_phPlate += (float)(micros() - ph0) / 1000.0f;
    const uint32_t ph1 = micros();
#endif
    if (!theme_style::clock().textOverHands) draw_banners();
#if defined(ESP_PLATFORM)
    s_phText += (float)(micros() - ph1) / 1000.0f;
#endif

    // kind 3/4 = the two static image layers — same pivot/center/blend metadata as
    // a hand, just always angle 0 (they never rotate, see custom_sprite.cpp).
    // A railway dial's minute hand sits ON a minute mark and never between two. Giving it
    // the seconds fraction put it wherever the clock happened to be when the Orb booted:
    // start at 12:02:15 and the hand drew a quarter of the way to the 3, and stayed there.
    // Whole minutes for railway, fractional for everything else, where the creep is the
    // point. The hour hand follows from mins, so on a railway dial it steps with it.
#if defined(ESP_PLATFORM)
    const uint32_t ph2 = micros();
#endif
    const theme_style::Clock &csA = theme_style::clock();
    const bool railwayNow = csA.secondRailway && csA.secondSweep;
    const float sec = ti->tm_sec;
    float mins = minute_hand_mins(railwayNow, ti->tm_min, ti->tm_sec);
    // Mid-step, the hand is between this minute and the next. Whole minutes resume the
    // instant the step ends, so it always comes to rest on a mark.
    if (railwayNow && s_stepEase >= 0.0f) mins = (float)ti->tm_min + s_stepEase;
    const float hrs = (ti->tm_hour % 12) + mins / 60.0f;
    const float ang[5] = { hrs * 30.0f, mins * 6.0f, sec * 6.0f, 0.0f, 0.0f };
    // Geometry, draw order, and the per-hand show gate come from the active theme at
    // runtime (theme_style, fed by /themes/<slug>/clock_style.json) rather than from the
    // compile-time CUSTOM_* macros, so hands travel with the theme like every other
    // layer. The macros are still the seed defaults inside theme_style::load().
    const theme_style::Clock &cs = theme_style::clock();
    // Every shadow first, then every hand.
    //
    // Interleaving them would let the minute hand's shadow fall across the hour hand drawn
    // below it, which is what really happens but reads as a smudge on a 466 px dial. Laying
    // all the shadows on the face and then standing the hands on top is the same choice a
    // watch photographer makes with a diffuser, and it costs a second short loop.
    if (cs.shadowOn) {
        bool pastStopSh = false;
        bool laidShadow = false;   // the cached pair goes down once, at the first of the two
        for (int i = 0; i < cs.orderN; ++i) {
            const int k = cs.order[i];
            // Shadows are ALL laid down before any hand, so the shadow of a hand this compose
            // stops below has to be left out with it, or restoring that hand later would leave
            // its old shadow printed underneath.
            if (stopKind >= 0 && k == stopKind) { pastStopSh = true; continue; }
            if (stopKind >= 0 && pastStopSh) continue;   // above the stop: drawn later
            if (k < 0 || k > 2) continue;              // statics do not cast; they are the face
            // The cached pair, blitted where the first of them would have been drawn, so the
            // sequence is byte-for-byte the one the slow path produces.
            if (s_layUse && (k == 0 || k == 1)) {
                if (!laidShadow) { laidShadow = true; lay_blit(s_layShadow); }
                continue;
            }
            const theme_style::Hand &hd = cs.hand[k];
            if (!hd.show) continue;
            CustomSprite sh = custom_shadow(k);
            // Say so when a shadow was asked for and did not arrive.
            //
            // This has now silently gone missing twice, both times right after a theme
            // install, and both times a reboot cured it before anything could be learned:
            // the sprites are decoded lazily into PSRAM and the hour and minute shadows are
            // the two largest allocations on the dial (294 KB and 539 KB), so they are the
            // first things to fail when an install has just left memory tight. The loader
            // reports its own failures, but only at the moment they happen, and by the time
            // anyone notices a missing shadow that line is long gone.
            //
            // Latched, not per frame: this runs sixty times a second and the interesting
            // event is the transition, not the state.
            static bool s_warned[3] = { false, false, false };
            if (k >= 0 && k < 3) {
                if (!sh.data && !s_warned[k]) {
                    s_warned[k] = true;
#if defined(ESP_PLATFORM)
                    Serial.printf("[clock] shadow %d wanted but not loaded - PSRAM free %u KB\n",
                                  k, (unsigned)(ESP.getFreePsram() / 1024));
#endif
                } else if (sh.data && s_warned[k]) {
                    s_warned[k] = false;
#if defined(ESP_PLATFORM)
                    Serial.printf("[clock] shadow %d is back\n", k);
#endif
                }
            }
            // Same art, same angle, same pivot as the hand — only the centre moves, and it
            // moves in SCREEN space, which is the whole reason the light appears to stay put
            // while the hand goes round.
            if (sh.data) blend_shadow(sh.data, sh.w, sh.h, hd.pivotX, hd.pivotY,
                                      (float)(hd.centerX + cs.shadowDX),
                                      (float)(hd.centerY + cs.shadowDY), ang[k]);
        }
    }
    bool pastStop = false;
    bool laidHand = false;   // likewise, once, at the first of minute/hour
    for (int i = 0; i < cs.orderN; ++i) {
        const int k = cs.order[i];
        if (k < 0 || k > 4) continue;
        // Stop below the hand this cache is for. Everything from there up is redrawn by
        // whoever owns the cache, clipped to the rows that hand actually covers, which is what
        // lets a design order a hand ABOVE another and still have both move cheaply. Zion:
        // "there's never a reason to not have a feature we get working not work for all the
        // themes."
        if (stopKind >= 0 && pastStop) continue;
        if (stopKind >= 0 && k == stopKind) { pastStop = true; continue; }
        if (s_layUse && (k == 0 || k == 1)) {
            if (!laidHand) { laidHand = true; lay_blit(s_layHand); }
            continue;
        }
        const theme_style::Hand &hd = cs.hand[k];
        if (!hd.show) continue;
        CustomSprite spr = custom_hand(k);
        if (spr.data) blend_custom_hand(spr.data, spr.w, spr.h, hd.pivotX, hd.pivotY,
                                        (float)hd.centerX, (float)hd.centerY, ang[k], hd.blend);
    }
#if defined(ESP_PLATFORM)
    s_phHands += (float)(micros() - ph2) / 1000.0f;
    const uint32_t ph3 = micros();
#endif
    if (cs.textOverHands) draw_banners();

    // Row by row and inside the clip, so a sweeping hand pays for its own box rather than
    // all 217,156 pixels. Full screen this is 26 ms, the second largest cost on the dial.
    if (overlay && withOverlay) {
        for (int dy = s_clipY0; dy <= s_clipY1; ++dy) {
            const int base = dy * SCREEN_W;
            for (int dx = s_clipX0; dx <= s_clipX1; ++dx) {
                const int i = base + dx;
                const uint8_t a = overlay[i * 3 + 2];
                if (!a) continue;
                lv_color_t sc; sc.full = (uint16_t)(overlay[i * 3] | (overlay[i * 3 + 1] << 8));
                s_buf[i] = lv_color_mix(sc, s_buf[i], a);
            }
        }
    }
}

// A full compose, with its phase breakdown, on whatever path asked for it.
//
// The breakdown already existed and was only reported from the sweep path's rebuild branch,
// so a design with secondSweep off — which is every dial that does not ask to sweep — could
// not be measured at all. That is the case an animated background is most expensive in,
// because every frame of the animation is one of these, so it is the one that most needed a
// number. Reported every eighth compose rather than every one: the line itself costs serial
// time, and eight of these is several seconds of wall clock.
// Both defined below, with the caches whose staleness rules they feed.
static float minute_angle_now(const struct tm *ti);
static float hour_angle_now(const struct tm *ti);

static void draw_custom(const struct tm *ti) {
    // THE FAST PATH FOR AN ANIMATED BACKGROUND.
    //
    // A full face costs 527 ms on this design and 472 of it is the hands, so a background
    // that wants eight frames a second is asking for four seconds of work per second. The
    // hands are not what changed: build them once and blit them.
    //
    // Only ever an optimisation. If the layers cannot be allocated, or either hand has
    // turned far enough to matter, this falls through to exactly the compose it always did.
    const float minAng = minute_angle_now(ti);
    const float hrAng  = hour_angle_now(ti);
    s_layUse = layers_tick(minAng, hrAng);
#if defined(ESP_PLATFORM)
    const uint32_t t0 = micros();
    compose_custom(ti, -1, true);
    const float took = (float)(micros() - t0) / 1000.0f;
    static int   runs = 0;
    static float sum  = 0.0f;
    sum += took;
    if (++runs >= 8) {
        // `rest` is whatever is not plate, text or hands, and on this screen it is almost
        // entirely the overlay: a full-screen alpha mix over 217,156 pixels.
        const float avg = sum / runs;
        Serial.printf("[compose] full face %.0f ms  -> plate %.0f  text %.0f  hands %.0f  rest %.0f\n",
                      avg, s_phPlate / runs, s_phText / runs, s_phHands / runs,
                      avg - (s_phPlate + s_phText + s_phHands) / runs);
        runs = 0; sum = 0.0f;
        s_phPlate = 0; s_phText = 0; s_phHands = 0; s_phRest = 0;
    }
#else
    compose_custom(ti, -1, true);
#endif
    s_layUse = false;   // never leaks into the sweep's own partial composes
}

// ---- the smooth second hand -------------------------------------------------
//
// The dial without its second hand and without its glass, kept between frames. Everything in
// it changes at most once a minute, so it is composed once and then only the sweeping hand's
// own rectangle is rebuilt: copy that box back out of here, blend the hand into it, put the
// glass over it, and invalidate nothing else.
//
// Measured, which is why it is built this way. A full custom redraw is 72 ms, and 55 of those
// are the two full-screen passes: 28.7 ms copying the plate and 26.1 ms mixing the overlay
// across 217,156 pixels. Both scale with area, so a hand covering a quarter of the dial costs
// a quarter of each, and a sweep becomes affordable rather than impossible.

static lv_timer_t *s_tick = nullptr;
// The tick has its own timer, and that is the point of it.
//
// It used to fire from the drawing callback, which ties the sound to whatever the dial costs
// to compose. On a design that sweeps, that callback runs on the sweep's own period, which is
// anything from 33 to 150 ms and adapts, so the tick landed up to a sixth of a second from
// where it belonged and the amount varied second to second. On one that does not sweep, the
// callback also redraws the whole face, between 70 and 250 ms of work, in the same task.
//
// A clock's tick is the one sound on this device where timing IS the content. So it is its
// own timer, it draws nothing, and it re-aims itself at the next whole second every time it
// fires. Zion: "i'm still hearing clicks drop."
static lv_timer_t *s_beat = nullptr;
// A rolling average of what one sweep frame costs, in milliseconds, measured end to end
// including everything LVGL then does with it.
static float s_sweepMs = 45.0f;
static uint32_t s_tickPeriod = 0;

// 30 a second is the ceiling: the hand turns six degrees a second, so a step is a fifth of a
// degree, well under a pixel at the tip, and asking for more would spend the whole device on
// motion nobody can see. 200 ms is the floor, for a design heavy enough that anything faster
// would be a promise the renderer cannot keep.
// THE HAND BEATS. It does not glide.
//
// Greg's observation, 2026-10-06: "28,800 beats per hour ... that is what most clocks run at
// so a natural beat at that rate is probably what we should optimize for, or half of it."
// beats-per-hour counts half-oscillations, so 28,800 is EIGHT a second, and the common rates
// are 18,000 (five), 21,600 (six), 28,800 (eight) and 36,000 (ten).
//
// This screen was already half-committed to the idea. THEME_CAPS 57 is tickRate, and its own
// note says "a mechanical watch beats at 2 or 4, and that faster beat IS the sweep of its
// second hand" — but only the AUDIO ever used it. The hand itself chased a continuous
// position at whatever rate the device could deliver, so every step was a slightly different
// size and a slightly different length of time apart. That is what reads as jerky: not the
// size of the steps, which at a 271 px reach are under two pixels, but their unevenness. A
// real escapement is relentlessly regular, and regular is what the eye reads as smooth.
//
// So the hand is quantised to a beat and the frame is aimed at the beat boundary. Eight by
// default, because that is the most common high-beat movement and the richest-looking; the
// theme's own tickRate wins when it declares one, since a theme that ships recordings off a
// 21,600 watch should move like one. And when the design is too expensive to hold eight, it
// drops to four rather than slipping: 14,400 is also a real movement, so the degraded case
// still looks like a watch instead of looking like a struggling one.
static int sweep_beat() {
    const theme_style::Clock &cs = theme_style::clock();
    if (cs.tickRate >= 2) return cs.tickRate;       // the theme owns its own movement

    // WHILE THE BACKGROUND IS MOVING, THE HAND KEEPS ITS TIME.
    //
    // Not a compromise — the measurement says the alternative does not work. A hand frame
    // repaints the second hand's own box and costs about 13 ms. A background frame repaints
    // all 217,156 pixels and costs about 200. A beat slot at eight a second is 125 ms. So
    // every background frame overruns its slot and swallows the beat behind it: asking for
    // eight against a 2 fps background measured SIX delivered, with 41% of the CPU still
    // idle. Not a budget problem — one of the two things sharing this thread takes longer
    // than the other's entire slot.
    //
    // Two beats in every eight arriving 75 ms late is worse than eight evenly spaced ones,
    // because irregularity is what the eye catches and not rate. Greg, on seeing both:
    // 4 fps background with a 4 beat hand was "a better experience" than 2 fps with a
    // nominal 8, and he was right for a reason I had argued past — at equal rates the two
    // coincide, every frame advances the background AND the hand, and nothing interrupts
    // anything. At different rates they beat against each other.
    //
    // So while frames are running the hand takes the background's rate as its own. When the
    // background is still there is nothing to collide with and it goes back to eight.
    // IN PHASE WITH IT, not equal to it. A MULTIPLE of the background's rate.
    //
    // Taking the background's rate outright is wrong at the slow end: a 2 fps background
    // would give a hand that moves twice a second, which is worse than anything this screen
    // has done yet. What matters is that the two COINCIDE, and a multiple coincides just as
    // well as equality does — at 2 fps background and 4 beats, every background change lands
    // on a beat and the beats between it are free.
    //
    // Floor of four, because that is the slowest step this dial has been judged acceptable
    // at, and four also keeps the slot at 250 ms, which is comfortably longer than the ~200
    // ms a full-screen background frame costs. Eight would put the slot at 125 ms and the
    // background frame would overrun it — measured, and the reason this function exists.
    const theme_style::Clock::BgAnim &ba = cs.bgAnim;
    if (ba.frames > 0 && (ba.loop || bg_anim_playing())) {
        int f = ba.fps < 1 ? 1 : ba.fps;
        if (f >= 4) return f;              // already fast enough; stay exactly on it
        int beat = f;
        while (beat < 4) beat += f;        // smallest multiple of f that reaches four
        return beat;
    }
    // Two full frames of headroom, the same rule sweep_period() uses below.
    if (s_sweepMs > 0.0f && s_sweepMs * 2.0f > 110.0f) return 4;   // 14,400 bph
    return 8;                                                       // 28,800 bph
}

// The hand's position, snapped back to the beat it is in.
static float beat_quantize(float secs) {
    const int b = sweep_beat();
    if (b <= 1) return secs;
    const float q = floorf(secs * (float)b) / (float)b;
    return q;
}

// Milliseconds until the next beat, so a step lands ON it rather than near it.
static uint32_t beat_aim() {
    const int b = sweep_beat();
    if (b <= 1) return 1000;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    const long  slotUs = 1000000L / b;
    const long  intoUs = (long)tv.tv_usec % slotUs;
    uint32_t ms = (uint32_t)((slotUs - intoUs) / 1000);
    if (ms < 5) ms += (uint32_t)(slotUs / 1000);   // too close to chase; take the next one
    return ms;
}

static uint32_t sweep_period() {
    // Twice the compositing, because LVGL then renders the invalidated box and pushes it over
    // QSPI, roughly as much again. Measured: 26 ms of work held 13 frames a second at a 70 ms
    // period, and 52 ms of work could not hold 25 at a 40 ms one.
    float ms = s_sweepMs * 2.0f;
    if (ms < 33.0f) ms = 33.0f;
    if (ms > 150.0f) ms = 150.0f;
    return (uint32_t)ms;
}

// Defined below, beside the minute-move path it guards; declared here because the cache
// BUILD (further up) is its first caller.
static bool nomin_cache_usable();

static lv_color_t *s_under = nullptr;
// The dial with NO minute hand on it, and nothing above the minute hand either.
//
// s_under is rebuilt whenever the minute hand has to creep a pixel, which on a long hand is
// every three seconds, and a rebuild is a whole dial: 28 ms reading the plate out of flash,
// 44 ms turning the hands, 19 ms copying the result. 92 ms measured on Zion's Orb, during
// which a sweeping second hand cannot move at all, which is seen as a stutter every few
// seconds.
//
// Almost none of that work is needed. The plate has not changed and the hour hand has barely
// moved. So this holds everything below the minute hand, and moving the minute means
// restoring only the rows it covers out of here and drawing it again. 30,000 pixels instead
// of 217,000, and no flash read.
//
// Rebuilt only when something under the minute hand actually changes: the hour, the
// background frame, or the theme.
static lv_color_t *s_noMin = nullptr;
static int  s_noMinHr = -1;
static bool s_noMinValid = false;
// The hour hand's angle at the moment that cache was composed. The hour, on its own, is not
// enough: it only changes once an hour and the hand moves the whole time.
static float s_noMinHrAng = -1000.0f;
// The angle the minute hand was last drawn at, so a move knows what to wipe.
static float s_prevMinAng = -1000.0f;
static int  s_underMin = -1, s_underHr = -1;   // what minute this cache is of
// WHICH CANVAS these caches are a picture of.
//
// s_buf is taken in onEnter() and given back in onExit(), so it is a different buffer every
// time the clock is looked at. The caches outlive it and carry no memory of that, and every
// freshness test here asks about TIME. Seconds after a trip to Settings the cache reads as
// perfectly fresh while describing a canvas that no longer exists, so the cheap road gets
// taken into a canvas that was just filled black and the dial comes back in slivers.
// Zion saw exactly that on 2026-10-04 after changing the tick level.
//
// Staleness is therefore two questions, not one: is this of the right MOMENT, and is it of
// the right CANVAS. onEnter answers the second by hand as well; this is what makes a future
// path that forgets to impossible to get wrong.
static lv_color_t *s_underFor = nullptr;
// Minutes-of-the-hour, fractional, that the cache was composed at. The minute hand lives
// in the cache, so for the whole life of a cache that hand cannot move. Holding one cache
// per whole minute is what made the Orb's minute hand jump a full division at the top of
// the minute instead of creeping, which is right for a railway dial and wrong for every
// other sweeping one. Jean-Paul Stringaro spotted it against Studio, 2026-09-29.
static float s_underMins = -1.0f;
// What a full compose actually costs on this design, rolling average, milliseconds. The
// sweep has measured itself for a long time; the compose never did, and a figure noted for
// a plain dial (72 ms) got quoted at a busy one that takes three or four times that. The
// minute hand's step is paid for in whole composes, so this is the number that decides
// whether the step can be animated at all.
#if defined(ESP_PLATFORM)
static float s_composeMs = 0.0f;     // measured on the device, nothing assumed
#else
static float s_composeMs = 20.0f;    // the simulator composes in software, and fast
#endif
static bool s_stepTold = false;
static bool s_prevSecValid = false;
static lv_area_t s_prevSec = { 0, 0, 0, 0 };
static float s_prevAng = 0.0f;
// The frame after a cache rebuild repaints everything, because everything changed.
static bool s_fullNext = true;

// Can this design sweep at all?
//
// The cache holds everything BELOW the hand, so anything a theme draws ABOVE it — a static
// layer ordered on top, or banners set to sit over the hands — would be composited under the
// sweeping hand and come out in the wrong order. Rather than draw it wrongly, such a design
// keeps its tick. The glass is the one exception, because it is re-applied per frame.
// -1 auto (the theme decides), 0 force tick, 1 force sweep. Never persisted: an instrument
// for proving this on the glass before any theme carries the flag.
static int s_forceSweep = -1;

static const char *s_sweepWhyNot = "";

static bool sweep_possible() {
    const theme_style::Clock &cs = theme_style::clock();
    s_sweepWhyNot = "";
    if (s_forceSweep == 0) { s_sweepWhyNot = "forced off"; return false; }
    if (s_forceSweep < 0 && !cs.secondSweep) { s_sweepWhyNot = "the design does not ask for it"; return false; }
    if (s_face != FACE_CUSTOM) return (s_sweepWhyNot = "not a custom face", false);          // the drawn faces have their own painters
    if (!cs.hand[2].show) return (s_sweepWhyNot = "the second hand is hidden", false);
    if (cs.textOverHands && (cs.text1.show || cs.text2.show)) return (s_sweepWhyNot = "the words sit over the hands", false);
    // A layer ABOVE the second hand used to rule this out, because the cache held the whole
    // face and the sweeping hand would have landed on top of things meant to cover it. The
    // cache stops at the second hand now and everything above is redrawn each frame, inside
    // the runs that were wiped, so any order works. Zion's Beige dial was refused for exactly
    // this and his objection was the right one: a feature that works should work everywhere.
    bool seenSecond = false;
    for (int i = 0; i < cs.orderN; ++i) if (cs.order[i] == 2) seenSecond = true;
    if (!seenSecond) s_sweepWhyNot = "the second hand is not in the draw order";
    return seenSecond;
}

// The box the second hand can reach at this angle, padded by a pixel for the bilinear tap.
static lv_area_t hand_box(int kind, float angDeg) {
    const theme_style::Hand &hd = theme_style::clock().hand[kind];
    CustomSprite spr = custom_hand(kind);
    const int sw = spr.data ? spr.w : 0, sh = spr.data ? spr.h : 0;
    const float px = (float)hd.pivotX, py = (float)hd.pivotY;
    const float th = angDeg * DEG2RAD, ct = cosf(th), st = sinf(th);
    // The four corners of the sprite, turned about the pivot. A rectangle, not a disc: the
    // whole point of the box is that it is smaller than the swing.
    const float xs[4] = { -px, sw - px, sw - px, -px };
    const float ys[4] = { -py, -py, sh - py, sh - py };
    float lo_x = 1e9f, hi_x = -1e9f, lo_y = 1e9f, hi_y = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const float rx = xs[i] * ct - ys[i] * st + (float)hd.centerX;
        const float ry = xs[i] * st + ys[i] * ct + (float)hd.centerY;
        if (rx < lo_x) lo_x = rx;
        if (rx > hi_x) hi_x = rx;
        if (ry < lo_y) lo_y = ry;
        if (ry > hi_y) hi_y = ry;
    }
    lv_area_t a;
    a.x1 = (lv_coord_t)fmaxf(0.0f, floorf(lo_x) - 2.0f);
    a.y1 = (lv_coord_t)fmaxf(0.0f, floorf(lo_y) - 2.0f);
    a.x2 = (lv_coord_t)fminf((float)SCREEN_W - 1, ceilf(hi_x) + 2.0f);
    a.y2 = (lv_coord_t)fminf((float)SCREEN_H - 1, ceilf(hi_y) + 2.0f);
    return a;
}

static void area_join(lv_area_t &a, const lv_area_t &b) {
    if (b.x1 < a.x1) a.x1 = b.x1;
    if (b.y1 < a.y1) a.y1 = b.y1;
    if (b.x2 > a.x2) a.x2 = b.x2;
    if (b.y2 > a.y2) a.y2 = b.y2;
}

// The run of dx, within one row, that a sprite at this angle and centre can touch.
//
// The same arithmetic the blits use, lifted out so the RESTORE and the GLASS can be as
// tight as the drawing is. A second hand is a thin diagonal; its bounding box is three or
// four times its own area, and copying and re-glassing that whole rectangle was costing
// more than the hand itself.
static bool sprite_span(int dy, float ang, float cx, float cy, int pivotX, int pivotY,
                        int sw, int sh, int &lo, int &hi) {
    const float th = ang * DEG2RAD, ct = cosf(th), st = sinf(th);
    const float oy = dy - cy;
    const float ax = oy * st + pivotX, ay = oy * ct + pivotY;
    float uLo = -4000.0f, uHi = 4000.0f;
    bool dead = false;
    row_span(ct,  ax, (float)(sw - 1), uLo, uHi, dead);
    row_span(-st, ay, (float)(sh - 1), uLo, uHi, dead);
    if (dead || uHi < uLo) return false;
    lo = (int)floorf(cx + uLo) - 2;
    hi = (int)ceilf(cx + uHi) + 2;
    return true;
}

// How stale the cached minute hand is allowed to get, in fractional minutes.
//
// The minute hand is in the cache, so it only moves when the cache is rebuilt. Rebuilding
// once a whole minute makes it jump a division; rebuilding every frame would cost a full
// compose thirty times a second and there would be no sweep left. The honest limit is the
// screen: rebuild once the tip has travelled about a pixel, and the hand reads as creeping
// because nothing finer than a pixel can be shown anyway.
//
// reach is the hand's own length past the pivot, in pixels, so a short hand on a sub-dial
// rebuilds less often than a long one that spans the glass. 6 degrees a minute is the
// minute hand's rate, so a pixel at radius r takes (1 / (r * 6 * DEG2RAD)) minutes.
//
// A railway design is exempt and keeps the whole-minute cache: there the jump is the point.
static float cache_minutes_allowed() {
    const theme_style::Clock &cs = theme_style::clock();
    if (cs.secondRailway && cs.secondSweep) return 1.0f;   // the step IS the design
    const theme_style::Hand &hd = cs.hand[1];
    if (!hd.show) return 1.0f;                             // no minute hand, nothing to creep
    CustomSprite spr = custom_hand(1);
    if (!spr.data) return 1.0f;
    // The longer side of the sprite away from its pivot: that is what sweeps the biggest arc.
    const float up = (float)hd.pivotY, down = (float)(spr.h - hd.pivotY);
    const float reach = up > down ? up : down;
    if (reach < 8.0f) return 1.0f;                         // too short for a pixel to matter
    const float mins = 1.0f / (reach * 6.0f * DEG2RAD);
    // Never more often than a tenth of a second's worth of work, never less often than once
    // a minute. The floor is what protects the sweep on a very long hand.
    return mins < 0.05f ? 0.05f : (mins > 1.0f ? 1.0f : mins);
}

// Compose the dial without its second hand and keep it. Once a minute, not once a frame.
// Where the minute hand points right now, by the same rule compose_custom uses. One function,
// because a cache that disagrees with the compose about this would leave a hand drawn twice.
// HOW LONG THE CACHE UNDER THE MINUTE HAND MAY STAND, in minutes of wall time.
//
// s_noMin holds everything below the minute hand, and the HOUR hand is the thing down there
// that moves. It was only ever thrown away when tm_hour changed, so the hour hand was composed
// once an hour and the in-place minute move kept restoring that same frozen copy over and over.
// On a sweeping dial the hour hand therefore did not creep at all: it stood still for up to an
// hour and then jumped a whole division. Reported by wizard.oz on 2026-10-06, who read the
// change that caused it and named it before I did.
//
// The minute hand has had the right rule since 2.16.38, in cache_minutes_allowed just above:
// hold the cache until the hand's TIP has travelled a pixel, because a hand that has not moved
// a pixel has not moved. This is that rule for the hand one layer down. The hour hand turns at
// half a degree a minute against the minute hand's six, and is shorter, so it buys roughly a
// minute where the minute hand buys three seconds. A recompose a minute is nothing beside the
// one every 3.4 seconds that 2.16.56 was written to remove.
static float hour_cache_minutes() {
    const theme_style::Clock &cs = theme_style::clock();
    const theme_style::Hand &hd = cs.hand[0];
    if (!hd.show) return 60.0f;                    // no hour hand: nothing under there to go stale
    CustomSprite spr = custom_hand(0);
    if (!spr.data) return 60.0f;
    const float up = (float)hd.pivotY, down = (float)(spr.h - hd.pivotY);
    const float reach = up > down ? up : down;
    if (reach < 8.0f) return 60.0f;                // too short for a pixel to matter
    // Degrees for one pixel at that reach, then at the hour hand's own half a degree a minute.
    const float mins = (1.0f / (reach * DEG2RAD)) / 0.5f;
    // Never more often than every two seconds, and never longer than the hour it used to be.
    return mins < 0.034f ? 0.034f : (mins > 60.0f ? 60.0f : mins);
}

static float hour_angle_now(const struct tm *ti) {
    const theme_style::Clock &cs = theme_style::clock();
    const bool rw = cs.secondRailway && cs.secondSweep;
    float mins = minute_hand_mins(rw, ti->tm_min, ti->tm_sec);
    if (rw && s_stepEase >= 0.0f) mins = (float)ti->tm_min + s_stepEase;
    return ((ti->tm_hour % 12) + mins / 60.0f) * 30.0f;
}

static float minute_angle_now(const struct tm *ti) {
    const theme_style::Clock &cs = theme_style::clock();
    const bool rw = cs.secondRailway && cs.secondSweep;
    float mins = minute_hand_mins(rw, ti->tm_min, ti->tm_sec);
    if (rw && s_stepEase >= 0.0f) mins = (float)ti->tm_min + s_stepEase;
    return mins * 6.0f;
}

static bool rebuild_under(const struct tm *ti) {
    if (!s_buf) return false;
    if (!s_under) {
        const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_under = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_under = (lv_color_t *)malloc(bytes);
#endif
        // No cache, no sweep. Falling back to a tick is a slower clock; drawing without the
        // cache would be a wrong one.
        if (!s_under) return false;
    }
    if (!s_noMin) {
        const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_noMin = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
        s_noMin = (lv_color_t *)malloc(bytes);
#endif
        // Not fatal. Without it every minute move costs a full rebuild, which is exactly what
        // happened before this existed, so the clock is slower and not wrong.
    }
    clip_reset();
    // The lower cache first, from the same compose: stop below the minute hand, keep that,
    // then carry on up to the second hand for s_under. Two memcpys and one compose rather
    // than two composes.
    if (s_noMin && nomin_cache_usable()) {
        compose_custom(ti, 1, false);
        memcpy(s_noMin, s_buf, (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t));
        s_noMinHr = ti->tm_hour;
        s_noMinHrAng = hour_angle_now(ti);
        s_noMinValid = true;
    }
    clip_reset();
    compose_custom(ti, 2, false);
    memcpy(s_under, s_buf, (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t));
    s_underFor = s_buf;
    s_underHr = ti->tm_hour;
    s_underMin = ti->tm_min;
    s_underMins = (float)ti->tm_min + (float)ti->tm_sec / 60.0f;
    s_prevMinAng = minute_angle_now(ti);
    s_prevSecValid = false;
    s_fullNext = true;
    return true;
}

// Is the s_noMin cache safe to use on THIS design?
//
// No, when a MOVING hand sits below the minute hand in the draw order.
//
// s_noMin is built with stopKind=1, "everything below the minute hand", and the minute-move
// path restores rows out of it. That is only sound while the things below the minute hand
// hold still. With Steam Punk's order — [static1, static2, SECOND, minute, hour] — the
// second hand is below the minute hand, so it gets composited INTO s_noMin at whatever
// angle it happened to have when the cache was built, and every later minute move reprints
// that stale hand. The sweep only ever wipes the box the hand is in NOW, so a copy left at
// a different angle is never erased.
//
// Reported by Greg, 2026-10-06, and the symptoms name the mechanism exactly: a second
// second hand, with a shadow of its own, absent at boot and appearing a little later (the
// first minute move), fixed in one spot for ever (the cache is not rebuilt), on the
// sub-dial, at a position that differs between reboots (wherever the hand was when the
// cache happened to be built).
//
// s_under already handles any draw order — that was Zion's Beige dial, and the fix was to
// stop the cache at the second hand and redraw everything above it per frame. s_noMin never
// got the same treatment, because until this theme nothing put a moving hand underneath the
// minute hand.
//
// This is the GUARD, not the cure: it declines the optimisation rather than risking a wrong
// picture, so these designs fall back to the full rebuild on a minute move — correct, and
// about every three seconds. The cure is to stop s_noMin below whichever moving hand comes
// first in the order, and that is a change to the most intricate code on this screen, so it
// wants hardware in front of it rather than being bundled in here.
static bool nomin_cache_usable() {
    const theme_style::Clock &cs = theme_style::clock();
    if (!cs.hand[2].show) return true;          // no second hand to go stale
    for (int i = 0; i < cs.orderN; ++i) {
        if (cs.order[i] == 2) return false;     // second hand reached first: it is BELOW the minute
        if (cs.order[i] == 1) return true;      // minute hand reached first: the usual case
    }
    return true;
}

// MOVE THE MINUTE HAND WITHOUT REBUILDING THE DIAL.
//
// The same trick the sweep uses on the second hand, applied one layer down. Restore only the
// rows the minute hand covers, out of the cache that has no minute hand in it, draw the hand
// again, and copy those rows back. 30,000 pixels rather than 217,000, and no plate read.
//
// Measured before this, on Zion's Orb: 92 ms every 3.7 seconds, with a sweeping second hand
// frozen for all of it. One frame at the 14 fps he was getting is 71 ms, so the stall did not
// fit inside a frame and was seen.
//
// Returns false when it cannot be done, and every caller falls back to the full rebuild. That
// is the whole safety story here: no cache, no sprite, no hand, nothing stale, just slower.
static bool refresh_minute(const struct tm *ti, float ang) {
    if (!s_buf || !s_under || !s_noMin || !s_noMinValid) return false;
    if (!nomin_cache_usable()) return false;   // a moving hand is below the minute: see above
    if (s_underFor != s_buf) return false;           // a cache of some previous canvas
    if (s_noMinHr != ti->tm_hour) return false;      // the hour hand moved; that is under us
    // ...and it moves BETWEEN hours too, which this used to miss entirely. See
    // hour_cache_minutes: hold the cache until the hour hand's tip has travelled a pixel,
    // then rebuild rather than keep painting a hand that is no longer where it belongs.
    {
        float apart = hour_angle_now(ti) - s_noMinHrAng;
        while (apart > 180.0f)  apart -= 360.0f;
        while (apart < -180.0f) apart += 360.0f;
        if (apart < 0.0f) apart = -apart;
        if (apart >= hour_cache_minutes() * 0.5f) return false;
    }
    const theme_style::Clock &cs = theme_style::clock();
    const theme_style::Hand &hd = cs.hand[1];
    if (!hd.show) return false;
    CustomSprite spr = custom_hand(1);
    if (!spr.data) return false;
    if (s_prevMinAng < -900.0f) return false;        // nothing to restore from yet

    lv_area_t box = hand_box(1, ang);
    area_join(box, hand_box(1, s_prevMinAng));
    if (box.x2 < box.x1 || box.y2 < box.y1) return false;

    const bool shadow = cs.shadowOn && custom_shadow(1).data;
    // STATIC, not on the stack, for the reason sweep_frame gives: two arrays of 466 ints is
    // 3.7 KB on a task whose headroom is measured in single kilobytes. Only one of these runs
    // at a time.
    static int runLo[SCREEN_H], runHi[SCREEN_H];
    const float angs[2] = { s_prevMinAng, ang };
    for (int y = box.y1; y <= box.y2; ++y) {
        int lo = 1 << 20, hi = -(1 << 20), a, b;
        for (int i = 0; i < 2; ++i) {
            if (sprite_span(y, angs[i], (float)hd.centerX, (float)hd.centerY,
                            hd.pivotX, hd.pivotY, spr.w, spr.h, a, b)) {
                if (a < lo) lo = a;
                if (b > hi) hi = b;
            }
            if (shadow && sprite_span(y, angs[i], (float)(hd.centerX + cs.shadowDX),
                                      (float)(hd.centerY + cs.shadowDY),
                                      hd.pivotX, hd.pivotY, spr.w, spr.h, a, b)) {
                if (a < lo) lo = a;
                if (b > hi) hi = b;
            }
        }
        if (lo < box.x1) lo = box.x1;
        if (hi > box.x2) hi = box.x2;
        runLo[y] = lo; runHi[y] = hi;
        if (hi < lo) continue;
        memcpy(&s_buf[y * SCREEN_W + lo], &s_noMin[y * SCREEN_W + lo],
               (size_t)(hi - lo + 1) * sizeof(lv_color_t));
    }

    // Everything from the minute hand up to the second hand, drawn into exactly what was
    // wiped. Shadows first, as the full compose does, then the hands in the design's order.
    s_clipX0 = box.x1; s_clipY0 = box.y1; s_clipX1 = box.x2; s_clipY1 = box.y2;
    s_runLo = runLo; s_runHi = runHi;
    const float mins = minute_hand_mins(cs.secondRailway && cs.secondSweep, ti->tm_min, ti->tm_sec);
    const float hrs  = (ti->tm_hour % 12) + mins / 60.0f;
    const float all[5] = { hrs * 30.0f, ang, 0.0f, 0.0f, 0.0f };
    if (cs.shadowOn) {
        bool past = false;
        for (int i = 0; i < cs.orderN; ++i) {
            const int k = cs.order[i];
            if (k == 2) break;                       // the sweep owns everything from here up
            if (k < 0 || k > 2) continue;
            if (!past) { if (k == 1) past = true; else continue; }
            const theme_style::Hand &oh = cs.hand[k];
            if (!oh.show) continue;
            CustomSprite sh = custom_shadow(k);
            if (sh.data) blend_shadow(sh.data, sh.w, sh.h, oh.pivotX, oh.pivotY,
                                      (float)(oh.centerX + cs.shadowDX),
                                      (float)(oh.centerY + cs.shadowDY), all[k]);
        }
    }
    {
        bool past = false;
        for (int i = 0; i < cs.orderN; ++i) {
            const int k = cs.order[i];
            if (k == 2) break;
            if (k < 0 || k > 4) continue;
            if (!past) { if (k == 1) past = true; else continue; }
            const theme_style::Hand &oh = cs.hand[k];
            if (!oh.show) continue;
            CustomSprite os = custom_hand(k);
            if (os.data) blend_custom_hand(os.data, os.w, os.h, oh.pivotX, oh.pivotY,
                                           (float)oh.centerX, (float)oh.centerY, all[k], oh.blend);
        }
    }
    // The clip STAYS ON until the layers above the second hand have gone back too. Resetting
    // it here drew them over the whole dial instead of over the rows this wiped, which is a
    // louder version of the same fault.
    //
    // Back into the cache the sweep restores from, the same rows and no more. Before either
    // the layers above the second hand or the glass, because that cache holds neither.
    for (int y = box.y1; y <= box.y2; ++y) {
        if (runHi[y] < runLo[y]) continue;
        memcpy(&s_under[y * SCREEN_W + runLo[y]], &s_buf[y * SCREEN_W + runLo[y]],
               (size_t)(runHi[y] - runLo[y] + 1) * sizeof(lv_color_t));
    }

    // AND EVERYTHING THE DESIGN DRAWS ABOVE THE SECOND HAND, over the rows this wiped.
    //
    // The second hand belongs to the sweep and is not drawn here. Whatever sits ABOVE it does
    // not: this function has just restored its rows out of a cache that stops below the
    // minute hand, so those layers are gone from every row it touched, and the sweep frame
    // that follows only repairs its own small box. Everywhere else they stayed missing until
    // something repainted the whole dial, which is every few seconds on a creeping minute
    // hand. Measured on the simulator: 3,496 pixels wrong, and 33 once the frame after it was
    // allowed to repaint everything, which is what pointed at this rather than at the drawing.
    //
    // A design with nothing above its second hand shows none of it, which is why this reached
    // somebody else's Orb rather than Zion's.
    {
        bool past = false;
        for (int i = 0; i < cs.orderN; ++i) {
            const int k = cs.order[i];
            if (k < 0 || k > 4) continue;
            if (k == 2) { past = true; continue; }
            if (!past) continue;
            const theme_style::Hand &oh = cs.hand[k];
            if (!oh.show) continue;
            if (cs.shadowOn && k <= 2) {
                CustomSprite osh = custom_shadow(k);
                if (osh.data) blend_shadow(osh.data, osh.w, osh.h, oh.pivotX, oh.pivotY,
                                           (float)(oh.centerX + cs.shadowDX),
                                           (float)(oh.centerY + cs.shadowDY), all[k]);
            }
            CustomSprite os = custom_hand(k);
            if (os.data) blend_custom_hand(os.data, os.w, os.h, oh.pivotX, oh.pivotY,
                                           (float)oh.centerX, (float)oh.centerY, all[k], oh.blend);
        }
    }
    // AND THE GLASS BACK OVER WHAT WAS JUST REDRAWN.
    //
    // This was missing, and it is Jean-Paul Stringaro's report of 2026-10-05: "portions of
    // the screen / dial show changes in brightness/darkness as the seconds hand sweeps".
    //
    // The caches are deliberately held with no overlay on them, because a sweep frame mixes
    // the glass in per frame on its way out, over exactly the pixels it restored. This
    // function restores its own pixels out of the same unglassed cache and never did. So
    // every few seconds, when the minute hand crept, its bounding box lost the glass and did
    // not get it back until something repainted the whole dial. Measured on the simulator:
    // 9,916 pixels, up to 51 levels out of 63, inside exactly the minute hand's box. On a
    // design with no glass nothing showed at all, which is why it reached a stranger's Orb.
    //
    // It is the same loop sweep_frame ends with, over the rows this one wiped.
    if (const uint8_t *overlay = custom_overlay()) {
        for (int y = box.y1; y <= box.y2; ++y) {
            if (runHi[y] < runLo[y]) continue;
            const int base = y * SCREEN_W;
            for (int x = runLo[y]; x <= runHi[y]; ++x) {
                const int i = base + x;
                const uint8_t a = overlay[i * 3 + 2];
                if (!a) continue;
                lv_color_t sc; sc.full = (uint16_t)(overlay[i * 3] | (overlay[i * 3 + 1] << 8));
                s_buf[i] = lv_color_mix(sc, s_buf[i], a);
            }
        }
    }
    clip_reset();
    s_prevMinAng = ang;
    s_underMin = ti->tm_min;
    s_underMins = (float)ti->tm_min + (float)ti->tm_sec / 60.0f;
    // The next sweep frame repaints its whole box rather than trusting what was in s_buf
    // around the second hand, because this has just rewritten part of it.
    s_prevSecValid = false;
    s_fullNext = true;
    lv_obj_invalidate_area(s_canvas, &box);
    return true;
}

// One frame of sweep: restore the hand's box out of the cache, turn the hand into it, put
// the glass back over that box, and invalidate only that.
static void sweep_frame(float secs) {
#if defined(ESP_PLATFORM)
    const uint32_t t0 = micros();
#endif
    const theme_style::Clock &cs = theme_style::clock();
    const theme_style::Hand &hd = cs.hand[2];
    const float ang = secs * 6.0f;

    lv_area_t box = hand_box(2, ang);
    if (s_prevSecValid) area_join(box, s_prevSec);
    s_prevSec = hand_box(2, ang);
    s_prevSecValid = true;

    s_clipX0 = box.x1; s_clipY0 = box.y1; s_clipX1 = box.x2; s_clipY1 = box.y2;

    // What has to be put back, row by row: everywhere the hand WAS and everywhere it is
    // going, and the same for its shadow. Anything else in the box was never touched.
    //
    // The bounding rectangle of a thin diagonal is three or four times its own area, so
    // restoring and re-glassing the whole box was 18 ms of a 52 ms frame to repair pixels
    // nothing had drawn on.
    CustomSprite spr0 = custom_hand(2);
    const int sw = spr0.data ? spr0.w : 0, sh = spr0.data ? spr0.h : 0;
    const bool shadow = cs.shadowOn && custom_shadow(2).data;
    // STATIC, not on the stack. Two arrays of 466 ints is 3.7 KB, and this runs on the LVGL
    // task whose headroom is measured in single kilobytes; a frame that overflows it would
    // look like a random crash somewhere else entirely. There is only ever one sweep in
    // flight, so one copy is enough.
    static int runLo[SCREEN_H], runHi[SCREEN_H];
    for (int y = box.y1; y <= box.y2; ++y) {
        int lo = 1 << 20, hi = -(1 << 20), a, b;
        if (!s_fullNext && sw > 0) {
            const float angs[2] = { s_prevAng, ang };
            for (int i = 0; i < 2; ++i) {
                if (sprite_span(y, angs[i], (float)hd.centerX, (float)hd.centerY,
                                hd.pivotX, hd.pivotY, sw, sh, a, b)) {
                    if (a < lo) lo = a;
                    if (b > hi) hi = b;
                }
                if (shadow && sprite_span(y, angs[i], (float)(hd.centerX + cs.shadowDX),
                                          (float)(hd.centerY + cs.shadowDY),
                                          hd.pivotX, hd.pivotY, sw, sh, a, b)) {
                    if (a < lo) lo = a;
                    if (b > hi) hi = b;
                }
            }
        } else {
            lo = box.x1; hi = box.x2;
        }
        if (lo < box.x1) lo = box.x1;
        if (hi > box.x2) hi = box.x2;
        runLo[y] = lo; runHi[y] = hi;
        if (hi < lo) continue;
        memcpy(&s_buf[y * SCREEN_W + lo], &s_under[y * SCREEN_W + lo],
               (size_t)(hi - lo + 1) * sizeof(lv_color_t));
    }
    s_fullNext = false;
    s_prevAng = ang;
    // From here on, every layer is confined to exactly what was wiped.
    s_runLo = runLo; s_runHi = runHi;
    CustomSprite spr = custom_hand(2);
    if (cs.shadowOn) {
        CustomSprite sh = custom_shadow(2);
        if (sh.data) blend_shadow(sh.data, sh.w, sh.h, hd.pivotX, hd.pivotY,
                                  (float)(hd.centerX + cs.shadowDX), (float)(hd.centerY + cs.shadowDY), ang);
    }
    if (spr.data) blend_custom_hand(spr.data, spr.w, spr.h, hd.pivotX, hd.pivotY,
                                    (float)hd.centerX, (float)hd.centerY, ang, hd.blend);

    // Everything the design draws ABOVE its second hand, put back over it, inside the box.
    //
    // The cache stops at the second hand, so these are not in it. Redrawing them here is
    // what makes the sweep work on any layer order rather than only on designs that happen
    // to put the second hand on top. They are clipped, so this costs their overlap with the
    // hand's box and nothing more; on a design with nothing above the hand it is an empty
    // loop. The angles are the same ones the cache was built with, so they land exactly
    // where the minute has them.
    {
        struct tm ti;
        time_for_face(&ti);
        {
            // The SAME minute rule the cache was built with. This recomputed it with the
            // plain creeping formula, so on a design that draws its minute hand above the
            // second hand the cache placed that hand on its mark and this put it back on
            // the creep, a fraction of a degree away, every frame. 2.16.39 fixed the rule
            // in compose_custom and missed its twin here, which is the kind of thing having
            // one shared helper is supposed to prevent.
            const bool rwAbove = cs.secondRailway && cs.secondSweep;
            float p_min = minute_hand_mins(rwAbove, ti.tm_min, ti.tm_sec);
            if (rwAbove && s_stepEase >= 0.0f) p_min = (float)ti.tm_min + s_stepEase;
            const float p_hr = (ti.tm_hour % 12) + p_min / 60.0f;
            const float above[5] = { p_hr * 30.0f, p_min * 6.0f, ang, 0.0f, 0.0f };
            // THE CACHED PAIR, HERE, WHICH IS WHERE IT ACTUALLY EARNS ANYTHING.
            //
            // On a draw order that puts the second hand below the minute and hour — which is
            // what sent me looking — s_under stops below the second hand and therefore holds
            // NEITHER of the other two. They are redrawn from here on every sweep frame, and
            // on the frame after any full rebuild the clip is the whole screen, so that is
            // the 472 ms of bilinear sprite work the layer cache exists to remove. The first
            // version of this wired it into draw_custom() only, which the sweep path never
            // calls: correct, validated, and never once executed on a sweeping dial.
            //
            // Order is unchanged from what this loop already did. It draws shadow-then-hand
            // per hand, so the second hand is already underneath the minute and hour shadows
            // here; laying the shadow layer and then the hand layer keeps that.
            const bool layered = layers_tick(above[1], above[0]);
            if (layered) {
                if (cs.shadowOn) lay_blit(s_layShadow);
                lay_blit(s_layHand);
            }
            bool past = false;
            for (int i = 0; i < cs.orderN; ++i) {
                const int k = cs.order[i];
                if (k < 0 || k > 4) continue;
                if (k == 2) { past = true; continue; }
                if (!past) continue;
                if (layered && (k == 0 || k == 1)) continue;   // in the layers already
                const theme_style::Hand &oh = cs.hand[k];
                if (!oh.show) continue;
                if (cs.shadowOn && k <= 2) {
                    CustomSprite osh = custom_shadow(k);
                    if (osh.data) blend_shadow(osh.data, osh.w, osh.h, oh.pivotX, oh.pivotY,
                                               (float)(oh.centerX + cs.shadowDX),
                                               (float)(oh.centerY + cs.shadowDY), above[k]);
                }
                CustomSprite ospr = custom_hand(k);
                if (ospr.data) blend_custom_hand(ospr.data, ospr.w, ospr.h, oh.pivotX, oh.pivotY,
                                                 (float)oh.centerX, (float)oh.centerY,
                                                 above[k], oh.blend);
            }
        }
    }
    if (const uint8_t *overlay = custom_overlay()) {
        for (int dy = s_clipY0; dy <= s_clipY1; ++dy) {
            const int base = dy * SCREEN_W;
            // Exactly the pixels that were restored. Glassing anything else would be
            // re-mixing the overlay onto a pixel that already has it.
            int gx0 = s_clipX0, gx1 = s_clipX1;
            clip_row(dy, gx0, gx1);
            for (int dx = gx0; dx <= gx1; ++dx) {
                const int i = base + dx;
                const uint8_t a = overlay[i * 3 + 2];
                if (!a) continue;
                lv_color_t sc; sc.full = (uint16_t)(overlay[i * 3] | (overlay[i * 3 + 1] << 8));
                s_buf[i] = lv_color_mix(sc, s_buf[i], a);
            }
        }
    }
    clip_reset();
    // The box only. Invalidating the whole canvas would hand back every pixel this exists to
    // avoid touching.
    lv_obj_invalidate_area(s_canvas, &box);

    // THE WORK, not the interval between frames.
    //
    // This measured the gap between one frame and the next, which is a feedback loop with
    // the very thing it sets: a longer period makes a longer gap, which asks for a longer
    // period again. It walked straight up to its own ceiling and the hand fell to four
    // frames a second, worse than the fixed number it replaced. Compositing cost does not
    // depend on how often it is asked for, so that is what gets measured.
#if defined(ESP_PLATFORM)
    {
        const uint32_t took2 = micros() - t0;
        if (took2 < 400000UL) s_sweepMs += 0.1f * ((float)took2 / 1000.0f - s_sweepMs);
        const uint32_t want = sweep_period();
        if (s_tick && want != s_tickPeriod) {
            s_tickPeriod = want;
            lv_timer_set_period(s_tick, want);
        }
    }
#endif
}

// A shadow cast by the second hand has to be inside the box too, or it smears. Widen by the
// light's offset so the restore covers wherever the shadow landed last frame.
static void sweep_pad_for_shadow() {
    const theme_style::Clock &cs = theme_style::clock();
    if (!cs.shadowOn) return;
    const int dx = cs.shadowDX < 0 ? -cs.shadowDX : cs.shadowDX;
    const int dy = cs.shadowDY < 0 ? -cs.shadowDY : cs.shadowDY;
    s_prevSec.x1 = (lv_coord_t)((s_prevSec.x1 - dx) < 0 ? 0 : s_prevSec.x1 - dx);
    s_prevSec.y1 = (lv_coord_t)((s_prevSec.y1 - dy) < 0 ? 0 : s_prevSec.y1 - dy);
    s_prevSec.x2 = (lv_coord_t)((s_prevSec.x2 + dx) > SCREEN_W - 1 ? SCREEN_W - 1 : s_prevSec.x2 + dx);
    s_prevSec.y2 = (lv_coord_t)((s_prevSec.y2 + dy) > SCREEN_H - 1 ? SCREEN_H - 1 : s_prevSec.y2 + dy);
}

// ---- tick + face management -------------------------------------------------
static void redraw(const struct tm *ti) {
    if (!s_canvas || !s_buf) return;
    switch (s_face) {
        case FACE_IMPERIAL: draw_imperial(ti); break;
        case FACE_AVIATOR:  draw_aviator(ti);  break;
        case FACE_OFFICE:   draw_office(ti);   break;
        case FACE_CUSTOM:   draw_custom(ti);   break;
        default:            draw_digital(ti);  break;
    }
    lv_obj_invalidate(s_canvas);
}

// Seconds within the minute, with the fraction. A sweeping hand needs to know where it is
// between ticks, and getLocalTime only ever answers in whole seconds.
static float second_now() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm ti;
    const time_t t = (time_t)tv.tv_sec;
    localtime_r(&t, &ti);
    return (float)ti.tm_sec + (float)tv.tv_usec / 1000000.0f;
}

// The Swiss railway stop, THEME_CAPS 52: the hand goes round in 58.5 seconds and waits at
// 12 until the minute rolls, the way the SBB station clocks and the Mondaine watch do. The
// wait is 60 rather than 0 so the hand sits at the top of its sweep instead of snapping
// back through the dial, and the box maths sees it at the same angle either way.
//
// BOTH flags, every frame, read from the theme on screen right now. theme_style reseeds
// every field on each theme load (seed_defaults), so a design that does not ask for this
// cannot inherit it from the one before; secondSweep is required as well, because a stop is
// a pause in a glide and Studio only ever writes the pair together. This was blamed for the
// hands flashing to twelve in September 2026 and was innocent: that was the clock read
// underneath it (orb_time.h), and the self-test in sim_main.cpp now holds both apart.
static const float STOP_AT = 58.5f;   // the second hand parks here and waits for the roll
static float railway_seconds(float secs) {
    const theme_style::Clock &cs = theme_style::clock();
    if (!cs.secondRailway || !cs.secondSweep) return secs;
    return secs >= STOP_AT ? 60.0f : secs * (60.0f / STOP_AT);
}

// The step, drawn rather than jumped. A real railway minute hand takes a moment to cross
// the gap, and without that moment it simply appears on the far side: correct, and lifeless.
//
// It runs in the last STEP_SECS of the stop, BEFORE the minute rolls, not after it. That is
// the whole trick. From 58.5 s the second hand is already parked at 12, so the full composes
// this needs cost nothing visible: no other hand is moving. Animating after the roll would
// have frozen the first third of a second of the second hand's glide, which is the one thing
// this must not touch. The hand therefore sits a little past its mark for the last third of
// a second of the minute, which is also what a real one does while it is stepping.
//
// Driven by the clock rather than by a frame count, so it lands on the mark at the roll
// whether the device gets five frames into the window or three.
static const float STEP_SECS = 0.60f;    // about 8 frames at the ~72 ms a full compose takes

// Ease in, ease out, no overshoot: smoothstep.
//
// This was easeOutBack, and at this frame rate that was simply wrong. A back ease puts most
// of its travel in the first instant, so the first frame alone covered 71% of the gap, the
// second overshot to 103%, and the last three crawled backwards by a few percent each. Five
// frames shaped like that do not read as one movement, they read as two: a jump to about the
// middle, then a small correction onto the mark. Zion described it as "dut dut", which is
// precisely what those numbers draw.
//
// A step this short cannot afford a curve that spends its frames unevenly. Smoothstep moves
// at most 18% of the gap in any one frame and is symmetric, so the hand accelerates away and
// decelerates in, which is what momentum looks like when you only have eight frames to say
// it with. The overshoot went with it: at eight frames a settle is a second motion, not a
// flourish.
static float ease_step(float t) {
    return t * t * (3.0f - 2.0f * t);
}

// Whether this design can afford to ANIMATE the step at all.
//
// Every frame of the step is a whole compose, because the minute hand lives inside the
// sweep cache. A plain dial composes in about 72 ms and gets eight frames; a busy one with
// pictures takes three or four times that and gets two. Two frames is the worst of both
// worlds: not a movement, and not the clean click it replaced. Zion saw exactly that, twice,
// and described it both times as one big jump followed by a small one, which is what two
// frames of any curve look like.
//
// So the step animates only where there are frames to animate it with, and clicks over in
// one move everywhere else. Measured per design, not assumed, and re-measured as the design
// changes.
static const int STEP_MIN_FRAMES = 6;
static bool step_affordable(float composeMs) {
    if (composeMs <= 0.0f) return false;   // nothing measured yet; do not gamble on the first
    return (STEP_SECS * 1000.0f / composeMs) >= (float)STEP_MIN_FRAMES;
}

// Where in its step the minute hand is for a given wall second, or -1 when it is not
// stepping. The window is asserted to sit inside the stop by the simulator self-test.
static float minute_step_ease(float wallSecs) {
    const float t = (wallSecs - (60.0f - STEP_SECS)) / STEP_SECS;
    return (t > 0.0f && t < 1.0f) ? ease_step(t) : -1.0f;
}

// A tick a second, or a frame every 40 ms while sweeping.
//
// Reset whenever a theme is applied, because whether this design sweeps is the theme's
// answer and not a fixed property of the screen.
static void retime(void) {
    if (!s_tick) return;
    // The sweep asks for whatever it has been managing, not a number chosen in advance.
    //
    // A fixed period was wrong twice over. 40 ms asked for frames the device could not draw,
    // so the timer was late every time and lv_timer_handler saturated for no extra frames.
    // 70 ms was measured honestly, and then the frame got two and a half times cheaper and
    // 70 became a ceiling holding back a hand that could have moved twice as often.
    //
    // s_sweepMs is an average of what a frame actually costs on THIS design — a plain dial
    // is a quarter of the work of a busy one — with a fifth on top so the timer, not the
    // renderer, decides when frames happen. That is radar_view's rule about even arrival,
    // applied without having to guess the number.
    lv_timer_set_period(s_tick, sweep_possible() ? sweep_period() : 1000);
}

// One click a second, from whichever set the worn theme shipped. Nothing else.
//
// Guarded like the drawing callback, so an Orb showing another app or sitting under the wind
// screen is silent. Driven by the second CHANGING rather than by this firing, and re-aimed at
// the next whole second afterwards, so the interval between two clicks is the clock's own and
// not this timer's.
// How long the clock stays silent after it appears, and how long it then takes to come up to
// the level somebody set.
//
// A clock that has just appeared is not yet keeping time smoothly: the first frames are
// expensive, the art may still be baking, and after a theme install or a flash there is real
// work going on behind the face. Ticking through that puts the unsteady part of the start
// into the one thing on this device where unevenness is the whole fault. Zion: "it would be
// better from a user standpoint to experience the clock ticking once it's going to be
// absolutely steady."
//
// So it waits, then arrives rather than switching on. The ramp is on the level rather than
// the audio, so it costs nothing and cannot distort anything.
static const uint32_t TICK_QUIET_MS = 2000;
static const uint32_t TICK_FADE_MS  = 3000;
static uint32_t s_faceShownMs = 0;

static void beat_cb(lv_timer_t * /*t*/) {
    if (!s_beat) return;
    // Stamped on the transition INTO view, not on every frame, so the wait starts when the
    // face appears and runs once. Coming back from another app counts, because that redraws
    // the whole dial and is just as unsteady as a cold start.
    const bool showing = lv_scr_act() == s_screen && !orb_screen_covered();
    static bool wasShowing = false;
    if (showing && !wasShowing) s_faceShownMs = lv_tick_get();
    wasShowing = showing;
    if (!showing) return;
    if (theme_audio::tickCount() <= 0) return;
    // s_noTime READ, not refreshed. The drawing callback maintains it; calling time_for_face
    // again here would be a second clock read per wake for an answer that changes about never.
    if (s_noTime) return;

    struct timeval tv; gettimeofday(&tv, nullptr);
    static long lastSec = -1;
    if ((long)tv.tv_sec != lastSec) {
        lastSec = (long)tv.tv_sec;
        // Silent while it settles, then up over three ticks. Below one percent is silence
        // rather than a sound nobody can hear, which audio_play_pcm treats as nothing to do.
        const uint32_t age = lv_tick_get() - s_faceShownMs;
        int level = audio_tick_level();
        if (age < TICK_QUIET_MS) level = 0;
        else if (age < TICK_QUIET_MS + TICK_FADE_MS)
            level = (int)((float)level * (float)(age - TICK_QUIET_MS) / (float)TICK_FADE_MS);
        if (level > 0) {
            size_t n = 0;
            if (const uint8_t *pcm = theme_audio::nextTick(n)) audio_play_pcm(pcm, n, false, level);
        }
    }

    // ONE wake per second, aimed AT the boundary rather than short of it.
    //
    // This used to stop 40 ms early and then poll every 5 ms, which is thirteen wakes a
    // second instead of one, every one of them on the same task that draws. On a stepping
    // dial that is invisible; on a sweeping one those wakes land in the middle of the hand's
    // own frames and Zion saw it stutter. An LVGL timer fires at or after its period, so
    // aiming at the boundary wakes on it or a few milliseconds after, which is what the
    // click wants anyway and costs a twelfth of the interruptions.
    const uint32_t toGo = (uint32_t)((1000000 - tv.tv_usec) / 1000);
    lv_timer_set_period(s_beat, toGo < 5 ? 5 : toGo);
}

static void tick_cb(lv_timer_t * /*t*/) {
    if (lv_scr_act() != s_screen) return;
    // ...and not while something is drawn over the top of it. The guard above catches
    // another APP being on screen, because a switch changes the active screen. It does not
    // catch a full-screen panel on the top layer, which leaves this the active screen while
    // hiding every pixel of it, and that is exactly what the wind screen is.
    //
    // Measured, not assumed: 655 ms of canvas work here, once a second, every second
    // somebody spent winding, with all of it discarded because a hidden object's
    // invalidation is dropped. It was the whole of the hitch.
    if (orb_screen_covered()) return;
    struct tm ti;
    time_for_face(&ti);

    // THEME_CAPS 55. Advancing a background frame changes every pixel beneath the hand, so
    // the cache of what is under it is now a picture of the wrong background. Dropping it is
    // the whole cost of this feature, and holding on frame nought is what keeps it rare.
    {
        const int want = bg_anim_frame();
        if (want != s_bgFrame) {
            s_bgFrame  = want;
            // INVALIDATE BOTH CACHES, BY THE KEY EACH ONE ACTUALLY USES.
            //
            // s_underMin alone was not enough and the comment that used to sit here
            // ("makes the sweep path rebuild below") was true when it was written and
            // stopped being true later. s_underMin is the staleness key for a RAILWAY
            // dial only — `ti.tm_min != s_underMin`. A dial that creeps, which is every
            // sweeping design that is not a railway clock, is tested against s_underMins,
            // a float holding minutes-with-fraction, and that was left alone. So on
            // Steam Punk the frame index advanced and the cache holding the old background
            // was never rebuilt: the picture only changed when the cache happened to be
            // rebuilt for its OWN reason, the three-second minute creep, which picked up
            // whatever frame was current by then.
            //
            // Greg, 2026-10-06: "no animation is playing, I occasionally will see a single
            // frame change on the background but that's it" — which is this exactly, and is
            // what a six-frame sequence looks like when five of the six invalidations are
            // dropped on the floor.
            //
            // s_noMin needs it too. Its own comment already says it is "rebuilt only when
            // something under the minute hand actually changes: the hour, the background
            // frame, or the theme" — the background frame was in the list and not in the
            // code, so a minute move would restore the previous frame's background.
            s_underMin   = -1;        // railway dials
            s_underMins  = -1.0f;     // ...and creeping ones, which is what was missing
            s_noMinValid = false;     // s_noMin holds the background as well
            s_fullNext   = true;      // and the frame after it repaints in full
        }
        // Tick fast enough for whichever of the two needs it more, never just the animation.
        //
        // This took the animation's rate and DROPPED the hand's, which is fine at six frames
        // a second and ruinous at one: a background clicking once a second, which is exactly
        // what a mechanical gear train wants to do, set the whole clock to one frame a second
        // and turned a sweeping hand into a ticking one. The two are not alternatives. The
        // animation advances on its own clock inside bg_anim_frame(), so asking for frames
        // more often than it needs costs it nothing and keeps the hand at its own rate.
        if (s_tick) {
            const theme_style::Clock::BgAnim &a2 = theme_style::clock().bgAnim;
            const bool running = a2.frames > 0 && (a2.loop || bg_anim_playing());
            const uint32_t base = sweep_possible() ? sweep_period() : 1000;
            const uint32_t need = (uint32_t)(1000 / (a2.fps < 1 ? 1 : a2.fps));
            const uint32_t want2 = running ? (need < base ? need : base) : base;
            if (want2 != s_tickPeriod) { s_tickPeriod = want2; lv_timer_set_period(s_tick, want2); }
        }
    }

    if (sweep_possible()) {
        // The cache holds the hour and minute hands, so it has to be recomposed before
        // either of them is visibly out of date. On a railway dial that is once a whole
        // minute and the hand steps, which is the design. Everywhere else it is roughly
        // every three seconds, which is how long the minute hand's tip takes to travel one
        // pixel, and the hand reads as creeping the way a mechanical watch does.
        //
        // Railway is a SEPARATE test, not a one-minute budget. Measuring elapsed time from
        // the last rebuild meant the step landed a whole minute after the previous one,
        // which is wherever the Orb happened to boot: start at 12:02:15 and it stepped at
        // :15 past every minute, never at the top. Jean-Paul Stringaro caught it the day
        // 2.16.38 shipped. The minute roll is an event, so test for the event.
        const theme_style::Clock &csR = theme_style::clock();
        const bool railwayDial = csR.secondRailway && csR.secondSweep;
        // Read the clock ONCE: the step window and the second hand have to agree about
        // which instant this frame is, or the hand can step against a different second
        // than the one it is drawn beside.
        const float wall = second_now();
        float aged = 0.0f;
        bool stale;
        if (railwayDial) {
            // Each frame of the step moves the hand, so each one needs the cache rebuilt.
            // When the step ends, one more rebuild puts the hand exactly on the new mark.
            const bool  canAnimate = step_affordable(s_composeMs);
#if defined(ESP_PLATFORM)
            if (!s_stepTold && s_composeMs > 0.0f) {
                s_stepTold = true;
                Serial.printf("[step] a compose costs %.0f ms on this design, so the minute "
                              "hand's step %s (needs %d frames in %.0f ms)\n",
                              s_composeMs,
                              canAnimate ? "is animated" : "clicks over in one move",
                              STEP_MIN_FRAMES, STEP_SECS * 1000.0f);
            }
#endif
            const float ease = canAnimate ? minute_step_ease(wall) : -1.0f;
            const bool  wasStepping = s_stepEase >= 0.0f;
            s_stepEase = ease;
            stale = ease >= 0.0f || wasStepping || ti.tm_min != s_underMin;
        } else {
            const float nowMins = (float)ti.tm_min + (float)ti.tm_sec / 60.0f;
            aged = nowMins - s_underMins;
            if (aged < 0.0f) aged += 60.0f;                // the hour rolled under us
            stale = s_underMins < 0.0f || aged >= cache_minutes_allowed();
        }
        (void)aged;
        if (!s_under || stale || ti.tm_hour != s_underHr || s_underFor != s_buf) {
            // The cheap road first: if the only thing that moved is the minute hand, move it
            // in place instead of recomposing the dial around it. Falls through to the full
            // rebuild on any doubt, which is what makes this safe to try.
            if (s_under && stale && ti.tm_hour == s_underHr) {
#if defined(ESP_PLATFORM)
                const uint32_t m0 = micros();
#endif
                const bool moved = refresh_minute(&ti, minute_angle_now(&ti));
#if defined(ESP_PLATFORM)
                if (moved) {
                    // Reported beside the full rebuild, because the whole question is which of
                    // the two the clock is actually doing and what each costs.
                    static uint32_t mLast = 0, mRuns = 0; static float mGap = 0, mTook = 0;
                    const uint32_t mNow = millis();
                    if (mLast) { mGap += (float)(mNow - mLast); mTook += (float)(micros() - m0) / 1000.0f; mRuns++; }
                    mLast = mNow;
                    if (mRuns >= 8) {
                        Serial.printf("[sweep] minute moved in place every %.1f s, %.0f ms each"
                                      " (hand stops %.0f%% of the time)\n",
                                      mGap / mRuns / 1000.0f, mTook / mRuns,
                                      100.0f * (mTook / mRuns) / (mGap / mRuns));
                        mRuns = 0; mGap = 0; mTook = 0;
                    }
                }
#endif
                if (moved) {
                sweep_pad_for_shadow();
                sweep_frame(railway_seconds(wall));
                return;
                }
            }
#if defined(ESP_PLATFORM)
            const uint32_t c0 = micros();
#endif
            if (!rebuild_under(&ti)) { redraw(&ti); return; }
#if defined(ESP_PLATFORM)
            // Same shape as the sweep's own meter below: the WORK, not the gap between
            // frames, so it cannot feed back into the period that schedules it.
            {
                const float took = (float)(micros() - c0) / 1000.0f;
                if (took < 2000.0f)
                    s_composeMs = (s_composeMs <= 0.0f) ? took : s_composeMs + 0.2f * (took - s_composeMs);

                // SAY IT OUT LOUD, every eighth rebuild.
                //
                // A recompose is the one thing on this screen that stops the sweep dead: the
                // whole dial is redrawn and the hand cannot move while it happens. How often
                // that is depends on the minute hand's reach, and how long it takes depends
                // on the design, so neither can be worked out from here. Zion is watching a
                // sweep stutter about every seven seconds and asked to be sure rather than
                // told; this is the Orb answering for itself.
                static uint32_t lastMs = 0, runs = 0; static float gapSum = 0, tookSum = 0;
                const uint32_t nowMs = millis();
                if (lastMs) { gapSum += (float)(nowMs - lastMs); tookSum += took; runs++; }
                lastMs = nowMs;
                if (runs >= 8) {
                    const float avg = tookSum / runs;
                    Serial.printf("[sweep] FULL recompose every %.1f s, %.0f ms each (hand stops %.0f%% of the time)"
                                  " -> plate %.0f  text %.0f  hands %.0f  glass %.0f\n",
                                  gapSum / runs / 1000.0f, avg, 100.0f * avg / (gapSum / runs),
                                  s_phPlate / runs, s_phText / runs, s_phHands / runs,
                                  avg - (s_phPlate + s_phText + s_phHands) / runs);
                    runs = 0; gapSum = 0; tookSum = 0;
                    s_phPlate = 0; s_phText = 0; s_phHands = 0; s_phRest = 0;
                }
            }
#endif
            // First frame after a rebuild repaints everything, because everything changed.
            // Same path as any other frame, just with the whole dial as its box.
            s_prevSec.x1 = 0; s_prevSec.y1 = 0;
            s_prevSec.x2 = SCREEN_W - 1; s_prevSec.y2 = SCREEN_H - 1;
            s_prevSecValid = true;
        }
        sweep_pad_for_shadow();
        // Snapped to the beat (see sweep_beat): the hand holds a position for a whole beat
        // and then moves, which is what an escapement does and what the eye reads as even.
        sweep_frame(railway_seconds(beat_quantize(wall)));
        // ...and the next frame is aimed AT the next beat, not merely scheduled soon. A step
        // that is regular to the millisecond reads as smooth at eight a second; the same step
        // arriving whenever the renderer happens to finish reads as a stutter, which is the
        // whole of what "a beat jerky" was.
        // THE HAND AND THE BACKGROUND ARE NOT ON THE SAME CLOCK, and they should not be.
        //
        // A watch's hand beats continuously while its wheels barely turn, and that is the
        // right shape here for a reason of cost rather than taste: a hand frame repaints
        // only the box the hand sweeps through, restored out of s_under, while a background
        // frame repaints all 217,156 pixels and reloads the plate. One is tens of
        // milliseconds, the other a couple of hundred. So the hand can and should be allowed
        // to move more often than the picture behind it.
        //
        // The tick therefore runs at whichever of the two wants a frame SOONER, and the
        // background advances only on the ticks where its own index has changed. Taking the
        // sooner rather than letting the last writer win is the same lesson as the re-aim on
        // the non-sweep path: this clobbered the bgAnim block's request for one build, and
        // got away with it only because the theme happened to ask for four of each.
        if (s_tick) {
            uint32_t aim = beat_aim();
            const theme_style::Clock::BgAnim &ba = theme_style::clock().bgAnim;
            if (ba.frames > 0 && (ba.loop || bg_anim_playing())) {
                const uint32_t need = 1000u / (uint32_t)(ba.fps < 1 ? 1 : ba.fps);
                if (need < aim) aim = need;
            }
            if (aim != s_tickPeriod) { s_tickPeriod = aim; lv_timer_set_period(s_tick, aim); }
        }
        return;
    }
    // The FACE still moves once a second even when the sound beats faster. A watch running at
    // four does not move its hand four times; the beat is what its sweep is made of, and the
    // hand it drives still steps once. Drawing four times a second would also cost four full
    // composes, which is the whole of this screen's budget spent on frames identical to each
    // other. Unchanged at beat 1, so no existing theme draws any differently.
    redraw(&ti);

    // AIM THE NEXT FRAME AT THE BEAT ITSELF.
    //
    // A clock that is not sweeping asked for a frame every 1000 ms, and 1000 ms from WHENEVER
    // the timer was last set, which is an arbitrary phase against the real second. So the hand
    // stepped at, say, .47 past every second, for as long as that theme was worn. Nobody
    // noticed while the hand was the only thing moving, because a second hand half a second
    // out still looks like a second hand. Giving the clock a voice made it obvious: Zion
    // heard the tick land about half a second away from where the hand moved.
    //
    // Aimed at the second, which is also when the tick fires.
    //
    // Re-aimed every frame rather than set once, because the device's clock and LVGL's timer
    // are not the same clock and will drift apart over hours.
    if (s_tick && !sweep_possible()) {
        struct timeval tv; gettimeofday(&tv, nullptr);
        uint32_t ms = (uint32_t)((1000000 - tv.tv_usec) / 1000);
        if (ms < 20) ms += 1000;        // too close to chase; take the one after
        // ...but never out-wait a background that is mid-play.
        //
        // This used to set the period unconditionally, and the bgAnim block at the top of
        // this same tick had already set it to 1000/fps a few hundred lines earlier. The
        // last writer won, so on EVERY non-sweeping dial the animation's rate was silently
        // replaced by "once a second", whatever the theme asked for. A theme asking for
        // eight frames a second got one, and since bg_anim_frame() derives the frame from
        // elapsed time rather than from how many frames have been drawn, it then jumped
        // eight places per draw: a six-frame loop sampled once a second looks like a
        // flicker, or like nothing at all.
        //
        // Steam Punk does not hit this any more because it sweeps, and sweeping dials take
        // the branch above. It cost a long evening to find on a dial that did not, so the
        // two requests are reconciled here rather than left to ordering.
        const theme_style::Clock::BgAnim &ba = theme_style::clock().bgAnim;
        if (ba.frames > 0 && (ba.loop || bg_anim_playing())) {
            const uint32_t need = 1000u / (uint32_t)(ba.fps < 1 ? 1 : ba.fps);
            if (need < ms) ms = need;   // the sooner of the two deadlines
        }
        lv_timer_set_period(s_tick, ms);
    }
}

// Redraw now, whatever the second says. For coming back from a screen that covered this one
// for a while: the canvas still holds the face as it was when the cover went up, so without
// this the clock shows the wrong time for up to a second after it reappears.
// Read-only windows for the self-test. Named for the questions they answer rather than for
// the fields behind them, so the test reads as the behaviour it protects.
bool  clockview::faceHasTime() { struct tm ti; time_for_face(&ti); return !s_noTime; }
float clockview::handSeconds(float wallSeconds) { return railway_seconds(wallSeconds); }
float clockview::cacheMinutesAllowed() { return cache_minutes_allowed(); }
// The movement the sweep is running, in beats a second. x3600 is the beats-per-hour a
// watchmaker would quote: 8 is 28,800, 4 is 14,400.
int   clockview::sweepBeat() { return sweep_beat(); }
float clockview::hourCacheMinutes() { return hour_cache_minutes(); }
float clockview::hourTipPixelsIn(float minutes) {
    const theme_style::Clock &cs = theme_style::clock();
    const theme_style::Hand &hd = cs.hand[0];
    CustomSprite spr = custom_hand(0);
    if (!hd.show || !spr.data) return 0.0f;
    const float up = (float)hd.pivotY, down = (float)(spr.h - hd.pivotY);
    const float reach = up > down ? up : down;
    return reach * (minutes * 0.5f) * DEG2RAD;      // half a degree a minute, at that reach
}
// How far one picture is from another, in the panel's own 565 levels, and where. Zero means
// identical. Both checks below come down to "are these the same".
static int worst_difference(const lv_color_t *a, const lv_color_t *b, long *count,
                            int *wx, int *wy) {
    const size_t n = (size_t)SCREEN_W * SCREEN_H;
    int worst = 0;
    if (count) *count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (a[i].full == b[i].full) continue;
        if (count) (*count)++;
        const int a0 = a[i].full, b0 = b[i].full;
        const int d[3] = { ((a0 >> 11) & 31) - ((b0 >> 11) & 31),
                           ((a0 >> 5)  & 63) - ((b0 >> 5)  & 63),
                           (a0 & 31) - (b0 & 31) };
        for (int c = 0; c < 3; ++c) {
            const int m = d[c] < 0 ? -d[c] : d[c];
            if (m > worst) { worst = m; if (wx) *wx = (int)(i % SCREEN_W); if (wy) *wy = (int)(i / SCREEN_W); }
        }
    }
    return worst;
}

// Sweep a whole revolution, remember the screen, sweep another, and compare. Same angle, same
// minute, so the two have to be identical. Anything else is a layer being applied to a pixel
// that already had it.
static long sweep_drift_once(int *wx, int *wy) {
    struct tm ti;
    time_for_face(&ti);
    if (!rebuild_under(&ti)) return -1;
    const size_t n = (size_t)SCREEN_W * SCREEN_H;
    lv_color_t *shot = (lv_color_t *)malloc(n * sizeof(lv_color_t));
    if (!shot) return -1;
    for (int rev = 0; rev < 2; ++rev) {
        for (int sec = 0; sec < 60; ++sec) { sweep_pad_for_shadow(); sweep_frame((float)sec); }
        sweep_pad_for_shadow();
        sweep_frame(0.0f);
        if (rev == 0) memcpy(shot, s_buf, n * sizeof(lv_color_t));
    }
    long count = 0;
    worst_difference(shot, s_buf, &count, wx, wy);
    free(shot);
    return count;
}

// A SWEPT FRAME AND A FULLY COMPOSED ONE, at the same instant, have to be the same picture.
//
// This is the question the drift check does not ask. That one proves the sweep does not
// DRIFT; it would pass happily while every swept frame was consistently wrong, and a band of
// dial consistently a shade off from the rest of it is exactly what somebody watching a clock
// reports as the dial changing brightness where the hand goes.
//
// moveMinute also runs the cheap road the minute hand takes every few seconds, which is where
// the fault actually was.
static long sweep_vs_full(bool moveMinute, int *wx, int *wy) {
    struct tm ti;
    time_for_face(&ti);
    const size_t n = (size_t)SCREEN_W * SCREEN_H;
    lv_color_t *full = (lv_color_t *)malloc(n * sizeof(lv_color_t));
    if (!full) return -1;

    // The whole dial, the way a ticking clock draws it. The same angle both ways, or this
    // measures the hand being somewhere else rather than the dial being the wrong colour.
    const float secs = railway_seconds((float)ti.tm_sec);
    clip_reset();
    compose_custom(&ti, -1, true);
    memcpy(full, s_buf, n * sizeof(lv_color_t));

    // The same instant, reached exactly the way the tick callback reaches it. The three lines
    // after the rebuild are not decoration: a rebuild leaves s_buf holding a compose with no
    // overlay on it, and it is the first sweep frame, with the whole screen as its box, that
    // puts the glass back over all of it. Leaving them out measured a dial with no glass
    // against one with glass and called 75% of the screen a fault.
    if (!rebuild_under(&ti)) { free(full); return -1; }
    s_prevSec.x1 = 0; s_prevSec.y1 = 0;
    s_prevSec.x2 = SCREEN_W - 1; s_prevSec.y2 = SCREEN_H - 1;
    s_prevSecValid = true;
    sweep_pad_for_shadow();
    sweep_frame(secs);

    if (moveMinute) {
        if (!refresh_minute(&ti, minute_angle_now(&ti))) { free(full); return -1; }
        sweep_pad_for_shadow();
        sweep_frame(secs);
    }

    long count = 0;
    const int worst = worst_difference(full, s_buf, &count, wx, wy);
    Serial.printf("[sweep] %s: %ld pixels differ from a full compose, worst %d levels\n",
                  moveMinute ? "after a minute move" : "a sweep frame", count, worst);
    free(full);
    return count;
}

// DOES THE LAYER CACHE DRAW THE SAME PICTURE? Answered in pixels, not by looking.
//
// A cache is only ever as good as its agreement with the thing it replaces, and "hands 0 ms"
// in the phase breakdown is just as consistent with "the blit is nearly free because the
// layers are mostly transparent" as it is with "the layers are empty and the hands are
// missing". Those need telling apart by something other than an opinion about a photograph.
//
// Composes the face twice at the same instant — once with the layers refused, once with them
// used — and reports how many of the 217,156 pixels differ and by how many 565 levels at
// worst. Zero is the only good answer. A handful of pixels differing by one level would be
// rounding in the source-over accumulation; thousands, or a large worst-case, means the
// overlap maths is wrong, and the hub is where two hands overlap.
long clockview::layerDiffersBy(int *worstOut, int *wx, int *wy) {
    if (worstOut) *worstOut = -1;
    if (wx) *wx = -1;
    if (wy) *wy = -1;
    if (!s_buf || !s_canvas || s_face != FACE_CUSTOM) return -1;
    struct tm ti;
    time_for_face(&ti);
    const size_t bytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
    lv_color_t *ref = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    lv_color_t *ref = (lv_color_t *)malloc(bytes);
#endif
    if (!ref) return -1;

    // The slow path, as the authority.
    clip_reset();
    s_layUse = false;
    compose_custom(&ti, -1, true);
    memcpy(ref, s_buf, bytes);

    // Then the same face through the layers.
    // Built OUTRIGHT at these exact angles, not merely accepted as near enough. The
    // tolerance that makes the cache worth having is up to 0.35 degrees of lag, about a
    // pixel and a half at the minute hand's tip, and comparing a tolerated layer against a
    // fresh compose measures that lag rather than whether the compositing is right. 3720
    // pixels differing by up to 24 levels is what that looked like, and it was the lag.
    const float minAng = minute_angle_now(&ti), hrAng = hour_angle_now(&ti);
    const bool used = layers_build(minAng, hrAng);
    if (used) { s_layMinAng = minAng; s_layHrAng = hrAng; s_layValid = true; s_layBuildY = -1; }
    clip_reset();
    s_layUse = used;
    compose_custom(&ti, -1, true);
    s_layUse = false;

    long count = 0;
    const int worst = worst_difference(ref, s_buf, &count, wx, wy);
    if (worstOut) *worstOut = worst;
#if defined(ESP_PLATFORM)
    Serial.printf("[layers] %s: %ld of %ld pixels differ from a full compose, worst %d levels\n",
                  used ? "in use" : "NOT USED (no memory, or the hands moved)",
                  count, (long)((size_t)SCREEN_W * SCREEN_H), worst);
    heap_caps_free(ref);
#else
    free(ref);
#endif
    if (!used) return -2;   // tell the caller the comparison was vacuous
    return count;
}

long clockview::sweepDiffersBy(int *wx, int *wy) {
    if (wx) *wx = -1;
    if (wy) *wy = -1;
    if (!s_buf || !s_canvas) return -1;

    // WITH THE SHADOW ON TOO, whatever this design asked for.
    //
    // The shadow is the layer that darkens, and the design the simulator happens to be
    // holding is not the one anybody reported from. So: run it as the design is, then again
    // with the shadow forced on, and answer for the worse of the two. s_clock is this
    // process's own copy of the style and is put back before returning.
    theme_style::Clock &style = const_cast<theme_style::Clock &>(theme_style::clock());
    const bool hadShadow = style.shadowOn;
    long worst = 0;
    for (int pass = 0; pass < 2; ++pass) {
        style.shadowOn = pass == 1 ? true : hadShadow;
        const long checks[3] = { sweep_drift_once(wx, wy),
                                 sweep_vs_full(false, wx, wy),
                                 sweep_vs_full(true,  wx, wy) };
        for (int i = 0; i < 3; ++i) {
            if (checks[i] < 0) { style.shadowOn = hadShadow; return -1; }
            if (checks[i] > worst) worst = checks[i];
        }
    }
    style.shadowOn = hadShadow;
    return worst;
}

long  clockview::litPixels() {
    if (!s_buf) return 0;
    long lit = 0;
    for (long i = 0; i < (long)SCREEN_W * SCREEN_H; ++i)
        if (s_buf[i].full) ++lit;
    return lit;
}
float clockview::minuteHandMins(bool railway, int min, int sec) { return minute_hand_mins(railway, min, sec); }
float clockview::minuteStepEase(float wallSecs) { return minute_step_ease(wallSecs); }
float clockview::minuteStepSecs() { return STEP_SECS; }
float clockview::railwayStopStart() { return STOP_AT; }
uint32_t clockview::beatAim(long usec) {
    const uint32_t toGo = (uint32_t)((1000000 - usec) / 1000);
    return toGo < 5 ? 5 : toGo;
}
long  clockview::beatSlot(long sec, long usec, int beat) {
    const int b = beat >= 2 ? beat : 1;
    return sec * b + (usec * b) / 1000000L;
}
bool  clockview::stepAffordable(float composeMs) { return step_affordable(composeMs); }

void clockview::setSweep(int mode) {
    s_forceSweep = mode;
    s_underMin = -1; s_underHr = -1; s_underMins = -1.0f; s_prevSecValid = false;
    retime();
#if defined(ESP_PLATFORM)
    const bool ok = sweep_possible();
    Serial.printf("[clock] sweep -> %s (possible: %s%s%s)\n",
                  mode < 0 ? "auto" : (mode ? "forced on" : "forced off"),
                  ok ? "yes" : "no", ok ? "" : " - ", ok ? "" : s_sweepWhyNot);
#endif
}

void clockview::refresh() {
    if (!s_screen) return;
    struct tm ti;
    time_for_face(&ti);
    redraw(&ti);
}

static void apply_face() {
    // The cache belongs to the old theme's dial. Dropping the minute stamp forces a rebuild
    // rather than sweeping a new second hand over somebody else's face.
    s_underMin = -1; s_underHr = -1; s_underMins = -1.0f; s_prevSecValid = false;
    // ...and what the last face COST. Nothing about how often this screen redraws is stored
    // with a design or carried between them: it is measured, here, from whatever is on the
    // glass now. A heavy dial must not leave a light one running at its pace.
    s_sweepMs = 45.0f; s_tickPeriod = 0;
    retime();
    // Hand sprites belong to the aviator face only; draw_aviator() re-shows them.
    if (s_face != FACE_AVIATOR) {
        if (s_hourImg)    lv_obj_add_flag(s_hourImg,    LV_OBJ_FLAG_HIDDEN);
        if (s_minImg)     lv_obj_add_flag(s_minImg,     LV_OBJ_FLAG_HIDDEN);
        if (s_hourShadow) lv_obj_add_flag(s_hourShadow, LV_OBJ_FLAG_HIDDEN);
        if (s_minShadow)  lv_obj_add_flag(s_minShadow,  LV_OBJ_FLAG_HIDDEN);
    }
    struct tm ti;
    time_for_face(&ti);
    redraw(&ti);
}

// The custom face's plate/overlay/hand sprites decode once into PSRAM and were
// never freed, so they sat resident even while some other app (radar, weather,
// intel) was on screen. Now the shell calls this on the way out, so that memory
// (up to ~1 MB for a photo-background design) goes back to whatever's shown next;
// custom_plate()/custom_overlay()/custom_hand() re-decode lazily the next time
// draw_custom() runs (see the timing it logs).
// Allocate the canvas here rather than in init(). See the header for the measurement that
// prompted it. Safe to call repeatedly: it only allocates what is missing.
void clockview::onEnter() {
    if (!s_screen) return;
    if (!s_buf) {
        const size_t bufBytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
        s_buf = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_buf) { Serial.println("[clock] PSRAM alloc for clock canvas failed"); return; }
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
        lv_obj_move_background(s_canvas);
        lv_canvas_fill_bg(s_canvas, COL_BLACK, LV_OPA_COVER);

        // This is a DIFFERENT canvas from the one the sweep caches were built against, and
        // nothing in them says so: s_under and s_noMin survive onExit, and their timestamps
        // still read as fresh. So the first tick back on a sweeping dial saw a cache that
        // was only seconds old, took the cheap road, and restored the second hand's rows
        // out of it into a canvas that had just been filled black. Every other pixel stayed
        // black, and the dial painted itself back in one hand-width at a time as the hand
        // swept past, never reaching the corners, which the hand cannot reach at all.
        //
        // Zion found it going into Settings to move the tick level and coming back out.
        // Stepping dials hid it, because a tick recomposes the whole face anyway.
        //
        // The canvas and the caches have to be dropped together, so drop them here, where
        // the new canvas is taken, and compose one full frame into it before anybody looks.
        s_underMin = -1; s_underHr = -1; s_underMins = -1.0f;
        s_noMinValid = false;
        s_prevSecValid = false;
        s_prevMinAng = -1000.0f;
        s_fullNext = true;
        struct tm ti;
        time_for_face(&ti);
        redraw(&ti);
    }
}

void clockview::onExit() {
    // 1.3 MB of hand layers, given back like every other screen's art. The Flight Tracker
    // wants ~1.4 MB of its own on the way in and this is the budget it comes out of.
    layers_free();
    custom_sprite_release();
    // The canvas and the rotation cache go too. The canvas object stays, pointing at
    // nothing until the next onEnter refills it: deleting and rebuilding an LVGL object
    // every switch is churn, and its z-order is re-asserted there anyway.
    // DELETE the object, do not hand it a null buffer.
    //
    // The first version called lv_canvas_set_buffer(s_canvas, nullptr, 1, 1, ...) to detach
    // it before freeing. LVGL does not accept that: it hung the UI thread on the very next
    // switch to another app, twice, while core 0 carried on logging happily, which is what
    // a wedged LVGL task looks like from the outside. Deleting the object costs one
    // allocation on the way back in and cannot be misread by the library.
    if (s_canvas) { lv_obj_del(s_canvas); s_canvas = nullptr; }
    if (s_buf)      { heap_caps_free(s_buf);      s_buf = nullptr; }
    if (s_rotCache) { heap_caps_free(s_rotCache); s_rotCache = nullptr; }
}

// ---- build ------------------------------------------------------------------
void clockview::init() {
    const bool office = app_theme::get() == APP_THEME_OFFICE;
    if (office) s_face = FACE_OFFICE;
    if (CUSTOM_CLOCK.active) s_face = FACE_CUSTOM;   // a pushed Launch Kit design wins over the theme default

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, office ? app_theme::palette().bg : COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    // The canvas is NOT allocated here any more; onEnter() takes it when the app is shown
    // and onExit() gives it back. This is the boot app, so it is taken moments later
    // regardless, and the difference is that it is released the moment you leave.
    (void)office;

    // Drop shadows: pre-blurred black silhouettes of the hand sprites (see
    // hand_hour_shadow_img.h / hand_min_shadow_img.h — soft gaussian-blurred alpha edges,
    // baked offline so there's no runtime blur cost), offset by a fixed screen-space vector
    // (HAND_SHADOW_DX/DY) instead of rotating with the hand — a real hand raised slightly
    // off the dial under one fixed light casts its shadow in the same direction no matter
    // what time it's showing. Created (and thus z-ordered) before the real hand sprites so
    // they always render underneath.
    s_hourShadow = lv_img_create(s_screen);
    lv_img_set_src(s_hourShadow, &HAND_HOUR_SHADOW_IMG);
    lv_img_set_pivot(s_hourShadow, HAND_HOUR_SHADOW_IMG_PIVOT_X, HAND_HOUR_SHADOW_IMG_PIVOT_Y);
    lv_img_set_antialias(s_hourShadow, true);
    lv_obj_set_pos(s_hourShadow, (lv_coord_t)lroundf(CX + HAND_SHADOW_DX) - HAND_HOUR_SHADOW_IMG_PIVOT_X,
                                 (lv_coord_t)lroundf(CY + HAND_SHADOW_DY) - HAND_HOUR_SHADOW_IMG_PIVOT_Y);
    lv_obj_add_flag(s_hourShadow, LV_OBJ_FLAG_HIDDEN);

    s_minShadow = lv_img_create(s_screen);
    lv_img_set_src(s_minShadow, &HAND_MIN_SHADOW_IMG);
    lv_img_set_pivot(s_minShadow, HAND_MIN_SHADOW_IMG_PIVOT_X, HAND_MIN_SHADOW_IMG_PIVOT_Y);
    lv_img_set_antialias(s_minShadow, true);
    lv_obj_set_pos(s_minShadow, (lv_coord_t)lroundf(CX + HAND_SHADOW_DX) - HAND_MIN_SHADOW_IMG_PIVOT_X,
                                (lv_coord_t)lroundf(CY + HAND_SHADOW_DY) - HAND_MIN_SHADOW_IMG_PIVOT_Y);
    lv_obj_add_flag(s_minShadow, LV_OBJ_FLAG_HIDDEN);

    // Aviator hand sprites (rotated each tick). Placed so each sprite's own pivot ring
    // sits at the dial centre: object top-left = centre - that sprite's pivot. Two
    // distinct assets (see hand_hour_img.h / hand_min_img.h), not one shape resized.
    // Antialias on for a smooth rotated edge.
    s_hourImg = lv_img_create(s_screen);
    lv_img_set_src(s_hourImg, &HAND_HOUR_IMG);
    lv_img_set_pivot(s_hourImg, HAND_HOUR_IMG_PIVOT_X, HAND_HOUR_IMG_PIVOT_Y);
    lv_img_set_antialias(s_hourImg, true);
    lv_obj_set_pos(s_hourImg, (lv_coord_t)lroundf(CX) - HAND_HOUR_IMG_PIVOT_X,
                              (lv_coord_t)lroundf(CY) - HAND_HOUR_IMG_PIVOT_Y);
    lv_obj_add_flag(s_hourImg, LV_OBJ_FLAG_HIDDEN);

    s_minImg = lv_img_create(s_screen);
    lv_img_set_src(s_minImg, &HAND_MIN_IMG);
    lv_img_set_pivot(s_minImg, HAND_MIN_IMG_PIVOT_X, HAND_MIN_IMG_PIVOT_Y);
    lv_img_set_antialias(s_minImg, true);
    lv_obj_set_pos(s_minImg, (lv_coord_t)lroundf(CX) - HAND_MIN_IMG_PIVOT_X,
                             (lv_coord_t)lroundf(CY) - HAND_MIN_IMG_PIVOT_Y);
    lv_obj_add_flag(s_minImg, LV_OBJ_FLAG_HIDDEN);

    // OFFICE minute hand + rim glow sprite is pre-decoded here (once) so the first Office
    // redraw doesn't pay the PNG-decode cost — see draw_office_minute_sprite().
    if (!office_minute_sprite()) Serial.println("[clock] office minute sprite decode failed");

    apply_face();
    s_tick = lv_timer_create(tick_cb, 1000, nullptr);
    // Guarded, because this builder runs again when a theme is applied and two beat timers
    // would play the set twice a second, half a beat apart, which would sound exactly like
    // the stutter it is here to remove.
    if (!s_beat) s_beat = lv_timer_create(beat_cb, 20, nullptr);
    retime();
}

lv_obj_t *clockview::screen() {
    return s_screen;
}
