#include "theme_sd.h"
#ifdef ARDUINO
#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include "sdcard.h"
#else
#include <cstdio>
#include <cstdlib>
#include <string>
#endif

namespace theme_sd {

#ifdef ARDUINO
uint8_t *read_whole(const char *path, size_t &outLen, size_t maxBytes) {
    outLen = 0;
    if (!sdcard::mounted()) return nullptr;
    File f = SD.open(path, "r");
    if (!f || f.isDirectory()) { if (f) f.close(); return nullptr; }
    const size_t sz = f.size();
    if (sz == 0 || sz > maxBytes) { f.close(); return nullptr; }
    uint8_t *buf = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { f.close(); return nullptr; }
    const size_t got = f.read(buf, sz);
    f.close();
    if (got != sz) { heap_caps_free(buf); return nullptr; }
    outLen = sz;
    return buf;
}
void free(uint8_t *buf) { if (buf) heap_caps_free(buf); }
#else
namespace {
constexpr const char *SIM_SD_ROOT = "sim/sdcard";   // same stand-in root as roads_sd
}
uint8_t *read_whole(const char *path, size_t &outLen, size_t maxBytes) {
    outLen = 0;
    const std::string full = std::string(SIM_SD_ROOT) + path;
    FILE *f = fopen(full.c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > maxBytes) { fclose(f); return nullptr; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return nullptr; }
    const size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { ::free(buf); return nullptr; }
    outLen = (size_t)sz;
    return buf;
}
void free(uint8_t *buf) { if (buf) ::free(buf); }
#endif

} // namespace theme_sd
