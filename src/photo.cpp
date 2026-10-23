#include "photo.h"
#include "detail_cache_policy.h"
#include <mutex>
#include <chrono>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

// Four retained thumbnails plus one network decode buffer: at most 354 KiB.
// Buffers are allocated separately and recycled, avoiding a large contiguous pool.
static constexpr int PH_MAXW = 232, PH_MAXH = 156, PH_SLOTS = 4;
static std::mutex s_m;
struct PhotoEntry {
    char hex[10] = {}, credit[192] = {};
    lv_color_t *pixels = nullptr;
    int w = 0, h = 0;
    uint64_t expires = 0, used = 0;
};
static PhotoEntry s_entries[PH_SLOTS];
static lv_color_t *s_buf = nullptr; // network-only scratch; never attached to LVGL
static char s_want[10] = {}, s_wantType[12] = {};
static uint64_t s_used = 0;
static uint64_t now_ms() {
#ifdef ORB_PHOTO_CACHE_TEST
    extern uint64_t photo_test_now_ms();
    return photo_test_now_ms();
#else
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
static void key(const char *in, char *out, size_t size) {
    size_t n = 0;
    for (const char *p = in; p && *p && n + 1 < size; ++p) {
        if (*p == ' ') continue;
        out[n++] = *p >= 'a' && *p <= 'z' ? *p - 'a' + 'A' : *p;
    }
    out[n] = 0;
}
// All entry access and pixel ownership changes happen with s_m held.
static PhotoEntry *find(const char *hex) {
    char normalized[10]; key(hex, normalized, sizeof(normalized));
    if (!normalized[0]) return nullptr;
    for (auto &e : s_entries)
        if (!strcmp(e.hex, normalized) && now_ms() < e.expires) return &e;
    return nullptr;
}
static PhotoEntry *victim(const char *hex) {
    for (auto &e : s_entries) if (!strcmp(e.hex, hex)) return &e;
    for (auto &e : s_entries) if (!e.hex[0] || now_ms() >= e.expires) return &e;
    auto *oldest = &s_entries[0];
    for (auto &e : s_entries) if (e.used < oldest->used) oldest = &e;
    return oldest;
}
void photo_request(const char *hex, const char *type) {
    std::lock_guard<std::mutex> g(s_m);
    key(hex, s_want, sizeof(s_want));
    snprintf(s_wantType, sizeof(s_wantType), "%s", type ? type : "");
    if (auto *e = find(s_want)) {
        e->used = ++s_used;
        printf("[photo-cache] %s hit%s\n", s_want, e->w ? "" : " (unavailable)");
    }
}
bool photo_pending(char *out, size_t n, char *typeOut, size_t typeN) {
    std::lock_guard<std::mutex> g(s_m);
    if (!s_want[0] || find(s_want)) return false;
    snprintf(out, n, "%s", s_want);
    if (typeOut && typeN) snprintf(typeOut, typeN, "%s", s_wantType);
    return true;
}
lv_color_t *photo_buffer(int *mw, int *mh) {
    std::lock_guard<std::mutex> g(s_m);
    if (mw) *mw = PH_MAXW;
    if (mh) *mh = PH_MAXH;
    if (!s_buf) {
        const size_t bytes = PH_MAXW * PH_MAXH * sizeof(lv_color_t);
#if defined(ESP_PLATFORM)
        s_buf = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#else
        s_buf = (lv_color_t *)malloc(bytes);
#endif
        // If memory is tight, recycle the least recently used image instead.
        if (!s_buf) {
            PhotoEntry *oldest = nullptr;
            for (auto &e : s_entries)
                if (e.pixels && (!oldest || e.used < oldest->used)) oldest = &e;
            if (oldest) {
                s_buf = oldest->pixels;
                *oldest = PhotoEntry{};
            }
        }
    }
    return s_buf;
}
void photo_commit(int w, int h, const char *hex, const char *credit) {
    std::lock_guard<std::mutex> g(s_m);
    char normalized[10]; key(hex, normalized, sizeof(normalized));
    if (!normalized[0]) return;
    auto *e = victim(normalized);
    const bool ready = s_buf && w > 0 && h > 0 && w <= PH_MAXW && h <= PH_MAXH;
    if (ready) {
        auto *reuse = e->pixels;
        e->pixels = s_buf;
        s_buf = reuse; // recycle the evicted slot for the next decode
    }
    snprintf(e->hex, sizeof(e->hex), "%s", normalized);
    snprintf(e->credit, sizeof(e->credit), "%s", credit ? credit : "");
    e->w = ready ? w : 0; e->h = ready ? h : 0;
    e->expires = now_ms() + (ready ? DETAIL_CACHE_TTL_MS : DETAIL_CACHE_MISS_TTL_MS);
    e->used = ++s_used;
    printf("[photo-cache] %s stored%s\n", normalized, ready ? " (15 min)" : " (unavailable, 1 min)");
}
bool photo_get(const char *hex, int *w, int *h, char *credit, size_t cn) {
    std::lock_guard<std::mutex> g(s_m);
    auto *e = find(hex);
    if (!e || !e->w) return false;
    if (w) *w = e->w;
    if (h) *h = e->h;
    if (credit && cn) snprintf(credit, cn, "%s", e->credit);
    e->used = ++s_used;
    return true;
}
bool photo_done(const char *hex) {
    std::lock_guard<std::mutex> g(s_m);
    return find(hex) != nullptr;
}
bool photo_copy(const char *hex, lv_color_t *dst, size_t pixels, int *w, int *h, char *credit, size_t cn) {
    std::lock_guard<std::mutex> g(s_m);
    auto *e = find(hex);
    if (!e || !e->w || !dst || pixels < (size_t)e->w * e->h) return false;
    memcpy(dst, e->pixels, (size_t)e->w * e->h * sizeof(lv_color_t));
    if (w) *w = e->w;
    if (h) *h = e->h;
    if (credit && cn) snprintf(credit, cn, "%s", e->credit);
    e->used = ++s_used;
    return true;
}
