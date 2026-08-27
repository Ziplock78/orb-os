#include "intel_client.h"
#include "config.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#else
// The desktop simulator has no WiFi stack, so the transport goes through libcurl exactly
// as the weather client's does. Everything below the fetch is shared, which is the point: the
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
#ifdef ARDUINO
using Payload = String;
#else
using Payload = std::string;
#endif

// One GET against the gateway, whichever platform this is.
//
// Extracted when the briefing needed a second endpoint. Two copies of the platform split is
// two places for a timeout, a user agent or a chunked-encoding workaround to drift, and the
// chunked one below is exactly the sort of thing that gets fixed in one copy only.
static bool gateway_get(const char *url, const char *what, Payload &payload) {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(INTEL_CONNECT_MS);
    http.setTimeout(INTEL_READ_MS);
    if (!http.begin(client, url)) {
        Serial.printf("[intel] %s: HTTP begin failed\n", what);
        return false;
    }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        Serial.printf("[intel] %s: HTTP %d: %s\n", what, status,
                      status < 0 ? http.errorToString(status).c_str() : "unexpected response");
        http.end();
        return false;
    }
    // The worker answers chunked, same as Open-Meteo. getString() strips the chunk framing;
    // handing getStream() to ArduinoJson makes it read the hexadecimal chunk size first and
    // report InvalidInput.
    payload = http.getString();
    http.end();
#else
    if (!native_https_get(url, ADSB_USER_AGENT, payload, INTEL_READ_MS)) {
        Serial.printf("[intel] %s: fetch failed\n", what);
        return false;
    }
#endif
    if (payload.length() == 0) {
        Serial.printf("[intel] %s: empty response body\n", what);
        return false;
    }
    return true;
}

bool intel_fetch(const char *topics, const char *source, int want, IntelSnapshot &out) {
    if (want < 1) want = 1;
    if (want > INTEL_MAX_ITEMS) want = INTEL_MAX_ITEMS;
    if (!topics || !*topics) topics = "general";
    if (!source || !*source) source = "bbc";

    char url[256];
    // The scheme differs by build, and only the scheme. The device must use plain HTTP
    // because it cannot do TLS at all; the simulator has a full TLS stack and no reason to
    // send the request in the clear, so it asks the same gateway over HTTPS.
#ifdef ARDUINO
    snprintf(url, sizeof(url), "http://%s/api/intel?topics=%s&n=%d&source=%s",
             INTEL_GATEWAY_HOST, topics, want, source);
#else
    snprintf(url, sizeof(url), "https://%s/api/intel?topics=%s&n=%d&source=%s",
             INTEL_GATEWAY_HOST, topics, want, source);
#endif

    Payload payload;
    if (!gateway_get(url, "headlines", payload)) return false;

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
        // Absent on a gateway older than the briefing feature. An item with no key simply
        // cannot be opened, which the view reports honestly rather than pretending the
        // press did nothing.
        snprintf(slot.key, sizeof(slot.key), "%s", it["k"] | "");
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

// One story's summary, asked for by the key the list handed out.
//
// Deliberately a second request rather than a field on the first: twenty briefs is about
// ten kilobytes of JSON, this holds a response whole and then builds a JsonDocument over
// it, and the peak would be two to three times that on a board that cannot find two
// contiguous 16 KB blocks. See the note on INTEL_BRIEF_BYTES.
//
// Every outcome is reported as a state rather than a bool, because "the feed has no summary
// for this one" and "the story aged out of the feed" and "the network is down" are three
// different sentences and the reader deserves the right one.
IntelBriefState intel_brief_fetch(const char *topics, const char *source, const char *key,
                                  char *headlineOut, size_t headlineCap,
                                  char *sourceOut, size_t sourceCap,
                                  char *bodyOut, size_t bodyCap) {
    if (headlineOut && headlineCap) headlineOut[0] = 0;
    if (sourceOut && sourceCap)     sourceOut[0] = 0;
    if (bodyOut && bodyCap)         bodyOut[0] = 0;
    if (!key || !*key) return INTEL_BRIEF_GONE;
    if (!topics || !*topics) topics = "general";
    if (!source || !*source) source = "bbc";

    char url[256];
#ifdef ARDUINO
    snprintf(url, sizeof(url), "http://%s/api/intel/brief?topics=%s&source=%s&k=%s",
             INTEL_GATEWAY_HOST, topics, source, key);
#else
    snprintf(url, sizeof(url), "https://%s/api/intel/brief?topics=%s&source=%s&k=%s",
             INTEL_GATEWAY_HOST, topics, source, key);
#endif

    Payload payload;
    // A 404 from the gateway means the story is gone, which gateway_get reports as a plain
    // failure like any other. Both land on the same screen wording ("that story is no longer
    // in the feed" versus "could not reach the gateway") only because the two states below
    // are kept apart; do not collapse them.
    if (!gateway_get(url, "brief", payload)) return INTEL_BRIEF_FAILED;

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[intel] brief: JSON parse failed: %s\n", err.c_str());
        return INTEL_BRIEF_FAILED;
    }
    if (doc["note"].is<const char *>()) {
        Serial.printf("[intel] brief: gateway said %s\n", doc["note"] | "no");
        return INTEL_BRIEF_GONE;
    }

    const char *h = doc["h"] | "";
    const char *s = doc["s"] | "";
    const char *b = doc["b"] | "";
    if (headlineOut && headlineCap) snprintf(headlineOut, headlineCap, "%s", h);
    if (sourceOut && sourceCap)     snprintf(sourceOut, sourceCap, "%s", s);
    if (bodyOut && bodyCap)         snprintf(bodyOut, bodyCap, "%s", b);
    // A real answer with nothing in it. Some feed items genuinely carry no description, and
    // saying so beats a spinner that never resolves.
    if (!*b) return INTEL_BRIEF_EMPTY;
    Serial.printf("[intel] brief %s: %u chars\n", key, (unsigned)strlen(b));
    return INTEL_BRIEF_READY;
}
