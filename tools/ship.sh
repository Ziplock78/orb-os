#!/usr/bin/env bash
# One command from a source edit to an Orb Studio that actually offers it.
#
# WHY THIS EXISTS. Getting firmware to a stranger is three steps - bump FW_VERSION, stage
# the bundle, deploy the site - and step three keeps getting skipped. On 2026-09-01 it cost
# an afternoon twice in one session: Studio was still serving 1.81.0 while the Orb on the
# desk ran 1.85.0, so "update firmware" in Studio rolled the device BACKWARDS four versions
# and put a bug back on the glass that had just been fixed. An hour later the two drifted
# again by one version, mid-task, for the same reason.
#
# publish-firmware.sh already prints "NOT DONE YET" when the deploy is outstanding. It is a
# good warning and it did not work, because a warning you can walk past is not a guard.
# Rule six says the shared path enforces it. This is the shared path.
#
# THE ORDER IS THE POINT, and it is rule one made mechanical. Rule one says never deploy a
# build that has not booted on hardware. That has been a promise somebody keeps; here it is
# a precondition the script checks, by flashing the Orb and then ASKING IT what it is
# running. If the Orb does not answer with this exact version, the deploy does not happen.
# There is no flag to skip it, because the flag would be the thing that gets used.
#
#   tools/ship.sh
#
# Stage without deploying (no Orb attached, work in progress): run publish-firmware.sh on
# its own. That path is unchanged and still just puts a binary on disk.
set -euo pipefail
cd "$(dirname "$0")/.."

PIO="${PIO:-$HOME/.platformio/penv/bin/pio}"
PY="${PY:-$HOME/.platformio/penv/bin/python}"
STUDIO_APP="${STUDIO_APP:-$HOME/Developer/hf-sites/buildtheorb/app}"
LIVE_URL="${LIVE_URL:-https://buildtheorb.zionbrock.workers.dev}"

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\n\033[31mSTOPPED: %s\033[0m\n' "$*" >&2; exit 1; }

# ---- 1. build + stage -------------------------------------------------------------
# publish-firmware.sh owns this, including the refusal when the binary moved and
# FW_VERSION did not. Not reimplemented here: two copies of that check would eventually
# disagree, and the one that mattered would be the one nobody ran.
say "building and staging"
bash tools/publish-firmware.sh

VERSION=$(grep -oE '#define FW_VERSION "[^"]+"' src/config.h | grep -oE '"[^"]+"' | tr -d '"')
[ -n "$VERSION" ] || die "could not read FW_VERSION from src/config.h"

# ---- 2. flash the Orb on the cable -------------------------------------------------
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
[ -n "$PORT" ] || die "no Orb on USB. Rule one: the deploy waits until a build has booted on
  hardware, so there is nothing to do here without one. To stage only, run
  tools/publish-firmware.sh instead."

say "flashing $VERSION to $PORT"
"$PIO" run -e esp32-s3-amoled-175 -t upload >/dev/null 2>&1 \
  || die "flash failed. If Orb Studio has the port open in a browser tab, disconnect it there
  and run this again."

# ---- 3. ask the Orb what it is running ---------------------------------------------
# The whole gate. A successful flash says the bytes were written; only the device saying
# its own version back proves they are the bytes now running. ?orb hello answers with
# "fw":"<version>" (orb_link.cpp).
say "asking the Orb what it is running"
RUNNING=$("$PY" - "$PORT" <<'PYEOF'
import serial, sys, time, json, re
port = sys.argv[1]
deadline = time.time() + 45          # generous: the board reboots and re-enumerates
while time.time() < deadline:
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
    except Exception:
        time.sleep(1); continue
    try:
        for _ in range(12):
            s.reset_input_buffer()
            s.write(b"?orb hello\n"); s.flush()
            buf = b""
            for _ in range(40):
                buf += s.read(4096)
                m = re.search(rb'!orb (\{.*\})', buf)
                if m:
                    try:
                        print(json.loads(m.group(1).decode()).get("fw", "")); sys.exit(0)
                    except Exception:
                        pass
                time.sleep(0.05)
    finally:
        s.close()
    time.sleep(1)
print("")
PYEOF
) || true

[ -n "$RUNNING" ] || die "the Orb never answered ?orb hello, so nothing here can say the build
  boots. NOT deploying. Look at the device, then run this again."
[ "$RUNNING" = "$VERSION" ] || die "the Orb is running $RUNNING, not $VERSION. The flash did not
  take. NOT deploying."
echo "   the Orb reports $RUNNING"

# ---- 4. deploy Studio ---------------------------------------------------------------
# Refused on a dirty Studio tree. `wrangler deploy` ships the working directory, not a
# commit, so a half-finished edit sitting in app/src would go out attached to a firmware
# release nobody would think to look inside. Firmware files are excluded from the check
# because step 1 is what changed them.
DIRTY=$(cd "$STUDIO_APP/.." && git status --porcelain -- . ':!app/public/firmware' | head -5)
if [ -n "$DIRTY" ] && [ "${ALLOW_DIRTY_STUDIO:-0}" != "1" ]; then
  printf '%s\n' "$DIRTY" >&2
  die "Orb Studio has uncommitted changes and deploy ships the working tree, not a commit.
  Commit them, or re-run with ALLOW_DIRTY_STUDIO=1 if they are meant to go out."
fi

say "deploying Orb Studio"
( cd "$STUDIO_APP" && npx vite build >/dev/null && npx wrangler deploy --name buildtheorb >/dev/null ) \
  || die "deploy failed. The Orb has $VERSION but Studio does not, which is the out-of-sync
  state this script exists to prevent. Fix and re-run."

# ---- 5. confirm the live site, not the upload log ------------------------------------
# Studio's own rule: source is not shipped, and "it deployed" has been mistaken for "it is
# serving" here before. Cloudflare also takes a few seconds to distribute.
say "confirming the live site"
for i in 1 2 3 4 5 6; do
  LIVE=$(curl -fsS "$LIVE_URL/firmware/manifest.json" 2>/dev/null | grep -o '"version"[^,]*' | grep -o '[0-9][0-9.]*' || true)
  [ "$LIVE" = "$VERSION" ] && break
  sleep 4
done
[ "${LIVE:-}" = "$VERSION" ] || die "deployed, but $LIVE_URL is still serving ${LIVE:-nothing}.
  Give it a moment and check again before telling anyone it is out."

printf '\n\033[32mv%s is on the Orb and on Orb Studio. They agree.\033[0m\n' "$VERSION"
