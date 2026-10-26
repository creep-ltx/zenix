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
# Up to ZENIX_EMU_SLOTS (default 6) FS-UAEs run on this machine at once, each with its own serial port and images
# (several runs sharing a port or an image file spoil each other; FS-UAE itself runs side by side fine). A run takes
# the first free slot, a lock file it holds until its run ends: slot 0 is /tmp/zenix-emulator.lock (other projects on
# this machine take that one with `flock /tmp/zenix-emulator.lock ...` and keep working unchanged), slot N is
# /tmp/zenix-emulator.lock.N. No run waits for anything but a free slot (and a load of 16 or less): no deadlock.
# tools/fsuae_conf.py (emulator_slot) does the same for the cage-rig runs.
LOCK="${ZENIX_EMU_LOCK:-/tmp/zenix-emulator.lock}"
NSLOTS="${ZENIX_EMU_SLOTS:-6}"
while :; do
    i=0
    while [ $i -lt "$NSLOTS" ]; do
        f="$LOCK"; [ $i = 0 ] || f="$LOCK.$i"
        exec 9>>"$f"
        if flock -n 9; then break 2; fi
        exec 9>&-
        i=$((i + 1))
    done
    sleep 2
done
while awk '{ exit !($1 > 16) }' /proc/loadavg; do sleep 30; done       # the machine is busy: wait, holding the slot

python3 "$(dirname "$0")/serial-tap.py" "$SERIAL" "$LOG" $SCRIPT >"$DIR/tap.out" 2>&1 9>&- &
TAP=$!
sleep 1
# Render offscreen: no window to steal focus (or get closed mid-test).
# Override with SDL_VIDEODRIVER=wayland when a visible window is wanted
# (e.g. for grim screenshots).
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" \
timeout "$SECS" fs-uae "$CONF" >/dev/null 2>&1 9>&- || true
kill $TAP 2>/dev/null || true
echo "=== serial output ($CONF, ${SECS}s):"
tr -d '\r' < "$LOG"
grep -h '^serial-tap: wait' "$DIR/tap.out" 2>/dev/null || true      # a script's `wait` that timed out says so here
rm -rf "$DIR"                                                         # the config copy, the log, FS-UAE's save states
