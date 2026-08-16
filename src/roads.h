#pragma once
// Major highways as a light background layer under the Weather app's precipitation/cloud
// radar, so there's some sense of where you're actually looking instead of colored blobs
// floating in a void. Both are local extracts around one home location, not a world
// dataset like coastline_data.h — see tools/gen_roads.py for the exact queries and
// re-bake steps if the device's home location ever moves somewhere outside the current
// extract.
//
// Two datasets, picked by the caller based on the weather map's zoom tier:
//  - roads_project_flat()      narrow: motorway+trunk, ~85km radius (roads_data.h)
//                               — used for the 10mi/50mi tiers, where the finer trunk-road
//                               detail is actually visible on screen.
//  - roads_wide_project_flat() wide: motorway only, ~170km radius (roads_wide_data.h)
//                               — used for the 100mi tier; trunk roads are dropped so the
//                               Overpass query and the point budget both stay manageable
//                               at 100+ mile scale, where they'd be an illegible tangle
//                               anyway.
#include <lvgl.h>
#include <stddef.h>

size_t roads_project_flat(double centerLat, double centerLon, double rangeKm,
                          float cx, float cy, float rOuterPx,
                          lv_point_t *outPts, size_t maxPts,
                          uint16_t *outPolyLen, size_t maxPolys);

size_t roads_wide_project_flat(double centerLat, double centerLon, double rangeKm,
                               float cx, float cy, float rOuterPx,
                               lv_point_t *outPts, size_t maxPts,
                               uint16_t *outPolyLen, size_t maxPolys);
