// Radar scope (M1) + aircraft (M2) + selection (M3) + selectable themes (M4).
// Pure LVGL, portable. Visual reference: assets/plane_radar_2.0_mockup.html
//   THEME_ORB   : Orb scope: green gradient, square grid, the 7 nearest
//                    aircraft as yellow balls (emitting waves) + off-range arrows.
#include "radar_view.h"
#include "app_theme.h"
#include "app_shell.h"       // knob capture: default view releases it, selection mode grabs it
#include "config.h"
#include "geo.h"
#include "coastline.h"
#include "roads_sd.h"
#include "airports.h"
#include "route.h"           // route_request()/route_get() — {from}/{to} tokens in a custom text banner
#include "custom_radar.h"    // CUSTOM_HAS_RADAR / CUSTOM_RTEXT{1,2,3}_* / CUSTOM_HAS_RADAR_STYLE / CUSTOM_SWEEP_*, CUSTOM_BLIP_*, CUSTOM_SEL_*, CUSTOM_OFFRANGE_*, CUSTOM_CENTER_* — a Launch Kit push's selection banners + visual styling
#include "radar_sprite.h"    // radar_custom_plate()/radar_custom_overlay()/radar_custom_blip_icon() — the editor's baked background+rings+crosshair / CRT+glass / aircraft-icon layers
#include "custom_radar_blip.h"   // CUSTOM_HAS_RADAR_BLIP_IMAGE / CUSTOM_RADAR_BLIP_PIVOT_X/Y
#include "custom_radar_sweep.h"  // CUSTOM_SWEEP_IMAGE_PIVOT_X/Y / CUSTOM_SWEEP_IMAGE_CENTER_X/Y — compile-time, coupled to whichever sweep sprite is baked in
#include "theme_style.h"
#include "theme_font.h"   // per-theme fonts, with the compiled font as fallback     // per-theme sweep/blip/selection/off-range/center/RTEXT values — see theme_style.h for what's covered vs. stays compile-time
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <deque>
#include <algorithm>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#else
// Desktop simulator: no ESP heap caps and no Serial. The flatten/etch code below is
// shared (the sim benefits from the same architecture), so shim the two device-isms
// rather than fork the logic. Matches the pattern location_view.cpp already uses.
#include <cstdarg>
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } void println(const char *s) const { puts(s); } } Serial;
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---- phosphor palette (mockup) ----
#define COL_GREEN  lv_color_hex(0x1DFF86)
#define COL_LEAD   lv_color_hex(0x3DFF9A)
#define COL_INK    lv_color_hex(0xEAFFF3)
#define COL_SOFT   lv_color_hex(0x9AFFC8)
#define COL_EMERG  lv_color_hex(0xFF5A3C)
// coastline outline — steel blue, deliberately off the red/amber/lime/green/cyan
// altitude-trail palette so land never reads as an aircraft track. Aviator theme
// swaps in a sepia/brass equivalent so it reads as an aged chart, not a scope.
#define COAST_COLOR lv_color_hex(0x4E86C6)
#define COAST_COLOR_AVI lv_color_hex(0x6B5638)
// roads (from the SD card, see roads_sd.cpp) — a muted neutral grey, distinct from
// both the coastline's blue and the airport markers' grey-blue so all three read
// as separate layers rather than blurring together.
#define ROAD_COLOR lv_color_hex(0x707868)
#define ROAD_COLOR_AVI lv_color_hex(0x8A7F63)
// airport markers — a neutral muted grey-blue so they sit quietly under the traffic.
#define AIRPORT_COLOR lv_color_hex(0x8A93A6)
#define AIRPORT_COLOR_AVI lv_color_hex(0x9C8F73)
// ---- aviator palette (WWII scope: brass rings, ivory sweep/ink, warm chrome) ----
#define AVI_RING lv_color_hex(0x6B5A3A)
#define AVI_LEAD lv_color_hex(0xDACFA6)
#define AVI_INK  lv_color_hex(0xEDE3CC)
#define AVI_SOFT lv_color_hex(0x9C8F73)
#define AVI_BG   lv_color_hex(0x14100A)
// ---- orb palette (Orb) ----
#define ORB_BLIP   lv_color_hex(0xFFE11A)
#define ORB_EMERG  lv_color_hex(0xFF4D2E)
#define ORB_ACCENT lv_color_hex(0xFF8A1E)
#define ORB_GRID   lv_color_hex(0x3F8B30)
#define ORB_BG_TOP lv_color_hex(0x18540F)
#define ORB_BG_BOT lv_color_hex(0x09250A)
#define ORB_FLOW   lv_color_hex(0xFFC24D)

// ---- sweep config ----
#define SWEEP_PERIOD_MS   8000
// Sweep redraw cadence. Every tick invalidates the sweep's rotated bounding box, and
// LVGL must then re-blend every layer intersecting it — with this theme that is seven
// layers, two of them full-screen with alpha. Measured on device: ~250 ms of compositing
// per frame, i.e. ~4 fps, while this timer was asking for a redraw every 30 ms. Asking
// eight times faster than the hardware can deliver does not make it faster, it just
// queues more invalidation work behind an already-late frame.
//
// Now that the sweep advances by REAL elapsed time (see sweep_timer_cb), a slower tick
// does not slow the rotation down — it just takes bigger angular steps per redraw. So
// this is chosen to be achievable rather than aspirational.
// Ask for frames at a rate the hardware can actually meet. This theme composites for
// ~166 ms per frame (measured: 6 fps, 88% of every second inside LVGL), and this timer was
// asking every 66 ms, two and a half times faster. The surplus requests do not produce
// surplus frames; they just land whenever the renderer gets to them, so the gaps between
// redraws are irregular. Since the sweep advances by real elapsed time, irregular gaps
// become irregular angular steps, which is the jitter Zion could see.
//
// Slower and regular beats faster and ragged here: a steady sweep is what makes this read
// as an instrument, and that was Zion's explicit priority over everything else on screen.
#define SWEEP_FRAME_MS    66
#define SWEEP_TRAIL_DEG   38.0f
#define SWEEP_TRAIL_STEPS 20
#define SWEEP_TRAIL_OPA   72

// ---- aircraft / flow / orb config ----
// How often aircraft glyphs are allowed to move. Deliberately coarse, and it is a product
// decision rather than a performance accident: a steady sweep is what makes this read as an
// instrument, while an aircraft's position being two seconds stale is invisible. Zion chose
// that trade explicitly.
//
// Each step invalidates one box per aircraft that moved, and with ~28 contacts on screen
// every one of those boxes forces LVGL to re-blend all the layers it touches. That was the
// variable cost per frame, and variable cost is exactly what the sweep cannot tolerate:
// because the sweep advances by real elapsed time, an unusually slow frame makes it take an
// unusually big angular jump. Correct speed, uneven motion. Measured before this change:
// 88% of every second inside LVGL, frame rate wandering 5-7 fps.
//
// Time-gated rather than counted in frames, so the cadence stays 2 s whatever the frame
// rate is doing. A frame counter would have made this drift with the very thing it is
// meant to stabilise.
#define AC_INTERP_MS      2000
#define TRAIL_MAX         7
#define TAP_RADIUS_PX     40    // generous finger-tap catch radius (picks the nearest glyph within it)
#define FLOW_MAX          240   // see setTrailLength: repaint cost is ~300 us per segment
#define FLOW_REDRAW_EVERY 80
#define FLOW_OPA          55
#define ORB_BLIPS      7
#define ORB_ARROWS     8
#define BALL_R            9
#define WAVE_EXPAND       28.0f

static int        s_theme    = THEME_AVIATOR;
static void      (*s_themeCb)(int) = nullptr;
// scope "chrome" palette (rings/sweep/crosshair/labels) — retinted per theme
static lv_color_t s_cRing = COL_GREEN, s_cLead = COL_LEAD, s_cInk = COL_INK, s_cSoft = COL_SOFT;
static const char *THEME_NAMES[THEME_COUNT] = { "ORB", "MILITARY", "AVIATOR" };
// First-entry loading notice. Opening the Flight Tracker from cold does real work
// before there is anything to show: the coastline, airports and roads are all projected
// for this location, the map is etched, and the first aircraft snapshot has to arrive.
// The sweep starts turning and then visibly stops for a few seconds, which reads as a
// crash rather than as loading. So say what is happening.
static lv_obj_t   *s_loading         = nullptr;
static bool        s_loadingPending  = false;   // shown, waiting for the first update()
static lv_obj_t   *s_themeLabel      = nullptr;   // "AVIATOR" etc. banner, shown briefly on a theme change
static lv_timer_t *s_themeLabelTimer = nullptr;   // one-shot: hides the banner after ~2s
static lv_obj_t  *s_parent   = nullptr;
static lv_obj_t  *s_gridLayer = nullptr;
static lv_obj_t  *s_sweep     = nullptr;
static lv_obj_t  *s_sweepImg  = nullptr;   // the sweep's "image" type — rotated live, replaces s_sweep's vector wedge when active
static lv_obj_t  *s_acLayer   = nullptr;
static lv_obj_t  *s_flowCanvas = nullptr;
static lv_color_t *s_flowBuf  = nullptr;
static lv_obj_t  *s_rose[4]   = {nullptr, nullptr, nullptr, nullptr};
static lv_obj_t  *s_centerDot = nullptr;
static lv_obj_t  *s_pulse     = nullptr;
static lv_obj_t  *s_rangeLbl  = nullptr;
static bool       s_rangeLblVisible = true;
static bool       s_sweepEnabled    = true;
static bool       s_airportsEnabled = true;
static int        s_maxOnScreen     = 20;          // how many (nearest) aircraft to draw (web-configurable)
static bool       s_bigText         = false;       // accessibility: bigger glyph labels (set before init)
static int        s_trailMax        = TRAIL_MAX;   // per-aircraft trail length (0 = off)
static int        s_flowMax         = FLOW_MAX;    // persistent flow-layer segments, count cap (0 = off)
static int        s_flowGenMax      = 14;          // ...and an age cap in polls (~2 s each) so tracks fade out
static lv_timer_t *s_timer    = nullptr;
static float       s_sweepDeg = 0.0f;
// Sweep pacing state, at file scope so the loading gate can reset it. A multi-second
// stall during first-entry projection would otherwise poison the smoothed frame time and
// make the sweep lurch on its first few steps after the wait.
static uint32_t    s_lastSweepMs = 0;
static float       s_emaDtMs     = 0.0f;
static float       s_prevSweepDeg = 0.0f;
static float       s_wavePhase = 0.0f;
static uint32_t    s_lastUpdateMs = 0;       // smooth-motion: cadence + animation clock
static uint32_t    s_animStartMs  = 0;
static uint32_t    s_pollMs       = POLL_INTERVAL_MS;
static int         s_frameCtr     = 0;
static lv_coord_t  s_cx = SCREEN_CX, s_cy = SCREEN_CY;
static std::string s_selHex;
// Flight Tracker knob state machine (shared by device + simulator so they can't
// drift). Two modes: DEFAULT VIEW — nothing selected, knob released, a turn opens
// the app switcher and a push enters selection. SELECTION MODE — an aircraft
// selected, knob captured, a turn cycles aircraft; a push, or SELECT_IDLE_MS of no
// input, drops back to the default view. See knobPress()/knobTurn()/knobEnter().
static bool        s_selectMode    = false;
static uint32_t    s_selActivityMs = 0;      // lv_tick_get() of the last knob input in selection mode
static constexpr uint32_t SELECT_IDLE_MS = 5000;
static void radar_exit_select();             // -> default view (deselect + release knob); defined below
static float       s_lastRangeKm = 0.0f;     // current scope range, for the range banner (radar_range_fmt)
static lv_obj_t   *s_textCanvas = nullptr;   // callsign/stats/route banners (curved+glow capable), a Launch Kit push
// The selection card: a plate under those banners, parked on the far side of the scope
// from whatever is selected. Two objects rather than one drawn shape, so LVGL does the
// rounded corners, the border and the compositing itself — the text canvas above stays
// purely text, and a glyph's glow lands over the card correctly instead of losing to it
// under the canvas's own "higher opacity wins" rule.
static lv_obj_t   *s_cardObj  = nullptr;     // the drawn (vector) card
static lv_obj_t   *s_cardImg  = nullptr;     // the image card
static lv_color_t *s_textBuf    = nullptr;
static lv_obj_t   *s_plateImg   = nullptr;   // baked background (bottom layer), a Launch Kit push
static lv_obj_t   *s_ringsImg   = nullptr;   // etched rings+crosshair, above the map, below the sweep (THEME_CAPS 6)
static lv_obj_t   *s_overlayImg = nullptr;   // baked CRT+glass (top layer), a Launch Kit push
static lv_obj_t   *s_staticImg[2] = { nullptr, nullptr };   // two plain decorative overlays, a Launch Kit push
static lv_obj_t   *s_dimLayer  = nullptr;   // plain full-scope color wash (the "Overlay" card) — reorderable, a Launch Kit push

struct FlowSeg { lv_point_t a, b; uint16_t gen; };   // gen = the poll it was laid down on
static std::deque<FlowSeg> s_flow;
static int s_flowRedrawCtr = 0;
static uint16_t s_flowGen = 0;        // ++ each update(); flow segments fade out after s_flowGenMax polls

struct AcDraw {
    lv_point_t pos;            // current (animated) screen position — what gets drawn
    lv_point_t from, to;       // smooth-motion glide endpoints (M4 interpolation)
    float      track;
    lv_color_t color;
    bool       emergency;
    bool       inRange;
    char       hex[8];
    char       call[12];
    char       type[8];
    char       altTxt[12];
    float      altFt;
    bool       onGround;
    float      vsFpm, gsKt, distKm, bearingDeg;
    int        squawk;
    std::vector<lv_point_t> trail;
};
static std::vector<AcDraw> s_acs;
// Hexes the scope is currently following. See the sticky-selection block in update() for
// why the set persists between polls instead of being recomputed from distance each time.
static std::set<std::string> s_tracked;
static std::map<std::string, std::vector<lv_point_t>> s_trails;

static const float GX[4] = { 0.0f,  7.0f, 0.0f, -7.0f };
static const float GY[4] = { -11.0f, 5.0f, 8.0f, 5.0f };

// Aviator theme only: a narrow kite for everyday traffic, a wide kite for recognized
// large/heavy types — same 4-point convex "kite" family as GX/GY above (just resized),
// not a literal notched silhouette. LVGL's software polygon fill ONLY supports convex
// polygons (see lv_draw_sw_polygon.c: "Only convex polygons are supported") — an
// earlier version of this used a notched (concave) shape and hard-locked the device,
// because a concave input can spin its scanline fill loop forever. Verify convexity
// (all four cross-products of consecutive edges same sign) before changing these.
static const float FIGHTER_X[4] = {  0.0f,  4.0f,  0.0f,  -4.0f };
static const float FIGHTER_Y[4] = { -9.0f,  3.0f,  5.0f,   3.0f };
static const float BOMBER_X[4]  = {  0.0f, 12.0f,  0.0f, -12.0f };
static const float BOMBER_Y[4]  = {-14.0f,  4.0f,  9.0f,   4.0f };

// Recognized large/heavy ICAO type-designator prefixes -> draw the bomber silhouette.
// Everything else (GA, regional, and anything the feed didn't identify) reads as a fighter.
static bool is_big_type(const char *t) {
    if (!t || !t[0]) return false;
    static const char *kBig[] = {
        "B7", "B4", "A3", "A2", "MD1", "MD9", "DC1", "DC9", "DC8",
        "IL9", "IL7", "C5", "C17", "KC1", "KC4", "E3", "E4", "P8", "B52", "B1", "B2"
    };
    for (const char *p : kBig) if (strncmp(t, p, strlen(p)) == 0) return true;
    return false;
}

// Office (see app_theme.h) is a whole-device light skin: it overrides the scope
// regardless of which of Orb/Military/Aviator is stored, the same way ui_apply_theme()
// overrides the HUD chrome. Orb's neon grid and Aviator's sepia dial don't have light
// variants designed for them, so both fold back to the plain ring/crosshair scope below.
static inline bool officeMode() { return app_theme::get() == APP_THEME_OFFICE; }
static inline bool orb() { return !officeMode() && s_theme == THEME_ORB; }
static inline bool aviator() { return !officeMode() && s_theme == THEME_AVIATOR; }
static inline lv_color_t coast_color()   { return aviator() ? COAST_COLOR_AVI   : COAST_COLOR; }
static inline lv_color_t airport_color() { return aviator() ? AIRPORT_COLOR_AVI : AIRPORT_COLOR; }
static inline lv_color_t road_color()    { return aviator() ? ROAD_COLOR_AVI    : ROAD_COLOR; }

// A Launch Kit push with full visual styling (background/rings/crosshair baked
// into a plate image, sweep/blip/selection/off-range/center as live parameters,
// CRT/glass baked into an overlay image) — replaces the built-in Orb/Military/
// Aviator scope look entirely, the same way a custom design already overrides
// the clock's faces. CUSTOM_HAS_RADAR_STYLE is always defined (0 in the
// committed stub) by custom_radar.h, so this is cheap and safe to call anywhere.
static inline bool customStyled() { return (bool)CUSTOM_HAS_RADAR_STYLE; }

