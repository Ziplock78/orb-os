// The Orb OS — entry point: setup(), loop(), and the few things only they use (the
// hold-to-reboot warning, brightness, seeding the clock from the RTC).
//
// Everything else that used to live here is in its own file, moved without changing
// behaviour (stage 1 of REWRITE_PLAN.md):
//   orb_state     the globals more than one module shares
//   orb_net_task  the core 0 networking task: ADS-B poll, weather, headlines, lookups
//   orb_storage   settings in NVS, theme overrides, the Settings app's setters
//   orb_location  saving a position, IP lookup, geocoding, recent cities, timezone
//   orb_wifi      boot-time connect and portal, joins from Settings and the cable, mDNS
//   orb_web       the web server and every route on it
//   orb_apps      app shell hooks, alert sounds, range and weather-unit hooks
//   orb_stats     the boot RAM ledger, /health counters, loop phase timer, [memdbg]
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include <map>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "tls_memory.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "weather.h"
#include "weather_client.h"
#include "wx_radar.h"
#include "wx_radar_client.h"
#include "cloud_image.h"
#include "cloud_image_client.h"
#include "radar_view.h"
#include "radar_sprite.h"   // radar_sprite_release() — Flight Tracker's onExit
#include "custom_radar.h"             // CUSTOM_HAS_RADAR — a Launch Kit push changes the Flight Tracker knob's behavior
#include "ui.h"
#include "app_theme.h"
#include "theme_select.h"  // which Launch Kit theme (of however many are on the SD card) is active
#include "theme_art.h"     // pre-baked RGB565 art in flash: no SD read, no decode, no PSRAM
#include "theme_font.h"    // per-theme fonts, loaded from that same partition
#include "update_ui.h"     // on-screen "updating…" status, so mid-update never looks like broken
#include "orb_link.h"
#include "theme_pull.h"      // USB serial command channel: how a browser (Orb Studio) talks to this device
#include "custom_weld.h"   // CUSTOM_WELD_HASH — lets a push tell whether new firmware is needed
#include "orb_time.h"      // one honest read of the wall clock; NOT Arduino's retrying getLocalTime
#include "theme_style.h"   // per-theme app roster (theme_style::apps())
#include "clock_wind.h"    // the clock's virtual mainspring, THEME_CAPS 37
#include "wind_notice.h"   // ...and the panel that asks for it
#include "theme_audio.h"   // sounds a theme brings with it: wind.pcm, chime.pcm
#include "chime_library.h" // every chime on the device, from flash and from every theme
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "lv_psram_alloc.h"           // [lvmem]: what the LVGL allocator holds, and where
#include "imu_qmi8658.h"             // face-down sleep
#include "battery.h"                 // AXP2101 battery gauge
#include "rtc_pcf85063.h"            // PCF85063 RTC (offline clock + date)
#include "audio.h"                   // ES8311 alert pings
#include "knob.h"                    // rotary encoder on the 8-pin header
#include "app_shell.h"               // "channel changer": knob flips between apps
#include "input_router.h"            // shared knob->app_shell routing (device + sim)
#include "diag_log.h"                // RTC-memory event ring buffer, survives a reboot
#include "sdcard.h"                  // microSD (TF) slot, SPI mode
#include "roads_sd.h"                // worldwide roads read off the SD card
#include "clock_view.h"              // clock app (app two)
#include "weather_view.h"           // animated weather-radar app (knob channel)
#include "settings_view.h"          // settings app (menu; captures the knob)
#include "custom_boot_target.h"       // CUSTOM_BOOT_TARGET — set by whichever Launch Kit push (clock/splash/radar) ran last
#include "custom_apps.h"              // CUSTOM_APP_* — which apps a theme flash includes in the menu
#include "spycam_view.h"             // Spy Cam: looping "security camera" flip-book
#include "intel_view.h"
#include "ticker_view.h"
#include "ticker.h"              // world headlines, read through the gateway
#include <set>                       // audio: track which contacts are in range
#include <string>
#include <WiFiManager.h>             // captive portal
#if __has_include("secrets.h")
#include "secrets.h"                 // optional, gitignored: DEV_WIFI_SSID / DEV_WIFI_PASS
#endif
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <esp_heap_caps.h>          // heap_caps_malloc_extmem_enable(), [memdbg]
#include "orb_state.h"              // shared globals, ORB_OTA_ENABLED
#include "orb_net_task.h"
#include "orb_storage.h"
#include "orb_location.h"
#include "orb_wifi.h"
#include "orb_web.h"
#include "orb_apps.h"
#include "orb_stats.h"

// Convert a UTC broken-down time to time_t (mktime assumes local TZ, so flip to UTC0).
static time_t utc_to_time(struct tm *utc) {
    setenv("TZ", "UTC0", 1); tzset();
    const time_t t = mktime(utc);
    setenv("TZ", g_tz.c_str(), 1); tzset();   // restore the loaded/located local TZ for getLocalTime()
    return t;
}

// Seed the ESP system clock from the RTC so the clock/date are right before NTP.
static void rtc_seed_clock() {
    struct tm utc;
    if (!rtc_read(&utc)) { Serial.println("[rtc] no valid time stored"); return; }
    const time_t t = utc_to_time(&utc);
    struct timeval tv = { t, 0 };
    settimeofday(&tv, nullptr);
    Serial.println("[rtc] system clock seeded from RTC");
}

// Brightness combines idle auto-dim and face-down sleep (sleep wins -> screen off), and
// an update in progress wins over everything: a theme arriving, a firmware flash about to
// start or the bake after a restart all put the panel at full brightness, however dim the
// owner keeps it and however long it has sat idle. update_ui drives that flag.
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static bool g_updateBright = false;   // an update surface is up (update_ui)
void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_asleep) b = 0;                                         // face-down -> screen off
    if (g_updateBright) b = 255;                                 // an update outranks both
    display::setBrightness(b);
}

