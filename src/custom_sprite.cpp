#include "custom_sprite.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
// Before PNGdec on purpose: it bundles zlib, whose `#define local static` leaks and
// breaks the `bool local` parameter in lvgl's lv_meter.h if lvgl is included after.
#include "theme_style.h"   // hasAsset() — ignore files the theme does not declare
#include <PNGdec.h>
#include <new>
#include <string.h>
#include "config.h"   // SCREEN_W / SCREEN_H — the fixed plate/overlay canvas size
#include "custom_plate.h"
#include "custom_overlay.h"
#include "custom_hands.h"
#include "theme_sd.h"   // theme_sd::read_whole/free — SD-hosted plate/overlay, one rung above flash
#include "theme_select.h"   // theme_select::activeSlug() — which /themes/<slug>/ folder to read from
#include "theme_art.h"      // pre-baked RGB565 in flash — tried before the card, costs nothing

namespace {

PNG *s_png = nullptr;
bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_png = new (mem) PNG();
    return true;
}

// One shared line callback; decodes are sequential (boot-time), never concurrent.
// The editor exports RGBA PNGs, so PNG_PIXEL_TRUECOLOR_ALPHA @ 8bpp is expected.
uint8_t *s_buf = nullptr;
int      s_w = 0;
bool     s_alpha = false;   // true -> 3 B/px (lo,hi,alpha); false -> 2 B/px RGB565

int line_cb(PNGDRAW *draw) {
    const uint8_t *src = draw->pPixels;
    const bool rgba = (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8);
    if (s_alpha) {
        uint8_t *dst = s_buf + (size_t)draw->y * s_w * 3;
        for (int x = 0; x < draw->iWidth; ++x, dst += 3) {
            if (rgba) { const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3)); dst[0] = v & 0xFF; dst[1] = v >> 8; dst[2] = src[3]; src += 4; }
            else { dst[0] = dst[1] = dst[2] = 0; }
        }
    } else {
        uint16_t *dst = (uint16_t *)s_buf + (size_t)draw->y * s_w;
        for (int x = 0; x < draw->iWidth; ++x) {
            if (rgba) { dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3)); src += 4; }
            else dst[x] = 0;
        }
    }
    return 1;
}

// Decode a PNG byte array into a fresh PSRAM buffer. alpha picks the pixel format.
// Timed and logged (not guessed) so re-entry cost after custom_sprite_release() can
// be read straight off the serial console rather than estimated.
bool decode(const uint8_t *png, uint32_t len, bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    const uint32_t t0 = millis();
    if (!ensure_decoder()) { Serial.printf("[custom_sprite] %s: decoder alloc failed\n", tag); return false; }
    s_alpha = alpha;
    if (s_png->openRAM((uint8_t *)png, len, line_cb) != PNG_SUCCESS) { Serial.printf("[custom_sprite] %s: open failed\n", tag); return false; }
    w = s_png->getWidth(); h = s_png->getHeight();
    const size_t bytes = (size_t)w * h * (alpha ? 3 : 2);
    out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { Serial.printf("[custom_sprite] %s: buffer alloc failed\n", tag); s_png->close(); return false; }
    s_buf = out; s_w = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { Serial.printf("[custom_sprite] %s: decode failed\n", tag); return false; }
    Serial.printf("[custom_sprite] %s: decoded %dx%d (%u KB) in %u ms\n", tag, w, h, (unsigned)(bytes / 1024), (unsigned)(millis() - t0));
    return true;
}

