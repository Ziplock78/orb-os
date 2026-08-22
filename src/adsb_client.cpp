// Fetch nearby aircraft from airplanes.live (fallback adsb.lol) and parse the
// readsb JSON into a vector<Aircraft>.
//
// Memory safety (important on the ESP32): we parse straight from the HTTP stream
// (no full-body String), use an ArduinoJson field filter so only the ~12 fields we
// need are kept, and hard-cap the number of aircraft (ADSB_MAX_AIRCRAFT). The radar
// then keeps only the nearest ~20 for display.
#include "adsb_client.h"
#include "config.h"
#include "geo.h"           // haversineKm — keep the nearest N aircraft
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>   // v7
#include <esp_heap_caps.h>
#include <memory>          // std::unique_ptr for the TLS client

// Parse the JSON in PSRAM, not internal RAM. Otherwise the per-poll JSON alloc/free
// churn fragments the internal heap and, after a while, mbedTLS can't find a large
// enough contiguous block for the TLS handshake (-32512), freezing the feed.
struct PsramJsonAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void  deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramJsonAllocator s_jsonPsram;

// NetworkClient::readBytes() treats a transient negative TLS read as end-of-input,
// which makes ArduinoJson intermittently report IncompleteInput. Deliberately wrap
// the client without overriding readBytes(): Stream's timed byte reader retries
// temporary no-data reads until the configured timeout.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    _refused = false;
    _lastStatus = 0;
    _refusedStatus = 0;
    // Try each independent provider once. Retrying the primary immediately can violate its
    // one-request-per-second limit and adds another full timeout to an already slow failure.
    if (fetchFrom(ADSB_PRIMARY_HOST, ADSB_PRIMARY_TLS, out)) return true;
#if !ADSB_FALLBACK_TLS
    // A TLS fallback on this board is not a fallback, it is a second guaranteed failure paid
    // for on every primary miss. Measured 2026-08-22: "SSL - Memory allocation failed" every
    // time, because a handshake needs two ~16 KB contiguous internal buffers and this board's
    // largest free block was 2-3 KB. Attempting it anyway spent time and fragmentation on a
    // request that could not have succeeded, right when the primary failing is the moment
    // memory is already tightest. Only compiled in when a plain-HTTP fallback host is
    // configured; skipped entirely while ADSB_FALLBACK_TLS is 1, which it is today.
    return fetchFrom(ADSB_FALLBACK_HOST, ADSB_FALLBACK_TLS, out);
#else
    return false;
#endif
}

