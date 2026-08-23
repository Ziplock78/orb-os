#include "intel_client.h"
#include "config.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#else
// The desktop simulator has no WiFi stack, so the transport goes through libcurl exactly
// as location_view's does. Everything below the fetch is shared, which is the point: the
// simulator parses the same payload the Orb does and shows the same headlines.
#include "native_http.h"
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <string>
static struct {
    void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
    void println(const char *s) const { std::printf("%s\n", s); }
} Serial;
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>

// Headlines come from the Orb's own gateway rather than from a publisher directly, because
// every news source worth reading is HTTPS-only and this board cannot raise the two
// contiguous ~16 KB internal buffers a TLS handshake needs (see the ADSB_PRIMARY_TLS notes
// in config.h). The worker holds the TLS end, reads the feeds, cuts each headline to fit
// this dial, and answers here over plain HTTP.
//
// This is the opposite of the aircraft feed, which must NOT be routed through the worker:
// the ADS-B services answer 403 to Cloudflare's network while answering a home connection
// normally. News publishers have no such objection. See the note above liveTraffic in the
// gateway's server.ts.
bool intel_fetch(const char *topics, int want, IntelSnapshot &out) {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return false;
#endif
    if (want < 1) want = 1;
    if (want > INTEL_MAX_ITEMS) want = INTEL_MAX_ITEMS;
    if (!topics || !*topics) topics = "general";

    char url[256];
    // The scheme differs by build, and only the scheme. The device must use plain HTTP
    // because it cannot do TLS at all; the simulator has a full TLS stack and no reason to
    // send the request in the clear, so it asks the same gateway over HTTPS.
#ifdef ARDUINO
    snprintf(url, sizeof(url), "http://%s/api/intel?topics=%s&n=%d",
             INTEL_GATEWAY_HOST, topics, want);
#else
    snprintf(url, sizeof(url), "https://%s/api/intel?topics=%s&n=%d",
             INTEL_GATEWAY_HOST, topics, want);
#endif

#ifdef ARDUINO
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(INTEL_CONNECT_MS);
    http.setTimeout(INTEL_READ_MS);
    if (!http.begin(client, url)) {
        Serial.println("[intel] HTTP begin failed");
        return false;
    }
    http.addHeader("User-Agent", ADSB_USER_AGENT);

    const int status = http.GET();
    if (status != 200) {
        Serial.printf("[intel] HTTP %d: %s\n", status,
                      status < 0 ? http.errorToString(status).c_str() : "unexpected response");
        http.end();
        return false;
    }

    // The worker answers chunked, same as Open-Meteo. getString() strips the chunk framing;
    // handing getStream() to ArduinoJson makes it read the hexadecimal chunk size first and
    // report InvalidInput.
    String payload = http.getString();
    http.end();
    if (payload.length() == 0) {
        Serial.println("[intel] empty response body");
        return false;
    }
#else
    std::string payload;
    if (!native_https_get(url, ADSB_USER_AGENT, payload, INTEL_READ_MS)) {
        Serial.println("[intel] fetch failed");
        return false;
    }
    if (payload.empty()) {
        Serial.println("[intel] empty response body");
        return false;
    }
#endif

    // Five headlines at 70 characters plus their source tags is well under a kilobyte;
    // measured at 458 bytes on 2026-08-23. The margin is for a feed that starts sending
    // longer source names, not for a payload shape that could grow without notice.
    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[intel] JSON parse failed: %s\n", err.c_str());
        return false;
    }

    JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.isNull()) {
        // A gateway that reached no feed answers with an empty list and a note. Say what it
        // said: "no headlines" from the worker is a different fault from a dead gateway,
        // and only one of them is the Orb's network.
        const char *note = doc["note"] | "no items";
        Serial.printf("[intel] gateway returned no headlines (%s)\n", note);
        return false;
    }

    IntelSnapshot snap = {};
    for (JsonObjectConst it : items) {
        if (snap.count >= INTEL_MAX_ITEMS) break;
        const char *h = it["h"] | "";
        if (!*h) continue;
        IntelItem &slot = snap.items[snap.count];
        // Truncating copy: the worker already cuts to fit, so this is the belt to that
        // braces. snprintf always terminates, which strncpy would not.
        snprintf(slot.text, sizeof(slot.text), "%s", h);
        snprintf(slot.source, sizeof(slot.source), "%s", it["s"] | "");
        snap.count++;
    }
    if (snap.count == 0) {
        Serial.println("[intel] every headline was empty");
        return false;
    }

    snap.valid = true;
    snap.fetchedMs = millis();
    out = snap;
    Serial.printf("[intel] %d headlines (%u bytes)\n", snap.count, (unsigned)payload.length());
    return true;
}