static void show(lv_obj_t *o, bool v) {
    if (!o) return;
    if (v) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else   lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void hide_theme_label_cb(lv_timer_t * /*t*/) {
    show(s_themeLabel, false);
    s_themeLabelTimer = nullptr;   // the one-shot timer already deleted itself
}

// Flash the theme name at the top of the radar for ~2s, so cycling themes (knob
// push, touch long-press, a screen tap, or a fresh boot) always shows which one
// you're on. White text on a solid black plaque so it stays readable over any theme.
static void show_theme_label(const char *name) {
    if (!s_themeLabel) return;
    // Never on a custom design. This banner names the STOCK scope skin (Phosphor/Orb/
    // Aviator/...), which is meaningless once a Launch Kit theme is driving the screen —
    // it was appearing as a black "AVIATOR" pill floating over the Steam Punk dial,
    // because radar::init() ends with setTheme() and setTheme() flashes the name.
    if (customStyled()) return;
    lv_label_set_text(s_themeLabel, name);
    show(s_themeLabel, true);
    lv_obj_move_foreground(s_themeLabel);
    if (s_themeLabelTimer) { lv_timer_del(s_themeLabelTimer); s_themeLabelTimer = nullptr; }
    s_themeLabelTimer = lv_timer_create(hide_theme_label_cb, 2000, nullptr);
    lv_timer_set_repeat_count(s_themeLabelTimer, 1);
}

static lv_color_t alt_color(float altFt, bool onGround) {
    if (officeMode()) {                           // darker ramp than the neon default — legible on white
        if (onGround)      return lv_color_hex(0x8A8F98);
        if (altFt < 3000)  return lv_color_hex(0xE1341F);
        if (altFt < 10000) return lv_color_hex(0xE08A00);
        if (altFt < 20000) return lv_color_hex(0x8FA300);
        if (altFt < 30000) return lv_color_hex(0x1C8A4B);
        return lv_color_hex(0x1E6FE0);
    }
    if (aviator()) {                              // warm rust->ivory ramp, same low->high order
        if (onGround)      return lv_color_hex(0x8A7F6B);
        if (altFt < 3000)  return lv_color_hex(0xB0402C);
        if (altFt < 10000) return lv_color_hex(0xC97A2E);
        if (altFt < 20000) return lv_color_hex(0xC9A227);
        if (altFt < 30000) return lv_color_hex(0xE8DCC0);
        return lv_color_hex(0xF2ECDD);
    }
    if (onGround)      return lv_color_hex(0x888888);
    if (altFt < 3000)  return lv_color_hex(0xFF5A3C);
    if (altFt < 10000) return lv_color_hex(0xFFB23C);
    if (altFt < 20000) return lv_color_hex(0xC8FF3C);
    if (altFt < 30000) return lv_color_hex(0x39FF14);
    return lv_color_hex(0x3CE0FF);
}

static inline lv_point_t rim_point(float bearingDeg, float r) {
    const float a = bearingDeg * (float)M_PI / 180.0f;
    lv_point_t p;
    p.x = (lv_coord_t)lroundf((float)s_cx + r * sinf(a));
    p.y = (lv_coord_t)lroundf((float)s_cy - r * cosf(a));
    return p;
}

// rotate the local point (px,py) by `deg` (clockwise, screen coords) and offset to (ox,oy)
static inline lv_point_t rot_pt(float px, float py, float deg, lv_coord_t ox, lv_coord_t oy) {
    const float a = deg * (float)M_PI / 180.0f;
    const float c = cosf(a), s = sinf(a);
    lv_point_t p;
    p.x = (lv_coord_t)(ox + (lv_coord_t)lroundf(px * c - py * s));
    p.y = (lv_coord_t)(oy + (lv_coord_t)lroundf(px * s + py * c));
    return p;
}

// =============================== flow map ====================================
// ---- lazy full-screen canvases ---------------------------------------------
// The flow (aircraft-trail) canvas and the selection-banner text canvas are each a
// 636 KB PSRAM buffer AND a full-screen alpha layer that every sweep frame must blend
// through. Both were allocated at boot and held forever, which (a) fragmented PSRAM —
// largest free block measured at 423 KB while Flight Tracker was open, below the 651 KB
// the menu's own canvas needs, which is exactly the black-menu-with-white-text failure —
// and (b) taxed every frame for features that are usually inactive: trails are a
// settings toggle, and the banners only exist while an aircraft is selected.
//
// Lifecycle now matches everything else on this device: acquire when there is something
// to show, release when there is not. While unbuffered, the canvas object stays HIDDEN,
// so LVGL also skips it entirely during composition.
static bool canvas_acquire(lv_obj_t *canvas, lv_color_t *&buf, const char *tag) {
    if (!canvas) return false;
    if (buf) return true;
    const size_t sz = LV_CANVAS_BUF_SIZE_TRUE_COLOR_ALPHA(SCREEN_W, SCREEN_H);
#if defined(ESP_PLATFORM)
    buf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
#else
    buf = (lv_color_t *)malloc(sz);
#endif
    if (!buf) {
        printf("[radar] %s canvas alloc FAILED (%u bytes) — feature skipped this session\n",
               tag, (unsigned)sz);
        return false;
    }
    lv_canvas_set_buffer(canvas, buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static void canvas_release(lv_obj_t *canvas, lv_color_t *&buf) {
    if (!buf) return;
    if (canvas) {
        lv_img_set_src(canvas, (const void *)NULL);   // detach before freeing: LVGL must never repaint from a freed buffer
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    }
#if defined(ESP_PLATFORM)
    heap_caps_free(buf);
#else
    free(buf);
#endif
    buf = nullptr;
}

static void flow_draw_seg(const FlowSeg &s) {
    if (!s_flowCanvas || !s_flowBuf) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = orb() ? ORB_FLOW : s_cRing;
    d.width = 2;
    d.opa = FLOW_OPA;
    lv_point_t pts[2] = { s.a, s.b };
    lv_canvas_draw_line(s_flowCanvas, pts, 2, &d);
}

static void flow_redraw_all(void) {
    if (!s_flowCanvas) return;
    if (s_flow.empty()) { canvas_release(s_flowCanvas, s_flowBuf); return; }
    if (!canvas_acquire(s_flowCanvas, s_flowBuf, "flow")) return;
    lv_canvas_fill_bg(s_flowCanvas, lv_color_black(), LV_OPA_TRANSP);
    for (const FlowSeg &s : s_flow) flow_draw_seg(s);
}

// =============================== grid ========================================
static void grid_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    const lv_point_t c = { s_cx, s_cy };

    // A pushed design bakes its own rings/crosshair (and Orb's grid/"you are
    // here" triangle don't apply to a custom look at all) into the plate image
    // set up in init()/refreshCustomStyle() — this layer only still owes the
    // coastline/airport markers, which are the device's own native OSM data,
    // not something a design push carries.
    if (customStyled()) {
        // The theme's map, not the firmware's. Roads used to be drawn here in one fixed
        // grey whatever the design was doing, which a dark or a sepia dial had no way to
        // argue with. Colour, strength and whether they draw at all now travel with the
        // theme; the defaults in theme_style.h are the exact constants used before, so a
        // theme that says nothing about the map is unchanged.
        const theme_style::Radar &rs = theme_style::radar();
        if (rs.mapRoadsOn) roads_sd::draw(d, lv_color_hex(rs.mapRoadColor), (lv_opa_t)rs.mapRoadOpacity, 1);
        // Coastline/waterways deliberately not drawn under a custom design. Inland it is
        // canals and washes rather than a recognisable shoreline, and on a 466 px dial it
        // read as clutter competing with the roads. The data still ships and the stock
        // scopes below still draw it; only the themed path opts out.
        //
        // Airports still answer to the device's own setting as well: it is a preference
        // about what the owner wants to see, not only about how a theme looks.
        if (s_airportsEnabled && rs.mapAirportsOn) airports_draw(d, lv_color_hex(rs.mapAirportColor), 150);
        return;
    }

    if (orb()) {
        lv_draw_line_dsc_t gl;
        lv_draw_line_dsc_init(&gl);
        gl.color = ORB_GRID;
        gl.width = 1;
        gl.opa = 120;
        const int step = 38;
        for (int x = s_cx % step; x < SCREEN_W; x += step) {
            lv_point_t p1 = { (lv_coord_t)x, 0 }, p2 = { (lv_coord_t)x, SCREEN_H - 1 };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        for (int y = s_cy % step; y < SCREEN_H; y += step) {
            lv_point_t p1 = { 0, (lv_coord_t)y }, p2 = { SCREEN_W - 1, (lv_coord_t)y };
            lv_draw_line(d, &gl, &p1, &p2);
        }
        // center "you are here" triangle (orange, pointing up)
        lv_point_t tri[3] = { rot_pt(0, -11, 0, s_cx, s_cy),
                              rot_pt(10, 8, 0, s_cx, s_cy),
                              rot_pt(-10, 8, 0, s_cx, s_cy) };
        lv_draw_rect_dsc_t td;
        lv_draw_rect_dsc_init(&td);
        td.bg_color = ORB_ACCENT;
        td.bg_opa = LV_OPA_COVER;
        td.border_color = lv_color_hex(0x8A4A00);
        td.border_width = 1;
        td.border_opa = 160;
        roads_sd::draw(d, road_color(), 130, 1);
        coastline_draw(d, coast_color(), 170, 2);    // landmass outline under the triangle
        if (s_airportsEnabled) airports_draw(d, airport_color(), 150);
        lv_draw_polygon(d, &td, tri, 3);
        return;
    }

    // roads + coastline first, so the rings/crosshair sit cleanly on top.
    // Steel blue (sepia in Aviator) + 2 px so the coastline reads as a map
    // outline, distinct from the altitude-trail palette; roads are a thinner,
    // more muted neutral so they don't compete with it.
    roads_sd::draw(d, road_color(), 150, 1);
    coastline_draw(d, coast_color(), 165, 2);
    if (s_airportsEnabled) airports_draw(d, airport_color(), 150);

    // phosphor: concentric rings + crosshair
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.color = s_cRing;
    ad.width = 2;
    const lv_coord_t rr[4] = { 50, 104, 160, RADAR_R_OUTER_PX };
    const lv_opa_t   ro[4] = { 66, 66, 66, 87 };
    for (int i = 0; i < 4; ++i) { ad.opa = ro[i]; lv_draw_arc(d, &ad, &c, rr[i], 0, 360); }

    lv_draw_line_dsc_t ll;
    lv_draw_line_dsc_init(&ll);
    ll.color = s_cRing;
    ll.width = 2;
    ll.opa = 41;
    lv_point_t h1 = { (lv_coord_t)(s_cx - 211), s_cy }, h2 = { (lv_coord_t)(s_cx + 211), s_cy };
    lv_point_t v1 = { s_cx, (lv_coord_t)(s_cy - 211) }, v2 = { s_cx, (lv_coord_t)(s_cy + 211) };
    lv_draw_line(d, &ll, &h1, &h2);
    lv_draw_line(d, &ll, &v1, &v2);
}

// =============================== sweep =======================================
// A custom design's sweep is a live parameter set (color/leadColor/trailDeg/
// opacity/length), not baked into the plate — it animates, so it has to stay a
// real draw callback either way. Orb's plain grid has no sweep by default, but
// a pushed design's own sweep.enabled should still apply regardless of theme.
static inline float sweepLenPx()    { return customStyled() ? (float)theme_style::radar().sweepLength  : (float)RADAR_R_OUTER_PX; }
static inline float sweepTrailDeg() { return customStyled() ? (float)theme_style::radar().sweepTrailDeg : SWEEP_TRAIL_DEG; }

static void sweep_draw_cb(lv_event_t *e) {
    if (s_loadingPending) return;   // no hand until there is something to sweep over
    if (!customStyled() && orb()) return;
    if (customStyled() && !theme_style::radar().sweepEnabled) return;
    // Image-type sweep is a separate rotating lv_img object (s_sweepImg, see
    // init()/refreshCustomStyle()/sweep_timer_cb) — this vector wedge stays
    // hidden/skipped whenever that's the active look.
    if (customStyled() && theme_style::radar().sweepTypeImage) return;
    lv_draw_ctx_t *dctx = lv_event_get_draw_ctx(e);
    const lv_point_t center = { s_cx, s_cy };
    const float R = sweepLenPx();
    const float trailDeg = sweepTrailDeg();
    const lv_color_t trailColor = customStyled() ? lv_color_hex(theme_style::radar().sweepColor) : s_cRing;
    const lv_color_t leadColor  = customStyled() ? lv_color_hex(theme_style::radar().sweepLeadColor) : s_cLead;
    const float trailOpaMax = customStyled() ? ((float)theme_style::radar().sweepOpacity * 2.55f) : (float)SWEEP_TRAIL_OPA;

    // The trail's line work. Clamped rather than trusted: a theme is a file on an SD card
    // and a zero step count here would divide by zero two lines down.
    const int steps = customStyled()
        ? (theme_style::radar().sweepTrailSteps < 1 ? 1 : (theme_style::radar().sweepTrailSteps > 60 ? 60 : theme_style::radar().sweepTrailSteps))
        : SWEEP_TRAIL_STEPS;
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = trailColor;
    ld.width = customStyled() ? (lv_coord_t)theme_style::radar().sweepTrailWidth : 5;
    ld.round_start = 1;
    ld.round_end = 1;
    for (int i = steps; i >= 1; --i) {
        const float frac = 1.0f - (float)i / (float)steps;
        const float ang  = s_sweepDeg - (float)i * (trailDeg / (float)steps);
        ld.opa = (lv_opa_t)(frac * frac * trailOpaMax);
        if (ld.opa < 2) continue;
        lv_point_t p2 = rim_point(ang, R);
        lv_draw_line(dctx, &ld, &center, &p2);
    }
    lv_draw_line_dsc_t le;
    lv_draw_line_dsc_init(&le);
    le.color = leadColor;
    le.width = customStyled() ? (lv_coord_t)theme_style::radar().sweepLeadWidth : 2;
    le.opa = 217;
    le.round_start = 1;
    le.round_end = 1;
    lv_point_t lead = rim_point(s_sweepDeg, R);
    lv_draw_line(dctx, &le, &center, &lead);
}

static void wedge_bbox(float deg, lv_area_t *out) {
    const float trailDeg = sweepTrailDeg();
    const float R = sweepLenPx();
    lv_coord_t minx = s_cx, maxx = s_cx, miny = s_cy, maxy = s_cy;
    const int steps = 10;
    for (int i = 0; i <= steps; ++i) {
        const float a = deg - trailDeg * (float)i / (float)steps;
        const lv_point_t p = rim_point(a, R);
        if (p.x < minx) minx = p.x;
        if (p.x > maxx) maxx = p.x;
        if (p.y < miny) miny = p.y;
        if (p.y > maxy) maxy = p.y;
    }
    const lv_coord_t pad = 6;
    out->x1 = minx - pad; out->y1 = miny - pad;
    out->x2 = maxx + pad; out->y2 = maxy + pad;
}

// How far one aircraft's mark can reach from its own position, in px.
//
// blipSize describes the VECTOR shapes only. An image blip is drawn at the sprite's own
// pixel size, about its pivot, so its reach is the distance from that pivot to the far
// corner: a 100 px icon padded as if it were a 10 px dot leaves the invalidation area
// short, and the glide smears the parts that fall outside it. Rotation is why the corner
// matters rather than the edge, and an off-centre pivot is why both sides are measured.
static inline int blip_reach(const theme_style::Radar &rs) {
    if (rs.blipTypeImage) {
        if (const lv_img_dsc_t *icon = radar_custom_blip_icon()) {
            const int w = icon->header.w, h = icon->header.h;
            const int bpx = rs.blipPivotX >= 0 ? rs.blipPivotX : CUSTOM_RADAR_BLIP_PIVOT_X;
            const int bpy = rs.blipPivotY >= 0 ? rs.blipPivotY : CUSTOM_RADAR_BLIP_PIVOT_Y;
            const float dx = (float)LV_MAX(bpx, w - bpx);
            const float dy = (float)LV_MAX(bpy, h - bpy);
            return (int)lroundf(sqrtf(dx * dx + dy * dy));
        }
    }
    return rs.blipSize;
}

// glyph + label bounding box (for partial invalidation during the glide).
// Must cover the label areas drawn in the aircraft layer (they grew for large-text mode).
static inline lv_area_t glyph_bbox(lv_point_t p) {
    lv_area_t a;
    if (customStyled()) {
        // No floating call/alt labels in custom style (those are the separate
        // selection-banner system) — just cover the blip + its glow + the
        // selection ring + its glow, generously, so the glide never trails ghosts.
        const theme_style::Radar &rs = theme_style::radar();
        const int pad = 16 + blip_reach(rs) + rs.blipGlow + rs.selDiameter / 2 + rs.selGlow;
        a.x1 = p.x - pad; a.y1 = p.y - pad; a.x2 = p.x + pad; a.y2 = p.y + pad;
    } else if (orb()) { a.x1 = p.x - 30; a.y1 = p.y - 30; a.x2 = p.x + 30;  a.y2 = p.y + 30; }
    else          { a.x1 = p.x - 22; a.y1 = p.y - 22; a.x2 = p.x + 174; a.y2 = p.y + 32; }
    return a;
}
static inline void area_union(lv_area_t &d, const lv_area_t &s) {
    d.x1 = LV_MIN(d.x1, s.x1); d.y1 = LV_MIN(d.y1, s.y1);
    d.x2 = LV_MAX(d.x2, s.x2); d.y2 = LV_MAX(d.y2, s.y2);
}

// Advance each glyph from its previous position toward the new target (ease-out),
// invalidating only the small region each one occupies. Self-limiting: when a plane
// barely moves (far away / slow), nx==pos and it's skipped — near-zero cost.
static void interp_step(void) {
#if MOTION_INTERP
    if (!s_acLayer || s_acs.empty()) return;
    const uint32_t now = lv_tick_get();
    float t = s_pollMs ? (float)(now - s_animStartMs) / (float)s_pollMs : 1.0f;
    if (t > 1.0f) t = 1.0f;
    const float e = t * (2.0f - t);                  // ease-out quad
    for (AcDraw &ac : s_acs) {
        const lv_coord_t nx = ac.from.x + (lv_coord_t)lroundf((float)(ac.to.x - ac.from.x) * e);
        const lv_coord_t ny = ac.from.y + (lv_coord_t)lroundf((float)(ac.to.y - ac.from.y) * e);
        if (nx == ac.pos.x && ny == ac.pos.y) continue;
        lv_point_t np; np.x = nx; np.y = ny;
        lv_area_t inv = glyph_bbox(ac.pos);
        area_union(inv, glyph_bbox(np));
        ac.pos = np;
        lv_obj_invalidate_area(s_acLayer, &inv);
    }
#endif
}

static void sweep_timer_cb(lv_timer_t *t) {
    (void)t;
    // Selection mode auto-times-out: 5s with no knob input drops back to the
    // populated default view (deselect + release the knob) so the scope doesn't
    // stay pinned on one aircraft. Runs before the early returns below.
    if (s_selectMode && (uint32_t)(lv_tick_get() - s_selActivityMs) >= SELECT_IDLE_MS) radar_exit_select();
    {   // aircraft glyph motion, throttled: see AC_INTERP_MS for why this is slow on purpose
        static uint32_t s_lastInterpMs = 0;
        const uint32_t nowIms = lv_tick_get();
        if (!s_lastInterpMs || (uint32_t)(nowIms - s_lastInterpMs) >= AC_INTERP_MS) {
            s_lastInterpMs = nowIms;
            interp_step();
        }
    }
    if (!customStyled() && orb()) {
        // animate the blip waves (invalidate only the ball areas)
        s_wavePhase += 0.05f;
        if (s_wavePhase >= 1.0f) s_wavePhase -= 1.0f;
        if (!s_acLayer) return;
        int balls = 0;
        for (const AcDraw &ac : s_acs) {
            if (!ac.inRange) continue;
            if (balls >= ORB_BLIPS) break;
            balls++;
            lv_area_t a = { (lv_coord_t)(ac.pos.x - 44), (lv_coord_t)(ac.pos.y - 44),
                            (lv_coord_t)(ac.pos.x + 44), (lv_coord_t)(ac.pos.y + 44) };
            lv_obj_invalidate_area(s_acLayer, &a);
        }
        return;
    }
    // sweep disabled (live toggle, or the pushed design's own sweep.enabled): glyph interpolation above still runs
    // (this used to read the compile-time CUSTOM_SWEEP_ENABLED/CUSTOM_SWEEP_SPEED
    // macros directly — a leftover from before theme_style existed, so a theme
    // switch could leave the sweep animating at a stale/wrong-theme speed even
    // though sweep_draw_cb's own coloring/trail already followed theme_style)
    if (!s_sweepEnabled || (customStyled() && !theme_style::radar().sweepEnabled)) return;
    // Held still until the scope has data. Re-seeding the pacing state here means the
    // first step after the wait is measured from the first real frame, not from the
    // seconds-long projection stall that preceded it.
    if (s_loadingPending) { s_lastSweepMs = 0; s_emaDtMs = 0.0f; return; }
    s_prevSweepDeg = s_sweepDeg;
    const float speedDps = customStyled() ? (float)theme_style::radar().sweepSpeed : (360.0f * 1000.0f / (float)SWEEP_PERIOD_MS);
    // Advance by REAL elapsed time, not by an assumed SWEEP_FRAME_MS per tick. LVGL
    // timers fire when lv_timer_handler() reaches them, so on a loaded frame they run
    // late; stepping a fixed amount each time made the sweep rotate at whatever fraction
    // of real time the render loop was achieving. Measured 5 fps against a 33 fps target
    // on Flight Tracker, which is exactly the "sweep is slower than Launch Kit shows"
    // Zion spotted. Elapsed-time stepping makes the rotation correct at any frame rate
    // (it just gets chunkier as frames drop, which is honest rather than wrong).
    const uint32_t nowMs = lv_tick_get();
    uint32_t dtMs = s_lastSweepMs ? (uint32_t)(nowMs - s_lastSweepMs) : (uint32_t)SWEEP_FRAME_MS;
    s_lastSweepMs = nowMs;
    if (dtMs > 500) dtMs = SWEEP_FRAME_MS;   // returning from a stall shouldn't teleport the sweep
    // Advance by a SMOOTHED frame time, not the raw one. Raw elapsed-time stepping keeps
    // the rotation speed exactly right, but when frame times wobble (66-100 ms on this
    // hardware) the angular step wobbles with them, +-40%, and that variance IS the
    // stutter the eye picks up. Zion's stated priority is explicit: perfectly even
    // motion beats exactly correct speed. An EMA drifts the speed by a few percent
    // while it adapts, which nobody can see; uneven steps are what everybody sees.
    if (s_emaDtMs <= 0.0f) s_emaDtMs = (float)dtMs;
    s_emaDtMs += 0.08f * ((float)dtMs - s_emaDtMs);
    if (s_emaDtMs < 20.0f) s_emaDtMs = 20.0f;
    if (s_emaDtMs > 400.0f) s_emaDtMs = 400.0f;
    s_sweepDeg += speedDps * s_emaDtMs / 1000.0f;
    if (s_sweepDeg >= 360.0f) s_sweepDeg -= 360.0f;
    // Image-type sweep: same angle, rotated as a real lv_img instead of the
    // vector wedge's manual bounding-box invalidation below.
    if (customStyled() && theme_style::radar().sweepTypeImage) {
        if (s_sweepImg) lv_img_set_angle(s_sweepImg, (int16_t)lroundf(s_sweepDeg * 10.0f));
        return;
    }
    if (!s_sweep) return;
    lv_area_t a, b, area;
    wedge_bbox(s_prevSweepDeg, &a);
    wedge_bbox(s_sweepDeg, &b);
    area.x1 = LV_MIN(a.x1, b.x1);
    area.y1 = LV_MIN(a.y1, b.y1);
    area.x2 = LV_MAX(a.x2, b.x2);
    area.y2 = LV_MAX(a.y2, b.y2);
    lv_obj_invalidate_area(s_sweep, &area);
}

// =============================== aircraft ====================================
// Is this point inside a theme's exclusion zone? See theme_style::Radar::zones for why
// these exist: they let decoration sit in the free baked background instead of in an
// expensive layer above the aircraft. Cheap enough to call per point per frame — at most
// six squared-distance comparisons, no square roots.
static inline bool in_excluded_zone(lv_coord_t x, lv_coord_t y) {
    if (!customStyled()) return false;
    const theme_style::Radar &rs = theme_style::radar();
    for (int i = 0; i < rs.zoneCount; ++i) {
        const theme_style::Radar::Zone &z = rs.zones[i];
        bool inside;
        if (z.rect) {
            const long dx = (long)x - z.x, dy = (long)y - z.y;
            inside = (dx >= -(z.w / 2) && dx <= z.w / 2 && dy >= -(z.h / 2) && dy <= z.h / 2);
        } else {
            const long dx = (long)x - z.x, dy = (long)y - z.y;
            inside = (dx * dx + dy * dy <= (long)z.r * z.r);
        }
        // Inverted zones hide everything OUTSIDE the shape, which is how one big circle
        // becomes a containment ring keeping aircraft off the dial's border.
        if (inside != z.invert) return true;
    }
    return false;
}
static inline bool ac_masked(const AcDraw &ac) { return in_excluded_zone(ac.pos.x, ac.pos.y); }

// The SWEEP IS NEVER MASKED, by decision (Zion, 2026-08-18), and this is not an oversight
// to be tidied up later. It is the one moving part of the instrument, and a hand that
// blinks out over a piece of artwork reads as a fault rather than as a design. Where a
// sweep needs to stop short of a border, sweepLength already does that honestly, by making
// the hand shorter rather than by hiding part of it.
//
// Masked, by contrast: aircraft and their off-range arrows, their trails and flow tracks,
// the rings and crosshair (baked into the plate by the editors), and the etched map.

static void draw_trail(lv_draw_ctx_t *d, const AcDraw &ac, lv_color_t col) {
    const int n = (int)ac.trail.size();
    if (n < 2) return;
    lv_draw_line_dsc_t t;
    lv_draw_line_dsc_init(&t);
    t.color = col;
    t.width = 2;
    for (int i = 1; i < n; ++i) {
        t.opa = (lv_opa_t)(10 + 45 * i / n);
        lv_point_t a = ac.trail[i - 1], b = ac.trail[i];
        // Hiding the aircraft but still drawing its track across the decoration would
        // defeat the point, so a segment is dropped if either end sits in a zone.
        if (in_excluded_zone(a.x, a.y) || in_excluded_zone(b.x, b.y)) continue;
        lv_draw_line(d, &t, &a, &b);
    }
}

static void draw_ball(lv_draw_ctx_t *d, const AcDraw &ac) {
    // emitted waves: several expanding rings (sonar-ping look)
    lv_draw_arc_dsc_t w;
    lv_draw_arc_dsc_init(&w);
    w.color = ORB_ACCENT;
    w.width = 3;
    for (int wv = 0; wv < 3; ++wv) {
        float ph = s_wavePhase + (float)wv * 0.34f;
        if (ph >= 1.0f) ph -= 1.0f;
        w.opa = (lv_opa_t)((1.0f - ph) * 245.0f);
        if (w.opa > 6) lv_draw_arc(d, &w, &ac.pos, (uint16_t)(BALL_R + 3 + ph * WAVE_EXPAND), 0, 360);
    }

    // the ball
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    b.border_color = lv_color_hex(0x7A5A00);
    b.border_width = 1;
    b.border_opa = 150;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - BALL_R), (lv_coord_t)(ac.pos.y - BALL_R),
                    (lv_coord_t)(ac.pos.x + BALL_R), (lv_coord_t)(ac.pos.y + BALL_R) };
    lv_draw_rect(d, &b, &r);

    // glossy highlight
    lv_draw_rect_dsc_t hl;
    lv_draw_rect_dsc_init(&hl);
    hl.bg_color = lv_color_hex(0xFFFBCC);
    hl.bg_opa = 170;
    hl.radius = LV_RADIUS_CIRCLE;
    lv_area_t hr = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 6),
                     (lv_coord_t)(ac.pos.x - 1), (lv_coord_t)(ac.pos.y - 2) };
    lv_draw_rect(d, &hl, &hr);
}

