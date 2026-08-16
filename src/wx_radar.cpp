#include "wx_radar.h"
#include <mutex>
#include <stdlib.h>
#include <string.h>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#include <Arduino.h>
#endif

// Nine frame buffers (one per past frame in the animation loop) plus one scratch buffer
// the client decodes into. The client decodes+composites a frame into the scratch, then
// wx_radar_commit_frame() copies it into a slot — the slot buffers keep stable identities
// (never handed back out as scratch), so a pointer the UI is currently displaying stays
// valid until that exact slot is re-committed on the next cycle. All buffers live in
// PSRAM: 10 * 360*360*2 ~= 2.5MB, trivial against the ~8MB pool, and keeps this churn off
// the small internal heap the TLS handshakes fight over.
static std::mutex s_mutex;
static uint16_t *s_frames[WX_RADAR_FRAMES] = { nullptr };
static uint32_t  s_slotGen[WX_RADAR_FRAMES] = { 0 };
static uint32_t  s_slotTime[WX_RADAR_FRAMES] = { 0 };
static uint16_t *s_back = nullptr;
static uint32_t  s_latestGen = 0;
static uint32_t  s_version = 0;
static double    s_lat = 0, s_lon = 0;
static bool      s_haveCenter = false;

static uint16_t *alloc_pixels(void) {
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
#ifdef ARDUINO
    return (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return (uint16_t *)malloc(bytes);
#endif
}

void wx_radar_begin(void) {
    // Each of the WX_RADAR_FRAMES+1 buffers is a full 360x360 RGB565 frame (~253KB). This
    // pool is shared with the Surveillance video buffer (~2.5MB) and everything else in
    // PSRAM, so the frame count is deliberately capped (see wx_radar.h) to leave room —
    // when it overran, allocations here started returning null and the weather map went
    // blank. The count line below is the check that they all actually landed.
    if (s_back) return;
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
    s_back = alloc_pixels();
    if (s_back) memset(s_back, 0, bytes);
    int ok = 0;
    for (int i = 0; i < WX_RADAR_FRAMES; ++i) {
        s_frames[i] = alloc_pixels();
        if (s_frames[i]) { memset(s_frames[i], 0, bytes); ++ok; }
    }
#ifdef ARDUINO
    Serial.printf("[wxradar] begin: %d/%d frame buffers allocated, PSRAM free now %u\n",
                  ok, WX_RADAR_FRAMES, (unsigned)ESP.getFreePsram());
#endif
}

uint16_t *wx_radar_back_buffer(void) { return s_back; }

void wx_radar_commit_frame(int slot, uint32_t gen, uint32_t frameTime, double lat, double lon) {
    if (slot < 0 || slot >= WX_RADAR_FRAMES || !s_back || !s_frames[slot]) return;
    const size_t bytes = WX_RADAR_SIZE * WX_RADAR_SIZE * sizeof(uint16_t);
    std::lock_guard<std::mutex> lock(s_mutex);
    memcpy(s_frames[slot], s_back, bytes);
    s_slotGen[slot]  = gen;
    s_slotTime[slot] = frameTime;
    if (gen > s_latestGen) s_latestGen = gen;
    s_lat = lat; s_lon = lon; s_haveCenter = true;
    ++s_version;
}

bool wx_radar_frame(int slot, uint32_t gen, const uint16_t **pixels, uint32_t *frameTime) {
    if (slot < 0 || slot >= WX_RADAR_FRAMES) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_frames[slot] || s_slotGen[slot] != gen || gen == 0) return false;
    if (pixels)    *pixels = s_frames[slot];
    if (frameTime) *frameTime = s_slotTime[slot];
    return true;
}

uint32_t wx_radar_gen(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_latestGen;
}

int wx_radar_gen_count(uint32_t gen) {
    if (gen == 0) return 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    int n = 0;
    for (int i = 0; i < WX_RADAR_FRAMES; ++i) if (s_slotGen[i] == gen) ++n;
    return n;
}

uint32_t wx_radar_version(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_version;
}

bool wx_radar_center(double *lat, double *lon) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_haveCenter) return false;
    if (lat) *lat = s_lat;
    if (lon) *lon = s_lon;
    return true;
}
