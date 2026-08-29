#include "ticker.h"
#include "config.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#else
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
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Quotes come from the Orb's own gateway, exactly as headlines do, and for exactly the same
// reason: every quote source on the internet is HTTPS-only and this board cannot do TLS at
// all. The gateway holds the TLS end, asks upstream for each symbol, throws away the 35 KB
// of chart series none of this needs, and answers here in a few hundred bytes of plain HTTP.
//
// Four symbols came back in 438 bytes when this was built, which is the whole argument for
// putting the work out there rather than in here.

#ifdef ARDUINO
using Payload = String;
#else
using Payload = std::string;
#endif

static std::mutex   s_mutex;
static TickerQuote  s_quotes[theme_style::TICKER_MAX_SYMBOLS];
static int          s_count      = 0;
static int          s_missing    = 0;
static TickerState  s_state      = TICKER_IDLE;
static uint32_t     s_updatedMs  = 0;
static uint32_t     s_nextAt     = 0;

void ticker_store_begin() {
    std::lock_guard<std::mutex> lk(s_mutex);
    s_count = 0; s_missing = 0; s_state = TICKER_IDLE; s_updatedMs = 0; s_nextAt = 0;
}

int ticker_count() { std::lock_guard<std::mutex> lk(s_mutex); return s_count; }
int ticker_missing() { std::lock_guard<std::mutex> lk(s_mutex); return s_missing; }
TickerState ticker_state() { std::lock_guard<std::mutex> lk(s_mutex); return s_state; }
uint32_t ticker_updated_ms() { std::lock_guard<std::mutex> lk(s_mutex); return s_updatedMs; }

bool ticker_get(int i, TickerQuote &out) {
    std::lock_guard<std::mutex> lk(s_mutex);
    if (i < 0 || i >= s_count) return false;
    out = s_quotes[i];
    return true;
}

static bool gateway_get(const char *url, Payload &payload) {
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return false;
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(INTEL_CONNECT_MS);
    http.setTimeout(INTEL_READ_MS);
    if (!http.begin(client, url)) { Serial.println("[ticker] HTTP begin failed"); return false; }
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        Serial.printf("[ticker] HTTP %d\n", status);
        http.end();
        return false;
    }
    // getString(), not getStream(): the worker answers chunked and ArduinoJson would read
    // the hexadecimal chunk size as the start of the document. Same trap as the news client.
    payload = http.getString();
    http.end();
#else
    if (!native_https_get(url, ADSB_USER_AGENT, payload, INTEL_READ_MS)) {
        Serial.println("[ticker] fetch failed");
        return false;
    }
#endif
    return payload.length() > 0;
}

// The watchlist, as the theme carries it, percent-escaped for a query string. Only ^ needs
// it among the characters a symbol may contain, but escaping by rule rather than by
// exception is what stops the next allowed character being the one that breaks the URL.
static void escape_symbols(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    for (const char *p = src; *p && o + 4 < cap; ++p) {
        const unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == ',' || c == '.' || c == '-' || c == '=') {
            dst[o++] = (char)c;
        } else if (c == ' ') {
            continue;                      // "AAPL, MSFT" is a list, not a symbol with a space
        } else {
            o += snprintf(dst + o, cap - o, "%%%02X", c);
        }
    }
    dst[o] = '\0';
}

bool ticker_fetch_step() {
    const theme_style::Ticker &cfg = theme_style::ticker();

    const uint32_t now = millis();
    if (s_nextAt && (int32_t)(now - s_nextAt) < 0) return false;

    if (!cfg.symbols[0]) {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_state = TICKER_NO_SYMBOLS;
        s_nextAt = now + 30000UL;
        return false;
    }
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) {
        std::lock_guard<std::mutex> lk(s_mutex);
        // Holding whatever we already had: a dropped connection does not make yesterday's
        // close untrue, and a screen that empties itself the moment the WiFi blinks is
        // worse than one that says the number is old.
        s_state = s_count ? TICKER_STALE : TICKER_NO_WIFI;
        s_nextAt = now + 15000UL;
        return false;
    }
#endif
    {
        std::lock_guard<std::mutex> lk(s_mutex);
        if (!s_count) s_state = TICKER_LOADING;
    }

    char syms[sizeof(cfg.symbols) * 3 + 8];
    escape_symbols(cfg.symbols, syms, sizeof(syms));

    char url[420];
#ifdef ARDUINO
    snprintf(url, sizeof(url), "http://%s/api/quote?symbols=%s", INTEL_GATEWAY_HOST, syms);
#else
    snprintf(url, sizeof(url), "https://%s/api/quote?symbols=%s", INTEL_GATEWAY_HOST, syms);
#endif

    Payload payload;
    if (!gateway_get(url, payload)) {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_state = s_count ? TICKER_STALE : TICKER_FAILED;
        s_nextAt = millis() + 30000UL;   // retry sooner than the poll: a failure is not a cadence
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, payload) != DeserializationError::Ok) {
        std::lock_guard<std::mutex> lk(s_mutex);
        s_state = s_count ? TICKER_STALE : TICKER_FAILED;
        s_nextAt = millis() + 30000UL;
        return false;
    }

    TickerQuote fresh[theme_style::TICKER_MAX_SYMBOLS];
    int n = 0;
    for (JsonObjectConst q : doc["quotes"].as<JsonArrayConst>()) {
        if (n >= theme_style::TICKER_MAX_SYMBOLS) break;
        const char *sym = q["sym"] | "";
        if (!*sym) continue;
        strlcpy(fresh[n].sym,  sym,            sizeof(fresh[n].sym));
        strlcpy(fresh[n].name, q["name"] | "", sizeof(fresh[n].name));
        strlcpy(fresh[n].cur,  q["cur"]  | "", sizeof(fresh[n].cur));
        fresh[n].price = q["price"] | 0.0f;
        fresh[n].prev  = q["prev"]  | 0.0f;
        fresh[n].at    = q["t"]     | 0u;
        ++n;
    }

    const int missing = doc["missing"].is<JsonArrayConst>()
                      ? (int)doc["missing"].as<JsonArrayConst>().size() : 0;

    if (!n) {
        // The gateway answered and priced nothing. That is a watchlist problem, not a
        // network one, and saying so is the difference between a person checking their
        // symbols and a person power-cycling the Orb.
        std::lock_guard<std::mutex> lk(s_mutex);
        s_state = s_count ? TICKER_STALE : TICKER_NO_SYMBOLS;
        s_missing = missing;
        s_nextAt = millis() + 60000UL;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(s_mutex);
        for (int i = 0; i < n; ++i) s_quotes[i] = fresh[i];
        s_count     = n;
        s_missing   = missing;
        s_state     = TICKER_READY;
        s_updatedMs = millis();
        s_nextAt    = millis() + (uint32_t)cfg.pollSeconds * 1000UL;
    }
    Serial.printf("[ticker] %d quote(s), %d unpriced, next in %ds\n", n, missing, cfg.pollSeconds);
    return true;
}
