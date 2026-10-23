// Shared route state. std::mutex works on both ESP32 (Arduino/FreeRTOS) and the
// native simulator, so the same code guards the cross-thread access on the device.
#include "route.h"
#include <string.h>
#include <stdio.h>
#include <mutex>
#include <chrono>
#include <new>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

static std::mutex s_m;
static char s_want[12]     = "";   // callsign the UI asked about
struct RouteEntry {
    char call[12] = {}, from[40] = {}, to[40] = {};
    uint64_t expiresMs = 0, used = 0;
};
static constexpr size_t ROUTE_MEMORY_ENTRIES = 16;
static uint64_t s_used = 0;
static size_t s_capacity = ROUTE_MEMORY_ENTRIES;
// Accessed only with s_m held. Keep this small working set off the scarce
// internal heap; an allocation failure still permits one cached route.
static RouteEntry *route_entries() {
#ifdef ARDUINO
    static RouteEntry fallback;
    static RouteEntry *entries = [] {
        auto *p = static_cast<RouteEntry *>(heap_caps_calloc(
            ROUTE_MEMORY_ENTRIES, sizeof(RouteEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!p) { s_capacity = 1; return &fallback; }
        for (size_t i = 0; i < ROUTE_MEMORY_ENTRIES; ++i) new (p + i) RouteEntry{};
        return p;
    }();
#else
    static RouteEntry entries[ROUTE_MEMORY_ENTRIES];
#endif
    return entries;
}

static void route_key(const char *call, char *out, size_t size) {
    size_t n = 0;
    for (const char *p = call; p && *p && n + 1 < size; ++p) {
        if (*p == ' ') continue;
        out[n++] = *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
    }
    out[n] = 0;
}
static uint64_t route_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static RouteEntry *route_find(const char *call) {
    char key[12]; route_key(call, key, sizeof(key));
    if (!key[0]) return nullptr;
    auto *entries = route_entries();
    const uint64_t now = route_now_ms();
    for (size_t i = 0; i < s_capacity; ++i)
        if (!strcmp(entries[i].call, key) && now < entries[i].expiresMs) return &entries[i];
    return nullptr;
}

void route_request(const char *callsign) {
    std::lock_guard<std::mutex> g(s_m);
    route_key(callsign, s_want, sizeof(s_want));
}

bool route_pending(char *callOut, size_t n) {
    std::lock_guard<std::mutex> g(s_m);
    if (s_want[0] && !route_find(s_want)) {
        snprintf(callOut, n, "%s", s_want);
        return true;
    }
    return false;
}

// Fold UTF-8 Latin accents (á, ñ, ü...) to ASCII so the LVGL font can render them
// (Montserrat has no accented glyphs -> they would show as a missing-glyph box).
static void ascii_fold(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; in && in[i] && o + 1 < n;) {
        const unsigned char c = (unsigned char)in[i];
        if (c < 0x80) { out[o++] = in[i++]; continue; }
        if (c == 0xC3 && in[i + 1]) {            // Latin-1 Supplement
            const unsigned char d = (unsigned char)in[i + 1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else if (d == 0x9F)              r = 's';   // ß
            else                             r = '?';
            out[o++] = r; i += 2; continue;
        }
        ++i;                                     // other multibyte: skip the sequence
        while ((unsigned char)in[i] >= 0x80 && (unsigned char)in[i] < 0xC0) ++i;
    }
    out[o] = 0;
}

void route_store(const char *callsign, const char *from, const char *to, uint32_t ttlMs) {
    std::lock_guard<std::mutex> g(s_m);
    char key[12]; route_key(callsign, key, sizeof(key));
    if (!key[0]) return;
    auto *entries = route_entries();
    RouteEntry *entry = nullptr;
    const uint64_t now = route_now_ms();
    for (size_t i = 0; i < s_capacity; ++i) {
        if (!strcmp(entries[i].call, key)) { entry = &entries[i]; break; }
    }
    if (!entry) {
        entry = &entries[0];
        for (size_t i = 0; i < s_capacity; ++i) {
            if (now >= entries[i].expiresMs) { entry = &entries[i]; break; }
            if (entries[i].used < entry->used) entry = &entries[i];
        }
    }
    snprintf(entry->call, sizeof(entry->call), "%s", key);
    ascii_fold(from, entry->from, sizeof(entry->from));
    ascii_fold(to, entry->to, sizeof(entry->to));
    // Do not retry an unavailable route on every frame, or retain it forever.
    entry->expiresMs = now + (entry->from[0] && entry->to[0] ? ttlMs : DETAIL_CACHE_MISS_TTL_MS);
    entry->used = ++s_used;
}

bool route_get(const char *callsign, char *from, size_t fn, char *to, size_t tn) {
    std::lock_guard<std::mutex> g(s_m);
    if (auto *entry = route_find(callsign)) {
        snprintf(from, fn, "%s", entry->from);
        snprintf(to, tn, "%s", entry->to);
        entry->used = ++s_used;
        return true;
    }
    return false;
}
