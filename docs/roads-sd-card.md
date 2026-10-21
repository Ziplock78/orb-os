# Road maps on the SD card

Flight Tracker's Map switch enables automatic on-device OpenStreetMap road
downloads when Wi-Fi, a saved home location and a mounted SD card are available.
Cached roads work offline and survive restarts. Orb Studio's preview uses stand-in
streets; it does not confirm that the device has downloaded its local roads.

## Preload maps in the browser

Firmware 2.18.12 adds **Preload road maps** to `http://theorb.local/` (direct link:
`http://theorb.local/roads`). Select the current location and/or any of the Orb's
eight saved recent locations, then choose **Preload selected maps**. Coverage
uses the radar range currently configured on the Orb. Insert an SD card first,
and keep the page open and active until it finishes.

The browser downloads OpenStreetMap motorway/trunk geometry directly from the
map service, simplifies and clips it, then uploads the small binary tiles to the
Orb. The device validates counts, lengths, size and cell bounds before renaming
a temporary file into the cache. Each completed section refreshes the map. No
card removal, application installation or change to the renderer is needed.
Uploads are limited to 512 KB per binary tile; map responses to 8 MB.

The page checks existing cache coverage and deduplicates sections shared by
selected locations. It shows waiting/downloading/preparing/saving progress, plus
uploaded, cached and pending counts. From 2.18.13, failed sections retry
automatically until all selected coverage is saved or **Stop** is pressed.
Other pending sections proceed first; each repeatedly failing section waits
30 seconds, then 60, 120, 240 and at most 300 seconds between attempts. Completed
sections are never downloaded again during that run, and the progress bar only
counts saved/cached sections. While waiting, the page shows a retry countdown.
If a full background cell succeeds, its coverage also clears any pending
starter cell for the same road class, avoiding an unnecessary retry.
A failed section leaves completed tiles intact; starting again skips saved
sections. **Stop** cancels an active map
request and preserves completed tiles. SD contention during an upload is retried
up to twice without downloading that section again.

Automatic device downloads remain available. A browser session pauses and
cooperatively cancels device road downloads before it can upload. A request
already waiting for headers may need to time out first, so the initial pause
can take up to about 65 seconds. Heartbeats keep the pause alive; finishing,
stopping, or leaving the page releases it. If the browser disappears without
releasing the session, the device resumes after 45 seconds without activity.
Only one browser session can preload at a time. Theme transfers defer uploads;
audio SD access causes a retry rather than blocking the UI on its lock.

The default service is the device's configured Overpass endpoint. The page's
**Map service** control can select another HTTPS endpoint that allows browser
requests through CORS. Browser TLS and processing avoid ESP32 limitations, but
public-server queues, rate limits and query failures can still delay downloads.
Requests are sequential and start at least ten seconds apart. From 2.18.14,
HTTP 429 pauses all map requests with a visible rate-limit countdown; repeated
429s increase the minimum pause from 60 seconds to 120, 240 and at most 300
seconds until a request succeeds. A readable `Retry-After` header (seconds or
HTTP date) can require a longer pause. The retry log reflects that actual
minimum wait. Some services do not expose this header through CORS, in which
case the fallback cooldown applies. This is independent of the deferred
vector-tile work and uses the existing SD cache format.

For regression checks, run `python tools/test_road_tiles.py`, then
`python tools/test_road_preload.py` with headless Chromium/Edge available
(`--browser` selects its executable). The browser test uses a local fake Orb and
map service, compares the actual embedded JavaScript's binary output against the
C++ generator, and exercises cache skipping, upload retries, partial failures,
resume and stop. `--live` additionally checks one real browser-to-Overpass
download. These checks do not flash the device or exercise its physical SD card.

## Progressive downloads

The device first requests the quarter-degree cell containing home (about 28 km
north to south), then fills coverage with one-degree background cells intersecting
the scope circle, nearest first. Using larger background cells avoids paying the
server's queue delay for dozens of small requests. Each cell has independent
motorway and trunk-road files. A successful request is
saved and displayed immediately, without waiting for the other road class or the
rest of the range. Failed requests preserve previously completed coverage.
Residential streets are not included. A rural location or tight range may show
no roads even with successfully downloaded coverage.

Fetching starts when the prerequisites are available, without the former
15-second startup wait or 30-second location polling interval. Home/range changes
cancel the current request and replan after the worker exits; a request already
waiting for HTTP headers may need to reach its transport timeout first.
Requests run sequentially, with at least
ten seconds between starts; slow requests incur no additional ten-second delay
after completion. Failed sections have a one-minute, then two-minute retry
backoff, with at most three attempts until home/range changes or restart. Other
sections can proceed meanwhile. Failures impose a ten-second global pause;
HTTP 429 imposes one minute. Public-server waits still prevent any guaranteed
completion time.

A Flight Tracker banner distinguishes waiting, downloading (live KB received),
saving, and retrying. Its saved/total counter counts cell/road-class sections,
including successfully downloaded empty sections, rather than roads or a byte
percentage. It disappears when all required sections are cached. Cache inspection
is incremental to avoid a long SD scan on the UI thread.

## Cache and memory

Files are automatically generated under `/roads/`, separate from theme files:

- `q{latIndex}_{lonIndex}_m.bin`: motorways.
- `q{latIndex}_{lonIndex}_t.bin`: trunk roads.
- `d{latFloor}_{lonFloor}_m.bin` and `_t.bin`: larger background sections.

