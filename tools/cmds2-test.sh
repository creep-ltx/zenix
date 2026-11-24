#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The commands that cannot be compared with GNU output (they depend on the clock, the machine or the process list):
# cal, tree, time, pgrep/pkill, kill -STOP/-CONT, watch (and a few filters, for the record), ls of a 450-name
# directory (more than the old 400 cap), msgping's round trips in microseconds, bench reporting failed calls. Then the audit batch E
# script (rootfs/tests/batch-e.sh: the shell's loops in flat memory, no silent caps, the expansion and builtin fixes,
# xargs, diff -r, watch, tar, expr :, $PATH in find/xargs/timeout, sort/grep "-", mv): its output, read from the disk
# with debugfs, must equal tools/tests/batch-e.expected line for line.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
# ZENIX_BOX=1 (tools/box-test.sh): the image's everyday commands are the box's links (DESIGN D25 revision 3, 10 step 1)
MKBOX=${ZENIX_BOX:+BOX=1}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $MKBOX $BD/zenix.hdf >/dev/null 2>&1
if [ -n "$MKBOX" ]; then [ "$(debugfs -R "stat /bin/ls" $BD/zenix.hdf 2>/dev/null | grep -o 'Links: [0-9]*')" = "Links: 65" ] || { echo "FAIL: the image's /bin/ls is not the box (65 links)"; exit 1; }; fi
OUT=${ZENIX_OUT:-/tmp/zenix-cmds2}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae ${CMDS2_SECS:-1300} tools/tests/cmds2-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/run.log
pass=1
want() { if grep -aqF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
want "cal: February 2024" "February 2024"
want "cal: a leap day" "25 26 27 28 29"
want "tree: counts directories" "directories"
want "time prints the elapsed time" "real	0m1."
want "pgrep finds the background sleep (count 1)" "1"
want "sh: a typed UTF-8 byte 0x80 is text, not the Del key (D28 15)" "utf=5"
want "sh: the Del key (ESC [ 3 ~) still deletes under the cursor" "delA=ok"
want "pgrep -l names it" "sleep"
want "kill -STOP stops it (ps shows one stopped)" "stopped=1"
want "kill -CONT lets it go on (none stopped)" "stopped=0"
want "pkill ends it, pgrep then finds none" "gone=1"
want "watch shows the command and its output" "Every 1s: echo tick"
want "Ctrl-C ends watch" "after-watch"
want "tac (last line first)" "zine::11:"
want "rev" "toor:0::toor"
want "paste -s -d+" "x+y"
want "pipes: /bin/sh through four pipes (bulk copies, the ring wrapping; audit OS-13) is the file, a short line too" "pipesum=sameab"
tr -d '\r' < $OUT/run.log | grep -aqxE 'msgping: 500 round trips in [0-9]+ us, [0-9]+\.[0-9] us each' && echo "PASS msgping times round trips in microseconds" || { echo "FAIL msgping times round trips in microseconds"; pass=0; }
want "bench: failed creates are reported, not timed; a writable /tmp gives no failures" "bench f1=2 f2=0"
want "... the line names the count and the first error" "create + write 50 small files      FAILED: 50 of 50 calls (first: open /tmp/bench/f00:"
want "ls lists all 450 names of a directory (hard links: the disk has few inodes; -a adds . and ..; -l finds the last)" "lsn=450 lsa=452 lsl=1"
want "awk refuses a field width over 4096 (audit A4)" "awk: field width too large (at most 4096)"
want "... with status 2" "awkrc=2"
want "... and a precision over it too" "awkrc2=2"
debugfs -R "cat /tmp/be.out" $BD/zenix.hdf > $OUT/batch-e.txt 2>/dev/null
if diff tools/tests/batch-e.expected $OUT/batch-e.txt > $OUT/batch-e.diff; then echo "PASS batch E script: $(wc -l < tools/tests/batch-e.expected) lines as expected"
else echo "FAIL batch E script (expected, then got):"; head -40 $OUT/batch-e.diff; pass=0; fi
# the one-system conventions (audit 2026-11-23 section 3): usage lines, unknown options, error texts, statuses
debugfs -R "cat /tmp/conv.out" $BD/zenix.hdf > $OUT/conv.txt 2>/dev/null
if diff tools/tests/conv.expected $OUT/conv.txt > $OUT/conv.diff; then echo "PASS conventions script: $(wc -l < tools/tests/conv.expected) lines as expected"
else echo "FAIL conventions script (expected, then got):"; head -40 $OUT/conv.diff; pass=0; fi
if e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1; then echo "PASS host e2fsck -fn clean after the run"; else echo "FAIL e2fsck finds errors (see $OUT/e2fsck.log)"; pass=0; fi
exit $((1 - pass))
