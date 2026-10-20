// The Orb OS — the networking task (core 0). Fetches and parses; never touches the display.
// Moved out of main.cpp unchanged. Started from setup() by xTaskCreatePinnedToCore.
#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "orb_state.h"
#include "orb_net_task.h"
#include "radar_view.h"
#include "weather.h"
#include "weather_client.h"
#include "wx_radar.h"
#include "wx_radar_client.h"
#include "route.h"
#include "route_client.h"
#include "photo.h"
#include "photo_client.h"
#include "intel_view.h"
#include "ticker.h"
#include "theme_style.h"
#include "diag_log.h"

// Upsert this poll's contacts into the persistent table, drop what has aged past
// AC_HARD_EXPIRE_MS, then hand the render side a flat snapshot of everything left.
//
// This is the whole fix for a contact vanishing the instant one poll misses it (routine
// ADS-B reception gaps, not a fault): the table only ever adds and refreshes entries here,
// it never wipes one because THIS poll happened not to mention it. An entry that goes
// unmentioned simply keeps its last known fix and its lastUpdateMs stops advancing, which
// is what lets radar_view.cpp read its age and dim it. Called under g_ac_mutex.
static void applyPolledAircraft(std::vector<Aircraft> &fresh, uint32_t nowMs) {
    for (Aircraft &ac : fresh) {
        ac.lastUpdateMs = nowMs;              // one clock for "when main.cpp last heard this",
        g_acTable[std::string(ac.hex.c_str())] = ac;   // independent of anything adsb_client stamped
    }
    for (auto it = g_acTable.begin(); it != g_acTable.end(); ) {
        it = (nowMs - it->second.lastUpdateMs > AC_HARD_EXPIRE_MS) ? g_acTable.erase(it) : std::next(it);
    }
    g_aircraft.clear();
    g_aircraft.reserve(g_acTable.size());
    for (auto &kv : g_acTable) g_aircraft.push_back(kv.second);
}

