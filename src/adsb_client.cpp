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
    // Bytes actually taken OFF THE SOCKET, which is not the same as bytes handed to the
    // parser: this reads ahead in 1 KB chunks, so at the closing brace the socket sits
    // further along than the parser does. Draining to Content-Length using the parser's
    // count therefore waits for bytes that were already consumed, times out, and drops the
    // connection on EVERY poll — measured as "sockets opened=28 reused=0", i.e. keep-alive
    // never once worked, which is what exhausted the socket table.
    size_t socketBytes() const { return _socketBytes; }

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
                if (got > 0) { _len = (size_t)got; _socketBytes += (size_t)got; return true; }
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
    size_t  _socketBytes = 0;
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
// When each edge may be tried again. A DEAD edge is not the same as a busy one, and the
// pool above treated them identically.
//
// Measured on Zion's Orb, 2026-08-29: api.adsb.lol resolved to three addresses and
// 89.58.34.223 refused to open a socket at all, while the other two answered a laptop on the
// same network in 353 ms. The Orb tried the dead one, waited out the full fifteen-second
// connect, tried one more, gave up, and backed off. Six polls in seventy seconds, none good,
// with a working feed two addresses away. The scope sat there holding aircraft that had
// stopped moving.
//
// Rate limiting is per edge, so a REFUSAL is a reason to ask a different door and come back
// soon. A connect that never opens is a different thing: nothing is listening, and asking
// again in ten seconds only spends the poll. So a failure to connect parks that address for
// a while, and the pool spends its two tries on doors that might open.
uint32_t  s_edgeNextOkMs[ADSB_EDGE_POOL] = { 0 };
// Forty-five seconds, not five minutes. The first version of this rested a failed address
// for five minutes, which is right for an address that is DEAD and wrong for this service,
// which flaps: the same address that refuses now answers in 355 ms a minute later. Resting
// it for five minutes throws away a working door. This is just long enough to stop spending
// both tries of the next poll on the same silent address.
constexpr uint32_t EDGE_COOLDOWN_MS = 45UL * 1000UL;

bool edge_usable(uint8_t i) {
    const uint32_t until = s_edgeNextOkMs[i];
    return until == 0 || (int32_t)(millis() - until) >= 0;
}

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
    if (_plain.connected()) {
        ++_reuseCount;
    } else {
        // Every one of these costs a socket, and a closed TCP socket does not disappear —
        // it sits in TIME_WAIT for a while holding an lwIP control block. That pool is small
        // (order of a dozen), which is why rotating hard across five edges worked for about
        // thirteen polls after a reboot and then failed EVERY connect while the same servers
        // answered a laptop fine. Reuse is not an optimisation here, it is the difference
        // between working and running out of sockets.
        ++_openCount;
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

    // At most TWO edges per poll, never all of them. Trying every edge on every poll is
    // what exhausted the socket table: five fresh connections every ten seconds, each
    // lingering in TIME_WAIT long after it closed. One retry is enough to route around a
    // single rate-limited or sulking server, and the pool still rotates across polls.
    // Walk the WHOLE pool looking for doors that are not on cooldown, but still only knock
    // on ADSB_TRIES_PER_POLL of them: the cap exists because trying every edge every poll is
    // what exhausted the socket table, and that reasoning is unchanged. What changes is that
    // the two tries are no longer spent on an address already known not to answer.
    uint8_t used = 0;
    for (uint8_t step = 0; step < s_edgeN && used < ADSB_TRIES_PER_POLL; ++step) {
        const uint8_t idx = (uint8_t)((s_edgeAt + step) % s_edgeN);
        if (!edge_usable(idx)) continue;
        ++used;
        if (fetchFrom(s_edge[idx], out)) {
            s_edgeAt = idx;                                    // stay on what works
            s_edgeNextOkMs[idx] = 0;                           // and forgive it entirely
            return true;
        }
        // Only a transport failure parks an address. A 429 means the server is there and
        // busy, which is exactly the case the rotation was built for and must stay in it.
        if (_lastStatus <= 0) {
            s_edgeNextOkMs[idx] = millis() + EDGE_COOLDOWN_MS;
            Serial.printf("[adsb] %s did not answer; resting it for %lu min\n",
                          s_edge[idx].toString().c_str(), (unsigned long)(EDGE_COOLDOWN_MS / 60000UL));
        }
    }
    // Every door is on cooldown: forget the cooldowns rather than sit out the outage. Being
    // wrong about one address is recoverable; refusing to try any of them is not.
    if (used == 0) {
        for (uint8_t i = 0; i < s_edgeN; ++i) s_edgeNextOkMs[i] = 0;
        Serial.println("[adsb] every edge was resting; trying them all again");
    }
    s_edgeAt = (uint8_t)((s_edgeAt + 1) % s_edgeN);            // rotate for next time
    Serial.printf("[adsb] poll gave up after %u edge(s); sockets opened=%lu reused=%lu\n",
                  (unsigned)used, (unsigned long)_openCount, (unsigned long)_reuseCount);
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
        long left = contentLen - (long)jsonStream.socketBytes();
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
