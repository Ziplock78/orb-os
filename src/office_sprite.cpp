#include "office_sprite.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#else
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
static struct { void printf(const char *fmt, ...) const { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); } } Serial;
static void *heap_caps_malloc(size_t sz, int) { return malloc(sz); }
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
#include <PNGdec.h>
#include <new>
#include <string.h>
#include "office_minute_png.h"
#include "office_minute_img_meta.h"
#include "office_hour_png.h"
#include "office_hour_img_meta.h"

namespace {

PNG *s_png = nullptr;

bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_png = new (mem) PNG();
    return true;
}

// Decodes are sequential (never concurrent), so one shared line callback driven by
// whichever target is "current" is safe — set right before each openRAM()/decode() pair.
uint8_t *s_curBuf   = nullptr;
int      s_curWidth = 0;

int line_cb(PNGDRAW *draw) {
    uint8_t *dst = s_curBuf + (size_t)draw->y * s_curWidth * 3;
    if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
        const uint8_t *src = draw->pPixels;
        for (int x = 0; x < draw->iWidth; ++x, src += 4, dst += 3) {
            const uint16_t v = (uint16_t)(((src[0] & 0xF8) << 8) | ((src[1] & 0xFC) << 3) | (src[2] >> 3));
            dst[0] = (uint8_t)(v & 0xFF); dst[1] = (uint8_t)(v >> 8); dst[2] = src[3];
        }
    } else {
        memset(dst, 0, (size_t)draw->iWidth * 3);   // shouldn't happen — source always has alpha
    }
    return 1;
}

// Shared by both sprites: decode `pngData` (len bytes, must be w x h) into a fresh PSRAM
// RGB565+alpha buffer and fill in `dsc`. `buf`/`dsc`/`ready` are the caller's persistent
// (static) storage, passed by reference so each sprite keeps its own.
bool decode_into(const uint8_t *pngData, uint32_t pngLen, int w, int h,
                 uint8_t *&buf, lv_img_dsc_t &dsc, bool &ready, const char *tag) {
    if (ready) return true;
    if (!ensure_decoder()) { Serial.printf("[office_sprite] %s: PSRAM alloc (decoder) failed\n", tag); return false; }
    if (!buf) buf = (uint8_t *)heap_caps_malloc((size_t)w * h * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { Serial.printf("[office_sprite] %s: PSRAM alloc (buffer) failed\n", tag); return false; }

    s_curBuf = buf; s_curWidth = w;
    if (s_png->openRAM((uint8_t *)pngData, pngLen, line_cb) != PNG_SUCCESS) {
        Serial.printf("[office_sprite] %s: PNG open failed\n", tag); return false;
    }
    if (s_png->getWidth() != w || s_png->getHeight() != h) {
        Serial.printf("[office_sprite] %s: unexpected dimensions\n", tag); s_png->close(); return false;
    }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    if (decoded != PNG_SUCCESS) { Serial.printf("[office_sprite] %s: PNG decode failed\n", tag); return false; }

    dsc.header.always_zero = 0;
    dsc.header.w  = w;
    dsc.header.h  = h;
    dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    dsc.data_size = (uint32_t)w * h * 3;
    dsc.data      = buf;
    ready = true;
    return true;
}

uint8_t     *s_minuteBuf   = nullptr;
lv_img_dsc_t s_minuteDsc;
bool         s_minuteReady = false;

uint8_t     *s_hourBuf   = nullptr;
lv_img_dsc_t s_hourDsc;
bool         s_hourReady = false;

} // namespace

const lv_img_dsc_t *office_minute_sprite() {
    if (!decode_into(OFFICE_MINUTE_PNG, OFFICE_MINUTE_PNG_LEN, OFFICE_MINUTE_IMG_W, OFFICE_MINUTE_IMG_H,
                     s_minuteBuf, s_minuteDsc, s_minuteReady, "minute"))
        return nullptr;
    return &s_minuteDsc;
}

const lv_img_dsc_t *office_hour_sprite() {
    if (!decode_into(OFFICE_HOUR_PNG, OFFICE_HOUR_PNG_LEN, OFFICE_HOUR_IMG_W, OFFICE_HOUR_IMG_H,
                     s_hourBuf, s_hourDsc, s_hourReady, "hour"))
        return nullptr;
    return &s_hourDsc;
}
