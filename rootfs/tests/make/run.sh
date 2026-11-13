# SPDX-License-Identifier: GPL-2.0-only
# Runs make on the Makefiles beside this script (DESIGN D52 section 4, tools/make-test.sh and tools/make-host-test.sh):
#   sh run.sh SRCDIR WORKDIR RELMAKE
# SRCDIR holds the *.mk files, WORKDIR is made fresh and used as the working directory, RELMAKE is a path to make
# relative to WORKDIR with a slash in it (one run starts make by it: make makes its own name absolute). make is
# otherwise found in PATH. Everything goes to standard output; the same output on the PC and on Zenix is the test.
# A file made in the same second as the file it was made from is out of date (make rebuilds when the target is not
# newer): after a build that the next step looks at, and after files are written that a build is to be newer than,
# settle waits two seconds of the wall clock (wait2.sh).
# Plain sh only (Zenix's sh and the PC's).
S=$1; W=$2; M=$3
rm -rf "$W"; mkdir -p "$W" || exit 1
cd "$W" || exit 1
cp "$S/wait2.sh" wait2.sh      # the recipes of deps.mk and suffix.mk run it
unset MAKEFLAGS MAKE PDPMAKE_POSIXLY_CORRECT PDPMAKE_PRAGMAS CMD FROMENV

# run NAME ARGS...: make with the arguments, its output (both streams), then its status
run() { echo "-- make $*"; make "$@" 2>&1; echo "status $?"; }
settle() { sh "$S/wait2.sh"; }
use() { cp "$S/$1" Makefile; }
# files in the working directory, with their contents, sorted by name
state() { for f in "$@"; do if [ -f "$f" ]; then echo "$f: $(cat "$f" | tr '\n' ' ')"; else echo "$f: missing"; fi; done; }

echo "== deps: build, nothing to do, a header newer, a source newer"
use deps.mk
echo "A" > a.c; echo "B" > b.c; echo "H" > common.h
settle
run
settle
run
settle; echo "H2" > common.h
run
state prog
settle
echo "A2" > a.c
settle
run
state prog
settle
run -n
run -q; run -q all

echo "== target newer than its prerequisite is not rebuilt"
use touch.mk
echo "s1" > src
settle; echo "t1" > tgt
run tgt
state tgt
settle; echo "s2" > src
run tgt
state tgt

echo "== touch: -t makes the target current without running anything"
settle; echo "s3" > src
settle
run -t tgt
run tgt
run -n tgt
rm -f tgt
run -t tgt
state tgt
run -t nosuchsource

echo "== suffix rules, chained"
use suffix.mk
echo "hi" > hello.txt
settle
run hello.len
state hello.txt hello.up hello.len
settle
run hello.len
echo "ho" > hello.txt
settle
run hello.len
state hello.up hello.len
settle
run -r hello.len

echo "== macros"
use macros.mk
run
run NAME=you CMD=cli
echo "-- FROMENV=env1 make"; FROMENV=env1 make 2>&1; echo "status $?"
echo "-- FROMENV=env1 NAME=envname make"; FROMENV=env1 NAME=envname make 2>&1; echo "status $?"
echo "-- NAME=envname make -e"; NAME=envname make -e 2>&1; echo "status $?"
run -s SRC="x.c y.c"

echo "== internal macros"
use internal.mk
echo "1" > in1; echo "2" > in2
settle
run
state out
settle
echo "3" > in2
settle
run
settle
echo "4" > in1
settle
run
settle
run -n

echo "== failing commands"
use fail.mk
run
run -k
run -i
run ign
run -s ign
run loud
run -n loud
run -q ok1
run ok2 ok1

echo "== errors"
use errors.mk
run
run -k
run nosuch
use circular.mk
run
use syntax.mk
run
rm -f Makefile
run
echo "== misc: include, phony, default, double colon, a recursive make"
use misc.mk
cp "$S/inc.mk" inc.mk; cp "$S/inc2.mk" inc2.mk
run clean
run dc
run whatever
run sub
run -n sub
run INC=cmdline clean
"$M" -f Makefile clean 2>&1; echo "status $? (make started by a path with a slash)"
make -C . -f Makefile clean 2>&1; echo "status $? (-C)"

echo "== a chain of 100 targets"
cp "$S/deep.mk" deep.mk
run -f deep.mk

echo "== 100 macros, each defined by the next"
cp "$S/deepmacro.mk" deepmacro.mk
run -f deepmacro.mk

echo "== end"
