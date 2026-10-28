#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# D21 step 7's PC-side tests (docs/d21-crypto-ssh-https.md 8, 10 step 7, 11.1): the TLS glue (crypto/zc/zc_tls.c),
# the time rules (crypto/zc/zc_clock.c), wget's core (user/wget/) over net/http/, and sntp's core (net/sntp/), on the
# PC only, no emulator, no root:
#   tools/host/tls-test.sh [PART...]   parts: units https sntp size (default: all of them); fuzz [SECONDS]: every
#                                      harness under libFuzzer, the time shared out (not in the default run)
# units   tools/host/tls-core-test.c with the sanitizers: wget's command line, names, plans and answers; the clock
#         file, the floor and the time callback; SNTP packets, hostile replies, the guard, /etc/sntp
# https   in unshare --map-root-user --map-auto -nm (only lo; nothing leaves the PC): tools/host/wget-client.c over
#         the cores against tools/host/tls-test.py's servers, a test CA made there at each run (keys in the work
#         folder, never in git): RSA, ECDSA, RSA-4096 and P-384 chains, SNI, expired, not yet valid, wrong name,
#         unknown root, missing intermediate, no serverAuth, a chain too long, a TLS 1.3-only server, the floor
#         without a trusted clock, the owner's own root, RSA chosen when a server has both, keep-alive, session
#         resumption counted, redirects (3 at most, never https to http, relative and //host), resume (a 200 after
#         -c refused), truncation, a slow server
# sntp    tools/host/sntp-client.c (the core over UDP) against tls-test.py's SNTP server with true, false and
#         hostile answers; the floor never moved by a lying one
# size    m68k -Os linked sizes (D21 7.1's way): the TLS client with the glue, wget's core with net/http/, sntp's
#         core; each through tools/check-68020-60.sh
# Needs a C compiler with AddressSanitizer, python3 and its cryptography module, openssl, unshare with --map-auto
# (newuidmap and /etc/subuid), clang (fuzz), the m68k cross compiler (size: TOOLPREFIX). A missing tool is a SKIPPED
# line, never a pass. Work files go to TLS_TEST_OUT (default build/tls-host).
cd "$(dirname "$0")/../.." || exit 1
grep() { if [ -x /usr/bin/grep ]; then /usr/bin/grep "$@"; else command grep "$@"; fi; }
OUT=${TLS_TEST_OUT:-build/tls-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra"
# BearSSL as Zenix builds it (crypto-test.sh's set), plus BR_USE_UNIX_TIME=0: x509_minimal never calls time() and
# loses its 64-bit division (D21 3.5, 8.3: the date comes from zc_clock's callback); and BR_RDRAND=0 on the PC: the
# 32 injected bytes are the only seed, as on a 68k (no RDRAND there), so a fixed seed replays a recorded handshake
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0"
CINC="-Icrypto/bearssl/inc -Icrypto/bearssl/src"
LIB_SRCS="$(ls crypto/zc/*.c) crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c $(find crypto/bearssl/src -name '*.c' | sort)"
CORE_SRCS="user/wget/plan.c net/http/http.c net/sntp/sntp.c"
fails=0
skips=""
say() { echo "tls-test: $*"; }
bad() { echo "FAIL tls-test: $*"; fails=$((fails + 1)); }
skip() { echo "SKIPPED tls-test: $*"; skips="$skips $1"; }

newer() {    # newer BIN SRC...: BIN exists and is newer than every SRC
    [ -x "$1" ] || return 1
    local b=$1; shift
    [ -z "$(find "$@" -newer "$b" -print -quit)" ]
}

build() {    # build NAME MAIN.c: a sanitized host program over the cores and the library
    local name=$1 main=$2
    newer "$OUT/$name" "$main" crypto user/wget net/http net/sntp tools/host/tls-test.sh && return 0
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -o "$OUT/$name" "$main" $CORE_SRCS $LIB_SRCS 2>"$OUT/$name.build.log" ||
        { tail -20 "$OUT/$name.build.log"; bad "$name does not build"; return 1; }
}

part_units() {
    build tls-core-test tools/host/tls-core-test.c || return
    if "$OUT/tls-core-test" >"$OUT/units.log" 2>&1; then say "units PASS: $(tail -1 "$OUT/units.log")"
    else grep -a '^FAIL\|ERROR\|runtime error' "$OUT/units.log" | head -30; bad "units (log $OUT/units.log)"; fi
}

ns_ok() {    # the namespace the network parts run in
    local t
    for t in openssl ip mount; do
        command -v "$t" >/dev/null || [ -x "/usr/bin/$t" ] || { skip "$1" "no $t on this PC"; return 1; }
    done
    python3 -c 'import cryptography' 2>/dev/null || { skip "$1" "Python's cryptography module is missing (it makes the test CA)"; return 1; }
    unshare --map-root-user --map-auto -nm true 2>/dev/null ||
        { skip "$1" "unshare --map-root-user --map-auto fails (newuidmap and an /etc/subuid range are needed)"; return 1; }
}

part_https() {
    ns_ok https || return
    build wget-client tools/host/wget-client.c || return
    if timeout 900 unshare --map-root-user --map-auto -nm sh -c 'ip link set lo up && exec python3 -I tools/host/tls-test.py https "$1" "$2"' sh \
            "$OUT" "$OUT/wget-client" >"$OUT/https.log" 2>&1; then
        say "https PASS: $(grep -ac '^PASS' "$OUT/https.log") checks against the test servers"
    else
        grep -a '^FAIL\|Traceback\|Error' "$OUT/https.log" | head -30
        bad "https (log $OUT/https.log)"
    fi
}

part_sntp() {
    ns_ok sntp || return
    build sntp-client tools/host/sntp-client.c || return
    if timeout 300 unshare --map-root-user --map-auto -nm sh -c 'ip link set lo up && exec python3 -I tools/host/tls-test.py sntp "$1" "$2"' sh \
            "$OUT" "$OUT/sntp-client" >"$OUT/sntp.log" 2>&1; then
        say "sntp PASS: $(grep -ac '^PASS' "$OUT/sntp.log") checks against the test server"
    else
        grep -a '^FAIL\|Traceback\|Error' "$OUT/sntp.log" | head -30
        bad "sntp (log $OUT/sntp.log)"
    fi
}

# libFuzzer, one harness at a time with a memory limit (this PC's rule, knowledge base claude-code/). The tls
# harness replays a server's recorded flight into the glue (the seed comes from part https's run with a fixed
# seed and clock: tls-test.py writes it with the anchors it needs)
FUZZ_TARGETS="tls url plan sntp"
part_fuzz() {
    local secs=$1 n=0 t each FCC=${FUZZCC:-clang} found=0
    local fl="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-sanitize=function"
    command -v "$FCC" >/dev/null || { skip fuzz "clang (libFuzzer) is missing"; return; }
    for t in $FUZZ_TARGETS; do n=$((n + 1)); done
    each=$(( (secs + n - 1) / n ))
    [ -f "$OUT/fuzz-seeds/tls/flight" ] || part_https >/dev/null
    [ -f "$OUT/fuzz-seeds/tls/flight" ] || { bad "fuzz: no recorded TLS flight (part https)"; return; }
    for t in $FUZZ_TARGETS; do
        local d="$OUT/fuzz/$t" T
        T=$(echo "$t" | tr a-z A-Z)
        mkdir -p "$d/corpus" "$d/artifacts" "$d/seeds"
        cp "$OUT/fuzz-seeds/$t/"* "$d/seeds/" 2>/dev/null
        # shellcheck disable=SC2086
        $FCC $HCFLAGS $fl $BRDEF $CINC -DFUZZ_$T -o "$d/fuzz" tools/host/tls-fuzz.c $CORE_SRCS $LIB_SRCS 2>"$d/build.log" ||
            { tail -5 "$d/build.log"; bad "fuzz $t does not build"; continue; }
        if [ $t = tls ]; then           # the recorded flight must replay into the HTTP answer, or the seed is useless
            TLS_FUZZ_SAY=1 TLS_FUZZ_ANCHORS="$OUT/fuzz-seeds/anchors" "$d/fuzz" "$d/seeds/flight" 2>&1 |
                grep -q 'round 0: error [0-9]*, status 200, body 1300 bytes' ||
                { bad "fuzz tls: the recorded flight does not replay to the HTTP answer"; continue; }
            say "fuzz tls: the recorded flight replays through the handshake to the HTTP answer"
        fi
        say "fuzz $t: $each s, one process, 2 GB memory limit (log $d/fuzz.log)"
        if TLS_FUZZ_ANCHORS="$OUT/fuzz-seeds/anchors" nice "$d/fuzz" -max_total_time="$each" -rss_limit_mb=2048 \
                -malloc_limit_mb=512 -max_len=32768 -timeout=20 -artifact_prefix="$d/artifacts/" -print_final_stats=1 \
                "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
            say "fuzz $t: $(grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges: nothing found"
        else
            grep -a 'ERROR\|SUMMARY\|Test unit written\|==ERROR' "$d/fuzz.log" | head -12
            bad "fuzz $t found something: $d/artifacts"
            found=1
        fi
    done
    [ $found = 0 ] && say "fuzz PASS: nothing found"
}

# m68k sizes: every source at -Os, one program per row linked with --gc-sections from tools/host/tls-size.c; the libz
# stand-ins (tools/host/crypto-size/stubs.c) counted apart
SIZE_ROWS="TLS_GLUE WGET_CORE WGET_TLS SNTP_CORE"
part_size() {
    local tp=${TOOLPREFIX:-}
    [ -z "$tp" ] && { if command -v m68k-elf-gcc >/dev/null; then tp=m68k-elf-; else tp=$HOME/opt/m68k-elf/bin/m68k-elf-; fi; }
    command -v "${tp}gcc" >/dev/null || { skip size "no m68k cross compiler (${tp}gcc; set TOOLPREFIX)"; return; }
    local d="$OUT/m68k-Os" f o objs row lib total stubs
    lib=$("${tp}gcc" -mcpu=68060 -msoft-float -print-libgcc-file-name)
    rm -rf "$d"; mkdir -p "$d"
    objs=""
    for f in $LIB_SRCS $CORE_SRCS; do
        o="$d/$(echo "$f" | tr / -).o"
        # shellcheck disable=SC2086
        "${tp}gcc" -m68020-60 -msoft-float -Os -ffunction-sections -fdata-sections -Wall $BRDEF $CINC \
            -c "$f" -o "$o" 2>>"$d/build.log" || { tail -5 "$d/build.log"; bad "m68k -Os: $f"; return; }
        objs="$objs $o"
    done
    "${tp}gcc" -m68020-60 -msoft-float -Os -ffunction-sections -c tools/host/crypto-size/stubs.c -o "$d/stubs1.o" &&
    "${tp}gcc" -m68020-60 -msoft-float -Os -ffunction-sections -DSTUBS -c tools/host/tls-size.c -o "$d/stubs2.o" &&
    "${tp}ld" -r -o "$d/stubs.o" "$d/stubs1.o" "$d/stubs2.o" || { bad "m68k stubs"; return; }
    stubs=$("${tp}size" "$d/stubs.o" | awk 'NR == 2 { print $1, $2 + $3 }')
    for row in $SIZE_ROWS; do
        # shellcheck disable=SC2086
        "${tp}gcc" -m68020-60 -msoft-float -Os -ffunction-sections -fdata-sections $BRDEF $CINC -DROW_$row \
            -c tools/host/tls-size.c -o "$d/entry-$row.o" &&
        "${tp}gcc" -m68020-60 -msoft-float -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-e,_start \
            -Wl,-Map,"$d/$row.map" -o "$d/$row.elf" "$d/stubs.o" "$d/entry-$row.o" $objs "$lib" 2>>"$d/build.log" ||
            { tail -5 "$d/build.log"; bad "m68k -Os: row $row does not link"; continue; }
        total=$("${tp}size" "$d/$row.elf" | awk 'NR == 2 { printf "%d %d", $1, $2 + $3 }')
        printf 'tls-test: size -Os %-10s code %6d, data+bss %d (the libz stand-ins not counted)\n' "$row" \
            $(( ${total% *} - ${stubs% *} )) $(( ${total#* } - ${stubs#* } ))
        TOOLPREFIX=$tp tools/check-68020-60.sh "$d/$row.elf" || bad "m68k: row $row has 64-bit multiplies or divides"
        if "${tp}nm" "$d/$row.elf" | grep -q '__divdi3\|__moddi3\|__udivdi3\| time$'; then
            bad "m68k: row $row links a 64-bit division or time()"
        fi
    done
    say "size PASS (code: text bytes of the linked program without the stand-ins; libgcc included)"
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(units https sntp size)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    units) part_units ;;
    https) part_https ;;
    sntp) part_sntp ;;
    size) part_size ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 600; fi ;;
    *) echo "tls-test: no part called $p"; exit 2 ;;
    esac
done
[ -n "$skips" ] && say "SKIPPED parts:$skips (not passed: install what their lines name)"
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
