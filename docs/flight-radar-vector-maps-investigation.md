# Flight radar vector maps investigation

Date: 2026-10-21. Initial investigation, before implementation or hardware validation.
The subsequent prototype and its current validation are described in
[Vector road maps](vector-road-maps.md).

## Recommendation

Prototype downloading prepared XYZ vector tiles on the Orb, decoding only major
road lines, and saving a small, versioned road cache on SD. Keep LVGL, the existing
radar projection and the theme's map controls. A full map engine is unnecessary.

This should remove Overpass query generation and JSON geometry parsing from the
normal download path. It does not establish a download speedup until tested with
a specific tile service, or eliminate TLS, SD contention or map snapshot costs.

Preserve tile-local coordinates in the new cache. Converting MVT into the existing
global int16 latitude/longitude format would discard much of the precision gained.

## Branch comparison

The current `MiltonBanana/fix/flight-tracker-vector-maps` branch is at `MiltonBanana/3c0c3ec`, the same base
as the previous experiment. `MiltonBanana/fix/flight-tracker-maps` adds:

- `MiltonBanana/d42c8cd`: automatic road downloads, generation, validation and radar integration.
- `MiltonBanana/5158df6`: optimization work and browser preload, including the reusable snapshot
  fix. Its firmware version is MiltonBanana/2.18.14; the current branch remains MiltonBanana/2.18.00.

Consequently, automatic downloads and browser preload are absent here rather than
already inherited. Preserve useful fixes and integration deliberately when building
the prototype; merging both commits wholesale would also bring the Overpass stack.

Useful work to retain from that branch:

- Network worker separated from UI-owned SD writes and LVGL.
- Bounded transfer queues, deferred SD work during theme/audio activity, cancellation,
  retry/backoff and incremental cache inspection.
- Validation before publishing temporary files; successful empty coverage cached.
- Cache revision refreshes for both flight and weather maps. New roads refresh the
  map without deleting aircraft trails; location/range changes still reset trails.
- `MapSnapshot` reuse and image-cache invalidation. The previous branch documents
  an allocation assertion with a 651,468-byte snapshot; changing the map data source
  does not fix that allocation path.
- Existing legacy `r*.bin` compatibility and useful progress diagnostics.

Airport marker changes are independent of the data-source experiment.

## What the current code already does

`src/roads_sd.cpp` streams legacy SD tiles one polyline at a time and caches clipped
screen points. It does not load a complete tile or read SD per frame. Current
`src/coastline.cpp` already uses a local projection approximation to avoid expensive
per-point double-precision trigonometry. Those optimizations should be retained.

Its four geometry allocations total 198,192 bytes with four-byte `lv_point_t`:
128,000 output point bytes, 30,000 output length bytes, 32,000 tile length bytes,
and 8,192 input point bytes. The static SD read buffer adds 4,096 bytes. These are
code-derived sizes, not a current device heap measurement. The source's approximate
230 KB comment is not the exact sum of these allocations.

The current reader scans the home degree cell and eight neighbours. That is not a
general coverage planner at high latitudes or for arbitrary theme ranges. XYZ
coverage should derive from the visible geographic bounds, wrap longitude, and
handle the Web Mercator latitude limit explicitly.

## Precision is a separate improvement

The legacy generator and reader use integer coordinates at 180 units per degree.
One latitude step is about 0.618 km. With the configured 218 px scope radius:

| Outer range | Legacy latitude step on screen |
| --- | ---: |
| 10 km | 13.47 px |
| 20 km | 6.73 px |
| 30 km | 4.49 px |
| 50 km | 2.69 px |
| 100 km | 1.35 px |

These are step sizes, not measured visual errors. Nearest rounding alone can move
a coordinate by half a step on an axis; simplification adds its own error. The
generator also simplifies at 0.0025 degrees, approximately 278 m in latitude.

For comparison, a tile around latitude 40.2 degrees at z10 spans approximately
29.9 km horizontally. At extent 4096 its local coordinate step is about 7.3 m,
or 0.053 px at the 30 km range. Actual detail depends on the producer's geometry
generalization; extent is not a promise of positional accuracy.

## Data and decoder