static void draw_offrange(lv_draw_ctx_t *d, const AcDraw &ac) {
    // small ball at the rim
    lv_draw_rect_dsc_t b;
    lv_draw_rect_dsc_init(&b);
    b.bg_color = ac.emergency ? ORB_EMERG : ORB_BLIP;
    b.bg_opa = LV_OPA_COVER;
    b.radius = LV_RADIUS_CIRCLE;
    lv_area_t r = { (lv_coord_t)(ac.pos.x - 5), (lv_coord_t)(ac.pos.y - 5),
                    (lv_coord_t)(ac.pos.x + 5), (lv_coord_t)(ac.pos.y + 5) };
    lv_draw_rect(d, &b, &r);

    // small orange triangle just outside it, pointing toward the aircraft's bearing
    const lv_coord_t ox = (lv_coord_t)lroundf(ac.pos.x + 12.0f * sinf(ac.bearingDeg * (float)M_PI / 180.0f));
    const lv_coord_t oy = (lv_coord_t)lroundf(ac.pos.y - 12.0f * cosf(ac.bearingDeg * (float)M_PI / 180.0f));
    lv_point_t tri[3] = { rot_pt(0, -7, ac.bearingDeg, ox, oy),
                          rot_pt(5, 4, ac.bearingDeg, ox, oy),
                          rot_pt(-5, 4, ac.bearingDeg, ox, oy) };
    lv_draw_rect_dsc_t td;
    lv_draw_rect_dsc_init(&td);
    td.bg_color = ORB_ACCENT;
    td.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(d, &td, tri, 3);
}

// Approximate a canvas shadowBlur glow: concentric filled circles behind the
// real shape, opacity falling off with radius — the same "soft ring" trick
// draw_ball's wave animation and the sweep's fading trail already use.
static void draw_glow(lv_draw_ctx_t *d, lv_point_t pos, float baseR, float glowPx, lv_color_t color) {
    if (glowPx <= 0.5f) return;
    const int steps = 5;
    lv_draw_rect_dsc_t g;
    lv_draw_rect_dsc_init(&g);
    g.bg_color = color;
    g.radius = LV_RADIUS_CIRCLE;
    for (int i = steps; i >= 1; --i) {
        const float t = (float)i / (float)steps;
        const float r = baseR + glowPx * t;
        const lv_opa_t opa = (lv_opa_t)((1.0f - t) * (1.0f - t) * 130.0f);
        if (opa < 3) continue;
        g.bg_opa = opa;
        lv_area_t a = { (lv_coord_t)lroundf(pos.x - r), (lv_coord_t)lroundf(pos.y - r),
                        (lv_coord_t)lroundf(pos.x + r), (lv_coord_t)lroundf(pos.y + r) };
        lv_draw_rect(d, &g, &a);
    }
}

// Mirrors the editor's radarKitePoints(): kite=0 is today's notched kite (a
// tail), kite=1 collapses the notch flush with the wings into a plain triangle.
static inline void custom_kite_points(float kite, float *gx, float *gy) {
    const float t = kite < 0.0f ? 0.0f : (kite > 1.0f ? 1.0f : kite);
    const float notchY = 8.0f - 3.0f * t;
    gx[0] = 0.0f; gx[1] = 7.0f; gx[2] = 0.0f; gx[3] = -7.0f;
    gy[0] = -11.0f; gy[1] = 5.0f; gy[2] = notchY; gy[3] = 5.0f;
}

