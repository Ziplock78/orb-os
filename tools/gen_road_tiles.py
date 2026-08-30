#!/usr/bin/env python3
"""Generate one worldwide-roads SD-card tile: fetches major roads for a fixed
lat/lon grid cell from OpenStreetMap's Overpass API, simplifies them, and
writes a compact binary file the device reads directly off the SD card (no
text parsing on-device — see roads_sd.cpp).

This is the SD-tile sibling of tools/gen_roads.py (which bakes ONE local
extract into flash for the Weather app). Same source, same simplification,
same coordinate scale — but tiled to a fixed worldwide grid instead of a
single home-centered extract, and written as raw binary instead of a C
header, since these ship as data files on an SD card, not compiled in.

Usage:
    python3 tools/gen_road_tiles.py <lat> <lon> <out_dir> [grid_deg]

    lat/lon    any point inside the tile you want (usually a home location)
    out_dir    directory tiles get written into, e.g. sim/sdcard/roads
    grid_deg   tile size in degrees (default 1) — must match ROAD_TILE_GRID_DEG
               in roads_sd.h, or the device will look in the wrong file.

Tile filename: r{latFloor}_{lonFloor}.bin, e.g. r30_-115.bin for a point in
the [30,35) x [-115,-110) cell. latFloor/lonFloor are floor(coord / grid_deg)
* grid_deg, so every point in a cell maps to the same, unique file name.

Binary format (little-endian, no header magic — the device knows the shape):
    u16 numPolys
    u32 numPts
    u16 polyLen[numPolys]      -- points per polyline, in order
    i16 pts[numPts * 2]        -- flat (lat, lon) pairs, degrees * SCALE
SCALE is fixed at 180 (matches coastline_data.h / roads_data.h) and is not
stored in the file — both generator and reader agree on it by convention.
"""
import json
import os
import sys
import struct
import time
import urllib.request

TOLERANCE_DEG = 0.0025   # same as gen_roads.py / gen_coastline.py — ~0.28km
MIN_POINTS = 2
SCALE = 180
OVERPASS_URLS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",   # fallback mirror — the default instance 504s under load
    "https://overpass.private.coffee/api/interpreter",
    "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
]
# The public instances 504 under load routinely, and a single pass over the mirror list
# often finds every one of them busy at the same moment. Sweeping the list a few times
# with a pause between costs nothing on a tool that runs once per city.
OVERPASS_ROUNDS = 3
OVERPASS_PAUSE_S = 20


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


def tile_floor(coord, grid):
    import math
    return int(math.floor(coord / grid) * grid)


# Road classes to include. Freeways and highways only, deliberately.
#
# This used to include "primary", matching gen_roads.py's older "major streets, not
# too granular" set. On a 466 px dial over a gridded city that is far too granular:
# in Phoenix, primary is every big surface arterial, so the scope filled with a
# street grid that buried the aircraft it exists to show. motorway+trunk leaves the
# interstates, the loops and the state highways — the shapes a person actually
# recognises their city by from the air.
#
# Override with ROAD_CLASSES=motorway,trunk,primary to regenerate the old density.
ROAD_CLASSES = tuple(
    c.strip() for c in os.environ.get("ROAD_CLASSES", "motorway,trunk").split(",") if c.strip()
)


def fetch_overpass(south, west, north, east):
    # One combined regex query 504s on the public instance at this box size per
    # gen_roads.py's notes, so issue one query per class and merge, same workaround.
    elements = []
    for hwy in ROAD_CLASSES:
        q = (
            '[out:json][timeout:90];'
            f'(way["highway"="{hwy}"]({south},{west},{north},{east}););'
            'out geom;'
        )
        data = None
        last_err = None
        for attempt in range(OVERPASS_ROUNDS):
            if attempt:
                print(f"  all mirrors busy, waiting {OVERPASS_PAUSE_S}s before round {attempt + 1}...", file=sys.stderr)
                time.sleep(OVERPASS_PAUSE_S)
            for url in OVERPASS_URLS:
                req = urllib.request.Request(
                    url, data=q.encode("utf-8"),
                    headers={"User-Agent": "orb-os-tile-gen/1.0 (github.com/Ziplock78/orb-firmware)"},
                )
                print(f"  querying {hwy} via {url}...", file=sys.stderr)
                try:
                    with urllib.request.urlopen(req, timeout=180) as r:
                        data = json.load(r)
                    break
                except Exception as e:
                    last_err = e
                    print(f"    failed ({e}), trying next mirror", file=sys.stderr)
            if data is not None:
                break
        if data is None:
            raise last_err
        els = data.get("elements", [])
        print(f"    {len(els)} ways", file=sys.stderr)
        elements.extend(els)
    return elements


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    lat = float(sys.argv[1])
    lon = float(sys.argv[2])
    out_dir = sys.argv[3]
    grid = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0

    lat_floor = tile_floor(lat, grid)
    lon_floor = tile_floor(lon, grid)
    south, north = lat_floor, lat_floor + grid
    west, east = lon_floor, lon_floor + grid
    print(f"tile: lat[{south},{north}) lon[{west},{east})", file=sys.stderr)

    elements = fetch_overpass(south, west, north, east)

    polylines = []
    raw_pts = 0
    for el in elements:
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
        for lon_, lat_ in pl:
            lat_s = max(-int(90 * SCALE), min(int(90 * SCALE), round(lat_ * SCALE)))
            lon_s = max(-int(180 * SCALE), min(int(180 * SCALE), round(lon_ * SCALE)))
            coords.append(lat_s)
            coords.append(lon_s)
            n += 1
        lengths.append(n)

    import os
    os.makedirs(out_dir, exist_ok=True)
    fname = f"r{lat_floor}_{lon_floor}.bin"
    fpath = os.path.join(out_dir, fname)
    with open(fpath, "wb") as f:
        f.write(struct.pack("<H", len(lengths)))
        f.write(struct.pack("<I", len(coords) // 2))
        for n in lengths:
            f.write(struct.pack("<H", n))
        for c in coords:
            f.write(struct.pack("<h", c))

    total_pts = len(coords) // 2
    bytes_total = 2 + 4 + len(lengths) * 2 + total_pts * 4
    print(f"polylines: {len(lengths)}", file=sys.stderr)
    print(f"points:    {total_pts} (from {raw_pts} raw)", file=sys.stderr)
    print(f"file:      {fpath} (~{bytes_total/1024:.1f} KB)", file=sys.stderr)


if __name__ == "__main__":
    main()
