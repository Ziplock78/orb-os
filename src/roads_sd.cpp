#include "roads_sd.h"
#include "coastline.h"   // geo_project_polylines_flat() — the shared projection/clip math
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "sdcard.h"
#else
#include <string>
#endif

namespace {

// ---------------------------------------------------------------------------
// Why this streams instead of reading whole tiles:
//
// The device runs with heap_caps_malloc_extmem_enable(4096), so nearly every
// large allocation in the whole system (WiFi/TLS, LVGL, weather frames, the
// baked plate+overlay) lives in the 8MB PSRAM — and by the time Flight Tracker
// is running, PSRAM is almost full (measured ~118KB free, largest block ~80KB).
// A dense metro tile is 95-110KB, so malloc-ing the whole tile mid-session
// FAILS (that's why the big tiles silently dropped while the tiny neighbor
// tiles squeaked through). Two earlier attempts (loop the read, bounce through
// internal RAM) didn't help because the failure was the allocation, not the
// read.
//
// So: allocate NOTHING per tile. All buffers are reserved once at boot (init(),
// while PSRAM is still whole) and reused. Each tile is streamed from the card a
// few KB at a time and projected one polyline at a time straight into the
// output cache. Peak extra memory is a fixed ~230KB reserved up front, with no
// mid-session allocation that can fail. If even the boot reservation fails
// (PSRAM tight), project() leaves the cache empty and draw() paints nothing —
// the scope just loses its roads, never a crash.
// ---------------------------------------------------------------------------

void *psram_alloc(size_t n) {
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

// Output cache: the in-view (clipped-to-scope-circle) screen-space polylines for
// the current scope. Sized generously so a dense metro at a wide range doesn't
// truncate; beyond the cap, projection just stops (roads thin out, no crash).
constexpr size_t MAX_PTS   = 32000;   // output points  (~128KB)
constexpr size_t MAX_POLYS = 15000;   // output polylines (~30KB)
// Per-tile scratch, reused for every tile (never the whole tile at once):
constexpr size_t MAX_TILE_POLYS = 16000;  // a tile's polyLen[] table (~32KB)
constexpr size_t MAX_LINE_PTS   = 2048;   // points in one polyline before project (~8KB)

// WHERE a projection is being accumulated. project() aims this at the module's own cache;
// project_flat() aims it at the caller's buffers. One streamer, two destinations, so the
// flight scope and the weather map cannot drift in what they read off the same card.
struct Sink {
    lv_point_t *pts;
    uint16_t   *polyLen;
    size_t      maxPts;
    size_t      maxPolys;
    size_t      numPts   = 0;
    size_t      numPolys = 0;
};

// Both callers -- the scope, and the weather map -- run on the UI thread, and they must
// keep doing so: this reads the SD card, and the Arduino SD driver cannot be called from two
// tasks at once. read_exact() also funnels every read through one shared 4 KB static and
// copies out of it, so two readers here would quietly hand each other's bytes back as road
// geometry even if the driver survived it.
//
// The lock is not what makes that safe; being on one thread is. It is here so that a caller
// added later on another task degrades to "no roads this cycle" -- the same answer this
// module already gives for a missing tile or tight PSRAM -- instead of corrupting the map or
// deadlocking the card. The timeout means it can never become a place to hang.
#ifdef ARDUINO
SemaphoreHandle_t s_lock = nullptr;
bool lock()   { if (!s_lock) return true; return xSemaphoreTake(s_lock, pdMS_TO_TICKS(250)) == pdTRUE; }
void unlock() { if (s_lock) xSemaphoreGive(s_lock); }
#else
bool lock()   { return true; }   // the simulator projects from one thread
void unlock() {}
#endif

lv_point_t *s_pts      = nullptr;   // output points  (PSRAM, boot-reserved)
uint16_t   *s_polyLen  = nullptr;   // output polyline lengths (PSRAM)
uint16_t   *s_tPolyLen = nullptr;   // current tile's polyLen[] table (PSRAM)
int16_t    *s_tPts     = nullptr;   // one polyline's raw lat/lon pairs (PSRAM)
size_t      s_numPts = 0;
size_t      s_numPolys = 0;

bool ensure_buffers() {
    if (!s_pts)      s_pts      = (lv_point_t *)psram_alloc(MAX_PTS * sizeof(lv_point_t));
    if (!s_polyLen)  s_polyLen  = (uint16_t *)  psram_alloc(MAX_POLYS * sizeof(uint16_t));
    if (!s_tPolyLen) s_tPolyLen = (uint16_t *)  psram_alloc(MAX_TILE_POLYS * sizeof(uint16_t));
    if (!s_tPts)     s_tPts     = (int16_t *)    psram_alloc(MAX_LINE_PTS * 2 * sizeof(int16_t));
    return s_pts && s_polyLen && s_tPolyLen && s_tPts;
}

long tile_floor(double coord, double grid) {
    return (long)floor(coord / grid) * (long)grid;
}

// --- Portable sequential tile reader ---------------------------------------
// Opens a tile and reads it start-to-end (no seeking). read_exact() always
// pulls through a small INTERNAL-RAM chunk first, then memcpy's into the
// caller's (PSRAM) destination — the SD SPI path can't DMA into PSRAM, and this
// also keeps the transfer sizes small. dst == nullptr means "consume and
// discard" (used to skip an over-long polyline while staying byte-aligned).
#ifdef ARDUINO
struct TileFile { File f; };
bool th_open(const char *path, TileFile &h) {
    if (!sdcard::mounted()) return false;
    h.f = SD.open(path, "r");
    return (bool)h.f;
}
int  th_read(TileFile &h, uint8_t *dst, size_t n) { return h.f.read(dst, n); }
void th_close(TileFile &h) { h.f.close(); }
#else
std::string s_simRoot;
struct TileFile { FILE *f; };
bool th_open(const char *path, TileFile &h) {
    const std::string full = s_simRoot + path;
    h.f = fopen(full.c_str(), "rb");
    return h.f != nullptr;
}
int  th_read(TileFile &h, uint8_t *dst, size_t n) { return (int)fread(dst, 1, n, h.f); }
void th_close(TileFile &h) { if (h.f) fclose(h.f); }
#endif

bool read_exact(TileFile &h, uint8_t *dst, size_t n) {
    static uint8_t chunk[4096];   // internal RAM (DMA-safe); single-task use
    size_t total = 0;
    while (total < n) {
        const size_t want = (n - total) < sizeof(chunk) ? (n - total) : sizeof(chunk);
        const int r = th_read(h, chunk, want);
        if (r <= 0) return false;            // EOF or read error
        if (dst) memcpy(dst + total, chunk, (size_t)r);
        total += (size_t)r;
    }
    return true;
}

} // namespace

namespace roads_sd {

#ifndef ARDUINO
void set_root(const char *root) { s_simRoot = root; }
#endif

void init() {
#ifdef ARDUINO
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
#endif
    const bool ok = ensure_buffers();
#ifdef ARDUINO
    Serial.printf("[roads_sd] init: buffers %s (PSRAM free %u)\n",
                  ok ? "reserved" : "FAILED", (unsigned)ESP.getFreePsram());
#else
    (void)ok;
#endif
}

// Stream one tile from the card into the output cache. Reads the header, then
// the polyLen[] table, then the points polyline-by-polyline — projecting each
// as it arrives so only one polyline is ever held in RAM. Returns silently on
// any malformed/short read (missing tiles are normal near the edge of coverage).
static void stream_tile(const char *path, double homeLat, double homeLon,
                        double rangeKm, float cx, float cy, float rOuterPx, Sink &out) {
    TileFile h;
    if (!th_open(path, h)) return;

    uint8_t hdr[6];
    if (!read_exact(h, hdr, 6)) { th_close(h); return; }
    uint16_t numPolys; memcpy(&numPolys, hdr, 2);
    uint32_t numPts;   memcpy(&numPts, hdr + 2, 4);
    if (numPolys == 0 || numPolys > MAX_TILE_POLYS) { th_close(h); return; }

    // polyLen[] table for the whole tile (tells us how to slice the point stream).
    if (!read_exact(h, (uint8_t *)s_tPolyLen, (size_t)numPolys * 2)) { th_close(h); return; }

    for (uint16_t i = 0; i < numPolys; ++i) {
        if (out.numPolys >= out.maxPolys || out.numPts >= out.maxPts) break;   // output full
        uint16_t n = s_tPolyLen[i];
        if (n == 0) continue;
        if (n > MAX_LINE_PTS) { read_exact(h, nullptr, (size_t)n * 4); continue; }  // skip, stay aligned
        if (!read_exact(h, (uint8_t *)s_tPts, (size_t)n * 4)) break;                // truncated file

        // Clip+project this single polyline into the cache at the current offset.
        // One input line can clip into several output segments, so ask for the
        // remaining capacity and advance by however many it wrote.
        const size_t got = geo_project_polylines_flat(
            s_tPts, 1, &n, 180,
            homeLat, homeLon, rangeKm, cx, cy, rOuterPx,
            out.pts + out.numPts, out.maxPts - out.numPts,
            out.polyLen + out.numPolys, out.maxPolys - out.numPolys);
        size_t written = 0;
        for (size_t j = 0; j < got; ++j) written += out.polyLen[out.numPolys + j];
        out.numPts   += written;
        out.numPolys += got;
    }
    th_close(h);
}

// Re-derive the cache for this home/range — home tile plus its 8 immediate
// neighbors (most of which simply won't exist on the card yet, th_open() just
// skips them), so a home near a tile boundary doesn't show a hard cutoff.
// Called only when home/range changes (see radar_view.cpp), same cadence as
// coastline_project() — never touches SD/disk per-frame.
// The home tile plus its eight neighbours, so a location near a tile edge does not show a
// hard cutoff. Most neighbours simply are not on the card and th_open() skips them.
static void walk_tiles(double homeLat, double homeLon, double rangeKm,
                       float cx, float cy, float rOuterPx, Sink &out) {
    const long latFloor = tile_floor(homeLat, ROAD_TILE_GRID_DEG);
    const long lonFloor = tile_floor(homeLon, ROAD_TILE_GRID_DEG);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (out.numPts >= out.maxPts || out.numPolys >= out.maxPolys) return;  // full
            char path[48];
            snprintf(path, sizeof(path), "/roads/r%ld_%ld.bin",
                    latFloor + dy * (long)ROAD_TILE_GRID_DEG,
                    lonFloor + dx * (long)ROAD_TILE_GRID_DEG);
            stream_tile(path, homeLat, homeLon, rangeKm, cx, cy, rOuterPx, out);
        }
    }
}

