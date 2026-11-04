#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The shell's line editor: Enter pressed with the cursor inside the line runs the whole line and leaves the whole
# line on the screen (as bash does), not only the part before the cursor. termtest types "echo one two three", five
# left-arrow key codes and Return, then reads the console's grid back once the output is there.
cd "$(dirname "$0")/.." || exit 1
OUT=${ZENIX_OUT:-/tmp/zenix-lineedit}; mkdir -p $OUT
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >$OUT/build.log 2>&1 || { echo "FAIL the build"; tail -5 $OUT/build.log; exit 1; }
rm -f $BD/zenix.hdf
make -s BUILD=$BD $BD/zenix.hdf >>$OUT/build.log 2>&1 || { echo "FAIL the image"; tail -5 $OUT/build.log; exit 1; }
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 300 tools/tests/lineedit-test.txt 2>&1 | tr -d '\r' > $OUT/run.log
pass=1
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; pass=0; }
grep -q '^FAIL' $OUT/run.log && bad "the script: $(grep '^FAIL' $OUT/run.log | head -1)"
grep -q '^row [0-9]*|zenix:/# echo one two three|' $OUT/run.log && ok "the typed line stays whole on the screen after Enter mid-line" || bad "the line on the screen was cut at the cursor"
grep -q '^one two three' $OUT/run.log && ok "the whole line ran" || bad "the line did not run whole"
[ $pass = 1 ] && echo "lineedit-test: all passed" || { echo "lineedit-test: FAILED (log $OUT/run.log)"; exit 1; }
