#include "wx_radar_client.h"
#include "wx_radar.h"
#include "net_fetch.h"
#include "config.h"
#include "roads.h"
#ifdef ARDUINO
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#else
// Desktop/native build (no ESP32 core, no PSRAM): shim the Arduino-only calls this
// file uses outside the actual HTTP fetch (which goes through native_http.h/curl
// instead of WiFiClientSecure/HTTPClient below). Same pattern as clock_view.cpp.
#include "native_http.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } void println(const char *s) const { puts(s); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <new>
#include <stdlib.h>
#include <string>
#include <math.h>

static PNG *s_png = nullptr;
static uint32_t s_decodedPixels = 0;
static uint32_t s_sourcePixels = 0;
static int s_minX = WX_RADAR_SOURCE_SIZE, s_minY = WX_RADAR_SOURCE_SIZE;
static int s_maxX = -1, s_maxY = -1;

// Two display-range tiers (50mi / 100mi, cycled by pushing the knob while on the Weather
// app; a 10mi tier used to lead but was dropped as not useful). fetchZoomLevel/fetchRangeKm
// describe what RainViewer's tile endpoint is assumed to return at that zoom — unverifiable
// from their docs (this lat/lon-centered endpoint isn't the documented {z}/{x}/{y} form):
// 75km at zoom 7 is the original tuned value (the old "75 KM" UI label); each zoom step is
// assumed to double/halve the ground distance per pixel, standard web-mercator style, so
// zoom 6 (one step out) ~= 2x the coverage and zoom 5 (two steps out) ~= 4x. displayRangeKm
// is always <= fetchRangeKm, so every tier is a crop-and-upscale of whatever comes back
// (see composite_zoom()) rather than needing the fetch itself to exactly match — if the
// real numbers are off, the picture just crops a bit tighter or looser, it won't break.
struct WxZoomSpec { double displayKm; int fetchZoomLevel; double fetchRangeKm; };
static const WxZoomSpec WX_ZOOM[2] = {
    { 80.4672,  6, 150.0 },   // 50mi
    { 160.9344, 5, 300.0 },   // 100mi
};
constexpr uint16_t ROAD_COLOR = 0x4A49;   // dim grey — reads as a road, not precipitation
constexpr size_t ROAD_MAX_PTS   = 48000;  // baked datasets: narrow ~43k pts/21.4k polys (motorway+trunk+primary), wide ~8.5k pts/4.2k polys — margin
constexpr size_t ROAD_MAX_POLYS = 24000;

static lv_point_t *s_roadPts      = nullptr;   // PSRAM — projected once, reused until center/tier changes
static uint16_t   *s_roadPolyLen  = nullptr;
static size_t       s_roadPolyCount = 0;
static double        s_roadLat = 1000.0, s_roadLon = 1000.0;   // last-projected center
static int           s_roadTier = -1;                          // last-projected zoom tier

// Frame list cached at slot 0 of each refresh, reused for the rest of the loop's slots so
// the RainViewer index JSON is fetched once per cycle, not once per frame. Holds the last
// WX_RADAR_FRAMES entries of the "past" array, oldest first (slot order == time order).
static char     s_host[80]  = "";
static char     s_paths[WX_RADAR_FRAMES][48] = { { 0 } };
static uint32_t s_times[WX_RADAR_FRAMES] = { 0 };
static int      s_availFrames = 0;

// Raw decoded precipitation, at native fetch resolution, no roads mixed in — kept
// separate from the display buffer so composite_zoom() can crop/upscale it independently
// of the roads layer (which is reprojected fresh at the true display range instead, so
// it stays crisp instead of getting blocky right along with the coarse radar data).
static uint16_t *s_nativeBuf = nullptr;   // PSRAM, WX_RADAR_SIZE x WX_RADAR_SIZE

// Bresenham, clipped to the same circle the precipitation crop uses, so a road segment
// that crosses the boundary doesn't leave a stray line poking past the display's edge.
// The roads, as one bit per pixel, rasterised ONCE per location.
//
// They were redrawn into every frame: ~20,000 polylines, Bresenham-stepped, each pixel a
// bounds check and a scattered 16-bit write into PSRAM. Three frames a cycle meant three
// helpings of that, and it showed. The sweep is timed on the other core and its own log
// says what it cost: 100/101/114 ms and 13 ms of spread while idle, against 100/104/373 ms
// and 273 ms of spread while frames were landing. A third of a second of frozen sweep,
// three times per refresh.
//
// Roads are static for a given centre and they are drawn in ONE colour, so all that work
// produced the same shape every time and the shape fits in a bitmap: 360 x 360 bits is
// 16,200 bytes, against 253 KB for another full frame buffer. Per frame it becomes a
// linear scan of that bitmap, mostly zero bytes, which is both far less work and far
// kinder to the cache than scattered writes along diagonal lines.
#define ROAD_MASK_BYTES ((WX_RADAR_SIZE * WX_RADAR_SIZE + 7) / 8)
static uint8_t *s_roadMask = nullptr;
// Bumped whenever the projection is recomputed; the mask is stale until it matches. Two
// counters rather than re-comparing lat/lon/tier, so a cycle where the mask could not be
// allocated retries on the next one instead of being remembered as done.
static uint32_t s_roadStamp   = 0;
static uint32_t s_roadMaskFor = 0;

