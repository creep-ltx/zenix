#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The shell's line editor: Enter pressed with the cursor inside the line runs the whole line and leaves the whole
# line on the screen (as bash does), not only the part before the cursor. termtest types "echo one two three", five
# left-arrow key codes and Return, then reads the console's grid back once the output is there. Then a line typed on
# the serial port: its echo on the serial copy holds the prompt once (only what changed is drawn).
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
grep -q '^row [0-9]*|zenix:/# zpkg -Qq coreutils *|' $OUT/run.log && ok "Tab after zpkg -Qq completes an installed package's name (coreutils)" || bad "Tab did not complete the package name"
sed 's/\x1b\[[0-9;]*[A-Za-z]//g' $OUT/run.log | grep -q '^ahiru$' && ok "... and zpkg -Qq prints names only" || bad "zpkg -Qq did not print the name alone"
# typed at the end of the line, each key writes that character alone (DESIGN D28 20): the prompt is drawn once, not
# once a key (it was "\r", the prompt and the whole line for every key)
n=$(sed 's/\x1b\[[0-9;]*[A-Za-z]//g' $OUT/run.log | grep -v "^row [0-9]*|" | grep -o "# printf" | wc -l)
[ "$n" = 1 ] && grep -q '^qz-ok' $OUT/run.log && ok "typing at the end of the line writes only the new characters (the prompt drawn once)" ||
    bad "typing redrew the line: the prompt and 'printf' $n times on the serial copy"
# Tab twice on a path lists the matches by their last part (Latex/ Loonies/), as bash does, not the whole path
if grep -q '^row [0-9]*|Latex/ *Loonies/ *|' $OUT/run.log && ! grep -q '^row [0-9]*|/tmp/tc/Latex/' $OUT/run.log; then
    ok "Tab lists a path's matches by their last part (Latex/  Loonies/)"
else bad "Tab's list of a path's matches: $(grep -m1 'Latex/' $OUT/run.log)"; fi
# Tab on a name with a space and brackets puts backslashes in (as bash), so the line runs: cd lands in the folder
if grep -q '^row [0-9]*|zenix:/# cd /tmp/tc/001\\ Star\\ \\(Lotus\\)/' $OUT/run.log && grep -q '^row [0-9]*|/tmp/tc/001 Star (Lotus) *|' $OUT/run.log; then
    ok "Tab escapes a name's space and brackets (cd /tmp/tc/001\\ Star\\ \\(Lotus\\)/), and cd goes there"
else bad "Tab on a name with a space and brackets: $(grep -m1 'tc/001' $OUT/run.log)"; fi
[ $pass = 1 ] && echo "lineedit-test: all passed" || { echo "lineedit-test: FAILED (log $OUT/run.log)"; exit 1; }
