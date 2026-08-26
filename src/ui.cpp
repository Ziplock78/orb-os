// M3 UI: tileview (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
#include "ui.h"
#include "app_theme.h"
#include "radar_view.h"
#include "custom_radar.h"     // CUSTOM_HAS_RADAR_STYLE — a pushed design's own banners replace this card
#include "route.h"
#include "photo.h"
#include "weather.h"
#include "wx_radar.h"
#include "cloud_image.h"
#include "airports.h"
#include "config.h"
#include "splash_art.h"       // splash_art_decode() — boot-splash PNG, decoded on demand
#include "splash_lines.h"     // the three standing lines, and the glass over them
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

// Runtime-retintable HUD chrome — plain variables (not #define) so ui_apply_theme()
// can repaint every existing UI_* call site below without touching each one.
static lv_color_t UI_GREEN = lv_color_hex(0x1DFF86);
static lv_color_t UI_INK   = lv_color_hex(0xEAFFF3);
static lv_color_t UI_SOFT  = lv_color_hex(0x9AFFC8);
static lv_color_t UI_DIM   = lv_color_hex(0x5F7A6C);
static lv_color_t UI_PANEL = lv_color_hex(0x0C160F);
static lv_color_t UI_EMERG = lv_color_hex(0xFF5A3C);
static lv_color_t UI_BG    = lv_color_hex(0x000000);   // screen background — was a bare lv_color_black() everywhere

