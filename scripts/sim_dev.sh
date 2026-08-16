#!/bin/bash
# Keeps the Orb simulator running continuously across rebuilds. Launch this instead of
# `pio run -e native -t exec` directly: it watches src/, include/ and platformio.ini for
# changes, rebuilds in the background while the CURRENTLY RUNNING sim stays up and shows
# "Updating..." (see sim_main.cpp's poll_updating_overlay / /tmp/orb_sim_updating), then
# swaps in the fresh binary once the build finishes. The window never just vanishes for
# the length of a compile.
#
#   scripts/sim_dev.sh
#
# The sim's own RESTART button (or holding Select 8s, matching real hardware) re-execs
# itself in place — same PID — so it doesn't look like a quit to this script; only an
# actual window-close/Esc ends the loop.

set -uo pipefail
cd "$(dirname "$0")/.."

PIO=~/.platformio/penv/bin/pio
BIN=.pio/build/native/program
FLAG=/tmp/orb_sim_updating
LOG=/tmp/orb_sim_build.log
MARKER=/tmp/orb_sim_last_build   # NOT $BIN's own mtime: a no-op incremental rebuild
                                  # (nothing to relink) never touches $BIN, which would
                                  # otherwise make the watcher re-detect the same edit
                                  # forever. This marker always advances, build or no-op.

build() {
    touch "$FLAG"
    "$PIO" run -e native >"$LOG" 2>&1
    local status=$?
    rm -f "$FLAG"
    touch "$MARKER"
    return $status
}

echo "[sim_dev] initial build..."
if ! build; then
    echo "[sim_dev] initial build failed — see $LOG"
    cat "$LOG"
    exit 1
fi

"$BIN" &
SIM_PID=$!
echo "[sim_dev] simulator running (pid $SIM_PID). Watching src/ + include/ for changes..."

while true; do
    sleep 1
    CHANGED=$(find src include platformio.ini -type f -newer "$MARKER" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$CHANGED" -gt 0 ]; then
        echo "[sim_dev] change detected, rebuilding (sim stays up, overlay on)..."
        if build; then
            echo "[sim_dev] build ok, swapping in the new binary"
            kill "$SIM_PID" 2>/dev/null
            wait "$SIM_PID" 2>/dev/null
            "$BIN" &
            SIM_PID=$!
        else
            echo "[sim_dev] build FAILED — leaving the running sim as-is. See $LOG"
            cat "$LOG"
        fi
    fi
    if ! kill -0 "$SIM_PID" 2>/dev/null; then
        echo "[sim_dev] simulator window closed — stopping."
        break
    fi
done
