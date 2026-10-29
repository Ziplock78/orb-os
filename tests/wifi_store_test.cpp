// Run from the repository root (C++17 host compiler):
//   g++ -std=c++17 -Isrc tests/wifi_store_test.cpp -o /tmp/wifi_store_test
//   /tmp/wifi_store_test
//
// Covers the list rules only. The Preferences I/O in wifi_store.cpp is a straight blob
// read and write and needs a device to mean anything, but the ordering, the de-duplication
// and the eviction are decisions, and a wrong one there shows up as an Orb that forgets
// the network somebody uses every day.
#include "wifi_store_list.h"

#include <assert.h>
#include <stdio.h>

using wifi_store::Entry;
using wifi_store::MAX_NETWORKS;

static int add(Entry *l, int n, const char *ssid, const char *pass) {
    return wifi_store::promote(l, n, ssid, pass);
}
static bool is(const Entry &e, const char *ssid, const char *pass) {
    return strcmp(e.ssid, ssid) == 0 && strcmp(e.pass, pass) == 0;
}

int main() {
    // A new network goes to the front, and the one that was there moves down.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = add(l, 0, "home", "pw-home");
        assert(n == 1 && is(l[0], "home", "pw-home"));
        n = add(l, n, "work", "pw-work");
        assert(n == 2 && is(l[0], "work", "pw-work") && is(l[1], "home", "pw-home"));
    }

    // Re-joining a network already held moves it up without duplicating it.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = 0;
        n = add(l, n, "a", "pa");
        n = add(l, n, "b", "pb");
        n = add(l, n, "c", "pc");
        assert(n == 3);
        n = add(l, n, "a", "pa");
        assert(n == 3);                                  // still three, not four
        assert(is(l[0], "a", "pa") && is(l[1], "c", "pc") && is(l[2], "b", "pb"));
        assert(wifi_store::find(l, n, "a") == 0);
    }

    // A password changed at the router replaces the old one rather than sitting beside it.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = add(l, 0, "home", "old");
        n = add(l, n, "home", "new");
        assert(n == 1 && is(l[0], "home", "new"));
    }

    // The list caps, and it is the least recently used that falls off, not the newest.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = 0;
        char name[8];
        for (int i = 0; i < MAX_NETWORKS; ++i) {         // n0 .. n7, n7 ends up at the front
            snprintf(name, sizeof(name), "n%d", i);
            n = add(l, n, name, "p");
        }
        assert(n == MAX_NETWORKS);
        assert(strcmp(l[MAX_NETWORKS - 1].ssid, "n0") == 0);   // oldest sits at the back
        n = add(l, n, "extra", "p");
        assert(n == MAX_NETWORKS);                       // does not grow past the cap
        assert(strcmp(l[0].ssid, "extra") == 0);
        assert(wifi_store::find(l, n, "n0") < 0);        // n0 is the one that went
        assert(wifi_store::find(l, n, "n1") >= 0);       // and n1 survived
    }

    // Forgetting removes the named network and leaves the order of the rest alone.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = 0;
        n = add(l, n, "a", "pa");
        n = add(l, n, "b", "pb");
        n = add(l, n, "c", "pc");                        // c, b, a
        n = wifi_store::drop(l, n, "b");
        assert(n == 2 && is(l[0], "c", "pc") && is(l[1], "a", "pa"));
        n = wifi_store::drop(l, n, "not-here");
        assert(n == 2);                                  // dropping an unknown changes nothing
    }

    // A nameless network is not a network. These arrive from a driver read that found an
    // empty slot, and storing one would put an unjoinable blank in the list forever.
    {
        Entry l[MAX_NETWORKS] = {};
        int n = add(l, 0, "", "p");
        assert(n == 0);
        n = add(l, 0, nullptr, "p");
        assert(n == 0);
        assert(wifi_store::find(l, 0, "") < 0);
    }

    // An over-long SSID or password is truncated rather than running off the end.
    {
        Entry l[MAX_NETWORKS] = {};
        char longSsid[128], longPass[256];
        memset(longSsid, 'S', sizeof(longSsid)); longSsid[sizeof(longSsid) - 1] = '\0';
        memset(longPass, 'P', sizeof(longPass)); longPass[sizeof(longPass) - 1] = '\0';
        const int n = add(l, 0, longSsid, longPass);
        assert(n == 1);
        assert(strlen(l[0].ssid) == wifi_store::SSID_LEN - 1);
        assert(strlen(l[0].pass) == wifi_store::PASS_LEN - 1);
    }

    printf("wifi_store: all list rules hold\n");
    return 0;
}
