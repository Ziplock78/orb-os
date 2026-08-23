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
