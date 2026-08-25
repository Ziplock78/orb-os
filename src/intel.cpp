#include "intel.h"
#include <mutex>

// Written by the network task, read by LVGL on the UI core. Same handoff the weather
// snapshot uses, and for the same reason: the fetch cannot touch the display, and a
// half-copied headline is worse than an old one.
static std::mutex s_mutex;
static IntelSnapshot s_snapshot = {};

void intel_store(const IntelSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_snapshot = snapshot;
}

bool intel_get(IntelSnapshot &snapshot) {
    std::lock_guard<std::mutex> lock(s_mutex);
    snapshot = s_snapshot;
    return snapshot.valid;
}

bool intel_meta(uint32_t &fetchedMs, int &count) {
    std::lock_guard<std::mutex> lock(s_mutex);
    fetchedMs = s_snapshot.fetchedMs;
    count     = s_snapshot.count;
    return s_snapshot.valid;
}

int intel_window(int from, int n, IntelItem *out, int &total) {
    std::lock_guard<std::mutex> lock(s_mutex);
    total = s_snapshot.valid ? s_snapshot.count : 0;
    if (!out || n <= 0 || from < 0 || from >= total) return 0;
    int wrote = 0;
    for (int i = from; i < total && wrote < n; ++i, ++wrote) out[wrote] = s_snapshot.items[i];
    return wrote;
}
