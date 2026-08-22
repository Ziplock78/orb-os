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
//
// BUFFERED as of 2026-08-22. ArduinoJson pulls its input one byte at a time, so parsing a
// ~40 KB aircraft response meant ~40,000 separate single-byte reads straight into the
// socket, every poll, forever. Each of those goes through lwIP, whose buffers live in the
// internal RAM this feed is starved for — a far better explanation for the measured
// fragmentation than the parsed document itself, which is already allocated in PSRAM (see
// PsramJsonAllocator above, and note it was added for exactly this class of problem).
//
// The buffer refills in 1 KB chunks and ArduinoJson is served from it. Same bytes, same
// order, same reliability contract; roughly a thousandth of the socket calls.
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return (int)(_len - _pos) + _source.available(); }
    int read() override {
        if (_pos >= _len && !refill()) return -1;
        ++_bytesRead;
        return _buf[_pos++];
    }
    // peek() must not consume, but it may legitimately need to pull the next chunk in to
    // answer at all — the parser peeks across a buffer boundary like any other position.
    int peek() override {
        if (_pos >= _len && !refill()) return -1;
        return _buf[_pos];
    }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    // Keeps the original contract: wait for bytes rather than treating a momentary empty
    // socket as end-of-input, which is the bug the single-byte version existed to dodge.
    // Returns false only on a real timeout or a closed connection with nothing left.
    bool refill() {
        _pos = _len = 0;
        const uint32_t started = millis();
        for (;;) {
            const int avail = _source.available();
            if (avail > 0) {
                const size_t want = avail < (int)sizeof(_buf) ? (size_t)avail : sizeof(_buf);
                const int got = _source.readBytes(_buf, want);
                if (got > 0) { _len = (size_t)got; return true; }
            }
            // Nothing right now. Give up only on the same timeout the single-byte reader
            // used. Deliberately NOT down-casting _source to ask whether the peer is still
            // connected: this holds a Stream&, and quietly assuming it is really a
            // NetworkClient would be undefined behaviour the moment anything else is passed
            // in. The timeout is the honest, type-safe stopping condition, and it is exactly
            // what the previous implementation relied on.
            if (millis() - started > 8000) return false;
            delay(1);
        }
    }

    uint8_t _buf[1024];
    size_t  _pos = 0;
    size_t  _len = 0;
    Stream& _source;
    size_t _bytesRead = 0;
};

void AdsbClient::begin(double homeLat, double homeLon, float rangeKm) {
    _lat = homeLat; _lon = homeLon; _rangeKm = rangeKm;
}

// ---------------------------------------------------------------- edge pool
//
// Rate limiting on this feed is applied PER EDGE, not per account and not per client.
// Measured directly on 2026-08-22: with five addresses behind api.adsb.lol, one answered
// 429 while four others answered 200 in the same second. A refusal is therefore not a
// reason to stop asking, it is a reason to ask a different door.
//
// DNS hands back one address at a time and lwIP caches it, so the pool is LEARNED rather
// than hardcoded: every resolve that returns something new is remembered. Hardcoding the
// addresses would work today and rot silently the first time the operator renumbers, and a
// dead hardcoded address costs a full connect timeout on every rotation.
namespace {

IPAddress s_edge[ADSB_EDGE_POOL];
uint8_t   s_edgeN  = 0;      // how many distinct addresses learned so far
uint8_t   s_edgeAt = 0;      // which one to try first next time

void learn_edge() {
    IPAddress ip;
    if (!WiFi.hostByName(ADSB_PRIMARY_HOST, ip)) return;
    for (uint8_t i = 0; i < s_edgeN; ++i) if (s_edge[i] == ip) return;
    if (s_edgeN < ADSB_EDGE_POOL) {
        s_edge[s_edgeN++] = ip;
        Serial.printf("[adsb] learned edge %s (%u known)\n", ip.toString().c_str(), (unsigned)s_edgeN);
    }
}

// One line of a response header, read without Arduino String. Headers are a few hundred
// bytes so byte-at-a-time is fine here; the BODY is what gets the buffered reader.
int read_line(WiFiClient &c, char *out, size_t cap, uint32_t deadline) {
    size_t n = 0;
    while ((int32_t)(millis() - deadline) < 0) {
        const int ch = c.read();
        if (ch < 0) { delay(1); continue; }
        if (ch == '\n') { if (n && out[n - 1] == '\r') --n; out[n] = 0; return (int)n; }
        if (n + 1 < cap) out[n++] = (char)ch;
    }
    return -1;
}

} // namespace

