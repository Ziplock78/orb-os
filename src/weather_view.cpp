// Standalone animated weather-radar app. Holds up to N decoded RainViewer frames
// in PSRAM and loops them on its own LVGL screen. The fetch is incremental (one
// tile per fetchStep() call, driven from adsb_task) so it never stalls the live feed.
//
// Cross-core safety: frames are DECODED on core 0 (adsb_task) into a private scratch
// buffer, then copied into a frame slot under a mutex. The animation (core 1) copies
// the current slot into its OWN display buffer under the same mutex. So the LVGL
// canvas only ever renders a buffer written by core 1 — no torn/half-written frames.
#include "weather_view.h"
#include <Arduino.h>
#include <lvgl.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include <new>
#include <time.h>
#include <mutex>
#include <string.h>
#include "config.h"
#include "net_fetch.h"
#include "wx_radar.h"    // WX_RADAR_SIZE / WX_RADAR_SOURCE_SIZE
#include "weather.h"     // current-conditions snapshot

namespace {

constexpr int      N        = 8;                       // frames to loop
constexpr int      SZ       = WX_RADAR_SIZE;           // 360 (decoded disc)
constexpr int      SRC      = WX_RADAR_SOURCE_SIZE;    // 512 (source tile)
constexpr size_t   FRAME_BYTES = (size_t)SZ * SZ * sizeof(uint16_t);
constexpr uint32_t ANIM_MS   = 650;                    // per-frame dwell (calmer loop)
constexpr int      HOLD_TICKS = 3;                     // extra dwell on the newest frame (~2s)
constexpr int      WX_ZOOM   = 7;                      // ~100-mile radius on the 360px disc

// ---- frame store (PSRAM) ----
uint16_t  *s_frames[N] = { nullptr };
uint32_t   s_times[N]  = { 0 };
uint16_t  *s_scratch = nullptr;   // core 0 decode target
uint16_t  *s_display = nullptr;   // core 1 canvas buffer
int        s_capacity = 0;
volatile int s_ready  = 0;
std::mutex  s_mx;

// ---- fetch (core 0 only) ----
PNG       *s_png = nullptr;
int        s_decodedRows = 0;   // rows the draw callback actually wrote this decode

// ---- LVGL objects (core 1 only) ----
lv_obj_t *s_screen  = nullptr;
lv_obj_t *s_canvas  = nullptr;
lv_obj_t *s_temp    = nullptr;
lv_obj_t *s_stamp   = nullptr;
lv_obj_t *s_loading = nullptr;
int       s_animIdx = 0;

// ---- decode (mirrors wx_radar_client's proven crop + circle mask) ----
bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) { Serial.println("[wxa] PNG decoder alloc failed"); return false; }
    s_png = new (mem) PNG();
    return true;
}

int wxa_png_line(PNGDRAW *draw) {
    uint16_t *dst = s_scratch;
    if (!dst) return 1;
    const int crop = (SRC - SZ) / 2;
    uint16_t line[SRC];
    if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
        const uint8_t *src = draw->pPixels;
        for (int x = 0; x < draw->iWidth; ++x, src += 4)
            // honor alpha: transparent "no rain" pixels (which carry white RGB) -> nothing
            line[x] = (src[3] < 48) ? 0
                    : (uint16_t)((src[2] >> 3) | ((src[1] >> 2) << 5) | ((src[0] >> 3) << 11));
    } else {
        s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    }
    if (draw->y < crop || draw->y >= crop + SZ) return 1;
    const int outY = draw->y - crop;
    const int c = SZ / 2;
    const int dy = outY - c;
    for (int outX = 0; outX < SZ; ++outX) {
        const int dx = outX - c;
        dst[outY * SZ + outX] = (dx * dx + dy * dy <= (c - 2) * (c - 2)) ? line[outX + crop] : 0;
    }
    s_decodedRows++;
    return 1;
}

// Decode one PNG into the private scratch buffer. Returns true on success.
bool decode_to_scratch(uint8_t *image, size_t len) {
    memset(s_scratch, 0, FRAME_BYTES);
    s_decodedRows = 0;
    const int op = s_png->openRAM(image, len, wxa_png_line);
    if (op != PNG_SUCCESS) { Serial.printf("[wxa] openRAM=%d\n", op); return false; }
    const int w = s_png->getWidth(), h = s_png->getHeight();
    if (w != SRC || h != SRC) {
        Serial.printf("[wxa] dims=%dx%d bpp=%d ct=%d\n", w, h, s_png->getBpp(), s_png->getPixelType());
        s_png->close(); return false;
    }
    // These RainViewer PNGs decode fully but return a non-zero code (trailing chunk),
    // so trust the rows actually drawn, not the return code.
    s_png->decode(nullptr, 0);
    s_png->close();
    return s_decodedRows >= SZ - 2;
}

