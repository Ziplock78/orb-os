# Lean weather radar redesign

> **STATUS (implemented):** Pieces 1 & 2 done, flashed, verified stable & working on the
> stock prebuilt-libs build (weather radar fetches successfully; 0 reboots / 0 `-32512` /
> 0 watchdog over multi-minute watches; PSRAM free ~947KB -> ~2MB). Fixed 50mi radius live,
> 3 frames live, 2s/frame + 5s-hold animation live.
> **Piece 3 (burn-in roads) turned out NOT to be needed for stability** — the watchdog
> never tripped once frames dropped to 3 on the stock (fast, hardware-crypto) build, so it
> was deferred as an optional optimization, not built. The road projection is already
> cached per-location (only re-projects when the home location changes, which with a fixed
> radius is rare). Open item: confirm it stays working over *hours* (fragmentation builds
> slowly); the reduced churn should hold it but only long runtime proves it. Details below
> are the original plan.


## Why

The weather radar has two failure modes on this memory- and CPU-constrained ESP32-S3:

1. **`-32512` "SSL - Memory allocation failed"** — the device can't find a big enough
   contiguous block for a TLS handshake. Moving mbedTLS buffers to PSRAM (a custom
   ESP-IDF build, tried and reverted, see `platformio.ini` history) only *relocated* the
   failure: the weather radar's own heavy buffers then exhaust PSRAM and TLS fails there
   too. Whack-a-mole. The device is simply tight, and the weather app is the heaviest load.
2. **Task-watchdog reboot loop** — drawing the road overlay re-runs `roads_project_flat`
   (~43k points of software double-precision trig) on the core-0 network task, stalling
   it past the ~5s watchdog. Backtrace confirmed: `adsb_task -> wx_radar_fetch_frame ->
   roads_project_flat -> sin`.

The right answer is not a bigger memory budget, it's a **smaller demand**. This design
(Zion's) shrinks what the weather app asks for so it fits the hardware's real limits.

## The design

### 1. Single fixed radius (drop user-selectable zoom)
- Lock to one range (**50 mi**). Remove the zoom tiers (`WX_ZOOM[]`, tier cycling, the
  50/100mi UI). Fewer code paths, and, critically, no re-projection of the road overlay on
  tier change (a tier change currently re-runs the expensive projection).
- Files: `wx_radar_client.cpp` (`WX_ZOOM`, `composite_zoom`, `draw_roads` tier arg),
  `wx_radar.*`, the Weather app knob handler in `main.cpp`/`radar_view.cpp`.

### 2. Three frames, simple cycle (down from 7)
- Keep ~3 recent RainViewer frames (last ~10-15 min). Animate ~2s/frame, hold the newest
  ~5s, loop.
- Directly cuts memory two ways: fewer persistent frame buffers held, and fewer per-cycle
  tile downloads churning memory (each download is a TLS handshake + up to ~260KB PSRAM
  image buffer + PNG decode). Fewer downloads = far less of the memory pressure that
  triggers `-32512`.
- Files: `WX_RADAR_FRAMES` (config), `wx_radar.*` frame-buffer allocation, the
  slot-fill/animation loop.

### 3. Static "burn-in" road overlay (the key fix for the reboots)
- Compute the road overlay **once, when the location is set**, render it into a single
  static bitmap, and reuse that bitmap for every weather frame until the location changes.
  During animation, just composite precip over the pre-rendered road bitmap, no
  re-projection, no per-frame line drawing.
- Turns the watchdog trigger from a recurring event into a rare, one-time one (only on
  location change).
- The one remaining care point: the one-time projection still has to run once and must not
  stall then. Options: run it with much more aggressive yielding than today, or on a
  dedicated task that resets the watchdog, or on core 1 during idle. It's "once, when you
  move," which is very solvable, versus "constantly," which is what breaks it now.
- Today the code already caches the *projected points* (`s_roadPts`, re-projected only on
  center/tier change), and draws lines every frame. The redesign goes further: cache the
  *rendered bitmap*, so per-frame cost is a memcpy/composite, not a redraw, and the
  projection is genuinely one-shot per location.
- Files: `wx_radar_client.cpp` (`draw_roads`, `composite_zoom`, `wx_radar_fetch_frame`),
  `coastline.cpp`/`roads.cpp` (`geo_project_polylines_flat`, `YIELD_EVERY`).

## Expected outcome

- Weather radar works within the stock (prebuilt-libs) build, no custom ESP-IDF rebuild,
  no fragile `custom_sdkconfig`. The mbedTLS-to-PSRAM hack should become unnecessary once
  the app's footprint drops.
- No watchdog reboots (road math is one-shot, not per-frame).
- Simpler, more maintainable weather app.

## Verify

- Build stock (`pio run -e esp32-s3-amoled-175`), flash, then watch a long serial capture
  (use `tools/orb_watch.py`) for: no `-32512`, no `task_wdt`/reboot over 5+ minutes,
  weather frames actually drawing, flight tracker unaffected, PSRAM staying healthy.
- Confirm on-device: enter Weather Radar, see the precip animate over a stable road map;
  it should not reboot; changing location re-bakes the road map once.

## Notes / history

- All of tonight's mbedTLS/custom-build experiment is reverted in `platformio.ini`
  (disabled block + comment) and `.dummy/idf_component.yml`; the details are in git
  history if the memory route is ever revisited (e.g. shrinking `SSL_IN/OUT_CONTENT_LEN`).
- The persistent-connection experiment (holding one TLS connection open forever) was also
  reverted, it starved later connections of internal RAM and did not fix the original
  reboot. Fetches are back to fresh-per-request.
