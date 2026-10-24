#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Priorities in the emulator (DESIGN D10): the built-in servers run at nice -10; nice and renice for root (may raise) and
# for a user (may only lower their own processes; not a server's, not a raise); renice -u; ps's PRI and NI columns;
# then priotest's timed checks (E-clock): equal priorities take turns only at the end of a 50 ms slice, a higher one
# always gets the CPU, a woken higher one runs at once, and priority inheritance (a nice 0 client is not held up by a
# nice 5 spinner while a nice 10 server works for it). Last, a test-only build without inheritance (NO_PRIO_INHERIT)
# must fail that last check, so the check is known to see what inheritance prevents.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-prio}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 150 tools/tests/prio-test.txt 2>&1 | sed 's/\x1b\[K//g' | tr -d '\r' > $OUT/run.log
pass=1
line() { if grep -aqxF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
rx()   { if grep -aqxE -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
rx   "diskd runs at nice -10 (level 8)" " +1 +0 +8 -10 msg +[0-9]+ diskd"
rx   "fsd runs at nice -10 (level 8)" " +2 +0 +8 -10 msg +[0-9]+ fsd"
rx   "the shell runs at nice 0 (level 16)" " +[0-9]+ +[0-9]+ +16 +0 [a-z]+ +[0-9]+ sh"
line "nice: prints the niceness, adds -n, adds up, root may go negative, 10 by default" "n0=0 n5=5 n7=7 r=-5 d=10"
rx   "renice sets a value (root)" "[0-9]+ \(process ID\) old priority 0, new priority 7"
rx   "root may raise with renice" "[0-9]+ \(process ID\) old priority 7, new priority -3"
rx   "ps shows PRI 13 and NI -3 for it" " +[0-9]+ +[0-9]+ +13 +-3 sleep +[0-9]+ sleep"
line "a user may lower with nice" "u5=5 uc=5"
line "a user may not raise with nice" "nice: cannot set niceness -1: permission denied"
line "... and nice says so in its status" "neg=125"
line "a user may lower their own process with renice" "lower=0"
rx   "a user may not raise it again" "renice: [0-9]+: permission denied"
line "... status 1" "raise=1"
line "a user may not renice a server" "renice: 1: permission denied"
line "... status 1" "server=1"
line "renice -u lowers all of a user's processes" "1000 (user ID) old priority 0, new priority 5"
line "... status 0" "user=0"
rx   "ps shows PRI 20 and NI 5 for the user's process" " +[0-9]+ +[0-9]+ +20 +5 sleep +[0-9]+ sleep"
for t in fair prio wake inherit; do
    l=$(grep -a "^priotest $t: " $OUT/run.log | head -1)
    case "$l" in *": PASS"*) echo "PASS ${l#priotest }" ;; *) echo "FAIL priotest $t: ${l:-no result}"; pass=0 ;; esac
done
# the same inheritance check in a kernel built without inheritance: it must fail there
NB=$OUT/noinherit
if make -s BUILD=$NB DIAG=-DNO_PRIO_INHERIT all >/dev/null 2>&1; then
    ZENIX_BUILD=$NB tools/run-test.sh fsuae/a1200-020-fast.fs-uae 50 tools/tests/prio-noinherit-test.txt 2>&1 | sed 's/\x1b\[K//g' | tr -d '\r' > $OUT/noinherit.log
    l=$(grep -a "^priotest inherit: " $OUT/noinherit.log | head -1)
    case "$l" in *": FAIL"*) echo "PASS without inheritance the client is held up: ${l#priotest inherit: FAIL }" ;;
                 *) echo "FAIL the check did not see a kernel without inheritance: ${l:-no result}"; pass=0 ;; esac
else
    echo "FAIL the NO_PRIO_INHERIT build failed"; pass=0
fi
exit $((1 - pass))
