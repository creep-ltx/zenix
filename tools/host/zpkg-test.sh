#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zpkg's PC-side tests (DESIGN D22 8.1, steps 2 and 3 of section 9), on the PC only, no emulator, no root:
#   tools/host/zpkg-test.sh [PART...]   parts: core repo differential slot net (default: all of them)
#                                       fuzz [SECONDS]: every parser under libFuzzer, the time shared out (not in
#                                       the default run; 300 s when no number follows)
# core          user/zpkg/ (the list, latest, the trust rules, the resolver) and net/http/ (URLs, requests, the
#               response parser, chunked bodies, resume) built with the sanitizers: tools/host/zpkg-core-test.c
# repo          tools/mkrepo.py and mkzup.py's D22 additions on synthetic builds (tools/host/zpkg-test.py repo): the
#               version rules of 3.4, serials, latest, promote, snapshot, prune, check finding damage, installdb,
#               the test key refused for release channels, two runs byte for byte; every list read by the C parser
# differential  the C list parser against mkrepo's Python one on 3,000 damaged lists
# slot          the update slot's read-only FFS walk and in-place write on tools/mkffs.py images, DOS\1 and DOS\3,
#               every refusal of 4.5 on a damaged copy
# net           in unshare -rnm (a user, mount and network namespace with only lo): tools/host/zpkg-client.c over
#               the cores against tools/host/http-test-server.py: keep-alive, chunked and huge chunks, Range
#               honoured, ignored and wrong, bodies longer, shorter and endless, a slow drip, a stalled server, a
#               header over 4 KB, redirect loops, a cut then a resume; -Sy and -Sw with the TEST key's signatures
#               (zc_sig_verify): mirrors behind or damaged, replays, wrong keys and classes, expiry, the clock
# The TEST key pair is made by tools/zsign.py testkey in the work folder, never in the repository. Needs a C
# compiler with AddressSanitizer, python3, openssl; unshare and ip (net); clang with libFuzzer (fuzz). A missing
# tool is a SKIPPED line, never a pass. Work files go to ZPKG_TEST_OUT (default build/zpkg-host).
cd "$(dirname "$0")/../.." || exit 1
OUT=${ZPKG_TEST_OUT:-build/zpkg-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra -Werror"
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0"
CINC="-Icrypto/bearssl/inc -Icrypto/bearssl/src"
CORE_SRCS="user/zpkg/index.c user/zpkg/resolve.c user/zpkg/ffsslot.c net/http/http.c"
SIG_SRCS="user/sha256.c $(ls crypto/zc/*.c) crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c $(find crypto/bearssl/src -name '*.c' | sort)"
fails=0
skips=""
say() { echo "zpkg-test: $*"; }
bad() { echo "FAIL zpkg-test: $*"; fails=$((fails + 1)); }
skip() { echo "SKIPPED zpkg-test: $*"; skips="$skips $1"; }

build_core() {
    [ -x "$OUT/zpkg-core-test" ] && [ -z "$(find user/zpkg net/http tools/host/zpkg-core-test.c -newer "$OUT/zpkg-core-test" -print -quit)" ] && return 0
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN -o "$OUT/zpkg-core-test" tools/host/zpkg-core-test.c $CORE_SRCS || { bad "zpkg-core-test does not build"; return 1; }
}

build_client() {
    [ -x "$OUT/zpkg-client" ] && [ -z "$(find user/zpkg net/http crypto user/sha256.c tools/host/zpkg-client.c -newer "$OUT/zpkg-client" -print -quit)" ] && return 0
    # shellcheck disable=SC2086
    $HOSTCC -std=gnu99 -g -O1 -Wall -Wextra $SAN $BRDEF $CINC -o "$OUT/zpkg-client" tools/host/zpkg-client.c $CORE_SRCS $SIG_SRCS 2>"$OUT/client-build.log" ||
        { tail -20 "$OUT/client-build.log"; bad "zpkg-client does not build"; return 1; }
}

part_core() {
    build_core || return
    if "$OUT/zpkg-core-test" units >"$OUT/core.log" 2>&1; then say "core PASS: $(tail -1 "$OUT/core.log")"
    else grep -a '^FAIL\|ERROR\|runtime error' "$OUT/core.log" | head -30; bad "core (log $OUT/core.log)"; fi
}

py_part() {    # py_part NAME ARGS...: tools/host/zpkg-test.py NAME ARGS, its FAIL lines shown
    local name=$1
    shift
    if python3 -I tools/host/zpkg-test.py "$name" "$@" >"$OUT/$name.log" 2>&1; then
        say "$name PASS: $(grep -ac '^PASS' "$OUT/$name.log") checks"
    else
        grep -a '^FAIL\|Traceback\|Error' "$OUT/$name.log" | head -20
        bad "$name (log $OUT/$name.log)"
    fi
}

part_repo() {
    command -v openssl >/dev/null || { skip repo "no openssl (tools/zsign.py signs with it)"; return; }
    build_core || return
    py_part repo "$OUT" "$OUT/zpkg-core-test"
}

part_differential() {
    build_core || return
    [ -f "$OUT/repo/test/latest" ] || part_repo >/dev/null
    py_part differential "$OUT" "$OUT/zpkg-core-test" "${ZPKG_DIFF_COUNT:-3000}"
}

part_slot() {
    build_core || return
    py_part slot "$OUT" "$OUT/zpkg-core-test"
}

part_net() {
    local t
    for t in unshare ip openssl; do
        command -v "$t" >/dev/null || { skip net "no $t on this PC"; return; }
    done
    unshare -rnm true 2>/dev/null || { skip net "unshare -rnm fails (user namespaces are off)"; return; }
    build_client || return
    [ -f "$OUT/repo/test/latest" ] || part_repo >/dev/null
    if timeout 600 unshare -rnm sh -c 'ip link set lo up && exec python3 -I tools/host/zpkg-test.py net "$1" "$2"' sh \
            "$OUT" "$OUT/zpkg-client" >"$OUT/net.log" 2>&1; then
        say "net PASS: $(grep -ac '^PASS' "$OUT/net.log") checks against the test server"
    else
        grep -a '^FAIL\|Traceback\|Error' "$OUT/net.log" | head -20
        bad "net (log $OUT/net.log)"
    fi
}

# libFuzzer, one harness at a time with a memory limit (this PC's rule, knowledge base claude-code/)
FUZZ_TARGETS="index latest http ffs resolve"
part_fuzz() {
    local secs=$1 n=0 t each FCC=${FUZZCC:-clang} found=0
    local fl="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
    command -v "$FCC" >/dev/null || { skip fuzz "clang (libFuzzer) is missing"; return; }
    for t in $FUZZ_TARGETS; do n=$((n + 1)); done
    each=$(( (secs + n - 1) / n ))
    [ -f "$OUT/repo/test/latest" ] || part_repo >/dev/null
    [ -f "$OUT/slot/boot-DOS1.img" ] || part_slot >/dev/null
    for t in $FUZZ_TARGETS; do
        local d="$OUT/fuzz/$t" T
        T=$(echo "$t" | tr a-z A-Z)
        mkdir -p "$d/corpus" "$d/artifacts" "$d/seeds"
        case $t in
        index|resolve) cp "$OUT"/repo/test/index-* "$OUT"/release-repo/*/index-* "$d/seeds/" 2>/dev/null; rm -f "$d"/seeds/*.sig ;;
        latest) cp "$OUT/repo/test/latest" "$d/seeds/" 2>/dev/null ;;
        http) printf '\x01\x00HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello' >"$d/seeds/len"
              printf '\x01\x00HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;x\r\nhello\r\n0\r\nT: 1\r\n\r\n' >"$d/seeds/chunked"
              printf '\x7f\xffHTTP/1.1 206 P\r\nContent-Range: bytes 100-199/200\r\nContent-Length: 100\r\n\r\n' >"$d/seeds/range"
              printf '\x00\x10HTTP/1.0 302 F\r\nLocation: /x\r\nConnection: keep-alive\r\n\r\n' >"$d/seeds/redir"
              printf 'http://127.0.0.1:8088/zenix/testing/latest' >"$d/seeds/url" ;;
        ffs) head -c 20480 /dev/zero >"$d/s20k"
             head -c 40960 /dev/zero >"$d/s40k"
             python3 -I tools/mkffs.py "$d/seeds/one.img" 64K BOOT "$d/s20k=Zenix/zenix-update.slot" >/dev/null
             python3 -I tools/mkffs.py "$d/seeds/two.img" 128K BOOT --intl "tools/mkffs.py=S/x" \
                 "$d/s40k=Zenix/zenix-update.slot" "tools/zsign.py=Zenix/f3" >/dev/null ;;
        esac
        # shellcheck disable=SC2086
        $FCC -std=gnu99 -g -O1 $fl -DFUZZ_$T -o "$d/fuzz" tools/host/zpkg-fuzz.c $CORE_SRCS 2>"$d/build.log" ||
            { tail -5 "$d/build.log"; bad "fuzz $t does not build"; continue; }
        say "fuzz $t: $each s, one process, 2 GB memory limit (log $d/fuzz.log)"
        if nice "$d/fuzz" -max_total_time="$each" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=262144 -timeout=20 \
                -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
            say "fuzz $t: $(grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges: nothing found"
        else
            grep -a 'ERROR\|SUMMARY\|Test unit written\|==ERROR' "$d/fuzz.log" | head -12
            bad "fuzz $t found something: $d/artifacts"
            found=1
        fi
    done
    [ $found = 0 ] && say "fuzz PASS: nothing found"
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(core repo differential slot net)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    core) part_core ;;
    repo) part_repo ;;
    differential) part_differential ;;
    slot) part_slot ;;
    net) part_net ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 300; fi ;;
    *) echo "zpkg-test: no part called $p"; exit 2 ;;
    esac
done
[ -n "$skips" ] && say "SKIPPED parts:$skips (not passed: install what their lines name)"
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
