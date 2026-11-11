#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# cu and /dev/ttyS0 in FS-UAE (DESIGN D12): the other computer's line reaches cu's screen half, the keyboard's line
# reaches the serial port, ~. ends cu and the console's copy on the serial port comes back. tools/cu-test.py types and
# plays the PC; the results are read here from the serial log and the disk image. ZENIX_SERIAL=/tmp/name: own port.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
export ZENIX_OUT=${ZENIX_OUT:-/tmp/zenix-cu}; OUT=$ZENIX_OUT; rm -rf $OUT; mkdir -p $OUT
SERIAL=${ZENIX_SERIAL:-/tmp/zenix-serial}
ZENIX_SERIAL=$SERIAL python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae $OUT/cu.fs-uae || exit 1
python3 tools/cu-test.py $OUT/cu.fs-uae "$SERIAL" > $OUT/run.log 2>&1
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
got=$(debugfs -R "cat /tmp/cu.out" $BD/zenix.hdf 2>/dev/null | tr -d '\r')
log=$(tr -d '\r' < $OUT/serial.log | sed 's/\x1b\[[0-9;:]*[mK]//g')
echo "$got" | grep -q 'Connected to /dev/ttyS0' && check "cu opens /dev/ttyS0" ok || check "cu opens /dev/ttyS0" bad
echo "$got" | grep -q 'hello-from-pc' && check "the other computer's line reaches cu's screen" ok || check "the other computer's line reaches cu's screen" bad
# the serial log holds what Zenix sent; from the typed cu command on: cu's "abc" + CR comes first, alone (the
# console's copy is off while cu runs, so neither the PC's line nor cu's screen output is echoed back), then the
# console's copy again (the prompt, "echo back", "back"). The host test (serial-test.sh) checks the kernel log too.
# The kernel's own notes about the port ("serial: nothing received for 10 s: ...", said at the end of the first console
# line after 10 s without input, which is the typed cu line) are left out: they can come only before cu opens the
# port (while a program owns it the log stays in the ring, kernel/serial.c kputc), so they are not "while cu runs".
after=$(printf '%s\n' "$log" | sed 's/\x1b\[[0-9;:]*[mK]//g' | sed -n '/cu > \/tmp\/cu\.out$/,$p' | sed 1d | grep -v '^serial: ')
first=$(printf '%s\n' "$after" | head -1)
case "$first" in abc*) check "keys typed in cu reach the serial port (and nothing else while cu runs)" ok ;;
    *) check "keys typed in cu reach the serial port (first line after cu: '$first')" bad ;; esac
printf '%s\n' "$after" | grep -qx 'back' && check "after cu the console's copy on the serial port is back" ok || check "after cu the console's copy on the serial port is back" bad
printf '%s\n' "$after" | grep -q 'hello-from-pc' && check "the PC's line is not echoed back to the PC" bad || check "the PC's line is not echoed back to the PC" ok
echo "$got" | grep -q 'cu: disconnected from /dev/ttyS0' && check "~. ends cu" ok || check "~. ends cu" bad
printf '%s\n' "$log" | grep -qx 'cukill-done' && check "a killed cu's receiving half ends at the next byte: the port is free again" ok || check "a killed cu's receiving half ends at the next byte: the port is free again" bad
exit $((1 - pass))