// A whole HTTP/1.1 GET, by hand, onto a socket we own.
//
// Deliberately not HTTPClient. Two reasons, both measured. It cannot send a Host header
// that differs from the address it dialled (addHeader silently drops "Host", the same trap
// that had this device calling itself ESP32HTTPClient for months), and addressing a chosen
// edge by IP while still saying "Host: api.adsb.lol" is the entire point of the pool above.
// And it rebuilds several Arduino Strings per request on an internal heap whose largest
// free block has been as low as 500 bytes on this board.
//
// Returns the HTTP status, or a negative transport error.
int AdsbClient::rawGet(const IPAddress &ip, const char *path, long &contentLen) {
    contentLen = -1;
    // Reuse the socket when it is already open to the SAME edge; otherwise start clean.
    if (_plain.connected() && !(_epIp == ip)) { _plain.stop(); }
    if (!_plain.connected()) {
        if (!_plain.connect(ip, 80, ADSB_CONNECT_MS)) return -1;
        _epIp = ip;
    }

    char req[320];
    const int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Accept: application/json\r\n"
        "Connection: keep-alive\r\n\r\n",
        path, ADSB_PRIMARY_HOST, ADSB_USER_AGENT);
    if (n <= 0 || _plain.write((const uint8_t *)req, (size_t)n) != (size_t)n) { _plain.stop(); return -2; }

    const uint32_t deadline = millis() + ADSB_READ_MS;
    char line[160];
    if (read_line(_plain, line, sizeof(line), deadline) < 0) { _plain.stop(); return -3; }
    // "HTTP/1.1 200 OK"
    int status = 0;
    { const char *sp = strchr(line, ' '); if (sp) status = atoi(sp + 1); }
    if (status <= 0) { _plain.stop(); return -4; }

    bool keepAlive = true;
    for (;;) {
        const int len = read_line(_plain, line, sizeof(line), deadline);
        if (len < 0) { _plain.stop(); return -5; }
        if (len == 0) break;                                   // blank line: body follows
        if (!strncasecmp(line, "Content-Length:", 15)) contentLen = atol(line + 15);
        else if (!strncasecmp(line, "Connection:", 11) && strcasestr(line, "close")) keepAlive = false;
        else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line, "chunked")) contentLen = -2;
    }
    _canKeepAlive = keepAlive;
    return status;
}

bool AdsbClient::poll(std::vector<Aircraft>& out) {
    if (WiFi.status() != WL_CONNECTED) return false;
    _refused = false;
    _lastStatus = 0;
    _refusedStatus = 0;

    // Keep discovering addresses. Cheap (a cached lookup most of the time) and it is what
    // keeps the pool current without anything being written down in the source.
    if (s_edgeN < ADSB_EDGE_POOL) learn_edge();
    if (s_edgeN == 0) return false;                            // no DNS yet; try again next poll

    // Try each known edge once before giving up on the poll entirely. A 429 or a dead
    // socket on one address says nothing about the others.
    for (uint8_t attempt = 0; attempt < s_edgeN; ++attempt) {
        const uint8_t idx = (uint8_t)((s_edgeAt + attempt) % s_edgeN);
        if (fetchFrom(s_edge[idx], out)) {
            s_edgeAt = idx;                                    // stay on what works
            return true;
        }
    }
    s_edgeAt = (uint8_t)((s_edgeAt + 1) % s_edgeN);            // rotate for next time
    return false;
}

bool AdsbClient::fetchFrom(const IPAddress &ip, std::vector<Aircraft>& out) {
    const double nm = _rangeKm * 0.539957;            // km -> nautical miles (API radius unit)
    char path[96];
    snprintf(path, sizeof(path), "/v2/point/%.4f/%.4f/%.0f", _lat, _lon, nm);

    long contentLen = -1;
    const int code = rawGet(ip, path, contentLen);
    _lastStatus = code;
    // Any 4xx is a server declining, not this board failing. Recorded so the caller can back
    // off and, above all, NOT reboot over it. With the pool above, a 4xx from one edge is
    // handled by simply trying the next one first.
    if (code >= 400 && code < 500) { _refused = true; _refusedStatus = code; }
    if (code != 200) {
        Serial.printf("[adsb] %s -> %d  heap=%u largest=%u\n",
                      ip.toString().c_str(), code,
                      (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        if (code < 0) _plain.stop();                   // transport failure: do not reuse it
        return false;
    }
    if (contentLen == -2) { Serial.println("[adsb] chunked response, unsupported"); _plain.stop(); return false; }

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
    ReliableJsonStream jsonStream(_plain);
    DeserializationError err = deserializeJson(doc, jsonStream,
                                               DeserializationOption::Filter(filter));
    if (err) {
        Serial.printf("[adsb] %s parse failed: %s; expected=%ld read=%u\n",
                      ip.toString().c_str(), err.c_str(), contentLen,
                      (unsigned)jsonStream.bytesRead());
        _plain.stop();                       // unknown position in the stream: never reuse it
        return false;
    }
    // Keep-alive only works if the socket is left exactly at the end of this body. The
    // parser stops on the closing brace, so anything the server appended after it (a
    // trailing newline is common) has to be consumed or it becomes the first bytes of the
    // NEXT response and corrupts a perfectly good poll.
    if (_canKeepAlive && contentLen > 0) {
        long left = contentLen - (long)jsonStream.bytesRead();
        const uint32_t until = millis() + 1000;
        while (left > 0 && (int32_t)(millis() - until) < 0) {
            if (_plain.read() < 0) { delay(1); continue; }
            --left;
        }
        if (left > 0) _plain.stop();         // could not get clean: start fresh next time
    } else {
        _plain.stop();
    }

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