Indices are `floor(coordinate * 4)`, including negative longitudes. Central Provo
uses `q160_-447`: latitude 40.00–40.25, longitude -111.75–-111.50. Geometry is
simplified and clipped to its cell so neighbouring queries do not paint an entire
long road repeatedly. Completed background sections supersede smaller sections
of the same road class, avoiding overlapping rendering.

Existing complete one-degree `r{latFloor}_{lonFloor}.bin` files remain supported
and take precedence over smaller files within that degree cell. Upgrades preserve
them. Download-speed testing needs a location without existing cached coverage.

The binary layout is unchanged: little-endian polyline count (uint16), point
count (uint32), polyline lengths (uint16 each), then latitude/longitude pairs
(int16, 180 units per degree). Only a validated complete response is renamed
into its final filename. Interrupted temporary files are removed before the next
attempt; incomplete responses are never cached as valid coverage.

The network worker never accesses SD or LVGL. Lengths and coordinates are batched
into separate 512-byte packets across roads, replacing two tiny packets per road.
A four-packet queue feeds the UI thread, which writes at most 2 KB per pass and
defers writes during theme transfers or while audio owns the card. Publication
refreshes roads without clearing aircraft trails.

Per-request limits are 8 MB of response data, 150 seconds of body processing,
ten seconds without incoming data, 96 KB of JSON allocations, 2,048 raw points per
road and 16,000 output polylines. Geometry scratch uses about 50 KB PSRAM; TLS and
the worker stack need additional memory. Projection covers at most the home
degree cell and its eight neighbours. Automatic refresh and cache eviction are
not implemented.

Coordinate decoding reuses the JSON document's memory pools and one bounded
128-byte temporary block between points. This avoids clearing and reallocating
the entire document for every coordinate. In the saved Provo motorway response,
the budgeted allocator made two allocations and no reallocations instead of
17,510 allocations and 8,755 reallocations; encoded geometry matched exactly.
Host timing improvements do not establish the speedup on the ESP32.

## Diagnostics

Read `http://theorb.local/roads.json` for:

- `status`, `error`, `http`, `responseLength` (-1 for unknown/chunked).
- `endpoint`, `tile` (including class suffix), `roadClass`.
- `bytesRead` (live, current request), `roadsReceived`, `jsonPeak`.
- `firstSectionMs` (first newly saved section since home/range changed; zero if
  none has been generated), `elapsedMs` (current/last request through publication), `headersMs` (connection,
  server wait and response headers), `downloadMs` (body receive/parse/queue time).
- `areasSaved`, `areasTotal` (cell/class sections for the current view).
- `transportMs` (input refills, including TLS reads, chunk framing and waiting),
  `idleMs` (the deliberate retry sleeps within that time).
- `queueWaitMs` (sending geometry packets to the UI/SD writer), `processingMs`
  (remaining body time: JSON, simplification, clipping and output; includes task
  preemption rather than measuring CPU time alone).
- `readCalls`, `emptyPolls` (body read attempts and retries while awaiting data).
- `revision` (sections published this boot, excluding existing cached files).

Body-profile fields finalize when the body phase ends; `bytesRead` remains live.
Approximately `downloadMs = transportMs + processingMs + queueWaitMs`, with
rounding differences. `idleMs` is already included in `transportMs`; do not add
it again. Serial `[roads_fetch] body profile:` reports the same breakdown.

`ready` means the latest section was saved or all coverage is cached; use the
coverage counters to distinguish these. A neighbouring request may fail while
previously cached roads remain visible. Parser diagnostics include Overpass
`remark` messages even for HTTP 200. Serial `[roads_fetch]` logs report HTTP,
header/body timings and publication. `[roads_sd] flight:` reports opened files
and visible polylines. Radar snapshot and background-rebuild start/completion
timings help distinguish a display stall from a network delay.

Version 2.18.10 fixes a confirmed UI hang after publishing roads: LVGL's snapshot
convenience API asserted forever when a new 651,468-byte pixel allocation failed
in fragmented PSRAM, before reaching its NULL-return path. The map now retains
and redraws into one reusable pixel buffer, invalidating its image-cache entry
before reuse. Large-buffer allocation failures return to the live map layer;
they no longer pass through that allocation assertion. This does not change
the normal snapshot size or require another full-size pixel buffer per refresh.

The default endpoint is `https://maps.mail.ru/osm/tools/overpass/api/interpreter`,
configurable with `ROAD_OVERPASS_URL` in `src/config.h`. Queries allow 45 seconds
and 128 MB of server memory. Headers can take up to 65 seconds; TLS handshakes
have a ten-second timeout. Server memory is independent of Orb RAM. This SDK
uses TLS 1.2. Earlier probes of `overpass-api.de` rejected TLS 1.2 while accepting
TLS 1.3, matching the device's fatal TLS alert. The configured mirror accepted
the firmware's TLS version and existing certificate policy.

Run `python tools/test_road_tiles.py` for portable parser, transport, planning and
clipping tests. With a host C++17 compiler and PlatformIO dependencies installed,
benchmark a live small cell with:

```text
python tools/test_road_tiles.py --live --tls12 --insecure --quarter-cell 160 -447
```

This reports real header/download timings and checks geometry through the shared
parser and clipper. It does not flash the device or measure device rendering,
SD performance or total on-device latency.

Use `--fixture path/to/response.json` to recheck a saved response without another
network request. `--compact-output` tests the optional `noids qt` output modifiers
and compares geometry with a saved standard response. One Provo trunk-road test
preserved geometry but saved only 3.5% of response bytes, so firmware queries
continue to use standard `skel geom` output.

Ensure home is correct and Map has nonzero strength and a visible colour.
Airports use built-in data, independently of road downloads, and require both
the theme's Airports switch and the device's airports setting.