// ---- networking task (core 0): fetch + parse, never touches the display ----
void adsb_task(void*) {
    std::vector<Aircraft> fresh;
    bool wasConnected = false;
    uint32_t lastPoll = 0;
    uint32_t adsbBackoffMs = 0;                // extra delay after feed failures (exponential, 30s cap)
    uint32_t nextWeatherAt = UINT32_MAX;       // armed five seconds after WiFi connects
    uint32_t nextWxRadarAt = UINT32_MAX;
    int      wxFillIdx = WX_RADAR_FRAMES;      // which animation frame to fetch next (== FRAMES: idle)
    uint32_t wxGen = 0;                        // refresh generation, bumped each full loop
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
        // Whether the theme has a Flight Tracker at all. A theme that hides it entirely has
        // no reachable screen and no reason to fetch. Whether anyone is LOOKING at it is a
        // separate question, answered below by g_radarViewActive, and it decides how often
        // the feed is asked: full rate on screen, a slow keep-warm for a while after, then
        // nothing until the tracker is opened again.
        const bool radarActive = theme_style::apps().flight;
        if (radarActive && !wasRadarActive) {
            // First tick after boot (or after a theme switch turns Flight Tracker back
            // on): poll right away and give the stuck-feed watchdog a fresh 180s window
            // rather than one dated from setup().
            lastPoll = 0;
            lastFeedOk = millis();
        }
        wasRadarActive = radarActive;

        // WIFI DOES NOT COME BACK BY ITSELF. WiFiManager's autoConnect runs once, at boot,
        // and nothing here ever tried again: this loop watched `conn` go false and then just
        // carried on asking a network it no longer had. Measured on the device 2026-08-23 —
        // four hours of uptime showing "No WiFi" with the router two metres away and the
        // credentials still saved, and it reconnected instantly the moment it was rebooted.
        //
        // A desk instrument has to survive its router restarting overnight without a person
        // power-cycling it, so: log the drop (previously silent, which is why this hid for so
        // long), then retry on a slow cadence. 20 s is unhurried enough not to thrash the
        // radio and quick enough that a blip costs one missed poll rather than a whole night.
        if (!conn && wasConnected) {
            Serial.println("[wifi] connection LOST");
            diag::log("wifi lost");
        }
        if (!conn) {
            static uint32_t s_wifiRetryAt = 0;
            if (millis() - s_wifiRetryAt > 20000UL) {
                s_wifiRetryAt = millis();
                Serial.println("[wifi] disconnected — reconnecting");
                WiFi.reconnect();
            }
        }
        if (conn && !wasConnected) {
            // disable WiFi modem power-save: on a mains-powered desk gadget it just adds latency
            // and makes RSSI bounce (feed goes stale -> amber bars) even sitting next to the router.
            WiFi.setSleep(false);
            Serial.printf("[adsb] WiFi up, IP %s\n", WiFi.localIP().toString().c_str());
            diag::log("wifi up %s", WiFi.localIP().toString().c_str());
            configTzTime(g_tz.c_str(), "pool.ntp.org", "time.nist.gov");  // local time (web-configurable TZ)
            Serial.println("[web] config: http://" ORB_MDNS_ADDR "/  (or the IP above)");
            nextWeatherAt = millis() + 5000UL; // let the first ADS-B poll complete before weather TLS
            nextWxRadarAt = millis() + 12000UL;
            // mDNS + OTA are started on core 1 (loop) to keep all mDNS use on one core
        }
        wasConnected = conn;
        // A feed outage is reported, not "recovered" by restarting the device. This used to
        // reboot once per power cycle after 180 s of failed polls, on the theory that the
        // internal heap had fragmented under TLS handshakes. The feeds have been plain HTTP
        // since 2026-08-17, so that theory no longer describes anything, and what the reboot
        // actually did was reset an Orb sitting on the CLOCK because a free public feed had
        // a bad three minutes (CanadianAvenger, 2026-09-16: "just had another reset"). A
        // desk clock does not restart itself over somebody else's server. If the link
        // itself is the problem the network task's own WiFi retry above handles it; if the
        // feed is, the scope says so and the backoff keeps asking politely.
        if (!conn || !radarActive) lastFeedOk = millis();
        else if (!feedEverOk) lastFeedOk = millis();
        else if (millis() - lastFeedOk > 180000UL) {
            lastFeedOk = millis();
            radar::setFeedNote("No aircraft feed\nStill trying\nEverything else still works");
            Serial.println("[adsb] no good poll for 180 s with WiFi up; staying up and retrying");
            diag::log("feed stuck 180s; staying up (heap %u)", (unsigned)ESP.getFreeHeap());
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
            // ONLY WHILE SOMEBODY CAN SEE IT. The feed used to be polled every ten seconds
            // for as long as the theme had a Flight Tracker, clock or no clock, all night:
            // thousands of requests a day to a free public service for data nobody looked
            // at, which CanadianAvenger measured off the serial line and rightly called
            // unfriendly. Now: the tracker on screen polls at the full rate; for ten minutes
            // after it was last on screen (or after boot) it polls once a minute, so a quick
            // return finds a warm scope; after that it stops, and opening the tracker polls
            // at once. The "Loading aircraft" notice covers that first second or two.
            static uint32_t s_lastOnScreenMs = 0;
            static bool     s_wasOnScreen = false;
            const bool onScreen = g_radarViewActive;
            if (onScreen) s_lastOnScreenMs = nowMs;
            if (onScreen && !s_wasOnScreen) lastPoll = 0;      // just opened: ask now
            s_wasOnScreen = onScreen;
            const bool warm = onScreen || (nowMs - s_lastOnScreenMs) < 600000UL;
            const uint32_t baseInterval =
                g_pollOverrideMs ? g_pollOverrideMs
              : !onScreen        ? 60000UL
              : g_onBattery      ? POLL_INTERVAL_BATTERY_MS
                                 : POLL_INTERVAL_MS;
            const uint32_t pollInterval = baseInterval + adsbBackoffMs;
            // g_locationSet: no centre, no query. The coordinates are 0,0 until the network
            // lookup or Settings supplies a real one, and asking a free non-commercial API
            // every few seconds for the traffic over the Gulf of Guinea is a request nobody
            // wanted answered. The scope says "Location not set" meanwhile, so the silence
            // is explained rather than looking like a dead feed.
            if (radarActive && g_locationSet && warm && (lastPoll == 0 || nowMs - lastPoll >= pollInterval)) {  // aircraft feed, while it can be seen
                lastPoll = nowMs;
                static int failCount = 0;
                // poll() tries the fallback provider after a primary failure; keep the HUD
                // healthy through isolated misses and warn only after a sustained outage.
                // NO SYNTHESISED TRAFFIC. A real Orb shows the real sky, always.
                //
                // This used to fabricate eight aircraft whenever the active theme asked for
                // it, and the flag travelled INSIDE THE THEME FILE. That is the part that
                // made it indefensible rather than merely untidy: a theme is shared, so one
                // designer's debugging state became a stranger's device. Fly4Funn reported
                // his Orb full of SIM00x aircraft on 2026-09-27; he had never asked for
                // them, he had installed somebody else's theme. Panerai Punk was carrying
                // the flag in the library at the time.
                //
                // Zion, 2026-09-28: "on the real orb, it should always be real aircraft
                // traffic. There is never, ever a need for fake simulated air traffic."
                //
                // Orb Studio keeps the switch, because the reason for it is real: designing
                // a scope somewhere with no traffic overhead means nothing to design
                // against. But that is a question about a PREVIEW IN A BROWSER, and it is
                // answered there. Nothing about it reaches the card, and this device can no
                // longer invent an aircraft under any circumstances. A theme.json that still
                // carries the old flag is simply ignored, which is why the parse went too.
                if (g_adsb.poll(fresh)) {
                    Serial.printf("[adsb] fetched %u aircraft\n", (unsigned)fresh.size());
                    failCount = 0;
                    adsbBackoffMs = 0;                        // recovered: back to real-time polling
                    feedEverOk = true;                        // a restart now has a known-good state to return to
                    radar::setFeedNote(nullptr);              // back to the plain notice if it is somehow still up
                    g_feedOk = true;
                    const uint32_t receivedMs = millis();
                    lastFeedOk = receivedMs;
                    g_lastFeedOkMs = receivedMs;      // HUD: mark data as fresh

                    // Every poll upserts, empty or not: a poll that came back with nothing this
                    // cycle is not "the sky is empty," it is one sample, and the table already
                    // knows how to age a contact it did not hear about this time. See
                    // applyPolledAircraft() for the part that used to be a separate empty-poll
                    // grace timer — aging per contact made that timer's whole job redundant.
                    if (xSemaphoreTake(g_ac_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        applyPolledAircraft(fresh, receivedMs);
                        g_acDirty = true;
                        xSemaphoreGive(g_ac_mutex);
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
            // Opened: take the buffers and start a cycle NOW rather than waiting out the
            // five-minute refresh in front of somebody who just asked to see the weather.
            if (g_wxOpened) {
                g_wxOpened = false;
                wx_phase_set(WX_PHASE_BUFFERS);
#if !APPS_LAUNCH_ONE
                if (theme_style::apps().weather) wx_radar_begin();
#endif
                wxFillIdx = 0; ++wxGen; nextWxRadarAt = nowMs;
                wx_phase_set(WiFi.status() == WL_CONNECTED ? WX_PHASE_INDEX : WX_PHASE_NO_WIFI);
            }
            // Closed: give them back, but only from here, and only while idle. wxFillIdx ==
            // WX_RADAR_FRAMES means no frame is part-way through, which is the only moment
            // it is safe: this task is the one that writes into them.
            // The app has gone: abandon whatever is left of the current cycle rather than
            // finishing it. Each remaining frame is a PNG fetched and decoded for a screen
            // nobody is looking at, several seconds apiece, and until the cycle ends the
            // gate below cannot take the 1.2 MB back. Measured stuck at frame 1 of 5 with
            // the app closed, holding the lot.
            //
            // Safe HERE and nowhere else, for the same reason the release itself is: this is
            // the top of the network task's loop, so the fetch that was in flight has
            // already returned and no frame is part-way through being written.
            if (g_wxClosed && wxFillIdx < WX_RADAR_FRAMES) {
                Serial.printf("[wxradar] app closed at frame %d/%d; dropping the rest\n",
                              wxFillIdx, WX_RADAR_FRAMES);
                wxFillIdx = WX_RADAR_FRAMES;
            }
            if (g_wxClosed && wxFillIdx >= WX_RADAR_FRAMES) {
                g_wxClosed = false;
                wx_radar_release();
                // The road and coastline masks are NOT freed here. They are 16 KB each and
                // they are the one thing both threads can see, so keeping them makes "the
                // network task is mid-blit while the app closes" a non-question rather than
                // a window to be narrowed. The 1.2 MB of frame buffers is what matters.
                wx_phase_set(WX_PHASE_IDLE);
            }

            // A refresh must not start for an app nobody is looking at. Without the
            // !g_wxClosed here, the periodic timer kept arming a new cycle behind the closed
            // app: wxFillIdx went back to zero before the release gate above could ever
            // catch it idle, so 1.2 MB of frame buffers stayed held for the rest of the
            // session. The release was written to wait for a quiet moment and the refresh
            // guaranteed there was never one.
            //
            // Blocking the whole timer instead was worse and I tried it first: a cycle that
            // was part-way through when the app closed then never finished either, so
            // wxFillIdx never reached the value the gate waits for and the buffers were held
            // just the same. An in-flight fetch has to be allowed to complete, because the
            // task doing it is writing into those buffers and taking them away mid-write is
            // the hang this whole handshake exists to avoid.
            if (wxFillIdx >= WX_RADAR_FRAMES && (int32_t)(nowMs - nextWxRadarAt) >= 0
                && wx_radar_ready() && !g_wxClosed) {
                wxFillIdx = 0; ++wxGen;                // periodic refresh: start a new loop
            }
            if (wxFillIdx < WX_RADAR_FRAMES && (int32_t)(nowMs - nextWxRadarAt) >= 0) {
                const int r = wx_radar_fetch_frame(g_settings.homeLat, g_settings.homeLon,
                                                   g_wxZoomTier, wxGen, wxFillIdx);
                if (r == 1) {
                    g_wxRadarDirty = true;             // new frame committed
                    wxFillIdx++;
                    // Only READY once the whole loop is up. Saying so at the first frame
                    // would clear the notice over a map that still has four frames missing,
                    // and the animation would visibly stutter into life afterwards.
                    wx_phase_set(wxFillIdx >= WX_RADAR_FRAMES ? WX_PHASE_READY : WX_PHASE_FRAMES,
                                 wxFillIdx, WX_RADAR_FRAMES);
                    nextWxRadarAt = millis() + (wxFillIdx >= WX_RADAR_FRAMES ? WX_RADAR_REFRESH_MS : 250UL);
                } else if (r == 0) {                   // fewer frames than the loop length — cycle done
                    if (wxFillIdx > 0) g_wxRadarDirty = true;
                    wx_phase_set(wxFillIdx > 0 ? WX_PHASE_READY : WX_PHASE_FAILED, wxFillIdx, WX_RADAR_FRAMES);
                    wxFillIdx = WX_RADAR_FRAMES;
                    nextWxRadarAt = millis() + WX_RADAR_REFRESH_MS;
                } else {                               // error — retry the whole cycle in 60s
                    wx_phase_set(WiFi.status() == WL_CONNECTED ? WX_PHASE_FAILED : WX_PHASE_NO_WIFI,
                                 wxFillIdx, WX_RADAR_FRAMES);
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
            // Intel. One plain-HTTP request against a cached gateway response, well under
            // a kilobyte, so there is nothing to spread over several cycles. fetchStep() owns its own timing and returns immediately when
            // nothing is due, which is almost every pass through this loop.
            if (intelview::fetchStep()) g_intelDirty = true;
            // Quotes, through the same gateway and for the same reason. Well under a
            // kilobyte for a whole watchlist, so like the headlines there is nothing here
            // worth spreading over several passes; the step owns its own timing and returns
            // immediately when nothing is due, which is almost every pass.
#if !APPS_LAUNCH_ONE
            if (theme_style::apps().ticker && ticker_fetch_step()) g_tickerDirty = true;
#endif
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