// A pushed design's own blip/selection/off-range/center look, used for every
// theme once a design is active (replaces both Orb's ball-with-waves and the
// phosphor kite/triangle paths below). Trails stay on regardless of style —
// the editor has no live data to preview motion history with, but it's a
// harmless, useful device-only extra, not a visual regression from the design.
// Phase 0 instrumentation for Flight Tracker. Every aircraft on screen gets its icon
// rotated to heading with antialiasing on, which is the most expensive primitive LVGL
// has, and there can be two dozen of them. Print the real cost before optimising it:
// the alternative is pre-rotating the icon into cached bitmaps, which is real work and
// should not be built on an estimate.
static void draw_custom_ac(lv_draw_ctx_t *d) {
#ifdef ARDUINO
    const uint32_t t_ac0 = micros();
    int acDrawn = 0;
#endif
    const theme_style::Radar &rs = theme_style::radar();
    for (const AcDraw &ac : s_acs) {
        // Masked by an exclusion zone: draw nothing for it at all, icon or off-range
        // arrow. It reappears the moment it clears the far side.
        if (ac_masked(ac)) continue;
        if (!ac.inRange) {
            if (!rs.offRangeEnabled) continue;
            const lv_color_t oc = lv_color_hex(rs.offRangeColor);
            lv_draw_rect_dsc_t b;
            lv_draw_rect_dsc_init(&b);
            b.bg_color = oc; b.bg_opa = LV_OPA_COVER; b.radius = LV_RADIUS_CIRCLE;
            const lv_coord_t sz = (lv_coord_t)rs.offRangeSize;
            lv_area_t r = { (lv_coord_t)(ac.pos.x - sz), (lv_coord_t)(ac.pos.y - sz),
                            (lv_coord_t)(ac.pos.x + sz), (lv_coord_t)(ac.pos.y + sz) };
            lv_draw_rect(d, &b, &r);
            const lv_coord_t ox = (lv_coord_t)lroundf(ac.pos.x + 12.0f * sinf(ac.bearingDeg * (float)M_PI / 180.0f));
            const lv_coord_t oy = (lv_coord_t)lroundf(ac.pos.y - 12.0f * cosf(ac.bearingDeg * (float)M_PI / 180.0f));
            lv_point_t tri[3] = { rot_pt(0, -7, ac.bearingDeg, ox, oy),
                                  rot_pt(5, 4, ac.bearingDeg, ox, oy),
                                  rot_pt(-5, 4, ac.bearingDeg, ox, oy) };
            lv_draw_rect_dsc_t td;
            lv_draw_rect_dsc_init(&td);
            td.bg_color = oc; td.bg_opa = LV_OPA_COVER;
            lv_draw_polygon(d, &td, tri, 3);
            continue;
        }

        draw_trail(d, ac, ac.color);

        lv_color_t blipColor;
        if (rs.blipFixedColorMode) {
            blipColor = lv_color_hex(rs.blipFixedColor);
        } else {
            if (ac.onGround)           blipColor = lv_color_hex(rs.blipAltGround);
            else if (ac.altFt < 3000)  blipColor = lv_color_hex(rs.blipAltLow);
            else if (ac.altFt < 10000) blipColor = lv_color_hex(rs.blipAltMid);
            else if (ac.altFt < 20000) blipColor = lv_color_hex(rs.blipAltHigh);
            else if (ac.altFt < 30000) blipColor = lv_color_hex(rs.blipAltCruise);
            else                       blipColor = lv_color_hex(rs.blipAltJet);
        }
        // Selection style 1 (Glow) / 2 (Recolor) change the selected aircraft's
        // OWN icon draw below instead of adding a separate ring — style 0
        // (Ring, the original/default look) leaves blipColor/glow untouched
        // here and draws the ring afterward, exactly as before this field existed.
        const bool isSelected = rs.selEnabled && !s_selHex.empty() && s_selHex == ac.hex;
        if (isSelected && rs.selStyle == 2) blipColor = lv_color_hex(rs.selColor);
        // The blip icon itself (dot/kite/image) — off-range arrow and
        // selection ring below have their own separate enabled toggles.
        if (rs.blipEnabled) {
#ifdef ARDUINO
        ++acDrawn;
#endif
        // Selection style 1 (Glow): boost this aircraft's own glow to at
        // least a visible amount (a 0 selGlow default would otherwise be
        // invisible the moment you switch to Glow) using the selection's
        // color, same "boost the blip's own draw" approach the editor
        // preview uses (see drawBlipImage/drawBlipVector in app.js).
        float selBlipGlowAmt = (float)rs.blipGlow;
        lv_color_t selBlipGlowColor = lv_color_hex(rs.blipGlowColor);
        if (isSelected && rs.selStyle == 1) {
            const float boosted = rs.selGlow > 0 ? (float)rs.selGlow : 18.0f;
            if (boosted > selBlipGlowAmt) selBlipGlowAmt = boosted;
            selBlipGlowColor = lv_color_hex(rs.selGlowColor);
        }
        draw_glow(d, ac.pos, (float)rs.blipSize, selBlipGlowAmt, selBlipGlowColor);

        if (rs.blipTypeImage) {
            // The uploaded aircraft icon, rotated to heading and (optionally) recolored
            // by altitude band via LVGL's own image recolor mix — the icon's alpha was
            // already derived from its Blend mode client-side (see exportRadarLayers'
            // blipIcon step), so this is always a plain alpha-over rotate here, no
            // blend-mode handling needed at draw time. Pivot/icon bitmap themselves
            // stay compile-time (coupled to whichever icon PNG is actually baked in —
            // see theme_style.h) — only color/size/tint travel per theme here.
            if (const lv_img_dsc_t *icon = radar_custom_blip_icon()) {
                // "Rotate to heading" off: the icon always shows at its own
                // baseline angle, it just moves with the aircraft's position.
                const float headingDeg = rs.blipRotate ? ((ac.track != ac.track) ? 0.0f : ac.track) : 0.0f;
                lv_draw_img_dsc_t idsc;
                lv_draw_img_dsc_init(&idsc);
                idsc.angle = (int16_t)lroundf((headingDeg + CUSTOM_BLIP_IMAGE_BASELINE_DEG) * 10.0f);
                // Theme data first, welded macro as the fallback. A theme that ships its
                // own blip sprite has to be able to say where that sprite turns.
                const int bpx = rs.blipPivotX >= 0 ? rs.blipPivotX : CUSTOM_RADAR_BLIP_PIVOT_X;
                const int bpy = rs.blipPivotY >= 0 ? rs.blipPivotY : CUSTOM_RADAR_BLIP_PIVOT_Y;
                idsc.pivot.x = bpx;
                idsc.pivot.y = bpy;
                idsc.opa = LV_OPA_COVER;
                idsc.antialias = 1;
                // Selection style 2 (Recolor) forces the tint on for this one
                // aircraft even if the design normally leaves the icon
                // untinted — blipColor is already overridden to selColor
                // above, so this recolors it, same as the editor preview.
                if (rs.blipImageTint || (isSelected && rs.selStyle == 2)) {
                    idsc.recolor = blipColor;
                    idsc.recolor_opa = LV_OPA_COVER;
                }
                const lv_coord_t x0 = (lv_coord_t)(ac.pos.x - bpx);
                const lv_coord_t y0 = (lv_coord_t)(ac.pos.y - bpy);
                lv_area_t r = { x0, y0, (lv_coord_t)(x0 + icon->header.w - 1), (lv_coord_t)(y0 + icon->header.h - 1) };
                lv_draw_img(d, &idsc, &r, icon);
            } else {
                // Design says "image" but nothing decoded (no icon uploaded, or the SD/
                // flash asset failed) — fall back to the plain dot so a blip is never
                // silently invisible.
                lv_draw_rect_dsc_t g;
                lv_draw_rect_dsc_init(&g);
                g.bg_color = blipColor; g.bg_opa = LV_OPA_COVER; g.radius = LV_RADIUS_CIRCLE;
                const lv_coord_t sz = (lv_coord_t)rs.blipSize;
                lv_area_t r = { (lv_coord_t)(ac.pos.x - sz), (lv_coord_t)(ac.pos.y - sz),
                                (lv_coord_t)(ac.pos.x + sz), (lv_coord_t)(ac.pos.y + sz) };
                lv_draw_rect(d, &g, &r);
            }
        } else if (rs.blipKiteShape) {
            const float th = (rs.blipRotate ? ((ac.track != ac.track) ? 0.0f : ac.track) : 0.0f) * (float)M_PI / 180.0f;
            const float cth = cosf(th), sth = sinf(th);
            float gx[4], gy[4];
            custom_kite_points((float)rs.blipKiteT / 100.0f, gx, gy);
            const float scale = (float)rs.blipSize / 9.0f;
            lv_point_t pts[4];
            for (int i = 0; i < 4; ++i) {
                const float x = (gx[i] * scale) * cth - (gy[i] * scale) * sth;
                const float y = (gx[i] * scale) * sth + (gy[i] * scale) * cth;
                pts[i].x = (lv_coord_t)(ac.pos.x + (lv_coord_t)lroundf(x));
                pts[i].y = (lv_coord_t)(ac.pos.y + (lv_coord_t)lroundf(y));
            }
            lv_draw_rect_dsc_t g;
            lv_draw_rect_dsc_init(&g);
            g.bg_color = blipColor; g.bg_opa = LV_OPA_COVER;
            lv_draw_polygon(d, &g, pts, 4);
        } else {
            lv_draw_rect_dsc_t g;
            lv_draw_rect_dsc_init(&g);
            g.bg_color = blipColor; g.bg_opa = LV_OPA_COVER; g.radius = LV_RADIUS_CIRCLE;
            const lv_coord_t sz = (lv_coord_t)rs.blipSize;
            lv_area_t r = { (lv_coord_t)(ac.pos.x - sz), (lv_coord_t)(ac.pos.y - sz),
                            (lv_coord_t)(ac.pos.x + sz), (lv_coord_t)(ac.pos.y + sz) };
            lv_draw_rect(d, &g, &r);
        }
        } // rs.blipEnabled

        if (isSelected && rs.selStyle == 0) {
            draw_glow(d, ac.pos, (float)rs.selDiameter / 2.0f, (float)rs.selGlow, lv_color_hex(rs.selGlowColor));
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.color = lv_color_hex(rs.selColor); sr.width = rs.selWidth; sr.opa = 240;
            lv_draw_arc(d, &sr, &ac.pos, (uint16_t)(rs.selDiameter / 2), 0, 360);
        }
    }

    // Center marker, drawn last so it sits over every blip — matches the editor's z-order.
    if (rs.centerEnabled) {
        lv_draw_rect_dsc_t cd;
        lv_draw_rect_dsc_init(&cd);
        cd.bg_color = lv_color_hex(rs.centerColor); cd.bg_opa = LV_OPA_COVER; cd.radius = LV_RADIUS_CIRCLE;
        lv_area_t cr = { (lv_coord_t)(s_cx - rs.centerRadius), (lv_coord_t)(s_cy - rs.centerRadius),
                         (lv_coord_t)(s_cx + rs.centerRadius), (lv_coord_t)(s_cy + rs.centerRadius) };
        lv_draw_rect(d, &cd, &cr);
        lv_draw_rect_dsc_t ci;
        lv_draw_rect_dsc_init(&ci);
        ci.bg_color = lv_color_hex(rs.centerInnerColor); ci.bg_opa = LV_OPA_COVER; ci.radius = LV_RADIUS_CIRCLE;
        lv_area_t cir = { (lv_coord_t)(s_cx - rs.centerInnerRadius), (lv_coord_t)(s_cy - rs.centerInnerRadius),
                          (lv_coord_t)(s_cx + rs.centerInnerRadius), (lv_coord_t)(s_cy + rs.centerInnerRadius) };
        lv_draw_rect(d, &ci, &cir);
    }
#ifdef ARDUINO
    {   // Rate-limited: this runs every frame and the log itself must not become the cost.
        static uint32_t s_lastLog = 0;
        const uint32_t now = millis();
        if (now - s_lastLog > 2000) {
            s_lastLog = now;
            Serial.printf("[perf] radar aircraft draw: %lu us for %d aircraft (%s icons)\n",
                          (unsigned long)(micros() - t_ac0), acDrawn,
                          rs.blipTypeImage ? (rs.blipRotate ? "rotated image" : "image, no rotate")
                                           : "vector");
        }
    }
#endif
}

static void ac_draw_cb(lv_event_t *e) {
    lv_draw_ctx_t *d = lv_event_get_draw_ctx(e);
    if (customStyled()) { draw_custom_ac(d); return; }
    const bool drg = orb();
    int balls = 0, arrows = 0;

    for (const AcDraw &ac : s_acs) {
        if (ac_masked(ac)) continue;   // exclusion zones apply to the stock scopes too
        if (drg) {
            if (ac.inRange) {
                if (balls >= ORB_BLIPS) continue;   // up to 7 in-range balls
                draw_trail(d, ac, ORB_FLOW);
                draw_ball(d, ac);
                balls++;
            } else {
                if (arrows >= ORB_ARROWS) continue;  // up to 8 off-range arrows
                draw_offrange(d, ac);
                arrows++;
            }
        } else {
            if (!ac.inRange) continue;            // phosphor shows in-range traffic only
            draw_trail(d, ac, ac.color);
            const float th = ((ac.track != ac.track) ? 0.0f : ac.track) * (float)M_PI / 180.0f;
            const float c = cosf(th), s = sinf(th);
            const float *gx = GX, *gy = GY;
            if (aviator()) {                     // little bombers vs little fighters (both convex kites)
                if (is_big_type(ac.type)) { gx = BOMBER_X;  gy = BOMBER_Y; }
                else                      { gx = FIGHTER_X; gy = FIGHTER_Y; }
            }
            lv_point_t pts[4];
            for (int i = 0; i < 4; ++i) {
                const float x = gx[i] * c - gy[i] * s;
                const float y = gx[i] * s + gy[i] * c;
                pts[i].x = (lv_coord_t)(ac.pos.x + (lv_coord_t)lroundf(x));
                pts[i].y = (lv_coord_t)(ac.pos.y + (lv_coord_t)lroundf(y));
            }
            lv_draw_rect_dsc_t g;
            lv_draw_rect_dsc_init(&g);
            g.bg_color = ac.color;
            g.bg_opa = LV_OPA_COVER;
            lv_draw_polygon(d, &g, pts, 4);
            if (ac.emergency) {
                lv_draw_arc_dsc_t h;
                lv_draw_arc_dsc_init(&h);
                h.color = COL_EMERG; h.width = 2; h.opa = 200;
                lv_draw_arc(d, &h, &ac.pos, 16, 0, 360);
            }
        }

        // selection ring(s)
        if (!s_selHex.empty() && s_selHex == ac.hex) {
            lv_draw_arc_dsc_t sr;
            lv_draw_arc_dsc_init(&sr);
            sr.width = 2;
            sr.opa = 240;
            if (drg) {
                sr.color = ORB_ACCENT;
                lv_draw_arc(d, &sr, &ac.pos, 15, 0, 360);
                lv_draw_arc(d, &sr, &ac.pos, 23, 0, 360);
            } else {
                sr.color = ac.emergency ? COL_EMERG : s_cInk;
                lv_draw_arc(d, &sr, &ac.pos, 19, 0, 360);
            }
        }

        // floating labels (phosphor only; orb keeps clean balls + the tap card)
        if (!drg) {
            lv_draw_label_dsc_t lc;
            lv_draw_label_dsc_init(&lc);
            lc.font = s_bigText ? &lv_font_montserrat_18 : &lv_font_montserrat_14;
            lc.color = s_cInk;
            lv_area_t a1 = { (lv_coord_t)(ac.pos.x + 12), (lv_coord_t)(ac.pos.y - 14),
                             (lv_coord_t)(ac.pos.x + 168), (lv_coord_t)(ac.pos.y + 4) };
            if (ac.call[0]) lv_draw_label(d, &lc, &a1, ac.call, NULL);
            lv_draw_label_dsc_t la;
            lv_draw_label_dsc_init(&la);
            la.font = s_bigText ? &lv_font_montserrat_16 : &lv_font_montserrat_12;
            la.color = ac.color;
            lv_area_t a2 = { a1.x1, (lv_coord_t)(ac.pos.y + 4), a1.x2, (lv_coord_t)(ac.pos.y + 26) };
            if (ac.altTxt[0]) lv_draw_label(d, &la, &a2, ac.altTxt, NULL);
        }
    }
}

// =============================== helpers =====================================
static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                            lv_color_t color, lv_align_t align, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, align, dx, dy);
    return l;
}

static lv_obj_t *make_layer(lv_obj_t *parent, lv_event_cb_t draw_cb) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, SCREEN_W, SCREEN_H);
    lv_obj_center(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (draw_cb) lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    return o;
}

static void pulse_anim_cb(void *obj, int32_t v) {
    lv_obj_t *o = (lv_obj_t *)obj;
    const lv_coord_t dia = 10 + (lv_coord_t)((v * 44) / 100);
    lv_obj_set_size(o, dia, dia);
    lv_obj_center(o);
    lv_obj_set_style_border_opa(o, (lv_opa_t)(220 - v * 220 / 100), 0);
}

// Leave selection mode: clear the selection and hand the knob back to the shell
// so a turn opens the app switcher again. File scope so the sweep timer's idle
// check (above, outside the namespace) and knobPress()/knobExit() can all call it.
static void radar_exit_select() {
    radar::select(-1);
    s_selectMode = false;
    app_shell::setCaptured(false);
}

// A pushed design can reorder its six movable layers — sweep, aircraft
// (blips/selection/off-range/center, all drawn by ac_draw_cb), the
// selection-banner text canvas, the two plain static image overlays, and the
// plain color-wash overlay — e.g. so the sweep sits above the text instead
// of below it, or so the color wash covers everything but the topmost
// static. The order lists them back-to-front (0=sweep, 1=aircraft, 2=text,
// 3=static1, 4=static2, 5=overlay), the same convention as the clock's
// CUSTOM_HAND_ORDER. Unlike the hands (sprites redrawn in order inside one
// callback), these are separate LVGL objects, so re-stacking means actually
// moving them; s_overlayImg (CRT+glass, NOT the same thing as the color-wash
// s_dimLayer above) is reasserted last so it always stays the true top layer
// regardless of where the other six land.

// Where that order comes from. A theme installed as files alone (Orb Studio) states it
// in radar_style.json; anything older says nothing and keeps whatever
// CUSTOM_RADAR_LAYER_ORDER the last firmware push welded in.
static int radarLayerOrder(const int **out) {
    static const int welded[] = CUSTOM_RADAR_LAYER_ORDER;
    if (customStyled() && theme_style::radar().orderN > 0) {
        *out = theme_style::radar().order;
        return theme_style::radar().orderN;
    }
    *out = welded;
    return CUSTOM_RADAR_LAYER_ORDER_N;
}

