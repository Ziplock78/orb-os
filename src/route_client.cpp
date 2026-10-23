// Route lookup via adsbdb.com (free, no API key): GET /v0/callsign/{callsign}.
// Returns origin/destination city names (English). Device-only.
#include "route_client.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <string.h>
#include <time.h>   // route-cache TTL
#include <esp_heap_caps.h>
#include "flightaware_route.h"

// Direct FlightAware lookup; successful results use Orb's existing NVS cache.
static constexpr bool DIRECT_FLIGHTAWARE_POC = true;

static bool flightaware_fetch(const char* callsign, char* from, size_t fn, char* to, size_t tn) {
    char cs[12] = {};
    size_t n = 0;
    for (const char* p = callsign; p && *p; ++p) {
        if (*p == ' ') continue;
        const char c = toupper(static_cast<unsigned char>(*p));
        if (!isalnum(static_cast<unsigned char>(c)) || n + 1 >= sizeof(cs)) return false;
        cs[n++] = c;
    }
    if (!n || WiFi.status() != WL_CONNECTED) return false;
    Serial.printf("[route-fa] %s begin: internal=%u largest=%u stack=%u\n", cs,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    WiFiClientSecure client;
    // Match Capsule-Radar's transport for this experiment. No credentials sent.
    // Certificate authentication must be added before promoting this to release.
    client.setInsecure();
    client.setHandshakeTimeout(5);
    HTTPClient http;
    http.setReuse(false);
    http.useHTTP10(true);  // raw stream has no HTTP chunk framing
    http.setConnectTimeout(3000);
    http.setTimeout(5000);
    char url[128];
    snprintf(url, sizeof(url), "https://www.flightaware.com/live/flight/%s", cs);
    if (!http.begin(client, url)) return false;
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36");
    http.addHeader("Accept", "text/html");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Accept-Language", "en-US,en;q=0.9");
    const uint32_t started = millis();
    const int code = http.GET();
    if (code < 0) {
        char error[96] = {};
        const int tlsError = client.lastError(error, sizeof(error));
        Serial.printf("[route-fa] TLS error=%d: %s\n", tlsError, error);
    }
    Serial.printf("[route-fa] HTTP=%d after %u ms (%s)\n", code, (unsigned)(millis()-started),
                  code < 0 ? HTTPClient::errorToString(code).c_str() : "connected");
    FlightAwareRoute route;
    size_t bytes = 0;
    if (code == 200) {
        auto* stream = http.getStreamPtr();
        const uint32_t bodyStart = millis();
        while (bytes < 25600 && millis() - bodyStart < 2500 && !route.complete()) {
            if (stream->available()) {
                const int c = stream->read();
                if (c >= 0) { route.feed(static_cast<char>(c)); ++bytes; }
            } else if (!http.connected()) break;
            else delay(1);
        }
    }
    http.end();
    Serial.printf("[route-fa] bytes=%u route=%s -> %s elapsed=%u ms\n",
        (unsigned)bytes, route.from, route.to, (unsigned)(millis()-started));
    if (!route.complete()) return false;
    snprintf(from, fn, "%s", route.from);
    snprintf(to, tn, "%s", route.to);
    return true;
}

#define ROUTE_CACHE_MAX 200   // wrap the cache before it can crowd NVS

// strip spaces -> a valid NVS key (callsigns are <= 8 chars)
static void route_key(const char *callsign, char *out, size_t on) {
    size_t j = 0;
    for (const char *p = callsign; *p && j < on - 1; ++p)
        if (*p != ' ') out[j++] = *p;
    out[j] = 0;
}

#define ROUTE_FMT_VER 3   // invalidate the old gateway routes
static constexpr uint32_t ROUTE_CACHE_TTL_S = 15 * 60;

void route_cache_begin() {
    Preferences p;
    if (!p.begin("routes", false)) return;
    if (p.getUChar("__v", 0) != ROUTE_FMT_VER) { p.clear(); p.putUChar("__v", ROUTE_FMT_VER); }
    p.end();
}

bool route_cache_get(const char *callsign, char *from, size_t fn, char *to, size_t tn,
                     uint32_t *remainingMs) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0]) return false;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return false;
    Preferences p;
    if (!p.begin("routes", true)) return false;
    String v = p.getString(key, "");     // stored as "epoch|from|to"
    p.end();
    if (v.length() == 0) return false;
    const int b1 = v.indexOf('|');
    if (b1 < 0) return false;
    const uint32_t ts = (uint32_t)v.substring(0, b1).toInt();
    const String rest = v.substring(b1 + 1);
    const int b2 = rest.indexOf('|');
    if (b2 < 0) return false;
    const uint32_t now = (uint32_t)time(nullptr);    // expire stale routes (reused callsigns)
    if (now <= 1700000000UL || ts <= 1700000000UL || now < ts || now - ts >= ROUTE_CACHE_TTL_S)
        return false;
    snprintf(from, fn, "%s", rest.substring(0, b2).c_str());
    snprintf(to, tn, "%s", rest.substring(b2 + 1).c_str());
    if (remainingMs) *remainingMs = (ROUTE_CACHE_TTL_S - (now - ts)) * 1000;
    return from[0] && to[0];
}

