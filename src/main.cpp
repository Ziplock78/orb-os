// Capsule Radar — entry point / glue. SKELETON: TODOs mark what to implement.
// Order of work is in CLAUDE.md (milestones). Bring up the Waveshare demo first.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "config.h"
#include "aircraft.h"
#include "geo.h"
#include "adsb_client.h"
#include "snapshot_gate.h"
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
#include "orb_link.h"      // USB serial command channel: how a browser (Orb Studio) talks to this device
#include "custom_weld.h"   // CUSTOM_WELD_HASH — lets a push tell whether new firmware is needed
#include "theme_style.h"   // per-theme app roster (theme_style::apps())
#include "display.h"                  // M0: CO5300 + LVGL bring-up
#include "imu_qmi8658.h"             // face-down sleep
#include "gps.h"                     // LC76G GNSS (-G variant only)
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
#include "location_view.h"          // location info app (Aviator dial)
#include "spycam_view.h"             // Spy Cam: looping "security camera" flip-book
#include <set>                       // audio: track which contacts are in range
#include <string>
#include <WiFiManager.h>             // captive portal
#if __has_include("secrets.h")
#include "secrets.h"                 // optional, gitignored: DEV_WIFI_SSID / DEV_WIFI_PASS
#endif
#include <Preferences.h>            // NVS (persist theme/settings)
#include <time.h>                   // NTP/RTC clock + date
#include <WebServer.h>              // configuration web page
#include <ESPmDNS.h>                // http://capsuleradar.local
// Wireless firmware update (ArduinoOTA + the browser upload page). Needs a second app
// partition to write the incoming image into, and partitions_16MB_themeart.csv gives
// that 6.25 MB to pre-baked theme art instead. Flip this and the partition table
// together, never one alone: with no OTA partition present the code below would compile
// and run but fail at the first write, which is worse than not offering it.
#define ORB_OTA_ENABLED 0
#if ORB_OTA_ENABLED
#include <ArduinoOTA.h>             // OTA firmware update over WiFi (PlatformIO/espota)
#include <Update.h>                 // browser OTA: self-flash an uploaded .bin
#endif
#include <esp_system.h>             // esp_reset_reason() for /health
#include <SD.h>                     // /sdput: write theme files straight to the microSD
#include <esp_heap_caps.h>          // largest-free-block metric (heap health)
#include <esp_wifi.h>               // WiFi driver control (reset must survive the reboot)
#include <nvs.h>                    // erase the driver's "nvs.net80211" namespace (WiFi reset)

// ---- shared state ----
static std::vector<Aircraft> g_aircraft;      // latest snapshot
static SemaphoreHandle_t     g_ac_mutex;      // guards g_aircraft
// Handles captured at creation, purely so /taskmem can ask each task how much of its own
// stack it has ever come within of using (uxTaskGetStackHighWaterMark). Phase 0 of the
// 2026-08-22 memory investigation: before trimming anything, find out who is actually
// holding the internal RAM that ADS-B polling needs.
static TaskHandle_t          g_adsbTaskHandle = nullptr;
static TaskHandle_t          g_audioTaskHandle = nullptr;
static volatile bool         g_acDirty = false; // set when a new snapshot is ready
static AdsbClient            g_adsb;
static RadarSettings         g_settings;
static WiFiManager           g_wm;
static int                   g_brightnessDay = BRIGHTNESS_DEFAULT;   // user brightness (web/NVS)
static int                   g_volume = 60;                          // alert volume 0..100 (web/NVS)
static bool                  g_muted  = false;                       // mute alert pings
static int                   g_chimeIdx = 0;                         // selected chime (Settings/NVS)
static bool                  g_soundRadar = false;                   // on-device: radar pings on/off
static bool                  g_soundChime = false;                   // on-device: top-of-hour clock chime
static int                   g_alertMode = 2;                        // 0=off 1=emergencies 2=new+emergencies (web/NVS)
static float                 g_proximityKm = 0.0f;                   // proximity alert radius, km (0=off) (web/NVS)
static uint32_t              g_idleDimMs = IDLE_DIM_MS;              // dim after this idle time (0 = never)
static bool                  g_showSweep = true;                     // rotating sweep line on/off (web/NVS)
static int                   g_units = 0;                            // 0=Aviation 1=Metric 2=Imperial (web/NVS)
static int                   g_wxUnits = 0;                          // Weather app only: 0=Auto 1=Metric 2=Imperial (Settings/NVS)
static int                   g_wxZoomTier = 0;                       // Weather map range: 0=50mi 1=100mi (Settings/NVS)
static volatile bool         g_wxZoomChanged = false;                // set on cycle so adsb_task refetches immediately
static bool                  g_showAirports = true;                  // airport markers on/off (web/NVS)
static bool                  g_hideGround   = false;                 // skip on-ground aircraft in the feed (web/NVS)
static int                   g_minAltFt     = 0;                     // only show aircraft above this altitude, ft (0 = off) (web/NVS)
static int                   g_deadZonePx   = 0;                     // ignore aircraft inside this many px of scope center (0 = off) (pushed design)
static bool                  g_milOnly      = false;                 // only show military-flagged aircraft (web/NVS)
static int                   g_rotation = 0;                         // clockwise display rotation, 0..359° (web/NVS)
static bool                  g_useGps = false;                       // auto-set home from the LC76G GPS (-G variant) (web/NVS)
static int                   g_trailLen = 2;                         // aircraft trails 0=off 1=short 2=med 3=long (web/NVS)
static int                   g_maxAc = 20;                           // max aircraft drawn on the scope (web/NVS)
static bool                  g_bigText = false;                      // accessibility: large fonts (web/NVS, applied at boot)
static volatile bool         g_onBattery = false;                    // discharging (set on core 1, read on core 0)
static bool                  g_rtcSynced = false;                    // RTC written from NTP this session?
static std::vector<Aircraft> g_snap;                                 // last snapshot (instant re-render on zoom)
static volatile bool         g_requery = false;                      // range changed -> adsb_task re-begins
// Diagnostic override for the ADS-B poll interval (0 = use the compiled default). Exists
// because the poll is the biggest periodic work on the device and the prime suspect for a
// periodic frame hitch: proving that by changing one number live beats one flash per guess.
static volatile uint32_t     g_pollOverrideMs = 0;
static float                 g_requeryKm = 0.0f;
static volatile bool         g_feedOk = true;                        // ADS-B feed healthy? (HUD warning)
// Flight Tracker is the only consumer of ADS-B data, but adsb_task used to poll it
// every 2s regardless of which app was on screen. Each poll opens a fresh TLS
// session (WiFiClientSecure), and mbedTLS's handshake buffers come from internal
// RAM, not PSRAM — dozens of alloc/free cycles a minute fragment that heap even
// while the user is sitting on Clock or the menu. Set true only while Flight
// Tracker is the entered app (radar_show_home_custom / radar_exit_release_style).
static volatile bool         g_radarViewActive = false;
static volatile uint32_t     g_lastFeedOkMs = 0;                     // millis() of the last good poll (HUD staleness)

// How many times we have already restarted THIS POWER CYCLE trying to un-stick the feed.
// RTC_NOINIT survives ESP.restart() and is cleared by a real power cycle, which is exactly
// the scope wanted: one attempt per plug-in.
//
// The restart genuinely does help here, briefly. The feed reads its first snapshots fine on
// a fresh heap and then starts timing out as internal memory fragments, so rebooting buys
// another minute or two. That is precisely why it must be capped: a recovery that works for
// ninety seconds and then needs performing again is not a recovery, it is a loop, and from
// the desk it looks like a device that will not stay on the screen you put it on. One go,
// then stop and show the honest stale-feed state instead.
RTC_NOINIT_ATTR static uint32_t g_feedReboots;
RTC_NOINIT_ATTR static uint32_t g_feedRebootsMagic;
static const uint32_t FEED_REBOOT_MAGIC = 0x0FEED123;
static const uint32_t FEED_REBOOT_LIMIT = 1;
static volatile uint32_t     g_rebootAtMs = 0;
// /theme?slug=... — applied from loop() rather than the request handler, because
// theme_select::set() reboots and would cut the HTTP reply off mid-flight.
static String                g_pendingSlug;
static volatile uint32_t     g_applySlugAtMs = 0;                       // !=0: reboot when millis() reaches it (clean start after WiFi config)
static String                g_tz = TZ_STR;                          // POSIX timezone (web-configurable, NVS); applied via configTzTime
static volatile bool         g_weatherDirty = false;
static volatile bool         g_wxRadarDirty = false;
static volatile bool         g_wxAnimDirty = false;      // new Weather app: frame set ready
static volatile bool         g_cloudImageDirty = false;

// Web-selectable time zones (label + POSIX TZ). The <option> value is the index; the save
// handler maps it back to the POSIX string stored in NVS and used by configTzTime at boot.
// (Index avoids putting POSIX strings with '<>' / ',' into HTML attributes.)
// offMin = standard (winter) UTC offset in minutes; dst = 1 if the zone observes DST.
// The web page uses these to auto-pick the visitor's zone from their browser clock.
static const struct { const char *label; const char *tz; int offMin; int dst; } TZOPTS[] = {
    {"UTC",                      "UTC0",                              0, 0},
    {"London / Lisbon",          "GMT0BST,M3.5.0/1,M10.5.0",          0, 1},
    {"Madrid / Paris / Berlin",  "CET-1CEST,M3.5.0,M10.5.0/3",       60, 1},
    {"Athens / Helsinki",        "EET-2EEST,M3.5.0/3,M10.5.0/4",     120, 1},
    {"New York (US Eastern)",    "EST5EDT,M3.2.0,M11.1.0",          -300, 1},
    {"Chicago (US Central)",     "CST6CDT,M3.2.0,M11.1.0",          -360, 1},
    {"Denver (US Mountain)",     "MST7MDT,M3.2.0,M11.1.0",          -420, 1},
    {"Phoenix (Arizona)",        "MST7",                            -420, 0},
    {"Los Angeles (US Pacific)", "PST8PDT,M3.2.0,M11.1.0",          -480, 1},
    {"Anchorage (Alaska)",       "AKST9AKDT,M3.2.0,M11.1.0",        -540, 1},
    {"Honolulu (Hawaii)",        "HST10",                           -600, 0},
    {"Argentina / Brazil (E)",   "<-03>3",                          -180, 0},
    {"India (IST)",              "<+0530>-5:30",                     330, 0},
    {"China / Singapore",        "<+08>-8",                          480, 0},
    {"Japan / Korea",            "JST-9",                            540, 0},
    {"Sydney (AU Eastern)",      "AEST-10AEDT,M10.1.0,M4.1.0/3",     600, 1},
    {"Auckland (NZ)",            "NZST-12NZDT,M9.5.0,M4.1.0/3",      720, 1},
};
static const int TZOPTS_N = sizeof(TZOPTS) / sizeof(TZOPTS[0]);