bool AdsbClient::fetchFrom(const char* host, bool tls, std::vector<Aircraft>& out) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char url[160];
    snprintf(url, sizeof(url), "%s://%s/v2/point/%.4f/%.4f/%.0f",
             tls ? "https" : "http", host, _lat, _lon, nm);

    // WiFiClientSecure is heap-allocated and ONLY when tls is actually requested. It used to
    // be a plain stack local built unconditionally on every call — "cheap", the old comment
    // said, on the reasoning that only the handshake was expensive. That undersold its
    // constructor: WiFiClientSecure wraps an mbedtls context that allocates its own internal
    // state, and this function runs once per poll, forever, whether or not tls is true. With
    // the TLS fallback disabled (see poll() above) tls is always false today, which means the
    // "cheap" object was being built and torn down every 5 seconds for a code path that could
    // never be reached. Suspected to be the source of the slow allocated-block creep measured
    // 2026-08-22 (1956 -> 1968 blocks over 240 s of failed polls, free_bytes plateaued but
    // never fully recovering) — a constructor/destructor pair whose alloc/free do not land in
    // the exact reverse order leaves a permanent hole even when nothing looks "leaked" by
    // total byte count.
    // unique_ptr, not a raw new/delete pair: fetchFrom has several early returns (begin
    // failed, non-200, JSON error, empty payload), and a leak only has to be missed on ONE
    // of them to reproduce exactly the slow creep this was built to fix. RAII means every
    // exit, however it happens, frees this the same way.
    WiFiClient                        plain;
    std::unique_ptr<WiFiClientSecure> secure;
    WiFiClient                       *client = &plain;
    if (tls) {
        secure = std::make_unique<WiFiClientSecure>();
#if ADSB_HTTPS_INSECURE
        secure->setInsecure();                          // hobby: skip cert validation
#else
        // secure->setCACert(ROOT_CA_PEM);              // production: pin the root CA
#endif
        client = secure.get();
    }

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(6000);    // fail reasonably fast: a slow host must not block the
    http.setTimeout(8000);           // task (and the user's route/photo lookups) for too long
    if (!http.begin(*client, url)) { Serial.printf("[adsb] begin failed (%s)\n", host); return false; }
    // setUserAgent, NOT addHeader. ESP32's HTTPClient keeps its own _userAgent member and
    // addHeader() silently DROPS "User-Agent" (along with Host and Connection) rather than
    // erroring, so this line looked correct for as long as it has existed while the Orb
    // actually introduced itself as the library default, "ESP32HTTPClient". Captured on the
    // wire 2026-08-22 by pointing the device at a local server and printing what arrived.
    //
    // It stopped being harmless when api.adsb.lol began refusing that default agent with a
    // 403: a generic unidentified client is exactly what an anti-abuse rule looks for. Every
    // other fetch in this project already used setUserAgent (see net_fetch.cpp); this one
    // was the odd one out.
    http.setUserAgent(ADSB_USER_AGENT);
    http.addHeader("Accept", "application/json");

    const int code = http.GET();
    _lastStatus = code;
    // Any 4xx is the server declining, not this board failing. 403 and 429 are the two that
    // actually turn up: a feed that has decided we are asking too often. Recorded here so
    // the caller can back off for minutes and, above all, NOT reboot over it.
    if (code >= 400 && code < 500) { _refused = true; _refusedStatus = code; }
    if (code != 200) {
        // Only the secure client can explain itself; over plain HTTP there is no TLS state
        // to report, and asking for it would mean calling through the base pointer.
        char tlsMsg[128] = "";
        const int tlsCode = (tls && secure) ? secure->lastError(tlsMsg, sizeof(tlsMsg)) : 0;
        Serial.printf("[adsb] HTTP %d (%s) tls=%d '%s' heap=%u largest=%u psram=%u\n",
                      code, host, tlsCode, tlsMsg,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end(); return false;
    }

    // Only keep the fields we use -> much smaller parsed document.
    JsonDocument filter(&s_jsonPsram);
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;

    JsonDocument doc(&s_jsonPsram);
    const int expectedBytes = http.getSize();
    NetworkClient& responseStream = http.getStream();
    ReliableJsonStream jsonStream(responseStream);
    DeserializationError err = deserializeJson(doc, jsonStream,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[adsb] JSON parse failed (%s): %s; expected=%d read=%u available=%d connected=%d\n",
                      host, err.c_str(), expectedBytes, (unsigned)jsonStream.bytesRead(),
                      responseStream.available(), responseStream.connected());
        http.end();
        return false;
    }
    http.end();

    JsonArrayConst arr = doc["ac"].as<JsonArrayConst>();
    if (arr.isNull()) arr = doc["aircraft"].as<JsonArrayConst>();
    if (arr.isNull()) return false;

    // Keep the ADSB_MAX_AIRCRAFT *nearest* aircraft (not just the first ones the feed happens to
    // list), so busy areas still show the traffic closest to you. We gate by distance BEFORE
    // parsing the strings, so the hundreds of far-away aircraft never allocate anything.
    std::vector<Aircraft> tmp;
    std::vector<float>     dist;             // parallel array: km from home for each kept aircraft
    tmp.reserve(ADSB_MAX_AIRCRAFT);
    dist.reserve(ADSB_MAX_AIRCRAFT);
    const uint32_t now = millis();
    for (JsonObjectConst a : arr) {
        if (a["lat"].isNull() || a["lon"].isNull()) continue;   // need a position
        const double lat = a["lat"].as<double>();
        const double lon = a["lon"].as<double>();

        // alt_baro is the string "ground" for aircraft on the ground; skip them if hide-ground is on.
        const bool  onGround = a["alt_baro"].is<const char*>();
        const float altFt    = onGround ? 0.0f : (a["alt_baro"] | 0.0f);
        if (_hideGround && onGround) continue;
        // optional filters (applied before the cap, so slots only go to matching aircraft)
        if (_minAltFt > 0.0f && (onGround || altFt < _minAltFt)) continue;
        if (_milOnly && (((a["dbFlags"] | 0u) & 0x1) == 0)) continue;

        const float d = (float)geo::haversineKm(_lat, _lon, lat, lon);

        // Center dead zone: traffic this close projects into the middle of the
        // scope, underneath whatever the theme puts there (a hub, a gear, a hand
        // pivot), where a cluster of blips just reads as clutter. Dropped here,
        // before the nearest-N gate below, so the slots go to aircraft that will
        // actually be visible. Set from the design's own Scope card (0 = off).
        if (_minDistKm > 0.0f && d < _minDistKm) continue;

        // nearest-N gate: if the buffer is full and this one isn't closer than the farthest kept,
        // drop it now — before any string allocation.
        int farIdx = -1;
        if ((int)tmp.size() >= ADSB_MAX_AIRCRAFT) {
            farIdx = 0;
            for (int i = 1; i < (int)dist.size(); ++i) if (dist[i] > dist[farIdx]) farIdx = i;
            if (d >= dist[farIdx]) continue;
        }

        Aircraft ac;
        ac.hex = (const char*)(a["hex"] | "");
        if (ac.hex.length() == 0) continue;
        ac.flight = String((const char*)(a["flight"] | "")); ac.flight.trim();
        ac.type   = (const char*)(a["t"] | "");
        ac.lat = lat; ac.lon = lon;
        ac.onGround = onGround;
        ac.altBaro  = altFt;
        ac.track    = a["track"].is<float>() ? a["track"].as<float>() : (a["true_heading"] | NAN);
        ac.gs       = a["gs"] | NAN;
        ac.baroRate = a["baro_rate"] | NAN;
        ac.squawk   = a["squawk"].is<const char*>() ? atoi(a["squawk"]) : (a["squawk"] | -1);
        ac.seenPos  = a["seen_pos"] | 0;
        ac.military = ((a["dbFlags"] | 0u) & 0x1) != 0;
        ac.lastUpdateMs = now;

        if (farIdx >= 0) { tmp[farIdx] = std::move(ac); dist[farIdx] = d; }   // replace the farthest kept
        else             { tmp.push_back(std::move(ac)); dist.push_back(d); }
    }

    out.swap(tmp);
    _lastOkMs = now;
    return true;
}
