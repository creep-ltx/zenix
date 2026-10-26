#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Job control in the emulator: Ctrl-Z stops the foreground program, jobs lists it, ps shows it stopped, bg continues it,
# fg brings it back (and Ctrl-C then ends it with status 130), kill ends a stopped job; Ctrl-R searches the history;
# multi-line commands typed at the prompt (if, loops, case, functions, groups, | and backslash at the end of a line).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-jobs}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 130 tools/tests/jobs-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/run.log
pass=1
want() { if grep -aqF -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
want "^Z stops the foreground program" "Stopped    sleep"
grep -aqE "stopped +[0-9]+ +[0-9]+ sleep" $OUT/run.log && echo "PASS ps shows it stopped" || { echo "FAIL ps shows it stopped"; pass=0; }   # any size: programs shrink and grow
want "bg continues it" "sleep &"
want "the continued job runs on" "Running    sleep"
want "fg brings it back and ^C ends it (status 130)" "status=130"
want "a background job gets a number" "[1] "
want "kill ends a stopped job" "killed=0"
n=$(grep -ac '^alpha-one' $OUT/run.log); [ "$n" -ge 2 ] && echo "PASS Ctrl-R finds an older command and Enter runs it" || { echo "FAIL Ctrl-R (alpha-one seen $n times)"; pass=0; }
n=$(grep -ac '^beta-two' $OUT/run.log); [ "$n" -ge 2 ] && echo "PASS Ctrl-R again, then Enter" || { echo "FAIL Ctrl-R twice (beta-two seen $n times)"; pass=0; }
# multi-line commands at the prompt (PS2 "> " until the command is complete, as bash): each output line is one the
# typed text cannot show ($((...)) is worked out only when the command runs)
line() { if grep -aqx -- "$2" $OUT/run.log; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
line "if ... then ... fi typed over three lines runs" "if-2"
line "a while loop over three lines" "while-2"
line "a for loop over three lines (first item)" "for-aa"
line "... (second item)" "for-bb"
line "case ... esac over three lines" "case-9"
line "a function defined over three lines, then called" "fn-55"
line "a { } group over two lines" "grp-7"
line "a ( ) subshell over two lines" "sub-8"
line "a line ending in | goes on with the next" "PIPE-XYZ"
line "a line ending in a backslash joins the next" "backslash-10"
line "Ctrl-C at the PS2 prompt drops the unfinished command" "dropped-9"
line "a lone fi is a syntax error, not a PS2 prompt; the shell goes on" "after-1 ps2-count"
grep -aq "^sh: .*fi" $OUT/run.log && echo "PASS ... and it says so" || { echo "FAIL no syntax error for a lone fi"; pass=0; }
n=$(grep -ac '^> ' $OUT/run.log); [ "$n" = 15 ] && echo "PASS the PS2 prompt \"> \" is shown for each continued line (15)" || { echo "FAIL PS2 prompts: $n, want 15"; pass=0; }
exit $((1 - pass))