// SD-hosted plate/overlay for the active theme (theme_select::activeSlug()),
// tried before the flash-baked PNG below. No slug selected, or any SD failure
// (missing file, bad PNG), falls straight through to whatever
// CUSTOM_HAS_PLATE/OVERLAY already resolves to, unchanged.
constexpr size_t SD_ASSET_MAX_BYTES = 2 * 1024 * 1024;   // a 466x466 plate/overlay PNG is never remotely this big
bool decode_sd_first(const char *assetName, const uint8_t *flashPng, uint32_t flashLen, bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    const char *slug = theme_select::activeSlug();
    // Only read what the theme says it ships. A push never deletes from the card, so
    // files from older pushes linger; trusting them meant decoding and drawing layers
    // the theme had already dropped. See theme_style::hasAsset().
    if (slug[0] && theme_style::hasAsset(assetName)) {
        char path[64];
        snprintf(path, sizeof(path), "/themes/%s/%s", slug, assetName);
        size_t sdLen = 0;
        uint8_t *sdBuf = theme_sd::read_whole(path, sdLen, SD_ASSET_MAX_BYTES);
        if (sdBuf) {
            const bool ok = decode(sdBuf, (uint32_t)sdLen, alpha, out, w, h, tag);
            theme_sd::free(sdBuf);
            if (ok) { Serial.printf("[custom_sprite] %s: source SD %s\n", tag, path); return true; }
            Serial.printf("[custom_sprite] %s: SD file %s read (%u B) but DECODE FAILED\n",
                          tag, path, (unsigned)sdLen);
        } else {
            Serial.printf("[custom_sprite] %s: no SD file at %s\n", tag, path);
        }
    } else {
        Serial.printf("[custom_sprite] %s: no active theme slug, skipping SD\n", tag);
    }
    if (!flashPng) {
        Serial.printf("[custom_sprite] %s: no flash fallback either — nothing to draw\n", tag);
        return false;
    }
    return decode(flashPng, flashLen, alpha, out, w, h, tag);
}

uint16_t *s_plate = nullptr;   bool s_plateTried = false;
uint8_t  *s_overlay = nullptr; bool s_overlayTried = false;
// 0=hour,1=minute,2=second,3=static1,4=static2 — the two statics share this exact
// same slot/decode machinery, just always drawn at angle 0 (see clock_view.cpp).
uint8_t  *s_hand[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
int       s_handW[5] = { 0, 0, 0, 0, 0 }, s_handH[5] = { 0, 0, 0, 0, 0 };
bool      s_handTried[5] = { false, false, false, false, false };

} // namespace

const uint16_t *custom_plate() {
    if (!s_plate && !s_plateTried) {
        s_plateTried = true;
        int w = 0, h = 0;
        // Pre-baked in flash? Then there is nothing to do at all: no 226 ms card read,
        // no 205 ms decode, no 424 KB of PSRAM. See theme_art.h.
        if (const uint8_t *p = theme_art::find_active("clock_plate.png", theme_art::FMT_RGB565, w, h)) {
            s_plate = (uint16_t *)p;
            Serial.printf("[custom_sprite] plate: flash-resident %dx%d (0 ms, 0 KB PSRAM)\n", w, h);
            return s_plate;
        }
        uint8_t *o = nullptr;
#if CUSTOM_HAS_PLATE
        if (decode_sd_first("clock_plate.png", CUSTOM_PLATE_PNG, CUSTOM_PLATE_PNG_LEN, false, o, w, h, "plate")) s_plate = (uint16_t *)o;
#else
        if (decode_sd_first("clock_plate.png", nullptr, 0, false, o, w, h, "plate")) s_plate = (uint16_t *)o;
#endif
    }
    return s_plate;
}

const uint8_t *custom_overlay() {
    if (!s_overlay && !s_overlayTried) {
        s_overlayTried = true;
        int w = 0, h = 0;
        if (const uint8_t *p = theme_art::find_active("clock_overlay.png", theme_art::FMT_RGB565_ALPHA, w, h)) {
            s_overlay = (uint8_t *)p;
            Serial.printf("[custom_sprite] overlay: flash-resident %dx%d (0 ms, 0 KB PSRAM)\n", w, h);
            return s_overlay;
        }
        uint8_t *o = nullptr;
#if CUSTOM_HAS_OVERLAY
        if (decode_sd_first("clock_overlay.png", CUSTOM_OVERLAY_PNG, CUSTOM_OVERLAY_PNG_LEN, true, o, w, h, "overlay")) s_overlay = o;
#else
        if (decode_sd_first("clock_overlay.png", nullptr, 0, true, o, w, h, "overlay")) s_overlay = o;
#endif
    }
    return s_overlay;
}

