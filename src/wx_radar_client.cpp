#include "wx_radar_client.h"
#include "wx_radar.h"
#include "net_fetch.h"
#include "config.h"
#include "roads_sd.h"
#include "coastline.h"
#include "theme_style.h"
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
#include <chrono>
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
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
// 0xRRGGBB down to the 565 these buffers hold. The colour was hard-coded at 0x4A49 before
// the theme was consulted at all, and that is exactly what this returns for the default
// 0x4A4A4A, so a theme that says nothing about it draws the map it has always drawn.
static inline uint16_t rgb565(uint32_t c) {
    return (uint16_t)((((c >> 19) & 0x1F) << 11) | (((c >> 10) & 0x3F) << 5) | ((c >> 3) & 0x1F));
}
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
static uint8_t *s_roadMask  = nullptr;
static uint8_t *s_coastMask = nullptr;   // the shoreline, its own layer and its own colour
// Bumped whenever the projection is recomputed; the mask is stale until it matches. Two
// counters rather than re-comparing lat/lon/tier, so a cycle where the mask could not be
// allocated retries on the next one instead of being remembered as done.
static uint32_t s_roadStamp   = 0;
static uint32_t s_roadMaskFor = 0;

static inline void road_mask_set(uint8_t *mask, int x, int y) {
    const size_t bit = (size_t)y * WX_RADAR_SIZE + (size_t)x;
    mask[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

// Same Bresenham, same clipping, writing a bit instead of a pixel.
static void mask_road_line(uint8_t *mask, lv_point_t a, lv_point_t b, int c) {
    int x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;
    const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const int ddx = x0 - c, ddy = y0 - c;
        if (x0 >= 0 && x0 < WX_RADAR_SIZE && y0 >= 0 && y0 < WX_RADAR_SIZE &&
            ddx * ddx + ddy * ddy <= (c - 2) * (c - 2)) {
            road_mask_set(mask, x0, y0);
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// The map under the weather: major roads and the shoreline, from the SAME worldwide data
// the Flight Tracker draws, at the same granularity.
//
// This used to read a separate extract baked into flash, and that extract covered a box
// around Phoenix and nothing else. Point an Orb at Orlando and the map drew no roads at
// all, while still projecting and rasterising ~20,000 Arizona polylines, every one of which
// landed off-screen. Two datasets for one kind of thing is how that happens: nothing
// reports a fault, the map is simply empty, and only somebody who knew both existed would
// think to compare them.
//
// WHICH THREAD DOES WHAT is the load-bearing part of this file, and I got it wrong first.
//
// The road data lives on the SD card, and the Arduino SD driver is not safe to call from
// two tasks at once. My first version read tiles from the network task, which put it in a
// race with the UI thread loading theme art off the same card on every app switch. It
// survived a slow walk through the apps and wedged core 0 within seconds of a fast one:
// the aircraft task carried on logging while the screen, the web server and the USB command
// handler all stopped together, which is what a locked-up SD driver looks like from outside.
//
// So the split is: THE UI THREAD READS THE CARD, in wx_map_prepare(), on the same
// take-on-enter contract as everything else. The network task only ever reads the finished
// 1-bit masks, and touches no file and no allocator. The masks are 16 KB each and are kept
// for the life of the process on purpose: they are the one thing both threads see, and not
// freeing them is what makes "the network task is mid-blit while the app closes" a
// non-question rather than a window to be narrowed.
static void mask_polylines(uint8_t *mask, size_t polys, int c) {
    memset(mask, 0, ROAD_MASK_BYTES);
    size_t idx = 0;
    for (size_t poly = 0; poly < polys; ++poly) {
        const uint16_t n = s_roadPolyLen[poly];
        for (uint16_t i = 1; i < n; ++i)
            mask_road_line(mask, s_roadPts[idx + i - 1], s_roadPts[idx + i], c);
        idx += n;
    }
}

// The plate, cropped to the radar circle. See the note in wx_radar.h for why it is never
// freed. 253 KB, against the 1.2 MB of frame buffers this screen already takes and gives
// back around it.
static uint16_t *s_plateCrop = nullptr;
// Separate from the pointer, because the buffer is kept once allocated and a design that
// drops its picture has to stop drawing it without the allocation going away.
static bool      s_plateHave = false;

bool wx_plate_have() { return s_plateHave && s_plateCrop != nullptr; }

// Is this point inside one of the weather map's keep-out zones?
//
// Zones are written in 466x466 screen coordinates, because that is what a designer is
// looking at in Orb Studio. The radar image is 360x360 drawn at (53, 52) on that screen, so
// the buffer coordinate is offset before it is tested. Getting this offset wrong would put
// every zone 53 pixels from where it was drawn, which looks like the feature almost working.
static bool wx_in_zone(int bx, int by) {
    const theme_style::Weather &w = theme_style::weather();
    if (w.zoneCount <= 0) return false;
    const int x = bx + (466 - WX_RADAR_SIZE) / 2;
    const int y = by + 52;
    bool anyInvert = false, insideInvert = false;
    for (int i = 0; i < w.zoneCount; ++i) {
        const theme_style::Zone &z = w.zones[i];
        bool inside;
        if (z.rect) {
            inside = (x >= z.x - z.w / 2 && x <= z.x + z.w / 2 &&
                      y >= z.y - z.h / 2 && y <= z.y + z.h / 2);
        } else {
            const int dx = x - z.x, dy = y - z.y;
            inside = (dx * dx + dy * dy) <= (z.r * z.r);
        }
        if (z.invert) { anyInvert = true; if (inside) insideInvert = true; }
        else if (inside) return true;
    }
    // An inverted zone means "hide everything OUTSIDE me", so it masks the point when the
    // point is not in it. Only one is ever honoured; theme_style drops the rest.
    return anyInvert && !insideInvert;
}

// Put back what was underneath, everywhere a zone says the map may not draw.
//
// Called once per frame build, not per displayed frame. The map, the coastline and the
// precipitation are three separate passes that know nothing about zones, and teaching each
// of them to clip would mean threading zone state through all three. They have all already
// been flattened into this one buffer, so the whole job is a single pass that restores the
// base underneath: the theme's plate where it has one, its background colour where it does
// not. That is what lets decoration live in the baked background instead of in a layer above
// the map, which is the entire reason the Flight Tracker got zones first.
// The range rings, into the frame rather than over it.
//
// They were three LVGL objects sitting on top of the canvas, which meant no keep-out area
// could touch them: the zones restore the background INSIDE the frame buffer, and anything
// drawn above that buffer is simply out of reach. A design with a keep-out area got a clean
// gap in the roads with the rings still ruled straight across it.
//
// Drawn here, one line below the map and one line above the zone pass, they are covered like
// everything else. Which is what the card column in Orb Studio has been claiming all along.
static void draw_rings(uint16_t *dst) {
    const theme_style::Weather &w = theme_style::weather();
    if (!dst || !w.ringsEnabled) return;
    const int c = WX_RADAR_SIZE / 2;
    // The same three radii the objects had, as a fraction of the dial, so a theme that has
    // never heard of this change draws exactly the rings it drew before.
    const int radii[3] = { c - 2, (c * 2) / 3, c / 3 };
    const uint16_t col = rgb565(w.ringColorOn ? w.ringColor : 0x1DFF86);
    for (int k = 0; k < 3; ++k) {
        const int r = radii[k];
        if (r < 2) continue;
        // Midpoint circle, eight-way symmetric. No trig, no allocation, and it lands on the
        // same pixels every frame so the rings cannot shimmer against the precipitation.
        int x = r, y = 0, err = 1 - r;
        while (x >= y) {
            const int pts[8][2] = { {x,y},{y,x},{-y,x},{-x,y},{-x,-y},{-y,-x},{y,-x},{x,-y} };
            for (auto &pt : pts) {
                const int px = c + pt[0], py = c + pt[1];
                if (px >= 0 && px < WX_RADAR_SIZE && py >= 0 && py < WX_RADAR_SIZE)
                    dst[(size_t)py * WX_RADAR_SIZE + px] = col;
            }
            ++y;
            if (err < 0) err += 2 * y + 1;
            else { --x; err += 2 * (y - x) + 1; }
        }
    }
}

static void wx_apply_zones(uint16_t *dst) {
    if (!dst || theme_style::weather().zoneCount <= 0) return;
    const uint16_t bg = rgb565(theme_style::weather().bg);
    const bool havePlate = wx_plate_have() && s_plateCrop;
    int cleared = 0;
    for (int y = 0; y < WX_RADAR_SIZE; ++y) {
        uint16_t *row = dst + (size_t)y * WX_RADAR_SIZE;
        const uint16_t *src = havePlate ? s_plateCrop + (size_t)y * WX_RADAR_SIZE : nullptr;
        for (int x = 0; x < WX_RADAR_SIZE; ++x) {
            if (!wx_in_zone(x, y)) continue;
            row[x] = src ? src[x] : bg;
            cleared++;
        }
    }
    Serial.printf("[wxradar] zones cleared %d px of map\n", cleared);
}

void wx_plate_blit(uint16_t *dst) {
    if (s_plateCrop && dst)
        memcpy(dst, s_plateCrop, (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
}

void wx_plate_set(const uint16_t *src, int w, int h) {
    if (!src || w <= 0 || h <= 0) {
        if (s_plateCrop) memset(s_plateCrop, 0, (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
        s_plateHave = false;
        return;
    }
    if (!s_plateCrop) {
        s_plateCrop = (uint16_t *)heap_caps_malloc((size_t)WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t),
                                                   MALLOC_CAP_SPIRAM);
        if (!s_plateCrop) { Serial.println("[wxradar] no PSRAM for the background crop"); return; }
    }
    // The centre of the plate, at the size the radar image is drawn. Both are centred on the
    // screen, so the crop is a straight offset rather than a scale: a design's picture lines
    // up with what it looked like in Orb Studio instead of being subtly resampled.
    const int ox = (w - WX_RADAR_SIZE) / 2, oy = (h - WX_RADAR_SIZE) / 2;
    for (int y = 0; y < WX_RADAR_SIZE; ++y) {
        const int sy = oy + y;
        uint16_t *d = s_plateCrop + (size_t)y * WX_RADAR_SIZE;
        if (sy < 0 || sy >= h) { memset(d, 0, WX_RADAR_SIZE * sizeof(uint16_t)); continue; }
        for (int x = 0; x < WX_RADAR_SIZE; ++x) {
            const int sx = ox + x;
            d[x] = (sx < 0 || sx >= w) ? 0 : src[(size_t)sy * w + sx];
        }
    }
    s_plateHave = true;
    Serial.printf("[wxradar] background picture cropped to %dx%d\n", WX_RADAR_SIZE, WX_RADAR_SIZE);
}

void wx_map_prepare(double lat, double lon, int tier) {
    const theme_style::Weather &wx = theme_style::weather();
    if (!wx.roadsEnabled && !wx.coastEnabled) return;
    if (tier < 0 || tier > 1) return;
    if (lat == s_roadLat && lon == s_roadLon && tier == s_roadTier) return;   // already built

    if (!s_roadMask  && wx.roadsEnabled) s_roadMask  = (uint8_t *)heap_caps_malloc(ROAD_MASK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_coastMask && wx.coastEnabled) s_coastMask = (uint8_t *)heap_caps_malloc(ROAD_MASK_BYTES, MALLOC_CAP_SPIRAM);

    // The projection scratch is big (~430 KB) and is wanted only for the few milliseconds
    // this function runs, so it is taken and given back here rather than held all session.
    s_roadPts     = (lv_point_t *)heap_caps_malloc(ROAD_MAX_PTS * sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    s_roadPolyLen = (uint16_t *)heap_caps_malloc(ROAD_MAX_POLYS * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_roadPts || !s_roadPolyLen) {
        if (s_roadPts)     { heap_caps_free(s_roadPts);     s_roadPts = nullptr; }
        if (s_roadPolyLen) { heap_caps_free(s_roadPolyLen); s_roadPolyLen = nullptr; }
        Serial.println("[wxradar] map: no room to project; leaving the last one up");
        return;
    }

    const int    c       = WX_RADAR_SIZE / 2;
    const float  cf      = WX_RADAR_SIZE / 2.0f;
    const double rangeKm = WX_ZOOM[tier].displayKm;
    bool ok = false;
    // Timed because this runs on the UI thread and everything else stops while it does.
    // If it ever grows past a frame or two the answer is to break it up, and the only way
    // to know that is to have been watching the number all along.
    const uint32_t t0 = millis();

    if (s_roadMask) {
        const size_t polys = roads_sd::project_flat(lat, lon, rangeKm, cf, cf, cf - 2,
                                                    s_roadPts, ROAD_MAX_PTS,
                                                    s_roadPolyLen, ROAD_MAX_POLYS, ok);
        // `ok` is not the same as "found something": a cycle short of memory must not be
        // recorded as "there are no roads here", or the map stays empty until the Orb moves.
        if (ok) {
            mask_polylines(s_roadMask, polys, c);
            Serial.printf("[wxradar] roads: %u polylines from the card (tier=%d)\n",
                          (unsigned)polys, tier);
        }
    }
    if (s_coastMask) {
        const size_t polys = coastline_project_flat(lat, lon, rangeKm, cf, cf, cf - 2,
                                                    s_roadPts, ROAD_MAX_PTS,
                                                    s_roadPolyLen, ROAD_MAX_POLYS);
        mask_polylines(s_coastMask, polys, c);
        Serial.printf("[wxradar] coastline: %u polylines\n", (unsigned)polys);
    }

    heap_caps_free(s_roadPts);     s_roadPts = nullptr;
    heap_caps_free(s_roadPolyLen); s_roadPolyLen = nullptr;
    if (ok || !s_roadMask) { s_roadLat = lat; s_roadLon = lon; s_roadTier = tier; }
    Serial.printf("[wxradar] map built in %lums (UI thread)\n", (unsigned long)(millis() - t0));
}

// The network task's whole share of the map: one linear pass per layer over a bitmap that
// is mostly zero bytes, so the inner loop is skipped outright for most of them. No file, no
// allocation, nothing another thread can be in the middle of. Coast first and roads over the
// top, so a highway crossing an inlet stays continuous.
static void draw_map() {
    uint16_t *dst = wx_radar_back_buffer();
    if (!dst) return;
    const theme_style::Weather &wx = theme_style::weather();
    struct Layer { const uint8_t *mask; uint16_t color; };
    const Layer layers[2] = {
        { wx.coastEnabled ? s_coastMask : nullptr, rgb565(wx.coastColor) },
        { wx.roadsEnabled ? s_roadMask  : nullptr, rgb565(wx.roadColor)  },
    };
    for (const Layer &L : layers) {
        if (!L.mask) continue;
        size_t bit = 0;
        for (size_t byteIdx = 0; byteIdx < ROAD_MASK_BYTES; ++byteIdx, bit += 8) {
            const uint8_t m = L.mask[byteIdx];
            if (!m) continue;
            for (int b = 0; b < 8; ++b) {
                if (!(m & (1u << b))) continue;
                const size_t px = bit + (size_t)b;
                if (px < (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE) dst[px] = L.color;
            }
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

    // Composite, bottom up: the theme's background, then the roads and coastline, then the
    // precipitation. The background used to be a memset to black, which is what made a
    // design's chosen picture and colour both invisible under this screen's opaque image.
    if (wx_plate_have()) wx_plate_blit(wx_radar_back_buffer());
    else {
        const uint16_t bg = rgb565(theme_style::weather().bg);
        uint16_t *dst = wx_radar_back_buffer();
        if (bg == 0) memset(dst, 0, WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t));
        else for (size_t i = 0; i < (size_t)WX_RADAR_SIZE * WX_RADAR_SIZE; ++i) dst[i] = bg;
    }
    draw_map();
    composite_zoom(zoomTier);
    draw_rings(wx_radar_back_buffer());     // over the rain, under the keep-out areas
    wx_apply_zones(wx_radar_back_buffer());
    wx_radar_commit_frame(slot, gen, s_times[slot], lat, lon);
    Serial.printf("[wxradar] gen %lu frame %d/%d @%lu (tier=%d, %lu px)\n",
                  (unsigned long)gen, slot + 1, s_availFrames, (unsigned long)s_times[slot],
                  zoomTier, (unsigned long)s_decodedPixels);
    return 1;
}
