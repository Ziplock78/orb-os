// Location Info app. A baked cream Aviator dial (DIAL_LOC) with live text drawn on
// top: city/state/country, coordinates, temperature, barometric pressure, elevation,
// and a one-line "INTEL" fact. Three free HTTPS sources, fetched on core0:
//   - api.bigdatacloud.net  reverse geocode  -> city, state, country
//   - api.open-meteo.com    forecast         -> temperature, surface pressure, elevation
//   - en.wikipedia.org      page summary     -> the INTEL fun fact (first sentence)
#include "location_view.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#else
// Desktop/native build (no ESP32 core): shim the Arduino-only calls this file uses
// outside the HTTP transport itself (which goes through native_http.h/curl instead
// of WiFiClientSecure/HTTPClient — see http_json() below). Same pattern as
// clock_view.cpp / wx_radar_client.cpp. The fetch STATE MACHINE (startRefresh/pump/
// step_geo/step_wx/trivia) is shared, unguarded, identical on both platforms — only
// the transport differs, so the simulator hits the exact same three real endpoints
// the device does.
#include "native_http.h"
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } void println(const char *s) const { puts(s); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static struct { unsigned getFreeHeap() const { return 0; } } ESP;   // desktop: no heap-pressure logging needed
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include "config.h"
#include "dial_loc.h"      // DIAL_LOC — blank Aviator dial, 466x466 RGB565

namespace {
    using LocInfo = locationview::LocInfo;

    LocInfo s_info = {};
#ifdef ARDUINO
    SemaphoreHandle_t s_mutex = nullptr;
#endif
    volatile bool     s_dirty   = false;      // fetch (core0) has new data for the UI (core1)
    volatile bool     s_want    = true;       // a refresh has been requested

    lv_obj_t   *s_screen = nullptr;
    lv_obj_t   *s_canvas = nullptr;
    lv_color_t *s_buf    = nullptr;

    // dark sepia inks that read on aged cream
    const lv_color_t COL_INK  = LV_COLOR_MAKE(0x24, 0x1E, 0x14);
    const lv_color_t COL_DIM  = LV_COLOR_MAKE(0x5A, 0x4E, 0x3A);
    const lv_color_t COL_BLACK= LV_COLOR_MAKE(0x00, 0x00, 0x00);

    // vertical layout (top-y of each text row); tuned on-device like the clock date
    constexpr int Y_CITY   = 118;
    constexpr int Y_REGION = 176;
    constexpr int Y_COORD  = 208;
    constexpr int Y_DATA1  = 238;
    constexpr int Y_DATA2  = 266;
    constexpr int Y_INTEL_LABEL = 294;
    constexpr int Y_INTEL  = 314;
    constexpr int INTEL_W  = 256;   // keep the wrapped fact inside the round face

    // Copy src -> dst uppercased (for the stamped all-caps city/state look).
    void upper(const char *src, char *dst, size_t n) {
        size_t i = 0;
        for (; src[i] && i < n - 1; ++i) dst[i] = (char)toupper((unsigned char)src[i]);
        dst[i] = '\0';
    }

    void draw_line(int y, const lv_font_t *font, lv_color_t col, const char *txt) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.color = col;
        d.font  = font;
        d.align = LV_TEXT_ALIGN_CENTER;
        lv_canvas_draw_text(s_canvas, 0, y, SCREEN_W, &d, txt);
    }

    // Wrapped, centred INTEL block (LVGL wraps within the given width).
    void draw_intel(int y, const char *txt) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.color = COL_INK;
        d.font  = &lv_font_montserrat_16;
        d.align = LV_TEXT_ALIGN_CENTER;
        lv_canvas_draw_text(s_canvas, (SCREEN_W - INTEL_W) / 2, y, INTEL_W, &d, txt);
    }

    void redraw() {
        if (!s_canvas || !s_buf) return;
        LocInfo info;
#ifdef ARDUINO
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            info = s_info;
            xSemaphoreGive(s_mutex);
        } else return;
#else
        info = s_info;
