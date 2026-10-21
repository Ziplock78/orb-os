#include "road_preload.h"
#ifdef ARDUINO
#include <Arduino.h>
#include "road_preload_page.h"
#include "road_preload_js.h"
#include "road_tile_plan.h"
#include "road_tile_upload.h"
#include "roads_download.h"
#include "roads_sd.h"
#include "sdcard.h"
#include "theme_sd.h"
#include <ArduinoJson.h>
#include <SD.h>
#include <WebServer.h>
#include <esp_random.h>

namespace road_preload {
namespace {
WebServer *web;
String token;
uint32_t touched = 0;
bool otherBusy = false;
struct Target { int lat, lon, span; unsigned roadClass; } targets[180];
unsigned targetCount = 0;
File upload;
unsigned uploadIndex = 0;
size_t uploaded = 0;
const char *uploadError = nullptr;
bool uploadComplete = false;
bool closePending = false, uploadRetry = false;
constexpr const char *TEMP = "/roads/_browser.part";
struct Guard {
    bool held = theme_sd::try_lock();
    ~Guard() { if (held) theme_sd::unlock(); }
};
void error(int status, const char *message) { web->send(status, "text/plain", message); }
bool authorized() { return token.length() && web->arg("token") == token; }
bool ready() { return roads_download::browserReady() && !otherBusy; }
bool requireSession() {
    if (!authorized()) { error(403, "Preload session expired; start again"); return false; }
    touched = millis();
    if (!ready()) { error(409, "Waiting for device downloads or another SD transfer to stop"); return false; }
    return true;
}
void path(char *out, size_t size, const Target &t) {
    road_tile_plan::path(out, size, t.lat, t.lon, t.roadClass, t.span);
}
bool cached(const char *name) {
    File f = SD.open(name, FILE_READ);
    uint8_t h[6];
    if (!f || f.read(h, 6) != 6) return false;
    const uint32_t p = road_tile_upload::u16(h+2) | ((uint32_t)road_tile_upload::u16(h+4) << 16);
    const unsigned n = road_tile_upload::u16(h);
    return n <= 16000 && p >= n*2 && p <= 2*1024*1024 && f.size() == 6+n*2+p*4;
}
bool covered(const Target &t) {
    char name[48];
    snprintf(name, sizeof(name), "/roads/r%d_%d.bin", road_tile_plan::degree(t.lat), road_tile_plan::degree(t.lon));
    if (cached(name)) return true;
    road_tile_plan::path(name, sizeof(name), road_tile_plan::degree(t.lat)*4,
                         road_tile_plan::degree(t.lon)*4, t.roadClass, 4);
    if (cached(name)) return true;
    path(name, sizeof(name), t);
    return cached(name);
}
void sessionReply() {
    JsonDocument doc;
    doc["token"] = token; doc["ready"] = ready();
    String body; serializeJson(doc, body); web->send(200, "application/json", body);
}
void release() {
    Guard guard;
    closePending = !guard.held;
    if (guard.held) { upload.close(); if (sdcard::mounted()) SD.remove(TEMP); }
    token = ""; targetCount = 0;
    uploadComplete = false;
    roads_download::browserPause(false);
}
void plan() {
    if (!requireSession()) return;
    if (!sdcard::mounted()) { error(409, "Insert an SD card first"); return; }
    if (web->arg("plain").length() > 4096) { error(413, "Too many locations"); return; }
    JsonDocument request;
    if (deserializeJson(request, web->arg("plain"))) { error(400, "Invalid locations"); return; }
    JsonArrayConst locations = request["locations"].as<JsonArrayConst>();
    const double range = request["rangeKm"] | 0.0;
    if (!locations.size() || locations.size() > 9 || !std::isfinite(range) || range <= 0 || range > 1000) {
        error(400, "Select 1 to 9 locations and a valid range"); return;
    }
    targetCount = 0;
    for (JsonObjectConst location : locations) {
        if (!location["lat"].is<double>() || !location["lon"].is<double>()) { error(400, "Invalid coordinates"); return; }
        road_tile_plan::Cell cells[road_tile_plan::MAX_CELLS];
        const unsigned n = road_tile_plan::build(location["lat"], location["lon"], range, cells);
        if (!n) { error(400, "Invalid coordinates"); return; }
        for (unsigned i = 0; i < n; ++i) for (unsigned roadClass = 0; roadClass < 2; ++roadClass) {
            const Target t {cells[i].lat, cells[i].lon, cells[i].span, roadClass};
            bool duplicate = false;
            for (unsigned j = 0; j < targetCount; ++j) {
                const auto &old = targets[j];
                if (old.roadClass != t.roadClass) continue;
                if (old.lat == t.lat && old.lon == t.lon && old.span == t.span) duplicate = true;
                // Earlier full cells also cover later selected cities' starter cells.
                if (t.span == 1 && old.span == 4 && road_tile_plan::degree(old.lat) == road_tile_plan::degree(t.lat) &&
                    road_tile_plan::degree(old.lon) == road_tile_plan::degree(t.lon)) duplicate = true;
            }
            if (!duplicate) targets[targetCount++] = t;
        }
    }
    JsonDocument response; JsonArray tiles = response["tiles"].to<JsonArray>();
    {
        Guard guard;
        if (!guard.held) { error(503, "SD card busy; try again shortly"); return; }
        for (unsigned i = 0; i < targetCount; ++i) {
            const auto &t = targets[i]; char name[48]; path(name, sizeof(name), t);
            JsonObject row = tiles.add<JsonObject>();
            row["id"] = i; row["path"] = name; row["lat"] = t.lat; row["lon"] = t.lon;
            row["span"] = t.span; row["roadClass"] = t.roadClass ? "trunk" : "motorway";
            row["cached"] = covered(t);
        }
    }
    String body; serializeJson(response, body); web->send(200, "application/json", body);
}
void receive() {
    HTTPUpload &up = web->upload();
    if (up.status == UPLOAD_FILE_START) {
        uploadError = nullptr; uploaded = 0; uploadComplete = false; uploadRetry = false;
        if (!authorized() || !ready()) { uploadError = "Preload session is not ready"; return; }
        touched = millis();
        const String id = web->arg("id");
        if (!id.length() || id.length() > 3) { uploadError = "Invalid tile ID"; return; }
        for (unsigned i = 0; i < id.length(); ++i) if (!isDigit(id[i])) { uploadError = "Invalid tile ID"; return; }
        uploadIndex = id.toInt();
        if (uploadIndex >= targetCount) { uploadError = "Unknown tile ID"; return; }
        Guard guard;
        if (!guard.held) { uploadError = "SD card busy; retry this section"; uploadRetry = true; closePending = true; return; }
        upload.close();
        closePending = false;
        if (!sdcard::mounted() || (!SD.exists("/roads") && !SD.mkdir("/roads"))) { uploadError = "SD card unavailable"; return; }
        upload = SD.open(TEMP, FILE_WRITE);
        if (!upload) uploadError = "Cannot create temporary tile";
    } else if (up.status == UPLOAD_FILE_WRITE && !uploadError) {
        touched = millis();
        if (!authorized() || !ready()) { uploadError = "Preload interrupted by another transfer"; return; }
        if (!upload || uploaded + up.currentSize > road_tile_upload::MAX_BYTES) { uploadError = "Tile exceeds upload limit"; return; }
        Guard guard;
        if (!guard.held) { uploadError = "SD card busy; retry this section"; uploadRetry = true; closePending = true; return; }
        if (upload.write(up.buf, up.currentSize) != up.currentSize) uploadError = "SD write failed";
        uploaded += up.currentSize;
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        Guard guard;
        closePending = !guard.held;
        if (guard.held) { upload.close(); SD.remove(TEMP); }
        uploadError = "Upload interrupted";
    } else if (up.status == UPLOAD_FILE_END) {
        Guard guard;
        if (!guard.held) { uploadError = "SD card busy; retry this section"; uploadRetry = true; closePending = true; return; }
        upload.close(); closePending = false;
        if (!authorized() || !ready()) uploadError = "Preload session is not ready";
        if (!uploadError) {
            auto &t = targets[uploadIndex];
            File f = SD.open(TEMP, FILE_READ);
            if (!f || !road_tile_upload::validate(f, f.size(), t.lat, t.lon, t.span)) uploadError = "Invalid road tile";
            f.close();
            if (!uploadError) {
                char name[48]; path(name, sizeof(name), t);
                if (!covered(t)) {
                    if (SD.exists(name)) SD.remove(name);
                    if (!SD.rename(TEMP, name)) uploadError = "Cannot publish road tile";
                    else { roads_sd::tiles_changed(); Serial.printf("[roads_browser] saved %s (%u bytes)\n", name, (unsigned)uploaded); }
                }
            }
        }
        SD.remove(TEMP);
        uploadComplete = !uploadError;
    }
}
}
void begin(WebServer &server) {
    web = &server;
    web->on("/roads", HTTP_GET, [] { web->send_P(200, "text/html", ROAD_PRELOAD_PAGE); });
    web->on("/roads/processor.js", HTTP_GET, [] { web->send_P(200, "application/javascript", ROAD_PRELOAD_JS); });
    web->on("/roads/session", HTTP_POST, [] {
        if (!sdcard::mounted()) { error(409, "Insert an SD card first"); return; }
        // A delayed heartbeat after release/expiry must not acquire a new lease
        // whose token the old page never learns.
        if (!token.length() && web->hasArg("token")) { error(403, "Preload session expired; start again"); return; }
        if (token.length() && !authorized()) { error(409, "Another browser is preloading maps"); return; }
        if (!token.length()) { token = String(esp_random(), HEX) + String(esp_random(), HEX); roads_download::browserPause(true); }
        touched = millis(); sessionReply();
    });
    web->on("/roads/release", HTTP_POST, [] {
        if (!authorized()) { error(403, "Unknown preload session"); return; }
        release(); web->send(200, "text/plain", "ok");
    });
    web->on("/roads/plan", HTTP_POST, plan);
    web->on("/roads/tile", HTTP_POST, [] {
        if (uploadError) { error(uploadRetry ? 503 : 400, uploadError); return; }
        if (!authorized() || !uploadComplete) { error(400, "No completed tile upload"); return; }
        uploadComplete = false;
        web->send(200, "text/plain", "ok");
    }, receive);
}
void step(bool otherTransferBusy) {
    otherBusy = otherTransferBusy;
    if (closePending) {
        Guard guard;
        if (guard.held) { upload.close(); if (sdcard::mounted()) SD.remove(TEMP); closePending = false; }
    }
    if (token.length() && millis()-touched > 45000) release();
}
}
#endif
