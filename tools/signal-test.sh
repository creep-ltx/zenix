#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Events and signals in the emulator (DESIGN D16, docs/d16-events-and-signals.md): sigtest's checks, each one line
# "sigtest CHECK: PASS|FAIL (...)" (docs/man/sigtest.txt), also as a program with shared code.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-signal}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 420 tools/tests/signal-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' | tr -d '\r' > $OUT/run.log
pass=1
for t in stopsrv handler block ignore kill alarm restart chld pipe poll group cont srvcall stack forged ttyread select; do
    l=$(grep -a "^sigtest $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS ${l#sigtest }" ;; *) echo "FAIL sigtest $t: ${l:-no result}"; pass=0 ;; esac
done
# the same handlers in a program with shared code (ZEX2, DESIGN D13: the entry and libz's table reached through a5)
for t in handler block ignore alarm restart chld cont srvcall forged; do
    l=$(grep -a "^pure $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS shared code: ${l#pure }" ;; *) echo "FAIL shared code: sigtest $t: ${l:-no result}"; pass=0 ;; esac
done
line() { if grep -aqx -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
rx()   { if grep -aqE -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
rx   "a frame without 512 bytes of stack room: the kernel says so" "^\[kernel\] pid [0-9]+ \(sigtest\): stack overflow in a signal: killed"
rx   "a forged frame outside the process: refused" "^\[kernel\] pid [0-9]+ \(sigtest\): bad signal frame at 0x10: killed"
rx   "a forged SR 0x2700 runs in user mode: the privilege violation kills it" "^\[kernel\] pid [0-9]+ \(sigtest\) killed: vector 8 "
grep -aq "overflowed its user stack" $OUT/run.log && { echo "FAIL a stack guard was hit (a handler wrote below the stack)"; pass=0; }
grep -aq "kernel panic" $OUT/run.log && { echo "FAIL a kernel panic"; pass=0; }
exit $((1 - pass))
