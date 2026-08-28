#pragma once
// Worldwide roads, read from a microSD card instead of baked into flash — the
// only way "major roads near wherever this particular device actually is"
// can work for every customer, each in a different real-world location,
// without a live network fetch on the device. See tools/gen_road_tiles.py
// for how tiles are generated (OpenStreetMap via Overpass, same
// simplify-and-encode pipeline as coastline_data.h/roads_data.h) and
// docs/roads-sd-card.md (if present) for the SD card layout.
//
// Fixed worldwide grid, ROAD_TILE_GRID_DEG per cell, file
// "/roads/r{latFloor}_{lonFloor}.bin" (binary, not a C header — read directly
// off the card, no text parsing on-device). Whichever tile(s) cover the
// current home location (plus its immediate neighbors, so a home near a tile
// edge doesn't show a hard cutoff) are read ONCE into PSRAM when home/range
// changes, exactly like coastline_project()'s cache-until-changed pattern —
// never touched again per-frame.
//
// On the real device this reads the physical SD card (sdcard::mounted()). In
// the native simulator, ROADS_SD_ROOT (sim_main.cpp) points at a local
// directory that stands in for the card, so both behave identically — same
// tile format, same lookup, same "missing tile = draw nothing" fallback.
#include <lvgl.h>
#include <stddef.h>

#define ROAD_TILE_GRID_DEG 1

namespace roads_sd {

// Reserve the projection buffers up front. Call once at boot, right after the SD
// mount, while PSRAM is still fresh and unfragmented (~8MB free). project() also
// allocates lazily as a fallback, but by the time the radar first runs the custom
// design's plate+overlay (~1MB) plus the weather/spycam churn have fragmented
// PSRAM enough that a mid-session ~76KB block can fail ("PSRAM tight") — grabbing
// it at boot avoids that. Harmless on a device with no card.
void init();

// Re-derive the cached screen-space lines for this home/range — call only
// when either changes (mirrors coastline_project()). Loads whichever tile(s)
// are needed off the card; if the card's missing or the tile doesn't exist
// yet, this just leaves the cache empty (roads_sd_draw() then draws nothing —
// no error state, the scope just doesn't have roads there yet).
void project(double homeLat, double homeLon, double rangeKm,
            float cx, float cy, float rOuterPx);

// The same roads projected into buffers the CALLER owns, for a screen drawing at its own
// size. Mirrors coastline_project_flat() beside coastline_project(). Returns the polyline
// count; `ok` reports whether a projection actually ran, which is not the same as finding
// nothing, so a caller that caches per location does not record an empty result for a cycle
// that was locked out or short of memory.
size_t project_flat(double lat, double lon, double rangeKm,
                    float cx, float cy, float rOuterPx,
                    lv_point_t *outPts, size_t maxPts,
                    uint16_t *outPolyLen, size_t maxPolys, bool &ok);

// Draw whatever's currently cached. Cheap — no file I/O, mirrors coastline_draw().
void draw(lv_draw_ctx_t *ctx, lv_color_t color, lv_opa_t opa, lv_coord_t width);

#ifndef ARDUINO
// Simulator only: point at the local directory standing in for the SD card
// (e.g. sim/sdcard). Call once at startup before the first project(). Real
// device reads sdcard::mounted()+SD.h instead — no equivalent call needed.
void set_root(const char *root);
#endif

} // namespace roads_sd
