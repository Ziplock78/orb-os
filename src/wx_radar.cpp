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

// The counterpart to alloc_pixels(). Native gets malloc/free, the device gets the PSRAM
// allocator, and no caller has to know which.
static inline void wx_free(void *p) {
#ifdef ARDUINO
    heap_caps_free(p);
#else
    free(p);
#endif
}

// Give the frame buffers back.
//
// ONLY EVER CALLED FROM THE NETWORK TASK, and that is the whole design. The buffers used to
// be taken once at boot and held for the life of the device, ~1.5 MB reserved before
// anything else had asked; taking them on entry and giving them back on exit is the
// contract every screen here is meant to keep.
//
// The first attempt let the UI thread free them on app exit. That hung the device inside a
// minute: the network task writes a decoded frame straight into these buffers without
// holding a lock for the duration, so freeing one out from under it is a write to memory
// that no longer belongs to us. Gating new fetches was not enough, because it did nothing
// about the fetch already in flight.
//
// So the UI only ever ASKS, by setting the flag below, and the task that owns the writing
// does the freeing at a moment when it knows it is not writing. There is no window.
void wx_radar_release(void) {
    std::lock_guard<std::mutex> lock(s_mutex);
    for (int i = 0; i < WX_RADAR_FRAMES; ++i) {
        if (s_frames[i]) { wx_free(s_frames[i]); s_frames[i] = nullptr; }
        s_slotGen[i] = 0;
        s_slotTime[i] = 0;
    }
    if (s_back) { wx_free(s_back); s_back = nullptr; }
    // s_latestGen is deliberately NOT reset: it only ever increases, and the UI asks "is
    // this slot from generation N". Restarting it would let a stale slot answer yes.
    s_haveCenter = false;
#ifdef ARDUINO
    Serial.printf("[wxradar] released; PSRAM free now %u\n", (unsigned)ESP.getFreePsram());
#endif
}

bool wx_radar_ready(void) { return s_back != nullptr; }

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

// ---- what the app is doing, for the screen to say -----------------------------
//
// Written by the network task, read by the UI. See the note in wx_radar.h for why these
// are plain scalars rather than anything guarded.
static volatile WxPhase s_phase      = WX_PHASE_IDLE;
static volatile int     s_phaseDone  = 0;
static volatile int     s_phaseTotal = 0;

void wx_phase_set(WxPhase p, int done, int total) {
    s_phase = p; s_phaseDone = done; s_phaseTotal = total;
}

WxPhase wx_phase_get(int *done, int *total) {
#ifndef ARDUINO
    // Simulator only: ORBWXPHASE pins the phase so the loading notice can be photographed.
    // Every one of these states is a few seconds long on real hardware and some need the
    // network to be broken in a particular way, which is not a thing to arrange by hand
    // each time somebody changes the wording.
    if (const char *e = getenv("ORBWXPHASE")) {
        if (done)  *done  = 3;
        if (total) *total = WX_RADAR_FRAMES;
        return (WxPhase)atoi(e);
    }
#endif
    if (done)  *done  = s_phaseDone;
    if (total) *total = s_phaseTotal;
    return s_phase;
}

const char *wx_phase_text(void) {
    static char buf[48];
    int done = s_phaseDone, total = s_phaseTotal;
    WxPhase phase = s_phase;
#ifndef ARDUINO
    if (const char *e = getenv("ORBWXPHASE")) { phase = (WxPhase)atoi(e); done = 3; total = WX_RADAR_FRAMES; }
#endif
    switch (phase) {
        case WX_PHASE_MAP:     return "DRAWING THE MAP";
        case WX_PHASE_BUFFERS: return "MAKING ROOM";
        case WX_PHASE_INDEX:   return "FINDING THE LATEST SCAN";
        case WX_PHASE_FRAMES:
            // The one people watch. A bare spinner here is what makes a slow thing feel
            // broken; a count that moves makes the same wait obviously alive.
            snprintf(buf, sizeof(buf), "LOADING RAIN  %d/%d", done, total > 0 ? total : WX_RADAR_FRAMES);
            return buf;
        case WX_PHASE_NO_WIFI: return "NO WIFI, SO NO RADAR";
        case WX_PHASE_FAILED:  return "RAINVIEWER IS NOT ANSWERING";
        case WX_PHASE_READY:   return "";
        default:               return "ACQUIRING WX RADAR...";
    }
}