bool https_get(const char *url, String &body, int timeoutMs) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(4000);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) return false;
    http.addHeader("User-Agent", ADSB_USER_AGENT);
    const int status = http.GET();
    if (status != 200) { http.end(); return false; }
    body = http.getString();
    http.end();
    return body.length() > 0;
}

} // namespace

// ---- fetch: pull the whole frame set in one burst (core 0) -----------------
// Fetching all tiles back-to-back (no live-feed TLS interleaved) keeps the secure
// handshakes from starving each other, which is what made all-but-the-first fail.
// Returns 2 if at least one frame decoded, else 0. Runs ~10-20s, only every 5 min.
int weatherview::fetchStep(double lat, double lon) {
    if (WiFi.status() != WL_CONNECTED || s_capacity == 0 || !s_scratch || !ensure_decoder()) return 0;

    String meta;
    if (!https_get("http://api.rainviewer.com/public/weather-maps.json", meta, 6500)) {
        Serial.println("[wxa] metadata fetch failed"); return 0;
    }
    JsonDocument doc;
    if (deserializeJson(doc, meta)) { Serial.println("[wxa] metadata JSON failed"); return 0; }
    const char *host = doc["host"] | "";
    JsonArrayConst past = doc["radar"]["past"].as<JsonArrayConst>();
    if (!host[0] || past.size() == 0) { Serial.println("[wxa] no frames"); return 0; }

    const int total = (int)past.size();
    const int want  = total < s_capacity ? total : s_capacity;
    const int start = total - want;

    int got = 0, lastGood = -1;
    for (int i = 0; i < want; ++i) {
        JsonObjectConst o = past[start + i].as<JsonObjectConst>();
        const char    *path = o["path"] | "";
        const uint32_t t    = o["time"] | 0;
        char url[320];
        snprintf(url, sizeof(url), "%s%s/512/%d/%.5f/%.5f/2/1_1.png", host, path, WX_ZOOM, lat, lon);

        bool ok = false;
        uint8_t *image = nullptr; size_t len = 0;
        const bool fetched = net_fetch_psram(url, ADSB_USER_AGENT, &image, &len, 220000, 5000, 15000);
        if (fetched) { ok = decode_to_scratch(image, len); heap_caps_free(image); }
        Serial.printf("[wxa] frame %d/%d fetched=%d len=%u ok=%d\n",
                      i + 1, want, fetched, (unsigned)len, ok);
        {
            std::lock_guard<std::mutex> lk(s_mx);
            if (ok)                  { memcpy(s_frames[i], s_scratch, FRAME_BYTES); got++; lastGood = i; }
            else if (lastGood >= 0)    memcpy(s_frames[i], s_frames[lastGood], FRAME_BYTES);  // hold last good
            else                       memset(s_frames[i], 0, FRAME_BYTES);
            s_times[i] = t;
        }
    }

    { std::lock_guard<std::mutex> lk(s_mx); s_ready = want; }
    Serial.printf("[wxa] burst done: %d/%d ok, free psram=%u\n", got, want, (unsigned)ESP.getFreePsram());
    return got > 0 ? 2 : 0;
}

// ---- animation + UI (core 1) -----------------------------------------------
static void update_conditions() {
    WeatherSnapshot w;
    if (weather_get(w) && w.valid) {
        const int f = (int)lroundf(w.tempC * 9.0f / 5.0f + 32.0f);
        char buf[40];
        snprintf(buf, sizeof(buf), "%d°F  %s", f, weather_condition(w.code));
        lv_label_set_text(s_temp, buf);
    } else {
        lv_label_set_text(s_temp, "--");
    }
}