// ---- networking task (core 0): fetch + parse, never touches the display ----
static void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    AircraftSnapshotGate snapshotGate;
    bool retainingEmptySnapshot = false;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t adsbBackoffMs = 0;                // extra delay after feed failures (exponential, 30s cap)
    uint32_t nextWeatherAt = UINT32_MAX;       // armed five seconds after WiFi connects
    uint32_t nextWxRadarAt = UINT32_MAX;
    int      wxFillIdx = WX_RADAR_FRAMES;      // which animation frame to fetch next (== FRAMES: idle)
    uint32_t wxGen = 0;                        // refresh generation, bumped each full loop
    uint32_t nextLocInfoAt = UINT32_MAX;
    uint32_t lastFeedOk = millis();          // self-heal: time of last good (or no-WiFi) poll
    // Has the feed EVER answered since this boot? The restart below is a recovery, and a
    // recovery needs something to recover to. If no poll has ever succeeded, restarting
    // cannot restore a working state that never existed; it just arrives back in the same
    // place twenty seconds later and tries again, forever. That is exactly what the Flight
    // Tracker was doing on 2026-08-22: rebooting every three minutes against a feed that was
    // refusing the request, which no restart was ever going to change.
    bool feedEverOk = false;
    bool wasRadarActive = false;
    for (;;) {
        const bool conn = (WiFi.status() == WL_CONNECTED);
        const bool radarActive = g_radarViewActive;
        if (radarActive && !wasRadarActive) {
            // Just entered Flight Tracker: poll right away and give the stuck-feed
            // watchdog a fresh 180s window instead of one aged by however long the
            // user was on another app (where lastFeedOk was frozen, not stale-broken).
            lastPoll = 0;
            lastFeedOk = millis();
        }
        wasRadarActive = radarActive;
        if (conn && !wasConnected) {
            // disable WiFi modem power-save: on a mains-powered desk gadget it just adds latency
            // and makes RSSI bounce (feed goes stale -> amber bars) even sitting next to the router.
            WiFi.setSleep(false);
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            diag::log("wifi up %s", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");  // local time (web-configurable TZ)
            Serial.println("[web] config: http://capsuleradar.local/  (or the IP above)");
            nextWeatherAt = millis() + 5000UL; // let the first ADS-B poll complete before weather TLS
            nextWxRadarAt = millis() + 12000UL;
            nextLocInfoAt = millis() + 9000UL;
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // self-heal: a long feed outage while WiFi is up usually means the internal heap
        // fragmented and the TLS handshake can't allocate -> reboot to recover (settings persist).
        if (!conn || !radarActive) lastFeedOk = millis();
        else if (!feedEverOk) {
            // Never worked this session: nothing to restore, so hold the clock and let the
            // backoff keep trying. The HUD already shows the feed as stale, which is the
            // honest thing to show, and the rest of the device stays usable.
            lastFeedOk = millis();
        }
        else if (millis() - lastFeedOk > 180000UL) {
            if (g_feedReboots >= FEED_REBOOT_LIMIT) {
                // Already tried it this power cycle and here we are again. Stop bouncing.
                lastFeedOk = millis();
                radar::setFeedNote("Aircraft feed keeps dropping\nThis Orb needs a power cycle\nEverything else still works");
                Serial.println("[adsb] feed stuck again, but a restart already failed to fix it this power cycle — staying up");
                diag::log("feed stuck again; restart already tried, staying up");
            } else {
                ++g_feedReboots;
                Serial.println("[adsb] feed stuck >180s with WiFi up -> restarting to recover (once)");
                diag::log("feed stuck 180s -> reboot %u (heap %u)",
                          (unsigned)g_feedReboots, (unsigned)ESP.getFreeHeap());
                delay(100);
                ESP.restart();
            }
        }
        if (g_requery) {                          // display range changed (double-tap zoom)
            g_adsb.begin(g_settings.homeLat, g_settings.homeLon, g_requeryKm);
            g_requery = false;
            lastPoll = 0;                         // poll immediately at the new radius
        }
        if (conn) {
            // The live aircraft feed is the primary job, so poll FIRST every cycle. That keeps
            // it refreshing even while the user taps around — a slow route/photo lookup (below)
            // can block this single network task, so it must never get ahead of the feed.
            const uint32_t nowMs = millis();
            const uint32_t pollInterval =
                (g_pollOverrideMs ? g_pollOverrideMs
                                  : (g_onBattery ? POLL_INTERVAL_BATTERY_MS : POLL_INTERVAL_MS)) + adsbBackoffMs;
            if (radarActive && (lastPoll == 0 || nowMs - lastPoll >= pollInterval)) {  // aircraft feed, Flight Tracker only
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() tries the fallback provider after a primary failure; keep the HUD
                // healthy through isolated misses and warn only after a sustained outage.
                // Synthesised traffic, when the active theme asks for it. Straight courses at
                // fixed speeds, seeded once so the same aircraft persist and actually travel
                // rather than teleporting each poll — which is what makes trails, sticky
                // tracking and zone masking all observable without waiting on the sky.
                // Positions advance by real elapsed time, so it runs at the same pace
                // whatever the poll interval is.
                const bool simulated = theme_style::radar().simulate;
                if (simulated) {
                    static bool     simInit = false;
                    static uint32_t simT0 = 0;
                    struct SimAc { double lat0, lon0; float brgDeg, gsKt, altFt; const char *call; const char *type; };
                    static SimAc sim[8];
                    if (!simInit) {
                        simInit = true;
                        simT0 = millis();
                        // Spread around the home point at varied radii and headings, so some
                        // cross the middle, some skirt the rim, and some pass through
                        // whatever keep-out areas a design has drawn.
                        for (int i = 0; i < 8; ++i) {
                            const float a = (float)i * 45.0f;
                            const float rKm = 8.0f + (float)(i % 4) * 9.0f;
                            sim[i].lat0   = g_settings.homeLat + (double)(rKm / 111.0f) * cos(a * (float)M_PI / 180.0f);
                            sim[i].lon0   = g_settings.homeLon + (double)(rKm / 111.0f) * sin(a * (float)M_PI / 180.0f)
                                            / cos(g_settings.homeLat * (double)M_PI / 180.0);
                            sim[i].brgDeg = fmodf(a + 115.0f, 360.0f);   // not radial: they cross the scope
                            sim[i].gsKt   = 180.0f + (float)(i % 5) * 55.0f;
                            sim[i].altFt  = 3500.0f + (float)i * 2600.0f;
                            sim[i].call   = "SIM";
                            sim[i].type   = "SIM";
                        }
                    }
                    const float hrs = (float)(millis() - simT0) / 3600000.0f;
                    fresh.clear();
                    for (int i = 0; i < 8; ++i) {
                        const float nm  = sim[i].gsKt * hrs;
                        const float km  = nm * 1.852f;
                        const float brg = sim[i].brgDeg * (float)M_PI / 180.0f;
                        Aircraft a;
                        char hexBuf[8]; snprintf(hexBuf, sizeof(hexBuf), "sim%03d", i);
                        a.hex     = hexBuf;
                        char callBuf[10]; snprintf(callBuf, sizeof(callBuf), "SIM%03d", i);
                        a.flight  = callBuf;
                        a.type    = "SIM";
                        a.lat     = sim[i].lat0 + (double)(km / 111.0f) * cos(brg);
                        a.lon     = sim[i].lon0 + (double)(km / 111.0f) * sin(brg)
                                    / cos(g_settings.homeLat * (double)M_PI / 180.0);
                        a.altBaro = sim[i].altFt;
                        a.onGround = false;
                        a.track   = sim[i].brgDeg;
                        a.gs      = sim[i].gsKt;
                        a.baroRate = 0.0f;
                        a.squawk  = 1200;
                        a.seenPos = 0;
                        a.lastUpdateMs = millis();
                        fresh.push_back(a);
                    }
                }
                if (simulated || g_adsb.poll(fresh)) {
                    if (!simulated) Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    adsbBackoffMs = 0;                        // recovered: back to real-time polling
                    feedEverOk = true;                        // a restart now has a known-good state to return to
                    radar::setFeedNote(nullptr);              // back to the plain notice if it is somehow still up
                    g_feedOk = true;
                    const uint32_t receivedMs = millis();
                    lastFeedOk = receivedMs;
                    g_lastFeedOkMs = receivedMs;      // HUD: mark data as fresh

                    const bool publish = snapshotGate.shouldPublish(
                        !fresh.empty(), receivedMs, AC_STALE_MS);
                    if (!publish) {
                        if (!retainingEmptySnapshot) {
                            Serial.printf("[adsb] empty snapshot; retaining contacts for %u ms\n",
                                          (unsigned)AC_STALE_MS);
                            retainingEmptySnapshot = true;
                        }
                    } else {
                        if (retainingEmptySnapshot && fresh.empty())
                            Serial.println("[adsb] empty snapshot persisted; clearing stale contacts");
                        retainingEmptySnapshot = false;
                        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                            g_aircraft.swap(fresh);   // O(1) handoff: no per-Aircraft String copies under the lock
                            g_acDirty = true;
                            xSemaphoreGive(g_ac_mutex);
                        }
                    }
                } else if (g_adsb.lastWasRefused()) {
                    // The SERVER said no (4xx), which no amount of retrying changes. Two things
                    // follow, and both are the opposite of what the branch below does.
                    //
                    // Back off in minutes, not seconds: this is a rate limit, and the honest
                    // response to being told we ask too often is to ask far less often.
                    //
                    // And hold the watchdog off. lastFeedOk is pushed forward so the 180 s
                    // "feed stuck -> reboot" recovery cannot fire, because rebooting does not
                    // clear a rate limit; it just returns in twenty seconds and asks again,
                    // which is precisely how a temporary limit becomes a lasting one. That
                    // recovery exists for a fragmented heap, and this is not that.
                    if (++failCount >= 5) g_feedOk = false;
                    adsbBackoffMs = (adsbBackoffMs < 60000UL) ? 60000UL
                                  : (adsbBackoffMs < 300000UL) ? adsbBackoffMs * 2
                                                               : 300000UL;
                    lastFeedOk = millis();
                    // Say it on the dial too. The "Loading aircraft and location data" notice
                    // clears only when aircraft arrive, so a feed that never answers left it
                    // claiming to be loading for as long as the device was switched on.
                    radar::setFeedNote("No aircraft feed\nThe service is refusing requests\nTrying again in a few minutes");
                    Serial.printf("[adsb] refused by the feed (HTTP %d) — waiting ~%lus, not rebooting\n",
                                  g_adsb.refusedStatus(), (unsigned long)(adsbBackoffMs / 1000));
                } else {
                    if (++failCount >= 5) g_feedOk = false;   // sustained outage -> HUD warning
                    // A failed poll here is the TLS handshake starving on a fragmented internal
                    // heap (SSL -32512). Hammering both hosts only fragments it further and floods
                    // the log; backing off exponentially (2s -> 30s cap) lets the internal heap
                    // coalesce so a later handshake can allocate, and we snap straight back to
                    // real-time the instant a poll succeeds (see the success branch above).
                    adsbBackoffMs = (adsbBackoffMs == 0)     ? 2000UL
                                  : (adsbBackoffMs < 15000UL) ? adsbBackoffMs * 2
                                                              : 30000UL;
                    Serial.printf("[adsb] poll failed (backing off ~%lus)\n",
                                  (unsigned long)(adsbBackoffMs / 1000));
                }
            }
            // Forecasts change slowly. Fetch only after the live ADS-B poll has had priority.
            if ((int32_t)(nowMs - nextWeatherAt) >= 0) {
                Serial.printf("[weather] fetching %.5f, %.5f...\n",
                              g_settings.homeLat, g_settings.homeLon);
                WeatherSnapshot forecast;
                if (weather_fetch(g_settings.homeLat, g_settings.homeLon, forecast)) {
                    weather_store(forecast);
                    g_weatherDirty = true;
                    nextWeatherAt = millis() + WEATHER_REFRESH_MS;
                    Serial.println("[weather] forecast updated");
                } else {
                    nextWeatherAt = millis() + 60000UL;
                    Serial.println("[weather] fetch failed; retrying in 60s");
                }
            }
            // Weather radar animation: fetch the past frames one per pass (not all 9 in one
            // blocking burst) so each ~2-3s tile fetch yields back to the live ADS-B poll
            // between frames instead of freezing the feed for ~25s. wxFillIdx == FRAMES = idle.
            if (g_wxZoomChanged) {                     // zoom changed: restart the loop right away
                g_wxZoomChanged = false;
                wxFillIdx = 0; ++wxGen; nextWxRadarAt = nowMs;
            }
            if (wxFillIdx >= WX_RADAR_FRAMES && (int32_t)(nowMs - nextWxRadarAt) >= 0) {
                wxFillIdx = 0; ++wxGen;                // periodic refresh: start a new loop
            }
            if (wxFillIdx < WX_RADAR_FRAMES && (int32_t)(nowMs - nextWxRadarAt) >= 0) {
                const int r = wx_radar_fetch_frame(g_settings.homeLat, g_settings.homeLon,
                                                   g_wxZoomTier, wxGen, wxFillIdx);
                if (r == 1) {
                    g_wxRadarDirty = true;             // new frame committed
                    wxFillIdx++;
                    nextWxRadarAt = millis() + (wxFillIdx >= WX_RADAR_FRAMES ? WX_RADAR_REFRESH_MS : 250UL);
                } else if (r == 0) {                   // fewer frames than the loop length — cycle done
                    if (wxFillIdx > 0) g_wxRadarDirty = true;
                    wxFillIdx = WX_RADAR_FRAMES;
                    nextWxRadarAt = millis() + WX_RADAR_REFRESH_MS;
                } else {                               // error — retry the whole cycle in 60s
                    wxFillIdx = WX_RADAR_FRAMES;
                    nextWxRadarAt = millis() + 60000UL;
                    Serial.println("[wxradar] fetch failed; retrying in 60s");
                }
            }
            // Satellite clouds (EUMETSAT) is no longer reachable from the Weather app's
            // knob-push cycle (see weather_press_cycle() in main.cpp) — nothing to spend
            // a periodic fetch + PSRAM cache on. nextCloudImageAt stays permanently
            // un-armed (UINT32_MAX) since it's never referenced now. cloud_image_fetch()
            // itself is untouched if this ever needs to come back on some other input.
            // Location Info app: reverse-geocode + weather + a Wikipedia fact. One HTTPS
            // call per cycle (pump); a new cycle starts on demand or on the refresh timer.
            if (locationview::takeRefresh() || (int32_t)(nowMs - nextLocInfoAt) >= 0) {
                Serial.println("[locinfo] fetching...");
                locationview::startRefresh();
                nextLocInfoAt = millis() + (locationview::hasData() ? 300000UL : 30000UL);
            }
            locationview::pump(g_settings.homeLat, g_settings.homeLon);
            // Then the on-demand lookups for the selected aircraft. Their timeouts are kept
            // short (see photo_client / route_client) so a slow photo server can't freeze the
            // feed for long; the next loop iteration polls again as soon as they return.
            char wantCall[12];
            if (route_pending(wantCall, sizeof(wantCall))) {
                char from[40] = "", to[40] = "";
                if (route_cache_get(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);                       // NVS hit, no network
                    Serial.printf("[route] %s (cache): '%s' -> '%s'\n", wantCall, from, to);
                } else if (route_fetch(wantCall, from, sizeof(from), to, sizeof(to))) {
                    route_store(wantCall, from, to);
                    route_cache_put(wantCall, from, to);                  // remember across reboots
                    Serial.printf("[route] %s (net): '%s' -> '%s'\n", wantCall, from, to);
                } else {
                    route_store(wantCall, from, to);   // empty -> don't refetch this session
                    Serial.printf("[route] %s: no route\n", wantCall);
                }
            }
            char wantHex[10];
            if (photo_pending(wantHex, sizeof(wantHex))) photo_fetch(wantHex);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

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
static void applyThemeSettings() {
    const theme_style::Radar &rs = theme_style::radar();
    // -1 (or 0 for range) means "no opinion", leaving the Orb's own stored setting alone.
    if (rs.rangeKm     > 0.0f) g_settings.rangeKm = rs.rangeKm;
    if (rs.maxAircraft > 0)    g_maxAc     = rs.maxAircraft;
    if (rs.minAltFt   >= 0)    g_minAltFt  = rs.minAltFt;
    if (rs.hideGround >= 0)    g_hideGround = (rs.hideGround != 0);
    Serial.printf("[theme] applied: rangeKm=%.0f maxAircraft=%d minAltFt=%d hideGround=%d\n",
                  (double)g_settings.rangeKm, g_maxAc, g_minAltFt, (int)g_hideGround);
}

static void loadSettings() {
    Preferences p;
    p.begin("capsuleradar", true);
    g_settings.homeLat = p.getDouble("homeLat", HOME_LAT_DEFAULT);
    g_settings.homeLon = p.getDouble("homeLon", HOME_LON_DEFAULT);
#if CUSTOM_HAS_RADAR_HOME
    // Launch Kit is the source of truth for location: a pushed design's own
    // Latitude/Longitude override whatever's saved in NVS (and the on-device
    // Settings/IP/GPS detection, see below), so the editor, simulator, and Orb
    // all center on the exact same coordinates. Not persisted — same
    // one-shot-per-flash model as range; a re-push (or reflash) is how location
    // changes, matching "the editor is the source of truth".
    g_settings.homeLat = CUSTOM_RADAR_HOME_LAT;
    g_settings.homeLon = CUSTOM_RADAR_HOME_LON;
#endif
    g_settings.rangeKm = p.getFloat("rangeKm", RANGE_KM_DEFAULT);
#if CUSTOM_HAS_RADAR_RANGE
    // A pushed design's own Range slider is authoritative for what the scope
    // captures (and what the range banner shows) until the next push — not
    // persisted to NVS, so an on-device zoom afterward still works normally
    // for the rest of this session; it just won't survive a reboot without a
    // fresh push, same one-shot-per-boot precedent as CUSTOM_BOOT_TARGET.
    g_settings.rangeKm = CUSTOM_RADAR_RANGE_KM;
#endif
    g_brightnessDay    = p.getInt("bright", BRIGHTNESS_DEFAULT);
    g_volume           = p.getInt("vol", 60);
    g_muted            = p.getBool("mute", false);
    g_soundRadar       = p.getBool("sndRadar", false);
    g_soundChime       = p.getBool("sndChime", false);
    g_alertMode        = p.getInt("alertmode", 2);
    g_proximityKm      = p.getFloat("proxkm", 0.0f);
    g_useGps           = p.getBool("usegps", false);
    g_trailLen         = p.getInt("traillen", 2);
    g_maxAc            = p.getInt("maxac", 20);
#if CUSTOM_HAS_RADAR_MAXAC
    g_maxAc = CUSTOM_RADAR_MAXAC;   // a pushed design's own "max aircraft shown" cap, same one-shot-per-boot precedent as range/boot-target
#endif
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
    p.end();
    audio_set_chime(g_chimeIdx);   // no hardware dependency, safe before audio_begin()
    // fonts are baked into the widgets at creation time, so the large-text flag must be
    // in place before display::begin() builds the UI (loadSettings runs first in setup)
    ui_set_large_text(g_bigText);
    radar::setLargeText(g_bigText);
}

// Audio alerts. g_alertMode: 0 = off, 1 = emergencies only, 2 = new aircraft + emergencies.
// g_proximityKm > 0 also pings (once) when any aircraft crosses into that radius.
static void checkAudioEvents() {
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
static float queryRadiusKm() {
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
static float deadZoneKm() {
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

// Persist the visual theme in NVS (called when the user long-presses to switch).
static void saveTheme(int t) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putInt("theme", t);
    p.end();
    ui_apply_theme(t);   // keep the HUD chrome in sync with the scope's palette
    diag::log("theme -> %d", t);
}

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

// Brightness combines idle auto-dim and face-down sleep (sleep wins -> screen off).
static bool g_asleep = false;   // face-down
static bool g_idle   = false;   // no touch for a while
static void applyBrightness() {
    int b = g_brightnessDay;
    if (g_idle  && BRIGHTNESS_IDLE  < b) b = BRIGHTNESS_IDLE;   // idle only dims down
    if (g_asleep) b = 0;                                         // face-down -> screen off
    display::setBrightness(b);
}

// Shared with the Settings app (settings_view.cpp).
// App-shell onEnter hooks: flip the radar screen's tileview between the radar view
// and the original app's weather view (radar / clouds / forecast).
static void radar_show_home()    { ui_show_view(0); }
static void radar_show_weather() { ui_show_view(1); }   // tile 1 since list/stats went

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
static void radar_show_home_custom() {
    radar_show_home();
    radar::knobEnter();   // re-attach style + land in the default view (knob released)
    g_radarViewActive = true;    // adsb_task may now poll; see g_radarViewActive above
}
static void radar_turn_select(int delta) { radar::knobTurn(delta); }
static void radar_press_custom_or_theme() { radar::knobPress(); }
static void radar_exit_release_style() { radar::knobExit(); g_radarViewActive = false; }
void host_wx_zoom_set(int tier);   // defined below, near the other weather-units hosts
int  host_wx_zoom_tier();

// Knob push while on Weather: one cycle through everything the app shows, no touch
// needed — 50mi -> 100mi radar zoom, then the 3-day forecast, then back to 50mi.
// Satellite clouds isn't in this cycle (see ui_set_weather_forecast()'s comment).
static void weather_press_cycle() {
    // Lean redesign: one fixed 50mi radar range, no user-selectable zoom (see
    // docs/lean-weather-radar-redesign.md). The knob push just toggles forecast <-> radar
    // map; the old 100mi tier and the re-fetch it forced are gone. g_wxZoomTier is pinned
    // to 0 (50mi) in loadSettings, so the radar map is always the 50mi view.
    ui_set_weather_forecast(!ui_weather_is_forecast());
}

// Recovery-reboot warning: the knob's hold-to-reboot is meant for a genuinely stuck
// device, but a silent multi-second countdown means a thumb resting on the button
// during normal browsing can trigger it by accident with zero warning. This shows a
// big on-screen countdown starting partway into any hold, so you can just let go.
static constexpr uint32_t HOLD_WARNING_START_MS = 2500;   // when the countdown appears
static lv_obj_t *g_holdWarning    = nullptr;
static lv_obj_t *g_holdWarningLbl = nullptr;

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

// Set home location from the Settings menu and reboot to re-center radar + weather
// (mirrors the web /save handler, which also restarts). Reuses the hold-warning overlay
// (built in build_hold_warning()) for a visible countdown first — a silent 250ms-later
// reboot was jarring with no explanation of what was happening or why.
void host_set_location(double lat, double lon) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putDouble("homeLat", lat);
    p.putDouble("homeLon", lon);
    // Also clears the post-Reset "needs setup" flag (see host_factory_reset()) — the
    // auto-locate path (host_locate_current()) lands here directly on success, without
    // going through host_wifi_connected_reboot(), so that was the one path that left
    // the flag stuck set and kept forcing WiFi setup open on every boot after.
    p.putBool("needsWifiSetup", false);
    p.end();

    if (g_holdWarning && g_holdWarningLbl) {
        lv_obj_clear_flag(g_holdWarning, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_holdWarning);
        for (int s = 3; s >= 1; s--) {
            char buf[48];
            snprintf(buf, sizeof(buf), "LOCATION SAVED\n\nRESTARTING IN %ds", s);
            lv_label_set_text(g_holdWarningLbl, buf);
            lv_refr_now(NULL);
            delay(1000);
        }
    } else {
        delay(250);
    }
    ESP.restart();
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

// Named chime library (Settings > Sound > Chime sound). Only one entry exists today
// (Westminster) but the picker UI and NVS persistence are built for more.
int  host_chime_count()          { return audio_chime_count(); }
const char *host_chime_name(int i) { return audio_chime_name(i); }
int  host_chime_index()          { return audio_chime_index(); }
void host_chime_set(int i) {
    audio_set_chime(i);
    Preferences p; p.begin("capsuleradar", false); p.putInt("chimeIdx", i); p.end();
}
void host_chime_preview(int i) { if (audio_present()) audio_preview_chime(i); }

void host_recents_add(const char *name, double lat, double lon);   // defined below

// Approximate current location from the public IP (city-level; no GPS on this board).
// Reboots via host_set_location() on success and never returns; returns false quietly
// on failure so a caller that needs a location set either way (first-boot WiFi setup)
// can fall back to something else instead of getting stuck waiting for a reboot that
// isn't coming.
// Build a POSIX TZ string (fixed offset, no DST) from a UTC offset in seconds, as ip-api's
// "offset" field reports it. Correct year-round for zones that don't observe DST (Arizona,
// most of Asia); for DST zones it's correct as of when the device located and drifts an
// hour after the next DST change until it re-locates. Fixed-offset because deriving proper
// POSIX DST rules would need a full timezone database the device doesn't carry.
static void posix_tz_from_offset(long offsetSec, char *out, size_t n) {
    const long offMin  = offsetSec / 60;    // minutes east of UTC (Arizona: -420)
    const long westMin = -offMin;           // POSIX offset field is positive west of UTC
    const char sign = offMin < 0 ? '-' : '+';
    const int aH = (int)(labs(offMin) / 60), aM = (int)(labs(offMin) % 60);
    const int wh = (int)(westMin / 60),       wm = (int)(labs(westMin) % 60);
    if (aM == 0) snprintf(out, n, "<%c%02d>%d", sign, aH, wh);
    else         snprintf(out, n, "<%c%02d%02d>%d:%02d", sign, aH, aM, wh, wm);
}

bool host_locate_current() {
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    // Ask for `offset` (UTC offset in seconds) alongside the position so the clock's
    // timezone follows the located region, not just the map centre.
    if (!http.begin(client, "http://ip-api.com/json/?fields=status,message,city,region,lat,lon,offset")) return false;
    const int code = http.GET();
    if (code != 200) { http.end(); return false; }
    String body = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (String((const char *)(doc["status"] | "")) != "success") return false;
    const double lat = doc["lat"] | 1000.0;
    const double lon = doc["lon"] | 1000.0;
    if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180) {
        const char *city   = doc["city"]   | "";
        const char *region = doc["region"] | "";
        if (city[0]) {
            char nm[40];
            snprintf(nm, sizeof(nm), "%s%s%s", city, region[0] ? ", " : "", region);
            host_recents_add(nm, lat, lon);            // remember where we landed
        }
        // Derive + persist the timezone before the reboot, so the clock comes back on
        // local time. host_set_location() writes homeLat/Lon and reboots (never returns).
        const long off = doc["offset"] | 0x7FFFFFFFL;
        if (off != 0x7FFFFFFFL && off >= -50400 && off <= 50400) {
            char tz[24];
            posix_tz_from_offset(off, tz, sizeof(tz));
            g_tz = tz;
            Preferences p;
            p.begin("capsuleradar", false);
            p.putString("tz", tz);
            p.end();
            Serial.printf("[locate] tz offset %lds -> %s\n", off, tz);
        }
        host_set_location(lat, lon);   // saves + reboots — does not return
        return true;
    }
    return false;
}

// Free city search (Open-Meteo geocoding, no key). Fills names/lats/lons with up to
// maxN matches for the query; returns the count. Runs synchronously (~1s).
int host_geocode(const char *query, char names[][40], double *lats, double *lons, int maxN) {
    if (WiFi.status() != WL_CONNECTED || !query || strlen(query) < 2) return 0;
    String q;
    for (const char *p = query; *p; ++p) q += (*p == ' ') ? String("%20") : String(*p);
    // Plain HTTP for the same memory reason as the ADS-B and weather feeds; see the
    // ADSB_PRIMARY_TLS notes in config.h. This is a place-name lookup with no credentials.
    String url = "http://geocoding-api.open-meteo.com/v1/search?name=" + q +
                 "&count=" + String(maxN) + "&language=en&format=json";
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(4000);
    http.setTimeout(6000);
    if (!http.begin(client, url)) return 0;
    if (http.GET() != 200) { http.end(); return 0; }
    String body = http.getString();
    http.end();
    JsonDocument doc;
    if (deserializeJson(doc, body)) return 0;
    JsonArrayConst results = doc["results"].as<JsonArrayConst>();
    int n = 0;
    for (JsonObjectConst r : results) {
        if (n >= maxN) break;
        const char *name   = r["name"]   | "";
        const char *admin1 = r["admin1"] | "";
        const char *cc     = r["country_code"] | "";
        const char *sub    = admin1[0] ? admin1 : cc;
        snprintf(names[n], 40, "%s%s%s", name, sub[0] ? ", " : "", sub);
        lats[n] = r["latitude"]  | 1000.0;
        lons[n] = r["longitude"] | 1000.0;
        if (lats[n] <= 90 && lats[n] >= -90) n++;
    }
    return n;
}

// ---- recent cities (small list persisted in NVS, survives the reboot on location change) ----
static const int RECENTS_MAX = 8;

int host_recents_get(char names[][40], double *lats, double *lons, int maxN) {
    Preferences p;
    p.begin("capsuleradar", true);
    String blob = p.getString("recents", "");
    p.end();
    if (blob.length() == 0) return 0;
    JsonDocument doc;
    if (deserializeJson(doc, blob)) return 0;
    JsonArrayConst arr = doc.as<JsonArrayConst>();
    int n = 0;
    for (JsonObjectConst r : arr) {
        if (n >= maxN) break;
        snprintf(names[n], 40, "%s", (const char *)(r["n"] | ""));
        lats[n] = r["la"] | 1000.0;
        lons[n] = r["lo"] | 1000.0;
        if (names[n][0] && lats[n] <= 90 && lats[n] >= -90) n++;
    }
    return n;
}

void host_recents_add(const char *name, double lat, double lon) {
    if (!name || !name[0]) return;
    char   names[RECENTS_MAX][40];
    double lats[RECENTS_MAX], lons[RECENTS_MAX];
    const int n = host_recents_get(names, lats, lons, RECENTS_MAX);
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    { JsonObject o = arr.add<JsonObject>(); o["n"] = name; o["la"] = lat; o["lo"] = lon; }
    int count = 1;
    for (int i = 0; i < n && count < RECENTS_MAX; ++i) {
        if (strcmp(names[i], name) == 0) continue;      // the new front entry already covers it
        JsonObject o = arr.add<JsonObject>();
        o["n"] = names[i]; o["la"] = lats[i]; o["lo"] = lons[i];
        count++;
    }
    String out;
    serializeJson(doc, out);
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("recents", out);
    p.end();
}

// Like host_set_location but records the named city in the recents list first (then reboots).
void host_set_location_named(const char *name, double lat, double lon) {
    host_recents_add(name, lat, lon);
    host_set_location(lat, lon);
}

// --- On-device WiFi setup (Settings > WiFi) --------------------------------------
// Lets the user scan/select/enter-password entirely from the knob+touchscreen, no
// phone or captive portal needed. Scanning is async (WiFi.scanNetworks(true)) so it
// never blocks the UI thread; settings_view.cpp polls host_wifi_scan_result() from a
// timer. Connecting hands off cleanly from WiFiManager's non-blocking portal, then
// mirrors WiFiManager's own approach on success: reboot for a clean start, since this
// chip's WiFi/web/mDNS stack doesn't reliably hot-swap networks in place.
void host_wifi_scan_start() { WiFi.scanNetworks(true /*async*/); }

// Returns: -1 = scan still running, -2 = scan failed, >=0 = number of networks written.
// Deduplicates repeated SSIDs (multiple access points/bands) to the strongest signal.
int host_wifi_scan_result(char names[][33], int8_t *rssi, bool *isOpen, int maxN) {
    const int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return -1;
    if (n == WIFI_SCAN_FAILED || n < 0) return -2;
    int count = 0;
    for (int i = 0; i < n && count < maxN; ++i) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        int dupIdx = -1;
        for (int j = 0; j < count; ++j) if (strcmp(names[j], ssid.c_str()) == 0) { dupIdx = j; break; }
        const int8_t r = (int8_t)WiFi.RSSI(i);
        if (dupIdx >= 0) { if (r > rssi[dupIdx]) rssi[dupIdx] = r; continue; }   // keep the stronger AP
        snprintf(names[count], 33, "%s", ssid.c_str());
        rssi[count] = r;
        isOpen[count] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
        count++;
    }
    return count;
}

void host_wifi_connect(const char *ssid, const char *pass) {
    if (g_wm.getConfigPortalActive()) g_wm.stopConfigPortal();   // hand off cleanly
    WiFi.begin(ssid, pass);
}

// 0 = still connecting, 1 = connected, 2 = failed/rejected.
int host_wifi_connect_status() {
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) return 1;
    if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) return 2;
    return 0;
}

// Called on a successful manual connect: reboot shortly after, mirroring
// g_wm.setSaveConfigCallback() — same reasoning (clean web/mDNS start). Also clears the
// post-Reset "needs setup" flag (see host_factory_reset()) so the next boot is normal.
void host_wifi_connected_reboot() {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putBool("needsWifiSetup", false);
    p.end();
    g_rebootAtMs = millis() + 1500;
}

// ----------------------------- configuration web --------------------------------
static WebServer g_web(80);

static void handleRoot() {
    const int th = radar::theme();
    const int ranges[] = {10, 15, 25, 30, 50, 100, 150, 250};
    // The value submitted stays in km (the device works in km); only the label is shown in
    // the user's chosen distance unit so the config page matches the screen.
    const float    ufac  = (g_units == 0) ? 0.539957f : (g_units == 2 ? 0.621371f : 1.0f);
    const char    *uname = (g_units == 0) ? "nm" : (g_units == 2 ? "mi" : "km");
    String ropts;
    for (int r : ranges) {
        char o[72];
        snprintf(o, sizeof(o), "<option value=%d%s>%.0f %s</option>",
                 r, (r == (int)(g_settings.rangeKm + 0.5f)) ? " selected" : "", r * ufac, uname);
        ropts += o;
    }
    String topts;
    for (int i = 0; i < THEME_COUNT; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == th ? " selected" : "", radar::themeName(i));
        topts += o;
    }
    const int idleSecs[] = {10, 20, 30, 60, 120, 300, 1800, 3600, 7200, 14400, 28800};
    const int curIdle = (int)(g_idleDimMs / 1000);
    String iopts;
    for (int sV : idleSecs) {
        char lbl[16];
        if      (sV < 60)   snprintf(lbl, sizeof(lbl), "%d s", sV);
        else if (sV < 3600) snprintf(lbl, sizeof(lbl), "%d min", sV / 60);
        else                snprintf(lbl, sizeof(lbl), "%d h", sV / 3600);
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", sV, sV == curIdle ? " selected" : "", lbl);
        iopts += o;
    }
    { char o[64]; snprintf(o, sizeof(o), "<option value=0%s>Never</option>", curIdle == 0 ? " selected" : ""); iopts += o; }
    const char *unames[] = {"Aviation (ft, kt, nm)", "Metric (m, km/h, km)", "Imperial (ft, mph, mi)"};
    String uopts;
    for (int i = 0; i < 3; ++i) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_units ? " selected" : "", unames[i]);
        uopts += o;
    }
    const char *tlnames[] = {"Off", "Short", "Medium", "Long"};
    String tlopts;
    for (int i = 0; i < 4; ++i) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_trailLen ? " selected" : "", tlnames[i]);
        tlopts += o;
    }
    const int mxvals[] = {10, 15, 20, 30, 40, 60};   // max aircraft on the scope (<= feed cap)
    String mxopts;
    for (int mv : mxvals) {
        char o[64];
        snprintf(o, sizeof(o), "<option value=%d%s>%d</option>", mv, mv == g_maxAc ? " selected" : "", mv);
        mxopts += o;
    }
    // minimum-altitude filter options (stored in ft; labels show ft + km for clarity)
    const struct { int ft; const char *lbl; } mavals[] = {
        {0, "Off"}, {5000, "&gt; 5,000 ft (1.5 km)"}, {10000, "&gt; 10,000 ft (3 km)"},
        {20000, "&gt; 20,000 ft (6 km)"}, {33000, "&gt; 33,000 ft (10 km)"},
    };
    String maopts;
    for (auto &mv : mavals) {
        char o[96];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", mv.ft, mv.ft == g_minAltFt ? " selected" : "", mv.lbl);
        maopts += o;
    }
    const char *anames[] = {"Off", "Emergencies only", "New aircraft + emergencies"};
    String aopts;
    for (int i = 0; i < 3; ++i) {
        char o[80];
        snprintf(o, sizeof(o), "<option value=%d%s>%s</option>", i, i == g_alertMode ? " selected" : "", anames[i]);
        aopts += o;
    }
    const int proxUnit[] = {0, 2, 5, 10, 25};   // 0 = off; rest in the user's distance unit
    String popts;
    for (int pv : proxUnit) {
        const float pkm = (pv == 0) ? 0.0f : (pv / ufac);   // user unit -> km (value submitted)
        const bool  sel = (pv == 0) ? (g_proximityKm <= 0.0f) : (fabsf(g_proximityKm - pkm) < 0.4f);
        char lbl[24];
        if (pv == 0) snprintf(lbl, sizeof(lbl), "Off");
        else         snprintf(lbl, sizeof(lbl), "%d %s", pv, uname);
        char o[80];
        snprintf(o, sizeof(o), "<option value=%.3f%s>%s</option>", pkm, sel ? " selected" : "", lbl);
        popts += o;
    }
    String tzopts;   // time-zone dropdown (value = index into TZOPTS; mapped to POSIX TZ on save)
    for (int i = 0; i < TZOPTS_N; ++i) {
        char o[128];
        snprintf(o, sizeof(o), "<option value=%d data-off=%d data-dst=%d%s>%s</option>",
                 i, TZOPTS[i].offMin, TZOPTS[i].dst, g_tz == TZOPTS[i].tz ? " selected" : "", TZOPTS[i].label);
        tzopts += o;
    }
    String gpsRow;   // only on the -G variant: offer to auto-set the centre from GPS
    if (gps_present()) {
        gpsRow  = "<label><input type=checkbox class=ck ";
        gpsRow += g_useGps ? "checked" : "";
        gpsRow += " onchange='gp(this.checked)'>Use GPS for location</label>";
        gpsRow += "<div style='font-size:12px;opacity:.6;margin:-2px 0 6px'>"
                  "When on, the location above is used until the GPS gets a fix, then it takes over.</div>";
    }
    static const size_t BUFSZ = 10240;
    static char *buf = (char *)ps_malloc(BUFSZ);   // PSRAM: keep this big page buffer off the scarce
    if (!buf) return;                              //   internal heap (the contiguous RAM mbedTLS needs)
    snprintf(buf, BUFSZ,
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar</title>"
        "<link rel=stylesheet href='https://unpkg.com/leaflet@1.9.4/dist/leaflet.css'>"
        "<script src='https://unpkg.com/leaflet@1.9.4/dist/leaflet.js'></script>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{background:radial-gradient(circle at 50%% -10%%,#0a1f15,#04100a 70%%);color:#cdd6d1;"
        "font-family:system-ui,-apple-system,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        ".hd{display:flex;align-items:center;gap:12px;margin-bottom:16px}"
        ".dot{width:44px;height:44px;border-radius:50%%;border:2px solid #1dff86;position:relative;"
        "overflow:hidden;flex:0 0 auto;box-shadow:0 0 16px rgba(29,255,134,.4)}"
        ".dot::before{content:'';position:absolute;inset:0;animation:sw 3s linear infinite;"
        "background:conic-gradient(from 0deg,rgba(29,255,134,.65),transparent 55%%)}"
        "@keyframes sw{to{transform:rotate(360deg)}}"
        "h1{color:#1dff86;font-size:20px;margin:0}.sub{color:#6f8c7d;font-size:12px;margin:2px 0 0}"
        ".t{color:#1dff86;font-size:11px;letter-spacing:1.5px;text-transform:uppercase;margin-bottom:10px;opacity:.85}"
        "label{display:block;margin:12px 0 4px;color:#9affc8;font-size:13px}"
        "input,select{width:100%%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #2a4a39;"
        "background:#0c1a12;color:#eafff3;font-size:16px}"
        "input:focus,select:focus{outline:none;border-color:#1dff86;box-shadow:0 0 0 2px rgba(29,255,134,.18)}"
        "button{margin-top:16px;width:100%%;padding:12px;border:0;border-radius:8px;background:#1dff86;"
        "color:#04140b;font-weight:700;font-size:16px}button:active{opacity:.85}"
        ".w{background:#ffb23c}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px;margin-bottom:14px}"
        ".ft{color:#5f7a6c;font-size:12px;text-align:center;margin-top:6px}.ft code{color:#9affc8}"
        ".ck{width:auto;display:inline;margin-right:8px;vertical-align:middle}"
        ".sec{background:#0c1a12!important;color:#1dff86!important;border:1px solid #2a4a39!important}"
        "#map{height:220px;border-radius:10px;margin:6px 0 8px;border:1px solid #2a4a39;z-index:0}"
        "</style></head><body>"
        "<div class=hd><div class=dot></div><div><h1>Capsule Radar</h1><p class=sub>Live ADS-B radar &middot; configuration</p></div></div>"
        "<div class=card><div class=t>Location &amp; range</div><form method=POST action=/save>"
        "<label>Center point &mdash; tap the map or drag the pin</label>"
        "<div id=map></div>"
        "<label>Center latitude</label><input id=lat name=lat value='%.5f'>"
        "<label>Center longitude</label><input id=lon name=lon value='%.5f'>"
        "%s"
        "<label>Display range</label><select name=range>%s</select>"
        "<label>Theme</label><select name=theme>%s</select>"
        "<label>Time zone</label><select name=tz>%s</select>"
        "<button>Save &amp; restart</button></form></div>"
        "<div class=card><div class=t>Display</div>"
        "<label>Brightness</label>"
        "<input type=range min=5 max=255 value='%d' oninput='b(this.value,0)' onchange='b(this.value,1)'>"
        "<label>Dim screen after</label><select onchange='d(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='sw(this.checked)'>Show radar sweep</label>"
        "<label><input type=checkbox class=ck %s onchange='ap(this.checked)'>Show airports</label>"
        "<label><input type=checkbox class=ck %s onchange='hg(this.checked)'>Hide aircraft on the ground</label>"
        "<label>Minimum altitude</label><select onchange='ma(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='mo(this.checked)'>Military aircraft only</label>"
        "<label>Aircraft trails</label><select onchange='tl(this.value)'>%s</select>"
        "<label>Max aircraft on screen</label><select onchange='mx(this.value)'>%s</select>"
        "<label><input type=checkbox class=ck %s onchange='bt(this.checked)'>Large text (restarts the device)</label>"
        "<label>Screen rotation (degrees clockwise)</label>"
        "<input type=number min=0 max=359 step=1 value='%d' onchange='ro(this.value)'>"
        "<label>Units</label><select onchange='u(this.value)'>%s</select></div>"
        "<div class=card><div class=t>Sound</div>"
        "<label>Volume</label>"
        "<input type=range min=0 max=100 value='%d' oninput='v(this.value,0)' onchange='v(this.value,1)'>"
        "<label><input type=checkbox class=ck %s onchange='m(this.checked)'>Mute alerts</label>"
        "<label>Alert on</label><select onchange='al(this.value)'>%s</select>"
        "<label>Proximity alert</label><select onchange='px(this.value)'>%s</select>"
        "<button type=button class=sec onclick='t()'>Test ping</button></div>"
        "<div class=card><div class=t>Network</div>"
        "<p style='color:#9affc8;font-size:13px;margin:0 0 4px'>Forget the saved WiFi and reopen the setup portal.</p>"
        "<form method=POST action=/wifi><button class=w>Reset WiFi</button></form></div>"
        // The "Firmware update" link only exists when there is an OTA partition to write
        // into; otherwise it would advertise a page that 404s.
#if ORB_OTA_ENABLED
        "<p class=ft>Reach me at <code>capsuleradar.local</code> &middot; <a href=/update style='color:#9affc8'>Firmware update</a> &middot; v" FW_VERSION "</p>"
#else
        "<p class=ft>Reach me at <code>capsuleradar.local</code> &middot; Update over USB &middot; v" FW_VERSION "</p>"
#endif
        "<script>"
        "var C=[%.5f,%.5f];var MAP=L.map('map').setView(C,10);"
        "L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'(c) OpenStreetMap'}).addTo(MAP);"
        "var MK=L.marker(C,{draggable:true}).addTo(MAP);"
        "function S(p){document.getElementById('lat').value=p.lat.toFixed(5);document.getElementById('lon').value=p.lng.toFixed(5);}"
        "MK.on('dragend',function(){S(MK.getLatLng());});"
        "MAP.on('click',function(e){MK.setLatLng(e.latlng);S(e.latlng);});"
        "setTimeout(function(){MAP.invalidateSize();},300);"
        "function b(v,s){fetch('/bright?v='+v+(s?'&save=1':''))}"
        "function v(x,s){fetch('/vol?v='+x+(s?'&save=1':''))}"
        "function m(c){fetch('/vol?mute='+(c?1:0)+'&save=1')}"
        "function t(){fetch('/vol?test=1')}"
        "function d(v){fetch('/idle?v='+v+'&save=1')}"
        "function sw(c){fetch('/sweep?v='+(c?1:0)+'&save=1')}"
        "function ap(c){fetch('/airports?v='+(c?1:0)+'&save=1')}"
        "function hg(c){fetch('/ground?v='+(c?1:0)+'&save=1')}"
        "function ma(v){fetch('/altmin?v='+v+'&save=1')}"
        "function mo(c){fetch('/milonly?v='+(c?1:0)+'&save=1')}"
        "function tl(v){fetch('/trail?v='+v+'&save=1')}"
        "function mx(v){fetch('/maxac?v='+v+'&save=1')}"
        "function bt(c){fetch('/bigtext?v='+(c?1:0)+'&save=1')}"
        "function ro(v){fetch('/rotate?v='+v+'&save=1')}"
        "function u(v){fetch('/units?v='+v+'&save=1')}"
        "function al(v){fetch('/alerts?mode='+v+'&save=1')}"
        "function px(v){fetch('/alerts?prox='+v+'&save=1')}"
        "function gp(c){fetch('/gps?v='+(c?1:0)+'&save=1')}"
        // auto-pick the visitor's time zone from their browser clock (only if they haven't set one)
        "var TZSET=%d;(function(){if(TZSET)return;"
        "var d=new Date(),j=new Date(d.getFullYear(),0,1).getTimezoneOffset(),"
        "u=new Date(d.getFullYear(),6,1).getTimezoneOffset(),o=-Math.max(j,u),s=(j!=u)?1:0,"
        "e=document.querySelector('select[name=tz]'),b=-1,i;"
        "for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o&&+e.options[i].dataset.dst===s){b=i;break;}}"
        "if(b<0)for(i=0;i<e.options.length;i++){if(+e.options[i].dataset.off===o){b=i;break;}}"
        "if(b>=0)e.selectedIndex=b;})();</script></body></html>",
        g_settings.homeLat, g_settings.homeLon, gpsRow.c_str(), ropts.c_str(), topts.c_str(),
        tzopts.c_str(),
        g_brightnessDay, iopts.c_str(), g_showSweep ? "checked" : "",
        g_showAirports ? "checked" : "", g_hideGround ? "checked" : "", maopts.c_str(), g_milOnly ? "checked" : "",
        tlopts.c_str(), mxopts.c_str(), g_bigText ? "checked" : "", g_rotation, uopts.c_str(),
        g_volume, g_muted ? "checked" : "", aopts.c_str(), popts.c_str(),
        g_settings.homeLat, g_settings.homeLon, (g_tz == TZ_STR ? 0 : 1));
    g_web.send(200, "text/html", buf);
}

