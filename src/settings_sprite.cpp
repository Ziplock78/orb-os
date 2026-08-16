#include "settings_sprite.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
static void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <new>
#include <string.h>
#include "custom_settings_plate.h"
#include "custom_settings_overlay.h"
#include "theme_sd.h"   // theme_sd::read_whole/free — SD-hosted plate/overlay, one rung above flash
#include "theme_select.h"   // theme_select::activeSlug() — which /themes/<slug>/ folder to read from

namespace {

PNG *s_png = nullptr;
bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_png = new (mem) PNG();
    return true;
}

uint8_t *s_buf = nullptr;
int      s_w = 0;
bool     s_alpha = false;

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

bool decode(const uint8_t *png, uint32_t len, bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    if (!ensure_decoder()) { Serial.printf("[settings_sprite] %s: decoder alloc failed\n", tag); return false; }
    s_alpha = alpha;
    if (s_png->openRAM((uint8_t *)png, len, line_cb) != PNG_SUCCESS) { Serial.printf("[settings_sprite] %s: open failed\n", tag); return false; }
    w = s_png->getWidth(); h = s_png->getHeight();
    const size_t bytes = (size_t)w * h * (alpha ? 3 : 2);
    out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { Serial.printf("[settings_sprite] %s: buffer alloc failed\n", tag); s_png->close(); return false; }
    s_buf = out; s_w = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { Serial.printf("[settings_sprite] %s: decode failed\n", tag); return false; }
    Serial.printf("[settings_sprite] %s: decoded %dx%d (%u KB)\n", tag, w, h, (unsigned)(bytes / 1024));
    return true;
}

// SD-hosted plate/overlay for the active theme (theme_select::activeSlug()),
// tried before the flash-baked PNG. No slug selected, or any SD failure
// (missing file, bad PNG), falls straight through to whatever
// CUSTOM_HAS_SETTINGS_PLATE/OVERLAY already resolves to.
constexpr size_t SD_ASSET_MAX_BYTES = 2 * 1024 * 1024;
bool decode_sd_first(const char *assetName, const uint8_t *flashPng, uint32_t flashLen, bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    const char *slug = theme_select::activeSlug();
    if (slug[0]) {
        char path[64];
        snprintf(path, sizeof(path), "/themes/%s/%s", slug, assetName);
        size_t sdLen = 0;
        uint8_t *sdBuf = theme_sd::read_whole(path, sdLen, SD_ASSET_MAX_BYTES);
        if (sdBuf) {
            const bool ok = decode(sdBuf, (uint32_t)sdLen, alpha, out, w, h, tag);
            theme_sd::free(sdBuf);
            if (ok) { Serial.printf("[settings_sprite] %s: source SD %s\n", tag, path); return true; }
        }
    }
    if (!flashPng) return false;
    return decode(flashPng, flashLen, alpha, out, w, h, tag);
}

uint8_t     *s_plateBuf = nullptr;   lv_img_dsc_t s_plateDsc;   bool s_plateTried = false;
uint8_t     *s_overlayBuf = nullptr; lv_img_dsc_t s_overlayDsc; bool s_overlayTried = false;

} // namespace

const lv_img_dsc_t *settings_custom_plate() {
    if (!s_plateTried) {
        s_plateTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_SETTINGS_PLATE
        const bool ok = decode_sd_first("settings_plate.png", CUSTOM_SETTINGS_PLATE_PNG, CUSTOM_SETTINGS_PLATE_PNG_LEN, false, s_plateBuf, w, h, "plate");
#else
        const bool ok = decode_sd_first("settings_plate.png", nullptr, 0, false, s_plateBuf, w, h, "plate");
#endif
        if (ok) {
            s_plateDsc.header.always_zero = 0;
            s_plateDsc.header.w = w;
            s_plateDsc.header.h = h;
            s_plateDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            s_plateDsc.data_size = (uint32_t)w * h * 2;
            s_plateDsc.data = s_plateBuf;
        }
    }
    if (s_plateBuf) return &s_plateDsc;
    return nullptr;
}

const lv_img_dsc_t *settings_custom_overlay() {
    if (!s_overlayTried) {
        s_overlayTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_SETTINGS_OVERLAY
        const bool ok = decode_sd_first("settings_overlay.png", CUSTOM_SETTINGS_OVERLAY_PNG, CUSTOM_SETTINGS_OVERLAY_PNG_LEN, true, s_overlayBuf, w, h, "overlay");
#else
        const bool ok = decode_sd_first("settings_overlay.png", nullptr, 0, true, s_overlayBuf, w, h, "overlay");
#endif
        if (ok) {
            s_overlayDsc.header.always_zero = 0;
            s_overlayDsc.header.w = w;
            s_overlayDsc.header.h = h;
            s_overlayDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            s_overlayDsc.data_size = (uint32_t)w * h * 3;
            s_overlayDsc.data = s_overlayBuf;
        }
    }
    if (s_overlayBuf) return &s_overlayDsc;
    return nullptr;
}

void settings_sprite_release() {
    if (s_plateBuf)   { heap_caps_free(s_plateBuf);   s_plateBuf = nullptr; }
    if (s_overlayBuf) { heap_caps_free(s_overlayBuf); s_overlayBuf = nullptr; }
    s_plateTried = s_overlayTried = false;
}
