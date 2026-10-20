// The Orb OS — settings: reading them from NVS at boot, letting the active theme override
// the ones it has an opinion on, and the setters the Settings app and the web page call.
//
// Moved out of main.cpp unchanged. Everything is still stored under the "capsuleradar"
// namespace, and must stay there: renaming it makes every Orb in the world look factory
// reset (see CLAUDE.md).
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <nvs.h>
#include "config.h"
#include "orb_state.h"
#include "orb_storage.h"
#include "orb_apps.h"
#include "radar_view.h"
#include "ui.h"
#include "display.h"
#include "audio.h"
#include "chime_library.h"
#include "clock_wind.h"
#include "theme_audio.h"
#include "theme_style.h"
#include "diag_log.h"

// Settings the ACTIVE THEME gets to override, applied after it has been read.
//
// These four lived at the bottom of loadSettings(), which runs at main.cpp's line ~1971 —
// three lines BEFORE theme_select::init() loads the theme. So every one of them tested a
// theme_style struct that was still all defaults and therefore never fired: a design could
// state range 100 km and 14 aircraft, have both written correctly into radar_style.json,
// have the device read that file at boot, and still draw 5 aircraft at 31 km from its own
// stored settings. Verified on the device with [acdbg]: "drawn=5 cap=5 rangeKm=31" against
// a theme file holding 14 and 100.
//
// Order is the whole fix. Nothing else about these rules changed.
void applyThemeSettings() {
    // The mainspring belongs to the design, so it is replaced whole every time one is
    // applied. A theme without one can never leave the clock stopped.
    {
        const theme_style::Clock &cs = theme_style::clock();
        clock_wind::applyTheme(cs.windOn, cs.windSecs, cs.windTurns, cs.windSound, cs.windNotice);
        // Its sounds come with it, on the same path, so a theme can never be half applied:
        // wearing one design's clock while clicking in another's voice.
        theme_audio::load();
    }
    const theme_style::Radar &rs = theme_style::radar();
    // -1 (or 0 for range) means "no opinion", leaving the Orb's own stored setting alone.
    if (rs.rangeKm     > 0.0f) g_settings.rangeKm = rs.rangeKm;
    // Clamped on the way in, not just where it is drawn. loadSettings() clamps what NVS
    // held, but this runs afterwards, so a theme written against the old 20/40 dials could
    // put a larger number straight back into g_maxAc. setMaxOnScreen would still cap what
    // reaches the scope, leaving every other reader of g_maxAc (the web UI's selected
    // option, and whatever gets re-persisted on the next save) holding a count this
    // firmware will not draw.
    if (rs.maxAircraft > 0)    g_maxAc     = (rs.maxAircraft > ADSB_MAX_AIRCRAFT)
                                             ? ADSB_MAX_AIRCRAFT : rs.maxAircraft;
    if (rs.minAltFt   >= 0)    g_minAltFt  = rs.minAltFt;
    if (rs.hideGround >= 0)    g_hideGround = (rs.hideGround != 0);
    // Clamped again here even though theme_style clamped it on the way in: this is the
    // value deadZoneKm() divides the dial by, and it is the one operational setting with
    // no device-side control, so there is nowhere to correct it from if it lands wrong.
    if (rs.deadZonePx >= 0)    g_deadZonePx = (rs.deadZonePx > (int)RADAR_R_OUTER_PX)
                                              ? (int)RADAR_R_OUTER_PX : rs.deadZonePx;
    // The simulated badge went with the thing it warned about. It existed because the
    // device could be made to show invented aircraft and somebody had to be told; it
    // cannot any more, so there is nothing to say.
    Serial.printf("[theme] applied: rangeKm=%.0f maxAircraft=%d minAltFt=%d hideGround=%d deadZonePx=%d\n",
                  (double)g_settings.rangeKm, g_maxAc, g_minAltFt, (int)g_hideGround, g_deadZonePx);
}