// Repaint the HUD chrome for the active radar theme. Aviator gets a warm ivory/brass
// palette to match the clock faces + Location dial; every other theme keeps the
// original phosphor-green HUD regardless of the scope's own accent color, since only
// Aviator has a full matching palette designed for it. The Office app theme (see
// app_theme.h) overrides all of this with a white/charcoal/blue skin regardless of the
// radar scope theme underneath — it's a whole-device switch, not a per-scope one.
void ui_apply_theme(int theme) {
    if (app_theme::get() == APP_THEME_OFFICE) {
        const AppPalette &p = app_theme::palette();
        UI_GREEN = p.accent; UI_INK = p.ink; UI_SOFT = p.soft; UI_DIM = p.dim;
        UI_PANEL = p.panel; UI_EMERG = lv_color_hex(0xD1382A); UI_BG = p.bg;
        return;
    }
    UI_BG = lv_color_hex(0x000000);
    if (theme == THEME_AVIATOR) {
        UI_GREEN = lv_color_hex(0xDACFA6); UI_INK  = lv_color_hex(0xEDE3CC);
        UI_SOFT  = lv_color_hex(0x9C8F73); UI_DIM  = lv_color_hex(0x6B5A3A);
        UI_PANEL = lv_color_hex(0x14100A); UI_EMERG = lv_color_hex(0xB0402C);
    } else {
        UI_GREEN = lv_color_hex(0x1DFF86); UI_INK  = lv_color_hex(0xEAFFF3);
        UI_SOFT  = lv_color_hex(0x9AFFC8); UI_DIM  = lv_color_hex(0x5F7A6C);
        UI_PANEL = lv_color_hex(0x0C160F); UI_EMERG = lv_color_hex(0xFF5A3C);
    }
}

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileWeather = nullptr;
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardL = nullptr, *s_cardR = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static lv_obj_t *s_photo = nullptr, *s_photoCredit = nullptr;   // aircraft photo above the card
static char s_lastRouteReq[12] = "";
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_hudBars[4] = { nullptr, nullptr, nullptr, nullptr };   // WiFi signal-strength bars
static lv_obj_t *s_hudGps   = nullptr;   // HUD satellite icon (hidden unless GPS auto-location is on)
static lv_obj_t *s_weatherNow = nullptr, *s_weatherMeta = nullptr, *s_weatherDays = nullptr;
static lv_obj_t *s_wxCanvas = nullptr, *s_wxStatus = nullptr, *s_wxAirport = nullptr;
static lv_obj_t *s_wxFooter = nullptr, *s_wxMeta = nullptr, *s_wxAttrib = nullptr;
static lv_obj_t *s_wxRings[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_wxRingLbl[3] = { nullptr, nullptr, nullptr };

// "Push, wait, image refreshes" has a real lag (a fresh RainViewer fetch + roads
// reprojection + PNG decode), but the range label and rings update instantly since
// those are local — that mismatch reads as "the zoom didn't work" (live user feedback).
// This overlay covers the still-stale canvas with an animated "UPDATING..." as soon as
// the press lands, and clears itself the moment the frame actually meant for the new
// tier arrives (tracked by wx_radar_version()'s counter, not a timer, so it can't clear
// early or get stuck late regardless of how long the fetch actually takes).
static lv_obj_t *s_wxUpdateOverlay = nullptr;
static bool     s_wxUpdating = false;
static uint32_t s_wxLastVersion = 0;
static int      s_wxUpdateDots = 0;
static int      s_wxAnimSlot = 0;   // which frame of the loop is on screen right now
static lv_obj_t *s_wxNorth = nullptr, *s_wxCenter = nullptr, *s_wxRange = nullptr;
static lv_obj_t *s_weatherTitle = nullptr;
enum WeatherViewMode { WEATHER_RADAR, WEATHER_CLOUDS, WEATHER_FORECAST };
static WeatherViewMode s_weatherMode = WEATHER_RADAR;
static lv_obj_t *s_fcCurrent = nullptr, *s_fcCondition = nullptr, *s_fcUpdated = nullptr;
static lv_obj_t *s_fcMetricName[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcMetricValue[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDay[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayCondition[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayTemp[3] = { nullptr, nullptr, nullptr };
static lv_obj_t *s_fcDayRain[3] = { nullptr, nullptr, nullptr };

// --------------------------------------------------------------------- units
// 0 = Aviation (ft, kt, km) · 1 = Metric (m, km/h, km) · 2 = Imperial (ft, mph, mi).
// The feed gives altitude in ft, speed in kt, vertical speed in fpm, distance in km.
static int s_units = 0;
void ui_set_units(int u) { s_units = (u < 0 || u > 2) ? 0 : u; }

// Accessibility: "large text" swaps every font one-or-two steps up. The flag must be
// set BEFORE ui_create() — fonts are baked into the widgets at creation time (the web
// toggle saves to NVS and reboots, so it always takes effect through this path).
static bool s_bigText = false;
void ui_set_large_text(bool on) { s_bigText = on; }
static const lv_font_t *F12() { return s_bigText ? &lv_font_montserrat_16 : &lv_font_montserrat_12; }
static const lv_font_t *F14() { return s_bigText ? &lv_font_montserrat_18 : &lv_font_montserrat_14; }
static const lv_font_t *F16() { return s_bigText ? &lv_font_montserrat_20 : &lv_font_montserrat_16; }

static void fmt_alt(char *b, size_t n, float ft, bool gnd) {
    if (gnd)            snprintf(b, n, "GND");
    else if (s_units == 1) snprintf(b, n, "%.0f m",  ft * 0.3048f);
    else                snprintf(b, n, "%.0f ft", ft);
}
static void fmt_spd(char *b, size_t n, float kt) {
    if (kt != kt)          snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%.0f km/h", kt * 1.852f);
    else if (s_units == 2) snprintf(b, n, "%.0f mph",  kt * 1.15078f);
    else                   snprintf(b, n, "%.0f kt",   kt);
}
static void fmt_vs(char *b, size_t n, float fpm) {
    if (fpm != fpm)        snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%+.1f m/s", fpm * 0.00508f);
    else                   snprintf(b, n, "%+.0f fpm", fpm);
}
static float dist_val(float km) {
    if (s_units == 0) return km * 0.539957f;   // Aviation -> nautical miles
    if (s_units == 2) return km * 0.621371f;   // Imperial -> miles
    return km;                                   // Metric   -> km
}
static const char *dist_unit(void) { return s_units == 0 ? "nm" : (s_units == 2 ? "mi" : "km"); }

// Weather units are independent of s_units above: that preset also drives the aircraft
// ALT/SPD/DIST readout (and includes an Aviation nm/kt mode that makes no sense for a
// weather forecast), so weather gets its own metric/imperial flag instead of sharing it.
// Resolved on the host side (main.cpp) from either the saved manual choice or, in
// Automatic mode, the home location — see host_wx_units_set()/is_imperial_region().
static bool s_wxImperial = false;
void ui_set_wx_units(bool imperial) { s_wxImperial = imperial; }

static float weather_temp(float c) { return s_wxImperial ? c * 1.8f + 32.0f : c; }
static const char *weather_temp_unit(void) { return s_wxImperial ? "F" : "C"; }
static float weather_wind(float kmh) { return s_wxImperial ? kmh * 0.621371f : kmh; }
static const char *weather_wind_unit(void) { return s_wxImperial ? "mph" : "km/h"; }
static float wx_dist_val(float km) { return s_wxImperial ? km * 0.621371f : km; }
static const char *wx_dist_unit(void) { return s_wxImperial ? "MI" : "KM"; }

// Weather map zoom: 0=50mi 1=100mi. The actual fetch/crop math lives in
// wx_radar_client.cpp's WX_ZOOM[] table; this mirrors just the display radius (km) for
// the on-screen range label — keep the two in sync if the tiers ever change.
// ui_set_wx_zoom() itself is defined below, after build_weather().
static int s_wxZoom = 0;
static const float WX_ZOOM_KM[2] = { 80.4672f, 160.9344f };
static const char *cardinal(float deg) {
    static const char *p[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int i = ((int)(deg + 22.5f) / 45) & 7;
    return p[i];
}

// Fold Latin-1 accents / drop any other non-ASCII so the Montserrat font never hits a
// missing glyph (which renders as an empty box). Belt-and-suspenders for card text.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

// ----------------------------------------------------------------- detail card
static void refresh_card(void) {
#if CUSTOM_HAS_RADAR_STYLE
    // A pushed design's own selection banners (callsign/stats/route, styled and
    // positioned in the editor) fully replace this generic card — showing both
    // would double up the same info in two different looks on the same screen.
    if (s_card)        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
    if (s_photo)       lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
    if (s_photoCredit) lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
    return;
#endif
    AcInfo in;
    if (!radar::selected(in)) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (s_photo)       lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        s_lastRouteReq[0] = 0;
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    char title[40];
    if (in.type[0]) snprintf(title, sizeof(title), "%s  %s", in.call[0] ? in.call : "-", in.type);
    else            snprintf(title, sizeof(title), "%s", in.call[0] ? in.call : "-");
    fold_ascii(title);
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.emergency ? UI_EMERG : UI_INK, 0);

    char altS[16], vsS[24], spdS[16], sqS[16];
    fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_vs (vsS,  sizeof(vsS),  in.vsFpm);
    fmt_spd(spdS, sizeof(spdS), in.gsKt);
    if (in.squawk < 0)          snprintf(sqS, sizeof(sqS), "-");
    else                        snprintf(sqS, sizeof(sqS), "%04d", in.squawk);

    char left[96], right[96];
    snprintf(left,  sizeof(left),  "ALT  %s\nSPD  %s\nDIST %.1f %s", altS, spdS, dist_val(in.distKm), dist_unit());
    snprintf(right, sizeof(right), "V/S  %s\nHDG  %03.0f\nSQK  %s", vsS, in.bearingDeg, sqS);
    lv_label_set_text(s_cardL, left);
    lv_label_set_text(s_cardR, right);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call);
    }
    char rfrom[40], rto[40];
    if (!in.call[0]) {
        lv_label_set_text(s_cardRoute, "Route -");                 // no callsign -> nothing to look up
    } else if (route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) snprintf(rt, sizeof(rt), "%s -> %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        else                    snprintf(rt, sizeof(rt), "Route unavailable");
        fold_ascii(rt);
        lv_label_set_text(s_cardRoute, rt);
    } else {
        lv_label_set_text(s_cardRoute, "Looking up route...");     // pending: lookup in flight
    }

    // aircraft photo (planespotters), shown above the card when one is available
    if (in.hex[0]) photo_request(in.hex);
    int pw = 0, ph = 0; char pcred[40];
    if (s_photo && in.hex[0] && photo_get(in.hex, &pw, &ph, pcred, sizeof(pcred)) && pw > 0 && ph > 0) {
        int mw, mh;
        lv_color_t *pbuf = photo_buffer(&mw, &mh);
        lv_canvas_set_buffer(s_photo, pbuf, pw, ph, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_photo, pw, ph);
        lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -28 - ph / 2);   // sit lower: fill the band down to the card
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_photo);
        if (s_photoCredit) {
            char c[52];
            snprintf(c, sizeof(c), "Photo: %s", pcred[0] ? pcred : "planespotters.net");
            lv_label_set_text(s_photoCredit, c);
            lv_obj_align_to(s_photoCredit, s_photo, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_photo) {
        // No image to show yet: hide the canvas, but use the caption line to tell the
        // user what's happening — "Loading..." while the fetch is in flight, or a quiet
        // "No photo" once it finished without one. Unobtrusive (small, dim) but informative.
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) {
            const bool done = in.hex[0] && photo_done(in.hex);
            lv_label_set_text(s_photoCredit, done ? "No photo available" : "Loading photo...");
            lv_obj_align(s_photoCredit, LV_ALIGN_CENTER, 0, -104);   // where the photo would sit
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --------------------------------------------------------------------- input
// Nothing here any more. This file used to own the touch input surface: tap a plane to
// select it, tap the on-screen zoom button to change range, long-press to cycle the
// scope skin, swipe between radar / list / stats / weather. All of it went when the Orb
// became knob-only (docs/ARCHITECTURE.md).
//
// Where each of those lives now:
//   - selecting an aircraft -> radar::knobTurn()/knobPress(), via input_router
//   - changing range        -> Settings > Range (settings_view.cpp)
//   - cycling the skin      -> Settings > Design (theme_select)
//   - moving between apps   -> the knob and app_shell's switcher overlay
//   - the range readout     -> radar_view's own s_rangeLbl, re-enabled in ui_create()

// ----------------------------------------------------------------------- HUD
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    // bar count from RSSI (dBm): the weaker the signal, the fewer lit bars
    int level;
    if      (!wifiUp)     level = 0;
    else if (rssi >= -55) level = 4;   // excellent
    else if (rssi >= -67) level = 3;   // good
    else if (rssi >= -75) level = 2;   // ok
    else                  level = 1;   // weak (connected but marginal)
    // colour: red = no WiFi, amber = connected but feed stale (no fresh data), white = healthy
    const lv_color_t col = !wifiUp ? UI_EMERG : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
    for (int i = 0; i < 4; ++i) {
        if (!s_hudBars[i]) continue;
        lv_obj_set_style_bg_color(s_hudBars[i], col, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
    if (!s_hudBatt) return;
    if (!present || pct < 0) { lv_label_set_text(s_hudBatt, ""); return; }   // USB-only -> hide
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                      pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 35 ? LV_SYMBOL_BATTERY_2 :
                      pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s%d", charging ? LV_SYMBOL_CHARGE : "", sym, pct);
    lv_label_set_text(s_hudBatt, buf);
    lv_obj_set_style_text_color(s_hudBatt, (pct <= 15 && !charging) ? UI_EMERG : UI_INK, 0);
}

void ui_set_date(const char *date) {
    if (s_hudDate && date) lv_label_set_text(s_hudDate, date);
}

// GPS indicator. state: 0 = off / no module (hidden), 1 = acquiring (amber), 2 = fix (green).
// The fuller "GPS fix, N sats" line this also used to write lived on the Stats screen and
// went with it; the HUD icon carries the same information in less space.
void ui_set_gps(int state, int sats) {
    if (state <= 0) {                                 // hidden when GPS auto-location is off
        if (s_hudGps) lv_label_set_text(s_hudGps, "");
        return;
    }
    const bool fix = (state >= 2);
    const lv_color_t col = fix ? UI_GREEN : lv_color_hex(0xFFB23C);   // amber while acquiring
    if (s_hudGps) {
        char b[16];
        snprintf(b, sizeof(b), LV_SYMBOL_GPS "%d", sats);
        lv_label_set_text(s_hudGps, b);
        lv_obj_set_style_text_color(s_hudGps, col, 0);
    }
}

// The frame the loop is currently sitting on, from the newest (possibly still-filling)
// generation. s_wxAnimSlot is advanced by wx_anim_cb() at ~3fps; build_weather() and the
// animation tick both read through here so they always agree on what's shown.
static bool wx_anim_current(const uint16_t **px, uint32_t *ft) {
    const uint32_t gen = wx_radar_gen();
    const int n = wx_radar_gen_count(gen);
    if (n <= 0) return false;
    if (s_wxAnimSlot >= n) s_wxAnimSlot = 0;
    return wx_radar_frame(s_wxAnimSlot, gen, px, ft);
}

// Lean-redesign cadence: step to the next frame and repaint the canvas so precipitation
// appears to move, but dwell 2s on each frame and HOLD the newest (last) frame 5s before
// looping back to the start -- a deliberate "click, click, ... dwell, start over" feel
// rather than a fast blur. Runs only while the Weather Radar view is on screen and no
// "UPDATING" overlay is pending. Cheap: swaps the canvas to an already-decoded PSRAM
// buffer, it does not rebuild the tile.
static constexpr uint32_t WX_ANIM_STEP_MS = 2000;   // dwell on each intermediate frame
static constexpr uint32_t WX_ANIM_HOLD_MS = 5000;   // dwell on the newest frame before looping
static void wx_anim_cb(lv_timer_t *t) {
    if (!s_wxCanvas || !s_tv) return;
    if (s_weatherMode != WEATHER_RADAR || s_wxUpdating) return;
    if (lv_tileview_get_tile_act(s_tv) != s_tileWeather) return;   // weather not the visible tile
    const uint32_t gen = wx_radar_gen();
    const int n = wx_radar_gen_count(gen);
    if (n <= 0) return;
    s_wxAnimSlot = (s_wxAnimSlot + 1) % n;
    // Set how long THIS (newly shown) frame dwells before the next step: the newest frame
    // (highest slot index) holds 5s, every other frame holds 2s.
    lv_timer_set_period(t, (s_wxAnimSlot == n - 1) ? WX_ANIM_HOLD_MS : WX_ANIM_STEP_MS);
    const uint16_t *px = nullptr; uint32_t ft = 0;
    if (!wx_radar_frame(s_wxAnimSlot, gen, &px, &ft) || !px) return;
    lv_canvas_set_buffer(s_wxCanvas, (void *)px, WX_RADAR_SIZE, WX_RADAR_SIZE, LV_IMG_CF_TRUE_COLOR);
    lv_obj_invalidate(s_wxCanvas);
    if (s_wxAttrib) {
        char stamp[6] = "--:--"; time_t t = (time_t)ft; struct tm ti;
        if (ft && localtime_r(&t, &ti)) snprintf(stamp, sizeof(stamp), "%02d:%02d", ti.tm_hour, ti.tm_min);
        char attr[64]; snprintf(attr, sizeof(attr), "RADAR %s  |  RAINVIEWER", stamp);
        lv_label_set_text(s_wxAttrib, attr);
    }
}

static void build_weather(void) {
    if (!s_weatherNow || !s_weatherMeta || !s_weatherDays || !s_wxFooter) return;
    WeatherSnapshot w;
    if (!weather_get(w)) {
        lv_label_set_text(s_weatherNow, "Forecast unavailable");
        lv_label_set_text(s_weatherMeta, "Waiting for WiFi data...");
        lv_label_set_text(s_wxFooter, "WEATHER DATA PENDING");
        lv_label_set_text(s_weatherDays, "");
    } else {
        char now[96];
        snprintf(now, sizeof(now), "%.0f %s\n%s", weather_temp(w.tempC),
                 weather_temp_unit(), weather_condition(w.code));
        lv_label_set_text(s_weatherNow, now);
        char meta[128];
        snprintf(meta, sizeof(meta), "Feels %.0f %s   Humidity %d%%\nWind %.0f %s  %s   Updated %s",
                 weather_temp(w.feelsC), weather_temp_unit(), w.humidity,
                 weather_wind(w.windKmh), weather_wind_unit(), cardinal((float)w.windDeg), w.updated);
        lv_label_set_text(s_weatherMeta, meta);

        char footer[96];
        snprintf(footer, sizeof(footer), "%.0f %s   %s", weather_temp(w.tempC),
                 weather_temp_unit(), weather_condition(w.code));
        lv_label_set_text(s_wxFooter, footer);
        char wxmeta[96];
        snprintf(wxmeta, sizeof(wxmeta), "WIND %s %.0f %s   HUM %d%%",
                 cardinal((float)w.windDeg), weather_wind(w.windKmh), weather_wind_unit(), w.humidity);
        lv_label_set_text(s_wxMeta, wxmeta);

        char current[24];
        snprintf(current, sizeof(current), "%.0f %s", weather_temp(w.tempC), weather_temp_unit());
        lv_label_set_text(s_fcCurrent, current);
        lv_label_set_text(s_fcCondition, weather_condition(w.code));
        lv_label_set_text(s_fcMetricValue[0], current);
        char hum[16]; snprintf(hum, sizeof(hum), "%d%%", w.humidity);
        lv_label_set_text(s_fcMetricValue[1], hum);
        char wind[28]; snprintf(wind, sizeof(wind), "%s %.0f %s", cardinal((float)w.windDeg),
                                weather_wind(w.windKmh), weather_wind_unit());
        lv_label_set_text(s_fcMetricValue[2], wind);
        char updated[24]; snprintf(updated, sizeof(updated), "UPDATED %s", w.updated);
        lv_label_set_text(s_fcUpdated, updated);

        for (int col = 0; col < 3; ++col) {
            const int i = col + 1;
            if (i < w.dayCount) {
                lv_label_set_text(s_fcDay[col], weather_day_name(w.days[i].date));
                lv_label_set_text(s_fcDayCondition[col], weather_condition(w.days[i].code));
                char temps[28];
                snprintf(temps, sizeof(temps), "%.0f / %.0f %s",
                         weather_temp(w.days[i].tempMaxC), weather_temp(w.days[i].tempMinC), weather_temp_unit());
                lv_label_set_text(s_fcDayTemp[col], temps);
                char chance[20]; snprintf(chance, sizeof(chance), "RAIN %d%%", w.days[i].rainChance);
                lv_label_set_text(s_fcDayRain[col], chance);
            } else {
                lv_label_set_text(s_fcDay[col], "-");
                lv_label_set_text(s_fcDayCondition[col], "");
                lv_label_set_text(s_fcDayTemp[col], "");
                lv_label_set_text(s_fcDayRain[col], "");
            }
        }

        char days[320] = "";
        for (int i = 1; i < w.dayCount && i < 4; ++i) {
            char row[104];
            snprintf(row, sizeof(row), "%-3s  %-14s  %2.0f/%2.0f %s  %3d%%\n",
                     weather_day_name(w.days[i].date), weather_condition(w.days[i].code),
                     weather_temp(w.days[i].tempMaxC), weather_temp(w.days[i].tempMinC),
                     weather_temp_unit(), w.days[i].rainChance);
            strncat(days, row, sizeof(days) - strlen(days) - 1);
        }
        lv_label_set_text(s_weatherDays, days);
    }

    uint32_t frameTime = 0, version = 0;
    double rlat = 0, rlon = 0;
    const bool cloudMode = s_weatherMode == WEATHER_CLOUDS;
    const bool forecastMode = s_weatherMode == WEATHER_FORECAST;
    bool haveImage = false;
    const uint16_t *pixels = nullptr;
    if (cloudMode) {
        haveImage = cloud_image_front(&pixels, &frameTime, &rlat, &rlon, &version);
    } else {
        // Drop the "UPDATING" overlay the moment a frame of a NEW generation lands (the
        // version counter bumps on every commit, so the first new-zoom frame trips this).
        const uint32_t ver = wx_radar_version();
        if (s_wxUpdating && ver != s_wxLastVersion) s_wxUpdating = false;
        s_wxLastVersion = ver;
        wx_radar_center(&rlat, &rlon);
        haveImage = wx_anim_current(&pixels, &frameTime);
    }
    if (haveImage && pixels && s_wxCanvas) {
        lv_canvas_set_buffer(s_wxCanvas, (void *)pixels, WX_RADAR_SIZE, WX_RADAR_SIZE, LV_IMG_CF_TRUE_COLOR);
        lv_obj_clear_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_wxCanvas);
        char iata[4]; float d = 0, b = 0;
        if (airports_nearest_iata(rlat, rlon, 200.0f, iata, &d, &b)) {
            char apt[64];
            snprintf(apt, sizeof(apt), "O  %s   %.0f %s %s", iata, dist_val(d), dist_unit(), cardinal(b));
            lv_label_set_text(s_wxAirport, apt);
        } else lv_label_set_text(s_wxAirport, "RADAR CENTRE");
        char stamp[6] = "--:--";
        time_t ft = (time_t)frameTime; struct tm ti;
        if (frameTime && localtime_r(&ft, &ti)) snprintf(stamp, sizeof(stamp), "%02d:%02d", ti.tm_hour, ti.tm_min);
        char attr[64];
        snprintf(attr, sizeof(attr), cloudMode ? "SAT %s  |  EUMETSAT" : "RADAR %s  |  RAINVIEWER", stamp);
        lv_label_set_text(s_wxAttrib, attr);
    } else {
        if (s_wxCanvas) lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_wxAirport, "RADAR CENTRE");
        lv_label_set_text(s_wxAttrib, cloudMode ? "WAITING FOR SATELLITE DATA" : "WAITING FOR RADAR DATA");
    }

    lv_obj_t *forecastObjs[] = {
        s_fcCurrent, s_fcCondition, s_fcUpdated,
        s_fcMetricName[0], s_fcMetricName[1], s_fcMetricName[2],
        s_fcMetricValue[0], s_fcMetricValue[1], s_fcMetricValue[2],
        s_fcDay[0], s_fcDay[1], s_fcDay[2],
        s_fcDayCondition[0], s_fcDayCondition[1], s_fcDayCondition[2],
        s_fcDayTemp[0], s_fcDayTemp[1], s_fcDayTemp[2],
        s_fcDayRain[0], s_fcDayRain[1], s_fcDayRain[2]
    };
    lv_obj_t *radarObjs[] = { s_wxCanvas, s_wxStatus, s_wxAirport, s_wxFooter, s_wxMeta,
                              s_wxAttrib, s_wxNorth, s_wxCenter, s_wxRange,
                              s_wxRings[0], s_wxRings[1], s_wxRings[2],
                              s_wxRingLbl[0], s_wxRingLbl[1], s_wxRingLbl[2] };
    for (lv_obj_t *o : forecastObjs) if (o) {
        if (forecastMode) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    for (lv_obj_t *o : radarObjs) if (o) {
        if (forecastMode) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (!forecastMode && haveImage) lv_obj_add_flag(s_wxStatus, LV_OBJ_FLAG_HIDDEN);
    if (!forecastMode && !haveImage) lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
    if (cloudMode) {
        lv_label_set_text(s_wxRange, "200 KM");
    } else {
        char rb[16];
        snprintf(rb, sizeof(rb), "%.0f %s", wx_dist_val(WX_ZOOM_KM[s_wxZoom]), wx_dist_unit());
        lv_label_set_text(s_wxRange, rb);
    }
    // Ring 0 is the outer edge (full range), ring 1 is 2/3 out, ring 2 is 1/3 out — label
    // each with the real distance it stands for so the zoom is visible even when the
    // roads/precip underneath don't obviously change (a bare number, not a repeat of the
    // "N MI" range label, since three of those stacked up would be redundant clutter).
    {
        const float fullKm = cloudMode ? 200.0f : WX_ZOOM_KM[s_wxZoom];
        const float frac[3] = { 1.0f, 2.0f / 3.0f, 1.0f / 3.0f };
        for (int i = 0; i < 3; ++i) {
            if (!s_wxRingLbl[i]) continue;
            char b[12];
            snprintf(b, sizeof(b), "%.0f", wx_dist_val(fullKm * frac[i]));
            lv_label_set_text(s_wxRingLbl[i], b);
        }
    }
    if (s_weatherTitle) lv_label_set_text(s_weatherTitle,
        s_weatherMode == WEATHER_RADAR ? "WX RADAR" :
        s_weatherMode == WEATHER_CLOUDS ? "SAT CLOUDS" : "WEATHER");
}

void ui_set_weather_forecast(bool forecast) {
    s_weatherMode = forecast ? WEATHER_FORECAST : WEATHER_RADAR;
    build_weather();
}

// WEATHER_CLOUDS is no longer reachable from here (nothing assigns it) — the touch
// button that used to cycle RADAR/CLOUDS/FORECAST is gone; the knob push handler in
// main.cpp (weather_press_cycle()) drives radar zoom tiers + forecast instead. Left in
// place rather than ripped out in case satellite cloud view comes back some other way.
bool ui_weather_is_forecast(void) { return s_weatherMode == WEATHER_FORECAST; }

void ui_set_wx_zoom(int tier) {
    s_wxZoom = (tier < 0 || tier > 1) ? 0 : tier;
    s_wxUpdating = true;   // covers the canvas with "UPDATING..." until the new frame lands
    build_weather();       // range label + rings update now; the map image catches up later
}

// Rebuild the weather tile when it is the one on screen. Only the visible tile pays the
// cost. This used to cover the list and stats tiles too, which no longer exist.
static void refresh_active_tile(void) {
    if (!s_tv) return;
    if (lv_tileview_get_tile_act(s_tv) == s_tileWeather) build_weather();
}

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%d", radar::countInRange());
        lv_label_set_text(s_hudCount, cbuf);
    }
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// ------------------------------------------------------------------- building
static lv_obj_t *make_tile_title(lv_obj_t *tile, const char *txt) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, F16(), 0);
    lv_obj_set_style_text_color(l, UI_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

// A full-screen round panel that clips its content to the circle (for list/stats views).
static lv_obj_t *make_round_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, UI_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, UI_GREEN, 0);
    lv_obj_set_style_border_opa(p, 50, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_card(void) {
    s_card = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_card);
    // large text needs a taller card (three 18px data lines + the route line below them)
    lv_obj_set_size(s_card, s_bigText ? 316 : 300, s_bigText ? 148 : 118);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, s_bigText ? 56 : 66);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, 235, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_color(s_card, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_card, 90, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardTitle, F16(), 0);
    lv_obj_set_style_text_color(s_cardTitle, UI_INK, 0);
    lv_obj_align(s_cardTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cardL = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardL, F14(), 0);
    lv_obj_set_style_text_color(s_cardL, UI_SOFT, 0);
    lv_obj_align(s_cardL, LV_ALIGN_TOP_LEFT, 0, s_bigText ? 30 : 26);

    s_cardR = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardR, F14(), 0);
    lv_obj_set_style_text_color(s_cardR, UI_SOFT, 0);
    lv_obj_align(s_cardR, LV_ALIGN_TOP_LEFT, s_bigText ? 160 : 150, s_bigText ? 30 : 26);

    s_cardRoute = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardRoute, F14(), 0);
    lv_obj_set_style_text_color(s_cardRoute, UI_GREEN, 0);
    lv_obj_align(s_cardRoute, LV_ALIGN_TOP_LEFT, 0, s_bigText ? 100 : 76);

    // aircraft photo + credit, floating above the card (hidden until one loads)
    s_photo = lv_canvas_create(s_tileRadar);
    lv_obj_set_style_radius(s_photo, 6, 0);
    lv_obj_set_style_clip_corner(s_photo, true, 0);
    lv_obj_set_style_border_color(s_photo, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_photo, 170, 0);
    lv_obj_set_style_border_width(s_photo, 1, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_photoCredit = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_photoCredit, F12(), 0);
    lv_obj_set_style_text_color(s_photoCredit, UI_DIM, 0);
    lv_label_set_text(s_photoCredit, "");
    lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_view(int idx) {
    if (s_tv && idx >= 0 && idx <= 1) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) {
    // Before the container goes, not after. splash_lines owns a 651 KB PSRAM buffer and
    // holds pointers to two children of this object; deleting the parent first would free
    // neither the buffer nor the pointers, and leave release() deleting objects that are
    // already gone.
    splash_lines::release();
    lv_obj_del((lv_obj_t *)a->var);
}

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_cb);
    lv_anim_start(&a);
}

