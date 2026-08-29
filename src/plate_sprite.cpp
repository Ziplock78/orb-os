#include "plate_sprite.h"
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
#include "theme_sd.h"
#include "theme_select.h"
#include "theme_style.h"
#include "theme_art.h"

namespace {

PNG *s_png = nullptr;
bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_png = new (mem) PNG();
    return true;
}

// The decoder writes one row at a time through a callback with no user pointer, so these
// have to be file scope. Only ever set immediately before a decode and only ever read
// during it, and every caller is on the UI thread.
uint8_t *s_buf = nullptr;
int      s_w = 0;

int line_cb(PNGDRAW *draw) {
    const uint8_t *src = draw->pPixels;
    const bool rgba = (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8);
    uint16_t *dst = (uint16_t *)s_buf + (size_t)draw->y * s_w;
    for (int x = 0; x < draw->iWidth; ++x) {
        if (rgba) { dst[x] = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3)); src += 4; }
        else dst[x] = 0;
    }
    return 1;
}

constexpr size_t SD_ASSET_MAX_BYTES = 2 * 1024 * 1024;

bool decode(const uint8_t *png, uint32_t len, uint8_t *&out, int &w, int &h, const char *tag) {
    if (!ensure_decoder()) { Serial.printf("[%s] decoder alloc failed\n", tag); return false; }
    if (s_png->openRAM((uint8_t *)png, len, line_cb) != PNG_SUCCESS) { Serial.printf("[%s] open failed\n", tag); return false; }
    w = s_png->getWidth(); h = s_png->getHeight();
    const size_t bytes = (size_t)w * h * 2;
    out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { Serial.printf("[%s] buffer alloc failed\n", tag); s_png->close(); return false; }
    s_buf = out; s_w = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { Serial.printf("[%s] decode failed\n", tag); return false; }
    Serial.printf("[%s] decoded %dx%d (%u KB)\n", tag, w, h, (unsigned)(bytes / 1024));
    return true;
}

// Flash first, then the card, then nothing. hasAsset() is what stops a file left behind by
// an older push of the same theme being drawn after the design dropped it.
bool load_asset(const char *assetName, uint8_t *&out, int &w, int &h, const char *tag) {
    const uint8_t *p = nullptr;
    theme_art::Format fmt = theme_art::FMT_RGB565;
    if (theme_art::lookup(theme_select::activeSlug(), assetName, p, w, h, fmt)) {
        if (fmt == theme_art::FMT_RGB565) {
            out = (uint8_t *)p;
            Serial.printf("[%s] flash-resident %dx%d (0 ms, 0 KB PSRAM)\n", tag, w, h);
            return true;
        }
    }
    const char *slug = theme_select::activeSlug();
    if (!slug[0] || !theme_style::hasAsset(assetName)) return false;
    char path[64];
    snprintf(path, sizeof(path), "/themes/%s/%s", slug, assetName);
    size_t sdLen = 0;
    uint8_t *sdBuf = theme_sd::read_whole(path, sdLen, SD_ASSET_MAX_BYTES);
    if (!sdBuf) return false;
    const bool ok = decode(sdBuf, (uint32_t)sdLen, out, w, h, tag);
    theme_sd::free(sdBuf);
    if (ok) Serial.printf("[%s] source SD %s\n", tag, path);
    return ok;
}

}  // namespace

const lv_img_dsc_t *plate_sprite::get(Plate &p) {
    if (!p.tried) {
        p.tried = true;
        int w = 0, h = 0;
        if (load_asset(p.asset, p.buf, w, h, p.tag)) {
            p.dsc.header.always_zero = 0;
            p.dsc.header.w  = w;
            p.dsc.header.h  = h;
            p.dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
            p.dsc.data_size = (uint32_t)w * h * 2;
            p.dsc.data      = p.buf;
        }
    }
    return p.buf ? &p.dsc : nullptr;
}

void plate_sprite::release(Plate &p) {
    // theme_art::owns() means the pixels are memory-mapped flash rather than an allocation:
    // freeing that would be a wild pointer into the partition.
    if (p.buf) {
        if (!theme_art::owns(p.buf)) heap_caps_free(p.buf);
        p.buf = nullptr;
    }
    p.tried = false;
}
