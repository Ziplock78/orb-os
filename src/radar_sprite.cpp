#include "radar_sprite.h"
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
#include "custom_radar_plate.h"
#include "custom_radar_overlay.h"
#include "custom_radar_blip.h"
#include "custom_radar_static.h"
#include "custom_radar_sweep.h"
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

// One shared line callback; decodes are sequential (boot/push-time), never concurrent.
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

bool decode(const uint8_t *png, uint32_t len, bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    if (!ensure_decoder()) { Serial.printf("[radar_sprite] %s: decoder alloc failed\n", tag); return false; }
    s_alpha = alpha;
    if (s_png->openRAM((uint8_t *)png, len, line_cb) != PNG_SUCCESS) { Serial.printf("[radar_sprite] %s: open failed\n", tag); return false; }
    w = s_png->getWidth(); h = s_png->getHeight();
    const size_t bytes = (size_t)w * h * (alpha ? 3 : 2);
    out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { Serial.printf("[radar_sprite] %s: buffer alloc failed\n", tag); s_png->close(); return false; }
    s_buf = out; s_w = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { Serial.printf("[radar_sprite] %s: decode failed\n", tag); return false; }
    Serial.printf("[radar_sprite] %s: decoded %dx%d (%u KB)\n", tag, w, h, (unsigned)(bytes / 1024));
    return true;
}

// SD-hosted plate/overlay for the active theme (theme_select::activeSlug()),
// tried before the flash-baked PNG. No slug selected, or any SD failure
// (missing file, bad PNG), falls straight through to whatever
// CUSTOM_HAS_RADAR_PLATE/OVERLAY already resolves to.
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
            if (ok) { Serial.printf("[radar_sprite] %s: source SD %s\n", tag, path); return true; }
        }
    }
    if (!flashPng) return false;
    return decode(flashPng, flashLen, alpha, out, w, h, tag);
}

// Flash first, card second. Deliberately the same signature as decode_sd_first() above,
// so every call site below is just a rename: a hit costs nothing at all (no SD read, no
// PNG decode, no PSRAM), and a miss falls through to exactly the previous behaviour.
bool load_asset(const char *assetName, const uint8_t *flashPng, uint32_t flashLen,
                bool alpha, uint8_t *&out, int &w, int &h, const char *tag) {
    if (const uint8_t *p = theme_art::find_active(
            assetName, alpha ? theme_art::FMT_RGB565_ALPHA : theme_art::FMT_RGB565, w, h)) {
        out = (uint8_t *)p;
        Serial.printf("[radar_sprite] %s: flash-resident %dx%d (0 ms, 0 KB PSRAM)\n", tag, w, h);
        return true;
    }
    return decode_sd_first(assetName, flashPng, flashLen, alpha, out, w, h, tag);
}

uint8_t     *s_plateBuf = nullptr;   lv_img_dsc_t s_plateDsc;   bool s_plateTried = false;
uint8_t     *s_overlayBuf = nullptr; lv_img_dsc_t s_overlayDsc; bool s_overlayTried = false;
uint8_t     *s_blipBuf = nullptr;    lv_img_dsc_t s_blipDsc;    bool s_blipTried = false;
uint8_t     *s_staticBuf[2] = { nullptr, nullptr }; lv_img_dsc_t s_staticDsc[2]; bool s_staticTried[2] = { false, false };
uint8_t     *s_sweepBuf = nullptr; lv_img_dsc_t s_sweepDsc; bool s_sweepTried = false;

} // namespace