#endif

        memcpy(s_buf, DIAL_LOC, sizeof(DIAL_LOC));

        if (!info.valid) {
            draw_line(Y_CITY, &lv_font_montserrat_28, COL_DIM, "ACQUIRING");
            draw_line(Y_REGION, &lv_font_montserrat_16, COL_DIM, "reading position...");
            lv_obj_invalidate(s_canvas);
            return;
        }

        // city, stamped all-caps (shrink the font for long names so it never overflows)
        char cityU[40]; upper(info.city, cityU, sizeof(cityU));
        const lv_font_t *cityFont = (strlen(cityU) > 9) ? &lv_font_montserrat_28
                                                        : &lv_font_montserrat_48;
        const int cityY = (cityFont == &lv_font_montserrat_28) ? Y_CITY + 12 : Y_CITY;
        draw_line(cityY, cityFont, COL_INK, cityU);

        char regionU[40], rc[56];
        upper(info.region, regionU, sizeof(regionU));
        if (info.country[0]) snprintf(rc, sizeof(rc), "%s, %s", regionU, info.country);
        else                 snprintf(rc, sizeof(rc), "%s", regionU);
        draw_line(Y_REGION, &lv_font_montserrat_20, COL_DIM, rc);

        char coord[48];
        snprintf(coord, sizeof(coord), "LAT %.2f %c     LON %.2f %c",
                 fabs(info.lat), info.lat >= 0 ? 'N' : 'S',
                 fabs(info.lon), info.lon >= 0 ? 'E' : 'W');
        draw_line(Y_COORD, &lv_font_montserrat_18, COL_INK, coord);

        char d1[48], d2[32];
        if (info.hasWx) {
            snprintf(d1, sizeof(d1), "TEMP %d F      BARO %.2f inHg", info.tempF, info.pressInHg);
            snprintf(d2, sizeof(d2), "ALT %d ft", info.altFt);
        } else {
            snprintf(d1, sizeof(d1), "TEMP --      BARO --");
            snprintf(d2, sizeof(d2), "ALT --");
        }
        draw_line(Y_DATA1, &lv_font_montserrat_18, COL_INK, d1);
        draw_line(Y_DATA2, &lv_font_montserrat_18, COL_INK, d2);

        if (info.intel[0]) {
            draw_line(Y_INTEL_LABEL, &lv_font_montserrat_14, COL_DIM, "INTEL");
            draw_intel(Y_INTEL, info.intel);
        }

        lv_obj_invalidate(s_canvas);
    }
}

namespace {
    // --- one step = one HTTPS call, run from pump() on successive adsb_task cycles ---

    // A refresh runs ONE HTTPS call per adsb_task cycle (three back-to-back TLS
    // handshakes exhaust the fragmented internal heap; every fetcher here does one).
    // The desktop simulator has no such constraint, but drives pump() the same way
    // (see locationview::pumpUntilDone) so the two platforms share this exact code.
    enum Step { STEP_IDLE, STEP_GEO, STEP_WX, STEP_TRIVIA };
    Step    s_step  = STEP_IDLE;
    int     s_triviaTry = 0;                    // Wikivoyage title attempt (0..2), one per pump
    LocInfo s_stage = {};                      // built up across the steps, committed at the end

#ifdef ARDUINO
    bool http_json(const char *url, JsonDocument &doc, const JsonDocument *filter) {
        // Scheme-aware, because this helper serves three endpoints and they no longer agree:
        // the two open-meteo/bigdatacloud lookups moved to plain HTTP so they stop dying on
        // the TLS memory limit (see ADSB_PRIMARY_TLS in config.h), while Wikivoyage redirects
        // HTTP to HTTPS and so has no choice. Picking off the URL keeps that a per-endpoint
        // fact rather than one this function has to be told.
        const bool tls = (strncmp(url, "https://", 8) == 0);
        WiFiClient       plain;
        WiFiClientSecure secure;
        WiFiClient      *client = &plain;
        if (tls) { secure.setInsecure(); client = &secure; }
        HTTPClient http;
        http.setConnectTimeout(4000);
        http.setTimeout(6000);
        http.setUserAgent("CapsuleRadar/1.0 (esp32; contact: device)");
        if (!http.begin(*client, url)) return false;
        const int code = http.GET();
        if (code != 200) { Serial.printf("[locinfo] HTTP %d\n", code); http.end(); return false; }
        // Read the whole body first (handles chunked transfer-encoding, which a raw
        // streaming parse chokes on — that was silently failing the weather call).
        String body = http.getString();
        http.end();
        DeserializationError err = filter
            ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
            : deserializeJson(doc, body);
        if (err) Serial.printf("[locinfo] json parse: %s\n", err.c_str());
        return !err;
    }
#else
    bool http_json(const char *url, JsonDocument &doc, const JsonDocument *filter) {
        std::string body;
        if (!native_https_get(url, "CapsuleRadar/1.0 (esp32; contact: device)", body, 6000)) return false;
        DeserializationError err = filter
            ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
            : deserializeJson(doc, body);
        if (err) Serial.printf("[locinfo] json parse: %s\n", err.c_str());
        return !err;
    }
#endif