[OpenMapTiles transportation](https://openmaptiles.org/docs/schema/#transportation)
provides road classes and a ramp flag. Initial filtering should accept only line
features in `transportation` with `class=motorway` or `class=trunk`. Exclude ramps
initially to stay close to the previous exact OSM highway-class queries; compare
interchanges visually before changing that policy. Ignore road names, buildings,
polygons, rail and minor streets. Airports can remain on the existing separate path.

[MVT 2.1](https://github.com/mapbox/vector-tile-spec/blob/master/2.1/README.md)
encodes geometry and properties using protobuf. It does not define the road schema,
compression, zoom-specific road inclusion or generalization policy. A different
provider is not automatically compatible just because its files are called PBF.

A bounded decoder needs protobuf field skipping, checked varints and lengths,
layer key/value lookup, tag-index validation, and MoveTo/LineTo zigzag deltas.
Handle multipart lines, per-layer extent, buffered coordinates outside the nominal
tile extent, unknown fields and arbitrary protobuf field order. MVT fields may put
features before their property dictionaries; use bounded staged input or an SD
spool with a second pass rather than assuming a convenient order. Reject malformed
geometry and count overflow without publishing partial results.

Transport must define and test Content-Encoding handling, including gzip if the
chosen endpoint serves it, independently of HTTP chunk framing. Bound compressed
and expanded sizes. Skipping unwanted layers reduces retained geometry and decode
work, but does not reduce the bytes downloaded from a general-purpose basemap.

Suggested cache: a separate `/roads/v2/` namespace keyed by source/schema revision
and z/x/y, with magic, version, extent, validated counts and lengths, checksum and
tile-local coordinates. Clip producer buffers to tile bounds before publication
to avoid duplicate strokes at seams. Preserve enough signed coordinate range
during decode. Cache successful empty tiles distinctly from failed downloads.
Do not mix legacy and new geometry over the same coverage; retain legacy fallback
for areas without new coverage. Any additional screen simplification should use
an explicit pixel error budget and preserve clipping intersections.

## Initial zoom experiment

These are candidate levels to compare, not fixed firmware settings. Calculations
use home 40.2, -111.65 and a spherical Mercator tile grid. Counts cover the bounding
square, before circle rejection; byte counts require actual provider samples.

| Outer range | Candidate zoom | Tile width near home | Bounding-square tile count |
| --- | ---: | ---: | ---: |
| 10 km | 11 | 14.9 km | 4 |
| 20 km | 10 | 29.9 km | 6 |
| 30 km | 10 | 29.9 km | 9 |
| 50 km | 9 | 59.8 km | 9 |
| 100 km | 8 | 119.6 km | 9 |

Plan nearest tiles first. Adapt level to latitude, visible range and the source's
actual road inclusion; compare adjacent zooms before choosing. A lower zoom can
omit useful trunk roads, and fewer tiles need not mean fewer bytes or faster decode.
Range changes can reuse raw geometry only where cached detail is adequate.

## Theme contract

Keep `theme_style::radar().mapRoadsOn` as the existing road-map switch, together with
`mapRoadColor`, `mapRoadOpacity` and `mapRoadWidth`. The new cache stores geometry,
not colours. Theme changes invalidate the rendered map/snapshot, not downloaded data.

When the road-map switch is off, stop scheduling road fetches, cancel pending work,
hide progress and remove cached road strokes from the composed background. Keep SD
files for a quick return when enabled. Enabling must reproject/recompose even if
home and range have not changed. Test switching while a response is in flight.

The current custom-theme draw path honours the switch; stock draw paths use fixed
road styling, and current projection still runs when themed roads are disabled.
Make road visibility consistent on all supported radar paths. Airports retain
their existing independent `mapAirportsOn` and device preference controls. Weather
retains its separate `roadsEnabled` setting and cache revision handling.

## Source decision

OpenMapTiles is a schema and toolchain, not a guaranteed free global tile endpoint.
[MapTiler's Tiles API](https://docs.maptiler.com/cloud/api/tiles/) requires an API key.
Provider-specific persistent/offline cache permission, attribution, quotas, schema,
and TLS compatibility must be settled before choosing it for firmware.

For a controlled experiment, use a licensed regional OpenMapTiles extract served
as XYZ; [TileServer GL](https://openmaptiles.org/docs/host/tileserver-gl/) documents
serving these extracts. For production, a prepared road-only feed under our control
would minimize downloaded bytes and device decoding, at the cost of maintaining
preparation and hosting. Both options leave fetching, caching and drawing on-device.
Browser preload remains a useful optional transport for the same validated cache.

## Prototype acceptance criteria

1. Capture real tiles for a dense city, rural area and tile boundary; compare
   candidate levels against the previous branch at the same home/range/theme.
2. Validate the decoder on those tiles plus malformed lengths, reordered fields,
   unknown layers, ramps, multipart lines, buffered geometry and compressed input.
3. Measure cold time to first visible roads, full coverage time, transferred and
   cached bytes, peak internal RAM/PSRAM, largest free blocks, decoder time, SD time,
   projection and snapshot time separately. Repeat with warm cache and Wi-Fi off.
4. Confirm responsive aircraft updates and sweep during downloads, theme transfers,
   SD/audio contention, cancellation, location/range changes and interrupted writes.
5. Verify theme off/on and restyling, empty coverage, source errors, cache migration,
   seams, longitude wrapping and high-latitude planning.

No speedup, final tile size, selected provider, decoder memory budget or device
stability is established by this investigation. The next useful implementation is
a bounded host-tested decoder/cache experiment followed by a hardware comparison,
before replacing the working Overpass path.
