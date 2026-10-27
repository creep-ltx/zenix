#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Events and signals in the emulator (DESIGN D16, docs/d16-events-and-signals.md): sigtest's checks, each one line
# "sigtest CHECK: PASS|FAIL (...)" (docs/man/sigtest.txt); then the shell's job control with process groups (^C to a
# pipeline, ^Z/bg/fg with a program's TSTP and CONT handlers, kill %job), ^C to a handler, and less and edit stopped and
# brought back.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-signal}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 420 tools/tests/signal-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' | tr -d '\r' > $OUT/run.log
pass=1
for t in stopsrv handler block ignore kill alarm restart chld pipe pipenb poll group cont srvcall stack forged ttyread select; do
    l=$(grep -a "^sigtest $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS ${l#sigtest }" ;; *) echo "FAIL sigtest $t: ${l:-no result}"; pass=0 ;; esac
done
# the same handlers in a program with shared code (ZEX2, DESIGN D13: the entry and libz's table reached through a5)
for t in handler block ignore alarm restart chld cont srvcall forged; do
    l=$(grep -a "^pure $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS shared code: ${l#pure }" ;; *) echo "FAIL shared code: sigtest $t: ${l:-no result}"; pass=0 ;; esac
done
# the shell's job control (rules 7, 9, 15), handlers in programs (less, edit), the kernel's messages
line() { if grep -aqx -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
rx()   { if grep -aqE -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
line "^C reaches every process of a pipeline (its group)" "pipe-sleeps=0"
line "^Z: a program's TSTP handler runs, then it stops" "sigtest: TSTP handler ran"
rx   "... and the shell has it as a stopped job" "^\[1\]\+ +Stopped +sigtest"
line "bg continues it (SIGCONT): its CONT handler runs" "sigtest: CONT handler ran"
line "fg, then ^C ends it: status 130" "jobber-status=130"
line "^C runs a program's INT handler" "sigtest: INT handler ran"
line "... which ends it with its own status" "intr-status=3"
rx   "Ctrl+Z in less stops the job (man and less)" "^\[1\]\+ +Stopped +man"
line "fg brings less back in raw mode (q quits it at once)" "less-status=0"
rx   "edit stopped by SIGTSTP leaves its screen and stops" "Stopped +edit"
line "fg: edit's CONT handler puts it in raw mode again (Ctrl+Q quits it)" "edit-status=0"
rx   "kill %job ends the whole pipeline (its group)" "^group-left=0 "
line "... and the shell leaves no zombie of its earlier stages after the next prompt" "zombies-after=0"
rx   "a frame without 512 bytes of stack room: the kernel says so" "^\[kernel\] pid [0-9]+ \(sigtest\): stack overflow in a signal: killed"
rx   "a forged frame outside the process: refused" "^\[kernel\] pid [0-9]+ \(sigtest\): bad signal frame at 0x10: killed"
rx   "a forged SR 0x2700 runs in user mode: the privilege violation kills it" "^\[kernel\] pid [0-9]+ \(sigtest\) killed: vector 8 "
grep -aq "overflowed its user stack" $OUT/run.log && { echo "FAIL a stack guard was hit (a handler wrote below the stack)"; pass=0; }
grep -aq "kernel panic" $OUT/run.log && { echo "FAIL a kernel panic"; pass=0; }
exit $((1 - pass))