// update_ui's hook (declared extern there). The transfer counts as activity too, so the
// idle clock starts over when the notice comes down rather than dimming the very next tick
// because the install outlasted the idle time.
void host_update_bright(bool on) {
    g_updateBright = on;
    display::noteActivity();
    g_idle = false;
    applyBrightness();
}

// Recovery-reboot warning: the knob's hold-to-reboot is meant for a genuinely stuck
// device, but a silent multi-second countdown means a thumb resting on the button
// during normal browsing can trigger it by accident with zero warning. This shows a
// big on-screen countdown starting partway into any hold, so you can just let go.
static constexpr uint32_t HOLD_WARNING_START_MS = 2500;   // when the countdown appears
lv_obj_t *g_holdWarning    = nullptr;
lv_obj_t *g_holdWarningLbl = nullptr;

static void build_hold_warning() {
    g_holdWarning = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_holdWarning);
    lv_obj_set_size(g_holdWarning, SCREEN_W, SCREEN_H);
    lv_obj_center(g_holdWarning);
    lv_obj_set_style_bg_color(g_holdWarning, lv_color_hex(0x3A0000), 0);
    lv_obj_set_style_bg_opa(g_holdWarning, LV_OPA_80, 0);
    lv_obj_clear_flag(g_holdWarning, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    g_holdWarningLbl = lv_label_create(g_holdWarning);
    lv_obj_set_style_text_color(g_holdWarningLbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(g_holdWarningLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(g_holdWarningLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(g_holdWarningLbl);
    lv_obj_add_flag(g_holdWarning, LV_OBJ_FLAG_HIDDEN);
}

// Called every loop(): shows/updates the countdown while held past the warning
// threshold, hides it the instant the button is released (or once it actually reboots).
static void update_hold_warning() {
    if (!g_holdWarning) return;
    const uint32_t held = knob::heldMs();
    if (held < HOLD_WARNING_START_MS) {
        lv_obj_add_flag(g_holdWarning, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const uint32_t total = knob::longPressMs();
    const uint32_t remainMs = (held >= total) ? 0 : (total - held);
    char buf[64];
    snprintf(buf, sizeof(buf), "RELEASE TO CANCEL\n\nREBOOTING IN %lus",
             (unsigned long)((remainMs + 999) / 1000));
    lv_label_set_text(g_holdWarningLbl, buf);
    lv_obj_clear_flag(g_holdWarning, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_holdWarning);
}


// Theme switching asked for over the USB cable. Same two rules as the /theme endpoint it
// mirrors: only ever switch to a slug that is genuinely installed (a typo would otherwise
// leave the device pointed at an empty folder, drawing stock art and looking broken), and
// defer the actual switch, because theme_select::set() reboots and the caller needs its
// answer first. Shared with orb_link rather than duplicated so the two front doors cannot
// drift apart.
static bool request_theme_switch(const char *slug) {
    if (!slug || !*slug) return false;
    static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
    const int n = theme_select::listInstalled(slugs);
    for (int i = 0; i < n; ++i) {
        if (!strcmp(slug, slugs[i])) {
            g_pendingSlug   = slug;
            g_applySlugAtMs = millis() + 400;
            return true;
        }
    }
    return false;
}

void setup() {
    // Room for one full orb_link put-data line (~400 bytes) to ARRIVE IN ONE USB burst
    // while loop() is busy rendering a frame. The default RX ring is 256 bytes, so a
    // long line overflowed it, the tail was dropped, and the half-line was discarded by
    // the overflow guard — which read as "put-data never gets a reply" while short
    // commands worked perfectly. Must be set before begin().
    Serial.setRxBufferSize(4096);
    Serial.begin(115200);
    delay(200);
    Serial.println("\nThe Orb OS boot");
    orb_link::begin();
    orb_link::setThemeRequestHook(request_theme_switch);
    orb_link::setWifiJoinHooks(serial_wifi_join, serial_wifi_join_status);
    theme_pull::begin();
    theme_pull::setSwitchHook(request_theme_switch);
    diag::boot();   // print + continue the RTC-memory event history across this reboot

    // RTC_NOINIT holds whatever was in it, including rubbish after a real power cycle, so it
    // is only trusted when the companion magic says we wrote it. Same guard diag_log uses.

    // Ordinary malloc/new allocations >=512 bytes go to PSRAM. The stock
    // mbedTLS allocator explicitly requests internal RAM, so TLS needs the
    // separate runtime callbacks installed below as well.
    //
    // 512, NOT 4096, SINCE 2.16.65. Everything under 4 KB still landed on the internal heap:
    // HTTP and socket buffers, JSON documents, Strings, the aircraft vector. None of them are
    // large, all of them are allocated and freed constantly, and over a long uptime that churn
    // cuts the internal heap into pieces even while plenty of it is free. That is the state
    // everything else fails from. Greg Takacs named it in #fix-requests on 2026-10-06, "a
    // memory starvation problem across the board", and Techtobi83 had already measured it in
    // their fork: a soak test ended in a task-watchdog reset with the WiFi driver unable to
    // get a transmit buffer, largest internal block around 5 KB. At 512 they measured the
    // largest block back at 18.4 KB.
    //
    // Nothing that must be internal is affected, because nothing that must be internal relies
    // on this. Checked rather than assumed: the LVGL draw buffer asks for
    // MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA, the audio scratch and the rotation and frame
    // buffers ask for MALLOC_CAP_SPIRAM, task stacks and the WiFi driver's own allocations ask
    // by capability, and LVGL routes its heap through orb_lv_malloc with its own threshold.
    // This moves the ordinary malloc/new/String traffic and nothing else.
    //
    // What it trades is speed for room: that traffic now sits behind the slower external bus.
    // None of it is in the render loop. If the Orb feels slower rather than steadier, this is
    // the line to put back.
    heap_caps_malloc_extmem_enable(512);
    tls_memory_begin();  // before WiFi/tasks: TLS allocations explicitly use PSRAM

    psram_mark("boot start");
    sdcard::begin();
    psram_mark("after sdcard");

    if (PIN_LCD_SCLK < 0 || PIN_I2C_SDA < 0) {
        Serial.println("[!] Pins in config.h are still -1. Copy them from the Waveshare demo.");
    }
    Serial.printf("PSRAM: %u bytes free\n", (unsigned)ESP.getFreePsram());

    // Reserve the roads projection buffers now, while PSRAM is fresh — by the time
    // the Flight Tracker first runs, the custom design's plate/overlay (~1MB) plus
    // weather/spycam churn have fragmented the pool and a mid-session grab fails.
    roads_sd::init();

    loadSettings();
    route_cache_begin();   // clear stale route cache if the label format changed
    app_theme::init();     // load the saved app skin (Default/Office) before any view reads it
    theme_select::init();  // load the saved Launch Kit theme slug before any screen reads it
    applyThemeSettings();  // ...and only NOW can the theme's own range/count/altitude win
    psram_mark("after theme_select");

    // Map the pre-baked art partition. Must come after theme_select::init() (it needs
    // the active slug) and before any view asks for artwork. The BAKE, though, now runs
    // after display::begin(): it used to run here, before the panel was initialised, so
    // the ~15 s conversion after a theme push happened over a dead screen and looked
    // exactly like a hang. Deferred so the user can watch it instead.
    theme_art::begin();
    psram_mark("after theme_art");

    // --- Display + LVGL (M0) ----------------------------------------------
    // CO5300 AMOLED over QSPI + LVGL draw buffers in PSRAM, then a hello screen.
    // The panel is powered from the always-on DC1 rail, so it lights without the
    // PMIC. Touch (CST9217 indev) + AXP2101 come in later milestones.
    if (!display::begin()) {
        Serial.println("[!] display::begin() failed — check QSPI pins / power.");
    }
    // The owner's own brightness, from here on. display::begin() lights the panel at
    // BRIGHTNESS_DEFAULT and nothing used to correct that until the first idle or wake,
    // so a level set in Settings > Display came back only after the screen had dimmed
    // once. loadSettings() ran above, so the saved value is already in g_brightnessDay.
    applyBrightness();
    // The bake, now that there is a screen to narrate it on. Only does real work on the
    // first boot after a theme push; the progress callback puts "Preparing theme, k of n"
    // on update_ui's plain panel while it grinds, which is the second-restart leg of an
    // update the user was previously left to guess about.
    //
    // The panel is still dark here: ui_create() no longer raises the splash, so nothing has
    // painted yet. The order below is the one Zion asked for after watching an install:
    // the title card must not appear until the theme is installed, and then it holds for
    // three seconds and gives way to the clock. It used to go up first and carry the bake
    // progress in small type along its bottom edge, which read as "finished, but not".
    // So: bake behind the plain panel, THEN the splash, THEN the panel comes down under
    // it (bake_done repaints, and with the splash already over everything that repaint
    // never shows the Flight Tracker for a frame).
    theme_art::set_progress([](const char *name, int done, int total) {
        if (done == 0 && !name) update_ui::bake_begin(total);
        else update_ui::bake_progress(name, done, total);
    });
    {
        // A /rebake asked for from the web page, carried across the restart. Cleared before
        // the bake rather than after, so a bake that panics cannot wedge the device in a
        // re-bake loop.
        Preferences rp;
        rp.begin("capsuleradar", false);
        const bool forced = rp.getBool("rebake", false);
        if (forced) rp.putBool("rebake", false);
        rp.end();
        if (forced) Serial.println("[theme_art] /rebake: honouring the request from before the restart");
        theme_art::bake_active_theme(forced);
    }
    ui_splash_show();          // the theme's title card, clean, with the install behind it
    update_ui::bake_done();    // no-op on an ordinary boot
    // After the bake and after lv_init(): the font loader reads the freshly baked fonts,
    // and every view built below gets the theme's typography on its first label.
    theme_font::begin();
    build_hold_warning();   // hold-to-reboot countdown, sits above every app

    applyStoredDisplaySettings();   // restore the saved theme, then persist any future change (orb_storage.cpp)
    radar::setThemeChangedCb(saveTheme);
    ui_set_units(g_units);                       // apply saved unit preset
    ui_set_wx_units(host_wx_is_imperial());      // apply saved (or auto-resolved) weather units
    ui_set_wx_zoom(g_wxZoomTier);                 // apply saved weather map zoom tier

    knob::begin();     // rotary encoder on GPIO16/17/18

    // --- App shell: the knob flips between full-screen apps ------------------
    // Clock goes first (app index 0, the boot app) on purpose: it's the only app that
    // needs no network data to be fully correct (RTC-seeded time), so it's what should
    // be sitting there the instant the splash clears, while WiFi/ADS-B/weather are still
    // connecting in the background. See the UI-pump loop below, right before the
    // blocking WiFi connect call, for the other half of that plan.
    // display::begin() already built the radar UI onto the active screen; Flight Tracker
    // and Weather Radar share that screen: "Weather" just jumps its tileview to the
    // original app's weather tile (view 3). List/Stats stay touch-swipe-only (swipe
    // right from Radar) — they don't get their own knob-menu entry. Push while on Radar
    // cycles the visual theme (Phosphor/Orb/Amber/Military/Aviator).
    // App lineup, in app_shell::Slot order. Every app is always registered (so indices never
    // shift), but the ones a Launch Kit theme flash turns off (custom_apps.h) are
    // marked hidden — still built, just skipped when the knob cycles the menu. The
    // default custom_apps.h has all apps on, so a stock build / single-screen push
    // is unchanged.
    lv_obj_t *radarScreen = lv_scr_act();
    psram_mark("after display+radar");
    clockview::init();
    psram_mark("after clockview");
    // onEnter takes the canvas, onExit gives it back. It answers neither a turn nor a press.
    // The Clock stays built for ever. It is registered eagerly, which app_shell treats as
    // pinned: it is the screen people come back to, and a rebuild would be felt there more
    // than anywhere else. See the residency note in app_shell.h.
    app_shell::add(clockview::screen(), theme_style::names().clock, nullptr, nullptr, false, clockview::onEnter, clockview::onExit, !theme_style::apps().clock);
    app_shell::add(radarScreen, theme_style::names().flight, radar_press_custom_or_theme, radar_turn_select, false, radar_show_home_custom, radar_exit_release_style, !theme_style::apps().flight);
#if !APPS_LAUNCH_ONE
    app_shell::add(radarScreen, theme_style::names().weather,  weather_press_cycle, nullptr, false, radar_show_weather, radar_hide_weather, !theme_style::apps().weather);
    spycamview::init();
    psram_mark("after spycamview");
#endif
#if !APPS_LAUNCH_ONE
    app_shell::add(spycamview::screen(), theme_style::names().surveillance, spycamview::onPress, spycamview::onTurn, false, nullptr, nullptr, !theme_style::apps().surveillance);  // push cycles cams; clip loads lazily on commit
#endif
    // Intel before Settings. It used to be appended after, purely because the jumps below
    // were written as bare integers and moving anything would have pointed the jumps at
    // the wrong screen. They name app_shell::Slot now, so the menu can be ordered the way it
    // should read: Settings last, after everything it configures.
    // Built the first time it is opened, like Settings. ~303 LVGL allocations and ~7.4 KB
    // of internal RAM. Its network step keeps running while the screen is unbuilt, so the
    // headlines are current when you do open it — see the note in intel_view.h.
    app_shell::addLazy(theme_style::names().headlines,
                       intelview::build, intelview::destroy,
                       intelview::onPress, intelview::onTurn, false,
                       intelview::onEnter, intelview::onExit, !theme_style::apps().headlines);  // push fetches now, or toggles scroll mode when the type size overflows; onEnter resets to the top
    psram_mark("after intelview (lazy)");
#if !APPS_LAUNCH_ONE
    tickerview::init();
    psram_mark("after tickerview");
    app_shell::add(tickerview::screen(), theme_style::names().ticker,
                   tickerview::onPress, tickerview::onTurn, false,
                   tickerview::onEnter, tickerview::onExit, !theme_style::apps().ticker);  // turn steps the watchlist; onEnter takes the strip canvas only when the design curves it
#endif
    // Settings is built the first time somebody opens it, not here. It was the single
    // largest holder of internal RAM on the device — 909 LVGL allocations and ~24 KB,
    // measured 2026-10-06 — for the screen reached least often, and internal RAM is the
    // pool that runs out (docs/memory.md). app_shell keeps it resident afterwards under
    // RESIDENT_CACHE, so coming straight back to it costs no rebuild.
    app_shell::addLazy(theme_style::names().settings,
                       settingsview::build, settingsview::destroy,
                       settingsview::onPress, settingsview::onTurn,
                       true, settingsview::onEnter, settingsview::onExit, false);  // captures the knob on entry; onEnter resets to the menu and takes the text canvas, onExit gives it back
    psram_mark("after settingsview (lazy)");
    // Before anything jumps to a slot by name. See app_shell::verifySlots(): the enum and
    // the registration order above have drifted apart twice, and both times the only
    // symptom was the wrong screen appearing with nothing said about it.
#if APPS_LAUNCH_ONE
    // A theme is allowed to ask for an app this build does not carry — theme.json's roster
    // is the design's opinion about a finished product, and TC-008 keeps those keys alive
    // whether or not this firmware acts on them. But asking and silently getting nothing is
    // the shape of every fault this week, so it is said out loud once, here, naming the app.
    {
        const theme_style::Apps &ta = theme_style::apps();
        const struct { bool want; const char *name; } cut[] = {
            { ta.weather,      "Weather Radar" },
            { ta.surveillance, "Surveillance"  },
            { ta.ticker,       "Stock Ticker"  },
        };
        for (const auto &c : cut)
            if (c.want)
                Serial.printf("[shell] theme asks for \"%s\" but this build does not carry it "
                              "(CUT-01, APPS_LAUNCH_ONE in config.h). Its settings are still "
                              "read and kept; nothing draws them.\n", c.name);
    }
#endif
    // By build function, not screen: a lazy Settings has no screen yet. Still a pointer
    // comparison, so the check that caught two silent slot reorders is undiminished.
    app_shell::verifySlots(nullptr, settingsview::build);
    app_shell::begin();                // start on the clock (index 0 — see comment above)
    psram_mark("after app_shell::begin");

    // Fresh out of the box, or right after Settings > Reset: skip the clock and walk
    // straight into WiFi setup instead — see host_factory_reset().
    bool wantWifiSetup = false;
    {
        Preferences p;
        p.begin("capsuleradar", true);
        // Set by Settings > Reset. NOT set on a factory-fresh board: an erased NVS reads
        // the default, false, which is why a brand-new Orb never got this prompt and sat
        // on a clock instead. The real answer comes from autoConnect below, which is the
        // only thing that actually knows whether there is a network to join.
        wantWifiSetup = p.getBool("needsWifiSetup", false);
        p.end();
#if CUSTOM_BOOT_TARGET == 1
        // Set only by the splash push (the clock push clears it, even if a custom
        // splash is still baked in) — so this is genuinely "you just pushed the
        // splash," not "a custom splash happens to exist." Lands on the About page,
        // which holds the same splash art up indefinitely (push the knob to leave)
        // instead of the ordinary 2s-hold-then-fade, so it stays put to look at.
        else {
            app_shell::selectApp(app_shell::APP_SETTINGS);
            app_shell::setCaptured(true);
            settingsview::openAboutPage();
        }
#elif CUSTOM_BOOT_TARGET == 2
        // Set only by a Flight Tracker push — boots straight into it instead of
        // landing on the clock and making you swipe/switch over, since a radar
        // push is almost always "I just changed this one screen, go look at it."
        // Selecting APP_FLIGHT runs Flight Tracker's onEnter, which captures the knob for
        // aircraft selection when a custom design is active (see
        // radar_show_home_custom) — left captured on purpose: turning the knob
        // should select an aircraft immediately, not open the switcher, since
        // that's the whole point of landing here. Press the knob to leave
        // selection mode and reach the switcher/menu (Settings -> WiFi etc.) —
        // same gesture Settings itself already uses to back out.
        else {
            app_shell::selectApp(app_shell::APP_FLIGHT);
        }
#endif
    }

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", g_tz.c_str(), 1); tzset();   // local time for display even before NTP (loadSettings ran above)
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    clock_wind::begin();            // the stored wind, before any screen asks about it
    chime_library::begin();         // every chime on the card, and which one was chosen
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // The splash stays up, frozen, through everything below. Its hold-then-fade timer only
    // advances while lv_timer_handler() runs, and the pump that drives it now sits at the
    // very END of setup(), after WiFi. It used to sit here, which revealed the clock early
    // and then drew a black "connecting" notice over it, and a "Ready" card after that:
    // the clock appeared, so it looked finished, and then it visibly was not. The order
    // Zion asked for is finish everything, then the splash for three seconds, then the
    // clock, and nothing after. The next call blocks for up to twenty seconds, so the
    // splash says what is happening on its own status line meanwhile.
    update_ui::booting("Connecting to your network");

    wifi_boot_connect(wantWifiSetup);   // captive portal or stored network; orb_wifi.cpp
    // UX-024, and it comes before the WiFi choice on purpose. Both are things the Orb is
    // missing, and the card is the one that needs somebody to go and find a physical
    // object, so it is the one worth saying while they are still standing at the desk.
    //
    // The notice is dismissible and the boot continues either way: the device really does
    // run without a card, on the flash-baked artwork, so refusing to start would break a
    // working Orb in order to report a degraded one. What it must not do is stay silent,
    // which is what it did until now — sdcard::begin() returned false, printed one line to
    // a serial port nobody was watching, and the owner was left to work out for themselves
    // why no design would install.
    if (!sdcard::mounted()) {
        app_shell::selectApp(app_shell::APP_SETTINGS);
        app_shell::setCaptured(true);
        settingsview::openNoSdCardNotice(wantWifiSetup);   // chains into WiFi setup if that is owed too
    } else if (wantWifiSetup) {
        app_shell::selectApp(app_shell::APP_SETTINGS);
        app_shell::setCaptured(true);   // Settings captures the knob on entry; match that
        settingsview::openWifiSetupPrompt();
    }

    // --- OTA ---------------------------------------------------------------
    // ArduinoOTA is started from loop() once WiFi connects (see otaUp there).

    // --- ADS-B client + task ----------------------------------------------
    float queryKm = queryRadiusKm();
    g_adsb.begin(g_settings.homeLat, g_settings.homeLon, queryKm);
    // Four 360x360 RGB565 frame buffers, ~1 MB, and until now allocated even when the
    // active theme has Weather Radar switched off. Same reasoning as the Surveillance
    // clip buffer: an app you cannot reach should not be holding a quarter of the memory
    // the visible screens are competing for.
    // NOT here, for either branch. The weather app takes its buffers when it is opened,
    // from the network task that owns them (see radar_show_weather), and an app the theme
    // has switched off can never be opened, so it never asks. The skip is implicit.

    // cloud_image_begin() intentionally not called: the satellite-cloud view was dropped
    // from the Weather app's knob cycle, so its two full-frame PSRAM buffers (~0.5MB) would
    // just sit unused. That memory goes to the Weather Radar animation frames instead.
    g_ac_mutex = xSemaphoreCreateMutex();
    psram_mark("before adsb task");
    xTaskCreatePinnedToCore(adsb_task, "adsb", 7168, nullptr, 1, &g_adsbTaskHandle, 0);
    // 7168, not 6144. 6144 was tried and MEASURED: under a real fetch the high-water mark
    // fell to 1,012 bytes of headroom, which is not a margin, it is a fuse. A FreeRTOS stack
    // overflow is a hard crash and this task runs unattended for weeks. So the saving here is
    // 1 KB rather than 2; the real memory came from audio (reserving 4 KB to use 0.8) and
    // from keeping allocations off internal RAM in the first place.
    //
    // Measured on the device (uxTaskGetStackHighWaterMark over multi-minute soaks, the feed
    // both succeeding and failing): peak usage ~5.3 KB. Was 16 KB, then 8 KB, now 6 KB.
    //
    // A task stack is INTERNAL RAM, which is the resource this device actually runs out of:
    // a clean boot leaves about 9.5 KB free, and a TCP connect plus an HTTP request needs
    // more than that at peak. Every kilobyte reserved and never touched here is a kilobyte
    // the network cannot have. 6144 keeps ~800 B over the observed peak, and the high-water
    // mark is printed every 15 s in [memdbg] so the margin is watched rather than assumed.
    // The old comment here said "TLS needs a big stack"; that stopped being true when the
    // TLS fallback was disabled above, and even the plain-HTTP path never came close to
    // justifying the size. 8 KB keeps roughly double the observed peak as margin and returns
    // 8 KB of internal RAM, which is the exact resource the feed is starved for.

    web_server_begin();   // configuration web page (http://theorb.local/); orb_web.cpp

    // Everything is up. Noted for the log only: the "Ready, turn the knob" card that used
    // to gate a first boot after a firmware update is gone. It answered "is it finished?"
    // by interrupting a clock that was already showing, which is the one thing the
    // update_ui contract says must never happen. The answer now is the clock itself,
    // arriving once, after the splash, with nothing behind it.
    bool freshFirmware = false;
    {
        Preferences p;
        if (p.begin("orb", false)) {
            freshFirmware = p.getString("fwseen", "") != FW_VERSION;
            if (freshFirmware) p.putString("fwseen", FW_VERSION);
            p.end();
        }
    }
    update_ui::booted();

    // Now, and only now, the splash gets its three clean seconds and its fade. The clock
    // is already built underneath it (app_shell::begin() above, app index 0), seeded from
    // the RTC, so what the fade reveals is a live, correct clock and the boot is over.
    {
        const uint32_t pumpUntil = millis() + 3000 + 600 + 150;   // hold + fade + margin
        while (millis() < pumpUntil) {
            lv_timer_handler();
            delay(5);
        }
    }

    Serial.printf("setup done (firmware %s, %s)\n", FW_VERSION,
                  freshFirmware ? "first boot after an update" : "already seen this build");
}

void loop() {
    uint32_t lpT[LP_N + 1];
    lpT[0] = micros();
    // INPUT FIRST. The encoder is interrupt-driven, so no detent is ever lost — but this
    // used to run at the BOTTOM of the loop, after a full LVGL render and after the web
    // server. A detent arriving while the screen was drawing therefore waited for that
    // draw to finish, then the network work, then a SECOND full draw before anything
    // moved: two frames of latency on every turn, which is what made the knob feel
    // sluggish. Handling it first means a turn is acted on by the very next render.
    knob::poll();                   // drain detents/presses accumulated by the ISR
    // Before the input is routed, so a clock that ran out one second ago is already asking
    // by the time the next detent arrives rather than one frame later. Cheap: it compares
    // two integers unless the answer has actually changed.
    wind_notice::tick();
    // And step the crank toward the knob, on the loop's clock rather than on a timer of its
    // own. Returns immediately unless the wind screen is up.
    wind_notice::animate();
    update_hold_warning();          // countdown while the button is held (see build_hold_warning)
    if (knob::takeLongPress()) {    // held ~8 s -> manual recovery reboot
        Serial.println("[main] knob long-press -> reboot");
        diag::log("knob long-press -> reboot (app %s)", app_shell::name());
        delay(50);
        ESP.restart();
    }
    {
        int32_t kd = knob::takeDelta();
        bool pressed = knob::takePress();
        if (kd != 0 || pressed) {
            display::noteActivity();    // knob use keeps the screen awake
            // And brings it back NOW. The idle check below runs on the IMU's 400 ms tick,
            // and the first stranger to build one reported that a twist or a press did not
            // wake the dimmed screen while a rock did; whatever the panel was doing with the
            // deferred write, the knob is the one thing that must never fail to wake it, so
            // the brightness goes back on the same loop as the detent.
            if (g_idle) { g_idle = false; applyBrightness(); }
        }
        if (pressed) diag::log("push (app %s, browsing=%d, captured=%d)",
                               app_shell::name(), app_shell::browsing(), app_shell::captured());
        input_router::dispatch((int)kd, pressed);           // same 3-mode routing the sim uses
    }
    input_router::tick();                                    // a settling rock, resolved without new input

    lpT[1] = micros();
    display::loop();                // drive LVGL (render dirty areas + run timers)

    // Network and sensors last: they are throughput work, not interactive. handleClient()
    // in particular can spend real time on an /sdput chunk, and nothing about it should
    // sit between a knob turn and the frame that answers it.
    lpT[2] = micros();
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    web_server_handle();            // serve the configuration web page
    orb_link::poll();               // answer Orb Studio over the USB cable (bounded, non-blocking)
    lpT[3] = micros();
    // Mid-install, lean into the port instead of the screen. Every chunk needs a round
    // trip through this loop, so at the radar's ~77 ms frame the transfer crawled at one
    // chunk per frame: 5 KB/s, against 18 KB/s with the loop free. The display is showing
    // the update overlay throughout, so the frames being skipped here are frames of a
    // screen nobody is looking at. Bounded so the knob and the watchdog still get their
    // turn even if the host stalls mid-file.
    if (orb_link::transferActive()) {
        const uint32_t until = millis() + 40;
        while (orb_link::transferActive() && (int32_t)(millis() - until) < 0) orb_link::poll();
    }

    // Themes arriving over WiFi: one file per pass, from here because the card is only
    // ever touched from this task. The update panel is up throughout, so the frames this
    // costs are frames of a screen nobody is looking at.
    if (theme_pull::active()) theme_pull::step();
    serial_wifi_join_tick();   // a join asked for over the cable; a no-op otherwise

    // THE FORCED RE-BAKE IS NOT DONE HERE, and the first version of it was.
    //
    // Baking decodes PNGs, and decode_png wants 424-651 KB of PSRAM for its output on top of
    // the ~355 KB it takes to read the file. At boot there is 7902 KB free and every asset
    // fits. From loop() the live screen is holding its art — canvas, rotation cache, hand
    // layers — and the measurement that caught this was 545 KB free: the fonts baked, being
    // raw copies that decode nothing, and every plate and every animation frame either
    // failed to decode or could not even be read. The endpoint "succeeded" and left the
    // device with FEWER frames baked than before it ran.
    //
    // So /rebake records the request and restarts, and setup() does the work where the
    // memory is. See the Preferences flag read there.

    // scheduled reboot after a fresh WiFi config (see setSaveConfigCallback)
    if (g_rebootAtMs && (int32_t)(millis() - g_rebootAtMs) >= 0) { delay(50); ESP.restart(); }

    // deferred theme switch from /theme (set() persists to NVS and reboots by itself)
    if (g_applySlugAtMs && (int32_t)(millis() - g_applySlugAtMs) >= 0) {
        g_applySlugAtMs = 0;
        // Say so BEFORE going down. theme_select::set() holds a 700 ms delay for exactly
        // this notice ("hold the 'restarting...' notice on screen long enough to actually
        // read"), but nothing on this path ever put one up: an install over the cable
        // showed "receiving files", then the screen died mid-count with no explanation,
        // which reads as a crash rather than as the middle of an update. Both the cable
        // and the WiFi push land here, so one call covers both.
        update_ui::rebooting();
        theme_select::set(g_pendingSlug.c_str());
    }

    wifi_mdns_tick();   // mDNS (and OTA when compiled in), once WiFi is up; orb_wifi.cpp

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    //
    // The swap itself always runs: it costs a pointer exchange under a 5ms lock, nothing
    // more, and is what lets g_snap hold real data even while nobody is looking at it.
    // radar::update() is the expensive half (coastline reprojection, glyph rebuilding, the
    // LVGL compositing tonight's whole sweep-smoothness investigation was about), and it
    // has no reason to run against a screen that is not the one on the glass right now, so
    // it is gated on g_radarViewActive below. checkAudioEvents() stays unconditional on
    // purpose: an emergency squawk is worth a ping whichever screen someone is looking at.
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            checkAudioEvents();                // ping new-in-range / emergency / military, any screen
            if (g_radarViewActive) {
                // Timed because this is the one chunk of per-poll work that lands on the
                // RENDER thread: everything else about a poll happens on core 0. If a poll
                // costs a visible stutter, this is where it is spent.
                const uint32_t upd0 = micros();
                radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
                const uint32_t upd1 = micros();
                ui_on_data_updated();              // refresh card/list/stats
                const uint32_t upd2 = micros();
                // Only when it actually costs a frame. This ran on every poll while the
                // sweep stutter was being hunted, which is the right instrument but the
                // wrong volume once it is fixed: a healthy snapshot is ~25 ms and saying so
                // twice a second buries everything else on the console. 40 ms is half a
                // frame at the radar's ~13 fps, so anything printed here is a real regression.
                const uint32_t total = upd2 - upd0;
                if (total > 40000UL) {
                    Serial.printf("[perf] SLOW snapshot %u ac: update %lu us, ui %lu us, total %lu us\n",
                                  (unsigned)g_snap.size(),
                                  (unsigned long)(upd1 - upd0), (unsigned long)(upd2 - upd1),
                                  (unsigned long)total);
                }
            }
        }
    }
    // The catch-up render. A poll that landed while Flight Tracker was NOT on screen set
    // g_acDirty, which the block above deliberately ignored for the drawing half, so
    // g_snap can be sitting on real, current traffic the moment someone actually switches
    // over, with s_acs (radar_view's own render state) never told about any of it. This
    // fires exactly once, on the rising edge of g_radarViewActive, and draws from whatever
    // g_snap already holds, background-warmed data or not, so a scope that was already
    // being watched needs no catch-up (nothing to add) and a scope someone just switched
    // to shows what is actually out there instead of an empty dial waiting on the next
    // poll interval.
    {
        static bool s_wasRadarVisible = false;
        if (g_radarViewActive && !s_wasRadarVisible) {
            radar::update(g_snap, g_settings);
            ui_on_data_updated();
        }
        s_wasRadarVisible = g_radarViewActive;
    }
    if (g_weatherDirty) {
        g_weatherDirty = false;
        ui_on_data_updated();
    }
    // Headlines arrived on core 0; the labels are LVGL objects and may only be written
    // here. Cheap enough to do whether or not the screen is showing: it is five short
    // label writes, and doing it now means the screen is already right when someone
    // turns the knob to it rather than blank for a moment.
    if (g_intelDirty) {
        g_intelDirty = false;
        intelview::onHeadlinesReady();
    }
    if (g_tickerDirty) {
        g_tickerDirty = false;
        tickerview::onQuotesReady();
    }
    // The curved strip is advanced from here rather than from a timer of its own, so it
    // moves on the same clock as everything else the display does and cannot drift against
    // it. tick() returns immediately unless the theme actually curves its strip.
    tickerview::tick();
    if (g_wxRadarDirty) {
        g_wxRadarDirty = false;
        ui_on_data_updated();
    }
    if (g_cloudImageDirty) {
        g_cloudImageDirty = false;
        ui_on_data_updated();
    }

    // periodic: HUD clock + wifi/battery indicators
    static uint32_t lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
        stats_status_tick();   // heap trend + [memdbg] every ~15 s; orb_stats.cpp
        char clk[8] = "--:--";
        struct tm ti;
        // orb_local_time, not getLocalTime(&ti, 0): that one can answer false without
        // reading the clock at all (orb_time.h). Here it made the header time blink to
        // --:-- and could hand the hourly chime a bogus hour.
        const bool haveTime = orb_local_time(&ti);
        if (haveTime) {
            snprintf(clk, sizeof(clk), "%02d:%02d", ti.tm_hour, ti.tm_min);
            char date[20];
            strftime(date, sizeof(date), "%d %b %Y", &ti);   // e.g. "08 Jun 2026"
            ui_set_date(date);
            // Top-of-hour clock chime (once per hour change; not at boot).
            static int lastChimeHour = -1;
            if (lastChimeHour < 0) lastChimeHour = ti.tm_hour;
            else if (ti.tm_hour != lastChimeHour) {
                lastChimeHour = ti.tm_hour;
                // Whatever is selected in Settings, from flash or from any theme on the
                // card. Not the worn theme's own chime: see the note on host_chime_count().
                //
                // ONLY ON THE CLOCK, which is Zion's call and a better rule than the one it
                // replaces. A chime is a clock's feature. Ringing it over the flight tracker
                // talks across the aircraft alerts that screen exists to give you, and over
                // the camera or the news it is an interruption from an app you are not in.
                //
                // It also shrinks the thing I was uneasy about. Chimes stream off the SD card,
                // and the card is only safe for one reader at a time; requiring the clock to
                // be on screen means the camera cannot be open and the map is not fetching
                // roads while a chime plays, which is most of the overlap that worried me.
                // A limitation, deliberately taken, rather than a lock hoping to be enough.
                const bool onClock = !app_shell::browsing()
                                  && app_shell::index() == app_shell::APP_CLOCK;
                if (g_soundChime && onClock && audio_present()) chime_library::playSelected();
            }
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
        // First time WiFi is seen up on a device with no location, ask the network where it
        // is. Here rather than in setup() because WiFi associates seconds after boot, and
        // once per boot rather than on a timer because a network that refuses the lookup
        // will keep refusing it and the owner can always pick a place in Settings instead.
        if (!g_locateTried && wifiUp && !g_locationSet) {
            g_locateTried = true;
            host_locate_if_unset();
        }
        // Same two facts, said in words on the scope itself. The HUD's amber bars already
        // encode this, but a colour change on a signal icon is not something anyone reads as
        // "somebody else's server is down" — which is what it almost always means.
        radar::setFeedStatus(wifiUp, (uint32_t)((millis() - g_lastFeedOkMs) / 1000UL),
                             g_locationSet);
        char net[112];
        if (WiFi.status() == WL_CONNECTED)
            // Two lines, and NO coordinates. UX-028 says what the splash must carry - the
            // firmware version, the theme's name, its author and the data credits - and the
            // config address is not on that list at all; it arrived here from the old
            // touch-only Stats screen and stayed. The centre point was diagnostic rather
            // than an address, it was the longest string on the screen, and it now lives on
            // Settings > Location where somebody checking their location will look for it.
            snprintf(net, sizeof(net), "Configure at " ORB_MDNS_ADDR "\n%s",
                     WiFi.localIP().toString().c_str());
        else if (g_wm.getConfigPortalActive())
            snprintf(net, sizeof(net), "WiFi setup:\njoin \"The Orb Setup\"");
        else {
            // The portal is not up, so "join The Orb Setup" would send somebody looking for
            // an access point that does not exist. What is true instead: a network is
            // stored and the Orb is still trying to reach it (see the boot block).
            char ssid[33];
            host_wifi_saved_ssid(ssid, sizeof(ssid));
            if (ssid[0]) snprintf(net, sizeof(net), "Reconnecting to\n%s", ssid);
            else         snprintf(net, sizeof(net), "No WiFi:\nSettings > WiFi");
        }
        settingsview::setNetInfo(net);   // shown on Settings > About (was the Stats screen)
        // The centre point the scope is actually using, shown on Settings > Location. Same
        // contract as setNetInfo: safe every loop, only redraws while that page is open.
        settingsview::setHomeCoords(g_settings.homeLat, g_settings.homeLon, g_locationSet);
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        // GPS used to re-centre the radar from here whenever a fix moved more than a
        // kilometre. It is gone. UX-025 gives the device two location inputs — the network
        // it joins, and a location the owner picks in Settings — and says the manual one
        // always wins; a third input that moved the centre on its own could not be
        // reconciled with that. UX-026 is blunter still: GPS is never read, on any board,
        // ever. The LC76G driver stays in the tree (src/gps.cpp) and nothing calls it.
        //
        // The live re-centre this block performed was worth keeping, and it did not go: it
        // is apply_location_live() below, which is what the boot-time lookup uses to move
        // the scope without a reboot.
    }

    // Movement wakes the screen. Read fast, because a knock is short; only while the
    // screen is up (face-down sleep is deliberate and flipping the Orb back is what ends
    // it). Motion also counts as activity while awake, so a desk that is being used does
    // not dim the Orb sitting on it.
    static uint32_t lastMotion = 0;
    if (!g_asleep && millis() - lastMotion > 50) {
        lastMotion = millis();
        if (imu_motion() > 0) {
            display::noteActivity();
            if (g_idle) {
                g_idle = false;
                applyBrightness();
                Serial.println("[imu] motion woke the screen");
            }
        }
    }

    // face-down -> screen off (IMU); flip face-up to wake
    static uint32_t lastImu = 0;
    static int fdCount = 0;
    if (millis() - lastImu > 400) {
        lastImu = millis();
        const int fd = imu_facedown();              // 1 down, 0 not, -1 read error
        if (fd > 0)       { if (fdCount < 8) fdCount++; }
        else if (fd == 0) fdCount = 0;              // -1 (I2C hiccup): leave the counter as-is
        const bool sleep = (fdCount >= 4);   // ~1.6 s face-down
        const bool idle  = g_idleDimMs > 0 && display::inactiveMs() > g_idleDimMs;
        if (sleep != g_asleep || idle != g_idle) {
            g_asleep = sleep;
            g_idle = idle;
            applyBrightness();
        }
    }

    lpT[4] = micros();
    // Tunable so the trade between pass rate and leaving other tasks room can be measured on a
    // running Orb. Clamped at 1: zero stops yielding to this core's idle task.
    delay(g_loopDelayMs);
    lpT[5] = micros();
    loop_phase_report(lpT);
}