void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    // The owner's, and nothing outranks it. This used to be followed by a
    // CUSTOM_HAS_RADAR_HOME block that overwrote both from whichever design was last
    // compiled in, on every boot, without persisting anything: every unit flashed from one
    // push sat on that push's coordinates, the setup page's lat/lon box accepted an address
    // and silently discarded it, and GPS re-centring was compiled out to stop it fighting
    // back. A theme is data about how the Orb LOOKS; where it is standing is the owner's
    // to say, from the network it joined or from Settings.
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
    // Whether those two mean anything yet. The defaults are 0,0 and are never drawn: until
    // this is true the scope says so instead of guessing, and the feed is not polled.
    //
    // The fallback is isKey("homeLat"), not false, and that is the whole upgrade path. Every
    // Orb already in the world has a good homeLat in NVS and has never heard of locSet, so a
    // plain `false` would have told all of them they had no location, blanked their scope and
    // stopped their feed on the strength of a key this build invented. A stored coordinate IS
    // an established location; only a device that has never had one gets the new state.
    g_locationSet      = p.getBool("locSet", p.isKey("homeLat"));
    // Range, and below it max-aircraft, are read from the owner's saved settings here and
    // may then be overridden by the active theme in applyThemeSettings(), which runs after
    // this. The CUSTOM_RADAR_RANGE_KM / CUSTOM_RADAR_MAXAC blocks that used to sit in
    // between are gone: a value a theme does not state falls back to what this device has
    // saved, not to what somebody else's design happened to weld in.
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_tickVol          = p.getInt("tickVol", 20);
    g_chimeVol         = p.getInt("chimeVol", 30);
    audio_set_tick_level(g_tickVol);
    audio_set_chime_level(g_chimeVol);
    g_muted            = p.getBool("mute", false);
    g_soundRadar       = p.getBool("sndRadar", false);
    g_soundChime       = p.getBool("sndChime", false);
    g_alertMode        = p.getInt("alertmode", 2);
    g_proximityKm      = p.getFloat("proxkm", 0.0f);
    g_trailLen         = p.getInt("traillen", 2);
    g_maxAc            = p.getInt("maxac", 12);
    // Clamp after both sources: an Orb that ran an earlier build has a larger number sitting
    // in NVS (20, 40, 60), and a theme built before the ceiling moved can still carry one.
    // Neither should be able to reintroduce a count the scope no longer supports.
    if (g_maxAc > ADSB_MAX_AIRCRAFT) g_maxAc = ADSB_MAX_AIRCRAFT;
    if (g_maxAc < 1)                 g_maxAc = 1;
    g_idleDimMs        = p.getUInt("idledim", IDLE_DIM_MS);
    g_units            = p.getInt("units", 0);
    g_wxUnits          = p.getInt("wxUnits", 0);
    g_wxZoomTier       = 0;   // lean redesign: weather map is a single fixed 50mi range now
                              // (tier 0). Zoom is gone (see weather_press_cycle), so this is
                              // pinned to 0 regardless of any old saved "wxZoom2" value.
    g_tz               = p.getString("tz", TZ_STR);
    // Migrate off the old forked-in Spain default so a device that has it saved (from
    // before the default changed) still lands on local time. Re-locating overwrites this.
    if (g_tz == "CET-1CEST,M3.5.0,M10.5.0/3") g_tz = TZ_STR;
    g_bigText          = p.getBool("bigtext", false);
    g_chimeIdx         = p.getInt("chimeIdx", 0);
    // What this place is CALLED. Every path that sets a location already learns a name:
    // the Settings search, a Studio setloc, and the IP lookup all hand one to
    // host_recents_add(). Until now it was thrown away the moment the position was saved,
    // and the flight tracker had nothing but coordinates to show. Kept as its own pref so
    // the scope can name the place without reverse-geocoding anything, which is the whole
    // reason the Orb needs no city database for this (Lerxtwood's request, 2026-09-25).
    g_locName          = p.getString("homeName", "");
    p.end();
    audio_set_chime(g_chimeIdx);   // no hardware dependency, safe before audio_begin()
    // fonts are baked into the widgets at creation time, so the large-text flag must be
    // in place before display::begin() builds the UI (loadSettings runs first in setup)
    ui_set_large_text(g_bigText);
    radar::setLargeText(g_bigText);
}

