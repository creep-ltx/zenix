#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The commands that cannot be compared with GNU output (they depend on the clock, the machine or the process list):
# cal, tree, time, pgrep/pkill, kill -STOP/-CONT, watch (and a few filters, for the record). Then the audit batch E
# script (rootfs/tests/batch-e.sh: the shell's loops in flat memory, no silent caps, the expansion and builtin fixes,
# xargs, diff -r, watch, tar, expr :, $PATH in find/xargs/timeout, sort/grep "-", mv): its output, read from the disk
# with debugfs, must equal tools/tests/batch-e.expected line for line.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-cmds2}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae ${CMDS2_SECS:-330} tools/tests/cmds2-test.txt 2>&1 | sed 's/\x1b\[K//g' > $OUT/run.log
pass=1
want() { if grep -aqF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
want "cal: February 2024" "February 2024"
want "cal: a leap day" "25 26 27 28 29"
want "tree: counts directories" "directories"
want "time prints the elapsed time" "real	0m1."
want "pgrep finds the background sleep (count 1)" "1"
want "pgrep -l names it" "sleep"
want "kill -STOP stops it (ps shows one stopped)" "stopped=1"
want "kill -CONT lets it go on (none stopped)" "stopped=0"
want "pkill ends it, pgrep then finds none" "gone=1"
want "watch shows the command and its output" "Every 1s: echo tick"
want "Ctrl-C ends watch" "after-watch"
want "tac (last line first)" "users::100:user,guest"
want "rev" "toor:0::toor"
want "paste -s -d+" "x+y"
debugfs -R "cat /tmp/be.out" build/zenix.hdf > $OUT/batch-e.txt 2>/dev/null
if diff tools/tests/batch-e.expected $OUT/batch-e.txt > $OUT/batch-e.diff; then echo "PASS batch E script: $(wc -l < tools/tests/batch-e.expected) lines as expected"
else echo "FAIL batch E script (expected, then got):"; head -40 $OUT/batch-e.diff; pass=0; fi
if e2fsck -fn build/zenix.hdf > $OUT/e2fsck.log 2>&1; then echo "PASS host e2fsck -fn clean after the run"; else echo "FAIL e2fsck finds errors (see $OUT/e2fsck.log)"; pass=0; fi
exit $((1 - pass))