static void applyRadarLayerOrder() {
    const int *order = nullptr;
    const int orderN = radarLayerOrder(&order);
    // 0=sweep, 1=aircraft, 2=text, 3=static1, 4=static2, 5=overlay (color
    // wash). Whichever sweep object is actually active (vector wedge or
    // rotating image) takes the "sweep" slot — only one of them is ever
    // visible at a time.
    const bool sweepImgActive = customStyled() && theme_style::radar().sweepTypeImage;
    lv_obj_t *byKind[6] = { sweepImgActive ? s_sweepImg : s_sweep, s_acLayer, s_textCanvas, s_staticImg[0], s_staticImg[1], s_dimLayer };
    for (int i = 0; i < orderN; ++i) {
        const int k = order[i];
        if (k >= 0 && k < 6 && byKind[k]) lv_obj_move_foreground(byKind[k]);
    }
    if (s_overlayImg) lv_obj_move_foreground(s_overlayImg);
}

namespace radar {

static void apply_grid_visibility();   // defined with the flatten pass, used by the probe below

// Diagnostic: hide a single layer so its cost shows up as a frame-rate delta.
// Deliberately blunt and deliberately not persisted — it exists to answer "which
// layer is expensive" with a measurement rather than an argument.
void debugHideLayer(int kind, bool hide) {
    lv_obj_t *o = nullptr;
    switch (kind) {
        case 0: o = (customStyled() && theme_style::radar().sweepTypeImage) ? s_sweepImg : s_sweep; break;
        case 1: o = s_acLayer;      break;
        case 2: o = s_textCanvas;   break;
        case 3: o = s_staticImg[0]; break;
        case 4: o = s_staticImg[1]; break;
        case 5: o = s_dimLayer;     break;
        case 6: o = s_plateImg;     break;
        case 7: o = s_gridLayer;    break;   // map: roads + coastline + airports, re-vectored per draw
        default: return;
    }
    // "Show" for the map layer means "whatever the bake decided", not blindly visible:
    // un-hiding a map that is baked into the background would turn per-frame
    // re-vectoring back on, which is exactly what the probe did to tonight's baseline.
    if (kind == 7 && !hide) { apply_grid_visibility(); return; }
    if (o) show(o, !hide);
    Serial.printf("[radar] debug: layer %d %s\n", kind, hide ? "hidden" : "shown");
}


static void refresh_custom_text();   // defined near select()/selected(); update() below needs it forward-declared

void setTheme(int t) {
    s_theme = ((t % THEME_COUNT) + THEME_COUNT) % THEME_COUNT;
    const bool drg = orb();

    if (officeMode()) {
        const AppPalette &p = app_theme::palette();
        s_cRing = p.hairline; s_cLead = p.accent; s_cInk = p.ink; s_cSoft = p.soft;
    } else {
        switch (s_theme) {                          // pick the scope chrome palette
            case THEME_MILITARY:
                s_cRing = lv_color_hex(0x49C46B); s_cLead = lv_color_hex(0x76E08C);
                s_cInk  = lv_color_hex(0xE0FFE6); s_cSoft = lv_color_hex(0x9FD7A8); break;
            case THEME_AVIATOR:
                s_cRing = AVI_RING; s_cLead = AVI_LEAD; s_cInk = AVI_INK; s_cSoft = AVI_SOFT; break;
            default:                                // orb (uses its own colors elsewhere) / any invalid value
                s_cRing = COL_GREEN; s_cLead = COL_LEAD; s_cInk = COL_INK; s_cSoft = COL_SOFT; break;
        }
    }

    if (s_parent) {
        if (drg) {
            lv_obj_set_style_bg_color(s_parent, ORB_BG_TOP, 0);
            lv_obj_set_style_bg_grad_color(s_parent, ORB_BG_BOT, 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_VER, 0);
        } else {
            lv_obj_set_style_bg_color(s_parent, officeMode() ? app_theme::palette().bg : (aviator() ? AVI_BG : lv_color_black()), 0);
            lv_obj_set_style_bg_grad_dir(s_parent, LV_GRAD_DIR_NONE, 0);
        }
        lv_obj_set_style_bg_opa(s_parent, LV_OPA_COVER, 0);
    }
    // A custom design has no compass letters, range readout, or pulse ring in
    // its own preview — hide all of the native chrome so the device matches it.
    const bool styled = customStyled();
    for (int i = 0; i < 4; ++i) show(s_rose[i], !drg && !styled);   // hide compass in Orb
    show(s_rangeLbl, !drg && s_rangeLblVisible && !styled);
    show(s_centerDot, !drg && !styled);                   // orb draws an orange triangle instead; custom style draws its own center marker in ac_draw_cb
    show(s_pulse, !drg && !styled);

    // retint the persistent chrome objects for the active palette
    if (s_rose[0]) lv_obj_set_style_text_color(s_rose[0], s_cInk, 0);
    for (int i = 1; i < 4; ++i) if (s_rose[i]) lv_obj_set_style_text_color(s_rose[i], s_cSoft, 0);
    if (s_centerDot) lv_obj_set_style_bg_color(s_centerDot, s_cInk, 0);
    if (s_pulse)     lv_obj_set_style_border_color(s_pulse, s_cInk, 0);
    if (s_rangeLbl)  lv_obj_set_style_text_color(s_rangeLbl, s_cRing, 0);

    flow_redraw_all();
    if (s_parent) lv_obj_invalidate(s_parent);
    show_theme_label(THEME_NAMES[s_theme]);
    if (s_themeCb) s_themeCb(s_theme);
}

int  theme() { return s_theme; }
const char *themeName(int t) {
    return (t >= 0 && t < THEME_COUNT) ? THEME_NAMES[t] : "";
}
void cycleTheme() { setTheme(s_theme + 1); }
void flashThemeName() { show_theme_label(THEME_NAMES[s_theme]); }   // touch reveal (no theme change)
void setThemeChangedCb(void (*cb)(int)) { s_themeCb = cb; }
void setRangeLabelVisible(bool v) { s_rangeLblVisible = v; if (s_rangeLbl) show(s_rangeLbl, v && !orb() && !customStyled()); }

void setSweepEnabled(bool on) {
    s_sweepEnabled = on;
    const bool sweepImgActive = customStyled() && theme_style::radar().sweepTypeImage;
    if (s_sweep) {
        show(s_sweep, on && !sweepImgActive);
        if (!on) lv_obj_invalidate(s_sweep);   // clear any wedge currently painted
    }
    if (s_sweepImg) show(s_sweepImg, on && sweepImgActive);
}
bool sweepEnabled() { return s_sweepEnabled; }

// Defined with the flatten pass below; setAirportsEnabled sits above it in the file.
static void take_map_snapshot();
static void rebuild_flat_background();
static void apply_grid_visibility();

void setAirportsEnabled(bool on) {
    s_airportsEnabled = on;
    if (s_gridLayer) lv_obj_invalidate(s_gridLayer);   // repaint the chrome with/without markers
    if (customStyled()) {          // the etched copy includes the markers; re-etch without them
        take_map_snapshot();
        rebuild_flat_background();
        apply_grid_visibility();
    }
}
bool airportsEnabled() { return s_airportsEnabled; }

// 0 = off, 1 = short, 2 = medium (default), 3 = long. Controls both the per-aircraft
// trail and the persistent flow layer (the long-lived "where everything has been" tracks).
void setTrailLength(int level) {
    switch (level) {
        case 0: s_trailMax = 0;  s_flowMax = 0;    s_flowGenMax = 0;  break;
        // Segment counts halved from (150/1500/700). A repaint costs roughly 300 us per
        // segment on this hardware — that is LVGL canvas draw-call overhead, not line
        // length — so 700 segments is a 210 ms repaint and 240 is a 70 ms one. Even
        // batched, a repaint should fit inside about one frame rather than three.
        case 1: s_trailMax = 3;  s_flowMax = 80;   s_flowGenMax = 8;  break;   // ~16 s
        case 3: s_trailMax = 12; s_flowMax = 700;  s_flowGenMax = 30; break;   // ~60 s
        default: s_trailMax = 7; s_flowMax = 240;  s_flowGenMax = 14; break;   // ~28 s
    }
    if (s_flowMax == 0) { s_flow.clear(); s_trails.clear(); }
    else while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
    flow_redraw_all();                              // repaint the flow canvas at the new length
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setMaxOnScreen(int n) {
    s_maxOnScreen = (n < 1) ? 1 : (n > ADSB_MAX_AIRCRAFT ? ADSB_MAX_AIRCRAFT : n);  // never more than the feed pulls
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}

void setLargeText(bool on) {
    s_bigText = on;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
}


// Section ledger for radar::init(), measured taking 3.7 MB — the single largest consumer
// on the device, allocated at boot whether or not Flight Tracker is ever opened.
static void rmark(const char *what) {
#ifdef ARDUINO
    static uint32_t prev = 0;
    const uint32_t now = (uint32_t)ESP.getFreePsram();
    Serial.printf("[psram/radar] %-24s free %6u KB", what, (unsigned)(now / 1024));
    if (prev && prev >= now) Serial.printf("   (-%u KB)", (unsigned)((prev - now) / 1024));
    prev = now;
    Serial.println();
#else
    (void)what;
#endif
}

void init(void *lv_parent) {
    lv_obj_t *parent = (lv_obj_t *)lv_parent;
    s_parent = parent;
    s_cx = SCREEN_CX;
    s_cy = SCREEN_CY;
    s_acs.clear();
    s_trails.clear();
    s_flow.clear();
    s_selHex.clear();
    s_flowRedrawCtr = 0;

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    // Baked plate (background+rings+crosshair): the bottom-most layer, created
    // first so everything else (coastline, sweep, aircraft) draws over it.
    rmark("radar::init start");
    s_plateImg = lv_img_create(parent);
    rmark("after plate img");
    lv_obj_clear_flag(s_plateImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_plateImg);
    lv_obj_add_flag(s_plateImg, LV_OBJ_FLAG_HIDDEN);

    // Canvas OBJECT only; its 636 KB buffer arrives via canvas_acquire() the first time
    // a trail segment actually needs drawing (see flow_redraw_all/update), and leaves
    // when trails are cleared. Hidden while unbuffered so composition skips it.
    rmark("after flow buffer");
    s_flowCanvas = lv_canvas_create(parent);
    lv_obj_clear_flag(s_flowCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_flowCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(s_flowCanvas);

    s_gridLayer = make_layer(parent, grid_draw_cb);

    // Rings and crosshair, created straight after the map so LVGL's own creation order
    // puts the etching over the roads. Everything movable is lifted above this by
    // applyRadarLayerOrder(), so it can never end up over an aircraft.
    s_ringsImg = lv_img_create(parent);
    lv_obj_clear_flag(s_ringsImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_ringsImg);
    lv_obj_add_flag(s_ringsImg, LV_OBJ_FLAG_HIDDEN);

    s_sweep     = make_layer(parent, sweep_draw_cb);
    s_acLayer   = make_layer(parent, ac_draw_cb);

    // The sweep's "image" type: a real lv_img, rotated live (see
    // sweep_timer_cb), shown instead of s_sweep's vector wedge when active.
    rmark("after flow canvas");
    s_sweepImg = lv_img_create(parent);
    rmark("after sweep img");
    // Antialias the sweep's rotation. This was previously off, on the reasoning that the
    // blade's edges "are a soft glow to begin with" so filtering bought nothing. That was
    // true of the sweep it was written for and is false of a themed one: Steam Punk's is
    // hard-edged brass with gear teeth and a thin shaft, and nearest-neighbour rotation
    // makes that fine detail crawl and snap from frame to frame. Read as jitter on device.
    //
    // The cost argument does not survive measurement either. The sprite is 73x227, about
    // 16k pixels; the ~880 ms/s this screen spends in LVGL goes on recompositing the
    // near-full-screen area the rotation dirties, not on the transform itself. Filtering
    // it is close to free at this size.
    lv_img_set_antialias(s_sweepImg, true);
    lv_obj_clear_flag(s_sweepImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sweepImg, LV_OBJ_FLAG_HIDDEN);

    s_rose[0] = make_label(parent, "N", &lv_font_montserrat_28, COL_INK,  LV_ALIGN_TOP_MID,    0, 12);
    s_rose[1] = make_label(parent, "S", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_BOTTOM_MID, 0, -12);
    s_rose[2] = make_label(parent, "E", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_RIGHT_MID, -12, 0);
    s_rose[3] = make_label(parent, "W", &lv_font_montserrat_16, COL_SOFT, LV_ALIGN_LEFT_MID,   12, 0);

    char rng[16];
    snprintf(rng, sizeof(rng), "%.0f km", (double)RANGE_KM_DEFAULT);
    s_rangeLbl = make_label(parent, rng, &lv_font_montserrat_14, COL_GREEN, LV_ALIGN_CENTER, 92, -8);
    lv_obj_set_style_text_opa(s_rangeLbl, 128, 0);

    // theme-name banner: flashed briefly on a theme change or a screen tap (see
    // show_theme_label), sits below the HUD status row (y ~50-70) so it never overlaps
    // it. A solid black plaque behind white text keeps it readable over any theme/scene.
    // Deliberately plain and unthemeable, same reasoning as the update overlay: it is a
    // system message about the device's state, and a theme that styled it into
    // invisibility would defeat the one job it has.
    s_loading = make_label(parent, "Loading aircraft\nand location data", &lv_font_montserrat_20,
                           lv_color_white(), LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_loading, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_loading, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_loading, 12, 0);
    lv_obj_set_style_pad_all(s_loading, 18, 0);
    lv_obj_set_style_text_align(s_loading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_loading, 6, 0);
    show(s_loading, false);

    s_themeLabel = make_label(parent, "", &lv_font_montserrat_20, lv_color_white(), LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_color(s_themeLabel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_themeLabel, LV_OPA_90, 0);
    lv_obj_set_style_radius(s_themeLabel, 8, 0);
    lv_obj_set_style_pad_hor(s_themeLabel, 14, 0);
    lv_obj_set_style_pad_ver(s_themeLabel, 4, 0);
    show(s_themeLabel, false);

    s_pulse = lv_obj_create(parent);
    lv_obj_remove_style_all(s_pulse);
    lv_obj_set_size(s_pulse, 12, 12);
    lv_obj_center(s_pulse);
    lv_obj_set_style_radius(s_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_pulse, COL_INK, 0);
    lv_obj_set_style_border_width(s_pulse, 2, 0);
    lv_obj_clear_flag(s_pulse, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse);
    lv_anim_set_exec_cb(&a, pulse_anim_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_time(&a, 2600);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_centerDot = lv_obj_create(parent);
    lv_obj_remove_style_all(s_centerDot);
    lv_obj_set_size(s_centerDot, 7, 7);
    lv_obj_center(s_centerDot);
    lv_obj_set_style_radius(s_centerDot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_centerDot, COL_INK, 0);
    lv_obj_set_style_bg_opa(s_centerDot, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_centerDot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Selection text banners (callsign/stats/route), a Launch Kit push only — a
    // dedicated transparent canvas (not LVGL labels), so a banner can curve along
    // an arc and glow, redrawn by refresh_custom_text() whenever the custom
    // design is active and something is selected. Created after the aircraft
    // layer so banners sit above the scope, rings, and blips.
    // Created BEFORE the text canvas so they sit under it in LVGL's own z-order, which is
    // creation order among siblings — the card is a backdrop for the words, never over them.
    s_cardImg = lv_img_create(parent);
    lv_obj_clear_flag(s_cardImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cardImg, LV_OBJ_FLAG_HIDDEN);
    s_cardObj = lv_obj_create(parent);
    lv_obj_remove_style_all(s_cardObj);
    lv_obj_clear_flag(s_cardObj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cardObj, LV_OBJ_FLAG_HIDDEN);

#if CUSTOM_HAS_RADAR
    {
        // Canvas OBJECT only; the 636 KB buffer is acquired by refresh_custom_text()
        // while banners are actually visible (a selection exists, or an always-on
        // RTEXT4 range banner is compiled in) and released when they are not.
        rmark("after text buffer");
        s_textCanvas = lv_canvas_create(parent);
        lv_obj_clear_flag(s_textCanvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_textCanvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_center(s_textCanvas);
    }
#endif

    // Two plain decorative overlays — no rotation, just position+opacity —
    // insertable anywhere in the layer order via applyRadarLayerOrder().
    for (int i = 0; i < 2; ++i) {
        s_staticImg[i] = lv_img_create(parent);
        lv_obj_clear_flag(s_staticImg[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(s_staticImg[i], LV_OBJ_FLAG_HIDDEN);
    }

    // The "Overlay" card: a plain full-scope color wash, no image — just a
    // flat fill+opacity rect sized to the whole screen (the round display's
    // own hardware/LVGL clipping crops it to the circle, same as everything
    // else here, so no separate mask is needed). Insertable anywhere in the
    // layer order like the two statics above.
    s_dimLayer = lv_obj_create(parent);
    lv_obj_remove_style_all(s_dimLayer);
    lv_obj_set_size(s_dimLayer, SCREEN_W, SCREEN_H);
    lv_obj_center(s_dimLayer);
    lv_obj_clear_flag(s_dimLayer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_dimLayer, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_dimLayer, LV_OBJ_FLAG_HIDDEN);

    // Baked overlay (CRT+glass): the top-most layer, created last so it sits
    // over the sweep/aircraft/selection-banner layers too, matching the
    // editor's own draw order (CRT/glass are painted after everything else).
    rmark("after statics");
    s_overlayImg = lv_img_create(parent);
    rmark("after overlay img");
    lv_obj_clear_flag(s_overlayImg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_overlayImg);
    lv_obj_add_flag(s_overlayImg, LV_OBJ_FLAG_HIDDEN);

    s_sweepDeg = 0.0f;
    s_prevSweepDeg = 0.0f;
    if (!s_timer) s_timer = lv_timer_create(sweep_timer_cb, SWEEP_FRAME_MS, nullptr);

    rmark("before refreshCustomStyle");
    // NOT decoding the artwork here any more. This call pulled the plate, sweep, blip
    // and both static layers off the SD card at boot — measured at 2431 KB — for a
    // screen the user may never open. Flight Tracker already has the right lifecycle:
    // knobEnter() calls refreshCustomStyle() when the app is actually shown, and
    // knobExit() calls radar_sprite_release() when it is left. init() was simply doing
    // it eagerly as well, so the memory was claimed from boot and never handed back.
    //
    // Anything that boots straight into Flight Tracker (CUSTOM_BOOT_TARGET == 2) still
    // gets its artwork, because that path goes through the app shell's onEnter.
    rmark("after refreshCustomStyle");
    setTheme(s_theme);
    rmark("after setTheme (init done)");
}

// (Re-)attach the plate/overlay image sources, decoding lazily if needed.
// ---------------- flattened static background ----------------
// The radar's lower layers never change, but every sweep frame made LVGL
// re-blend all of them across the area the rotating hand dirties (roughly
// two thirds of the screen). Measured: ~880 ms of every second inside LVGL,
// ~6 fps, which is what made the sweep step ~2.2 deg at a time instead of turn.
//
// So they are composited ONCE here, into a single opaque image, and the plate
// object is pointed at that instead. Per frame the base then costs one plain
// copy rather than a stack of alpha blends.
//
// Which layers may be absorbed is decided by the theme's own layer order, not
// assumed: walk it from the back and take static layers until the first thing
// that moves. Anything above a moving layer has to stay live or it would be
// drawn underneath something it is supposed to cover. For Steam Punk's
// { 3, 5, 1, 2, 0, 4 } that absorbs static1 and the colour wash, and leaves
// static2 alone because it sits above the sweep on purpose.
//
// Drawing is done with LVGL's own canvas rather than hand-rolled blending, so
// the merged result is produced by the exact code path that drew the layers
// separately. Scale, opacity and centring therefore match by construction.
static lv_color_t  *s_flatBuf    = nullptr;    // PSRAM, SCREEN_W*SCREEN_H
static lv_obj_t    *s_flatCanvas = nullptr;    // offscreen only; never parented into the view
static bool         s_flatOn     = false;
static bool         s_flatTook[3] = { false, false, false };   // static1, static2, wash

// The map (roads + coastline + airports), etched. s_gridLayer re-vectors all of it on
// every frame through its draw callback — 513 polylines at Zion's location, measured at
// roughly a quarter of the radar's whole frame budget. The vectors only actually change
// when the projection does (home moved, range zoomed, airports toggled), so the layer is
// rendered ONCE into this snapshot on those events, the snapshot is baked into the
// flattened background, and the live layer is hidden. Zion's three-section architecture
// assumes the map is "etched in"; this makes that assumption true.
static lv_img_dsc_t *s_mapSnap  = nullptr;
static bool          s_mapBaked = false;
static bool          s_ringsBaked = false;   // the etching went into the flat plate, so hide the live object

static void take_map_snapshot() {
    if (!s_gridLayer || !customStyled()) return;
    // The layer must be visible while it renders: snapshot drives the object's own draw
    // events, and a hidden object draws nothing, which would etch an empty map.
    show(s_gridLayer, true);
    lv_obj_update_layout(s_gridLayer);
    if (s_mapSnap) { lv_snapshot_free(s_mapSnap); s_mapSnap = nullptr; }
    s_mapSnap = lv_snapshot_take(s_gridLayer, LV_IMG_CF_TRUE_COLOR_ALPHA);
    if (!s_mapSnap) { Serial.println("[radar] map snapshot failed; live layer stays"); return; }

    // Punch the exclusion zones out of the etched map.
    //
    // Roads, coastline and airports come from three separate modules that know nothing
    // about zones, and teaching each of them to clip would mean threading zone state
    // through all three. But they have already been flattened into one RGBA image by the
    // line above — so the whole job is a single pass over that image, zeroing alpha
    // inside the zones. Every map layer gets masked at once, and the background art shows
    // through cleanly where a theme asked it to.
    //
    // Runs only when the projection changes (home moved, range zoomed), not per frame.
    if (customStyled() && theme_style::radar().zoneCount > 0) {
        // LV_IMG_CF_TRUE_COLOR_ALPHA at LV_COLOR_DEPTH 16 is 3 bytes per pixel: two of
        // colour, then the alpha byte this clears.
        const int bpp = LV_IMG_PX_SIZE_ALPHA_BYTE;
        uint8_t *px = (uint8_t *)s_mapSnap->data;
        const int w = s_mapSnap->header.w, h = s_mapSnap->header.h;
        int cleared = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (!in_excluded_zone((lv_coord_t)x, (lv_coord_t)y)) continue;
                px[(y * w + x) * bpp + (bpp - 1)] = 0x00;
                ++cleared;
            }
        }
        Serial.printf("[radar] map: %d px cleared by %d exclusion zone(s)\n",
                      cleared, theme_style::radar().zoneCount);
    }
}

// The live map layer earns its keep only while there is no baked copy. Called after any
// rebuild attempt, so a failed bake degrades to the old per-frame path, never to no map.
static void apply_grid_visibility() {
    if (s_gridLayer) show(s_gridLayer, !(customStyled() && s_mapBaked));
}

static void release_flat_background() {
    if (s_flatCanvas) { lv_obj_del(s_flatCanvas); s_flatCanvas = nullptr; }
    if (s_flatBuf)    { heap_caps_free(s_flatBuf); s_flatBuf = nullptr; }
    s_flatOn = false;
    s_flatTook[0] = s_flatTook[1] = s_flatTook[2] = false;
    s_ringsBaked = false;
    if (s_ringsImg && radar_custom_rings()) show(s_ringsImg, true);
}

static void rebuild_flat_background() {
    s_flatOn = false;
    s_mapBaked = false;
    s_ringsBaked = false;
    s_flatTook[0] = s_flatTook[1] = s_flatTook[2] = false;
    if (s_ringsImg && radar_custom_rings()) show(s_ringsImg, true);
    if (!customStyled() || !s_plateImg) return;

    const lv_img_dsc_t *plate = radar_custom_plate();
    if (!plate) return;   // nothing opaque to build on; leave the live stack alone

    // Which layers sit below the first moving one.
    const int *order = nullptr;
    const int orderN = radarLayerOrder(&order);
    bool take[3] = { false, false, false };
    int  taken = 0;
    for (int i = 0; i < orderN; ++i) {
        const int k = order[i];
        if (k == 0 || k == 1 || k == 2) break;      // sweep / aircraft / text: stop here
        if (k == 3) { take[0] = true; ++taken; }
        else if (k == 4) { take[1] = true; ++taken; }
        else if (k == 5) { take[2] = true; ++taken; }
    }
    if (!taken && !s_mapSnap) return;   // nothing to merge; not worth 434 KB to copy the plate alone

    const theme_style::Radar &rs = theme_style::radar();
    const theme_style::RadarStatic *st[2] = { &rs.static1, &rs.static2 };
    // Recheck that the layers we picked are actually contributing; a theme can
    // list a layer in its order and then switch it off.
    bool any = (s_mapSnap != nullptr);
    for (int i = 0; i < 2; ++i) if (take[i] && st[i]->show && radar_custom_static(i)) any = true;
    if (take[2] && rs.overlayEnabled && rs.overlayOpacity > 0) any = true;
    if (!any) return;

    if (!s_flatBuf) {
        s_flatBuf = (lv_color_t *)heap_caps_malloc((size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_flatBuf) { Serial.println("[radar] flatten: no PSRAM, keeping live layers"); return; }
    }
    if (!s_flatCanvas) {
        // Parented to the screen but permanently hidden: it exists to own the
        // buffer and give lv_canvas_draw_* somewhere to render, never to display.
        s_flatCanvas = lv_canvas_create(lv_scr_act());
        if (!s_flatCanvas) { Serial.println("[radar] flatten: no canvas"); return; }
        lv_obj_add_flag(s_flatCanvas, LV_OBJ_FLAG_HIDDEN);
    }
    lv_canvas_set_buffer(s_flatCanvas, s_flatBuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);

    // 1. the plate, filling the canvas
    {
        lv_draw_img_dsc_t d; lv_draw_img_dsc_init(&d);
        lv_canvas_draw_img(s_flatCanvas, 0, 0, plate, &d);
    }
    // 1b. the etched map, directly on the plate — the same slot the live s_gridLayer
    //     occupies in the stack today (below the decorations and the wash).
    if (s_mapSnap) {
        lv_draw_img_dsc_t d; lv_draw_img_dsc_init(&d);
        lv_canvas_draw_img(s_flatCanvas, 0, 0, s_mapSnap, &d);
        s_mapBaked = true;
    }
    // 1c. the etched rings, over the map for the same reason they sit over it live.
    if (const lv_img_dsc_t *rings = radar_custom_rings()) {
        lv_draw_img_dsc_t d; lv_draw_img_dsc_init(&d);
        lv_canvas_draw_img(s_flatCanvas, 0, 0, rings, &d);
        s_ringsBaked = true;
    }
    // 2. the decorative statics we are allowed to absorb, same transform as the
    //    live path above (zoom about the image's own centre, positioned by
    //    unscaled w/h so the visual centre lands on x,y at any scale)
    for (int i = 0; i < 2; ++i) {
        if (!take[i] || !st[i]->show) continue;
        const lv_img_dsc_t *img = radar_custom_static(i);
        if (!img) continue;
        lv_draw_img_dsc_t d; lv_draw_img_dsc_init(&d);
        d.zoom    = (uint16_t)lroundf(st[i]->scale * 256.0f);
        d.opa     = (lv_opa_t)st[i]->opacity;
        d.pivot.x = (lv_coord_t)(img->header.w / 2);
        d.pivot.y = (lv_coord_t)(img->header.h / 2);
        lv_canvas_draw_img(s_flatCanvas,
                           (lv_coord_t)(st[i]->x - (int)img->header.w / 2),
                           (lv_coord_t)(st[i]->y - (int)img->header.h / 2), img, &d);
        s_flatTook[i] = true;
    }
    // 3. the colour wash, over everything absorbed so far
    if (take[2] && rs.overlayEnabled && rs.overlayOpacity > 0) {
        lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
        d.bg_color = lv_color_hex(rs.overlayColor);
        d.bg_opa   = (lv_opa_t)rs.overlayOpacity;
        d.radius   = 0;
        lv_canvas_draw_rect(s_flatCanvas, 0, 0, SCREEN_W, SCREEN_H, &d);
        s_flatTook[2] = true;
    }

    // Swap the plate over to the merged image and retire what it now contains.
    lv_img_set_src(s_plateImg, lv_canvas_get_img(s_flatCanvas));
    show(s_plateImg, true);
    if (s_ringsBaked && s_ringsImg) show(s_ringsImg, false);
    for (int i = 0; i < 2; ++i) if (s_flatTook[i] && s_staticImg[i]) show(s_staticImg[i], false);
    if (s_flatTook[2] && s_dimLayer) show(s_dimLayer, false);
    s_flatOn = true;
    Serial.printf("[radar] flattened into the plate: map=%d rings=%d static1=%d static2=%d wash=%d\n",
                  (int)s_mapBaked, (int)s_ringsBaked, (int)s_flatTook[0], (int)s_flatTook[1], (int)s_flatTook[2]);
}

// Call once at init(), and again from Flight Tracker's onEnter after an
// onExit released the decoded PSRAM (radar_sprite_release()) — an image
// object's src has to be re-set after that, the same way the clock's
// draw_custom() re-fetches custom_plate()/custom_overlay() every redraw.
void refreshCustomStyle() {
    if (s_plateImg) {
        const lv_img_dsc_t *plate = radar_custom_plate();
        if (plate) { lv_img_set_src(s_plateImg, plate); show(s_plateImg, true); }
        else show(s_plateImg, false);
    }
    if (s_ringsImg) {
        const lv_img_dsc_t *rg = radar_custom_rings();
        if (rg) { lv_img_set_src(s_ringsImg, rg); show(s_ringsImg, true); }
        else show(s_ringsImg, false);
    }
    if (s_overlayImg) {
        const lv_img_dsc_t *ov = radar_custom_overlay();
        if (ov) { lv_img_set_src(s_overlayImg, ov); show(s_overlayImg, true); }
        else show(s_overlayImg, false);
    }
    // Two plain decorative overlays: position/opacity come from theme_style
    // (per-theme, see radar_style.json), the pixels from radar_sprite.cpp —
    // same SD-first-then-flash decode as the plate/overlay, just centered at
    // (x,y) instead of always filling the whole screen.
    const theme_style::RadarStatic *rs[2] = { &theme_style::radar().static1, &theme_style::radar().static2 };
    for (int i = 0; i < 2; ++i) {
        if (!s_staticImg[i]) continue;
        const lv_img_dsc_t *img = rs[i]->show ? radar_custom_static(i) : nullptr;
        if (img) {
            lv_img_set_src(s_staticImg[i], img);
            // Zoom (256 = 100%) scales around the image's own pivot, which
            // defaults to its center — so positioning by unscaled w/h below
            // still lands the visual center at (x,y) at any scale.
            lv_img_set_zoom(s_staticImg[i], (uint16_t)lroundf(rs[i]->scale * 256.0f));
            lv_obj_set_pos(s_staticImg[i], (lv_coord_t)(rs[i]->x - (int)img->header.w / 2), (lv_coord_t)(rs[i]->y - (int)img->header.h / 2));
            lv_obj_set_style_img_opa(s_staticImg[i], (lv_opa_t)rs[i]->opacity, 0);
            show(s_staticImg[i], true);
        } else {
            show(s_staticImg[i], false);
        }
    }
    // The sweep's "image" type: pivot/center are compile-time (coupled to
    // whichever sprite is actually baked in, same reasoning as the blip
    // icon's pivot) — angle is live, driven by sweep_timer_cb via s_sweepDeg.
    if (s_sweepImg) {
        const bool useImage = customStyled() && theme_style::radar().sweepTypeImage;
        const lv_img_dsc_t *sweepSrc = useImage ? radar_custom_sweep() : nullptr;
        if (sweepSrc) {
            lv_img_set_src(s_sweepImg, sweepSrc);
            // Theme data first, welded macros as the fallback (see theme_style::Radar).
            const theme_style::Radar &rsw = theme_style::radar();
            const int spx = rsw.sweepPivotX  >= 0 ? rsw.sweepPivotX  : CUSTOM_SWEEP_IMAGE_PIVOT_X;
            const int spy = rsw.sweepPivotY  >= 0 ? rsw.sweepPivotY  : CUSTOM_SWEEP_IMAGE_PIVOT_Y;
            const int scx = rsw.sweepCenterX >= 0 ? rsw.sweepCenterX : CUSTOM_SWEEP_IMAGE_CENTER_X;
            const int scy = rsw.sweepCenterY >= 0 ? rsw.sweepCenterY : CUSTOM_SWEEP_IMAGE_CENTER_Y;
            lv_img_set_pivot(s_sweepImg, spx, spy);
            lv_obj_set_pos(s_sweepImg, scx - spx, scy - spy);
            lv_img_set_angle(s_sweepImg, (int16_t)lroundf(s_sweepDeg * 10.0f));
            show(s_sweepImg, true);
        } else {
            show(s_sweepImg, false);
        }
    }
    // The "Overlay" card: plain color+opacity, no image — see s_dimLayer above.
    if (s_dimLayer) {
        const theme_style::Radar &rs2 = theme_style::radar();
        if (rs2.overlayEnabled) {
            lv_obj_set_style_bg_color(s_dimLayer, lv_color_hex(rs2.overlayColor), 0);
            lv_obj_set_style_bg_opa(s_dimLayer, (lv_opa_t)rs2.overlayOpacity, 0);
            show(s_dimLayer, true);
        } else {
            show(s_dimLayer, false);
        }
    }
    // Merge the unchanging lower layers now that every one of them has been given
    // its current source, position, scale and opacity. Runs last on purpose: it
    // reads the finished state rather than trying to predict it, and it hides only
    // what it has actually absorbed, so a failure here degrades to the live stack
    // rather than to a missing layer.
    rebuild_flat_background();
    apply_grid_visibility();
    applyRadarLayerOrder();
}

void update(const std::vector<Aircraft> &aircraft, const RadarSettings &s) {
    std::vector<AcDraw> out;
    out.reserve(aircraft.size());
    std::set<std::string> present;
    const float R = (float)RADAR_R_OUTER_PX;
    ++s_flowGen;                                  // one tick per poll; flow segments age in these units
    s_lastRangeKm = s.rangeKm;                    // kept current for the range banner (radar_range_fmt)

    // Reproject the coastline only when the scope geometry actually changes (home
    // moved or range zoomed) — never per frame. Then repaint the static chrome layer.
    static double s_coLat = 1e9, s_coLon = 1e9; static float s_coRange = -1.0f;
    if (s.homeLat != s_coLat || s.homeLon != s_coLon || s.rangeKm != s_coRange) {
        const bool firstFix = (s_coRange < 0.0f);
        s_coLat = s.homeLat; s_coLon = s.homeLon; s_coRange = s.rangeKm;
        coastline_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        airports_project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        roads_sd::project(s.homeLat, s.homeLon, s.rangeKm, s_cx, s_cy, R);
        if (s_gridLayer) lv_obj_invalidate(s_gridLayer);
        // The projection is new, so the etched copy is stale: render it once at the new
        // geometry and fold it back into the flattened background. Costs one frame's
        // worth of work on an event a person triggers rarely (move home, zoom range),
        // and buys back the per-frame re-vectoring the rest of the time.
        if (customStyled()) {
            take_map_snapshot();
            rebuild_flat_background();
            apply_grid_visibility();
        }
        if (!firstFix) {
            // Scope scale/center changed: old trails were plotted at the previous
            // projection and would be wrong now — drop them and clear the flow layer.
            s_trails.clear();
            s_flow.clear();
            flow_redraw_all();
        }
    }

    std::map<std::string, lv_point_t> prevPos;        // smooth-motion: glide starts here
    for (const AcDraw &a : s_acs) prevPos[a.hex] = a.pos;

    for (const Aircraft &ac : aircraft) {
        const double distKm = geo::haversineKm(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const double brg = geo::bearingDeg(s.homeLat, s.homeLon, ac.lat, ac.lon);
        const geo::Point p = geo::projectToScreen(distKm, brg, s.rangeKm, s_cx, s_cy, R, s.rotationDeg);

        AcDraw d;
        lv_point_t target;
        target.x = (lv_coord_t)lroundf(p.x);
        target.y = (lv_coord_t)lroundf(p.y);
        d.to = target;
        {
            auto pit = prevPos.find(std::string(ac.hex.c_str()));
            if (pit != prevPos.end()) {
                const long dx = (long)target.x - pit->second.x;
                const long dy = (long)target.y - pit->second.y;
                d.from = (dx * dx + dy * dy > 120L * 120L) ? target : pit->second;  // snap if it jumped
            } else d.from = target;                                                  // new contact: appear in place
        }
#if MOTION_INTERP
        // A custom design's plate/overlay/text-canvas layers are all PSRAM-backed
        // alpha-composited images (unlike the built-in themes' plain vector draws),
        // so every ~90ms interpolation step's invalidation forces a much more
        // expensive recomposite. Snap straight to the polled position instead of
        // gliding — one redraw per ~2s poll rather than ~22 in between — since a
        // custom design already trades continuous smoothness for that heavier look.
        if (customStyled()) { d.pos = target; d.from = target; }
        else                 d.pos = d.from;   // begin the glide at the previous position
#else
        d.pos = target;
        d.from = target;
#endif
        d.inRange = p.inRange;
        d.track = ac.track;
        d.color = alt_color(ac.altBaro, ac.onGround);
        d.emergency = acIsEmergency(ac.squawk);
        snprintf(d.hex,  sizeof(d.hex),  "%s", ac.hex.c_str());
        snprintf(d.call, sizeof(d.call), "%s", ac.flight.c_str());
        snprintf(d.type, sizeof(d.type), "%s", ac.type.c_str());
        d.altFt = ac.altBaro;
        d.onGround = ac.onGround;
        d.vsFpm = ac.baroRate;
        d.gsKt = ac.gs;
        d.distKm = (float)distKm;
        d.bearingDeg = (float)brg;
        d.squawk = ac.squawk;
        if (ac.onGround) snprintf(d.altTxt, sizeof(d.altTxt), "GND");
        else             snprintf(d.altTxt, sizeof(d.altTxt), "%.0f ft", (double)ac.altBaro);

        const std::string key = ac.hex.c_str();
        present.insert(key);
        if (d.inRange) {
            std::vector<lv_point_t> &hist = s_trails[key];
            const bool moved = hist.empty() ||
                               abs((int)hist.back().x - (int)target.x) > 0 ||
                               abs((int)hist.back().y - (int)target.y) > 0;
            if (moved) {
                // Flow segments persist on their canvas once painted, so a segment laid
                // down inside a zone would sit on the artwork for its entire lifetime.
                // Tested here, at creation, rather than at draw time.
                if (s_flowMax > 0 && !hist.empty() &&
                    !in_excluded_zone(hist.back().x, hist.back().y) &&
                    !in_excluded_zone(target.x, target.y)) {
                    FlowSeg seg = { hist.back(), target, s_flowGen };
                    s_flow.push_back(seg);
                    while ((int)s_flow.size() > s_flowMax) s_flow.pop_front();
                    flow_draw_seg(seg);
                }
                if (s_trailMax > 0) {
                    hist.push_back(target);
                    while ((int)hist.size() > s_trailMax) hist.erase(hist.begin());
                } else {
                    hist.clear();
                }
            }
            d.trail = hist;
        }
        out.push_back(std::move(d));
    }

    for (auto it = s_trails.begin(); it != s_trails.end();) {
        if (present.find(it->first) == present.end()) it = s_trails.erase(it);
        else ++it;
    }
    if (!s_selHex.empty() && present.find(s_selHex) == present.end()) s_selHex.clear();

    // Fade the flow layer by AGE, not just count: drop segments older than s_flowGenMax
    // polls so old tracks self-clear even in busy airspace (a 5 nm view doesn't stay caked
    // in green). If any were dropped, repaint the flow canvas so they actually disappear.
    if (s_flowGenMax > 0 && !s_flow.empty()) {
        // Expire old segments, but do NOT repaint on every prune.
        //
        // The flow canvas is additive, so removing a segment means clearing and redrawing
        // every remaining one. That made one or two segments ageing out cost a full
        // repaint of up to 700, measured at 210-330 ms ON THE RENDER THREAD — three-plus
        // dropped frames, every poll, which is precisely the periodic stutter in the
        // sweep. The work was wildly disproportionate to the change: repaint everything
        // to remove two.
        //
        // So expiry is batched. Segments linger a little past their age limit until
        // enough have accumulated to be worth one repaint. A trail tail fading a beat
        // late is invisible; the sweep hitching is not, and Zion's stated priority is
        // explicit that even motion wins.
        size_t expired = 0;
        while (expired < s_flow.size() &&
               (uint16_t)(s_flowGen - s_flow[expired].gen) > (uint16_t)s_flowGenMax) {
            ++expired;
        }
        const size_t batch = s_flow.size() / 6 > 12 ? s_flow.size() / 6 : 12;
        // Repaint when a worthwhile batch has expired, or when everything has (the tail
        // of a fade-out, where waiting for a batch that will never arrive would strand
        // the last few segments on screen).
        if (expired >= batch || (expired > 0 && expired == s_flow.size())) {
            s_flow.erase(s_flow.begin(), s_flow.begin() + expired);
            flow_redraw_all();
        }
    }

    // Which aircraft the scope follows, and it is deliberately STICKY.
    //
    // This used to be "sort by distance, keep the nearest N", recomputed every poll. With
    // a cap of five over a busy city that set churns constantly: two aircraft trade places
    // by a kilometre and the scope drops one and adopts another on the far side of the
    // dial. It reads as the instrument losing its mind rather than tracking anything.
    //
    // So a contact keeps its slot for as long as it stays trackable, and a slot only opens
    // when the aircraft in it leaves the ring or lands. Free slots are then filled by the
    // nearest untracked contact. The instrument follows aircraft instead of re-deciding
    // what is interesting twice a second.
    std::sort(out.begin(), out.end(),
              [](const AcDraw &a, const AcDraw &b) { return a.distKm < b.distKm; });
    if ((int)out.size() > s_maxOnScreen) {
        // Trackable means on the scope and flying. Ground traffic is never worth a slot,
        // and the feed already drops it when hide-ground or a minimum altitude is set —
        // this is the backstop for when neither is.
        auto trackable = [](const AcDraw &a) { return a.inRange && !a.onGround; };

        std::vector<AcDraw> kept;
        kept.reserve(s_maxOnScreen);
        for (const AcDraw &a : out) {                       // incumbents first, nearest first
            if ((int)kept.size() >= s_maxOnScreen) break;
            if (!trackable(a)) continue;
            if (s_tracked.find(std::string(a.hex)) != s_tracked.end()) kept.push_back(a);
        }
        for (const AcDraw &a : out) {                       // then backfill any free slots
            if ((int)kept.size() >= s_maxOnScreen) break;
            if (!trackable(a)) continue;
            if (s_tracked.find(std::string(a.hex)) == s_tracked.end()) kept.push_back(a);
        }
        // Only if nothing qualified: better to show distant or grounded contacts than an
        // empty scope, which would look broken rather than quiet.
        if (kept.empty()) { out.resize(s_maxOnScreen); }
        else              { out.swap(kept); }
    }
    s_tracked.clear();
    for (const AcDraw &a : out) s_tracked.insert(std::string(a.hex));

    if (++s_flowRedrawCtr >= FLOW_REDRAW_EVERY) {
        s_flowRedrawCtr = 0;
        flow_redraw_all();
    }

    if (s_rangeLbl) {                                 // keep the range label in sync with settings
        char r[16];
        snprintf(r, sizeof(r), "%.0f km", (double)s.rangeKm);
        lv_label_set_text(s_rangeLbl, r);
    }

    const uint32_t now = lv_tick_get();              // measure actual cadence for the glide clock
    s_pollMs = (s_lastUpdateMs && now > s_lastUpdateMs) ? (now - s_lastUpdateMs) : (uint32_t)POLL_INTERVAL_MS;
    if (s_pollMs < 400)  s_pollMs = 400;
    if (s_pollMs > 8000) s_pollMs = 8000;
    s_lastUpdateMs = now;
    s_animStartMs  = now;

    s_acs = std::move(out);
    // The scope has real content now: the projection and etch above are done and this
    // snapshot is live. Dismissing here rather than on a timer means the notice lasts
    // exactly as long as the wait actually lasts.
    if (s_loadingPending) {
        s_loadingPending = false;
        if (s_loading) show(s_loading, false);
        // Mirrors refreshCustomStyle's condition for the image sweep: it is visible only
        // when a custom design asks for the image type AND actually ships the sprite.
        if (s_sweepImg && customStyled() && theme_style::radar().sweepTypeImage && radar_custom_sweep())
            show(s_sweepImg, true);
        if (s_sweep) lv_obj_invalidate(s_sweep);
    }
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
    refresh_custom_text();   // live values (alt/spd/dist/...) for the current selection, if any, just changed
}

int hitTest(int x, int y) {
    int best = -1;
    long bestD = (long)TAP_RADIUS_PX * TAP_RADIUS_PX;
    const bool drg = orb();
    int balls = 0, arrows = 0;
    for (size_t i = 0; i < s_acs.size(); ++i) {
        if (drg) {
            if (s_acs[i].inRange) { if (balls >= ORB_BLIPS) continue; balls++; }
            else { if (arrows >= ORB_ARROWS) continue; arrows++; }
        } else if (!s_acs[i].inRange) continue;
        const long dx = (long)s_acs[i].pos.x - x;
        const long dy = (long)s_acs[i].pos.y - y;
        const long dd = dx * dx + dy * dy;
        if (dd <= bestD) { bestD = dd; best = (int)i; }
    }
    return best;
}

static void fill_info(const AcDraw &a, AcInfo &out) {
    snprintf(out.hex, sizeof(out.hex), "%s", a.hex);
    snprintf(out.call, sizeof(out.call), "%s", a.call);
    snprintf(out.type, sizeof(out.type), "%s", a.type);
    out.altFt = a.altFt; out.onGround = a.onGround;
    out.vsFpm = a.vsFpm; out.gsKt = a.gsKt;
    out.distKm = a.distKm; out.bearingDeg = a.bearingDeg;
    out.squawk = a.squawk; out.emergency = a.emergency;
}

#if CUSTOM_HAS_RADAR
// Substitute {token} placeholders against one aircraft's live info — the exact
// same token set the Launch Kit editor's own radarFmt()/radarTokens() use, so a
// format string written there means the same thing here. {from}/{to} come from
// the same async route lookup the native detail card already uses.
struct RadarTok { const char *key; const char *val; };
// Shared {token} substitution engine — radar_fmt() (aircraft tokens) and
// radar_range_fmt() (the scope-range banner's {range}, which isn't
// aircraft-dependent at all) both just supply a different token table.
static void radar_fmt_toks(char *out, size_t outSz, const char *fmt, const RadarTok *toks, size_t nToks) {
    size_t oi = 0;
    for (const char *p = fmt; *p && oi + 1 < outSz; ) {
        if (*p == '{') {
            const char *close = strchr(p, '}');
            if (close) {
                char key[16]; size_t klen = (size_t)(close - p - 1);
                if (klen > 0 && klen < sizeof(key)) {
                    memcpy(key, p + 1, klen); key[klen] = 0;
                    const char *val = "";
                    for (size_t i = 0; i < nToks; ++i) if (!strcmp(toks[i].key, key)) { val = toks[i].val; break; }
                    for (const char *v = val; *v && oi + 1 < outSz; ++v) out[oi++] = *v;
                    p = close + 1;
                    continue;
                }
            }
        }
        out[oi++] = *p++;
    }
    out[oi] = 0;
}
// Returns false (leaves `out` empty) when the format needs {from}/{to} and the
// route lookup hasn't produced both yet — the caller skips drawing that banner
// entirely rather than showing a "?" placeholder while it's still resolving (or
// blank/half-blank if this particular callsign genuinely has no route on file).
static bool radar_fmt(char *out, size_t outSz, const char *fmt, const AcInfo &in) {
    char altS[16], spdS[16], distS[16], hdgS[8], sqkS[8];
    snprintf(altS, sizeof(altS), "%.0f", (double)in.altFt);
    snprintf(spdS, sizeof(spdS), "%.0f", (double)(isnan(in.gsKt) ? 0.0f : in.gsKt));
    snprintf(distS, sizeof(distS), "%.1f", (double)in.distKm);
    snprintf(hdgS, sizeof(hdgS), "%.0f", (double)in.bearingDeg);
    if (in.squawk < 0) snprintf(sqkS, sizeof(sqkS), "-");
    else                snprintf(sqkS, sizeof(sqkS), "%04d", in.squawk);
    const bool needsRoute = strstr(fmt, "{from}") || strstr(fmt, "{to}");
    char rfrom[40] = "", rto[40] = "";
    if (needsRoute && in.call[0]) {
        route_request(in.call);
        route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto));
    }
    if (needsRoute && (!rfrom[0] || !rto[0])) { if (outSz) out[0] = 0; return false; }
    RadarTok toks[] = {
        { "callsign", in.call[0] ? in.call : "-" }, { "type", in.type },
        { "alt", altS }, { "spd", spdS }, { "dist", distS }, { "hdg", hdgS }, { "sqk", sqkS },
        { "from", rfrom }, { "to", rto },
    };
    radar_fmt_toks(out, outSz, fmt, toks, sizeof(toks) / sizeof(toks[0]));
    return true;
}
// The scope-range banner: describes the radar's own configured radius (the
// Range slider), not a selected aircraft — always shown when CUSTOM_HAS_RTEXT4,
// regardless of selection. s_lastRangeKm (declared near the other statics
// above) is kept current by update().
static void radar_range_fmt(char *out, size_t outSz, const char *fmt) {
    char rangeS[16]; snprintf(rangeS, sizeof(rangeS), "%.0f", (double)s_lastRangeKm);
    RadarTok toks[] = { { "range", rangeS } };
    radar_fmt_toks(out, outSz, fmt, toks, 1);
}
// Selection banners render into their own transparent canvas (s_textCanvas),
// not LVGL labels — that's what lets a banner curve along an arc (LVGL has no
// curved-text primitive) and glow (canvas shadowBlur isn't a firmware effect
// either), the same way clock_view.cpp's draw_baked_arc_text/draw_baked_text
// work on the clock's own raster. Read a 4-bpp (16-level) glyph alpha bitmap
// (as lv_font_conv --bpp 4 --no-compress emits it, same as the builtin fonts):
// continuous bitstream, MSB-first, box_w px per row, no row padding.
static inline float rtext_glyph_alpha4(const uint8_t *bmp, int bw, int x, int y) {
    const int bit = (y * bw + x) * 4;
    const uint8_t byte = bmp[bit >> 3];
    const uint8_t nib = (bit & 4) ? (byte & 0x0F) : (byte >> 4);
    return nib * 17.0f;
}
// Rotate one glyph's alpha bitmap around its own centre by angleDeg and blend it
// into s_textCanvas's raw buffer (RGB565+alpha, 3 B/px, same layout custom_sprite/
// office_sprite already use) in a solid colour, box centred at (destCx,destCy).
// Composites by "higher opacity wins" per pixel rather than true alpha-over
// (lv_color_mix against the canvas's own prior content) — cheap, and correct
// for back-to-front painter's order: glow rings (drawn first, lower opacity)
// never dim a sharper pass already there, and the crisp glyph fill (drawn last,
// full opacity) always dominates its own footprint.
static void rtext_blit_glyph(const uint8_t *bmp, int bw, int bh, float destCx, float destCy,
                             float angleDeg, lv_color_t col, lv_opa_t maxOpa) {
    if (!bmp || bw <= 0 || bh <= 0 || !s_textBuf) return;
    uint8_t *buf = (uint8_t *)s_textBuf;
    const float th = angleDeg * (float)M_PI / 180.0f, ct = cosf(th), st = sinf(th);
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
            const float a00 = (ix >= 0 && iy >= 0 && ix < bw && iy < bh) ? rtext_glyph_alpha4(bmp, bw, ix, iy) : 0.0f;
            const float a10 = (ix + 1 >= 0 && iy >= 0 && ix + 1 < bw && iy < bh) ? rtext_glyph_alpha4(bmp, bw, ix + 1, iy) : 0.0f;
            const float a01 = (ix >= 0 && iy + 1 >= 0 && ix < bw && iy + 1 < bh) ? rtext_glyph_alpha4(bmp, bw, ix, iy + 1) : 0.0f;
            const float a11 = (ix + 1 >= 0 && iy + 1 >= 0 && ix + 1 < bw && iy + 1 < bh) ? rtext_glyph_alpha4(bmp, bw, ix + 1, iy + 1) : 0.0f;
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
// Glow: the same glyph blitted at a ring of offset positions (relative to the
// already-rotated destCx/destCy) at falling opacity — the same 3-ring/8-direction
// technique clock_view.cpp's draw_baked_text glow uses for its straight banners.
static void rtext_blit_glyph_glow(const uint8_t *bmp, int bw, int bh, float destCx, float destCy,
                                  float angleDeg, lv_color_t glowCol, int glow) {
    if (glow <= 0) return;
    static const float dirs[8][2] = { {1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f} };
    const int rings = 3;
    for (int ri = 1; ri <= rings; ++ri) {
        const int r = (int)lroundf((float)glow * ri / rings);
        if (r <= 0) continue;
        const lv_opa_t opa = (lv_opa_t)(90 / ri);
        for (int di = 0; di < 8; ++di)
            rtext_blit_glyph(bmp, bw, bh, destCx + dirs[di][0] * r, destCy + dirs[di][1] * r, angleDeg, glowCol, opa);
    }
}
// Straight layout: glyphs left-to-right from an aligned start X (a digit that's
// narrower/wider than its predecessor only pushes the tail end right, so a live
// value never wobbles), baseline vertically centred on `by` — mirrors the
// editor's textBaseline "middle". align: 0 left (bx is the start), 1 center,
// 2 right. Mirrors clock_view.cpp's draw_baked_text geometry.
static void rtext_draw_straight(const lv_font_t *font, const char *str, float bx, float by,
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
    float x = startX;
    for (int i = 0; i < cap; ++i) {
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) {
            const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
            if (bmp && g.box_w && g.box_h) {
                const float destCx = x + (float)g.ofs_x + (float)g.box_w * 0.5f;
                const float destCy = by + halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
                rtext_blit_glyph_glow(bmp, g.box_w, g.box_h, destCx, destCy, 0.0f, glowCol, glow);
                rtext_blit_glyph(bmp, g.box_w, g.box_h, destCx, destCy, 0.0f, col, 255);
            }
        }
        x += w[i];
    }
}
// Curved layout: lay each glyph along an arc of radius R centred on arcDeg (a
// clock angle, 0 = 12 o'clock), advancing by the glyph's own width AND tilting
// each glyph tangent to the arc — the exact geometry as the editor's
// drawCurvedText and clock_view.cpp's draw_baked_arc_text.
static void rtext_draw_curved(const lv_font_t *font, const char *str, float R, float arcDeg,
                              lv_color_t col, int glow, lv_color_t glowCol) {
    if (!font || !str || !str[0] || R < 1.0f) return;
    const int n = (int)strlen(str), cap = n < 80 ? n : 80;
    float w[80], total = 0.0f;
    for (int i = 0; i < cap; ++i) {
        char c[2] = { str[i], 0 }; lv_point_t s;
        lv_txt_get_size(&s, c, font, 0, 0, LV_COORD_MAX, 0);
        w[i] = (float)s.x; total += s.x;
    }
    const float norm = fmodf(fmodf(arcDeg, 360.0f) + 360.0f, 360.0f);
    const bool bottom = (norm > 90.0f && norm < 270.0f);
    const float dir = bottom ? -1.0f : 1.0f;
    const float base = arcDeg * (float)M_PI / 180.0f;
    const float lineH = (float)lv_font_get_line_height(font), desc = (float)font->base_line;
    const float halfMid = (lineH - 2.0f * desc) * 0.5f;
    float cursor = -total / 2.0f;
    for (int i = 0; i < cap; ++i) {
        const float mid = cursor + w[i] / 2.0f, ang = base + dir * mid / R;
        const float ax = (float)s_cx + sinf(ang) * R, ay = (float)s_cy - cosf(ang) * R;
        const float rot = ang + (bottom ? (float)M_PI : 0.0f);
        cursor += w[i];
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, (uint32_t)(uint8_t)str[i], 0)) continue;
        const uint8_t *bmp = lv_font_get_glyph_bitmap(font, (uint32_t)(uint8_t)str[i]);
        if (!bmp || g.box_w == 0 || g.box_h == 0) continue;
        const float offY = halfMid - (float)g.ofs_y - (float)g.box_h * 0.5f;
        const float cr = cosf(rot), sr = sinf(rot);
        const float destCx = ax - offY * sr, destCy = ay + offY * cr;
        const float rotDeg = rot * 180.0f / (float)M_PI;
        rtext_blit_glyph_glow(bmp, g.box_w, g.box_h, destCx, destCy, rotDeg, glowCol, glow);
        rtext_blit_glyph(bmp, g.box_w, g.box_h, destCx, destCy, rotDeg, col, 255);
    }
}
// Refresh the 4 selection banners for whatever's currently selected — the
// canvas is cleared fully transparent when nothing is, so a design with no
// aircraft picked shows a clean scope, matching the editor's own show/hide.
static void refresh_custom_text() {
    if (!s_textCanvas) return;
    AcInfo in;
    const bool have = selected(in);
    const theme_style::Radar &rs = theme_style::radar();
    // `show` is the gate now, not CUSTOM_HAS_RTEXT{n}.
    //
    // Those macros are baked in by whichever Launch Kit push last compiled the firmware,
    // so a theme installed as FILES ALONE — which is every theme Orb Studio makes — could
    // ship a selection line and have the Orb refuse to draw it, for no reason it could see
    // or state. Exactly the bug the clock's own text1/text2 had (see clock_view.cpp), and
    // exactly the same fix: the theme decides, at runtime.
    bool need = false;
    if (have) for (int i = 0; i < 3; ++i) if (rs.rtext[i].show) need = true;
    if (rs.rtext[3].show) need = true;   // the range banner is scope-wide, selection or not
    // The card, and where it sits. Placed before the text so the banners riding it have a
    // centre to be measured from.
    //
    // Always 180 degrees from the selected aircraft's own bearing: the thing just picked is
    // never underneath the words describing it, however the traffic moves. Recomputed on
    // every refresh, which is also every position update, so the card chases the far side
    // of the dial live rather than being placed once and left there.
    float cardCx = (float)s_cx, cardCy = (float)s_cy;
    const bool cardOn = rs.card.enabled && have;
    if (cardOn) {
        const float opp = (in.bearingDeg + 180.0f) * (float)M_PI / 180.0f;
        cardCx = (float)s_cx + sinf(opp) * (float)rs.card.radius;
        cardCy = (float)s_cy - cosf(opp) * (float)rs.card.radius;
    }
    if (s_cardImg && s_cardObj) {
        const lv_img_dsc_t *art = (cardOn && rs.card.typeImage) ? radar_custom_card() : nullptr;
        if (art) {
            lv_img_set_src(s_cardImg, art);
            lv_obj_set_pos(s_cardImg, (lv_coord_t)lroundf(cardCx - art->header.w / 2.0f),
                                      (lv_coord_t)lroundf(cardCy - art->header.h / 2.0f));
            lv_obj_set_style_img_opa(s_cardImg, (lv_opa_t)rs.card.opacity, 0);
            show(s_cardImg, true);
            show(s_cardObj, false);
        } else if (cardOn) {
            // Drawn card, or an image card whose art failed to decode: a plate is better
            // than words floating over the scope with nothing behind them.
            lv_obj_set_size(s_cardObj, (lv_coord_t)rs.card.w, (lv_coord_t)rs.card.h);
            lv_obj_set_pos(s_cardObj, (lv_coord_t)lroundf(cardCx - rs.card.w / 2.0f),
                                      (lv_coord_t)lroundf(cardCy - rs.card.h / 2.0f));
            lv_obj_set_style_bg_color(s_cardObj, lv_color_hex(rs.card.color), 0);
            lv_obj_set_style_bg_opa(s_cardObj, (lv_opa_t)rs.card.opacity, 0);
            lv_obj_set_style_radius(s_cardObj, (lv_coord_t)rs.card.corner, 0);
            lv_obj_set_style_border_color(s_cardObj, lv_color_hex(rs.card.borderColor), 0);
            lv_obj_set_style_border_width(s_cardObj, (lv_coord_t)rs.card.borderWidth, 0);
            lv_obj_set_style_border_opa(s_cardObj, LV_OPA_COVER, 0);
            show(s_cardObj, true);
            show(s_cardImg, false);
        } else {
            show(s_cardObj, false);
            show(s_cardImg, false);
        }
    }
    if (!need) { canvas_release(s_textCanvas, s_textBuf); return; }
    if (!canvas_acquire(s_textCanvas, s_textBuf, "text")) return;
    lv_canvas_fill_bg(s_textCanvas, lv_color_black(), LV_OPA_TRANSP);
    // CUSTOM_HAS_RTEXT{n} (whether this banner exists at all) and each FONT stay
    // compile-time (see theme_style.h); position/color/glow/format/align/curve now
    // follow the active SD theme.
    if (have) {
        for (int i = 0; i < 3; ++i) {
            const theme_style::RadarText &t = rs.rtext[i];
            if (!t.show) continue;
            char buf[64];
            if (!radar_fmt(buf, sizeof(buf), t.fmt, in)) continue;
            if (t.curved) { rtext_draw_curved(theme_font::radar_text(i), buf, (float)t.curveR, t.arcDeg, lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor)); continue; }
            // A line riding the card reads x/y as an offset from the card's own centre, so
            // it travels with it. Only when a card is actually showing: a line pinned to a
            // card that is switched off would otherwise land at an offset from the middle
            // of the scope, which is not where anyone put it.
            const float lx = (t.onCard && cardOn) ? cardCx + (float)(t.x - SCREEN_W / 2) : (float)t.x;
            const float ly = (t.onCard && cardOn) ? cardCy + (float)(t.y - SCREEN_H / 2) : (float)t.y;
            rtext_draw_straight(theme_font::radar_text(i), buf, lx, ly, lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor), t.align);
        }
    }
    // The range banner describes the scope itself (its configured radius), not a
    // selected aircraft, so it's outside the `have` gate above — it stays on the
    // whole time a custom design is active, matching the editor's own preview.
    if (rs.rtext[3].show) {
      const theme_style::RadarText &t = rs.rtext[3];
      char buf[64]; radar_range_fmt(buf, sizeof(buf), t.fmt);
      if (t.curved) rtext_draw_curved(theme_font::radar_text(3), buf, (float)t.curveR, t.arcDeg, lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor));
      else rtext_draw_straight(theme_font::radar_text(3), buf, (float)t.x, (float)t.y, lv_color_hex(t.color), t.glow, lv_color_hex(t.glowColor), t.align);
    }
    lv_obj_invalidate(s_textCanvas);
}
#else
static void refresh_custom_text() {}
#endif

void select(int idx) {
    if (idx < 0 || idx >= (int)s_acs.size()) s_selHex.clear();
    else s_selHex = s_acs[idx].hex;
    if (s_acLayer) lv_obj_invalidate(s_acLayer);
    refresh_custom_text();
}

// Cycle through in-range aircraft, with an explicit "none selected" stop at
// position 0 so turning wraps all the way back around to it. Builds the
// in-range list fresh each call (aircraft come and go every poll), finds where
// the current selection sits in it (or the "none" stop if nothing/no-longer
// in range), and steps by dir with wraparound.
void selectNext(int dir) {
    std::vector<int> inRangeIdx;
    // Masked aircraft are excluded from the knob's cycle: landing a selection on a
    // contact the person cannot see reads as the knob doing nothing.
    for (int i = 0; i < (int)s_acs.size(); ++i)
        if (s_acs[i].inRange && !ac_masked(s_acs[i])) inRangeIdx.push_back(i);
    const int n = (int)inRangeIdx.size();
    if (n == 0) { select(-1); return; }
    int pos = 0;   // 0 = "none"; 1..n = inRangeIdx[pos-1]
    if (!s_selHex.empty()) {
        for (int k = 0; k < n; ++k) if (s_acs[inRangeIdx[k]].hex == s_selHex) { pos = k + 1; break; }
    }
    pos = ((pos + dir) % (n + 1) + (n + 1)) % (n + 1);
    select(pos == 0 ? -1 : inRangeIdx[pos - 1]);
}

// --- Flight Tracker knob handlers (wired identically by the device + simulator) ---

// onEnter tail: re-attach the pushed plate/overlay (freed on the last onExit) and
// land in the DEFAULT VIEW — nothing selected, knob released, so a turn opens the
// app switcher. A push is what enters selection mode (knobPress below). Unconditional
// now that knobPress() always supports selection mode (not gated on CUSTOM_HAS_RADAR
// any more) — this reset has to run every entry regardless, or stale selection state
// from a prior visit could leak through in a stock (no custom design) build.
void knobEnter() {
    refreshCustomStyle();
    s_selectMode = false;
    select(-1);
    app_shell::setCaptured(false);
    // Only when there is actually nothing on the scope. Coming back to a Flight Tracker
    // that still holds its last snapshot has nothing to wait for, and flashing a loading
    // notice over a working display would be its own kind of lie.
    if (s_acs.empty()) {
        s_loadingPending = true;
        if (s_loading) { show(s_loading, true); lv_obj_move_foreground(s_loading); }
        if (s_sweepImg) show(s_sweepImg, false);
        if (s_sweep)    lv_obj_invalidate(s_sweep);   // clear the vector wedge's last frame
    }
}

// Push: toggle selection. From the default view it grabs the knob and selects the
// first in-range aircraft (nothing to select -> stays in the default view). From
// selection mode it drops straight back to the default view (the manual version of
// the 5s idle timeout). Aircraft selection doesn't depend on a custom design being
// active — it used to be stock-only-vs-cycle-the-scope-skin here, but that legacy
// theme-cycle gesture was a hidden, undiscoverable knob-press with no Settings entry
// at all, confusingly named the same as actual Launch Kit themes. Retired in favor of
// the real Settings "Design" picker (theme_select) — see its header for why.
void knobPress() {
    if (s_selectMode) { radar_exit_select(); return; }
    if (countInRange() <= 0) return;
    selectNext(1);
    s_selectMode = true;
    s_selActivityMs = lv_tick_get();
    app_shell::setCaptured(true);
}

// Turn (only reaches here while captured, i.e. in selection mode): step to the
// next/previous aircraft and restart the 5s idle countdown.
void knobTurn(int dir) {
    selectNext(dir > 0 ? 1 : -1);
    s_selActivityMs = lv_tick_get();
}

// onExit: free the decoded plate/overlay PSRAM and drop selection mode so the idle
// timer can't fire against a scope that's no longer on screen.
void knobExit() {
    radar_sprite_release();
    s_selectMode = false;
    s_loadingPending = false;
    if (s_loading) show(s_loading, false);   // never leave it stranded over another app
}

bool selected(AcInfo &out) {
    if (s_selHex.empty()) return false;
    for (const AcDraw &a : s_acs)
        if (s_selHex == a.hex) { fill_info(a, out); return true; }
    return false;
}

int count() { return (int)s_acs.size(); }

int countInRange() {
    int n = 0;
    for (const AcDraw &a : s_acs) if (a.inRange) ++n;
    return n;
}

bool info(int idx, AcInfo &out) {
    if (idx < 0 || idx >= (int)s_acs.size()) return false;
    fill_info(s_acs[idx], out);
    return true;
}

void tickSweep() { /* sweep self-animates via lv_timer */ }

} // namespace radar
