#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# make on the PC (DESIGN D52 section 4, ports/pdpmake): the pinned pdpmake source built for the PC (it needs no flag
# of Zenix's: the Makefile builds it as it is) under AddressSanitizer and UBSan, then
#  - upstream's own test suite (ports/pdpmake/upstream/testsuite, run from a copy: it makes files) must have no failure;
#  - rootfs/tests/make/run.sh runs make over its Makefiles (dependencies, timestamps, suffix rules, macros, the
#    internal macros, -n -k -i -q -s -t -e, failing commands and their status, errors, include, a recursive make)
#    and checks the facts that matter. Its output is the reference tools/make-test.sh compares Zenix's output with,
#    line for line: MAKE_EXPECTED=FILE keeps a copy of it.
# Leak detection is off: upstream frees nothing at its end on purpose (ENABLE_FEATURE_CLEAN_UP is 0).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-make.XXXXXX")
trap 'rm -rf "$T"' EXIT
fail=0
check() { if [ "$1" = 0 ]; then echo "PASS $2"; else echo "FAIL $2${3:+: $3}"; fail=1; fi; }
mkdir -p "$T/bin" "$T/work"
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/bin/make" \
    ports/pdpmake/upstream/*.c 2>"$T/cc.log"
check $? "the PC build of ports/pdpmake/upstream" "$(head -3 "$T/cc.log")"
[ $fail = 0 ] || exit 1
export ASAN_OPTIONS=detect_leaks=0
# upstream's suite: it looks for ../make, so a copy of the suite beside a copy of the binary
mkdir -p "$T/up"
cp -r ports/pdpmake/upstream/testsuite "$T/up/testsuite"
cp "$T/bin/make" "$T/up/make"
(cd "$T/up/testsuite" && ./runtest) >"$T/up.txt" 2>&1
npass=$(grep -c '^PASS' "$T/up.txt"); nfail=$(grep -c '^FAIL' "$T/up.txt")
nskip=$(grep -c "^SKIPPED" "$T/up.txt")
[ "$nfail" = 0 ] && [ $((npass + nskip)) = "$(grep -c "^testing " ports/pdpmake/upstream/testsuite/make.tests)" ]
check $? "upstream's test suite: $npass passed, $nskip skipped (POSIX 2017 only), $nfail failed" "$(grep '^FAIL' "$T/up.txt" | head -3)"
# our Makefiles
(cd "$T" && PATH="$T/bin:$PATH" sh "$OLDPWD/rootfs/tests/make/run.sh" "$OLDPWD/rootfs/tests/make" "$T/work" ../bin/make) \
    >"$T/out.txt" 2>"$T/err.txt"
check $? "run.sh ran to its end" "$(tail -3 "$T/err.txt")"
[ "$(tail -1 "$T/out.txt")" = "== end" ]; check $? "run.sh printed its last line" "$(tail -1 "$T/out.txt")"
[ ! -s "$T/err.txt" ]; check $? "nothing on standard error (ASan and UBSan say nothing)" "$(head -3 "$T/err.txt")"
grep -q "ERROR: AddressSanitizer\|runtime error" "$T/out.txt"; [ $? = 1 ]; check $? "no sanitizer report in the output"
# what must be there (run.sh's own words; the status line follows each make)
o=$T/out.txt
has() { grep -qxF -- "$1" "$o"; }
has "make: 'tgt' is up to date" && has "make: nothing to be done for all"; check $? "a target that is current is not rebuilt"
[ "$(grep -c '^cc a$' "$o")" = 3 ] && [ "$(grep -c '^cc b$' "$o")" = 2 ]; check $? "a newer header rebuilds both, a newer source its own"
has "prog: A2 B " ; check $? "the program rebuilt from the newer source"
has "make: (Makefile:7): failed to build 'bad1' exit 3"; check $? "a failing command: its target and its exit status"
has "make: 'all' not built due to errors"; check $? "-k goes on and says what it could not build"
has "make: don't know how to make nosuch"; check $? "a target with no rule and no file"
has "make: circular dependency for a"; check $? "a circular dependency"
has "touch tgt"; check $? "-t touches the target"
has "hello.len: hi hi hi hi "; check $? "a chained suffix rule"
has "[world] [world again]"; check $? ":=, = and += macros"
has "obj/a.o obj/b.o obj/c.o"; check $? "a pattern replacement in a macro"
has "sub make: from-top "; check $? "a recursive make with a macro from the command line"
has "bottom of the chain"; check $? "a chain of 100 targets"
[ -n "$MAKE_EXPECTED" ] && cp "$T/out.txt" "$MAKE_EXPECTED"
exit $fail