CustomSprite custom_hand(int kind) {
    if (kind < 0 || kind > 4) return { nullptr, 0, 0 };
    if (!s_handTried[kind]) {
        s_handTried[kind] = true;
        const uint8_t *png = nullptr; uint32_t len = 0;
#if CUSTOM_HAS_HOUR
        if (kind == 0) { png = CUSTOM_HOUR_PNG; len = CUSTOM_HOUR_PNG_LEN; }
#endif
#if CUSTOM_HAS_MINUTE
        if (kind == 1) { png = CUSTOM_MINUTE_PNG; len = CUSTOM_MINUTE_PNG_LEN; }
#endif
#if CUSTOM_HAS_SECOND
        if (kind == 2) { png = CUSTOM_SECOND_PNG; len = CUSTOM_SECOND_PNG_LEN; }
#endif
#if CUSTOM_HAS_STATIC1
        if (kind == 3) { png = CUSTOM_STATIC1_PNG; len = CUSTOM_STATIC1_PNG_LEN; }
#endif
#if CUSTOM_HAS_STATIC2
        if (kind == 4) { png = CUSTOM_STATIC2_PNG; len = CUSTOM_STATIC2_PNG_LEN; }
#endif
        // SD first, flash as fallback, same contract as plate/overlay. Hands were the
        // last visual element that could not travel per theme, which is why a theme
        // switch used to leave the previous theme's hands on the new clock face.
        static const char *sdName[5] = {
            "clock_hand_hour.png", "clock_hand_minute.png", "clock_hand_second.png",
            "clock_static1.png",   "clock_static2.png",
        };
        int w = 0, h = 0;
        if (const uint8_t *p = theme_art::find_active(sdName[kind], theme_art::FMT_RGB565_ALPHA, w, h)) {
            s_hand[kind] = (uint8_t *)p; s_handW[kind] = w; s_handH[kind] = h;
            return { s_hand[kind], s_handW[kind], s_handH[kind] };
        }
        uint8_t *o = nullptr;
        if (decode_sd_first(sdName[kind], png, len, true, o, w, h, "hand")) {
            s_hand[kind] = o; s_handW[kind] = w; s_handH[kind] = h;
        }
    }
    return { s_hand[kind], s_handW[kind], s_handH[kind] };
}

// Drop every decoded PSRAM buffer and reset the "tried" flags so the next call to
// custom_plate()/custom_overlay()/custom_hand() re-decodes from the flash-resident
// PNG bytes (which are never freed — they're .rodata, not a runtime allocation).
// Called when the custom clock face is no longer the app on screen, so a design's
// ~1 MB of decoded pixels isn't held resident while some other app is in front.
void custom_sprite_release() {
    const uint32_t t0 = millis();
    size_t freed = 0;
    // theme_art::owns() means the pixels live in memory-mapped flash: nothing was
    // allocated, so the reference is dropped rather than freed.
    if (s_plate)   { if (!theme_art::owns(s_plate))   { freed += (size_t)SCREEN_W * SCREEN_H * 2; heap_caps_free(s_plate); }   s_plate = nullptr; }
    if (s_overlay) { if (!theme_art::owns(s_overlay)) { freed += (size_t)SCREEN_W * SCREEN_H * 3; heap_caps_free(s_overlay); } s_overlay = nullptr; }
    for (int i = 0; i < 5; ++i) if (s_hand[i]) {
        if (!theme_art::owns(s_hand[i])) {
            freed += (size_t)s_handW[i] * s_handH[i] * 3;
            heap_caps_free(s_hand[i]);
        }
        s_hand[i] = nullptr; s_handW[i] = 0; s_handH[i] = 0;
    }
    s_plateTried = s_overlayTried = false;
    for (int i = 0; i < 5; ++i) s_handTried[i] = false;
    if (freed) Serial.printf("[custom_sprite] released ~%u KB of decoded PSRAM in %u ms\n", (unsigned)(freed / 1024), (unsigned)(millis() - t0));
}
