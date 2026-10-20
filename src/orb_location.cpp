// The Orb OS — where the Orb is: saving a position, finding one from the network (IP
// lookup) or a place name (geocoding), the recent-cities list, and the timezone that
// follows the position. Moved out of main.cpp unchanged.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "config.h"
#include "orb_state.h"
#include "orb_location.h"
#include "orb_apps.h"

// Remember what this place is called. Separate from persist_location() because a position
// can arrive without a name (?orb setloc with bare coordinates) and a name must never
// overwrite a good one with an empty string.
static void persist_location_name(const char *name) {
    if (!name || !name[0]) return;
    g_locName = name;
    Preferences p;
    p.begin("capsuleradar", false);
    p.putString("homeName", name);
    p.end();
}

// For the flight tracker's location line (theme_style Radar::locText) and anything else
// that wants to say where the scope is pointed. False when nothing has ever named it, so
// the caller can draw nothing rather than an empty plate.
bool host_location_name(char *out, size_t n) {
    if (!n) return false;
    snprintf(out, n, "%s", g_locName.c_str());
    return out[0] != 0;
}

// Write a location down. Split out of host_set_location() so the boot-time lookup can save
// a position WITHOUT the reboot below it: an Orb that restarted on its own because it
// worked out where it was would be the device reconfiguring itself, which UX-048 forbids.
static void persist_location(double lat, double lon) {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putDouble("homeLat", lat);
    p.putDouble("homeLon", lon);
    // The device now knows where it is, and will not ask the network again on later boots.
    // Nothing else distinguishes "never located" from "located, and it happens to be here",
    // which is how a failed lookup used to present as a confident wrong position.
    p.putBool("locSet", true);
    // Also clears the post-Reset "needs setup" flag (see host_factory_reset()) — the
    // auto-locate path (host_locate_current()) lands here directly on success, without
    // going through host_wifi_connected_reboot(), so that was the one path that left
    // the flag stuck set and kept forcing WiFi setup open on every boot after.
    p.putBool("needsWifiSetup", false);
    p.end();
    g_locationSet = true;
}

// Move the scope to a new centre while the device is running. This is the body of the GPS
// re-centre block that used to live in loop(): GPS is gone as a location input (UX-025,
// UX-026) but the live-apply it did was the useful half, and the boot-time lookup needs
// exactly it. Everything downstream of the centre re-reads g_settings, and the aircraft
// feed has to be told explicitly or adsb_task keeps querying the old circle.
static void apply_location_live(double lat, double lon) {
    g_settings.homeLat = lat;
    g_settings.homeLon = lon;
    // Set the radius too (same formula as boot/zoom), or adsb_task re-begins with a
    // stale/zero g_requeryKm and fetches 0 aircraft.
    g_requeryKm = queryRadiusKm();
    g_requery = true;
}

// Forget that a location was ever established, and nothing else. Debug channel only
// (`?orb locreset`, see orb_link.cpp).
//
// This exists because the only other way to reach the "no location" state is a factory
// reset plus a network with no route to the internet. That is a bad test harness: it takes
// minutes, it wipes WiFi and the theme selection which have nothing to do with the thing
// under test, and it depends on arranging a broken network on purpose — which already cost
// an evening when a phone hotspot turned out to have working data and the failure path
// never ran at all.
//
// "Location not set" is the promise this firmware makes to a stranger whose lookup failed,
// and a promise nobody can re-check in seconds is one that quietly rots the next time this
// code is touched.
//
// The coordinates are deliberately left in NVS. locSet is the flag every reader consults,
// so clearing it alone reproduces exactly what a failed first-boot lookup leaves behind,
// and leaving the numbers means Settings > Location > Recent can put things back in two
// clicks. g_locateTried is set so the once-per-boot retry does not immediately undo this;
// a reboot with WiFi up is the intended way out, along with Settings > Location.
void host_location_reset() {
    Preferences p;
    p.begin("capsuleradar", false);
    p.putBool("locSet", false);
    p.end();
    g_locationSet = false;
    g_locateTried = true;   // do not re-locate until the next boot
    Serial.println("[locate] locSet cleared by ?orb locreset — the scope should now read "
                   "\"Location not set\" and stop polling. Reboot with WiFi up, or use "
                   "Settings > Location, to restore it.");
}

