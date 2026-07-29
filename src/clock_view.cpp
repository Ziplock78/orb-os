// Clock app for the shell. Three faces, cycled by pushing the knob:
//   IMPERIAL — blue Imperial Signal dial, silver dauphine hands, center seconds, date.
//   AVIATOR  — cream WWII aviator dial, dark hands, small seconds in the 6-o'clock sub-dial.
//   DIGITAL  — big hand-drawn 24-hour readout (seven-segment style) + the date.
//
// Time comes from the system clock (RTC-seeded, NTP-synced; see main.cpp). TZ is
// applied at boot, so getLocalTime() returns local time.
#include "clock_view.h"
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "dial_img.h"      // DIAL_IMG  — Imperial Signal (blue)
#include "dial_avi.h"      // DIAL_AVI  — Aviator (cream), AVI_SUB_X/Y sub-dial centre

// ---- palette ----------------------------------------------------------------
static const lv_color_t COL_HAND      = LV_COLOR_MAKE(0xE4, 0xE9, 0xF0);  // imperial silver
static const lv_color_t COL_HAND_EDGE = LV_COLOR_MAKE(0x0A, 0x16, 0x28);  // imperial hand outline
static const lv_color_t COL_DATE      = LV_COLOR_MAKE(0xF2, 0xF5, 0xF9);
static const lv_color_t COL_LUME      = LV_COLOR_MAKE(0xDA, 0xCF, 0xA6);  // aged cream lume fill
static const lv_color_t COL_LUME_EDGE = LV_COLOR_MAKE(0x38, 0x2E, 0x18);  // dark sepia outline
static const lv_color_t COL_BRASS     = LV_COLOR_MAKE(0x9C, 0x7B, 0x44);  // brass centre boss
static const lv_color_t COL_RED       = LV_COLOR_MAKE(0xB2, 0x3A, 0x2C);  // red seconds hand
static const lv_color_t COL_DATE_DARK = LV_COLOR_MAKE(0x2A, 0x24, 0x18);  // date text on cream
static const lv_color_t COL_DIGIT     = LV_COLOR_MAKE(0xE8, 0xEC, 0xF1);
static const lv_color_t COL_BLACK     = LV_COLOR_MAKE(0x00, 0x00, 0x00);

static constexpr float CX = SCREEN_CX;   // 233 (main dial centre)
static constexpr float CY = SCREEN_CY;   // 233
static constexpr float DEG2RAD = 3.14159265358979f / 180.0f;

static constexpr int DATE_WIN_X = 233;   // Imperial date-window centre
static constexpr int DATE_WIN_Y = 328;
static constexpr float AVI_DATE_R    = 184.0f;  // date banner arc radius from the centre
static constexpr float AVI_DATE_MID  = 180.0f;  // centred at 6 o'clock
static constexpr float AVI_DATE_STEP = 4.0f;    // degrees between characters

enum Face { FACE_AVIATOR, FACE_IMPERIAL, FACE_DIGITAL, FACE_COUNT };

static Face        s_face   = FACE_AVIATOR;   // WWII aviator is the default face
static lv_obj_t   *s_screen = nullptr;
static lv_obj_t   *s_canvas = nullptr;
static lv_color_t *s_buf    = nullptr;
static lv_obj_t   *s_dateLabel = nullptr;   // DIGITAL face only

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
                           float width, lv_color_t col) {
    const float a  = angDeg * DEG2RAD;
    const float dx = sinf(a), dy = -cosf(a);
    lv_point_t sp[2] = { P(pxc - tail*dx, pyc - tail*dy), P(pxc + len*dx, pyc + len*dy) };
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = (lv_coord_t)lroundf(width);
    ld.round_start = 1;
    ld.round_end   = 1;
    lv_canvas_draw_line(s_canvas, sp, 2, &ld);
}

