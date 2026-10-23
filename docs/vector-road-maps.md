# Vector road-map prototype

Firmware 2.18.22 downloads prepared vector tiles directly on the Orb. The default
is [OpenFreeMap](https://openfreemap.org/): users need no account, API key, browser
preload session or SD preparation. Enable **Map** in the Flight Tracker theme,
set a location, connect Wi-Fi and insert an SD card. The theme's existing road
colour, opacity and width controls continue to apply.

Road data is fetched and decoded in a background task; the UI writes small chunks
to SD and publishes completed tiles. Only motorway and trunk line features are
retained; ramps and other map layers are excluded. Switching Map off cancels the
current plan and hides roads, while retaining SD files. Changing home, range or
source invalidates in-flight results. Newly published geometry refreshes the map
without resetting aircraft trails, including when the aircraft feed is unavailable.

Flight Tracker shows the download progress banner from the previous map experiment:
waiting time, live received/expected KB, preparing/saving, saved/total tiles, and
retry countdown or failure. It also reports cache clearing. The banner updates
twice per second and hides once coverage is complete or the theme disables Map.
The background worker supplies atomic counters; all LVGL changes stay on the UI task.

This is a prototype, with host tests and a firmware build. User-installed MiltonBanana/2.18.18
has now been monitored on hardware: [Salt Lake City observation](vector-road-maps-hardware-2026-10-21.md).
Version MiltonBanana/2.18.20 replaces per-tile stack allocation with one persistent
24 KiB internal worker stack reserved early during startup. Version MiltonBanana/2.18.19's
PSRAM stack caused a confirmed TCP assertion/reboot and must not be used.
Cold loads, stack use under heap pressure, SD/audio
contention and visual placement still need hardware validation. The monitoring work did not flash or
deploy firmware.

Version MiltonBanana/2.18.21 also defers range-change radar rendering while Settings is shown.
LVGL prefers PSRAM for new allocations when internal free memory approaches
16 KiB, retaining room for file locks/network work. Both road readers and the
download manager defer SD operations below 8 KiB internal free or a 2 KiB largest
block. These are best-effort headroom checks, not reservations against concurrent
network allocations. PSRAM allocation failure retains the allocator's existing
internal fallback. The observed MiltonBanana/2.18.20 range-change crash was inside newlib's
file-lock allocation, reached by an immediate hidden-radar SD reprojection.
When changing `include/lv_psram_alloc.h`, ensure LVGL's `lv_mem.c` is rebuilt:
the observed PlatformIO incremental build did not track that custom include.
A clean build also ensures the allocator change is included.

## Source and diagnostics

`http://theorb.local/roads` explains the map behaviour and includes attribution
links. The main configuration page links to it.
The page uses the main page's light background, cards, typography and rounded
controls; download status and attribution remain available. Its static HTML is
served directly from flash to avoid an extra page-sized String allocation.
Advanced options allow an
OpenMapTiles-compatible HTTP(S) XYZ template containing `{z}`, `{x}`, `{y}`, restore
OpenFreeMap, or select SD-only mode. SD-only mode retains the selected source and
reuses its cache. Configuration persists in the `roadvector` NVS namespace;
custom URL credentials are not echoed by the page or diagnostics.

**Clear road cache** on `/roads` removes cached roads for every location and source,
including legacy `r*`, `q*`, `d*` binaries, new `/roads/v2/` binaries and unfinished
download files. It cancels the current request, discards queued results and clears
the road geometry held in RAM. The page shows deletion progress. SD deletion runs
one directory entry per UI pass and waits during theme transfers or audio access.
Only road-cache files in `/roads` and `/roads/v2` are eligible; directories, theme
files and other SD data are left alone. The action is `POST /roads/clear`.

Fresh downloads resume automatically after successful clearing if Map is enabled,
automatic downloads are enabled, and Wi-Fi/location/SD are available. SD-only mode
stays SD-only. A removal error leaves a visible failure and downloads paused; retry
the clear button after resolving the card problem. Missing/empty cache directories
are a successful no-op. Clearing requires no reboot or SD-card removal.

The default template is:

```text
https://tiles.openfreemap.org/planet/latest/{z}/{x}/{y}.pbf
```

`http://theorb.local/roads.json` reports status/error, provider and source hash,
saved/total tiles, last HTTP code, wire bytes, network/decode/save milliseconds,
segment count and cache revision. `contentLength`, `chunked`, `contentEncoding`,
`readCalls` and `idlePolls` describe the last body transfer. `cacheClearing`,
`cacheClear`, `cacheDeleted` and `cacheClearError` report the clear operation.
`workerReady`, `workerStackBytes`, `workerStackExternal`, `workerStackFree`,
`workerCreates` and `workerRequests` report worker allocation and stack headroom.
The worker is created once and waits on a request queue between tiles. Allocation
failure waits for memory without consuming tile attempts. TCP requires an internal
stack; PSRAM stacks are not supported for this worker. Serial output reports the
stack location and per-tile memory.
These timings are per tile; they do not measure
radar compositing time. A failed section is retried up to three times per plan.
Changing location/range or toggling Map starts a fresh plan. HTTP 429 delays that
tile for five minutes. Other failed requests delay it for one minute; requests
are sequential and start at least one second apart. Cached sections are checked
incrementally and skipped. Theme transfers defer SD work; the map reader/writer
use the shared card lock to defer while streaming audio owns it.

[OpenFreeMap's published guidance](https://openfreemap.org/quick_start/) allows
offline downloads within 3,000 tiles or 50 MB, with at most four parallel requests
and an identifying User-Agent. This prototype uses one request at a time, at most
64 tiles per plan, a 512 KiB input ceiling per tile, an identifying OrbOS User-Agent
and an aggregate transfer ceiling below 50 MB including retries. Required
OpenStreetMap/OpenMapTiles credits are displayed in the shared About/startup
credits area rather than on the radar map. Full attribution links are also on
the road settings page; the weather renderer retains its credits.
No MapLibre runtime or style JSON is needed.

HTTP headers are handled by HTTPClient, using `setAcceptEncoding` (this SDK ignores
`Accept-Encoding` in `addHeader`). A bounded, available-driven body reader handles
fixed-length, chunked and connection-close bodies without HTTPClient's additional
body-copy allocation. Temporary zero/negative TLS reads retry under ten-second idle
and thirty-second total deadlines, with cancellation checked during reads. Truncated
bodies, invalid framing, excess size and empty HTTP 200 bodies are rejected with
specific errors. This replaces the generic `incomplete tile` error observed with
HTTP 200 and zero bytes; whether it resolves that device's failure awaits a retest.
Firmware MiltonBanana/2.18.18 fixes the subsequently identified cause of `tile input limit`
with a 24,211-byte response: the SDK's POSIX `MAX_INPUT` macro (255 terminal bytes)
overrode the generic constant name at worker call sites. Renamed tile-budget
constants retain 512 KiB even after SDK headers are included. `inputLimit` and
`inputCapacity` now expose the actual byte budget and body-buffer reservation;
`bytes` and `contentLength` update during the current request.
Gzip is bounded,
inflated with the ESP32 ROM decoder, and checked for size, consumed input and CRC.
HTTP(S) follows the project's existing certificate policy (`setInsecure` for TLS).
The network worker never touches SD or LVGL. It hands one completed road cache to
the UI, which writes at most 2 KiB per pass through a temporary file before rename.

## Cache and geometry

Files live under `/roads/v2/{sourceHash}_{z}_{x}_{y}.bin`. The source hash is FNV-1a
of the exact endpoint template, so different sources do not reuse one another's
geometry. Format `ORV1` is little-endian:

| Field | Size |
| --- | ---: |
| Magic `ORV1` | 4 bytes |
| Zoom, X, Y | 3 × uint32 |
| Extent (16384), segment count, source hash, payload CRC32 | 4 × uint32 |
| Segment endpoints: x0, y0, x1, y1 | 4 × uint16 per segment |

Coordinates are tile-local, clipped to the nominal tile bounds, retaining far more
precision than legacy 1/180-degree global coordinates. Border-aligned lines use
left/top ownership. The full CRC and coordinate bounds are validated before any
strokes are projected. Empty, validated tiles are cached as completed coverage.
Road caches are measured before allocation; only actual output bytes are reserved.
Each cache has a 256 KiB ceiling. Compressed and expanded inputs are individually
limited to 512 KiB; their overlapping allocations can approach 1 MiB during inflate,
plus TLS, inflater and a 24 KiB worker stack. The compressed buffer is freed before
road-cache allocation. These are limits, not measured device peak heap figures.

No vector tiles present for a view: read legacy `r*.bin` roads as before. Once any
valid vector tile is present for that view, draw the available vector coverage
alone to avoid painting legacy and vector roads over one another. Missing sections
then fill progressively; partial vector coverage can temporarily replace a more
complete legacy view. The earlier experimental `q*`/`d*` Overpass caches and browser
preload implementation have not been imported. Existing files remain on SD unless
the user explicitly chooses Clear road cache.

Projection and rasterization remain synchronous UI work when coverage changes,
using the existing static-map snapshot approach. Snapshot pixel reuse was ported
from `fix/flight-tracker-maps` to avoid the allocation assertion previously found
on hardware. Geometry buffers retain their existing capacity limits; exceptionally
dense views can truncate. The new reader batches SD segments instead of issuing
one card read per segment. No cache eviction, age refresh or cross-zoom reuse is
implemented yet. Offline zooming needs cached tiles at the newly selected zoom.
Latitude beyond Web Mercator coverage and ranges above 500 km are unsupported.

## Verification

```text
python tools/test_vector_roads.py
python tools/test_vector_roads.py --live --tls12
pio run -e esp32-s3-amoled-175
```

The portable suite exercises filtering, dictionary order, multipart geometry,
buffer clipping, bad lengths/varints, invalid commands/tags, cancellation, cache
corruption, source isolation, circle projection, longitude wrapping and plan bounds.
A second executable exercises production `roads_sd.cpp` and `coastline.cpp`, with
only LVGL drawing stubbed: vector projection, legacy fallback, empty coverage,
revision changes, Map-off clearing and reusable snapshot allocation.
The cache-clear/transport suite additionally exercises deletion scope, older and
temporary files, empty directories, removal failures/retry, temporary TLS no-data,
partial bodies, fixed/chunked/close framing, malformed chunks, cancellation and
body deadlines. Cache clearing also suppresses the production flight/weather
projection paths until deletion finishes.
The transport tests define the POSIX `MAX_INPUT=255` macro, then successfully read
the reported 24,211-byte body with the real capacity helper. They also exercise
live KB reporting, banner text and truncation, plus the actual 512 KiB boundary.

Five live OpenFreeMap samples decoded successfully, also with the HTTPS client
restricted to TLS 1.2. Representative results:

| Tile | Expanded MVT | Retained road cache |
| --- | ---: | ---: |
| Provo z10/194/387 | 31,414 bytes | 1,128 bytes |
| Provo z11/388/774 | 22,015 bytes | 576 bytes |
| London z10/511/340 | 308,088 bytes | 23,456 bytes |

Live results are saved in `.pio/vector-tests/live-results.json`. They are host
measurements, not ESP32 speed or heap measurements. Gzip inflation in the live host
tool uses Python; the ESP32 ROM inflation path is built but awaits hardware testing.

To prepare an SD cache from a downloaded raw or gzip PBF using the same C++ decoder:

```text
python tools/test_vector_roads.py --input sample.pbf --tile 10 194 387
```

Copy the generated `.pio/vector-tests/sdcard/roads/v2/` files to `/roads/v2/` on the
card. Use `--source` with the exact custom template when preparing another source.
The helper currently discovers MSVC 2017 on this Windows workspace, or uses a host
C++17 compiler on Unix; LVGL dependencies must already be installed by PlatformIO.

For the first hardware comparison, start with Map off, enable it at a location
with no v2 cache, record time to first roads and complete coverage plus heap/PSRAM
and sweep responsiveness, then repeat with Wi-Fi disconnected. Test off/on during
a download, location/range changes, theme transfer and audio playback before
publishing this firmware.