static void handleSave() {
    Preferences p;
    p.begin("capsuleradar", false);
    // Reject out-of-range coordinates so a typo can't leave the radar unusable.
    if (g_web.hasArg("lat")) {
        const double lat = g_web.arg("lat").toDouble();
        if (lat >= -90.0 && lat <= 90.0) p.putDouble("homeLat", lat);
    }
    if (g_web.hasArg("lon")) {
        const double lon = g_web.arg("lon").toDouble();
        if (lon >= -180.0 && lon <= 180.0) p.putDouble("homeLon", lon);
    }
    if (g_web.hasArg("range")) p.putFloat("rangeKm", g_web.arg("range").toFloat());
    if (g_web.hasArg("theme")) p.putInt("theme", g_web.arg("theme").toInt());
    if (g_web.hasArg("tz")) {
        const int i = g_web.arg("tz").toInt();
        if (i >= 0 && i < TZOPTS_N) p.putString("tz", TZOPTS[i].tz);
    }
    p.end();
    g_web.send(200, "text/html",
        "<meta http-equiv=refresh content='4;url=/'><body style='background:#06100a;color:#1dff86;"
        "font-family:sans-serif;padding:24px'>Saved. Restarting&hellip;</body>");
    delay(400);
    ESP.restart();
}

static void handleWifi() {
    g_web.send(200, "text/html",
        "<body style='background:#06100a;color:#ffb23c;font-family:sans-serif;padding:24px'>"
        "WiFi reset. Connect to the <b>The Orb Setup</b> network to reconfigure.</body>");
    delay(400);                     // let the response reach the browser
    // The driver stores the saved AP in its own NVS namespace ("nvs.net80211"). On Arduino
    // core 3.x both wm.resetSettings() and WiFi.disconnect(true,true) can silently no-op
    // (they fail once the driver is off), so v1.3.19's reset still reconnected. Erasing that
    // namespace directly is unconditional — it works whatever state the WiFi driver is in.
    g_wm.resetSettings();           // best-effort driver-level erase first...
    WiFi.disconnect(false, true);   // ...keep WiFi up so the erase can actually run
    delay(100);
    nvs_handle_t h;                 // ...then the guaranteed path: wipe the driver's namespace
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    delay(300);                     // let NVS finish committing before the reboot
    ESP.restart();
}