// Persist the visual theme in NVS (called when the user long-presses to switch).
void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
    ui_apply_theme(t);   // keep the HUD chrome in sync with the scope's palette
    diag::log("theme -> %d", t);
}

// Settings > Reset: wipes WiFi credentials and every saved device setting (home
// location, recent cities, theme, brightness, everything under the "capsuleradar"
// Preferences namespace), then reboots straight into WiFi setup — meant for handing
// the device to someone else. Same countdown-warning pattern as host_set_location(),
// and the same WiFi-namespace erase the web /wifi-reset endpoint (handleWifi()) uses.
void host_factory_reset() {
    if (g_holdWarning && g_holdWarningLbl) {
        lv_obj_clear_flag(g_holdWarning, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_holdWarning);
        for (int s = 3; s >= 1; s--) {
            char buf[48];
            snprintf(buf, sizeof(buf), "FACTORY RESET\n\nRESTARTING IN %ds", s);
            lv_label_set_text(g_holdWarningLbl, buf);
            lv_refr_now(NULL);
            delay(1000);
        }
    } else {
        delay(250);
    }

    Preferences p;
    p.begin("capsuleradar", false);
    p.clear();
    p.end();

    g_wm.resetSettings();           // best-effort driver-level erase first...
    WiFi.disconnect(false, true);   // ...keep WiFi up so the erase can actually run
    delay(100);
    nvs_handle_t h;                 // ...then the guaranteed path: wipe the driver's namespace
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    // Written last, after the clear() above, so it survives it — the next boot reads
    // this and walks whoever's setting the device up straight into WiFi setup.
    Preferences p2;
    p2.begin("capsuleradar", false);
    p2.putBool("needsWifiSetup", true);
    p2.end();

    delay(200);
    ESP.restart();
}

int host_get_brightness() { return g_brightnessDay; }
void host_set_brightness(int v, bool save) {
    g_brightnessDay = constrain(v, 8, 255);
    display::setBrightness((uint8_t)g_brightnessDay);   // immediate preview, bypasses idle clamp
    if (save) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("bright", g_brightnessDay);
        p.end();
    }
}

// Screen-dim idle timeout (Settings > Display). 0 = always on / never dim.
uint32_t host_get_idle_ms() { return g_idleDimMs; }
void host_set_idle_ms(uint32_t ms) {
    g_idleDimMs = ms;
    display::noteActivity();                             // reset the idle clock so it doesn't dim mid-change
    Preferences p;
    p.begin("capsuleradar", false);
    p.putUInt("idledim", g_idleDimMs);
    p.end();
}

// --- Sound settings (on-device menu) ---
int  host_get_volume() { return g_volume; }
void host_set_volume(int v, bool save) {
    g_volume = constrain(v, 0, 100);
    audio_set_volume(g_volume);
    if (save) {
        Preferences p; p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.end();
    }
}
int  host_tick_volume() { return g_tickVol; }
void host_set_tick_volume(int v, bool save) {
    g_tickVol = constrain(v, 0, 100);
    audio_set_tick_level(g_tickVol);
    if (save) { Preferences p; p.begin("capsuleradar", false); p.putInt("tickVol", g_tickVol); p.end(); }
}
int  host_chime_volume() { return g_chimeVol; }
void host_set_chime_volume(int v, bool save) {
    g_chimeVol = constrain(v, 0, 100);
    audio_set_chime_level(g_chimeVol);
    if (save) { Preferences p; p.begin("capsuleradar", false); p.putInt("chimeVol", g_chimeVol); p.end(); }
}
bool host_sound_radar() { return g_soundRadar; }
void host_sound_set_radar(bool on) {
    g_soundRadar = on;
    Preferences p; p.begin("capsuleradar", false); p.putBool("sndRadar", on); p.end();
}
bool host_sound_chime() { return g_soundChime; }
void host_sound_set_chime(bool on) {
    g_soundChime = on;
    Preferences p; p.begin("capsuleradar", false); p.putBool("sndChime", on); p.end();
}
void host_sound_preview_chime() { if (audio_present()) audio_play(AUDIO_CHIME); }
void host_sound_preview_beep()  { if (audio_present()) audio_play(AUDIO_NEW); }