void ui_splash_show(void) {
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    const bool office = app_theme::get() == APP_THEME_OFFICE;
    lv_obj_set_style_bg_color(cont, office ? app_theme::palette().bg : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // Title card — decoded from a flash-resident PNG at show time (see splash_art.h).
    // settings_view.cpp's About page decodes the same way.
    static lv_img_dsc_t splashImg;
    if (splash_art_decode(office, &splashImg)) {
        lv_obj_t *img = lv_img_create(cont);
        lv_img_set_src(img, &splashImg);
        lv_obj_center(img);
    }

    // The same three lines the About page shows, from the same theme data and the same
    // module: the version, the address and the data credits, with the glass composited over
    // them. The address reads capsuleradar.local here, which is true from the moment mDNS
    // is up and does not need an IP the board cannot have yet at this point in boot.
    splash_lines::attach(cont);

    // Force this onto the panel right now — the rest of setup() (WiFi connect, sensor
    // init, etc.) is blocking and won't call lv_timer_handler() again until loop()
    // starts, so without this the screen just sits black through all of that and the
    // splash only gets its first real paint at the same moment its overdue fade-timer
    // fires too, a barely-visible flash instead of the held title card it's meant to be.
    lv_refr_now(NULL);

    // Hold 2s then fade. This timer only advances while lv_timer_handler() is being
    // called, which does NOT happen during setup()'s blocking work (WiFi connect, sensor
    // init) -- main.cpp deliberately pumps the UI for ~2.6s right after the boot app
    // (Clock) is loaded and BEFORE the blocking WiFi connect call, specifically so this
    // timer gets to fire for real instead of sitting frozen for however long WiFi takes.
    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2000, cont);
    lv_timer_set_repeat_count(t, 1);
}


