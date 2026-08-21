#!/usr/bin/env bash
# Publish the built firmware into Orb Studio's public/firmware, with a manifest.
#
# Why this exists. Studio ships four binaries and offers to flash them, and that copy was
# made by hand. On 2026-08-21 the bundle had drifted to a THEME_CAPS 5 build while the
# source was at 8, so Studio's own Flash button quietly rolled a device back three
# capability levels and reported "the newest there is" while doing it. Both builds call
# themselves v1.4.2, because FW_VERSION tracks releases and not what the firmware can read.
#
# So: never copy these by hand again, and always write down what the copy can do.
set -euo pipefail
cd "$(dirname "$0")/.."

STUDIO="${STUDIO_DIR:-$HOME/Developer/hf-sites/buildtheorb/app/public/firmware}"
PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
BUILD=".pio/build/esp32-s3-amoled-175"

[ -d "$STUDIO" ] || { echo "no Studio firmware dir at $STUDIO" >&2; exit 1; }

echo "building..."
"$PIO" run -e esp32-s3-amoled-175 2>&1 | tail -2 | grep -q SUCCESS || { echo "build failed" >&2; exit 1; }

VERSION=$(grep -oE '#define FW_VERSION "[^"]+"' src/config.h | grep -oE '"[^"]+"' | tr -d '"')
CAPS=$(grep -oE 'constexpr int THEME_CAPS = [0-9]+' src/theme_style.h | grep -oE '[0-9]+$')
[ -n "$VERSION" ] && [ -n "$CAPS" ] || { echo "could not read FW_VERSION/THEME_CAPS" >&2; exit 1; }

for f in bootloader.bin partitions.bin firmware.bin; do
  cp "$BUILD/$f" "$STUDIO/$f"
done
# boot_app0 comes from the framework, not our build, and only changes with the core.
BOOT0=$(find "$HOME/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -1)
[ -n "$BOOT0" ] && cp "$BOOT0" "$STUDIO/boot_app0.bin"

cat > "$STUDIO/manifest.json" <<JSON
{
  "version": "$VERSION",
  "caps": $CAPS,
  "built": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "bytes": $(stat -f%z "$STUDIO/firmware.bin" 2>/dev/null || stat -c%s "$STUDIO/firmware.bin")
}
JSON

echo "published v$VERSION caps $CAPS -> $STUDIO"
cat "$STUDIO/manifest.json"
