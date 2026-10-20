// The Orb OS — glue between the app shell and the apps: what entering and leaving the
// Flight Tracker and Weather do, aircraft alert sounds, the feed's query radius, and the
// range / weather-units hooks Settings calls. Moved out of main.cpp unchanged.
#include <Arduino.h>
#include <Preferences.h>
#include <set>
#include <string>
#include "config.h"
#include "geo.h"
#include "orb_state.h"
#include "orb_apps.h"
#include "radar_view.h"
#include "ui.h"
#include "audio.h"
#include "wx_radar.h"

// Audio alerts. g_alertMode: 0 = off, 1 = emergencies only, 2 = new aircraft + emergencies.
// g_proximityKm > 0 also pings (once) when any aircraft crosses into that radius.
void checkAudioEvents() {
    if (!audio_present() || !g_soundRadar) return;
    static std::set<std::string> seen, seenProx;
    static bool first = true;
    static uint32_t lastNew = 0;
    std::set<std::string> now, nowProx;
    for (const Aircraft &ac : g_snap) {
        const double d = geo::haversineKm(g_settings.homeLat, g_settings.homeLon, ac.lat, ac.lon);
        if (d > g_settings.rangeKm) continue;                 // in-range only
        const std::string hex = ac.hex.c_str();
        now.insert(hex);
        const bool isNew     = !first && !seen.count(hex);
        const bool emergency = acIsEmergency(ac.squawk) || ac.military;  // military: feed dbFlags

        // proximity: fire once, when an aircraft first crosses into the radius (any aircraft)
        if (g_proximityKm > 0.0f && d <= g_proximityKm) {
            nowProx.insert(hex);
            if (!first && !seenProx.count(hex)) audio_play(AUDIO_ALERT);
        }

        // new-in-range pings (on entry), gated by the alert mode
        if (isNew) {
            if (emergency) { if (g_alertMode >= 1) audio_play(AUDIO_ALERT); }   // emergencies only / +new
            else if (g_alertMode >= 2 && millis() - lastNew > 3000) {
                audio_play(AUDIO_NEW);                                          // new contact (rate-limited)
                lastNew = millis();
            }
        }
    }
    seen.swap(now);
    seenProx.swap(nowProx);
    first = false;
}

// Feed query radius: wider than the display range (so off-range traffic shows as edge
// arrows) AND wide enough to cover the proximity-alert circle (else an alert radius larger
// than the query would never fire), clamped to keep busy-airspace downloads bounded.
float queryRadiusKm() {
    float km = g_settings.rangeKm * ADSB_QUERY_MULT;
    if (g_proximityKm > 0.0f && g_proximityKm * 1.2f > km) km = g_proximityKm * 1.2f;
    return constrain(km, ADSB_QUERY_MIN_KM, ADSB_QUERY_MAX_KM);
}

// The center dead zone is authored in PIXELS (Launch Kit's Scope card) rather
// than km, because what it exists to clear is a fixed piece of on-screen
// artwork — a center hub, a gear, a hand pivot — that stays the same size on the
// glass whatever the range is. So it becomes a real distance only here, against
// whatever range is currently live, and has to be recomputed whenever the range
// changes. Launch Kit's preview does the identical conversion.
float deadZoneKm() {
    if (g_deadZonePx <= 0) return 0.0f;
    const float px = (float)min(g_deadZonePx, (int)RADAR_R_OUTER_PX);
    return (px / (float)RADAR_R_OUTER_PX) * g_settings.rangeKm;
}

// Change the display range, persist it, and ask adsb_task to re-query at a matching
// radius (safely, on its own core). Re-render immediately. Reached from Settings >
// Range through host_set_range_km() below. This used to be driven by the on-screen
// zoom button, which was touch-only and went away when touch did.
static void onRangeChange(float km) {
    g_settings.rangeKm = km;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putFloat("rangeKm", km);
    p.end();
    g_adsb.setMinDistKm(deadZoneKm());   // px-based zone, so a new range means a new km threshold
    g_requeryKm = queryRadiusKm();
    g_requery = true;
    radar::update(g_snap, g_settings);   // instant visual zoom from the last snapshot
    ui_on_data_updated();
}

// Settings > Range hooks (declared extern in settings_view.cpp).
float host_get_range_km() { return g_settings.rangeKm; }
void  host_set_range_km(float km) { onRangeChange(km); }

// Shared with the Settings app (settings_view.cpp).
// App-shell onEnter hooks: flip the radar screen's tileview between the radar view
// and the original app's weather view (radar / clouds / forecast).
static void radar_show_home()    { ui_show_view(0); }
// Entering and leaving the weather app. Both only ASK: the network task owns the buffers
// and does the work, because it is the one that writes into them. See g_wxOpened above.
void radar_show_weather() {
    g_wxClosed = false;
    g_wxOpened = true;
    // The map under the weather is built HERE, on the UI thread, because it reads road tiles
    // off the SD card and the driver cannot take that from two tasks at once. The network
    // task only ever blits the finished masks. Returns immediately if this centre is built.
    wx_phase_set(WX_PHASE_MAP);
    ui_weather_art_attach();
    wx_map_prepare(g_settings.homeLat, g_settings.homeLon, g_wxZoomTier);
    ui_show_view(1);   // tile 1 since list/stats went
}

