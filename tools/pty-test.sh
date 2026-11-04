#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Pseudo-terminals in the emulator (DESIGN D20 7.2, D38 step 1): ptytest's checks, each one line
# "ptytest CHECK: PASS|FAIL (...)" (docs/man/ptytest.txt): names and inode numbers, echo, cooked and raw input, ^C,
# the window size and SIGWINCH, the owner, hang-up, numbers not reused too early, a shell on a pty running echo and ls.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-pty}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 150 tools/tests/pty-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' | tr -d '\r' > $OUT/run.log
pass=1
for t in open ptsname master-ino slave-ino isatty default-size second freed no-such-slave master-eio master-write-eio hung-up-slave \
         onlcr echo cooked erase size winch raw intr hangup owner-refused owner-set owner-user console-size no-reuse reuse \
         sh-echo sh-ls sh-hup; do
    l=$(grep -a "^ptytest $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS ${l#ptytest }" ;; *) echo "FAIL ptytest $t: ${l:-no result}"; pass=0 ;; esac
done
grep -aqx "ptytest: 0 failed" $OUT/run.log && echo "PASS ptytest: 0 failed" || { echo "FAIL ptytest's own count"; pass=0; }
exit $((1 - pass))
