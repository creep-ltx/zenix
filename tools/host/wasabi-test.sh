#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# wasabid's core on the PC (DESIGN D24 12.1, step 0 of section 14), no emulator, no root:
#   tools/host/wasabi-test.sh [PART...]   parts: core check size (default: all of them)
#                                         fuzz [SECONDS]: the whole core under libFuzzer (not in the default run)
# core    net/wasabi/pure.c (tools/host/wasabi-core-test.c): /etc/wasabid.conf and its refusals, the address filter,
#         the frame receiver, the payload fields, AmigaDOS's bits and DateStamps, PUT's modes, the guard's names,
#         ERR's codes (decision 2), the formatter
# check   net/wasabi/ built with AddressSanitizer over Linux's calls (tools/host/wasabi-host.c) and driven over TCP by
#         the conformance client tools/host/wasabi-check.py: every command Zenix offers, the wire format byte for
#         byte, wrong keys and the pause, short and oversized frames, unknown tags, the 10 s rule, the fifth
#         connection, the address filter, discovery, the configuration's refusals, RESTART/QUIT/REBOOT and init's
#         statuses, the console commands against pretend consoles; then, when ZENIX_WASABI names a checkout of the
#         owner's Wasabi (the repository names no path of this PC), its own `wasabi` client against the core
# size    net/wasabi/ for m68k at -Os and -O2: the code, the 64-bit multiply check (tools/check-68020-60.sh), no
#         libgcc helper
# Needs: a C compiler with AddressSanitizer, python3; for fuzz, clang with libFuzzer. Work files go to
# WASABI_TEST_OUT (default build/wasabi-host). user/regex.c (TEXT's patterns) is built from a copy beside a small
# libz.h of the C library's, in the work folder.
cd "$(dirname "$0")/../.." || exit 1
OUT=${WASABI_TEST_OUT:-build/wasabi-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra -Werror"
WSB_SRC="net/wasabi/core.c net/wasabi/files.c net/wasabi/info.c net/wasabi/pure.c net/wasabi/run.c net/wasabi/screen.c"
fails=0
say() { echo "wasabi-test: $*"; }
bad() { echo "FAIL wasabi-test: $*"; fails=$((fails + 1)); }

shim() {
    mkdir -p "$OUT/shim"
    cp user/regex.c user/regex.h "$OUT/shim/"
    printf '#include <stdlib.h>\n#include <string.h>\n' >"$OUT/shim/libz.h"
}

part_core() {
    $HOSTCC $HCFLAGS $SAN -o "$OUT/wasabi-core-test" tools/host/wasabi-core-test.c net/wasabi/pure.c ||
        { bad "wasabi-core-test does not build"; return; }
    local r
    r=$("$OUT/wasabi-core-test")
    echo "$r" | /usr/bin/grep '^FAIL'
    if echo "$r" | /usr/bin/grep -q '^wasabi-core-test: [0-9]* checks, 0 failed$'; then say "core PASS (${r##*: })"
    else bad "core"; fi
}

part_check() {
    shim
    $HOSTCC -std=gnu99 -g -O1 $SAN -c "$OUT/shim/regex.c" -o "$OUT/shim/regex.o" &&
        $HOSTCC $HCFLAGS $SAN -o "$OUT/wasabi-host" tools/host/wasabi-host.c $WSB_SRC "$OUT/shim/regex.o" ||
        { bad "wasabi-host does not build"; return; }
    local off=$((20000 + $$ % 30000)) r
    rm -rf "$OUT/check"
    # (SIGINT and SIGQUIT ignored around the client and the daemon, as in a suite started in the background from a
    # script: the commands the daemon runs must get them back, B2-16; every run checks it this way)
    r=$(trap '' INT QUIT; ASAN_OPTIONS=detect_leaks=1 timeout 600 python3 -I tools/host/wasabi-check.py "$OUT/wasabi-host" \
        "$OUT/check" "$off" "${ZENIX_WASABI:-}" 2>&1)
    echo "$r" | /usr/bin/grep -v '^wasabi-check: [a-z]* PASS$'
    if /usr/bin/grep -l 'runtime error\|AddressSanitizer\|LeakSanitizer' "$OUT"/check/daemon-*.log 2>/dev/null; then
        bad "check: a sanitizer report in the daemon's log (the files above)"
    elif echo "$r" | /usr/bin/grep -q '^wasabi-check: [0-9]* checks, 0 failed$'; then
        say "check PASS ($(echo "$r" | tail -1 | sed 's/^wasabi-check: //'))"
    else bad "check (the FAIL lines above; the daemons' logs in $OUT/check)"; fi
}

part_size() {
    local tp=${TOOLPREFIX:-}
    [ -z "$tp" ] && { if command -v m68k-elf-gcc >/dev/null; then tp=m68k-elf-; else tp=$HOME/opt/m68k-elf/bin/m68k-elf-; fi; }
    command -v "${tp}gcc" >/dev/null || { say "size skipped: no m68k cross compiler (${tp}gcc)"; return; }
    local d="$OUT/m68k" f o objs helpers
    for o in Os O2; do
        rm -rf "$d"; mkdir -p "$d"
        objs=""
        for f in $WSB_SRC; do
            "${tp}gcc" -m68020-60 -$o -msoft-float -ffreestanding -fno-builtin -nostdlib -fno-strict-aliasing -fno-common \
                -ffunction-sections -fdata-sections -Wall -Wextra -Werror -c "$f" -o "$d/$(basename "$f" .c).o" ||
                { bad "m68k: $f"; return; }
            objs="$objs $d/$(basename "$f" .c).o"
        done
        "${tp}size" -t $objs >"$d/size.txt"
        TOOLPREFIX=$tp tools/check-68020-60.sh $objs || { bad "m68k: 64-bit multiply or divide instructions"; return; }
        helpers=$("${tp}nm" $objs | awk '$1 == "U" && $2 ~ /^__/ { print $2 }' | sort -u | tr '\n' ' ')
        [ -n "$helpers" ] && { bad "m68k: libgcc helpers: $helpers"; return; }
        say "size: net/wasabi/ for m68k at -$o, unlinked (text data bss): $(tail -1 "$d/size.txt" | awk '{ print $1, $2, $3 }'); libgcc helpers: none"
    done
    say "size PASS"
}

part_fuzz() {
    local secs=$1 d="$OUT/fuzz" FCC=${FUZZCC:-clang}
    command -v "$FCC" >/dev/null || { bad "fuzz needs clang (libFuzzer)"; return; }
    shim
    mkdir -p "$d/corpus" "$d/seeds" "$d/artifacts"
    $FCC -std=gnu99 -g -O1 -DSEEDS -Wno-macro-redefined -o "$d/wasabi-fuzz-seeds" tools/host/wasabi-fuzz.c $WSB_SRC \
        "$OUT/shim/regex.c" && "$d/wasabi-fuzz-seeds" "$d/seeds" || { bad "the fuzzer's seeds"; return; }
    $FCC -std=gnu99 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -Wno-macro-redefined \
        -o "$d/wasabi-fuzz" tools/host/wasabi-fuzz.c $WSB_SRC "$OUT/shim/regex.c" || { bad "wasabi-fuzz does not build"; return; }
    say "fuzz: $secs s, one process, 2 GB memory limit (log $d/fuzz.log)"
    if nice "$d/wasabi-fuzz" -max_total_time="$secs" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=8192 -timeout=10 \
            -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
        say "fuzz: $(/usr/bin/grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(/usr/bin/grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges covered: nothing found"
        say "fuzz PASS"
    else
        /usr/bin/grep -a 'ERROR\|SUMMARY\|Test unit written\|abort' "$d/fuzz.log" | head -20
        bad "fuzz found something: $d/artifacts"
    fi
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(core check size)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    core|check|size) "part_$p" ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 60; fi ;;
    *) echo "wasabi-test: no part called $p"; exit 2 ;;
    esac
done
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