    // First sentence of a Wikipedia extract, trimmed to fit the dial (~2 lines).
    void first_sentence(const char *src, char *dst, size_t dstN) {
        size_t i = 0;
        bool ended = false;
        for (; src[i] && i < dstN - 1; ++i) {
            dst[i] = src[i];
            if (src[i] == '.' && (src[i + 1] == ' ' || src[i + 1] == '\0')) { ++i; ended = true; break; }
        }
        dst[i] = '\0';
        if (!ended && src[i] != '\0') {             // truncated: back up to a word break, add ellipsis
            int k = (int)i - 1;
            while (k > 0 && dst[k] != ' ') --k;
            if (k > 0) dst[k] = '\0';
            strncat(dst, "...", dstN - strlen(dst) - 1);
        }
    }

    void step_geo(double lat, double lon) {
        char url[224];
        snprintf(url, sizeof(url),
                 "http://api.bigdatacloud.net/data/reverse-geocode-client?latitude=%.5f&longitude=%.5f&localityLanguage=en",
                 lat, lon);
        // Filter to just the four fields we use — the full response carries a large
        // localityInfo array that would blow the fragmented internal heap on parse.
        JsonDocument filter;
        filter["city"] = true;
        filter["locality"] = true;
        filter["principalSubdivision"] = true;
        filter["countryCode"] = true;
        JsonDocument doc;
        if (http_json(url, doc, &filter)) {
            const char *city = doc["city"] | "";
            if (!city[0]) city = doc["locality"] | "";
            snprintf(s_stage.city,   sizeof(s_stage.city),   "%s", city);
            snprintf(s_stage.region, sizeof(s_stage.region), "%s", (const char *)(doc["principalSubdivision"] | ""));
            const char *cc = doc["countryCode"] | "";
            if      (!strcmp(cc, "US")) snprintf(s_stage.country, sizeof(s_stage.country), "USA");
            else if (!strcmp(cc, "GB")) snprintf(s_stage.country, sizeof(s_stage.country), "UK");
            else                        snprintf(s_stage.country, sizeof(s_stage.country), "%s", cc);
            Serial.printf("[locinfo] geo ok: %s (heap %u)\n", s_stage.city, (unsigned)ESP.getFreeHeap());
        } else {
            Serial.printf("[locinfo] geo failed (heap %u)\n", (unsigned)ESP.getFreeHeap());
        }
    }

    void step_wx(double lat, double lon) {
        char url[224];
        snprintf(url, sizeof(url),
                 "http://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,surface_pressure&temperature_unit=fahrenheit",
                 lat, lon);
        JsonDocument doc;
        if (http_json(url, doc, nullptr)) {
            const float tF   = doc["current"]["temperature_2m"] | NAN;
            const float phPa = doc["current"]["surface_pressure"] | NAN;
            const float elev = doc["elevation"] | NAN;
            if (!isnan(tF) && !isnan(phPa)) {
                s_stage.tempF     = (int)lroundf(tF);
                s_stage.pressInHg = phPa * 0.02952998f;              // hPa -> inHg (true local barometric)
                s_stage.altFt     = isnan(elev) ? 0 : (int)lroundf(elev * 3.280840f);
                s_stage.hasWx     = true;
                Serial.printf("[locinfo] wx ok: %dF %.2finHg %dft (heap %u)\n",
                              s_stage.tempF, s_stage.pressInHg, s_stage.altFt, (unsigned)ESP.getFreeHeap());
                return;
            }
        }
        Serial.printf("[locinfo] wx failed (heap %u)\n", (unsigned)ESP.getFreeHeap());
    }

    // Percent-encode a page title for a REST path (spaces -> underscores, parens kept).
    void url_encode(const char *title, char *enc, size_t n) {
        int j = 0;
        for (int i = 0; title[i] && j < (int)n - 4; ++i) {
            char c = title[i];
            if (c == ' ') enc[j++] = '_';
            else if (isalnum((unsigned char)c) || strchr("_-.()", c)) enc[j++] = c;
            else { snprintf(enc + j, 4, "%%%02X", (unsigned char)c); j += 3; }
        }
        enc[j] = '\0';
    }

    // Title for Wikivoyage trivia attempt `idx`. Wikivoyage titles are inconsistent
    // (Cave Creek is bare, Ithaca needs "(New York)"), so try a few, then the state.
    bool trivia_title(int idx, char *out, size_t n) {
        const bool hasR = s_stage.region[0] != '\0';
        if (hasR) {
            if (idx == 0) { snprintf(out, n, "%s (%s)", s_stage.city, s_stage.region); return true; }
            if (idx == 1) { snprintf(out, n, "%s", s_stage.city);   return s_stage.city[0]; }
            if (idx == 2) { snprintf(out, n, "%s", s_stage.region); return true; }
        } else {
            if (idx == 0) { snprintf(out, n, "%s", s_stage.city);   return s_stage.city[0]; }
        }
        return false;
    }

