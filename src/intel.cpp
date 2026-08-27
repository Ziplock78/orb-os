#include "intel.h"
#include <mutex>
#include <string.h>

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

// ---------------------------------------------------------------------------
// The brief being read, if any. One slot, because one person is holding one Orb and
// looking at one story.

static IntelBrief s_brief = {};

// A truncating copy that always terminates. strncpy does not, and a headline that fills
// its buffer exactly would leave the next field's bytes reading as part of it.
static void copy_into(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

void intel_brief_want(const char *key, const char *headline, const char *source) {
    if (!key || !*key) return;
    std::lock_guard<std::mutex> lock(s_mutex);
    // Already holding this exact story, in a state worth showing: leave it alone. Pressing
    // the same headline twice should not throw away the text and go back to the network for
    // an answer that is sitting right there.
    const bool sameStory = strncmp(s_brief.key, key, INTEL_KEY_BYTES) == 0;
    if (sameStory && (s_brief.state == INTEL_BRIEF_READY ||
                      s_brief.state == INTEL_BRIEF_EMPTY ||
                      s_brief.state == INTEL_BRIEF_LOADING ||
                      s_brief.state == INTEL_BRIEF_WANTED)) return;
    s_brief = {};
    copy_into(s_brief.key, sizeof(s_brief.key), key);
    // The headline and source come from the list rather than from the answer, so the
    // briefing can draw its heading the instant it opens instead of after a round trip.
    // The gateway sends them back too and they are used to correct these if they differ.
    copy_into(s_brief.headline, sizeof(s_brief.headline), headline);
    copy_into(s_brief.source, sizeof(s_brief.source), source);
    s_brief.state = INTEL_BRIEF_WANTED;
}

void intel_brief_release() {
    std::lock_guard<std::mutex> lock(s_mutex);
    // A fetch already in flight is left alone rather than cancelled: it will land in a slot
    // whose key no longer matches anything on screen, which costs one wasted store and no
    // correctness. Tearing down a request from the wrong task is how a use-after-free gets
    // written.
    if (s_brief.state == INTEL_BRIEF_LOADING) return;
    s_brief = {};
}

bool intel_brief_get(IntelBrief &out) {
    std::lock_guard<std::mutex> lock(s_mutex);
    out = s_brief;
    return s_brief.state != INTEL_BRIEF_IDLE;
}

bool intel_brief_take(char *keyOut, size_t keyCap) {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_brief.state != INTEL_BRIEF_WANTED) return false;
    copy_into(keyOut, keyCap, s_brief.key);
    s_brief.state = INTEL_BRIEF_LOADING;
    return true;
}

void intel_brief_store(const char *key, IntelBriefState state,
                       const char *headline, const char *source, const char *body) {
    std::lock_guard<std::mutex> lock(s_mutex);
    // The answer to a question nobody is asking any more. Dropped rather than stored: the
    // briefing was closed, or a different story was opened while this one was in flight,
    // and either way writing it here would put the wrong text under the right heading.
    if (!key || strncmp(s_brief.key, key, INTEL_KEY_BYTES) != 0) return;
    s_brief.state = state;
    if (headline && *headline) copy_into(s_brief.headline, sizeof(s_brief.headline), headline);
    if (source && *source)     copy_into(s_brief.source, sizeof(s_brief.source), source);
    copy_into(s_brief.body, sizeof(s_brief.body), body);
}
