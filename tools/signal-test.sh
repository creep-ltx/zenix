#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Events and signals in the emulator (DESIGN D16, docs/d16-events-and-signals.md): sigtest's checks, each one line
# "sigtest CHECK: PASS|FAIL (...)" (docs/man/sigtest.txt).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-signal}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 150 tools/tests/signal-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' | tr -d '\r' > $OUT/run.log
pass=1
for t in stopsrv; do
    l=$(grep -a "^sigtest $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS ${l#sigtest }" ;; *) echo "FAIL sigtest $t: ${l:-no result}"; pass=0 ;; esac
done
grep -aq "kernel panic" $OUT/run.log && { echo "FAIL a kernel panic"; pass=0; }
exit $((1 - pass))
