#pragma once
// The saved-network list, as pure data.
//
// Kept apart from the Preferences I/O in wifi_store.cpp so it can be built and run on a
// host compiler with no Arduino under it, the way tests/jig_test.cpp does for the knob.
// The ordering and eviction rules are the whole of the behaviour worth testing here, and
// they are the easy thing to get subtly wrong on a device whose only inspection tool is a
// reflash.

#include <stddef.h>
#include <string.h>

namespace wifi_store {

// 32 octets is the 802.11 maximum and what wifi_config_t carries, so these match the
// driver's own buffers rather than inventing a smaller limit that would quietly truncate
// somebody's network name on the way in.
static const size_t SSID_LEN = 33;
static const size_t PASS_LEN = 65;

// Eight is past what a desk instrument plausibly needs (home, work, a phone hotspot, a
// relative's house) and still only about 800 bytes of NVS.
static const int MAX_NETWORKS = 8;

struct Entry {
    char ssid[SSID_LEN];
    char pass[PASS_LEN];
};

inline void copy_field(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

inline int find(const Entry *list, int n, const char *ssid) {
    if (!list || !ssid || !ssid[0]) return -1;
    for (int i = 0; i < n; ++i)
        if (strncmp(list[i].ssid, ssid, SSID_LEN) == 0) return i;
    return -1;
}

// Put this network at the front, which is where the next search starts looking. Returns
// the new count.
//
// Most recently used rather than insertion order, for two reasons. The list is capped, so
// something has to be given up, and the network not joined in months is a better loss than
// the one joined this morning. And an Orb carried between two places joins them
// alternately, so the front of the list is nearly always the right first guess.
//
// An SSID already held is updated in place and moved, never duplicated. A password changed
// at the router has to replace the old one, or the Orb would go on offering a credential
// that is known not to work.
inline int promote(Entry *list, int n, const char *ssid, const char *pass) {
    if (!list || !ssid || !ssid[0]) return n;
    if (n < 0) n = 0;
    if (n > MAX_NETWORKS) n = MAX_NETWORKS;

    const int at = find(list, n, ssid);
    int from;
    if (at >= 0)                 from = at;                 // known: lift it to the front
    else if (n < MAX_NETWORKS) { from = n; n++; }           // room for one more
    else                         from = MAX_NETWORKS - 1;   // full: the oldest falls off

    for (int i = from; i > 0; --i) list[i] = list[i - 1];
    copy_field(list[0].ssid, SSID_LEN, ssid);
    copy_field(list[0].pass, PASS_LEN, pass);
    return n;
}

// Remove a network by name, keeping the order of the rest. Returns the new count.
inline int drop(Entry *list, int n, const char *ssid) {
    const int at = find(list, n, ssid);
    if (at < 0) return n;
    for (int i = at; i + 1 < n; ++i) list[i] = list[i + 1];
    return n - 1;
}

} // namespace wifi_store
