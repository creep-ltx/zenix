#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Job control in the emulator: Ctrl-Z stops the foreground program, jobs lists it, ps shows it stopped, bg continues it,
# fg brings it back (and Ctrl-C then ends it with status 130), kill ends a stopped job.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=/tmp/zenix-jobs; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 75 tools/tests/jobs-test.txt 2>&1 | sed 's/\x1b\[K//g' > $OUT/run.log
pass=1
want() { if grep -aqF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
want "^Z stops the foreground program" "Stopped    sleep"
grep -aqE "stopped +[0-9]+ sleep" $OUT/run.log && echo "PASS ps shows it stopped" || { echo "FAIL ps shows it stopped"; pass=0; }   # any size: programs shrink and grow
want "bg continues it" "sleep &"
want "the continued job runs on" "Running    sleep"
want "fg brings it back and ^C ends it (status 130)" "status=130"
want "a background job gets a number" "[1] "
want "kill ends a stopped job" "killed=0"
n=$(grep -ac '^alpha-one' $OUT/run.log); [ "$n" -ge 2 ] && echo "PASS Ctrl-R finds an older command and Enter runs it" || { echo "FAIL Ctrl-R (alpha-one seen $n times)"; pass=0; }
n=$(grep -ac '^beta-two' $OUT/run.log); [ "$n" -ge 2 ] && echo "PASS Ctrl-R again, then Enter" || { echo "FAIL Ctrl-R twice (beta-two seen $n times)"; pass=0; }
exit $((1 - pass))