    // One Wikivoyage lookup. Returns true if it produced an INTEL sentence.
    bool trivia_attempt(int idx) {
        char title[80];
        if (!trivia_title(idx, title, sizeof(title)) || !title[0]) return false;
        char enc[160]; url_encode(title, enc, sizeof(enc));
        char url[256];
        snprintf(url, sizeof(url),
                 "https://en.wikivoyage.org/api/rest_v1/page/summary/%s?redirect=true", enc);
        JsonDocument filter; filter["type"] = true; filter["extract"] = true;
        JsonDocument doc;
        if (!http_json(url, doc, &filter)) return false;
        const char *type = doc["type"] | "";
        const char *ex   = doc["extract"] | "";
        if (strcmp(type, "standard") != 0 || !ex[0]) return false;
        if (!strncmp(ex, "There is more than", 18) || strstr(ex, "may refer to")) return false;
        first_sentence(ex, s_stage.intel, 90);   // up to ~3 lines at INTEL_W
        Serial.printf("[locinfo] trivia ok via '%s'\n", title);
        return true;
    }

    void commit() {
        s_stage.valid = (s_stage.city[0] != '\0') || s_stage.hasWx;
        if (!s_stage.valid) { Serial.println("[locinfo] cycle produced no data"); return; }
#ifdef ARDUINO
        if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            s_info = s_stage;
            xSemaphoreGive(s_mutex);
        }
#else
        s_info = s_stage;
#endif
        s_dirty = true;
        Serial.printf("[locinfo] committed: %s, %s %s\n", s_stage.city, s_stage.region, s_stage.country);
    }
}

void locationview::startRefresh() {
    if (s_step != STEP_IDLE) return;            // a cycle is already running
    s_stage = LocInfo{};
    s_triviaTry = 0;
    s_step  = STEP_GEO;
}

void locationview::pump(double lat, double lon) {
    switch (s_step) {
        case STEP_IDLE:  return;
        case STEP_GEO:   s_stage.lat = lat; s_stage.lon = lon;
                         step_geo(lat, lon);  s_step = STEP_WX; break;
        case STEP_WX:    step_wx(lat, lon);   s_step = STEP_TRIVIA; s_triviaTry = 0; break;
        case STEP_TRIVIA: {                    // one Wikivoyage title per pump; stop on a hit or after 3
            const bool hit = trivia_attempt(s_triviaTry);
            if (hit || s_triviaTry >= 2) { commit(); s_step = STEP_IDLE; }
            else                         { s_triviaTry++; }
            break;
        }
    }
}

// Desktop simulator convenience: the device spreads a refresh cycle over several
// adsb_task cycles to avoid back-to-back TLS handshakes (see the Step comment above);
// the sim has no such constraint, so just pump() until the cycle finishes. Bounded to
// 8 iterations (worst case: geo + wx + 3 trivia attempts = 5) so a stuck state machine
// can't loop forever.
void locationview::pumpUntilDone(double lat, double lon) {
    startRefresh();
    for (int i = 0; i < 8; ++i) {
        pump(lat, lon);
    }
}

bool locationview::hasData() { return s_info.valid; }

bool locationview::takeRefresh() {
    if (!s_want) return false;
    s_want = false;
    return true;
}

void locationview::onEnter() {
    s_want = true;      // ask adsb_task to (re)fetch for the current location
    redraw();           // paint what we have now (or ACQUIRING)
}

void locationview::onPress() {
    s_want = true;      // push = manual refresh
}

void locationview::debugSet(const LocInfo &info) {
    s_info = info;
    s_dirty = true;
    redraw();
}

static void poll_cb(lv_timer_t * /*t*/) {
    if (s_dirty) { s_dirty = false; redraw(); }
}

void locationview::init() {
#ifdef ARDUINO
    s_mutex = xSemaphoreCreateMutex();
#endif

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    const size_t bufBytes = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    s_buf = (lv_color_t *)heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf) {
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_buf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
    } else {
        Serial.println("[locinfo] PSRAM alloc for canvas failed");
    }

    redraw();                                   // initial ACQUIRING frame
    lv_timer_create(poll_cb, 400, nullptr);     // pick up fetched data on the LVGL thread
}

lv_obj_t *locationview::screen() { return s_screen; }
