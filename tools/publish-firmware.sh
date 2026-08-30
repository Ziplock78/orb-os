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

# A NEW BINARY UNDER AN OLD VERSION NUMBER IS INVISIBLE, and this is the failure that
# actually keeps happening. Studio decides whether to offer an update by comparing version
# STRINGS: the Orb says 1.63.1, the bundle says 1.63.1, so Studio reports "up to date" and
# there is no button to press. The change is sitting right there in the bundle and cannot
# be reached. It looks like the flasher is broken; nothing is broken, the build was just
# never given a name of its own.
#
# So: if the binary moved and FW_VERSION did not, refuse. Bumping is one edit, and it is
# the edit that makes the work reachable.
OLD_VERSION=$(grep -o '"version"[^,]*' "$STUDIO/manifest.json" 2>/dev/null | grep -o '[0-9][0-9.]*' || echo "none")
if [ "$OLD_VERSION" = "$VERSION" ] && [ -f "$STUDIO/firmware.bin" ] \
   && ! cmp -s "$BUILD/firmware.bin" "$STUDIO/firmware.bin"; then
  echo >&2
  echo "REFUSING TO PUBLISH: the firmware changed but FW_VERSION is still $VERSION." >&2
  echo >&2
  echo "  Studio compares version strings, so an Orb already running $VERSION would be" >&2
  echo "  told it is up to date and would never be offered this build." >&2
  echo >&2
  echo "  Bump FW_VERSION in src/config.h, then run this again." >&2
  exit 1
fi

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

# THIS IS NOT THE LAST STEP, and forgetting that has now shipped an old firmware twice.
#
# Copying the binary into public/firmware only puts it on disk. Orb Studio serves what its
# BUILD contains, so until the site is rebuilt and deployed the flasher hands out whatever
# version was current the last time somebody ran vite build. The second time this happened,
# the Orb was flashed BACKWARDS over a newer build from the cable, which looks like the
# update silently failing.
#
# So the script says what is left rather than trusting anyone to remember, and it says it
# after checking, because a reminder that fires when it is not needed gets ignored.
DIST_MANIFEST="$(dirname "$STUDIO")/../dist/client/firmware/manifest.json"
DIST_VERSION="$(grep -o '"version"[^,]*' "$DIST_MANIFEST" 2>/dev/null | grep -o '[0-9][0-9.]*' || echo "none")"
if [ "$DIST_VERSION" != "$VERSION" ]; then
  echo
  echo "=============================================================="
  echo " NOT DONE YET. Orb Studio still serves ${DIST_VERSION}."
  echo " Flashing from Studio right now would install ${DIST_VERSION}, not ${VERSION}."
  echo
  echo "   cd $(dirname "$STUDIO")/.. && npx vite build && npx wrangler deploy --name buildtheorb"
  echo "=============================================================="
fi