void route_cache_put(const char *callsign, const char *from, const char *to) {
    const uint32_t now = (uint32_t)time(nullptr);
    if (now <= 1700000000UL || !from || !from[0] || !to || !to[0]) return;
    if (!callsign || !callsign[0]) return;
    char key[12];
    route_key(callsign, key, sizeof(key));
    if (!key[0]) return;
    Preferences p;
    if (!p.begin("routes", false)) return;
    int n = p.getInt("__n", 0);
    bool exists = p.isKey(key);
    if (!exists && n >= ROUTE_CACHE_MAX) {
        p.clear(); p.putUChar("__v", ROUTE_FMT_VER); n = 0;
    }
    String v = String(now) + "|" + String(from) + "|" + String(to);
    if (p.putString(key, v) > 0 && !exists) p.putInt("__n", n + 1);
    p.end();
}

// The 3-letter IATA airport code ("JFK", "LHR") — short, unambiguous, and what a
// {from}/{to} banner actually has room for. Falls back to a cleaned-up name (then
// municipality) only on the rare response that has no IATA code at all.
bool route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    if (fn) from[0] = 0;
    if (tn) to[0] = 0;
    if (!callsign || !callsign[0] || WiFi.status() != WL_CONNECTED) return false;
    if (DIRECT_FLIGHTAWARE_POC) return flightaware_fetch(callsign, from, fn, to, tn);

    // strip spaces from the callsign
    char cs[12];
    size_t j = 0;
    for (const char *p = callsign; *p && j < sizeof(cs) - 1; ++p)
        if (*p != ' ') cs[j++] = *p;
    cs[j] = 0;
    if (j == 0) return false;

    char url[128];
    // THROUGH THE GATEWAY, because adsbdb stopped answering this device.
    //
    // This asked api.adsbdb.com directly over plain HTTP, and plain HTTP was not laziness:
    // a TLS handshake on this board needs two contiguous ~16 KB internal buffers while the
    // largest free block runs 7-16 KB, so https here is not slower, it is impossible. Every
    // attempt failed with SSL -32512. Port 80 served the same JSON, so port 80 it was.
    //
    // It does not any more. api.adsbdb.com now answers 301 Moved Permanently with an HTML
    // body, so the parse below found nothing and every route lookup on every Orb reported
    // "no route". Zion: "I don't see the information in the info card about the airport
    // they're coming from and where they're going to." Nothing on the device had broken and
    // nothing in its log looked alarming. The service moved.
    //
    // The gateway does the handshake this chip cannot, which is what it exists for, and it
    // hands back about forty bytes instead of seven hundred: the airport picking that used
    // to happen here now happens there, where there is memory to do it in.
#ifdef ARDUINO
    snprintf(url, sizeof(url), "http://%s/api/route?callsign=%s", INTEL_GATEWAY_HOST, cs);
#else
    snprintf(url, sizeof(url), "https://%s/api/route?callsign=%s", INTEL_GATEWAY_HOST, cs);
#endif

    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);   // short: runs on the feed task, don't stall the live poll
    http.setTimeout(6000);
    if (!http.begin(client, url)) return false;
    // setUserAgent, not addHeader: HTTPClient silently DROPS a "User-Agent" added as a
    // header and sends its own default. Same trap that had the ADS-B feed introducing this
    // device as "ESP32HTTPClient" until it started being refused for it.
    http.setUserAgent(ADSB_USER_AGENT);

    const int code = http.GET();
    if (code != 200) { http.end(); return false; }

    // {"from":"SMF","to":"PHX"}, or {} for a callsign nobody knows, which is normal rather
    // than an error. No filter needed at this size.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return false;

    snprintf(from, fn, "%s", doc["from"] | "");
    snprintf(to,   tn, "%s", doc["to"]   | "");
    return (from[0] || to[0]);
}