void radar_hide_weather() {
    ui_weather_art_release();   // UI thread, so the plate goes back here rather than by flag
    g_wxOpened = false;
    g_wxClosed = true;
    Serial.println("[wxradar] app closed; buffers will go back at the next idle moment");
}

// A Launch Kit push with a custom selection design changes what the knob does on
// Flight Tracker: turning cycles the selected aircraft (see selectNext()'s "none"
// stop — no more requires clearing needing a separate gesture) instead of opening
// the app switcher, so the shell captures the knob the same way Settings does.
// Push then means "leave selection mode" (back to the switcher) instead of
// cycling the built-in Orb/Military/Aviator skin, which a custom design overrides
// anyway. Stock behaviour (no custom push) is untouched either way.
// Flight Tracker knob wiring now lives in radar_view (knobEnter/knobPress/
// knobTurn/knobExit) so the device and simulator run one identical state machine:
// land in the default view (knob released, a turn opens the switcher), push to
// enter selection mode (turn cycles aircraft), push again or wait 5s to drop back.
// Flight Tracker's baked plate/overlay (~1.3MB of decoded PSRAM) is freed on exit
// via knobExit(), same discipline as clockview::onExit.
void radar_show_home_custom() {
    radar_show_home();
    radar::knobEnter();   // re-attach style + land in the default view (knob released)
    g_radarViewActive = true;    // adsb_task may now poll; see g_radarViewActive above
}
void radar_turn_select(int delta) { radar::knobTurn(delta); }
void radar_press_custom_or_theme() { radar::knobPress(); }
void radar_exit_release_style() { radar::knobExit(); g_radarViewActive = false; }
void host_wx_zoom_set(int tier);   // defined below, near the other weather-units hosts
int  host_wx_zoom_tier();

// Knob push while on Weather: one cycle through everything the app shows, no touch
// needed — 50mi -> 100mi radar zoom, then the 3-day forecast, then back to 50mi.
// Satellite clouds isn't in this cycle (see ui_set_weather_forecast()'s comment).
void weather_press_cycle() {
    // Lean redesign: one fixed 50mi radar range, no user-selectable zoom (see
    // docs/lean-weather-radar-redesign.md). The knob push just toggles forecast <-> radar
    // map; the old 100mi tier and the re-fetch it forced are gone. g_wxZoomTier is pinned
    // to 0 (50mi) in loadSettings, so the radar map is always the 50mi view.
    ui_set_weather_forecast(!ui_weather_is_forecast());
}

// Countries where everyday weather is reported in Fahrenheit/miles rather than
// Celsius/km: the US (+ territories), Liberia, Myanmar. Approximated with coarse
// bounding boxes — good enough for "Automatic" mode; Settings > Weather > Units always
// lets you override it manually regardless of what this returns.
static bool is_imperial_region(double lat, double lon) {
    if (lat >= 24.0 && lat <= 49.5 && lon >= -125.0 && lon <= -66.0)  return true;   // CONUS
    if (lat >= 51.0 && lat <= 72.0 && lon >= -170.0 && lon <= -129.0) return true;   // Alaska
    if (lat >= 18.5 && lat <= 22.5 && lon >= -161.0 && lon <= -154.0) return true;   // Hawaii
    if (lat >= 4.0  && lat <= 8.6  && lon >= -11.6  && lon <= -7.3)   return true;   // Liberia
    if (lat >= 9.5  && lat <= 28.6 && lon >= 92.0   && lon <= 101.2)  return true;   // Myanmar
    return false;
}

// Resolves the current Weather units mode (0=Auto 1=Metric 2=Imperial) against the home
// location to the actual imperial/metric flag the UI needs. Called at boot and whenever
// the mode changes from Settings; home location itself only ever changes via a reboot
// (host_set_location), so there's no need to re-resolve Auto mode outside of those.
bool host_wx_is_imperial() {
    if (g_wxUnits == 1) return false;
    if (g_wxUnits == 2) return true;
    return is_imperial_region(g_settings.homeLat, g_settings.homeLon);
}
int host_wx_units_mode() { return g_wxUnits; }
void host_wx_units_set(int mode) {
    g_wxUnits = constrain(mode, 0, 2);
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("wxUnits", g_wxUnits);
    p.end();
    ui_set_wx_units(host_wx_is_imperial());
}

int host_wx_zoom_tier() { return g_wxZoomTier; }
void host_wx_zoom_set(int tier) {
    g_wxZoomTier = constrain(tier, 0, 1);
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("wxZoom2", g_wxZoomTier);
    p.end();
    ui_set_wx_zoom(g_wxZoomTier);
    g_wxZoomChanged = true;   // adsb_task refetches with the new range on its next pass
}
