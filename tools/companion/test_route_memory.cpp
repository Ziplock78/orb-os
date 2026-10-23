#include "route.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    char from[40], to[40], pending[12];
    route_store("SWA296", "KLAS", "KSLC");
    route_store("JBU177", "KBOS", "KLAS");
    route_request(" swa296 ");
    assert(!route_pending(pending, sizeof(pending)));
    assert(route_get("SWA296", from, sizeof(from), to, sizeof(to)));
    assert(!strcmp(from, "KLAS") && !strcmp(to, "KSLC"));
    // A late result for another flight cannot evict the selected flight.
    route_store("OTHER1", "KDEN", "KJFK");
    assert(!route_pending(pending, sizeof(pending)));
    // Expired success is eligible for refresh; failure is briefly retained.
    route_store("EXPIRED", "KDEN", "KLAS", 0);
    route_request("EXPIRED");
    assert(route_pending(pending, sizeof(pending)));
    assert(!route_get("EXPIRED", from, sizeof(from), to, sizeof(to)));
    route_store("MISSING", "", "");
    route_request("MISSING");
    assert(!route_pending(pending, sizeof(pending)));
    assert(route_get("MISSING", from, sizeof(from), to, sizeof(to)) && !from[0] && !to[0]);
    // Fill the bounded cache. Reading an older entry makes it recent again.
    for (int i=0; i<16; ++i) {
        char call[12]; snprintf(call, sizeof(call), "TEST%02d", i);
        route_store(call, "KDEN", "KLAS");
    }
    assert(route_get("TEST00", from, sizeof(from), to, sizeof(to)));
    route_store("NEW", "KBOS", "KLAS");
    assert(!route_get("TEST01", from, sizeof(from), to, sizeof(to)));
    assert(route_get("TEST00", from, sizeof(from), to, sizeof(to)));
    puts("PASS: immediate revisits, independent results, expiry, failed results, normalization, LRU eviction");
}