// Set home location from the Settings menu and reboot to re-center radar + weather
// (mirrors the web /save handler, which also restarts). Reuses the hold-warning overlay
// (built in build_hold_warning()) for a visible countdown first — a silent 250ms-later
// reboot was jarring with no explanation of what was happening or why.
//
// The reboot is fine HERE and only here: every caller is a person who just picked a place,
// and UX-049 wants a step that takes the device out of service to say so, which the
// countdown does. The boot-time lookup uses persist_location() + apply_location_live()
// instead, because nobody asked it for anything.
void host_set_location(double lat, double lon) {
    persist_location(lat, lon);

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

// Ask the network where it is. ONE copy of the lookup, shared by the two callers below,
// because they differ only in what they do afterwards: the Settings item reboots to apply,
// and the boot-time retry must not. Fills lat/lon and records the city in recents and the
// timezone in NVS — both of those belong to the lookup rather than to either caller, since
// they are true the moment the answer arrives however it gets applied.
//
// Returns false on any failure, having changed nothing but possibly the timezone.
static bool ip_lookup_location(double &lat, double &lon) {
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
    const double la = doc["lat"] | 1000.0;
    const double lo = doc["lon"] | 1000.0;
    if (!(la >= -90 && la <= 90 && lo >= -180 && lo <= 180)) return false;

    const char *city   = doc["city"]   | "";
    const char *region = doc["region"] | "";
    if (city[0]) {
        char nm[40];
        snprintf(nm, sizeof(nm), "%s%s%s", city, region[0] ? ", " : "", region);
        host_recents_add(nm, la, lo);            // remember where we landed
        persist_location_name(nm);               // and what it is called
    }
    // Derive + persist the timezone, so the clock reads local wherever this landed.
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
    lat = la; lon = lo;
    return true;
}

// A position from Orb Studio, over the cable. UX-025 says the Orb has two location inputs,
// the network it joins and a place the owner picks; this is the second one, picked on the
// computer that is already plugged in rather than typed a letter at a time on the dial.
//
// It exists because IP geolocation is only as good as the ISP. It put one builder fifty
// kilometres away in the wrong county, watching London's traffic from Herefordshire, and
// CanadianAvenger's old ISP would have put him in Montreal while he sat in Toronto. A
// browser can do far better: it has WiFi positioning, and it knows its own timezone, which
// the IP lookup only ever guessed at from the same wrong place.
//
// NO RESTART, unlike Settings > Location, and that is the point of doing it here. Somebody
// is standing in Studio with a cable in their hand; taking the device out of service would
// drop the serial session they are using and leave the page hunting for an Orb that is
// rebooting. persist_location() and apply_location_live() exist precisely so a location can
// be applied without a restart, and the boot-time lookup already uses them that way.
void host_set_location_from_studio(const char *name, double lat, double lon,
                                   long tzOffsetSec, bool haveTz) {
    if (name && name[0]) { host_recents_add(name, lat, lon); persist_location_name(name); }
    // The browser's own timezone, which beats the IP lookup's guess for the same reason the
    // position does. Stored the same way and in the same format; the clock reads local from
    // the next second, with no restart, because tzset() is all that stands behind it.
    if (haveTz && tzOffsetSec >= -50400 && tzOffsetSec <= 50400) {
        char tz[24];
        posix_tz_from_offset(tzOffsetSec, tz, sizeof(tz));
        g_tz = tz;
        Preferences p;
        p.begin("capsuleradar", false);
        p.putString("tz", tz);
        p.end();
        setenv("TZ", g_tz.c_str(), 1);
        tzset();
        Serial.printf("[studio] tz offset %lds -> %s\n", tzOffsetSec, tz);
    }
    persist_location(lat, lon);
    apply_location_live(lat, lon);
    Serial.printf("[studio] location set to %.5f,%.5f%s%s\n", lat, lon,
                  (name && name[0]) ? " " : "", (name && name[0]) ? name : "");
}

// Settings > Location > Current, and the tail of first-boot WiFi setup. Somebody asked for
// this, so applying it by restarting is honest and is announced by the countdown inside
// host_set_location(). Returns false only if the lookup failed, in which case it has not
// rebooted and the caller still has a screen to put a message on.
bool host_locate_current() {
    double lat = 0, lon = 0;
    if (!ip_lookup_location(lat, lon)) return false;
    host_set_location(lat, lon);   // saves + reboots — does not return
    return true;
}

// The same lookup, for a device that has never had a location established: one attempt per
// boot, applied WITHOUT a restart. Nobody asked for this one, so it may not take the device
// out of service to apply itself (UX-048) — persist_location() and apply_location_live()
// exist to make that possible.
//
// This is the path that stops a failed first-boot lookup from being permanent. Before it,
// a stranger whose network blocked the lookup during setup sat on the compiled default
// forever, with no retry and nothing on the dial admitting the position was a guess.
void host_locate_if_unset() {
    if (g_locationSet) return;
    double lat = 0, lon = 0;
    if (!ip_lookup_location(lat, lon)) {
        Serial.println("[locate] no location set and the lookup failed; "
                       "the scope will say so until one is picked in Settings");
        return;
    }
    persist_location(lat, lon);
    apply_location_live(lat, lon);
    Serial.printf("[locate] located to %.4f, %.4f without a restart\n", lat, lon);
}

// Free city search (Open-Meteo geocoding, no key). Fills names/lats/lons with up to
// maxN matches for the query; returns the count. Runs synchronously (~1s).
int host_geocode(const char *query, char names[][40], double *lats, double *lons, int maxN) {
    if (WiFi.status() != WL_CONNECTED || !query || strlen(query) < 2) return 0;
    // Percent-encode anything that is not plainly safe in a query value. It used to encode
    // the space and pass everything else through, which was fine until the search keyboard
    // gained a comma (settings_view.cpp) and "Leeds, UT" started arriving here.
    String q;
    for (const char *p = query; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += (char)c;
        else { char esc[4]; snprintf(esc, sizeof(esc), "%%%02X", c); q += esc; }
    }
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
    persist_location_name(name);
    host_set_location(lat, lon);
}
