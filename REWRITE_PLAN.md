# Orb OS rewrite — handoff from claude.ai chat

Context for Claude Code. This fork of `Ziplock78/orb-os` is being restructured in stages.
Read this file and the existing `CLAUDE.md` before changing anything.

## Goal

Make the firmware easier to maintain and fix the WiFi experience, without breaking
existing devices. A staged refactor, not a big-bang rewrite: the current code contains
many hard-won fixes documented in its comments, and those must survive.

## What we found in the current code

- `src/main.cpp` is ~4,000 lines: boot, networking task, WiFi/NTP, web config page,
  OTA, location lookup and factory reset all live there.
- WiFi uses WiFiManager `autoConnect` once at boot. A manual check calls
  `WiFi.reconnect()` every 20 s after a drop. Changing networks reboots the device,
  because the WiFi/web/mDNS stack "doesn't reliably hot-swap networks in place".
- WiFi setup works three ways: on-device (Settings → WiFi, password typed with the
  knob), the `The Orb Setup` access point with a phone, and `/wifi-reset` on the
  web page. Settings → Reset wipes WiFi plus all settings and reboots into setup.
- TLS memory pressure is a known problem (`-32512 SSL memory allocation failed`).
  Moving mbedTLS buffers to PSRAM only relocated the failure (see the comments in
  `platformio.ini`). The fix is leaner apps, not a bigger memory budget.
- Networking runs on core 0 and LVGL on core 1. Keep that split.
- A working, tested touch driver exists (`src/touch_cst9217.cpp`) but is excluded in
  `build_src_filter` on purpose: the Orb is designed knob-only.
- The desktop simulator (`pio run -e native -t exec`) has headless capture modes
  (`--themeshot`, `--newsshot`, `--settingsshot`, `--bakeshot`, `--readyshot`).

## Must not break

- **NVS namespace `capsuleradar`**: keep it. Renaming it makes every existing Orb look
  factory reset. Any settings restructure needs a migration, not a rename.
- **Partition table**: the `nvs` offset and size must stay byte-identical so saved WiFi
  credentials survive a firmware update.
- **Theme compatibility**: `THEME_CAPS` in `src/theme_style.h` must stay in step with
  Orb Studio. A screen isn't done until firmware and Orb Studio agree
  (`docs/adding-a-screen.md`).
- **Licence**: keep `LICENSE` and `NOTICE` (MIT, Quique Tortosa and Zion Brock).
- **OTA path**: `theorb.local` OTA and Orb Studio's Web Serial flashing must keep working.

## Plan — one branch per stage, flash and test each before merging

1. `refactor/split-main` — split `main.cpp` into `net/wifi`, `net/http`, `web_server`,
   `location`, `storage` and a thin `main`. Move code only; no behaviour change.
2. `feature/wifi-state-machine` — event-driven WiFi (`WiFi.onEvent()` / ESP-IDF network
   events) instead of polling; exponential backoff; several saved networks with fallback;
   the setup portal as a state rather than a separate boot path; switch networks without
   a reboot if the stack allows it (verify on hardware).
3. `refactor/net-client` — one shared HTTPS client and request queue; stream-parse JSON
   with an ArduinoJson filter; per-app memory caps.
4. `feature/settings-v2` — settings layer with a schema version and migration functions,
   still stored under `capsuleradar`.
5. `refactor/core-messaging` — all core 0 ↔ core 1 traffic through typed FreeRTOS
   queues; no shared globals.
6. `feature/web-auth` — check whether the web page, `/wifi-reset` and OTA upload require
   authentication; if not, add a device password set during setup, and signed OTA images.
7. `ci/sim-screenshots` — run the simulator capture modes in GitHub Actions and diff
   against reference images.

Deferred until 1–3 are done: LVGL v8 → v9, and Arduino → pure ESP-IDF.

## Open decision

Whether to re-enable touch for an on-screen keyboard on the WiFi password screen.
The driver exists; the trade-off is design intent (knob-only) and a cramped keyboard on
a 1.75" round screen. The phone setup page is the current easy path.

## Git setup

```
git remote add upstream https://github.com/Ziplock78/orb-os.git
git fetch upstream
```

## Start here

Begin with stage 1 on a new branch. Build with
`pio run -e esp32-s3-amoled-175` and run the simulator before committing.