// ---- IMPERIAL face ----------------------------------------------------------
static void draw_imperial(const struct tm *ti) {
    memcpy(s_buf, DIAL_IMG, sizeof(DIAL_IMG));

    char ds[4];
    snprintf(ds, sizeof(ds), "%d", ti->tm_mday);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.color = COL_DATE;
    ld.font  = &lv_font_montserrat_20;
    ld.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(s_canvas, DATE_WIN_X - 24, DATE_WIN_Y - 12, 48, &ld, ds);

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
// A flat black "syringe" aviator hand with a cream lume inlay (WWII flieger style).
static void draw_syringe_hand(float pxc, float pyc, float angDeg, float len, float tail,
                              float wb, float wl) {
    const float a  = angDeg * DEG2RAD;
    const float dx = sinf(a), dy = -cosf(a);
    const float qx = cosf(a), qy =  sinf(a);
    auto at = [&](float along, float across) -> lv_point_t {
        return P(pxc + along*dx + across*qx, pyc + along*dy + across*qy);
    };
    // black body: straight sides, pointed tip, flat base with a short tail
    lv_point_t body[5] = {
        at(len, 0), at(len*0.80f, wb), at(-tail, wb*0.85f),
        at(-tail, -wb*0.85f), at(len*0.80f, -wb),
    };
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = COL_LUME_EDGE;
    d.bg_opa   = LV_OPA_COVER;
    lv_canvas_draw_polygon(s_canvas, body, 5, &d);
    // cream lume inlay: narrower, stops short of the tip and the centre
    lv_point_t lume[5] = {
        at(len*0.74f, 0), at(len*0.64f, wl), at(len*0.06f, wl),
        at(len*0.06f, -wl), at(len*0.64f, -wl),
    };
    d.bg_color = COL_LUME;
    lv_canvas_draw_polygon(s_canvas, lume, 5, &d);
}

static void draw_aviator(const struct tm *ti) {
    memcpy(s_buf, DIAL_AVI, sizeof(DIAL_AVI));

    // date curved along the banner at the bottom ("Mon 27th")
    {
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

    // WWII syringe hour + minute hands
    draw_syringe_hand(CX, CY, hrs  * 30.0f, 118, 16, 9.0f, 4.5f);
    draw_syringe_hand(CX, CY, mins * 6.0f,  165, 20, 8.0f, 4.0f);

    // centre boss
    draw_disc(CX, CY, 8, COL_LUME_EDGE);
    draw_disc(CX, CY, 4, COL_BRASS);
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

static void draw_digital(const struct tm *ti) {
    lv_canvas_fill_bg(s_canvas, COL_BLACK, LV_OPA_COVER);

    const int digits[4] = { ti->tm_hour/10, ti->tm_hour%10, ti->tm_min/10, ti->tm_min%10 };
    const float w = 74, h = 150, t = 15, gap = 16, colonW = 24;
    const float totalW = 4 * w + 4 * gap + colonW;
    float x  = (SCREEN_W - totalW) * 0.5f;
    const float oy = (SCREEN_H - h) * 0.5f - 10;

    draw_digit(x, oy, w, h, t, SEG[digits[0]], COL_DIGIT); x += w + gap;
    draw_digit(x, oy, w, h, t, SEG[digits[1]], COL_DIGIT); x += w + gap;
    draw_disc(x + colonW*0.5f, oy + h*0.34f, t*0.5f, COL_DIGIT);
    draw_disc(x + colonW*0.5f, oy + h*0.66f, t*0.5f, COL_DIGIT);
    x += colonW + gap;
    draw_digit(x, oy, w, h, t, SEG[digits[2]], COL_DIGIT); x += w + gap;
    draw_digit(x, oy, w, h, t, SEG[digits[3]], COL_DIGIT);

    char d[24];
    strftime(d, sizeof(d), "%a  %d %b", ti);
    lv_label_set_text(s_dateLabel, d);
}

// ---- tick + face management -------------------------------------------------
static void redraw(const struct tm *ti) {
    if (!s_canvas || !s_buf) return;
    switch (s_face) {
        case FACE_IMPERIAL: draw_imperial(ti); break;
        case FACE_AVIATOR:  draw_aviator(ti);  break;
        default:            draw_digital(ti);  break;
    }
    lv_obj_invalidate(s_canvas);
}

static void tick_cb(lv_timer_t * /*t*/) {
    if (lv_scr_act() != s_screen) return;
    struct tm ti;
    if (getLocalTime(&ti, 0)) redraw(&ti);
}

static void apply_face() {
    if (s_face == FACE_DIGITAL) lv_obj_clear_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_dateLabel, LV_OBJ_FLAG_HIDDEN);
    struct tm ti;
    if (getLocalTime(&ti, 0)) redraw(&ti);
}

void clockview::onPress() {
    s_face = (Face)((s_face + 1) % FACE_COUNT);
    apply_face();
    static const char *names[] = { "aviator", "imperial", "digital" };
    Serial.printf("[clock] face -> %s\n", names[s_face]);
}

// ---- build ------------------------------------------------------------------
void clockview::init() {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    const size_t bufBytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    s_buf = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf) {
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
        lv_canvas_fill_bg(s_canvas, COL_BLACK, LV_OPA_COVER);
    } else {
        Serial.println("[clock] PSRAM alloc for clock canvas failed");
    }

    s_dateLabel = lv_label_create(s_screen);
    lv_label_set_text(s_dateLabel, "");
    lv_obj_set_style_text_color(s_dateLabel, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(s_dateLabel, &lv_font_montserrat_20, 0);
    lv_obj_align(s_dateLabel, LV_ALIGN_CENTER, 0, 116);

    apply_face();
    lv_timer_create(tick_cb, 1000, nullptr);
}

lv_obj_t *clockview::screen() {
    return s_screen;
}
