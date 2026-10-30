#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZINE in FS-UAE (DESIGN D29 sections 15 and 16): step 2, the kernel's pause and resume with the null blob
# (/tests/zine/null.blob: back at once through the resume routine), once and then twenty times in a row in a script,
# and Zenix as before afterwards: the memory (as after the first run), the console's input (CON_INPUT), a file written
# and read back. zine runs only at the Amiga's own console: the serial line's input is the shown console's (kernel/tty.c).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-zine}; mkdir -p $OUT
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 400 tools/tests/zine-null-test.txt > $OUT/serial.log 2>&1
L=$OUT/serial.log
pass=1
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; pass=0; }
log=$(tr -d '\r' < $L)
n=$(echo "$log" | grep -ac '^zine: back after 0 s (0)')
[ "$n" = 21 ] && ok "twenty-one pauses and resumes (the kernel's 'back' line 21 times)" || no "the kernel's 'back' line $n times, not 21"
echo "$log" | grep -aq '^count=20 ok=20' && ok "the console's shell ran zine twenty times and went on" || no "the console's loop: $(echo "$log" | grep -a '^count=')"
f1=$(echo "$log" | grep -a -m1 '^fast RAM free' | tr -dc 0-9); f2=$(echo "$log" | grep -a '^fast RAM free' | tail -1 | tr -dc 0-9)
c1=$(echo "$log" | grep -a -m1 '^chip RAM free' | tr -dc 0-9); c2=$(echo "$log" | grep -a '^chip RAM free' | tail -1 | tr -dc 0-9)
[ -n "$f1" ] && [ "$f1" = "$f2" ] && [ "$c1" = "$c2" ] && ok "free memory after twenty more sessions as after the first (chip $c1 KB, fast $f1 KB)" \
    || no "free memory changed: chip $c1 -> $c2 KB, fast $f1 -> $f2 KB"
echo "$log" | grep -aq '^typed-after' && ok "the console takes input afterwards" || no "the console's input afterwards"
echo "$log" | grep -aq '^disk-ok' && ok "a file written and read back afterwards" || no "the disk afterwards"
echo "$log" | grep -aq '^zine-null-done' || no "the script did not reach its end"
echo "$log" | grep -aiE 'panic|exception|guru' && no "the kernel log has a panic or exception"
exit $((1 - pass))
