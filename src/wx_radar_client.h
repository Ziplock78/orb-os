#pragma once

#include <stdint.h>

// Fetches ONE frame of the animation loop per call so the 9 fetches interleave with the
// live ADS-B polls instead of freezing the feed for ~25s in one blocking burst.
//  - slot 0 also (re)loads the RainViewer frame list and caches it for slots 1..N-1.
//  - zoomTier: 0 = 50mi, 1 = 100mi (see wx_radar_client.cpp WX_ZOOM[]).
//  - gen: the refresh generation this frame belongs to (see wx_radar.h).
// Returns: 1 = frame committed, 0 = no frame at this slot (fewer than expected are
// available — the cycle is done), -1 = fetch/decode error (retry the whole cycle).
int wx_radar_fetch_frame(double lat, double lon, int zoomTier, uint32_t gen, int slot);
