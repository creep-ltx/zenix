#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZINE's step-1 tests (docs/d29-zine.md sections 15 and 16 step 1), on the PC only:
#   tools/host/zine-test.sh [PART...]   parts: core tables m68k (default: all three)
#                                       fuzz [SECONDS]: the three harnesses under libFuzzer, the time shared out (not
#                                       in the default run; 300 s when no number follows)
# core    user/zine/ (the hunk check and loader, the volume, the memory planner, the offset table) with the sanitizers:
#         tools/host/zine-core-test.c, every refusal's message, every prefix and one-byte change of a test program
# tables  tools/zine-tables.py check: lvo.c/lvo.h are what the tool makes from the RKRM's Function Offset Reference, and
#         every offset in amigaofs.h is its Structure Reference's; a wrong offset is caught. Needs ZENIX_MANUALS (the
#         folder of the manuals as text: the owner's private notes say where); without it: SKIPPED, never a pass
# m68k    each core built for Zenix (the Makefile's flags, -O2 and -Os), no instruction a 68060 lacks, the sizes
# Needs a C compiler with AddressSanitizer, python3; TOOLPREFIX's m68k-elf-gcc (m68k); clang with libFuzzer (fuzz).
# Work files go to ZINE_TEST_OUT (default build/zine-host).
cd "$(dirname "$0")/../.." || exit 1
OUT=${ZINE_TEST_OUT:-build/zine-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SRCS="user/zine/hunk.c user/zine/zvol.c user/zine/zplan.c user/zine/lvo.c"
if [ -z "$TOOLPREFIX" ]; then
    if command -v m68k-elf-gcc >/dev/null; then TOOLPREFIX=m68k-elf-; else TOOLPREFIX=$HOME/opt/m68k-elf/bin/m68k-elf-; fi
fi
fails=0
say() { echo "zine-test: $*"; }
bad() { echo "FAIL zine-test: $*"; fails=$((fails + 1)); }

part_core() {
    # shellcheck disable=SC2086
    if ! $HOSTCC -std=gnu99 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
            -fno-omit-frame-pointer -o "$OUT/zine-core-test" tools/host/zine-core-test.c $SRCS 2>"$OUT/core-build.log"; then
        tail -20 "$OUT/core-build.log"; bad "zine-core-test does not build"; return
    fi
    if "$OUT/zine-core-test" >"$OUT/core.log" 2>&1; then echo "PASS zine-test: core: $(tail -1 "$OUT/core.log" | sed 's/^PASS zine-core: //')"
    else grep -a '^FAIL\|ERROR\|runtime error' "$OUT/core.log" | head -20; bad "core (log $OUT/core.log)"; fi
}

part_tables() {
    if [ -z "$ZENIX_MANUALS" ] || [ ! -d "$ZENIX_MANUALS" ]; then
        echo "SKIPPED zine-test: tables (ZENIX_MANUALS does not name the manuals' text folder)"; return
    fi
    if python3 -I tools/zine-tables.py check "$ZENIX_MANUALS" >"$OUT/tables.log" 2>&1; then
        sed 's/^PASS /PASS zine-test: /' "$OUT/tables.log"
    else cat "$OUT/tables.log"; bad "tables"; return; fi
    # the check must catch a wrong number: three offsets moved by 2
    sed -e 's/^\(#define ZO_ExecBase_ChkBase\) 38/\1 40/' -e 's/^\(#define ZO_Process_pr_CLI\) 172/\1 174/' \
        -e 's/^\(#define ZO_FileLock_fl_Volume\) 16/\1 18/' user/zine/amigaofs.h >"$OUT/bad-ofs.h"
    n=$(python3 -I tools/zine-tables.py check "$ZENIX_MANUALS" "$OUT/bad-ofs.h" 2>&1 | grep -c '^FAIL amigaofs.h')
    [ "$n" = 3 ] && echo "PASS zine-test: three wrong offsets are caught" || bad "the check caught $n of 3 wrong offsets"
}

part_m68k() {
    command -v "${TOOLPREFIX}gcc" >/dev/null || { bad "${TOOLPREFIX}gcc not found (TOOLPREFIX)"; return; }
    local fl="-m68020-60 -msoft-float -ffreestanding -fno-builtin -nostdlib -g -Wall -Wextra -Werror -fno-strict-aliasing -fno-common"
    local o line=""
    for opt in O2 Os; do
        local tot=0
        for f in $SRCS; do
            o="$OUT/$(basename "$f" .c)-$opt.o"
            # shellcheck disable=SC2086
            "${TOOLPREFIX}gcc" $fl -$opt -c "$f" -o "$o" 2>>"$OUT/m68k.log" || { bad "$f does not build for Zenix (-$opt)"; continue; }
            TOOLPREFIX=$TOOLPREFIX tools/check-68020-60.sh "$o" || bad "$f has instructions a 68060 lacks"
            # no call into a C library: the cores stand alone
            und=$("${TOOLPREFIX}nm" -u "$o" | awk '{ print $2 }' | tr '\n' ' ')
            [ -z "$und" ] || bad "$f calls outside itself: $und"
            s=$("${TOOLPREFIX}size" "$o" | awk 'NR == 2 { print $1 + $2 }')
            tot=$((tot + s)); line="$line $(basename "$f" .c) $s"
        done
        echo "PASS zine-test: m68k -$opt: bytes (code + data):$line, together $tot"
        line=""
    done
}

part_fuzz() {
    local secs=${1:-300} FCC=${FUZZCC:-clang} found=0
    command -v "$FCC" >/dev/null || { echo "SKIPPED zine-test: fuzz (no clang)"; return; }
    local each=$((secs / 3))
    for t in hunk vol plan; do
        local T d="$OUT/fuzz-$t"
        T=$(echo "$t" | tr a-z A-Z)
        mkdir -p "$d/corpus" "$d/seeds" "$d/artifacts"
        # shellcheck disable=SC2086
        $FCC -std=gnu99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -DFUZZ_$T \
            -o "$d/fuzz" tools/host/zine-fuzz.c $SRCS 2>"$d/build.log" || { tail -5 "$d/build.log"; bad "fuzz $t does not build"; found=1; continue; }
        [ "$t" = hunk ] && [ -x "$OUT/zine-core-test" ] && "$OUT/zine-core-test" seeds "$d/seeds" >/dev/null 2>&1
        if nice "$d/fuzz" -max_total_time="$each" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=65536 -timeout=20 \
                -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
            say "fuzz $t: $(grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs, $(grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges: nothing found"
        else
            grep -a 'ERROR\|SUMMARY\|Test unit written' "$d/fuzz.log" | head -8
            bad "fuzz $t found something: $d/artifacts"; found=1
        fi
    done
    [ $found = 0 ] && say "fuzz PASS: nothing found"
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(core tables m68k)
for ((i = 0; i < ${#parts[@]}; i++)); do
    case "${parts[i]}" in
    core) part_core ;;
    tables) part_tables ;;
    m68k) part_m68k ;;
    fuzz) if [[ "${parts[i + 1]}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz; fi ;;
    *) echo "zine-test: no part called ${parts[i]}"; exit 2 ;;
    esac
done
[ $fails = 0 ] && say "PASS" || { say "$fails failed"; exit 1; }