// Section ledger for ui_create(). Measured taking 4.2 MB of an 8 MB budget in one call,
// which is more than every app's artwork put together. Marks split it by section so the
// expensive part can be named rather than guessed at.
static void umark(const char *what) {
#ifdef ARDUINO
    static uint32_t prev = 0;
    const uint32_t now = (uint32_t)ESP.getFreePsram();
    Serial.printf("[psram/ui] %-26s free %6u KB", what, (unsigned)(now / 1024));
    if (prev >= now && prev) Serial.printf("   (-%u KB)", (unsigned)((prev - now) / 1024));
    prev = now;
    Serial.println();
#else
    (void)what;
#endif
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, UI_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    umark("ui_create start");
    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    // Two tiles now: Flight Tracker and Weather Radar. The list and stats tiles that sat
    // between them were reachable only by swiping, so they went with the touchscreen.
    // The knob moves between apps via app_shell; nothing swipes any more, and these two
    // are switched programmatically by ui_show_view() from each app's onEnter.
    s_tileRadar   = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileWeather = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_LEFT);
    lv_obj_add_event_cb(s_tv, [](lv_event_t *) { refresh_active_tile(); }, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    umark("after tileview");
    radar::init(s_tileRadar);
    umark("after radar::init");
    radar::setRangeLabelVisible(true);   // radar draws its own range readout again, now that
                                         // the touch-only zoom button that replaced it is gone
    build_card();
    umark("after detail card+photo");

    // top status HUD (wifi / aircraft count / clock); white reads on both themes.
    // WiFi is a 4-bar signal meter: bar count = RSSI strength, colour = feed health.
    s_hudWifi = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_hudWifi);
    lv_obj_set_size(s_hudWifi, 21, 14);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -94, 50);
    lv_obj_clear_flag(s_hudWifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_hudBars[i] = lv_obj_create(s_hudWifi);
        lv_obj_remove_style_all(s_hudBars[i]);
        lv_obj_set_size(s_hudBars[i], 3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
        lv_obj_align(s_hudBars[i], LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(i * 5), 0);
        lv_obj_set_style_radius(s_hudBars[i], 1, 0);
        lv_obj_set_style_bg_color(s_hudBars[i], UI_INK, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_hudBars[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_hudGps = lv_label_create(s_tileRadar);     // GPS satellite icon (between WiFi bars and count)
    lv_obj_set_style_text_font(s_hudGps, F14(), 0);
    lv_obj_set_style_text_color(s_hudGps, UI_GREEN, 0);
    lv_label_set_text(s_hudGps, "");             // hidden until ui_set_gps() says GPS is on
    lv_obj_align(s_hudGps, LV_ALIGN_TOP_MID, -62, 50);

    s_hudCount = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudCount, F14(), 0);
    lv_obj_set_style_text_color(s_hudCount, UI_INK, 0);
    lv_label_set_text(s_hudCount, "0");
    lv_obj_align(s_hudCount, LV_ALIGN_TOP_MID, -34, 50);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, F14(), 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 30, 50);

    s_hudBatt = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudBatt, F14(), 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 92, 50);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, F12(), 0);
    lv_obj_set_style_text_color(s_hudDate, UI_INK, 0);
    lv_obj_set_style_text_opa(s_hudDate, 140, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 70);

