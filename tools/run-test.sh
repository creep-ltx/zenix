#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Boot a config in FS-UAE with serial captured, print the serial log.
# Usage: tools/run-test.sh fsuae/<config>.fs-uae [seconds] [keyscript]
# Tap and emulator run in the same invocation so the PTY is shared.
set -e
CONF="$1"
SECS="${2:-25}"
SCRIPT="$3"
DIR="$(mktemp -d)"
LOG="$DIR/serial.log"
# Several sessions may test at once: ZENIX_SERIAL=/tmp/other-name gives this run its own serial port. The config is
# always copied (tools/fsuae_conf.py): the Kickstart directory (ZENIX_KICKSTART_DIR), the build directory and the
# serial port are filled in, so the configs in fsuae/ name no path of this machine.
SERIAL="${ZENIX_SERIAL:-/tmp/zenix-serial}"
python3 "$(dirname "$0")/fsuae_conf.py" "$CONF" "$DIR/test.fs-uae"
CONF="$DIR/test.fs-uae"

python3 "$(dirname "$0")/serial-tap.py" "$SERIAL" "$LOG" $SCRIPT >"$DIR/tap.out" 2>&1 &
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
grep -h '^serial-tap: wait' "$DIR/tap.out" 2>/dev/null || true      # a script's `wait` that timed out says so here
