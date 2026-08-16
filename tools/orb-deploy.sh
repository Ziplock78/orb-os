#!/bin/bash
# One-command deploy for the Orb. Workflow rule R1 (see agentic-os
# data/documents/plans/orb-development-workflow.md): every step, in the only correct
# order, stopping loudly at the first failure. Nobody runs these steps by hand.
#
#   tools/orb-deploy.sh [themeId]      themeId defaults to 3 (Steam Punk)
#
# Steps: free the serial port -> clean if generated sources changed -> build sim ->
# sim selftest -> build device -> flash (port by Espressif VID) -> send theme files
# (Launch Kit reboots the Orb itself, rule R5) -> poll /health -> budget check.
set -u
cd "$(dirname "$0")/.."
PIO="$HOME/.platformio/penv/bin/pio"
THEME_ID="${1:-3}"
ORB_HOST="192.168.0.155"
LK="http://localhost:4183"
step() { printf '\n== %s\n' "$1"; }
fail() { printf 'FAIL: %s\n' "$1"; exit 1; }

step "free the serial port (kill any log captures)"
pkill -9 -f "cap[23]\.py" 2>/dev/null; pkill -9 -f "readser" 2>/dev/null; sleep 1
echo "ok"

step "stale generated sources check"
# Launch Kit regenerates src/custom_*.c; PlatformIO's incremental build has been seen
# keeping stub-era objects and failing at link with 'undefined reference'. Checksum the
# generated sources; any change since the last deploy forces a clean.
SUM=$(cat src/custom_*.c 2>/dev/null | /usr/bin/shasum | cut -d' ' -f1)
STAMP=".pio/orb-deploy-gen-sum"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$SUM" ]; then
  echo "unchanged since last deploy — incremental build ok"
else
  echo "generated sources changed — forcing clean build"
  "$PIO" run -e esp32-s3-amoled-175 -t clean >/dev/null 2>&1
  "$PIO" run -e native -t clean >/dev/null 2>&1
fi

step "build simulator"
"$PIO" run -e native 2>&1 | tail -2 | grep -q SUCCESS || fail "simulator build"
echo "ok"

step "simulator selftest (knob -> menu -> settings, real input_router)"
OUT=$(SIM_SELFTEST=1 ./.pio/build/native/program 2>&1 | grep "selftest")
echo "$OUT" | tail -3
echo "$OUT" | grep -q "Settings>Range: PASS" || fail "sim selftest"

step "build device firmware"
"$PIO" run -e esp32-s3-amoled-175 2>&1 | tail -2 | grep -q SUCCESS || fail "device build"
echo "$SUM" > "$STAMP"
echo "ok"

step "find the Orb (Espressif VID 303A, never by path)"
PORT=$("$PIO" device list --json-output 2>/dev/null | python3 -c "
import json,sys
for e in json.load(sys.stdin):
    if 'VID:PID=303A' in (e.get('hwid') or '').upper(): print(e['port']); break")
[ -n "$PORT" ] && echo "$PORT" || fail "no Orb on USB"

step "flash"
"$PIO" run -e esp32-s3-amoled-175 -t upload --upload-port "$PORT" 2>&1 | tail -2 | grep -q "1 succeeded" || fail "flash"
echo "ok"

step "wait for the Orb on WiFi"
UP=""
for i in $(seq 1 30); do
  if curl -s -m 3 "http://$ORB_HOST/health" >/dev/null 2>&1; then UP=1; break; fi
  sleep 3
done
[ -n "$UP" ] || fail "Orb never came back on WiFi"
echo "ok"

step "send theme files (Launch Kit reboots the Orb after, rule R5)"
R=$(curl -s -m 300 -X POST -H 'Content-Type: application/json' \
     -d "{\"themeId\":$THEME_ID}" "$LK/api/device/send-files")
echo "$R" | python3 -c "
import json,sys
d=json.load(sys.stdin)
print('sent', len(d.get('sent',[])), 'files, rebooted:', d.get('rebooted'))
sys.exit(0 if d.get('ok') else 1)" || fail "send-files: $R"

step "health check after reboot"
H=""
for i in $(seq 1 30); do
  H=$(curl -s -m 3 "http://$ORB_HOST/health" 2>/dev/null)
  # uptime under 60s proves this is the POST-reboot boot, not the pre-reboot one
  echo "$H" | python3 -c "
import json,sys
d=json.load(sys.stdin); sys.exit(0 if d.get('uptime_s',9999) < 60 else 1)" 2>/dev/null && break
  H=""; sleep 3
done
[ -n "$H" ] || fail "no fresh health report after reboot"
echo "$H"
echo "$H" | python3 -c "
import json,sys
d=json.load(sys.stdin)
ok=True
if d.get('psram_free_kb',0)    < 2500: print('BUDGET WARNING: free PSRAM below 2.5 MB'); ok=False
if d.get('psram_largest_kb',0) < 1024: print('BUDGET WARNING: largest free block below 1 MB'); ok=False
print('budgets:', 'PASS' if ok else 'CHECK ABOVE')"

printf '\n== DEPLOY COMPLETE\n'
