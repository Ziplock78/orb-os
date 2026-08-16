#!/usr/bin/env python3
"""Generate src/roads_data.h from an OpenStreetMap Overpass API response.

Reads the "elements" array of an Overpass [out:json] query (way objects with
inline "geometry": [{lat,lon}, ...]), simplifies each way with Douglas-Peucker,
and emits a compact C array of int16 coordinates (degrees * 180, same scale
and format as coastline_data.h) plus a polyline-length index — same shape,
so it can be walked by the exact same projection code (coastline_project_flat()
made generic, see coastline.cpp) instead of a second copy of that math.

Usage:
    python3 tools/gen_roads.py tools/data/roads_raw.json src/roads_data.h ROAD
    python3 tools/gen_roads.py tools/data/roads_raw_100mi.json src/roads_wide_data.h ROAD_WIDE

Two datasets feed the Weather app's three zoom tiers (10mi/50mi use the narrow
one, 100mi uses the wide one) — see roads.h for which is which. Source for
both: OpenStreetMap contributors, ODbL.

Narrow (roads_data.h, ROAD prefix) — motorway+trunk+primary, ~85km around
home (a little past the 50mi/80.5km tier so that tier doesn't run past the
edge of the data). Primary was added after the 10mi tier turned out nearly
empty in suburbs, where motorway/trunk tagging is sparse but primary covers
the major named arterials people actually navigate by. A single combined
`highway~"^(motorway|trunk|primary)$"` regex query 504s on the public
instance at this radius (even motorway+trunk alone started 504ing partway
through this session — the server's tolerance for that query shape seems to
have dropped) — run each classification as its own bbox query and merge the
"elements" arrays before feeding this script. Bbox corners are home lat/lon
+/- 85km, converted to degrees (85/111.0 for lat; 85/(111.32*cos(lat)) for
lon):
  [out:json][timeout:90];(way["highway"="motorway"](<south>,<west>,<north>,<east>););out geom;
  [out:json][timeout:90];(way["highway"="trunk"](<south>,<west>,<north>,<east>););out geom;
  [out:json][timeout:90];(way["highway"="primary"](<south>,<west>,<north>,<east>););out geom;
Secondary was deliberately left out — primary already ~4x'd the point count
(21.4k polylines / 43k points, ~210KB flash) and secondary would be denser
still; "major streets, not too granular" was the original brief for this
layer, so primary is the line drawn for now. If suburbs still look sparse at
10mi after this, secondary is the next lever, but it'll cost noticeably more
flash and per-fetch projection time — worth checking whether primary is
actually enough first.

Wide (roads_wide_data.h, ROAD_WIDE prefix) — motorway only, ~170km around
home, bbox query (the public Overpass instance 504s on an "around" query at
this radius even motorway-only; a bbox query over the same area returns in
~8s). Bbox corners are home lat/lon +/- 170km, converted to degrees
(170/111.0 for lat; 170/(111.32*cos(lat)) for lon):
  [out:json][timeout:180];
  (way["highway"="motorway"](<south>,<west>,<north>,<east>););out geom;

Re-run both against a fresh Overpass query centered on a new location if the
device's home moves somewhere the current extract doesn't cover.
"""
import json
import sys

TOLERANCE_DEG = 0.0025   # same as gen_coastline.py — ~0.28km, plenty fine at this display scale
MIN_POINTS = 2
SCALE = 180.0            # matches COAST_SCALE so the same projection code works unmodified


def perp_dist(p, a, b):
    ax, ay = a
    bx, by = b
    px, py = p
    dx, dy = bx - ax, by - ay
    if dx == 0 and dy == 0:
        return ((px - ax) ** 2 + (py - ay) ** 2) ** 0.5
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return ((px - cx) ** 2 + (py - cy) ** 2) ** 0.5


def douglas_peucker(pts, tol):
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        lo, hi = stack.pop()
        dmax, idx = 0.0, -1
        for i in range(lo + 1, hi):
            d = perp_dist(pts[i], pts[lo], pts[hi])
            if d > dmax:
                dmax, idx = d, i
        if idx != -1 and dmax > tol:
            keep[idx] = True
            stack.append((lo, idx))
            stack.append((idx, hi))
    return [pts[i] for i in range(len(pts)) if keep[i]]


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "tools/data/roads_raw.json"
    dst = sys.argv[2] if len(sys.argv) > 2 else "src/roads_data.h"
    prefix = sys.argv[3] if len(sys.argv) > 3 else "ROAD"

    with open(src) as f:
        data = json.load(f)

    polylines = []   # list of [(lon,lat), ...] simplified
    raw_pts = 0
    for el in data.get("elements", []):
        geom = el.get("geometry")
        if not geom:
            continue
        line = [(pt["lon"], pt["lat"]) for pt in geom if pt is not None]
        raw_pts += len(line)
        simp = douglas_peucker(line, TOLERANCE_DEG)
        if len(simp) >= MIN_POINTS:
            polylines.append(simp)

    coords = []
    lengths = []
    for pl in polylines:
        n = 0
        for lon, lat in pl:
            lat_s = max(-int(90 * SCALE), min(int(90 * SCALE), round(lat * SCALE)))
            lon_s = max(-int(180 * SCALE), min(int(180 * SCALE), round(lon * SCALE)))
            coords.append(lat_s)
            coords.append(lon_s)
            n += 1
        lengths.append(n)

    total_pts = len(coords) // 2
    with open(dst, "w") as f:
        f.write("// Auto-generated by tools/gen_roads.py — DO NOT EDIT.\n")
        f.write("// Source: OpenStreetMap via Overpass API (ODbL). Major highways only\n")
        f.write("// (motorway/trunk), local extract — see the file header comment in\n")
        f.write("// tools/gen_roads.py for the exact query and re-bake instructions.\n")
        f.write("// Coordinates are int16 degrees*%d (lat,lon pairs). %d polylines, %d points.\n"
                % (int(SCALE), len(lengths), total_pts))
        f.write("// Simplified with Douglas-Peucker tol=%.4f deg (from %d raw points).\n"
                % (TOLERANCE_DEG, raw_pts))
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write("#define %s_SCALE %d\n" % (prefix, int(SCALE)))
        f.write("#define %s_NUM_POLYS %d\n" % (prefix, len(lengths)))
        f.write("#define %s_NUM_PTS %d\n\n" % (prefix, total_pts))

        f.write("// Points per polyline (walk %s_PTS sequentially).\n" % prefix)
        f.write("static const uint16_t %s_POLY_LEN[%s_NUM_POLYS] = {\n" % (prefix, prefix))
        for i in range(0, len(lengths), 16):
            f.write("  " + ",".join(str(x) for x in lengths[i:i + 16]) + ",\n")
        f.write("};\n\n")

        f.write("// Flat lat,lon int16 pairs (degrees*%s_SCALE).\n" % prefix)
        f.write("static const int16_t %s_PTS[%s_NUM_PTS * 2] = {\n" % (prefix, prefix))
        for i in range(0, len(coords), 16):
            f.write("  " + ",".join(str(x) for x in coords[i:i + 16]) + ",\n")
        f.write("};\n")

    bytes_total = total_pts * 4 + len(lengths) * 2
    print("polylines: %d" % len(lengths))
    print("points:    %d (from %d raw, %.1f%% kept)" % (total_pts, raw_pts, 100.0 * total_pts / max(raw_pts, 1)))
    print("flash:     ~%.1f KB" % (bytes_total / 1024.0))
    print("written:   %s" % dst)


if __name__ == "__main__":
    main()
