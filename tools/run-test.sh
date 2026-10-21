#!/bin/sh
# Boot a config in FS-UAE with serial captured, print the serial log.
# Usage: tools/run-test.sh fsuae/<config>.fs-uae [seconds] [keyscript]
# Tap and emulator run in the same invocation so the PTY is shared.
set -e
CONF="$1"
SECS="${2:-25}"
SCRIPT="$3"
DIR="$(mktemp -d)"
LOG="$DIR/serial.log"

python3 "$(dirname "$0")/serial-tap.py" /tmp/zenix-serial "$LOG" $SCRIPT >"$DIR/tap.out" 2>&1 &
TAP=$!
sleep 1
# Render offscreen: no window to steal focus (or get closed mid-test).
# Override with SDL_VIDEODRIVER=wayland when a visible window is wanted
# (e.g. for grim screenshots).
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" \
timeout "$SECS" fs-uae "$CONF" >/dev/null 2>&1 || true
kill $TAP 2>/dev/null || true
echo "=== serial output ($CONF, ${SECS}s):"
tr -d '\r' < "$LOG"
