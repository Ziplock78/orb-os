#pragma once
// Look up a flight's origin/destination airport codes directly via FlightAware.
// Device-only (uses WiFi/HTTPS).
#include <stddef.h>
#include <stdint.h>

bool route_fetch(const char *callsign, char *from, size_t fn, char *to, size_t tn);

// NVS route cache: successful routes survive reboots for up to 15 minutes.
void route_cache_begin();   // call once at boot; clears the cache if the label format changed
bool route_cache_get(const char *callsign, char *from, size_t fn, char *to, size_t tn,
                     uint32_t *remainingMs = nullptr);
void route_cache_put(const char *callsign, const char *from, const char *to);
