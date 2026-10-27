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

# ZENIX_NOTAP=1: the caller has put something else on the serial port already (tools/slip-test.sh: zenix-slirp, whose
# log holds the console's bytes); the run ends at the timeout or when the caller leaves <serial port>.stop
TAP=
if [ -z "$ZENIX_NOTAP" ]; then
    python3 "$(dirname "$0")/serial-tap.py" "$SERIAL" "$LOG" $SCRIPT >"$DIR/tap.out" 2>&1 9>&- &
    TAP=$!
    sleep 1
fi
# Render offscreen: no window to steal focus (or get closed mid-test).
# Override with SDL_VIDEODRIVER=wayland when a visible window is wanted
# (e.g. for grim screenshots).
# A script's `stop` (tools/serial-tap.py) leaves $SERIAL.stop: FS-UAE is killed at once (a power cut, or simply the end).
rm -f "$SERIAL.stop"
SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" \
timeout "$SECS" fs-uae "$CONF" >/dev/null 2>&1 9>&- &
EMU=$!
while kill -0 $EMU 2>/dev/null; do
    if [ -e "$SERIAL.stop" ]; then pkill -KILL -P $EMU 2>/dev/null; break; fi
    sleep 0.2
done
wait $EMU 2>/dev/null || true
rm -f "$SERIAL.stop"
if [ -n "$TAP" ]; then kill $TAP 2>/dev/null || true; wait $TAP 2>/dev/null || true; fi   # the tap says whether the run ended during a wait
echo "=== serial output ($CONF, ${SECS}s):"
if [ -f "$LOG" ]; then tr -d '\r' < "$LOG" | sed 's/\x1b\[[0-9;:]*m//g'; fi   # colour codes (DESIGN D14) are not text
# A script's `wait` that timed out, or that was still waiting when the run ended, is a failure (audit 2026-10-26 F7):
# the tap's "FAIL: serial-tap: ..." lines go here, at the end of the output, and this script exits 3. With
# ZENIX_TIMEOUTS set (make check) the tap also adds them to that file, and check.sh fails the test whatever the test
# script makes of the output. A `waitopt` that timed out is allowed and only said.
rc=0
grep -q '^serial-tap: \(optional\|the run ended during an optional\)\|^FAIL: serial-tap: ' "$DIR/tap.out" 2>/dev/null && echo   # own lines
grep -h '^serial-tap: \(optional\|the run ended during an optional\)' "$DIR/tap.out" 2>/dev/null || true
if grep -q '^FAIL: serial-tap: ' "$DIR/tap.out" 2>/dev/null; then grep -h '^FAIL: serial-tap: ' "$DIR/tap.out"; rc=3; fi
rm -rf "$DIR"                                                         # the config copy, the log, FS-UAE's save states
exit $rc
