#include "roads_download.h"
#ifdef ARDUINO
#include "road_tile_builder.h"
#include "road_download_stream.h"
#include "road_tile_plan.h"
#include "road_tile_clip.h"
#include "roads_sd.h"
#include "sdcard.h"
#include "theme_sd.h"
#include "config.h"
#include <Arduino.h>
#include <SD.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <StreamUtils.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <new>

namespace roads_download {
namespace {
// A failed public query is retried slowly, never in a boot-blocking loop.
constexpr uint32_t RETRY_MS = 60000;
constexpr uint32_t MAX_RESPONSE = 8 * 1024 * 1024;
constexpr const char *LENGTHS = "/roads/_download.len";
constexpr const char *POINTS = "/roads/_download.pts";
constexpr const char *PART = "/roads/_download.part";
struct Packet { uint16_t kind, size; uint8_t data[512]; }; // 0=length, 1=points, 2=success, 3=error
QueueHandle_t queue = nullptr;
std::atomic<bool> cancelled {false}, running {false};
std::atomic<int> lastHttp {0}, lastLength {-1};
std::atomic<unsigned> lastBytes {0}, lastJsonPeak {0};
std::atomic<unsigned> lastClass {0};
std::atomic<unsigned> headerMs {0}, bodyMs {0}, receivedRoads {0};
std::atomic<unsigned> transportTimeMs {0}, idleTimeMs {0}, processingTimeMs {0};
std::atomic<unsigned> queueTimeUs {0}, transportReads {0}, transportPolls {0};
uint32_t requestStarted = 0, finishedMs = 0;
uint32_t planStarted = 0, firstSection = 0;
constexpr const char *ROAD_CLASSES[] = { "motorway", "trunk" };
char tileName[32] = "";
int tileLat = 0, tileLon = 0, tileSpan = 1;
const char *state = "idle", *failure = "";
File lengths, points, part, source;
uint16_t numPolys = 0;
uint32_t numPoints = 0, nextScan = 0, notBefore = 0;
unsigned combinePhase = 0;
struct Attempt { uint32_t retry = 0; unsigned count = 0; bool done = false; } attempts[road_tile_plan::MAX_CELLS * 2];
road_tile_plan::Cell cells[road_tile_plan::MAX_CELLS];
unsigned cellCount = 0, cursor = 0, activeIndex = 0, completed = 0;
double planLat = 999, planLon = 999;
float planRange = -1;
bool exhausted = false;
bool browserPaused = false;

struct CardGuard {
    bool held = theme_sd::try_lock();
    ~CardGuard() { if (held) theme_sd::unlock(); }
};
void *json_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void *json_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

bool send(Packet &p) {
    const uint32_t start = millis();
    while (!cancelled && millis() - start < 30000) {
        const uint32_t waitStarted = micros();
        const bool sent = xQueueSend(queue, &p, pdMS_TO_TICKS(100)) == pdTRUE;
        if (p.kind <= 1) queueTimeUs.fetch_add((uint32_t)(micros() - waitStarted));
        if (sent) return true;
    }
    return false;
}
struct Sink {
    unsigned polylines = 0;
    Packet lengthsPacket {}, pointsPacket {};
    Sink() { lengthsPacket.kind = 0; pointsPacket.kind = 1; }
    bool flush() {
        if (lengthsPacket.size && !send(lengthsPacket)) return false;
        lengthsPacket.size = 0;
        if (pointsPacket.size && !send(pointsPacket)) return false;
        pointsPacket.size = 0;
        return true;
    }
    bool line(const int16_t *coords, unsigned n) {
        if (++polylines > road_tile_builder::MAX_POLYS) return false;
        receivedRoads = polylines;
        if (lengthsPacket.size == sizeof(lengthsPacket.data)) {
            if (!send(lengthsPacket)) return false;
            lengthsPacket.size = 0;
        }
        lengthsPacket.data[lengthsPacket.size++] = n & 255;
        lengthsPacket.data[lengthsPacket.size++] = n >> 8;
        for (unsigned i = 0; i < n * 2; ++i) {
            if (pointsPacket.size == sizeof(pointsPacket.data)) {
                if (!send(pointsPacket)) return false;
                pointsPacket.size = 0;
            }
            const uint16_t v = (uint16_t)coords[i];
            pointsPacket.data[pointsPacket.size++] = v & 255;
            pointsPacket.data[pointsPacket.size++] = v >> 8;
        }
        return true;
    }
};
struct ClippedSink {
    Sink &sink;
    bool line(const int16_t *p, unsigned n) { return road_tile_plan::clip(p, n, tileLat, tileLon, sink, tileSpan); }
};
// Only ask readBytes() for bytes reported available. Retry transient TLS reads
// in BufferedInput instead of treating the first empty read as the end of JSON.
struct Transport {
    Stream &body;
    WiFiClient &client;
    ChunkDecodingStream *chunks;
    int length;
    unsigned received = 0;
    int available() { return body.available(); }
    bool connected() { return client.connected(); }
    bool finished() {
        return chunks ? chunks->ended() :
               length >= 0 ? received == (unsigned)length : !client.connected() && !body.available();
    }
    bool failed() { return chunks && chunks->error(); }
    bool cancelled() { return roads_download::cancelled.load(); }
    int read(uint8_t *dst, size_t n) {
        const size_t got = body.readBytes(dst, n);
        if (got > n) return -1;
        received += got;
        lastBytes = received;
        return (int)got;
    }
};
struct Clock {
    uint32_t now() { return millis(); }
    uint32_t micros() { return ::micros(); }
    void pause() { delay(1); }
};

void worker(void *) {
    const char *error = "map download failed";
    char detail[160] = {};
    char serverRemark[160] = {};
    bool ok = false;
    {
        auto scratch = (road_tile_builder::Scratch *)heap_caps_malloc(sizeof(road_tile_builder::Scratch), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!scratch) error = "not enough PSRAM for road generation";
        else {
            Sink sink;
            ClippedSink clipped {sink};
            {
                const unsigned pass = lastClass.load();
                ok = false;
                lastClass = pass; lastHttp = 0; lastLength = -1;
                WiFiClientSecure client;
                client.setInsecure(); // matches existing firmware HTTPS clients
                client.setTimeout(10000);
                client.setHandshakeTimeout(10);
                HTTPClient http;
                http.setReuse(false);
                http.setConnectTimeout(5000);
                // HTTPClient uses this timeout while waiting for RESPONSE HEADERS too.
                // Overpass may queue for ~15s and then execute the query. Its uint16_t
                // timeout cannot exceed 65535ms: allow 45s execution plus that queue time.
                http.setTimeout(65000);
                const char *headers[] = { "Transfer-Encoding" };
                http.collectHeaders(headers, 1);
                // maxsize limits the SERVER's query memory, not the Orb's RAM.
                // 32 MB failed for Provo before any geometry was returned. Keep the
                // on-device 96 KB JSON and 8 MB response limits independent of this.
                char query[256];
                snprintf(query, sizeof(query),
                         "[out:json][timeout:45][maxsize:134217728];way[highway=\"%s\"](%.2f,%.2f,%.2f,%.2f);out skel geom;",
                         ROAD_CLASSES[pass], tileLat / 4.0, tileLon / 4.0, (tileLat + tileSpan) / 4.0, (tileLon + tileSpan) / 4.0);
                if (http.begin(client, ROAD_OVERPASS_URL)) {
                    http.setUserAgent(ORB_USER_AGENT);
                    http.addHeader("Accept", "application/json");
                    http.setAcceptEncoding("identity");
                    http.addHeader("Content-Type", "text/plain");
                    const int code = http.POST((uint8_t *)query, strlen(query));
                    headerMs = millis() - requestStarted;
                    lastHttp = code; lastLength = http.getSize();
                    Serial.printf("[roads_fetch] HTTP %d for %s, length=%d internal_largest=%u psram_free=%u\n",
                                  code, tileName, http.getSize(),
                                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
                    if (code == 200 && http.getSize() <= (int)MAX_RESPONSE) {
                        http.setTimeout(10000); // body reads have a separate idle timeout
                        WiFiClient *raw = http.getStreamPtr();
                        raw->setTimeout(10000);
                        ChunkDecodingStream chunked(*raw);
                        const bool chunks = http.header("Transfer-Encoding").equalsIgnoreCase("chunked");
                        Stream &decoded = chunks ? static_cast<Stream &>(chunked) : static_cast<Stream &>(*raw);
                        decoded.setTimeout(10000);
                        Transport transport { decoded, client, chunks ? &chunked : nullptr, http.getSize() };
                        Clock clock;
                        BufferedInput<Transport, Clock> input(transport, clock, 10000, 150000, MAX_RESPONSE);
                        road_tile_builder::BoundedAllocator allocator(96 * 1024, json_alloc, json_realloc, heap_caps_free);
                        const uint32_t bodyStarted = micros();
                        ok = road_tile_builder::generate(input, clipped, *scratch, &allocator, error,
                                                         serverRemark, sizeof(serverRemark));
                        if (allocator.peak() > lastJsonPeak) lastJsonPeak = allocator.peak();
                        if (input.error) { ok = false; error = input.error; }
                        if (!ok) {
                            snprintf(detail, sizeof(detail), "%s; tile=%s bytes=%lu JSONpeak=%u",
                                     error ? error : "invalid map response", tileName,
                                     (unsigned long)input.bytes, (unsigned)allocator.peak());
                            error = detail;
                        }
                        if (ok && ((http.getSize() >= 0 && input.bytes != (uint32_t)http.getSize()) ||
                                   (chunks && (!chunked.ended() || chunked.error())) || cancelled)) {
                            ok = false; error = "incomplete map response";
                        }
                        if (ok && !sink.flush()) { ok = false; error = "map output cancelled"; }
                        const uint32_t totalUs = micros() - bodyStarted;
                        bodyMs = totalUs / 1000;
                        transportTimeMs = (unsigned)(input.transportUs / 1000);
                        idleTimeMs = (unsigned)(input.idleUs / 1000);
                        transportReads = input.readCalls; transportPolls = input.emptyPolls;
                        const uint64_t accounted = input.transportUs + queueTimeUs.load();
                        processingTimeMs = totalUs > accounted ? (unsigned)((totalUs - accounted) / 1000) : 0;
                    } else {
                        if (code < 0) {
                            char tls[80] = {};
                            const int tlsCode = client.lastError(tls, sizeof(tls));
                            if (tlsCode)
                                snprintf(detail, sizeof(detail), "HTTP transport %d (%s); TLS %d: %.70s",
                                         code, HTTPClient::errorToString(code).c_str(), tlsCode, tls);
                            else
                                snprintf(detail, sizeof(detail), "HTTP transport %d: %s",
                                         code, HTTPClient::errorToString(code).c_str());
                        } else if (code != 200) {
                            snprintf(detail, sizeof(detail), "Overpass HTTP %d%s", code,
                                     code == 429 ? ": rate limited" :
                                     code == 504 ? ": query timed out or server busy" :
                                     code == 400 ? ": query rejected" : "");
                        } else {
                            snprintf(detail, sizeof(detail), "Overpass response %d bytes exceeds %lu-byte limit",
                                     http.getSize(), (unsigned long)MAX_RESPONSE);
                        }
                        error = detail;
                    }
                    http.end();
                } else error = "cannot initialize Overpass HTTPS request";
            }
            Serial.printf("[roads_fetch] %s %s: headers=%ums body=%ums bytes=%u roads=%u ok=%d\n",
                          tileName, ROAD_CLASSES[lastClass.load()], headerMs.load(), bodyMs.load(),
                          lastBytes.load(), sink.polylines, (int)ok);
            Serial.printf("[roads_fetch] body profile: transport=%ums (idle=%ums), processing=%ums, queue=%ums, reads=%u polls=%u\n",
                          transportTimeMs.load(), idleTimeMs.load(), processingTimeMs.load(), queueTimeUs.load()/1000,
                          transportReads.load(), transportPolls.load());
            heap_caps_free(scratch);
        }
    }
    Packet done {}; done.kind = ok ? 2 : 3;
    if (!ok) {
        strlcpy((char *)done.data, error ? error : "map download failed", sizeof(done.data));
        done.size = strlen((char *)done.data) + 1;
    }
    send(done);
    running = false;
    vTaskDelete(nullptr);
}

char errorText[160] = "";
void cleanup() {
    lengths.close(); points.close(); source.close(); part.close();
    SD.remove(LENGTHS); SD.remove(POINTS); SD.remove(PART);
    if (queue) { vQueueDelete(queue); queue = nullptr; }
    combinePhase = 0;
}
void fail(const char *why) {
    strlcpy(errorText, why, sizeof(errorText)); failure = errorText;
    Serial.printf("[roads_fetch] %s: %s; retry with backoff (max 3 attempts per area/class)\n", tileName, failure);
    cancelled = true;
    state = "failed";
    // The worker may still own the queue. Only step() cleans it after running=false.
    finishedMs = millis() - requestStarted;
    nextScan = millis() + (lastHttp == 429 ? 60000 : 10000);
    notBefore = nextScan;
}
bool cached(const char *path) {
    File f = SD.open(path, FILE_READ);
    uint8_t h[6];
    if (!f || f.read(h, 6) != 6) return false;
    const unsigned polys = h[0] | (h[1] << 8);
    const uint32_t pts = h[2] | ((uint32_t)h[3] << 8) | ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
    return polys <= road_tile_builder::MAX_POLYS && pts <= 2 * 1024 * 1024 &&
           pts >= polys * 2 && f.size() == 6 + polys * 2 + pts * 4;
}
void pathFor(char *path, size_t size, int lat, int lon) {
    road_tile_plan::path(path, size, lat, lon, lastClass.load(), tileSpan);
}
void start(int lat, int lon, unsigned roadClass, int span) {
    tileLat = lat; tileLon = lon; tileSpan = span;
    lastClass = roadClass;
    char path[48]; pathFor(path, sizeof(path), lat, lon);
    snprintf(tileName, sizeof(tileName), "%.*s", (int)strlen(path)-11, path+7);
    requestStarted = millis(); finishedMs = 0;
    notBefore = requestStarted + 10000;
    cleanup();
    if (!SD.exists("/roads") && !SD.mkdir("/roads")) { fail("cannot create /roads"); return; }
    lengths = SD.open(LENGTHS, FILE_WRITE); points = SD.open(POINTS, FILE_WRITE);
    if (!lengths || !points) { fail("cannot create temporary map files"); cleanup(); return; }
    queue = xQueueCreate(4, sizeof(Packet));
    if (!queue) { fail("not enough memory for map queue"); cleanup(); return; }
    numPolys = 0; numPoints = 0; cancelled = false; running = true; failure = "";
    lastHttp = 0; lastLength = -1;
    lastBytes = 0; lastJsonPeak = 0; headerMs = 0; bodyMs = 0; receivedRoads = 0;
    transportTimeMs = 0; idleTimeMs = 0; processingTimeMs = 0;
    queueTimeUs = 0; transportReads = 0; transportPolls = 0;
    state = "fetching";
    if (xTaskCreatePinnedToCore(worker, "roads_fetch", 12288, nullptr, 1, nullptr, 0) != pdPASS) {
        running = false; fail("cannot start map worker"); cleanup(); return;
    }
    Serial.printf("[roads_fetch] generating %s on device\n", tileName);
}
} // namespace

void browserPause(bool paused) {
    if (browserPaused == paused) return;
    browserPaused = paused;
    if (paused && queue) cancelled = true;
    if (!paused) {
        cursor = 0; nextScan = 0; exhausted = false;
        for (auto &a : attempts) if (!a.done) { a.count = 0; a.retry = 0; }
    }
}
bool browserReady() { return !queue && !running; }
const char *status() { return browserPaused ? "preloading" : state; }
const char *lastError() { return browserPaused ? "" : failure; }
int httpStatus() { return lastHttp.load(); }
int responseLength() { return lastLength.load(); }
const char *tile() { return tileName; }
unsigned bytesRead() { return lastBytes.load(); }
unsigned jsonPeak() { return lastJsonPeak.load(); }
const char *roadClass() { return ROAD_CLASSES[lastClass.load()]; }
unsigned elapsedMs() { return queue ? millis() - requestStarted : finishedMs; }
unsigned headersMs() { return headerMs.load(); }
unsigned downloadMs() { return bodyMs.load(); }
unsigned transportMs() { return transportTimeMs.load(); }
unsigned idleMs() { return idleTimeMs.load(); }
unsigned processingMs() { return processingTimeMs.load(); }
unsigned queueWaitMs() { return queueTimeUs.load()/1000; }
unsigned readCalls() { return transportReads.load(); }
unsigned emptyPolls() { return transportPolls.load(); }
unsigned roadsReceived() { return receivedRoads.load(); }
unsigned completedAreas() { return completed; }
unsigned totalAreas() { return cellCount * 2; }
unsigned firstSectionMs() { return firstSection; }
void progressText(char *out, unsigned size) {
    if (!size) return;
    out[0] = 0;
    if (browserPaused) { snprintf(out, size, "Preloading maps from browser"); return; }
    if (!cellCount || completed == cellCount * 2) return;
    if (!sdcard::mounted()) { snprintf(out, size, "Insert SD card for road maps"); return; }
    if (WiFi.status() != WL_CONNECTED) { snprintf(out, size, "Connect Wi-Fi to download roads"); return; }
    if (queue && cancelled) { snprintf(out, size, "Updating map location"); return; }
    if (!strcmp(state, "fetching")) {
        if (lastBytes)
            snprintf(out, size, "Downloading roads: %u KB\n%u/%u areas saved", (lastBytes.load()+1023)/1024, completed, cellCount*2);
        else snprintf(out, size, "Waiting for map service: %us\n%u/%u areas saved", elapsedMs()/1000, completed, cellCount*2);
    } else if (!strcmp(state, "saving")) snprintf(out, size, "Adding roads to map\n%u/%u areas saved", completed, cellCount*2);
    else snprintf(out, size, "%s\n%u/%u areas saved", exhausted ? "Map service unavailable" :
                  !strcmp(state, "failed") || !strcmp(state, "waiting") ? "Roads pending; retrying shortly" : "Loading nearby roads", completed, cellCount*2);
}

void step(double lat, double lon, float rangeKm, bool enabled, bool transferBusy) {
    // Pause SD work during installs. Backpressure stops the worker; after 30s it gives
    // up and the partial tile is discarded. No other task ever writes map files.
    if (transferBusy) return;
    CardGuard guard;
    if (!guard.held) return;
    if (!sdcard::mounted() && queue && !cancelled) fail("SD card unavailable");
    if (queue && (!enabled || WiFi.status() != WL_CONNECTED)) cancelled = true;
    if (queue && cancelled) {
        if (!running) { if (strcmp(state, "failed")) fail("map request cancelled"); cleanup(); }
        return;
    }
    if (browserPaused) return;
    if (!sdcard::mounted()) return;
    const bool changed = lat != planLat || lon != planLon || rangeKm != planRange;
    if (changed && queue) {
        cancelled = true;
        if (!running) cleanup();
        return;
    }
    if (changed) {
        cellCount = road_tile_plan::build(lat, lon, rangeKm, cells);
        planLat = lat; planLon = lon; planRange = rangeKm;
        for (auto &a : attempts) a = Attempt {};
        completed = cursor = 0; exhausted = false; nextScan = notBefore; state = "idle";
        planStarted = millis(); firstSection = 0; failure = "";
    }
    if (queue && combinePhase == 0) {
        Packet p;
        // At most 2 KB of output per loop; SD writes are protected against audio's reader.
        for (unsigned i = 0; i < 4 && xQueueReceive(queue, &p, 0) == pdTRUE; ++i) {
            if (p.kind == 0) {
                if (lengths.write(p.data, p.size) != p.size) { fail("SD length write failed"); return; }
                for (unsigned j = 0; j < p.size; j += 2) {
                    ++numPolys; numPoints += p.data[j] | (p.data[j+1] << 8);
                }
            } else if (p.kind == 1) {
                if (points.write(p.data, p.size) != p.size) { fail("SD point write failed"); return; }
            } else if (p.kind == 3) { fail((char *)p.data); return; }
            else {
                lengths.close(); points.close();
                part = SD.open(PART, FILE_WRITE);
                uint8_t h[6] = { (uint8_t)numPolys, (uint8_t)(numPolys >> 8),
                    (uint8_t)numPoints, (uint8_t)(numPoints >> 8), (uint8_t)(numPoints >> 16), (uint8_t)(numPoints >> 24) };
                if (!part || part.write(h, 6) != 6) { fail("SD header write failed"); return; }
                source = SD.open(LENGTHS, FILE_READ);
                if (!source) { fail("SD length read failed"); return; }
                combinePhase = 1; state = "saving";
                break;
            }
        }
        if (combinePhase == 0 && !running && uxQueueMessagesWaiting(queue) == 0) fail("map worker stopped without a complete tile");
        return;
    }
    if (queue && combinePhase) {
        uint8_t buf[2048];
        const size_t want = source.size() - source.position();
        if (want) {
            const size_t n = source.read(buf, want < sizeof(buf) ? want : sizeof(buf));
            if (!n || part.write(buf, n) != n) fail("SD tile assembly failed");
            return;
        }
        source.close();
        if (combinePhase == 1) {
            source = SD.open(POINTS, FILE_READ);
            if (!source || source.size() != numPoints * 4) { fail("SD geometry size mismatch"); return; }
            combinePhase = 2; return;
        }
        if (running) return; // never free a queue the worker could still be using
        part.close();
        char path[48]; pathFor(path, sizeof(path), tileLat, tileLon);
        if (!cached(PART)) { fail("generated tile failed validation"); return; }
        // Only missing/invalid tiles are generated. Don't replace a valid tile installed
        // while the query was in flight. A reset during generation leaves no final file.
        if (!cached(path)) {
            if (SD.exists(path)) SD.remove(path);
            if (!SD.rename(PART, path)) { fail("cannot publish generated tile"); return; }
            roads_sd::tiles_changed();
        }
        Serial.printf("[roads_fetch] cached %s: %u roads, %lu points\n", path, numPolys, (unsigned long)numPoints);
        attempts[activeIndex].done = true; ++completed;
        if (!firstSection) firstSection = millis() - planStarted;
        finishedMs = millis() - requestStarted;
        Serial.printf("[roads_fetch] %s published in %ums; coverage %u/%u\n", tileName, finishedMs, completed, cellCount*2);
        cleanup(); state = "ready";
        // Space request STARTS by ten seconds; slow requests already paid this gap.
        nextScan = requestStarted + 10000;
        cursor = 0;
        return;
    }
    if (!enabled || WiFi.status() != WL_CONNECTED || !std::isfinite(lat) || !std::isfinite(lon) ||
        lat < -90 || lat >= 90 || lon < -180 || lon >= 180 || !std::isfinite(rangeKm) || rangeKm <= 0 ||
        (int32_t)(millis() - nextScan) < 0) return;
    // Bound cache inspection too: a large range can contain hundreds of class files.
    for (unsigned checked = 0; checked < 4 && cursor < cellCount*2; ++checked) {
        const unsigned index = cursor++;
        Attempt &a = attempts[index];
        if (a.done) continue;
        const auto &cell = cells[index/2];
        char path[48];
        snprintf(path, sizeof(path), "/roads/r%d_%d.bin", road_tile_plan::degree(cell.lat), road_tile_plan::degree(cell.lon));
        bool exists = cached(path); // complete old tiles cover both classes
        if (!exists) {
            road_tile_plan::path(path, sizeof(path), road_tile_plan::degree(cell.lat)*4,
                                 road_tile_plan::degree(cell.lon)*4, index%2, 4);
            exists = cached(path);
        }
        if (!exists && cell.span == 1) { road_tile_plan::path(path, sizeof(path), cell.lat, cell.lon, index%2); exists = cached(path); }
        if (exists) { a.done = true; ++completed; continue; }
        if (a.count >= 3 || (a.count && (int32_t)(millis() - a.retry) < 0)) continue;
        activeIndex = index;
        a.retry = millis() + (RETRY_MS << a.count); ++a.count;
        start(cell.lat, cell.lon, index%2, cell.span); return;
    }
    if (cursor >= cellCount*2) {
        cursor = 0; nextScan = millis() + 1000;
        exhausted = completed < cellCount*2;
        for (unsigned i = 0; i < cellCount*2; ++i)
            if (!attempts[i].done && attempts[i].count < 3) exhausted = false;
        state = completed == cellCount*2 ? "ready" : exhausted ? "failed" : "waiting";
    }
}
} // namespace roads_download
#endif