#if CUSTOM_HAS_RADAR_STYLE
    // This status row (WiFi bars, GPS icon, in-range count, clock, battery,
    // date) is Flight Tracker-only chrome, not a device-wide status bar — a
    // pushed design's own scope has no room reserved for it and never drew it
    // in the editor, so hide it here rather than have it float over the design.
    lv_obj_add_flag(s_hudWifi, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hudGps, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hudCount, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hudClock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hudBatt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_hudDate, LV_OBJ_FLAG_HIDDEN);
#endif

    umark("after radar HUD");
    // --- weather tile (current conditions + next three days) ---
    lv_obj_t *wp = make_round_panel(s_tileWeather);
    lv_obj_set_style_bg_color(wp, lv_color_black(), 0); // hide square radar-tile bounds on AMOLED
    s_weatherTitle = make_tile_title(wp, "WX RADAR");
    lv_obj_set_style_bg_color(s_weatherTitle, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_weatherTitle, 170, 0);
    lv_obj_set_style_pad_left(s_weatherTitle, 8, 0);
    lv_obj_set_style_pad_right(s_weatherTitle, 8, 0);
    lv_obj_set_style_pad_top(s_weatherTitle, 2, 0);
    lv_obj_set_style_pad_bottom(s_weatherTitle, 2, 0);
    lv_obj_set_style_radius(s_weatherTitle, 8, 0);
    s_weatherNow = lv_label_create(wp);
    lv_obj_set_width(s_weatherNow, 330);
    lv_obj_set_style_text_font(s_weatherNow, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_weatherNow, UI_INK, 0);
    lv_obj_set_style_text_align(s_weatherNow, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weatherNow, "Forecast unavailable");
    lv_obj_align(s_weatherNow, LV_ALIGN_TOP_MID, 0, 64);

    s_weatherMeta = lv_label_create(wp);
    lv_obj_set_width(s_weatherMeta, 380);
    lv_obj_set_style_text_font(s_weatherMeta, F14(), 0);
    lv_obj_set_style_text_color(s_weatherMeta, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_weatherMeta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_weatherMeta, "Waiting for WiFi data...");
    lv_obj_align(s_weatherMeta, LV_ALIGN_TOP_MID, 0, 150);

    s_weatherDays = lv_label_create(wp);
    lv_obj_set_width(s_weatherDays, 390);
    lv_obj_set_style_text_font(s_weatherDays, F16(), 0);
    lv_obj_set_style_text_color(s_weatherDays, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_weatherDays, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_weatherDays, "");
    lv_obj_align(s_weatherDays, LV_ALIGN_TOP_LEFT, 42, 234);
    // Legacy formatted labels are retained only to avoid touching older data-update
    // plumbing; the redesigned forecast uses independent aligned objects below.
    lv_obj_add_flag(s_weatherNow, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weatherMeta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weatherDays, LV_OBJ_FLAG_HIDDEN);

    // Default mode: genuine precipitation radar with aviation-style overlays.
    s_wxAirport = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxAirport, F14(), 0);
    lv_obj_set_style_text_color(s_wxAirport, UI_SOFT, 0);
    lv_obj_set_style_bg_color(s_wxAirport, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxAirport, 160, 0);
    lv_obj_set_style_pad_left(s_wxAirport, 6, 0);
    lv_obj_set_style_pad_right(s_wxAirport, 6, 0);
    lv_obj_set_style_radius(s_wxAirport, 6, 0);
    lv_label_set_text(s_wxAirport, "RADAR CENTRE");
    lv_obj_align(s_wxAirport, LV_ALIGN_TOP_MID, 0, 46);

    s_wxCanvas = lv_canvas_create(wp);
    lv_obj_set_size(s_wxCanvas, WX_RADAR_SIZE, WX_RADAR_SIZE);
    lv_obj_align(s_wxCanvas, LV_ALIGN_TOP_MID, 0, 52);
    lv_obj_add_flag(s_wxCanvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_background(s_wxCanvas);

    s_wxStatus = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxStatus, F14(), 0);
    lv_obj_set_style_text_color(s_wxStatus, UI_DIM, 0);
    lv_label_set_text(s_wxStatus, "ACQUIRING WX RADAR...");
    lv_obj_align(s_wxStatus, LV_ALIGN_TOP_MID, 0, 222);

    const int ringSize[3] = { 360, 240, 120 };
    for (int i = 0; i < 3; ++i) {
        s_wxRings[i] = lv_obj_create(wp);
        lv_obj_remove_style_all(s_wxRings[i]);
        lv_obj_set_size(s_wxRings[i], ringSize[i], ringSize[i]);
        lv_obj_align(s_wxRings[i], LV_ALIGN_TOP_MID, 0, 52 + (360 - ringSize[i]) / 2);
        lv_obj_set_style_radius(s_wxRings[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(s_wxRings[i], UI_GREEN, 0);
        lv_obj_set_style_border_opa(s_wxRings[i], i == 0 ? 180 : 90, 0);
        lv_obj_set_style_border_width(s_wxRings[i], 1, 0);
        lv_obj_clear_flag(s_wxRings[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        // Fixed-pixel rings alone don't communicate zoom — all three tiers drew the exact
        // same 360/240/120px circles regardless of what real-world distance they stood
        // for, so cycling zoom looked like nothing changed even though the underlying
        // roads/precip data genuinely did (found from live user feedback: "they all seem
        // the exact same zoom out, even tho the text changes"). Labeling the two inner
        // rings with their actual distance for the current tier makes the zoom
        // unmistakable — updated in build_weather() alongside the range label. The outer
        // ring skips its own label since s_wxRange already shows that distance; adding a
        // third label right next to it would just be clutter.
        if (i == 0) continue;
        s_wxRingLbl[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_wxRingLbl[i], F12(), 0);
        lv_obj_set_style_text_color(s_wxRingLbl[i], UI_GREEN, 0);
        lv_obj_set_style_text_opa(s_wxRingLbl[i], 150, 0);
        lv_label_set_text(s_wxRingLbl[i], "");
        // Left side, mirroring s_wxRange's right-side placement at the same height —
        // clear of the top status/airport labels, the bottom-center footer/meta text,
        // and the center marker.
        lv_obj_align(s_wxRingLbl[i], LV_ALIGN_TOP_MID, -(ringSize[i] / 2 - 12), 228);
    }
    s_wxNorth = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxNorth, F12(), 0);
    lv_obj_set_style_text_color(s_wxNorth, UI_GREEN, 0);
    lv_label_set_text(s_wxNorth, "N");
    lv_obj_align(s_wxNorth, LV_ALIGN_TOP_MID, 0, 58);
    s_wxCenter = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxCenter, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_wxCenter, UI_INK, 0);
    lv_label_set_text(s_wxCenter, "+");
    lv_obj_align(s_wxCenter, LV_ALIGN_TOP_MID, 0, 219);
    s_wxRange = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxRange, F12(), 0);
    lv_obj_set_style_text_color(s_wxRange, UI_GREEN, 0);
    lv_label_set_text(s_wxRange, "75 KM");
    lv_obj_align(s_wxRange, LV_ALIGN_TOP_MID, 128, 225);

    s_wxUpdateOverlay = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxUpdateOverlay, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_wxUpdateOverlay, UI_GREEN, 0);
    lv_obj_set_style_bg_color(s_wxUpdateOverlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxUpdateOverlay, 220, 0);
    lv_obj_set_style_pad_left(s_wxUpdateOverlay, 16, 0);
    lv_obj_set_style_pad_right(s_wxUpdateOverlay, 16, 0);
    lv_obj_set_style_pad_top(s_wxUpdateOverlay, 10, 0);
    lv_obj_set_style_pad_bottom(s_wxUpdateOverlay, 10, 0);
    lv_obj_set_style_radius(s_wxUpdateOverlay, 8, 0);
    lv_label_set_text(s_wxUpdateOverlay, "UPDATING");
    lv_obj_align(s_wxUpdateOverlay, LV_ALIGN_TOP_MID, 0, 214);   // centered over the canvas/rings
    lv_obj_add_flag(s_wxUpdateOverlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wxUpdateOverlay);
    lv_timer_create([](lv_timer_t *) {
        if (!s_wxUpdateOverlay) return;
        if (!s_wxUpdating || s_weatherMode != WEATHER_RADAR) {
            lv_obj_add_flag(s_wxUpdateOverlay, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(s_wxUpdateOverlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wxUpdateOverlay);
        s_wxUpdateDots = (s_wxUpdateDots + 1) % 4;
        char b[16];
        snprintf(b, sizeof(b), "UPDATING%.*s", s_wxUpdateDots, "...");
        lv_label_set_text(s_wxUpdateOverlay, b);
    }, 400, nullptr);

    lv_timer_create(wx_anim_cb, WX_ANIM_STEP_MS, nullptr);   // 2s/frame, 5s hold on newest (see wx_anim_cb)

    s_wxFooter = lv_label_create(wp);
    lv_obj_set_width(s_wxFooter, 360);
    lv_obj_set_style_text_font(s_wxFooter, F16(), 0);
    lv_obj_set_style_text_color(s_wxFooter, UI_INK, 0);
    lv_obj_set_style_text_align(s_wxFooter, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wxFooter, "WEATHER DATA PENDING");
    lv_obj_align(s_wxFooter, LV_ALIGN_TOP_MID, 0, 326);
    lv_obj_set_style_bg_color(s_wxFooter, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxFooter, 185, 0);
    lv_obj_set_style_pad_hor(s_wxFooter, 8, 0);
    lv_obj_set_style_radius(s_wxFooter, 7, 0);
    s_wxMeta = lv_label_create(wp);
    lv_obj_set_width(s_wxMeta, 360);
    lv_obj_set_style_text_font(s_wxMeta, F14(), 0);
    lv_obj_set_style_text_color(s_wxMeta, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_wxMeta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wxMeta, "");
    lv_obj_align(s_wxMeta, LV_ALIGN_TOP_MID, 0, 351);
    lv_obj_set_style_bg_color(s_wxMeta, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxMeta, 185, 0);
    lv_obj_set_style_pad_hor(s_wxMeta, 8, 0);
    lv_obj_set_style_radius(s_wxMeta, 7, 0);
    s_wxAttrib = lv_label_create(wp);
    lv_obj_set_style_text_font(s_wxAttrib, F12(), 0);
    lv_obj_set_style_text_color(s_wxAttrib, UI_DIM, 0);
    lv_label_set_text(s_wxAttrib, "WAITING FOR RADAR DATA");
    lv_obj_align(s_wxAttrib, LV_ALIGN_TOP_MID, 0, 376);
    lv_obj_set_style_bg_color(s_wxAttrib, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wxAttrib, 170, 0);
    lv_obj_set_style_pad_hor(s_wxAttrib, 6, 0);
    lv_obj_set_style_radius(s_wxAttrib, 6, 0);

    // Forecast mode: independent, aligned objects instead of a tiny text table.
    s_fcCurrent = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcCurrent, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_fcCurrent, UI_INK, 0);
    lv_label_set_text(s_fcCurrent, "-- C");
    lv_obj_align(s_fcCurrent, LV_ALIGN_TOP_MID, 0, 68);
    s_fcCondition = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcCondition, F16(), 0);
    lv_obj_set_style_text_color(s_fcCondition, UI_SOFT, 0);
    lv_label_set_text(s_fcCondition, "Waiting for data");
    lv_obj_align(s_fcCondition, LV_ALIGN_TOP_MID, 0, 105);

    const char *metricNames[3] = { "FEELS", "HUMIDITY", "WIND" };
    const int colX[3] = { -122, 0, 122 };
    for (int i = 0; i < 3; ++i) {
        s_fcMetricName[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcMetricName[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcMetricName[i], UI_DIM, 0);
        lv_label_set_text(s_fcMetricName[i], metricNames[i]);
        lv_obj_align(s_fcMetricName[i], LV_ALIGN_TOP_MID, colX[i], 150);
        s_fcMetricValue[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcMetricValue[i], F16(), 0);
        lv_obj_set_style_text_color(s_fcMetricValue[i], UI_INK, 0);
        lv_label_set_text(s_fcMetricValue[i], "-");
        lv_obj_align(s_fcMetricValue[i], LV_ALIGN_TOP_MID, colX[i], 170);

        s_fcDay[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDay[i], F16(), 0);
        lv_obj_set_style_text_color(s_fcDay[i], UI_GREEN, 0);
        lv_label_set_text(s_fcDay[i], "---");
        lv_obj_align(s_fcDay[i], LV_ALIGN_TOP_MID, colX[i], 226);
        s_fcDayCondition[i] = lv_label_create(wp);
        lv_obj_set_width(s_fcDayCondition[i], 116);
        lv_obj_set_style_text_font(s_fcDayCondition[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcDayCondition[i], UI_SOFT, 0);
        lv_obj_set_style_text_align(s_fcDayCondition[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(s_fcDayCondition[i], LV_LABEL_LONG_WRAP);
        lv_label_set_text(s_fcDayCondition[i], "");
        lv_obj_align(s_fcDayCondition[i], LV_ALIGN_TOP_MID, colX[i], 254);
        s_fcDayTemp[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDayTemp[i], F14(), 0);
        lv_obj_set_style_text_color(s_fcDayTemp[i], UI_INK, 0);
        lv_label_set_text(s_fcDayTemp[i], "");
        lv_obj_align(s_fcDayTemp[i], LV_ALIGN_TOP_MID, colX[i], 292);
        s_fcDayRain[i] = lv_label_create(wp);
        lv_obj_set_style_text_font(s_fcDayRain[i], F12(), 0);
        lv_obj_set_style_text_color(s_fcDayRain[i], lv_color_hex(0x4DDCFF), 0);
        lv_label_set_text(s_fcDayRain[i], "");
        lv_obj_align(s_fcDayRain[i], LV_ALIGN_TOP_MID, colX[i], 320);
    }
    s_fcUpdated = lv_label_create(wp);
    lv_obj_set_style_text_font(s_fcUpdated, F12(), 0);
    lv_obj_set_style_text_color(s_fcUpdated, UI_DIM, 0);
    lv_label_set_text(s_fcUpdated, "");
    lv_obj_align(s_fcUpdated, LV_ALIGN_TOP_MID, 0, 365);

    umark("after weather tile");
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);

    ui_splash_show();   // branded boot splash on top (auto-fades)
    umark("after splash");
}