static void handleBright() {
    if (g_web.hasArg("v")) {
        g_brightnessDay = constrain((int)g_web.arg("v").toInt(), 0, 255);
        applyBrightness();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("bright", g_brightnessDay);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleVol() {
    if (g_web.hasArg("v"))    { g_volume = constrain((int)g_web.arg("v").toInt(), 0, 100); audio_set_volume(g_volume); }
    if (g_web.hasArg("mute")) { g_muted = g_web.arg("mute").toInt() != 0; audio_set_muted(g_muted); }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("vol", g_volume);
        p.putBool("mute", g_muted);
        p.end();
    }
    if (g_web.hasArg("test")) {
        if (g_web.arg("test").toInt() == 2) audio_selftest();   // long tone, ignores mute
        else audio_play(AUDIO_NEW);
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAlerts() {   // what triggers the alert sound (live)
    if (g_web.hasArg("mode")) g_alertMode   = constrain((int)g_web.arg("mode").toInt(), 0, 2);
    if (g_web.hasArg("prox")) {
        g_proximityKm = g_web.arg("prox").toFloat();   // km (0 = off)
        g_requeryKm = queryRadiusKm();                 // the query must cover the new alert circle
        g_requery = true;
    }
    if (g_web.hasArg("save")) {
        Preferences p;
        p.begin("capsuleradar", false);
        p.putInt("alertmode", g_alertMode);
        p.putFloat("proxkm", g_proximityKm);
        p.end();
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleIdle() {   // idle auto-dim timeout (seconds; 0 = never)
    if (g_web.hasArg("v")) {
        const long s = g_web.arg("v").toInt();
        g_idleDimMs = (s <= 0) ? 0 : (uint32_t)s * 1000;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putUInt("idledim", g_idleDimMs);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleUnits() {   // measurement units preset (live re-render)
    if (g_web.hasArg("v")) {
        g_units = constrain((int)g_web.arg("v").toInt(), 0, 2);
        ui_set_units(g_units);
        ui_on_data_updated();                  // re-render card/list/stats in the new units
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("units", g_units);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleSweep() {   // show/hide the rotating sweep line (live)
    if (g_web.hasArg("v")) {
        g_showSweep = g_web.arg("v").toInt() != 0;
        radar::setSweepEnabled(g_showSweep);          // loop()/core 1: safe to touch LVGL
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("sweep", g_showSweep);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleTrail() {   // aircraft trail length 0/1/2/3 (live)
    if (g_web.hasArg("v")) {
        g_trailLen = constrain((int)g_web.arg("v").toInt(), 0, 3);
        radar::setTrailLength(g_trailLen);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("traillen", g_trailLen);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAltMin() {   // minimum-altitude feed filter, ft (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_minAltFt = constrain((int)g_web.arg("v").toInt(), 0, 60000);
        g_adsb.setMinAltFt((float)g_minAltFt);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("minalt", g_minAltFt);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMilOnly() {   // military-only feed filter (applies from the next poll)
    if (g_web.hasArg("v")) {
        g_milOnly = g_web.arg("v").toInt() != 0;
        g_adsb.setMilitaryOnly(g_milOnly);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("milonly", g_milOnly);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleBigText() {   // accessibility: large fonts. Fonts are baked at UI creation,
    if (g_web.hasArg("v")) {    // so persist the flag and reboot cleanly to apply it.
        g_bigText = g_web.arg("v").toInt() != 0;
        Preferences p;
        p.begin("capsuleradar", false);
        p.putBool("bigtext", g_bigText);
        p.end();
        g_rebootAtMs = millis() + 1200;   // let this response reach the browser first
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleMaxAc() {   // max aircraft drawn on the scope (live)
    if (g_web.hasArg("v")) {
        g_maxAc = constrain((int)g_web.arg("v").toInt(), 1, ADSB_MAX_AIRCRAFT);
        radar::setMaxOnScreen(g_maxAc);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("maxac", g_maxAc);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleAirports() {   // show/hide airport markers (live)
    if (g_web.hasArg("v")) {
        g_showAirports = g_web.arg("v").toInt() != 0;
        radar::setAirportsEnabled(g_showAirports);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("airports", g_showAirports);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGround() {   // hide/show on-ground aircraft (applies from the next feed poll)
    if (g_web.hasArg("v")) {
        g_hideGround = g_web.arg("v").toInt() != 0;
        g_adsb.setHideGround(g_hideGround);
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("hideground", g_hideGround);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleRotate() {   // arbitrary clockwise display rotation, applied live
    if (g_web.hasArg("v")) {
        g_rotation = constrain((int)g_web.arg("v").toInt(), 0, 359);
        display::setRotation((uint16_t)g_rotation);
        g_rotation = display::rotation();
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putInt("rotDeg", g_rotation);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

static void handleGps() {   // auto-set the centre point from the LC76G GPS (-G variant)
    if (g_web.hasArg("v")) {
        g_useGps = g_web.arg("v").toInt() != 0;
        if (g_web.hasArg("save")) {
            Preferences p;
            p.begin("capsuleradar", false);
            p.putBool("usegps", g_useGps);
            p.end();
        }
    }
    g_web.send(200, "text/plain", "ok");
}

#if ORB_OTA_ENABLED
// ---- browser OTA: upload an app .bin over WiFi and self-flash ----
static void handleUpdatePage() {
    g_web.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Capsule Radar - Update</title><style>"
        "body{background:radial-gradient(circle at 50% -10%,#0a1f15,#04100a 70%);color:#cdd6d1;"
        "font-family:system-ui,sans-serif;margin:0 auto;padding:20px;max-width:480px;min-height:100vh}"
        "h1{color:#1dff86;font-size:20px}.card{background:rgba(10,20,14,.85);border:1px solid #1f3a2b;border-radius:14px;padding:16px}"
        "input,button{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;margin-top:8px;font-size:16px}"
        "input{background:#0c1a12;color:#eafff3;border:1px solid #2a4a39}"
        "button{border:0;background:#1dff86;color:#04140b;font-weight:700}"
        "#bar{height:12px;background:#0c1a12;border-radius:6px;overflow:hidden;margin-top:14px;display:none}"
        "#fill{height:100%;width:0;background:#1dff86;transition:width .2s}#msg{margin-top:10px;color:#9affc8;font-size:13px}"
        "a{color:#1dff86}p{color:#9affc8;font-size:13px}"
        "</style></head><body><h1>Firmware update (OTA)</h1><div class=card>"
        "<p>Upload the <b>app firmware</b> <code>CapsuleRadar-ota.bin</code> from the GitHub release. "
        "Do NOT use the merged flash image here.</p>"
        "<input type=file id=f accept='.bin'>"
        "<button onclick=u()>Update over WiFi</button>"
        "<div id=bar><div id=fill></div></div><div id=msg></div></div>"
        "<p style='text-align:center;margin-top:14px'><a href=/>&larr; Back to settings</a></p>"
        "<script>function u(){var f=document.getElementById('f').files[0];if(!f){return}"
        "var x=new XMLHttpRequest(),fd=new FormData();fd.append('f',f);"
        "document.getElementById('bar').style.display='block';"
        "x.upload.onprogress=function(e){if(e.lengthComputable)document.getElementById('fill').style.width=(e.loaded/e.total*100)+'%'};"
        "x.onload=function(){document.getElementById('msg').innerText=x.responseText+' - rebooting...'};"
        "x.onerror=function(){document.getElementById('msg').innerText='Upload failed'};"
        "x.open('POST','/update');x.send(fd);}</script></body></html>");
}

static void handleUpdateUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[update] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[update] done: %u bytes\n", (unsigned)up.totalSize);
        else Update.printError(Serial);
    }
}
#endif  // ORB_OTA_ENABLED

// ---- theme file upload over WiFi -----------------------------------------
// POST /sdput?path=/themes/<slug>/<file>, multipart body, streamed to the microSD in
// chunks. Theme art no longer lives in the firmware binary (it stopped fitting: a
// detailed design needs several MB against a ~944 KB flash budget), so it has to reach
// the card some other way. The alternative was pulling the card out and using a reader
// on every push. The Orb is already on WiFi with a web server running, so Launch Kit
// POSTs the files here instead.
//
// No concurrency guard needed: g_web.handleClient() and every screen's SD decode both
// run inside loop() on core 1, so card access is serialised by construction. adsb_task
// (core 0) never touches SD.
static File   g_sdUpFile;
static bool   g_sdUpOk = false;
static String g_sdUpPath;

// Confined to /themes/. A malformed or hostile request must not be able to scribble
// over Spy Cam clips, road tiles, or anything else living on the card.
static bool sd_put_path_ok(const String &p) {
    return p.startsWith("/themes/") && p.indexOf("..") < 0 && p.length() > 8 && p.length() < 96;
}

// The Orb's own install page: pick a .orb file, watch it land on the card.
//
// This exists because Studio CANNOT push over the network. Studio is served over HTTPS, the
// Orb can only ever speak HTTP (there is not enough contiguous memory here for TLS), and a
// browser refuses to let a secure page call an insecure address. Turning the direction round
// solves it completely: an HTTP page on the Orb, talking to the Orb, upsets nobody. It also
// means an iPad or a phone can install a theme, which the USB cable can never do.
//
// The unpacking happens HERE, in the browser, not in C++. The file is a trivial container
// (see orb-bundle.ts) and the page POSTs its contents one at a time to /sdput, which already
// streams to the card. So a device with 320 KB of RAM never parses an archive, never holds a
// theme in memory, and this whole feature costs one static page and no new upload code.
static const char INSTALL_PAGE[] PROGMEM = R"HTML(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>My Orb</title><style>
*{box-sizing:border-box}
body{font:16px system-ui;margin:0;padding:22px 18px 60px;background:#101418;color:#e8edf2}
h1{font-size:20px;margin:0 0 2px}h2{font-size:12px;letter-spacing:.14em;text-transform:uppercase;color:#6a7480;margin:26px 0 10px}
p{color:#98a2ad;margin:4px 0 16px;line-height:1.5}
.row{display:flex;align-items:center;gap:10px;padding:12px 14px;border:1px solid #29323b;border-radius:12px;margin-bottom:8px;background:#161c22}
.row.on{border-color:#3c6338;background:#16211a}
.nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tag{font-size:11px;padding:2px 8px;border-radius:999px;background:#22303a;color:#8fb98a;white-space:nowrap}
button,label.btn{font:14px system-ui;padding:7px 12px;border:1px solid #3a444f;border-radius:9px;background:none;color:#c7ced6;cursor:pointer}
button:hover,label.btn:hover{border-color:#e05a3a;color:#fff}
button:disabled{opacity:.4;cursor:default}
label.btn{display:inline-block}input[type=file]{display:none}
#log{margin-top:14px;font:13px ui-monospace,monospace;white-space:pre-wrap;color:#98a2ad}
.ok{color:#7ddb8a}.bad{color:#ff8a6a}
</style>
<h1>My Orb</h1>
<p id=sub>Loading…</p>

<h2>Install a theme</h2>
<p>Download a theme from Orb Studio, then choose the file here.</p>
<label class=btn>Choose a .orb file<input type=file accept=".orb" id=f></label>
<div id=log></div>

<h2>On this Orb</h2>
<div id=list></div>

<script>
const $=s=>document.querySelector(s), log=$('#log');
const say=(m,c)=>{const d=document.createElement('div');if(c)d.className=c;d.textContent=m;log.appendChild(d)};

async function refresh(){
 const r=await fetch('/themes.json',{cache:'no-store'}); const d=await r.json();
 const box=$('#list'); box.textContent='';
 for(const t of d.themes){
  const worn = t.slug===d.active;
  const row=document.createElement('div'); row.className='row'+(worn?' on':'');
  const nm=document.createElement('span'); nm.className='nm'; nm.textContent=t.name; row.appendChild(nm);
  if(worn){ const g=document.createElement('span'); g.className='tag'; g.textContent='Wearing this'; row.appendChild(g); }
  else{
   const w=document.createElement('button'); w.textContent='Wear'; w.onclick=async()=>{
    w.disabled=true; await fetch('/theme?slug='+encodeURIComponent(t.slug));
    // The Orb reboots into the theme, so there is nothing useful to wait for here.
    nm.textContent=t.name+' — switching, the Orb is restarting';
   }; row.appendChild(w);
   const x=document.createElement('button'); x.textContent='Delete'; x.onclick=async()=>{
    if(x.textContent==='Delete'){ x.textContent='Really delete?'; return; }
    x.disabled=true;
    const rr=await fetch('/themedel?slug='+encodeURIComponent(t.slug),{method:'POST'});
    if(rr.ok) refresh(); else { x.textContent=await rr.text(); }
   }; row.appendChild(x);
  }
  box.appendChild(row);
 }
 $('#sub').textContent=d.themes.length+(d.themes.length===1?' theme':' themes')+' on the card. It cannot delete the one it is wearing — switch first.';
}
refresh();

$('#f').onchange=async e=>{
 const file=e.target.files[0]; if(!file) return; log.textContent='';
 try{
  const buf=new Uint8Array(await file.arrayBuffer());
  const v=new DataView(buf.buffer),dec=new TextDecoder();
  if(dec.decode(buf.subarray(0,8))!=='ORBTHM01') throw new Error('That is not an Orb theme file.');
  let at=8;
  const sl=v.getUint16(at,true);at+=2;
  const slug=dec.decode(buf.subarray(at,at+sl));at+=sl;
  const n=v.getUint16(at,true);at+=2;
  const items=[];
  for(let i=0;i<n;i++){
   const nl=v.getUint16(at,true);at+=2;
   const name=dec.decode(buf.subarray(at,at+nl));at+=nl;
   const dl=v.getUint32(at,true);at+=4;
   items.push({name,data:buf.subarray(at,at+dl)});at+=dl;
  }
  // _installed LAST, always. The Orb only counts a folder that has it, so a transfer that
  // dies halfway leaves an invisible folder rather than a half-broken theme in the picker.
  items.sort((a,b)=>(a.name==='_installed')-(b.name==='_installed'));
  say('Installing '+items.length+' files');
  for(let i=0;i<items.length;i++){
   const it=items[i];
   const fd=new FormData();
   fd.append('f',new Blob([it.data]),it.name);
   const r=await fetch('/sdput?path=/themes/'+encodeURIComponent(slug)+'/'+encodeURIComponent(it.name),{method:'POST',body:fd});
   if(!r.ok) throw new Error(it.name+' failed to write ('+r.status+')');
   say((i+1)+'/'+items.length+'  '+it.name);
  }
  say('Done.','ok'); refresh();
 }catch(err){ say(String(err.message||err),'bad'); }
};
</script>)HTML";

static void handleSdPutUpload() {
    HTTPUpload &up = g_web.upload();
    if (up.status == UPLOAD_FILE_START) {
        g_sdUpOk = false;
        g_sdUpPath = g_web.arg("path");
        if (!sdcard::mounted()) { Serial.println("[sdput] no card mounted"); return; }
        if (!sd_put_path_ok(g_sdUpPath)) { Serial.printf("[sdput] rejected path '%s'\n", g_sdUpPath.c_str()); return; }
        // Create every missing level, not just the last one. SD.mkdir() does not create
        // intermediate directories, so on a card that has never held a theme the whole
        // path fails at "/themes" and every upload dies with a bare open failure.
        for (int i = 1; i < (int)g_sdUpPath.length(); ++i) {
            if (g_sdUpPath[i] != '/') continue;
            String part = g_sdUpPath.substring(0, i);
            if (!SD.exists(part) && !SD.mkdir(part)) {
                Serial.printf("[sdput] mkdir failed: %s\n", part.c_str());
                return;
            }
        }
        g_sdUpFile = SD.open(g_sdUpPath.c_str(), FILE_WRITE);   // truncates any existing file
        if (!g_sdUpFile) {
            Serial.printf("[sdput] open failed: %s (card %llu MB, writable?)\n",
                          g_sdUpPath.c_str(), (unsigned long long)(sdcard::sizeBytes() / (1024ULL * 1024ULL)));
            return;
        }
        g_sdUpOk = true;
        Serial.printf("[sdput] start %s\n", g_sdUpPath.c_str());
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (g_sdUpOk && g_sdUpFile.write(up.buf, up.currentSize) != up.currentSize) {
            g_sdUpOk = false;
            Serial.println("[sdput] short write (card full or removed?)");
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (g_sdUpFile) g_sdUpFile.close();
        if (g_sdUpOk) {
            Serial.printf("[sdput] done %s (%u bytes)\n", g_sdUpPath.c_str(), (unsigned)up.totalSize);
            // Tell the user the device is mid-update. Without this, files arrived in
            // silence and the reboot that follows read as a crash or a stale load.
            static int s_updateFiles = 0;
            const int slash = g_sdUpPath.lastIndexOf('/');
            update_ui::file_received(g_sdUpPath.c_str() + (slash >= 0 ? slash + 1 : 0), ++s_updateFiles);
        }
    }
}

static void handleSdPutDone() {
    g_web.send(g_sdUpOk ? 200 : 500, "text/plain", g_sdUpOk ? "ok" : "failed");
}


// PSRAM ledger. Free memory went from 592 KB (before disabling two unused apps) to
// 105 KB (after), which means something else absorbed the ~3.5 MB that was freed and
// then some. Printing the balance at each stage of boot shows WHERE it goes, instead of
// inferring it from whatever fails to allocate first. Cheap: a handful of prints, once.
static void psram_mark(const char *stage) {
    static uint32_t s_prev = 0;
    const uint32_t now = (uint32_t)ESP.getFreePsram();
    const int32_t delta = s_prev ? (int32_t)((now - s_prev) / 1024) : 0;
    Serial.printf("[psram] %-26s free %6u KB", stage, (unsigned)(now / 1024));
    if (s_prev) Serial.printf("   (%+ld KB)", (long)delta);
    Serial.println();
    s_prev = now;
}


// Rendered frames per second, sampled between /health calls. The sweep advances a fixed
// step per timer tick rather than by elapsed time, so it visibly runs slow whenever the
// render loop cannot keep its 30 ms cadence — Zion spotted exactly that by comparing the
// device against Launch Kit's preview. This turns that observation into a number.
// Milliseconds of CPU spent per wall-clock second, sampled between /health calls.
// lvgl_ms_per_s near 1000 means the render loop is saturated; flush_ms_per_s says how
// much of that was the QSPI push rather than compositing.
static unsigned host_lvgl_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t us = display_lvgl_us(), now = millis();
    unsigned v = 0;
    if (lastMs && now > lastMs) v = (unsigned)(((us - last) / 1000UL) * 1000UL / (now - lastMs));
    last = us; lastMs = now;
    return v;
}
static unsigned host_flush_load() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t us = display_flush_us(), now = millis();
    unsigned v = 0;
    if (lastMs && now > lastMs) v = (unsigned)(((us - last) / 1000UL) * 1000UL / (now - lastMs));
    last = us; lastMs = now;
    return v;
}

// Full screens' worth of pixels repainted per second (466x466 = 1 screen). A value near
// the frame rate means every frame repaints essentially the whole display; a value far
// below it means LVGL really is honouring small dirty rectangles. This distinguishes
// "too much area" from "too many layers" — halving the sweep's redraw rate moved the
// frame cost by 0.2%, so one of those two assumptions is wrong.
static unsigned host_screens_per_s() {
    static uint32_t last = 0, lastMs = 0;
    const uint32_t px = display_flushed_px(), now = millis();
    unsigned v = 0;
    const uint32_t perScreen = (uint32_t)SCREEN_W * SCREEN_H;
    if (lastMs && now > lastMs) v = (unsigned)(((px - last) * 100UL / perScreen) * 1000UL / (now - lastMs));
    last = px; lastMs = now;
    return v;   // hundredths of a screen per second
}

static unsigned host_fps() {
    static uint32_t lastFrames = 0, lastMs = 0;
    const uint32_t frames = display_frames();
    const uint32_t now = millis();
    unsigned fps = 0;
    if (lastMs && now > lastMs) fps = (unsigned)((frames - lastFrames) * 1000UL / (now - lastMs));
    lastFrames = frames; lastMs = now;
    return fps;
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
    Serial.println("\nCapsule Radar boot");
    orb_link::begin();
    orb_link::setThemeRequestHook(request_theme_switch);
    diag::boot();   // print + continue the RTC-memory event history across this reboot

    // RTC_NOINIT holds whatever was in it, including rubbish after a real power cycle, so it
    // is only trusted when the companion magic says we wrote it. Same guard diag_log uses.
    if (g_feedRebootsMagic != FEED_REBOOT_MAGIC) { g_feedRebootsMagic = FEED_REBOOT_MAGIC; g_feedReboots = 0; }

    // Send large allocations (>=4KB) to PSRAM instead of the ~300KB internal heap.
    // TLS handshakes (WiFiClientSecure, fresh one built for every poll of every feed —
    // ADS-B every 2s, weather, wx radar, cloud imagery, aircraft photos) were the
    // biggest thing routinely landing on the internal heap, and repeatedly allocating
    // and freeing those over a long uptime fragmented it badly enough that eventually
    // no single free block was big enough for the next handshake, even with plenty of
    // total free memory — mbedTLS calls it "SSL - Memory allocation failed", and it was
    // tripping the "feed stuck 180s -> reboot" recovery every few minutes. Internal
    // memory allocations don't get restructured at all; they're just redirected to the
    // ~8MB PSRAM pool, which has vastly more room to absorb the same churn.
    heap_caps_malloc_extmem_enable(4096);

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
    // The bake, now that there is a screen to narrate it on. Only does real work on the
    // first boot after a theme push; the progress callback puts "Installing update…
    // preparing artwork k of n" on the panel while it grinds, which is the second-restart
    // leg of an update the user was previously left to guess about.
    theme_art::set_progress([](const char *name, int done, int total) {
        if (done == 0 && !name) update_ui::bake_begin(total);
        else update_ui::bake_progress(name, done, total);
    });
    if (theme_art::bake_active_theme()) update_ui::bake_done();
    // After the bake and after lv_init(): the font loader reads the freshly baked fonts,
    // and every view built below gets the theme's typography on its first label.
    theme_font::begin();
    build_hold_warning();   // hold-to-reboot countdown, sits above every app

    // restore the saved theme, then persist any future change
    {
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
        g_hideGround = p.getBool("hideground", false);
        g_minAltFt = p.getInt("minalt", 0);
#if CUSTOM_HAS_RADAR_HIDEGROUND
        g_hideGround = (bool)CUSTOM_RADAR_HIDEGROUND;   // a pushed design's own Scope settings, same one-shot-per-boot precedent as range/max-aircraft
#endif
#if CUSTOM_HAS_RADAR_MINALT
        g_minAltFt = CUSTOM_RADAR_MINALT;
#endif
#if CUSTOM_HAS_RADAR_DEADZONE
        g_deadZonePx = CUSTOM_RADAR_DEADZONE_PX;   // pushed-design only: there's no device-side control for it, since it's sized against the design's own center artwork
#endif
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
    // App lineup. Every app is always registered (so indices and selectApp(n) never
    // shift), but the ones a Launch Kit theme flash turns off (custom_apps.h) are
    // marked hidden — still built, just skipped when the knob cycles the menu. The
    // default custom_apps.h has all apps on, so a stock build / single-screen push
    // is unchanged.
    lv_obj_t *radarScreen = lv_scr_act();
    psram_mark("after display+radar");
    clockview::init();
    psram_mark("after clockview");
    app_shell::add(clockview::screen(), theme_style::names().clock, clockview::onPress, nullptr, false, nullptr, clockview::onExit, !theme_style::apps().clock);  // push flips analog/digital; onExit frees a custom face's decoded PSRAM
    app_shell::add(radarScreen, theme_style::names().flight, radar_press_custom_or_theme, radar_turn_select, false, radar_show_home_custom, radar_exit_release_style, !theme_style::apps().flight);
    app_shell::add(radarScreen, theme_style::names().weather,  weather_press_cycle, nullptr, false, radar_show_weather, nullptr, !theme_style::apps().weather);
    locationview::init();
    psram_mark("after locationview");
    app_shell::add(locationview::screen(), theme_style::names().intel,
                   locationview::onPress, nullptr, false, locationview::onEnter, nullptr, !theme_style::apps().intel);  // push refreshes
    spycamview::init();
    psram_mark("after spycamview");
    app_shell::add(spycamview::screen(), theme_style::names().surveillance, spycamview::onPress, nullptr, false, nullptr, nullptr, !theme_style::apps().surveillance);  // push cycles cams; clip loads lazily on commit
    settingsview::init();
    psram_mark("after settingsview");
    app_shell::add(settingsview::screen(), theme_style::names().settings,
                   settingsview::onPress, settingsview::onTurn,
                   true, settingsview::onEnter, settingsview::onExit, false);  // captures the knob on entry; onEnter resets to the menu and takes the text canvas, onExit gives it back
    app_shell::begin();                // start on the clock (index 0 — see comment above)
    psram_mark("after app_shell::begin");

    // Fresh out of the box, or right after Settings > Reset: skip the clock and walk
    // straight into WiFi setup instead — see host_factory_reset(). Index 5 is still
    // Settings: Clock was inserted at the front, Intel/Surveillance/Settings didn't move.
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
            app_shell::selectApp(5);
            app_shell::setCaptured(true);
            settingsview::openAboutPage();
        }
#elif CUSTOM_BOOT_TARGET == 2
        // Set only by a Flight Tracker push — boots straight into it instead of
        // landing on the clock and making you swipe/switch over, since a radar
        // push is almost always "I just changed this one screen, go look at it."
        // selectApp(1) runs Flight Tracker's onEnter, which captures the knob for
        // aircraft selection when a custom design is active (see
        // radar_show_home_custom) — left captured on purpose: turning the knob
        // should select an aircraft immediately, not open the switcher, since
        // that's the whole point of landing here. Press the knob to leave
        // selection mode and reach the switcher/menu (Settings -> WiFi etc.) —
        // same gesture Settings itself already uses to back out.
        else {
            app_shell::selectApp(1);
        }
#endif
    }

    imu_begin();       // face-down sleep (no-op if the IMU isn't detected)
    battery_begin();   // AXP2101 (no-op if not detected / no battery)
    gps_begin();       // LC76G GNSS (no-op if not the -G variant)
    battery_enable_codec_rail();   // power the ES8311 analog rail before audio init

    setenv("TZ", g_tz.c_str(), 1); tzset();   // local time for display even before NTP (loadSettings ran above)
    rtc_begin();
    rtc_seed_clock();                   // offline clock/date from the PCF85063
    if (audio_begin()) {                // ES8311 alert pings (no-op if codec absent)
        audio_set_volume(g_volume);
        audio_set_muted(g_muted);
    }

    // --- Radar UI ----------------------------------------------------------
    // radar::init() runs inside display::begin() (LVGL must be up first).

    // Let the boot splash actually hold-then-fade in real time before g_wm.autoConnect()
    // below blocks for potentially 10-20+ seconds trying the saved WiFi network. LVGL
    // timers and animations only advance while lv_timer_handler() runs, which does NOT
    // happen during blocking setup() code — without this pump, the splash's 2s/600ms
    // timer+fade (ui.cpp) just sit frozen for however long WiFi takes, then both fire
    // back-to-back the instant loop() finally starts. Clock is already app index 0 and
    // already loaded (app_shell::begin() above), so this is what actually reveals a
    // live, correct-time clock quickly instead of a stuck title card. rtc_seed_clock()
    // has already run, so the time it shows is correct from the very first frame.
    {
        const uint32_t pumpUntil = millis() + 2700;   // 2000ms hold + 600ms fade + margin
        while (millis() < pumpUntil) {
            lv_timer_handler();
            delay(5);
        }
    }

    // --- WiFi (captive portal, non-blocking) ------------------------------
    // First boot opens the "The Orb Setup" AP to enter WiFi creds. Non-blocking
    // so the radar keeps animating while you configure WiFi from your phone.
    g_wm.setConfigPortalBlocking(false);
    g_wm.setTitle("Capsule Radar");
    // light phosphor-green theme for the captive portal (small CSS, injected into <head>)
    g_wm.setCustomHeadElement(
        "<style>"
        "body{background:#06100a;color:#cdd6d1;font-family:system-ui,sans-serif}"
        "h1,h2,h3{color:#1dff86}"
        "button,input[type=submit],.btn{background:#1dff86!important;color:#04140b!important;"
        "border:0!important;border-radius:8px!important;font-weight:700}"
        "input,select{background:#0c1a12!important;color:#eafff3!important;"
        "border:1px solid #2a4a39!important;border-radius:8px!important}"
        "a{color:#1dff86}.q{filter:hue-rotate(90deg)}"
        "</style>");
    // After the portal saves new credentials, reboot for a clean start: WiFiManager's
    // own port-80 server (and mDNS) don't cleanly hand over to our web server / STA
    // interface in non-blocking mode, so the config page is flaky until a fresh boot.
    g_wm.setSaveConfigCallback([]() {
        Serial.println("[wifi] new credentials saved -> rebooting for a clean web/mDNS start");
        g_rebootAtMs = millis() + 2500;   // let the portal deliver its 'saved' page first
    });
    // DISABLED on purpose (2026-08-15). A secrets.h fallback used to run here and it was
    // destructive: WiFi.SSID() is empty this early because the WiFi driver has not loaded
    // the stored credentials yet, so the guard was always true, and the WiFi.begin() below
    // it overwrote the user's real saved network on EVERY boot. When the fallback
    // credentials then failed to associate, the device came up in the config portal with
    // its good credentials already gone.
    //
    // If this comes back, it must (a) determine "no stored network" properly, which means
    // after WiFi.mode(WIFI_STA) or via WiFiManager's own getWiFiSSID(), and (b) never call
    // WiFi.begin() with fallback credentials before autoConnect() has had its turn.
    psram_mark("before wifi connect");
    const bool wifiUp = g_wm.autoConnect("The Orb Setup");
    if (wifiUp) Serial.println("[wifi] connected");
    else        Serial.println("[wifi] config portal open - join 'The Orb Setup' to set WiFi; UI stays live");

    // No network means the first thing anyone should see is how to give it one, not a
    // clock that cannot tell the time. Both routes are open from here: the knob walks
    // through scan/pick/password on the screen itself, and the same moment the portal is
    // up on "The Orb Setup" for anyone who would rather type on a phone.
    if (!wifiUp) wantWifiSetup = true;
    if (wantWifiSetup) {
        app_shell::selectApp(5);        // Settings
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
    if (theme_style::apps().weather) wx_radar_begin();
    else Serial.println("[wxradar] app disabled by theme - skipping ~1 MB of frame buffers");
    // cloud_image_begin() intentionally not called: the satellite-cloud view was dropped
    // from the Weather app's knob cycle, so its two full-frame PSRAM buffers (~0.5MB) would
    // just sit unused. That memory goes to the Weather Radar animation frames instead.
    g_ac_mutex = xSemaphoreCreateMutex();
    psram_mark("before adsb task");
    xTaskCreatePinnedToCore(adsb_task, "adsb", 8192, nullptr, 1, &g_adsbTaskHandle, 0);
    // Measured on the device 2026-08-22 (uxTaskGetStackHighWaterMark over a 5-minute soak,
    // the feed both succeeding and failing): peak usage was ~4.6 KB of the 16 KB this held.
    // The old comment here said "TLS needs a big stack"; that stopped being true when the
    // TLS fallback was disabled above, and even the plain-HTTP path never came close to
    // justifying the size. 8 KB keeps roughly double the observed peak as margin and returns
    // 8 KB of internal RAM, which is the exact resource the feed is starved for.

    // configuration web page (http://capsuleradar.local/)
    g_web.on("/diag", []{ g_web.send(200, "text/plain", diag::text()); });
    // Per-task and heap-fragmentation detail, added for the 2026-08-22 investigation into
    // why ADS-B reads start timing out a minute or two into Flight Tracker. /health already
    // gives one internal-heap number; this is who is holding the rest of it, and how broken
    // up what remains actually is (a device can report 8 KB free and still be unable to
    // satisfy a 2 KB request if that 8 KB is forty scattered 200-byte crumbs).
    g_web.on("/taskmem", []{
        multi_heap_info_t hi;
        heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
        char b[640];
        snprintf(b, sizeof(b),
            "{\"internal\":{\"free_bytes\":%u,\"largest_free_block\":%u,"
            "\"free_blocks\":%u,\"allocated_blocks\":%u,\"total_blocks\":%u},"
            "\"stack_free_bytes\":{\"adsb\":%u,\"audio\":%u,\"loop\":%u},"
            "\"radar_active\":%s}",
            (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
            (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
            (unsigned)(hi.free_blocks + hi.allocated_blocks),
            (unsigned)(g_adsbTaskHandle ? uxTaskGetStackHighWaterMark(g_adsbTaskHandle) : 0),
            (unsigned)audio_stack_free_bytes(),
            (unsigned)uxTaskGetStackHighWaterMark(nullptr),   // nullptr = the calling (loop) task
            g_radarViewActive ? "true" : "false");
        g_web.send(200, "application/json", b);
    });
    // Reboot on request. Added because capturing the boot log (the only place the PSRAM
    // ledger prints) otherwise means physically unplugging the device, and the serial
    // port cannot be held open during a flash anyway. Deliberately delayed so the HTTP
    // response reaches the caller first, same pattern as the settings handlers above.
    g_web.on("/reboot", []{
        g_web.send(200, "text/plain", "rebooting");
        update_ui::rebooting();     // no-op unless the update overlay is up
        g_rebootAtMs = millis() + 400;
    });
    // Switch the active theme, e.g. /theme?slug=modern. Until this existed, pushing a
    // theme from Launch Kit copied its files onto the card but left the device running
    // whichever theme it was already on, so "push Modern to the Orb" produced the old
    // theme's artwork wearing the new theme's compiled fonts. Settings > Design was the
    // only way to actually select one, which is a menu dive for something the push
    // already knows. theme_select::set() persists to NVS and reboots by itself.
    g_web.on("/theme", []{
        const String slug = g_web.arg("slug");
        if (!slug.length()) { g_web.send(400, "text/plain", "missing slug"); return; }
        // Only switch to a theme that is genuinely on the card; a typo would otherwise
        // leave the device pointing at an empty folder and drawing stock art.
        static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
        const int n = theme_select::listInstalled(slugs);
        bool found = false;
        for (int i = 0; i < n && !found; ++i) found = (slug == slugs[i]);
        if (!found) { g_web.send(404, "text/plain", "no such theme on the card"); return; }

        g_web.send(200, "text/plain", "switching");
        Serial.printf("[theme] switching to '%s' by request\n", slug.c_str());
        // Applied from loop(), not here: theme_select::set() reboots, which would cut the
        // HTTP response off before the client ever saw it.
        g_pendingSlug   = slug;
        g_applySlugAtMs = millis() + 400;
    });
    // Diagnostic: hide/show one radar layer live, so each layer's per-frame cost can be
    // priced by watching fps change instead of reflashing once per hypothesis. Nothing
    // persists it; a reboot or re-entering the app puts every layer back.
    g_web.on("/rdbg", []{
        if (g_web.hasArg("poll")) {
            g_pollOverrideMs = (uint32_t)g_web.arg("poll").toInt();
            Serial.printf("[adsb] poll interval override -> %lu ms\n", (unsigned long)g_pollOverrideMs);
            g_web.send(200, "text/plain", "poll set");
            return;
        }
        const int  kind = g_web.arg("layer").toInt();
        const bool hide = (g_web.arg("hide") != "0");
        radar::debugHideLayer(kind, hide);
        g_web.send(200, "text/plain", hide ? "hidden" : "shown");
    });
    // Machine-readable device state, so diagnosis starts from facts instead of from a
    // photograph of the screen (workflow rule R2). largest_block matters as much as
    // free: an allocation can fail with plenty of total free PSRAM if churn has
    // fragmented it below the requested size.
    g_web.on("/health", []{
        char b[420];
        snprintf(b, sizeof(b),
                 // slug is the permanent folder id, theme is the display name. Reporting
                 // only the slug is what made "Modern" and "the-office" look unrelated.
                 // `assets` is the fingerprint of the theme data this device is actually
                 // running. Launch Kit compares it with the staged theme to decide whether
                 // anything needs sending at all, which is what turns a no-op push into a
                 // few seconds instead of shipping twenty unchanged files.
                 "{\"fw\":\"%s\",\"slug\":\"%s\",\"theme\":\"%s\",\"weld\":%lu,\"assets\":%lu,\"uptime_s\":%lu,"
                 "\"psram_free_kb\":%u,\"psram_largest_kb\":%u,"
                 "\"heap_free_kb\":%u,\"heap_largest_kb\":%u,"
                 "\"fps\":%u,\"lvgl_ms_per_s\":%u,\"flush_ms_per_s\":%u,"
                 "\"screens_per_s\":%u,"
                 "\"wifi_rssi\":%d,\"boot_reason\":\"%s\"}",
                 FW_VERSION, theme_select::activeSlug(), theme_style::themeLabel(),
                 (unsigned long)CUSTOM_WELD_HASH,
                 (unsigned long)theme_style::assetsFingerprint(),
                 (unsigned long)(millis() / 1000UL),
                 (unsigned)(ESP.getFreePsram() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                 (unsigned)(ESP.getFreeHeap() / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                 host_fps(), host_lvgl_load(), host_flush_load(), host_screens_per_s(),
                 (int)WiFi.RSSI(),
                 esp_reset_reason() == ESP_RST_POWERON ? "power-on" :
                 esp_reset_reason() == ESP_RST_SW      ? "software" :
                 esp_reset_reason() == ESP_RST_TASK_WDT ? "watchdog" : "other");
        g_web.send(200, "application/json", b);
    });
    g_web.on("/sdput", HTTP_POST, handleSdPutDone, handleSdPutUpload);   // Launch Kit pushes theme files here
    // The page that drives /sdput from a browser, so a theme can arrive over WiFi from any
    // device on the network rather than only down a USB cable from a Chromium desktop.
    g_web.on("/install", []{ g_web.send_P(200, "text/html", INSTALL_PAGE); });
    // What is on the card, for the page above. The same answer the serial link gives, which
    // matters now that a cable is optional: Studio is served over HTTPS and can never call
    // this, so the device's own page is the only place the card's contents can be seen.
    g_web.on("/themes.json", []{
        static char slugs[theme_select::MAX_THEMES][theme_select::MAX_SLUG_LEN];
        const int n = theme_select::listInstalled(slugs);
        String out = "{\"active\":\"";
        out += theme_select::activeSlug();
        out += "\",\"themes\":[";
        for (int i = 0; i < n; ++i) {
            char label[64];
            theme_style::labelFor(slugs[i], label, sizeof(label));
            // Quotes are the only character that can appear in a theme name and break this;
            // a name is 40 characters of the user's own typing, not arbitrary bytes.
            String name(label);
            name.replace("\"", "'");
            if (i) out += ',';
            out += "{\"slug\":\""; out += slugs[i];
            out += "\",\"name\":\""; out += name; out += "\"}";
        }
        out += "]}";
        g_web.send(200, "application/json", out);
    });
    g_web.on("/themedel", HTTP_POST, []{
        const String slug = g_web.arg("slug");
        if (!slug.length()) { g_web.send(400, "text/plain", "missing slug"); return; }
        // removeInstalled refuses the theme being worn: every screen is drawing from that
        // folder right now. Switch first, then delete.
        if (!theme_select::removeInstalled(slug.c_str())) {
            g_web.send(409, "text/plain", "cannot delete the theme the Orb is wearing");
            return;
        }
        Serial.printf("[theme] deleted '%s' by request\n", slug.c_str());
        g_web.send(200, "text/plain", "ok");
    });
    g_web.on("/", handleRoot);
    g_web.on("/save", HTTP_POST, handleSave);
    g_web.on("/wifi", HTTP_POST, handleWifi);
    g_web.on("/bright", handleBright);
    g_web.on("/vol", handleVol);
    g_web.on("/alerts", handleAlerts);
    g_web.on("/idle", handleIdle);
    g_web.on("/sweep", handleSweep);
    g_web.on("/airports", handleAirports);
    g_web.on("/ground", handleGround);
    g_web.on("/altmin", handleAltMin);
    g_web.on("/milonly", handleMilOnly);
    g_web.on("/trail", handleTrail);
    g_web.on("/maxac", handleMaxAc);
    g_web.on("/bigtext", handleBigText);
    g_web.on("/rotate", handleRotate);
    g_web.on("/gps", handleGps);
    g_web.on("/units", handleUnits);
#if ORB_OTA_ENABLED
    g_web.on("/update", HTTP_GET, handleUpdatePage);
    g_web.on("/update", HTTP_POST,
        []() {
            const bool ok = !Update.hasError();
            g_web.send(200, "text/plain", ok ? "OK" : "FAIL");
            delay(800);
            if (ok) ESP.restart();
        },
        handleUpdateUpload);
#endif
    g_web.begin();

    Serial.println("setup done");
}

void loop() {
    // INPUT FIRST. The encoder is interrupt-driven, so no detent is ever lost — but this
    // used to run at the BOTTOM of the loop, after a full LVGL render and after the web
    // server. A detent arriving while the screen was drawing therefore waited for that
    // draw to finish, then the network work, then a SECOND full draw before anything
    // moved: two frames of latency on every turn, which is what made the knob feel
    // sluggish. Handling it first means a turn is acted on by the very next render.
    knob::poll();                   // drain detents/presses accumulated by the ISR
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
        if (kd != 0 || pressed) display::noteActivity();    // knob use keeps the screen awake
        if (pressed) diag::log("push (app %s, browsing=%d, captured=%d)",
                               app_shell::name(), app_shell::browsing(), app_shell::captured());
        input_router::dispatch((int)kd, pressed);           // same 3-mode routing the sim uses
    }

    display::loop();                // drive LVGL (render dirty areas + run timers)

    // Network and sensors last: they are throughput work, not interactive. handleClient()
    // in particular can spend real time on an /sdput chunk, and nothing about it should
    // sit between a knob turn and the frame that answers it.
    g_wm.process();                 // service the WiFi config portal (non-blocking)
    g_web.handleClient();           // serve the configuration web page
    orb_link::poll();               // answer Orb Studio over the USB cable (bounded, non-blocking)
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
    if (g_useGps) gps_poll();       // pull NMEA from the LC76G (only when GPS auto-location is on)

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

    // mDNS (and OTA, when it is compiled in): set up once WiFi is up.
    // ArduinoOTA::setHostname() used to be what registered capsuleradar.local, because it
    // calls MDNS.begin() internally. Compiling OTA out therefore took the device's whole
    // .local name with it and broke Launch Kit's pushes, which address it by name.
    // MDNS.begin() is now called here explicitly and does not depend on OTA at all.
    static bool mdnsUp = false;
    if (!mdnsUp && WiFi.status() == WL_CONNECTED) {
        if (!MDNS.begin("capsuleradar")) Serial.println("[mdns] begin failed");
        MDNS.addService("http", "tcp", 80);            // advertise the config web page
#if ORB_OTA_ENABLED
        ArduinoOTA.setHostname("capsuleradar");
        ArduinoOTA.begin();
        Serial.println("[ota] ready: pio run -e esp32-s3-amoled-175-ota -t upload");
#endif
        mdnsUp = true;
        Serial.println("[mdns] http://capsuleradar.local/");
    }
#if ORB_OTA_ENABLED
    if (mdnsUp) ArduinoOTA.handle();
#endif

    // Push a fresh ADS-B snapshot to the radar (copy under the mutex, render outside).
    if (g_acDirty) {
        if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
            g_snap.swap(g_aircraft);   // O(1) handoff under the lock; render on g_snap outside it.
            g_acDirty = false;         // g_aircraft now holds the previous snapshot (overwritten next poll)
            xSemaphoreGive(g_ac_mutex);
            // Timed because this is the one chunk of per-poll work that lands on the
            // RENDER thread: everything else about a poll happens on core 0. If a poll
            // costs a visible stutter, this is where it is spent.
            const uint32_t upd0 = micros();
            radar::update(g_snap, g_settings); // rebuild the glyph/trail layer
            const uint32_t upd1 = micros();
            ui_on_data_updated();              // refresh card/list/stats
            const uint32_t upd2 = micros();
            checkAudioEvents();                // ping new-in-range / emergency / military
            const uint32_t upd3 = micros();
            // Only when it actually costs a frame. This ran on every poll while the
            // sweep stutter was being hunted, which is the right instrument but the
            // wrong volume once it is fixed: a healthy snapshot is ~25 ms and saying so
            // twice a second buries everything else on the console. 40 ms is half a
            // frame at the radar's ~13 fps, so anything printed here is a real regression.
            const uint32_t total = upd3 - upd0;
            if (total > 40000UL) {
                Serial.printf("[perf] SLOW snapshot %u ac: update %lu us, ui %lu us, audio %lu us, total %lu us\n",
                              (unsigned)g_snap.size(),
                              (unsigned long)(upd1 - upd0), (unsigned long)(upd2 - upd1),
                              (unsigned long)(upd3 - upd2), (unsigned long)total);
            }
        }
    }
    if (g_weatherDirty) {
        g_weatherDirty = false;
        ui_on_data_updated();
    }
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
        static int diagTick = 0;
        if (++diagTick >= 3) {   // every ~15s — a heap trend without flooding the ring buffer
            diagTick = 0;
            diag::log("heap %u min %u app %s", (unsigned)ESP.getFreeHeap(),
                      (unsigned)ESP.getMinFreeHeap(), app_shell::name());
            // Direct to Serial, not the diag ring buffer: this is instrumentation for the
            // 2026-08-22 memory investigation, wanted live over the cable rather than
            // competing for room in the compact RTC history. Fragmentation is the thing
            // "heap %u" above cannot show — a device can report 8 KB free and still be
            // unable to satisfy a 2 KB request if that 8 KB is forty scattered crumbs, which
            // is exactly the gap between free_bytes and largest_free_block below.
            {
                multi_heap_info_t hi;
                heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
                // Frames since the previous [memdbg], i.e. a real fps over a known 15 s
                // window. host_fps() cannot serve this: it measures frames since ITS last
                // call and is only called by /health, so a single request always reads 0 —
                // which is exactly why this looked like a dead renderer earlier today.
                static uint32_t s_dbgFrames = 0, s_dbgMs = 0;
                const uint32_t nowFr = display_frames(), nowMsFr = millis();
                unsigned realFps = 0;
                if (s_dbgMs && nowMsFr > s_dbgMs)
                    realFps = (unsigned)((nowFr - s_dbgFrames) * 1000UL / (nowMsFr - s_dbgMs));
                s_dbgFrames = nowFr; s_dbgMs = nowMsFr;
                Serial.printf("[memdbg] internal free=%u largest=%u blocks(free/alloc)=%u/%u "
                              "stacks(adsb/audio/loop)=%u/%u/%u radar=%d fps=%u\n",
                              (unsigned)hi.total_free_bytes, (unsigned)hi.largest_free_block,
                              (unsigned)hi.free_blocks, (unsigned)hi.allocated_blocks,
                              (unsigned)(g_adsbTaskHandle ? uxTaskGetStackHighWaterMark(g_adsbTaskHandle) : 0),
                              (unsigned)audio_stack_free_bytes(),
                              (unsigned)uxTaskGetStackHighWaterMark(nullptr),
                              (int)g_radarViewActive, realFps);
            }
        }
#if DEBUG_MEM
        static uint32_t lastFrames = 0;
        const uint32_t fr = display_frames();
        const unsigned fps = (fr - lastFrames) / 5;
        lastFrames = fr;
        Serial.printf("[mem] heap %u (min %u, biggest %u) | psram %u free | up %lus | aircraft %d | fps %u\n",
                      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram(), (unsigned long)(millis() / 1000),
                      (int)g_snap.size(), fps);
#endif
        char clk[8] = "--:--";
        struct tm ti;
        const bool haveTime = getLocalTime(&ti, 0);
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
                if (g_soundChime && audio_present()) audio_play(AUDIO_CHIME);
            }
        }
        const bool wifiUp = (WiFi.status() == WL_CONNECTED);
        const int  rssi   = wifiUp ? (int)WiFi.RSSI() : -127;
        // "fresh" = we got aircraft data recently. Catches a stalled feed (weak WiFi dropping
        // polls intermittently) that never trips the consecutive-fail counter -> aircraft
        // freeze but the icon would otherwise stay white.
        const bool feedFresh = wifiUp && (millis() - g_lastFeedOkMs < 18000UL);
        ui_set_status(wifiUp, feedFresh, rssi, clk);
        char net[112];
        if (WiFi.status() == WL_CONNECTED)
            // IP + the active centre point (helps users verify what actually got saved)
            snprintf(net, sizeof(net), "Configure at\ncapsuleradar.local\n%s  |  %.5f, %.5f",
                     WiFi.localIP().toString().c_str(), g_settings.homeLat, g_settings.homeLon);
        else
            snprintf(net, sizeof(net), "WiFi setup:\njoin \"The Orb Setup\"");
        settingsview::setNetInfo(net);   // shown on Settings > About (was the Stats screen)
        const bool bpresent = battery_present();
        ui_set_battery(battery_percent(), battery_charging(), bpresent);
        g_onBattery = bpresent && !battery_charging();
        // GPS HUD/Stats: 0 = off/no module (hidden), 1 = acquiring, 2 = fix
        const int gpsState = (!g_useGps || !gps_present()) ? 0 : (gps_has_fix() ? 2 : 1);
        ui_set_gps(gpsState, gps_satellites());
        // once NTP has a real fix, persist it to the RTC (core 1 only)
        if (!g_rtcSynced && time(nullptr) > 1700000000L) {
            time_t now = time(nullptr);
            struct tm utc;
            gmtime_r(&now, &utc);
            if (rtc_write(&utc)) { g_rtcSynced = true; Serial.println("[rtc] saved NTP time"); }
        }
        // GPS auto-location (-G variant): re-centre the radar when the fix moves enough.
        // Suppressed while a Launch Kit design pins the location — the pushed home is
        // the source of truth so the Orb keeps matching the editor/simulator exactly.
#if !CUSTOM_HAS_RADAR_HOME
        if (g_useGps) {
            double glat, glon;
            if (gps_location(&glat, &glon) &&
                geo::haversineKm(g_settings.homeLat, g_settings.homeLon, glat, glon) > 1.0) {
                g_settings.homeLat = glat; g_settings.homeLon = glon;   // radar/coastline recenter
                // re-query the new area — set the radius too (same formula as boot/zoom), or
                // adsb_task would re-begin with a stale/zero g_requeryKm and fetch 0 aircraft.
                g_requeryKm = queryRadiusKm();
                g_requery = true;                                       // adsb_task re-queries the new area
                Serial.printf("[gps] re-centred to %.4f, %.4f\n", glat, glon);
            }
        }
#endif
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

    delay(5);
}