// The chime picker (Settings > Sound > Chime sound), now spanning every theme on the card as
// well as the ones baked into flash. Settings drives the picker entirely through these five
// functions and needed no changes at all to gain them.
//
// The hour belongs to the DEVICE, not to the worn theme: somebody wearing Steam Punk can ring
// Modern's chime without changing what their clock looks like. A sound you hear once an hour
// and a dial you look at all day are not the same choice.
//
// chime_library persists WHICH chime rather than its position in the list, because installing
// or deleting a theme moves everything after it and a stored index would quietly start meaning
// something else.
int  host_chime_count()            { return chime_library::count(); }
const char *host_chime_name(int i) { return chime_library::name(i); }
int  host_chime_index()            { return chime_library::selected(); }
void host_chime_set(int i)         { chime_library::select(i); }
void host_chime_preview(int i)     { if (audio_present()) chime_library::preview(i); }

// Restore the saved scope theme and display settings, then persist any future change.
// Runs in setup() once the theme is loaded; was an inline block there.
void applyStoredDisplaySettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    // One-time migration: Phosphor and Amber CRT were retired, leaving Orb/Military/
    // Aviator renumbered 0/1/2. Remap whatever's stored (old numbering: 0=Phosphor
    // 1=Orb 2=Amber 3=Military 4=Aviator) once; after that it's already in the new
    // scheme and this is a no-op. Default (no "theme" key at all) reads as old-scheme
    // Aviator so it lands on new Aviator too, same as everything else.
    const bool migratedV2 = p.getBool("themeMigV2", false);
    int t = p.getInt("theme", 4);
    g_showSweep = p.getBool("sweep", true);
    g_showAirports = p.getBool("airports", true);
    // Read only when the active theme has no opinion, and nothing follows this that
    // reads them again. Three CUSTOM_RADAR_{HIDEGROUND,MINALT,DEADZONE} blocks used to
    // sit immediately below and assign unconditionally, which made the guard above
    // ornamental: applyThemeSettings() applied the theme's altitude floor and ground
    // filter, and sixty lines later the macros put the last-pushed design's values back
    // over the top of them. A theme could state both, have both parsed correctly off
    // the card, see them logged as applied, and still fly neither.
    {
        const theme_style::Radar &rs = theme_style::radar();
        if (rs.hideGround < 0) g_hideGround = p.getBool("hideground", false);
        if (rs.minAltFt   < 0) g_minAltFt   = p.getInt("minalt", 0);
    }
    g_milOnly = p.getBool("milonly", false);
    // Migrate the old quarter-turn setting (rot=0..3) without changing existing
    // installations' orientation. New firmware stores actual degrees separately.
    g_rotation = p.isKey("rotDeg") ? p.getInt("rotDeg", 0) : p.getInt("rot", 0) * 90;
    g_rotation = constrain(g_rotation, 0, 359);
    p.end();
    if (!migratedV2) {
        switch (t) {                     // old numbering -> new (see comment above)
            case 1:  t = THEME_ORB;      break;
            case 3:  t = THEME_MILITARY; break;
            case 4:  t = THEME_AVIATOR;  break;
            default: t = THEME_AVIATOR;  break;   // old Phosphor(0)/Amber(2)/anything else
        }
        Preferences pw;
        pw.begin("capsuleradar", false);
        pw.putInt("theme", t);
        pw.putBool("themeMigV2", true);
        pw.end();
    }
    radar::setTheme(t);
    ui_apply_theme(t);   // setThemeChangedCb isn't registered yet — paint the HUD explicitly
    radar::setSweepEnabled(g_showSweep);
    radar::setAirportsEnabled(g_showAirports);
    g_adsb.setHideGround(g_hideGround);
    g_adsb.setMinAltFt((float)g_minAltFt);
    g_adsb.setMinDistKm(deadZoneKm());
    g_adsb.setMilitaryOnly(g_milOnly);
    radar::setTrailLength(g_trailLen);
    radar::setMaxOnScreen(g_maxAc);
    display::setRotation((uint16_t)g_rotation);
    g_rotation = display::rotation();
}