static void anim_cb(lv_timer_t * /*t*/) {
    if (lv_scr_act() != s_screen) return;
    static int hold = 0;

    int ready; long ago; uint32_t ftime;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        ready = s_ready;
        if (ready <= 0) return;                       // still loading
        if (hold > 0) { hold--; return; }             // dwell (extra time on the newest frame)
        s_animIdx = (s_animIdx + 1) % ready;
        memcpy(s_display, s_frames[s_animIdx], FRAME_BYTES);   // stage into the UI-owned buffer
        ftime = s_times[s_animIdx];
        if (s_animIdx == ready - 1) hold = HOLD_TICKS;         // pause on the latest frame
    }

    if (!lv_obj_has_flag(s_loading, LV_OBJ_FLAG_HIDDEN)) {     // first frames arrived
        lv_obj_add_flag(s_loading, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        update_conditions();
    }
    lv_obj_invalidate(s_canvas);

    const time_t now = time(nullptr);
    ago = (long)now - (long)ftime;
    char st[24];
    if (now < 1000000000L)          snprintf(st, sizeof(st), "frame %d/%d", s_animIdx + 1, ready);
    else if (ago < 90)              snprintf(st, sizeof(st), "now");
    else                            snprintf(st, sizeof(st), "-%ld min", (ago + 30) / 60);
    lv_label_set_text(s_stamp, st);
}

void weatherview::onFramesReady() { update_conditions(); }
void weatherview::onPress() { /* reserved */ }

void weatherview::init() {
    for (int i = 0; i < N; ++i) {
        s_frames[i] = (uint16_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_frames[i]) break;
        memset(s_frames[i], 0, FRAME_BYTES);
        s_capacity++;
    }
    s_scratch = (uint16_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_display = (uint16_t *)heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_scratch) memset(s_scratch, 0, FRAME_BYTES);
    if (s_display) memset(s_display, 0, FRAME_BYTES);
    Serial.printf("[wxa] %d/%d frames + scratch=%d display=%d, free psram=%u\n",
                  s_capacity, N, s_scratch != nullptr, s_display != nullptr,
                  (unsigned)ESP.getFreePsram());
    if (!s_scratch || !s_display) s_capacity = 0;   // can't run safely without staging buffers

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    if (s_display) {
        s_canvas = lv_canvas_create(s_screen);
        lv_canvas_set_buffer(s_canvas, s_display, SZ, SZ, LV_IMG_CF_TRUE_COLOR);
        lv_obj_center(s_canvas);
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    // scope: 50 + 100 mile range rings and N/E/S/W compass, over the radar
    const lv_color_t C_RING = lv_color_hex(0xCBD3DD);
    for (int d = 360; d >= 180; d -= 180) {
        lv_obj_t *r = lv_obj_create(s_screen);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, d, d);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(r, C_RING, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_opa(r, LV_OPA_40, 0);
        lv_obj_center(r);
    }
    const char *dirs[4] = { "N", "E", "S", "W" };
    const int   dx[4]   = { 0, 168, 0, -168 };
    const int   dy[4]   = { -168, 0, 168, 0 };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = lv_label_create(s_screen);
        lv_label_set_text(l, dirs[i]);
        lv_obj_set_style_text_color(l, C_RING, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, dx[i], dy[i]);
    }

    lv_obj_t *dot = lv_obj_create(s_screen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 7, 7);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xF2F5F9), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dot, lv_color_black(), 0);
    lv_obj_set_style_border_width(dot, 1, 0);
    lv_obj_center(dot);

    s_temp = lv_label_create(s_screen);
    lv_label_set_text(s_temp, "");
    lv_obj_set_style_text_color(s_temp, lv_color_hex(0xE8ECF1), 0);
    lv_obj_set_style_text_font(s_temp, &lv_font_montserrat_20, 0);
    lv_obj_align(s_temp, LV_ALIGN_CENTER, 0, -150);

    s_stamp = lv_label_create(s_screen);
    lv_label_set_text(s_stamp, "");
    lv_obj_set_style_text_color(s_stamp, lv_color_hex(0x9AA0A6), 0);
    lv_obj_set_style_text_font(s_stamp, &lv_font_montserrat_16, 0);
    lv_obj_align(s_stamp, LV_ALIGN_CENTER, 0, 150);

    lv_obj_t *attrib = lv_label_create(s_screen);
    lv_label_set_text(attrib, "RainViewer");
    lv_obj_set_style_text_color(attrib, lv_color_hex(0x555B62), 0);
    lv_obj_set_style_text_font(attrib, &lv_font_montserrat_12, 0);
    lv_obj_align(attrib, LV_ALIGN_CENTER, 0, 178);

    s_loading = lv_label_create(s_screen);
    lv_label_set_text(s_loading, "ACQUIRING WX RADAR...");
    lv_obj_set_style_text_color(s_loading, lv_color_hex(0x6A7078), 0);
    lv_obj_set_style_text_font(s_loading, &lv_font_montserrat_16, 0);
    lv_obj_center(s_loading);

    lv_timer_create(anim_cb, ANIM_MS, nullptr);
}

lv_obj_t *weatherview::screen() { return s_screen; }
