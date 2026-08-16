#include "splash_art.h"
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
static uint32_t millis() {
    using namespace std::chrono;
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#endif
// Before PNGdec on purpose: it bundles zlib, whose `#define local static` leaks and
// breaks the `bool local` parameter in lvgl's lv_meter.h if lvgl is included after.
#include "theme_style.h"      // hasAsset() — ignore files the theme does not declare
#include "theme_art.h"        // pre-baked RGB565 in flash — no read, no decode, no PSRAM
#include <PNGdec.h>
#include <new>
#include <string.h>
#include "splash_png_default.h"
#include "splash_png_office.h"
#include "custom_splash.h"   // CUSTOM_HAS_SPLASH / CUSTOM_SPLASH_PNG(_LEN) — a Launch Kit flash-push, one rung below SD
#include "theme_sd.h"         // theme_sd::read_whole/free — shared SD-file helper, see its header for the portability story
#include "theme_select.h"     // theme_select::activeSlug() — which /themes/<slug>/ folder to read from

namespace {

constexpr int SZ = 466;
PNG      *s_png = nullptr;
uint16_t *s_buf  = nullptr;   // decode target, PSRAM — valid until the next splash_art_decode() call

bool ensure() {
    if (!s_png) {
        void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!mem) return false;
        s_png = new (mem) PNG();
    }
    if (!s_buf) s_buf = (uint16_t *)heap_caps_malloc((size_t)SZ * SZ * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return s_buf != nullptr;
}

int line_cb(PNGDRAW *draw) {
    uint16_t line[SZ];
    s_png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
    memcpy(s_buf + draw->y * SZ, line, SZ * sizeof(uint16_t));
    return 1;
}

// Attempt to decode one PNG (already in RAM) into s_buf. Leaves s_buf
// untouched on failure — caller falls through to the next source.
bool try_decode(const uint8_t *png, uint32_t len) {
    if (!png || !len) return false;
    if (s_png->openRAM((uint8_t *)png, len, line_cb) != PNG_SUCCESS) {
        Serial.printf("[splash] PNG open failed\n");
        return false;
    }
    if (s_png->getWidth() != SZ || s_png->getHeight() != SZ) {
        Serial.printf("[splash] unexpected dimensions (%dx%d)\n", s_png->getWidth(), s_png->getHeight());
        s_png->close();
        return false;
    }
    const int decoded = s_png->decode(nullptr, 0);
    s_png->close();
    if (decoded != PNG_SUCCESS) { Serial.printf("[splash] PNG decode failed\n"); return false; }
    return true;
}

// SD-hosted theme splash. Phase 1 of moving theme art off flash and onto the
// microSD card — see data/documents/plans/orb-sd-theme-architecture-plan.md.
// Path is /themes/<slug>/splash.png, slug picked by the Settings "Design" page
// (theme_select) and persisted across reboot. No slug selected (nothing
// installed yet, or a fresh device) skips SD entirely and falls straight to
// the flash-baked/stock path below — same "never a hard failure, just a worse
// picture" contract every other custom-art screen already follows.
constexpr size_t SD_SPLASH_MAX_BYTES = 2 * 1024 * 1024;   // sanity ceiling; a 466x466 PNG is never remotely this big

} // namespace

bool splash_art_decode(bool office, lv_img_dsc_t *out) {
    // 0) Pre-baked in flash: no card read, no decode, and — because ensure() is skipped —
    // not even the 424 KB decode buffer. Checked before ensure() for exactly that reason.
    {
        int fw = 0, fh = 0;
        if (const uint8_t *p = theme_art::find_active("splash.png", theme_art::FMT_RGB565, fw, fh)) {
            if (fw == SZ && fh == SZ) {
                out->header.always_zero = 0;
                out->header.w  = SZ;
                out->header.h  = SZ;
                out->header.cf = LV_IMG_CF_TRUE_COLOR;
                out->data_size = (uint32_t)SZ * SZ * 2;
                out->data      = p;
                Serial.printf("[splash] flash-resident %dx%d (0 ms, 0 KB PSRAM)\n", fw, fh);
                return true;
            }
        }
    }

    if (!ensure()) { Serial.printf("[splash] PSRAM alloc failed\n"); return false; }

    bool ok = false;

    // 1) SD-hosted theme splash, if a theme's selected and the file's there.
    const char *slug = theme_select::activeSlug();
    if (slug[0] && theme_style::hasAsset("splash.png")) {
        char path[64];
        snprintf(path, sizeof(path), "/themes/%s/splash.png", slug);
        size_t sdLen = 0;
        uint8_t *sdBuf = theme_sd::read_whole(path, sdLen, SD_SPLASH_MAX_BYTES);
        if (sdBuf) {
            const uint32_t t0 = millis();
            ok = try_decode(sdBuf, (uint32_t)sdLen);
            theme_sd::free(sdBuf);   // only the raw compressed bytes — s_buf (decoded RGB565) stays alive for the caller either way
            if (ok) Serial.printf("[splash] decoded from SD %s (%u bytes) in %u ms\n", path, (unsigned)sdLen, (unsigned)(millis() - t0));
        }
    }

    // 2) Fall back to whatever's flash-baked (a Launch Kit push) or, failing
    // that, the stock office/default art — unchanged from before this file
    // grew an SD path.
    if (!ok) {
#if CUSTOM_HAS_SPLASH
        ok = try_decode(CUSTOM_SPLASH_PNG, CUSTOM_SPLASH_PNG_LEN);
#else
        ok = try_decode(office ? SPLASH_PNG_OFFICE     : SPLASH_PNG_DEFAULT,
                         office ? SPLASH_PNG_OFFICE_LEN : SPLASH_PNG_DEFAULT_LEN);
#endif
    }

    if (!ok) return false;

    out->header.always_zero = 0;
    out->header.w  = SZ;
    out->header.h  = SZ;
    out->header.cf = LV_IMG_CF_TRUE_COLOR;
    out->data_size = (uint32_t)SZ * SZ * 2;
    out->data      = (const uint8_t *)s_buf;
    return true;
}
