// One-time PNG -> raw RGB565 conversion, writing the result into the `themeart` flash
// partition so nothing has to be read or unpacked while the device is in use.
//
// Runs at boot, only when the active theme has no baked assets yet, which in practice
// means "the first boot after a theme push" — the reboot the user is already waiting
// through. Everything here is best-effort: an asset that fails to read, fails to decode,
// or does not fit simply stays on the SD path, which still works exactly as before.
#include "theme_art.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <PNGdec.h>
#include <esp_heap_caps.h>
#include "theme_sd.h"
#include "theme_select.h"

namespace theme_art {
namespace {

// Priority order, most-frequently-shown first. The partition holds roughly 3.4 MB and a
// full-screen asset is 424 KB opaque / 636 KB with alpha, so a rich theme will not fit
// entirely. Baking in this order means the cheap wins land first and whatever spills over
// is the artwork shown least often. install_asset() logs each skip.
struct Asset { const char *name; bool alpha; };
const Asset ASSETS[] = {
    { "menu_plate.png",        false },   // every menu open — the whole reason for this
    { "settings_plate.png",    false },
    { "radar_plate.png",       false },
    { "clock_plate.png",       false },
    { "clock_overlay.png",     true  },
    { "clock_hand_hour.png",   true  },
    { "clock_hand_minute.png", true  },
    { "clock_hand_second.png", true  },
    { "radar_sweep.png",       true  },
    { "radar_blip.png",        true  },
    { "radar_static1.png",     true  },
    { "radar_static2.png",     true  },
    { "clock_static1.png",     true  },
    { "clock_static2.png",     true  },
    { "menu_overlay.png",      true  },
    { "settings_overlay.png",  true  },
    { "splash.png",            false },   // boot only, so it loses the space race by design
};
constexpr size_t ASSET_N = sizeof(ASSETS) / sizeof(ASSETS[0]);
constexpr size_t SD_ASSET_MAX_BYTES = 2 * 1024 * 1024;

PNG     *s_png   = nullptr;
uint8_t *s_buf   = nullptr;
int      s_w     = 0;
bool     s_alpha = false;

// Byte-for-byte the same conversion as custom_sprite.cpp's line_cb. It has to be: the
// baked bytes are handed to LVGL in place of that function's output, so any difference
// would show up as wrong colours with no other symptom.
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

bool ensure_decoder() {
    if (s_png) return true;
    void *mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) return false;
    s_png = new (mem) PNG();
    return true;
}

// Decode one PNG into a fresh PSRAM buffer. Caller frees.
bool decode_png(const uint8_t *png, size_t len, bool alpha,
                uint8_t *&out, int &w, int &h) {
    if (!ensure_decoder()) return false;
    s_alpha = alpha;
    if (s_png->openRAM((uint8_t *)png, (int)len, line_cb) != PNG_SUCCESS) return false;
    w = s_png->getWidth();
    h = s_png->getHeight();
    const size_t bytes = (size_t)w * h * (alpha ? 3 : 2);
    out = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) { s_png->close(); return false; }
    s_buf = out;
    s_w   = w;
    const int r = s_png->decode(nullptr, 0);
    s_png->close();
    if (r != PNG_SUCCESS) { heap_caps_free(out); out = nullptr; return false; }
    return true;
}

} // namespace

bool bake_active_theme() {
    const char *slug = theme_select::activeSlug();
    if (!slug || !slug[0]) return false;
    if (!space_total()) return false;              // no partition on this layout
    if (slug_baked(slug)) {
        Serial.printf("[theme_art] '%s' already baked — nothing to do\n", slug);
        return false;
    }

    Serial.printf("[theme_art] baking '%s' into flash (one time, this boot only)\n", slug);
    const uint32_t t0 = millis();
    if (!install_begin()) { Serial.println("[theme_art] install_begin failed — staying on SD"); return false; }

    int baked = 0;
    for (size_t i = 0; i < ASSET_N; ++i) {
        char path[80];
        snprintf(path, sizeof(path), "/themes/%s/%s", slug, ASSETS[i].name);
        size_t pngLen = 0;
        uint8_t *pngBuf = theme_sd::read_whole(path, pngLen, SD_ASSET_MAX_BYTES);
        if (!pngBuf) continue;                     // asset not part of this theme: normal

        uint8_t *raw = nullptr;
        int w = 0, h = 0;
        const bool ok = decode_png(pngBuf, pngLen, ASSETS[i].alpha, raw, w, h);
        theme_sd::free(pngBuf);
        if (!ok) { Serial.printf("[theme_art] %s: decode failed, leaving on SD\n", ASSETS[i].name); continue; }

        const size_t bytes = (size_t)w * h * (ASSETS[i].alpha ? 3 : 2);
        if (install_asset(slug, ASSETS[i].name, w, h,
                          ASSETS[i].alpha ? FMT_RGB565_ALPHA : FMT_RGB565, raw, bytes)) {
            ++baked;
            Serial.printf("[theme_art] baked %-22s %dx%d %u KB\n",
                          ASSETS[i].name, w, h, (unsigned)(bytes / 1024));
        }
        heap_caps_free(raw);
    }

    if (!baked) { Serial.println("[theme_art] nothing baked"); return false; }
    if (!install_commit()) { Serial.println("[theme_art] commit failed — staying on SD"); return false; }
    Serial.printf("[theme_art] baked %d asset(s) in %u ms — subsequent shows are free\n",
                  baked, (unsigned)(millis() - t0));
    return true;
}

} // namespace theme_art

#else

namespace theme_art { bool bake_active_theme() { return false; } }

#endif
