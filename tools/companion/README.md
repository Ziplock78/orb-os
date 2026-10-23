# Orb + PrintSphere companion

Web installer: **https://lerxtwood.github.io/orb-os/**

The installer lists compatible GitHub releases and downloads their checked images
from a same-origin Pages mirror. It detects the connected device's flash layout.
Existing companion installations update only the two application slots. Standalone
Orb migration first checks the artwork cache and offers a verified, full recovery
backup; it preserves Orb settings and retained cache sectors. A completely erased
device can be initialized. Other layouts are refused without writing.

The browser supports Chrome/Edge desktop with Web Serial. Its recovery backup stays
on the user's computer and is never uploaded. Keep that file until the installation
is verified. The installer never erases the whole chip.

For the 16 MiB ESP32-S3 AMOLED 1.75 device. Orb remains the normal firmware;
select **Printer**, then press to reboot into PrintSphere. From v2.16.27-companion,
jig the dial to open **Return to Orb**, then press the dial
to confirm. Turn to cancel, or let the menu close after eight seconds.
From v2.16.38-companion, turn either way and reverse within 250 ms to open
the menu immediately, with no limit on the length of either turn. The same
45 ms minimum reversal gap as Orb filters encoder bounce. Continued gesture
movement is absorbed until the dial has been quiet for 350 ms; a subsequent
turn cancels the menu. A fresh press after the menu opens confirms the return.
Only the running firmware serves its web interface.

PrintSphere's dial uses Orb's GPIO18(A)/17(B)/16(button) wiring and gesture timing.
Clockwise advances right through available print pages; counter-clockwise goes
left. It starts on printer status, stops at the ends, and skips unavailable pages.
Page movement waits until the dial has settled for 250 ms so a jig can open the
menu without bouncing between pages. Touch page swiping and
the old touch return shortcuts are removed; existing print-control taps and
brightness gestures remain. The existing power-save loop wakes the display for
dial activity, while interrupts retain quick turns and presses during rendering.
Input is routed by the existing LVGL worker, without another task stack.

Orb's configuration, USB theme protocol, web theme upload, SD themes, and theme
sync code are retained. PrintSphere uses a separate `ps_nvs` partition and imports
Orb's saved WiFi credentials on first use. Configure the printer through
PrintSphere's existing interface. The original PrintSphere checkout is untouched.

The flash theme cache shrinks from 9.625 MiB to 4.5 MiB. The installer refuses to
truncate an existing cache. Larger future themes still use Orb's existing SD
fallback, which may be slower. PrintSphere has 4 MiB for firmware, 512 KiB for
settings, and 640 KiB for uploaded sounds.

## Build and initial installation

Run from the Orb repository in PowerShell. These examples use the local PlatformIO
venv; another Python with PlatformIO and pyserial can also be used.

```powershell
.\.pio\build-venv\Scripts\python.exe -m platformio run -e esp32-s3-amoled-175-companion
.\.pio\build-venv\Scripts\python.exe tools\companion\prepare_printsphere.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools\companion\build_printsphere.ps1
```

Before migration, read **all 16 MiB** of the standalone Orb device with esptool
into `.pio/companion/orb-before-dualboot.bin`, and save its SHA256 digest in
`.pio/companion/orb-before-dualboot.sha256`. Never replace this recovery backup.
The backup and packaged image contain private settings; keep them local.

```powershell
.\.pio\build-venv\Scripts\python.exe tools\companion\package.py
.\.pio\build-venv\Scripts\python.exe tools\companion\test_package.py
.\.pio\build-venv\Scripts\python.exe tools\companion\flash.py install --port COM5
```

`package.py` checks the original layout, backup hash, both generated partition
tables, firmware identities/sizes, and theme-cache bounds. It retains Orb's NVS
and cached artwork, installs both firmwares with the newer PrintSphere bootloader,
and initializes the boot selector to Orb. Installation is only for migration from
the original standalone layout; repeating it would discard PrintSphere settings.

## Subsequent firmware updates and recovery

Build the appropriate companion firmware, then write only its app partition:

```powershell
.\.pio\build-venv\Scripts\python.exe tools\companion\flash.py update-orb --port COM5
.\.pio\build-venv\Scripts\python.exe tools\companion\flash.py update-printer --port COM5
```

These commands verify the live partition table and preserve settings, artwork,
and the other firmware. Do not use ordinary PlatformIO upload, IDF flash, or a
stock web **firmware** installer with this layout. Ordinary PlatformIO upload is
blocked for the companion environment. On-device firmware OTA remains disabled.
The **Orb Companion** web installer is the supported browser firmware updater.
Orb Studio theme uploads and web configuration are separate from firmware flashing.

To restore the complete pre-test standalone Orb flash:

```powershell
.\.pio\build-venv\Scripts\python.exe tools\companion\flash.py restore --port COM5
```

Restoration returns flash settings to their backup-time values; it does not roll
back files on the SD card. The helper verifies the backup hash before writing.

## Publishing releases and the website

Use the firmware version from the most recently merged upstream revision with
`-companion` appended: upstream `2.17.00` becomes `v2.17.00-companion`.
Keep upstream's `FW_VERSION` in source; the release workflow validates the tag
against it and stamps the companion suffix into the published firmware. Do not
increment a separate companion version counter. If that tag already exists,
resolve the release naming explicitly rather than overwriting a published tag.

The fork's `release.yml` builds both firmwares on a `v*` tag and publishes exactly
four public images plus `companion-release.json`. PrintSphere's source revision is
pinned in `printsphere-ref.txt`. Its settings-storage and installer-link adaptations
are applied by `prepare_printsphere.py` before building. Release builds stamp Orb
with the tag version and PrintSphere with the same version plus `-orb`.

Enable GitHub Pages with **GitHub Actions** as the source once. After publishing a
release, the reusable `companion-pages.yml` mirrors compatible releases, verifies
their SHA256 hashes, and deploys the installer. It can also be run manually to refresh
the release list. Draft releases are excluded; previews are labeled. The previous
standalone build environment remains available for development, but is not used for
the fork's combined releases.

Local preview from already-built firmware:

```powershell
python tools/companion/release_assets.py --output .pio/companion/public-release --version v2.16.26-companion --commit local --printer-commit 24bf0f4e237be151b036114b2ea9249c89d7a120
python tools/companion/build_site.py --site .pio/companion/site --local .pio/companion/public-release --tag v2.16.26-companion
python -m http.server 8080 --directory .pio/companion/site
```

`test_layout.mjs` tests protected flash regions and rejected layouts/cache sizes.
`test_release.py` tests public asset whitelisting and integrity checks.
`test_site.py` uses Playwright with a simulated device to exercise the browser's
update flow, corrupt downloads, foreign layouts, and mobile layout. The simulated
test does not flash hardware. `smoke.py` exercises real Orb configuration pages and
both existing USB/HTTP theme-file transports using a temporary folder.

`test_dial.cpp` runs the production quadrature decoder and gesture router with
direction changes, contact chatter, slow reversals, unrestricted turn lengths,
gesture-tail draining, menu timeout/cancellation, early presses, and millisecond
wrap. Compile with a host
C++17 compiler and `-Itools/companion`. Its tests also run in both CI workflows.
