#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# DESIGN D46 in FS-UAE (no USB there: the machine without a controller), the A1200 with 4 MB of fast RAM: `lsusb`
# starts the USB server through its port (the D11 service "usb"), which finds no xHCI in the hardware table, says so
# once in the kernel log and stops with status 0; lsusb says "no USB controller" and ends 1; `lsusb --hold` (the
# /etc/daemons line) ends 0 at once there; /sbin/usbd started by hand (not the service) cannot take port usb; a second
# lsusb starts the server again and gets the same answer. The kernel's CONI_HELD is tools/con-test.sh's `termtest held`;
# the controller and the devices are tools/usb-host-test.sh's.
# usage: tools/usb-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "FAIL the disk"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-usb-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-usb-serial}
pass=1
L=$OUT/run.log
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 240 tools/tests/usb-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$L"
has "the script ran to the end" '^(.*# )?usb-test-done$'
has "lsusb: no USB controller on this machine" '^lsusb: no USB controller on this machine'
has "... exit 1" '^(.*# )?l0=1'
has "lsusb --hold: nothing to keep running, exit 0" '^(.*# )?h0=0'
has "usbd by hand: port usb is the service's" '^usbd: port usb busy'
has "... exit 1" '^(.*# )?u0=1'
has "lsusb -v: the same answer from a server started again, exit 1" '^(.*# )?l1=1'
has "the service manager started usbd" 'svc: usb: started /sbin/usbd'
# three starts (lsusb, --hold, lsusb -v; the one by hand never takes the port), each a stop with status 0: counted by
# the servers' pids (the kernel log's lines reach the serial port as they happen and again in dmesg's copy)
n=$(tr -d '\r' < "$L" | grep -aoE 'svc: usb: usbd \(pid [0-9]+\) stopped' | sort -u | wc -l)
[ "$n" = 3 ] && echo "PASS three starts of usbd, each a stop with status 0 (not an end the service manager counts)" || { echo "FAIL usbd's stops: $n, wanted 3"; pass=0; }
has "... and each time it said why" 'usbd: no USB controller \(not a Pi 4 or CM4 under Emu68\): stopped until the next boot'
[ $pass = 1 ] && echo "usb-test: all checks passed" || { echo "usb-test: some checks failed (log $L)"; exit 1; }