static inline void road_mask_set(int x, int y) {
    const size_t bit = (size_t)y * WX_RADAR_SIZE + (size_t)x;
    s_roadMask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

// Same Bresenham, same clipping, writing a bit instead of a pixel.
static void mask_road_line(lv_point_t a, lv_point_t b, int c) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const int ddx = x0 - c, ddy = y0 - c;
        if (x0 >= 0 && x0 < WX_RADAR_SIZE && y0 >= 0 && y0 < WX_RADAR_SIZE &&
            ddx * ddx + ddy * ddy <= (c - 2) * (c - 2)) {
            road_mask_set(x0, y0);
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_road_line(uint16_t *dst, lv_point_t a, lv_point_t b, int c) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const int ddx = x0 - c, ddy = y0 - c;
        if (x0 >= 0 && x0 < WX_RADAR_SIZE && y0 >= 0 && y0 < WX_RADAR_SIZE &&
            ddx * ddx + ddy * ddy <= (c - 2) * (c - 2)) {
            dst[y0 * WX_RADAR_SIZE + x0] = ROAD_COLOR;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Draws the local major-highway extract (roads.cpp) into the back buffer as a base
// layer. Re-projected fresh at the tier's true display range (not upscaled along with
// the coarse precipitation raster — see composite_zoom()), so it stays a crisp line
// regardless of zoom. Re-projects only when the center or tier actually changed — cheap
// cache, same pattern coastline.cpp uses for the Radar scope.
static void draw_roads(double lat, double lon, int tier) {
    uint16_t *dst = wx_radar_back_buffer();
    if (!dst) return;
    if (!s_roadPts) s_roadPts = (lv_point_t *)heap_caps_malloc(ROAD_MAX_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    if (!s_roadPolyLen) s_roadPolyLen = (uint16_t *)heap_caps_malloc(ROAD_MAX_POLYS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_roadPts || !s_roadPolyLen) return;
    // ORBNOMASK=1 forces the old per-frame polyline path, so the two can be rendered from
    // the SAME frames and diffed. An optimisation that changes what is drawn is not an
    // optimisation, and "it looks about right" is not a check.
#ifndef ARDUINO
    static const bool noMask = getenv("ORBNOMASK") != nullptr;
    if (noMask) { /* leave s_roadMask null: the fallback below draws per frame */ } else
#endif
    if (!s_roadMask) s_roadMask = (uint8_t *)heap_caps_malloc(ROAD_MASK_BYTES, MALLOC_CAP_SPIRAM);

    if (lat != s_roadLat || lon != s_roadLon || tier != s_roadTier) {
        const float c = WX_RADAR_SIZE / 2.0f;
        const double rangeKm = WX_ZOOM[tier].displayKm;
        // 100mi tier (index 1): motorway-only wide extract (trunk roads would be an
        // illegible tangle at that scale anyway, and the Overpass query for that radius
        // only completed with motorway-only filtering — see tools/gen_roads.py).
        s_roadPolyCount = (tier == 1)
            ? roads_wide_project_flat(lat, lon, rangeKm, c, c, c - 2, s_roadPts, ROAD_MAX_PTS, s_roadPolyLen, ROAD_MAX_POLYS)
            : roads_project_flat(lat, lon, rangeKm, c, c, c - 2, s_roadPts, ROAD_MAX_PTS, s_roadPolyLen, ROAD_MAX_POLYS);
        s_roadLat = lat; s_roadLon = lon; s_roadTier = tier;
        ++s_roadStamp;   // the mask below is stale whenever this changes
        Serial.printf("[wxradar] roads: %u polylines near this center (tier=%d)\n", (unsigned)s_roadPolyCount, tier);
    }

    const int c = WX_RADAR_SIZE / 2;
    // Rasterise into the mask only when the projection actually moved. s_roadMaskFor is
    // separate from the s_roadLat/s_roadLon triple above so that a failed allocation on one
    // cycle is retried on the next rather than being remembered as done.
    if (s_roadMask && s_roadMaskFor != s_roadStamp) {
        memset(s_roadMask, 0, ROAD_MASK_BYTES);
        size_t idx = 0;
        for (size_t poly = 0; poly < s_roadPolyCount; ++poly) {
            const uint16_t n = s_roadPolyLen[poly];
            for (uint16_t i = 1; i < n; ++i) mask_road_line(s_roadPts[idx + i - 1], s_roadPts[idx + i], c);
            idx += n;
        }
        s_roadMaskFor = s_roadStamp;
        Serial.println("[wxradar] roads rasterised into the mask (once for this centre)");
    }

    if (!s_roadMask) {   // allocation failed: fall back to the old per-frame draw
        size_t idx = 0;
        for (size_t poly = 0; poly < s_roadPolyCount; ++poly) {
            const uint16_t n = s_roadPolyLen[poly];
            for (uint16_t i = 1; i < n; ++i) draw_road_line(dst, s_roadPts[idx + i - 1], s_roadPts[idx + i], c);
            idx += n;
        }
        return;
    }

    // The frame's share of the work: one linear pass, and the great majority of these bytes
    // are zero, so the inner loop is skipped outright for most of them.
    size_t bit = 0;
    for (size_t byteIdx = 0; byteIdx < ROAD_MASK_BYTES; ++byteIdx, bit += 8) {
        const uint8_t m = s_roadMask[byteIdx];
        if (!m) continue;
        for (int b = 0; b < 8; ++b) {
            if (!(m & (1u << b))) continue;
            const size_t px = bit + (size_t)b;
            if (px < (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE) dst[px] = ROAD_COLOR;
        }
    }
}

// Crops the native-resolution decoded precipitation (radius = fetchRangeKm) down to the
// tier's true display radius and upscales it (nearest-neighbor) to fill the same circle
// the roads layer draws into, then composites over it. At the tight 5mi tier this is a
// large magnification of a small source patch — RainViewer's mosaic is coarse to begin
// with, so that shows up as bigger, blockier precipitation cells rather than finer ones;
// there's no higher-resolution source data to zoom into further.
static void composite_zoom(int tier) {
    uint16_t *dst = wx_radar_back_buffer();
    if (!dst || !s_nativeBuf) return;
    const float scale = (float)(WX_ZOOM[tier].displayKm / WX_ZOOM[tier].fetchRangeKm);
    const int c = WX_RADAR_SIZE / 2;
    const int rMax2 = (c - 2) * (c - 2);
    for (int outY = 0; outY < WX_RADAR_SIZE; ++outY) {
        const int dy = outY - c;
        for (int outX = 0; outX < WX_RADAR_SIZE; ++outX) {
            const int dx = outX - c;
            if (dx * dx + dy * dy > rMax2) continue;
            const int srcX = c + (int)lroundf(dx * scale);
            const int srcY = c + (int)lroundf(dy * scale);
            if (srcX < 0 || srcX >= WX_RADAR_SIZE || srcY < 0 || srcY >= WX_RADAR_SIZE) continue;
            const uint16_t pixel = s_nativeBuf[srcY * WX_RADAR_SIZE + srcX];
            // Only overwrite where there's real precipitation — 0 means "nothing here",
            // so the roads drawn underneath stay visible through the gaps.
            if (pixel) dst[outY * WX_RADAR_SIZE + outX] = pixel;
        }
    }
}

static bool ensure_decoder(void) {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { Serial.println("[wxradar] PSRAM decoder allocation failed"); return false; }
    s_png = new (mem) PNG();
    Serial.printf("[wxradar] PNG decoder in PSRAM (%u bytes)\n", (unsigned)sizeof(PNG));
    return true;
}

static int radar_png_line(PNGDRAW *draw) {
    uint16_t *dst = s_nativeBuf;
    const int crop = (WX_RADAR_SOURCE_SIZE - WX_RADAR_SIZE) / 2;
    if (!dst) return 1;
    uint16_t line[WX_RADAR_SOURCE_SIZE];
    if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
        // Read alpha explicitly (src[3]) rather than assuming a transparent source
        // pixel's RGB happens to be (0,0,0) — 0 means "no precipitation here" once
        // composite_zoom() reads this buffer, not "paint it black".
        const uint8_t *src = draw->pPixels;
        for (int x = 0; x < draw->iWidth; ++x, src += 4) {
            if (src[3] < 20) { line[x] = 0; continue; }   // transparent: no precipitation here
            uint16_t rgb = (uint16_t)((src[2] >> 3) | ((src[1] >> 2) << 5) | ((src[0] >> 3) << 11));
            line[x] = rgb ? rgb : 1;   // never let real (if coincidentally black) data read as "no data"
        }
    } else {
        s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    }
    for (int x = 0; x < draw->iWidth; ++x) if (line[x]) {
        ++s_sourcePixels;
        if (x < s_minX) s_minX = x;
        if (x > s_maxX) s_maxX = x;
        if (draw->y < s_minY) s_minY = draw->y;
        if (draw->y > s_maxY) s_maxY = draw->y;
    }
    if (draw->y < crop || draw->y >= crop + WX_RADAR_SIZE) return 1;
    const int outY = draw->y - crop;
    const int c = WX_RADAR_SIZE / 2;
    const int dy = outY - c;
    for (int outX = 0; outX < WX_RADAR_SIZE; ++outX) {
        const int dx = outX - c;
        if (dx * dx + dy * dy > (c - 2) * (c - 2)) continue;   // outside the circle — already 0 from the clear
        const uint16_t pixel = line[outX + crop];
        if (pixel) {
            dst[outY * WX_RADAR_SIZE + outX] = pixel;
            ++s_decodedPixels;
        }
    }
    return 1;
}

// Fresh connection per call (reverted from a kept-alive experiment). A persistent, always-
// held WiFiClientSecure permanently occupies scarce internal RAM the moment it first
// connects, which starved LATER connections (this metadata fetch, the weather forecast)
// out of the contiguous internal block a TLS handshake needs -- the "SSL - Memory
// allocation failed" (-32512) / stuck "Updating..." symptom. Opening fresh and closing
// here releases that RAM between the ~5-min refreshes, giving every feed a fair window.
#ifdef ARDUINO
static bool http_get_string(const char *url, std::string &body, int timeoutMs) {
    // PLAIN, and named plainly. This built a WiFiClientSecure unconditionally, which
    // handshakes on connect whatever the URL scheme says, so every call was a TLS attempt
    // and every one failed -32512 on a board that cannot raise the two contiguous ~16 KB
    // internal blocks a handshake needs.
    //
    // 1.44 rewrote the tile HOST from https:// to http:// and left this alone, so the
    // screen still never came off "UPDATING": the scheme in the string was right and the
    // transport underneath it was still TLS. The old name is most of why, it read as
    // settled and the fix went looking somewhere else.
    if (!strncmp(url, "https://", 8)) {
        Serial.printf("[wxradar] refusing %s: this board cannot do TLS at all\n", url);
        return false;
    }
    WiFiClient client;
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3500);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) {
        // No TLS error to report any more: there is no TLS. What used to print here was
        // always -32512 'SSL - Memory allocation failed', which named the symptom of using
        // a secure client at all rather than anything about the request.
        Serial.printf("[wxradar] HTTP %d for %s heap=%u largest=%u psram=%u\n",
                      status, url, (unsigned)ESP.getFreeHeap(),
                      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                      (unsigned)ESP.getFreePsram());
        http.end(); return false;
    }
    String s = http.getString();
    body.assign(s.c_str(), s.length());
    http.end();
    return !body.empty();
}
#else
static bool http_get_string(const char *url, std::string &body, int timeoutMs) {
    return native_https_get(url, ADSB_USER_AGENT, body, timeoutMs);
}
#endif

// slot 0 of each cycle: pull the RainViewer frame index and cache the last N entries
// (oldest first) so the rest of the loop reuses it. Returns the number of frames available
// (0 on failure).
static int load_frame_list(void) {
    std::string meta;
    if (!http_get_string("http://api.rainviewer.com/public/weather-maps.json", meta, 6500)) {
        Serial.println("[wxradar] metadata fetch failed"); return 0;
    }
    JsonDocument doc;
    if (deserializeJson(doc, meta)) { Serial.println("[wxradar] metadata JSON failed"); return 0; }
    const char *host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    if (!host[0] || past.size() == 0) { Serial.println("[wxradar] no radar frames"); return 0; }

    // FORCE PLAIN HTTP, whatever the index says.
    //
    // RainViewer's weather-maps.json returns "host": "https://tilecache.rainviewer.com",
    // and this used to copy it verbatim. Every tile request was therefore an https:// URL
    // handed to a helper that ALWAYS opened a secure client, whatever the scheme said, because
    // this board cannot do TLS at all: it cannot raise the two contiguous ~16 KB internal
    // blocks a handshake needs, which is why every other feed on this device was moved to
    // plain HTTP in August.
    //
    // So the metadata fetch above succeeded, being hardcoded to http://, and then EVERY
    // TILE FAILED, silently and forever. The screen showed "UPDATING" and never came off it.
    //
    // The tiles are served over plain HTTP by the same host: verified 200 with a real PNG
    // body. Rewriting the scheme here rather than at the call site means it cannot be
    // missed if another URL is ever built from s_host.
    if (!strncmp(host, "https://", 8)) snprintf(s_host, sizeof(s_host), "http://%s", host + 8);
    else                               snprintf(s_host, sizeof(s_host), "%s", host);
    Serial.printf("[wxradar] tile host %s\n", s_host);
    const int total = (int)past.size();
    const int avail = total < WX_RADAR_FRAMES ? total : WX_RADAR_FRAMES;
    const int first = total - avail;   // take the newest `avail` frames, keep them time-ordered
    for (int i = 0; i < avail; ++i) {
        JsonObjectConst f = past[first + i].as<JsonObjectConst>();
        snprintf(s_paths[i], sizeof(s_paths[i]), "%s", (const char *)(f["path"] | ""));
        s_times[i] = f["time"] | 0;
    }
    s_availFrames = avail;
    Serial.printf("[wxradar] frame list: %d frames (of %d past)\n", avail, total);
    return avail;
}

int wx_radar_fetch_frame(double lat, double lon, int zoomTier, uint32_t gen, int slot) {
    if (zoomTier < 0 || zoomTier > 1) zoomTier = 0;
    if (slot < 0 || slot >= WX_RADAR_FRAMES) return 0;
#ifdef ARDUINO
    if (WiFi.status() != WL_CONNECTED) return -1;
#endif
    if (!wx_radar_back_buffer() || !ensure_decoder()) return -1;
    if (!s_nativeBuf) s_nativeBuf = (uint16_t *)heap_caps_malloc(WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_nativeBuf) { Serial.println("[wxradar] PSRAM native buffer allocation failed"); return -1; }

    if (slot == 0) {
        if (load_frame_list() <= 0) return -1;
    }
    if (slot >= s_availFrames || !s_paths[slot][0]) return 0;   // fewer frames than the loop length — done

    char url[320];
    snprintf(url, sizeof(url), "%s%s/512/%d/%.5f/%.5f/2/1_1.png",
             s_host, s_paths[slot], WX_ZOOM[zoomTier].fetchZoomLevel, lat, lon);
    // Stream the tile straight into PSRAM (net_fetch): a 512px PNG can be 100+ KB, and an
    // internal-heap String that big starves the live feed's TLS handshake.
    uint8_t *image = nullptr; size_t imageLen = 0;
    if (!net_fetch_psram(url, ADSB_USER_AGENT, &image, &imageLen, 260000, 3500, 8500)) {
        Serial.println("[wxradar] tile fetch failed"); return -1;
    }
    memset(s_nativeBuf, 0, WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
    s_decodedPixels = 0;
    s_sourcePixels = 0;
    s_minX = s_minY = WX_RADAR_SOURCE_SIZE;
    s_maxX = s_maxY = -1;
    const int opened = s_png->openRAM(image, imageLen, radar_png_line);
    if (opened != PNG_SUCCESS) {
        Serial.printf("[wxradar] PNG open error %d\n", opened);
        heap_caps_free(image); return -1;
    }
    if (s_png->getWidth() != WX_RADAR_SOURCE_SIZE || s_png->getHeight() != WX_RADAR_SOURCE_SIZE) {
        Serial.println("[wxradar] unexpected tile dimensions");
        s_png->close(); heap_caps_free(image); return -1;
    }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    heap_caps_free(image);           // PNG fully decoded (or failed) — buffer no longer needed
    if (decoded != PNG_SUCCESS) { Serial.printf("[wxradar] PNG decode error %d\n", decoded); return -1; }

    // Composite: roads (fresh, crisp, at the true display range) as the base layer, then
    // the native precipitation raster cropped/upscaled on top of it.
    memset(wx_radar_back_buffer(), 0, WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
    draw_roads(lat, lon, zoomTier);
    composite_zoom(zoomTier);
    wx_radar_commit_frame(slot, gen, s_times[slot], lat, lon);
    Serial.printf("[wxradar] gen %lu frame %d/%d @%lu (tier=%d, %lu px)\n",
                  (unsigned long)gen, slot + 1, s_availFrames, (unsigned long)s_times[slot],
                  zoomTier, (unsigned long)s_decodedPixels);
    return 1;
}