void project(double homeLat, double homeLon, double rangeKm,
            float cx, float cy, float rOuterPx) {
    s_numPts = 0;
    s_numPolys = 0;
    if (rangeKm <= 0) return;
    if (!ensure_buffers()) return;   // PSRAM tight — draw nothing, never crash
    if (!lock()) return;             // the other task is mid-read — keep the empty cache
    Sink out { s_pts, s_polyLen, MAX_PTS, MAX_POLYS };
    walk_tiles(homeLat, homeLon, rangeKm, cx, cy, rOuterPx, out);
    unlock();
    s_numPts   = out.numPts;
    s_numPolys = out.numPolys;
}

// The same worldwide roads, projected into buffers the CALLER owns, for a screen with its
// own geometry. Mirrors coastline_project_flat() beside coastline_project(): the scope keeps
// the cached form because it redraws from it constantly, and anything drawing at a different
// size gets its own copy rather than fighting over one.
//
// The weather map is why this exists. It used to read a separate flash-baked extract that
// covered Arizona and nothing else, so every Orb outside that box drew no roads at all while
// still paying to project ~20,000 polylines that all landed off-screen.
//
// `ok` says whether a projection actually ran, which is not the same as whether it found
// anything: a caller that caches per location must not record "no roads here" for a cycle
// that was simply locked out or short of memory.
size_t project_flat(double lat, double lon, double rangeKm,
                    float cx, float cy, float rOuterPx,
                    lv_point_t *outPts, size_t maxPts,
                    uint16_t *outPolyLen, size_t maxPolys, bool &ok) {
    ok = false;
    if (rangeKm <= 0 || !outPts || !outPolyLen || !maxPts || !maxPolys) return 0;
    if (!ensure_buffers()) return 0;
    if (!lock()) return 0;
    Sink out { outPts, outPolyLen, maxPts, maxPolys };
    walk_tiles(lat, lon, rangeKm, cx, cy, rOuterPx, out);
    unlock();
    ok = true;
    return out.numPolys;
}

void draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width) {
    if (!s_pts || !s_polyLen || s_numPts == 0) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.width = width;
    d.opa   = opa;
    d.round_start = d.round_end = 1;
    size_t pi = 0;
    for (size_t k = 0; k < s_numPolys; ++k) {
        const uint16_t n = s_polyLen[k];
        for (uint16_t i = 1; i < n; ++i) {
            lv_point_t a = s_pts[pi + i - 1];
            lv_point_t b = s_pts[pi + i];
            lv_draw_line(ctx, &d, &a, &b);
        }
        pi += n;
    }
}

} // namespace roads_sd
