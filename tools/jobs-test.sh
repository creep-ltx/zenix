#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Job control in the emulator: Ctrl-Z stops the foreground program, jobs lists it, ps shows it stopped, bg continues it,
# fg brings it back (and Ctrl-C then ends it with status 130), kill ends a stopped job; Ctrl-R searches the history;
# multi-line commands typed at the prompt (if, loops, case, functions, groups, | and backslash at the end of a line);
# long typed lines: kept whole up to the limits, refused with a message past them.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-jobs}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 260 tools/tests/jobs-test.txt 2>&1 | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/run.log
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
# long typed lines (audit 2026-10-26 F2): every character up to the limits, past them a message and nothing runs
line "a 477-character line at the prompt (the editor's 478) arrives whole" "fit-2 w000 w001 w002 w003 w004 w005 w006 w007 w008 w009 w010 w011 w012 w013 w014 w015 w016 w017 w018 w019 w020 w021 w022 w023 w024 w025 w026 w027 w028 w029 w030 w031 w032 w033 w034 w035 w036 w037 w038 w039 w040 w041 w042 w043 w044 w045 w046 w047 w048 w049 w050 w051 w052 w053 w054 w055 w056 w057 w058 w059 w060 w061 w062 w063 w064 w065 w066 w067 w068 w069 w070 w071 w072 w073 w074 w075 w076 w077 w078 w079 w080 w081 w082 w083 w084 w085 w086 w087 w088 w089 w090 w091"
grep -aq '^sh: the line was too long (more than 478 characters): not run' $OUT/run.log && ! grep -aq '^over-4' $OUT/run.log &&
    echo "PASS a 1000-character line at the prompt: the message, and nothing of it runs" || { echo "FAIL a 1000-character line at the prompt"; pass=0; }
line "... its status is 2" "st=2"
line "a 469-character line typed ahead while a command runs (the cooked tty) arrives whole" "ahead-6 x000 x001 x002 x003 x004 x005 x006 x007 x008 x009 x010 x011 x012 x013 x014 x015 x016 x017 x018 x019 x020 x021 x022 x023 x024 x025 x026 x027 x028 x029 x030 x031 x032 x033 x034 x035 x036 x037 x038 x039 x040 x041 x042 x043 x044 x045 x046 x047 x048 x049 x050 x051 x052 x053 x054 x055 x056 x057 x058 x059 x060 x061 x062 x063 x064 x065 x066 x067 x068 x069 x070 x071 x072 x073 x074 x075 x076 x077 x078 x079 x080 x081 x082 x083 x084 x085 x086 x087 x088 x089"
n=$(grep -ac '^tty: the line was too long (more than 511 characters): not taken' $OUT/run.log)
[ "$n" = 2 ] && ! grep -aq '^tail-8\|^cooked-10' $OUT/run.log && echo "PASS a typed-ahead line past 511 characters: the tty's message (cooked to the end, and cut by the switch to the editor), nothing of it runs" ||
    { echo "FAIL typed-ahead lines past 511 characters ($n messages)"; pass=0; }
line "... no tail of it runs: the next command does" "after-tail-3"
line "... the next command after a refused cooked line" "after-cooked-4"
exit $((1 - pass))
