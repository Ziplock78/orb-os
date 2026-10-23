#pragma once
// Shared route state (origin -> destination by callsign). Portable: the UI thread
// requests a lookup, a network task fulfils it, the UI reads the result.
#include <stddef.h>
#include <stdint.h>
#include "detail_cache_policy.h"

void route_request(const char *callsign);                     // UI: want a route for this callsign
bool route_pending(char *callOut, size_t n);                  // task: is a lookup needed? returns callsign
void route_store(const char *callsign, const char *from, const char *to,
                 uint32_t ttlMs = DETAIL_CACHE_TTL_MS);  // task/sim: store result
bool route_get(const char *callsign, char *from, size_t fn, char *to, size_t tn); // UI: read result