const lv_img_dsc_t *radar_custom_plate() {
    if (!s_plateTried) {
        s_plateTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_RADAR_PLATE
        const bool ok = load_asset("radar_plate.png", CUSTOM_RADAR_PLATE_PNG, CUSTOM_RADAR_PLATE_PNG_LEN, false, s_plateBuf, w, h, "plate");
#else
        const bool ok = load_asset("radar_plate.png", nullptr, 0, false, s_plateBuf, w, h, "plate");
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

const lv_img_dsc_t *radar_custom_overlay() {
    if (!s_overlayTried) {
        s_overlayTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_RADAR_OVERLAY
        const bool ok = load_asset("radar_overlay.png", CUSTOM_RADAR_OVERLAY_PNG, CUSTOM_RADAR_OVERLAY_PNG_LEN, true, s_overlayBuf, w, h, "overlay");
#else
        const bool ok = load_asset("radar_overlay.png", nullptr, 0, true, s_overlayBuf, w, h, "overlay");
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

// The aircraft-icon blip type's sprite: real alpha already baked in by the
// editor's export step (blend-mode extraction happens client-side, so this is
// a plain alpha PNG same as the overlay), rotated to heading and recolored by
// altitude band at draw time in radar_view.cpp's draw_custom_ac() — this just
// decodes the pixels once.
const lv_img_dsc_t *radar_custom_blip_icon() {
    if (!s_blipTried) {
        s_blipTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_RADAR_BLIP_IMAGE
        const bool ok = load_asset("radar_blip.png", CUSTOM_RADAR_BLIP_PNG, CUSTOM_RADAR_BLIP_PNG_LEN, true, s_blipBuf, w, h, "blip");
#else
        const bool ok = load_asset("radar_blip.png", nullptr, 0, true, s_blipBuf, w, h, "blip");
#endif
        if (ok) {
            s_blipDsc.header.always_zero = 0;
            s_blipDsc.header.w = w;
            s_blipDsc.header.h = h;
            s_blipDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            s_blipDsc.data_size = (uint32_t)w * h * 3;
            s_blipDsc.data = s_blipBuf;
        }
    }
    if (s_blipBuf) return &s_blipDsc;
    return nullptr;
}

// The two plain decorative overlays (Static 1/2): no rotation/pivot, just an
// image — same decode shape as the overlay (RGB565+alpha), SD-first with a
// flash-baked fallback (custom_radar_static.h), position/opacity applied by
// radar_view.cpp from theme_style::radar().static1/static2, not baked here.
const lv_img_dsc_t *radar_custom_static(int idx) {
    if (idx < 0 || idx > 1) return nullptr;
    if (!s_staticTried[idx]) {
        s_staticTried[idx] = true;
        int w = 0, h = 0;
        bool ok;
        if (idx == 0) {
#if CUSTOM_HAS_RADAR_STATIC1
            ok = load_asset("radar_static1.png", CUSTOM_RADAR_STATIC1_PNG, CUSTOM_RADAR_STATIC1_PNG_LEN, true, s_staticBuf[0], w, h, "static1");
#else
            ok = load_asset("radar_static1.png", nullptr, 0, true, s_staticBuf[0], w, h, "static1");
#endif
        } else {
#if CUSTOM_HAS_RADAR_STATIC2
            ok = load_asset("radar_static2.png", CUSTOM_RADAR_STATIC2_PNG, CUSTOM_RADAR_STATIC2_PNG_LEN, true, s_staticBuf[1], w, h, "static2");
#else
            ok = load_asset("radar_static2.png", nullptr, 0, true, s_staticBuf[1], w, h, "static2");
#endif
        }
        if (ok) {
            s_staticDsc[idx].header.always_zero = 0;
            s_staticDsc[idx].header.w = w;
            s_staticDsc[idx].header.h = h;
            s_staticDsc[idx].header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            s_staticDsc[idx].data_size = (uint32_t)w * h * 3;
            s_staticDsc[idx].data = s_staticBuf[idx];
        }
    }
    if (s_staticBuf[idx]) return &s_staticDsc[idx];
    return nullptr;
}

// The sweep's "image" type: one baked sprite, rotated live in radar_view.cpp
// around its own pivot (CUSTOM_SWEEP_IMAGE_PIVOT_X/Y, CUSTOM_SWEEP_IMAGE_CENTER_X/Y
// — compile-time, coupled to this exact sprite, same as the blip icon's pivot).
const lv_img_dsc_t *radar_custom_sweep() {
    if (!s_sweepTried) {
        s_sweepTried = true;
        int w = 0, h = 0;
#if CUSTOM_HAS_SWEEP_IMAGE
        const bool ok = load_asset("radar_sweep.png", CUSTOM_SWEEP_IMAGE_PNG, CUSTOM_SWEEP_IMAGE_PNG_LEN, true, s_sweepBuf, w, h, "sweep");
#else
        const bool ok = load_asset("radar_sweep.png", nullptr, 0, true, s_sweepBuf, w, h, "sweep");
#endif
        if (ok) {
            s_sweepDsc.header.always_zero = 0;
            s_sweepDsc.header.w = w;
            s_sweepDsc.header.h = h;
            s_sweepDsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
            s_sweepDsc.data_size = (uint32_t)w * h * 3;
            s_sweepDsc.data = s_sweepBuf;
        }
    }
    if (s_sweepBuf) return &s_sweepDsc;
    return nullptr;
}

// Drop the decoded PSRAM buffers and reset the "tried" flags so the next call
// re-decodes from the flash-resident PNG bytes (.rodata, never freed). Called
// when Flight Tracker is no longer the app on screen, mirroring custom_sprite's
// clock-face release so a design's decoded pixels aren't held resident while
// some other app is in front.
void radar_sprite_release() {
    // theme_art::owns() means the pixels are memory-mapped flash, never an allocation:
    // freeing that would be a wild pointer into the partition.
    if (s_plateBuf)   { if (!theme_art::owns(s_plateBuf))   heap_caps_free(s_plateBuf);   s_plateBuf = nullptr; }
    if (s_overlayBuf) { if (!theme_art::owns(s_overlayBuf)) heap_caps_free(s_overlayBuf); s_overlayBuf = nullptr; }
    if (s_blipBuf)    { if (!theme_art::owns(s_blipBuf))    heap_caps_free(s_blipBuf);    s_blipBuf = nullptr; }
    for (int i = 0; i < 2; ++i) if (s_staticBuf[i]) {
        if (!theme_art::owns(s_staticBuf[i])) heap_caps_free(s_staticBuf[i]);
        s_staticBuf[i] = nullptr;
    }
    if (s_sweepBuf) { if (!theme_art::owns(s_sweepBuf)) heap_caps_free(s_sweepBuf); s_sweepBuf = nullptr; }
    s_plateTried = s_overlayTried = s_blipTried = false;
    s_staticTried[0] = s_staticTried[1] = false;
    s_sweepTried = false;
}
