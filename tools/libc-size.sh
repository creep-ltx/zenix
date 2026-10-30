#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The C library's size table (DESIGN D27 section 10), with the real port: the test programs of tools/tests/libc-size
# built as ZEXE files ($BUILD/lcs), each against its budget. The ZEXE file's bytes (what the disk holds and the
# loader reads); data + BSS from the ELF (the memory every process of it takes besides its code). A program that
# links both Zenix's buffered output (oputs) and stdout's FILE functions gets a warning (two buffers mix their order).
# The budgets are section 10's as confirmed with the real port at build step 1 (2026-10-29: the ones with a stream or
# malloc raised by what libz's allocator and the lazily taken stream buffers cost over the design's stand-in).
#   tools/libc-size.sh        the table and the checks (FAIL on a budget over)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
SIZE=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}size
NM=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}nm
progs="raw hellofd hellooputs hello sort time float utf8 both"
make -s BUILD=$BD $(for p in $progs; do echo $BD/lcs/$p.zx; done) >/dev/null 2>&1 || { echo "FAIL the size table's programs do not build"; exit 1; }
pass=1
declare -A Z D
printf '%-11s %7s %11s  %s\n' program "file" "data+bss" "what it uses"
for p in $progs; do
    Z[$p]=$(wc -c < $BD/lcs/$p.zx)
    D[$p]=$($SIZE $BD/lcs/$p.celf | awk 'NR == 2 { print $2 + $3 }')
    printf '%-11s %7d %11d  %s\n' $p ${Z[$p]} ${D[$p]} "$(sed -n 2p tools/tests/libc-size/$p.c | sed 's#^/\* the size table: ##; s# \*/$##')"
done
budget() {      # budget WHAT BYTES LIMIT
    if [ "$2" -le "$3" ]; then echo "PASS $1: $2 bytes (budget $3)"; else echo "FAIL $1: $2 bytes, over its budget of $3"; pass=0; fi
}
budget "a raw program (read/write only)" ${Z[raw]} 600
budget "hello without streams (dprintf)" ${Z[hellofd]} 2800
budget "hello without streams (oputs)" ${Z[hellooputs]} 2800
# (2026-10-30, the allocator of docs/d27-c-library.md "The allocator": about 0.7 KB more of malloc's code in each)
budget "hello with printf" ${Z[hello]} 8100   # +380 B: audit B3-11 (the allocator keeps its quick lists after growing) and the environment fix (2026-10-30)
budget "a stdio + malloc + qsort tool" ${Z[sort]} 11700
budget "the time functions, over hello" $((Z[time] - Z[hello])) 14300
budget "floating point, over hello" $((Z[float] - Z[hello])) 16300
budget "the UTF-8 functions, over hello" $((Z[utf8] - Z[hello])) 6700   # D28: setlocale reads the environment
budget "the library's data + BSS per process (hello with printf)" ${D[hello]} 1024
warned=""
for p in $progs; do
    if $NM $BD/lcs/$p.celf | grep -q ' T oputn$' && $NM $BD/lcs/$p.celf | grep -q ' first_put$'; then
        echo "WARN $p links both oputs and stdout's FILE functions (two buffers mix their order)"; warned="$warned $p"
    fi
done
[ "$warned" = " both" ] && echo "PASS the warning names exactly the program that links both" || { echo "FAIL the both-buffers warning: '$warned'"; pass=0; }
[ $pass = 1 ]
