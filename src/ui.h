#pragma once
// The Flight Tracker scope + aircraft detail card + the Weather Radar view.
// Pure LVGL, portable (device + SDL simulator). Builds on top of radar_view.
//
// Knob only. This file used to own touch: tap-to-select, an on-screen zoom button, and
// swiping between radar / list / stats / weather. List and Stats are gone with the
// touchscreen, and range moved to Settings > Range. Full input model in
// docs/ARCHITECTURE.md.
void ui_create(void);            // build the whole UI on the active screen
void ui_on_data_updated(void);   // refresh the detail card + weather after radar::update()
void ui_show_view(int idx);      // 0 = Flight Tracker, 1 = Weather Radar

// The weather map's background picture. UI THREAD ONLY: it decodes from the SD card, and
// the driver cannot be called from two tasks at once.
void ui_weather_art_attach(void);
void ui_weather_art_release(void);
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock);  // HUD: signal bars (count=RSSI, colour: red=down, amber=stale feed, white=ok) + clock
void ui_set_battery(int pct, bool charging, bool present);  // top HUD battery indicator
void ui_set_date(const char *date);  // top HUD date line (e.g. "08 Jun 2026")
void ui_set_gps(int state, int sats);   // GPS indicator: state 0=off/hidden 1=acquiring 2=fix
void ui_splash_show(void);  // branded boot splash (auto-fades, covers init time)
void ui_apply_theme(int theme);  // repaint the HUD chrome to match the active radar theme
// Range moved to Settings > Range (settings_view.cpp, host_set_range_km). The scope's own
// range label is fed by radar::update() from the settings struct, so nothing here needs to
// know about it. ui_set_netinfo() moved to settingsview::setNetInfo().
void ui_set_units(int preset);               // 0 = Aviation (ft,kt,km) · 1 = Metric (m,km/h,km) · 2 = Imperial (ft,mph,mi)
void ui_set_wx_units(bool imperial);         // Weather app only, independent of the aviation preset above
void ui_set_wx_zoom(int tier);               // 0 = 50mi · 1 = 100mi — Weather map display range
void ui_set_large_text(bool on);             // accessibility: bigger fonts everywhere. Call BEFORE ui_create()
void ui_set_weather_forecast(bool forecast); // false = WX radar, true = 3-day forecast
bool ui_weather_is_forecast(void);           // current weather sub-view, for the knob-push cycle
