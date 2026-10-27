#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The crypto library's and ssh's host tests (DESIGN D21, docs/d21-crypto-ssh-https.md 10 steps 0-1, 11.1), on the
# PC only, no emulator, no root:
#   tools/host/crypto-test.sh [PART...]   parts: vectors upstream wycheproof openssl ssh sign anchors size
#                                         (default: all of them); fuzz [SECONDS]: every parser under libFuzzer, the
#                                         time shared out (not in the default run); long: RFC 7748's million
# vectors     tools/host/crypto-test.c: RFC 7748 (both X25519s, the 1,000 iterations, the all-zero results refused),
#             RFC 8032, RFC 8439, RFC 4231, FIPS 180-4 and 197, the random generator, ssh's mpint, key derivation and
#             packet framing, the .sig format
# upstream    the imported files unchanged against the pinned upstream commits (BearSSL 7bea48e, Monocypher f5f0a19,
#             OpenSSH V_10_6_P1), then BearSSL's own test_crypto for the implementations D21 chose
# wycheproof  Wycheproof's vectors (tools/host/crypto-test.py), fetched once into WYCHEPROOF_DIR, never committed
# openssl     differential runs against the PC's OpenSSL (Python's cryptography module)
# ssh         net/ssh/ against a real OpenSSH sshd in a user and network namespace (tools/host/ssh-test.py)
# sign        tools/zsign.py with OpenSSL and the library both ways
# anchors     tools/mkanchors.py on tools/certdata.txt, the anchors file and the X.509 wrapper against a test CA
# size        m68k -Os and -O2 linked sizes of the library's sets and of net/ssh/ (D21 7.1)
# A missing tool or download is a SKIPPED line, never a pass. Needs a C compiler with AddressSanitizer, python3 and
# its cryptography module, openssl, OpenSSH's sshd/ssh-keygen (ssh, sign), unshare with --map-auto (newuidmap and
# /etc/subuid; ssh), git and network access the first time (upstream, wycheproof: CRYPTO_CACHE), clang (fuzz), the
# m68k cross compiler (size: TOOLPREFIX). Work files go to CRYPTO_TEST_OUT (default build/crypto-host).
cd "$(dirname "$0")/../.." || exit 1
OUT=${CRYPTO_TEST_OUT:-build/crypto-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
CACHE=${CRYPTO_CACHE:-$OUT/cache}
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra"
# BearSSL as Zenix builds it: no unaligned word access (the m68k's rule, and UBSan's), no system random source (the
# library injects 32 bytes from /dev/random, D21 5)
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0"
CINC="-Icrypto/bearssl/inc -Icrypto/bearssl/src"
BEARSSL_SRCS=$(find crypto/bearssl/src -name '*.c' | sort)
MONO_SRCS="crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c"
ZC_SRCS=$(ls crypto/zc/*.c)
BCRYPT_SRCS="crypto/openssh/blowfish.c crypto/openssh/bcrypt_pbkdf.c"
SSH_SRCS=$(ls net/ssh/*.c 2>/dev/null)
LIB_SRCS="$ZC_SRCS $MONO_SRCS $BEARSSL_SRCS"
BEARSSL_COMMIT=7bea48e
MONOCYPHER_COMMIT=f5f0a19
OPENSSH_TAG=V_10_6_P1
WYCHEPROOF_COMMIT=d17cc89fa5c8f4f4d378b5385ebf75da5d0926c9
fails=0
skips=""
say() { echo "crypto-test: $*"; }
bad() { echo "FAIL crypto-test: $*"; fails=$((fails + 1)); }
skip() { echo "SKIPPED crypto-test: $*"; skips="$skips $1"; }

# the library for the PC, with the sanitizers (one object set, reused by every part)
build_test() {
    [ -x "$OUT/crypto-test" ] && [ "$OUT/crypto-test" -nt tools/host/crypto-test.c ] && [ -z "$(find crypto tools/host/crypto-vectors.h -newer "$OUT/crypto-test" -print -quit)" ] && return 0
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -o "$OUT/crypto-test" tools/host/crypto-test.c $LIB_SRCS ||
        { bad "crypto-test does not build"; return 1; }
}

part_vectors() {
    build_test || return
    if "$OUT/crypto-test" "${1:-vectors}" >"$OUT/vectors.log" 2>&1; then say "vectors PASS: $(tail -1 "$OUT/vectors.log")"
    else grep -a '^FAIL\|ERROR\|runtime error' "$OUT/vectors.log" | head -30; bad "vectors (log $OUT/vectors.log)"; fi
    # zc_x25519() and zc_x25519_public() as they are when D21 step 2 picks Monocypher's X25519
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -DZC_X25519_MONOCYPHER -o "$OUT/crypto-test-mono" tools/host/crypto-test.c $LIB_SRCS ||
        { bad "crypto-test (ZC_X25519_MONOCYPHER) does not build"; return; }
    if "$OUT/crypto-test-mono" vectors >"$OUT/vectors-mono.log" 2>&1; then say "vectors PASS with ZC_X25519_MONOCYPHER"
    else grep -a '^FAIL' "$OUT/vectors-mono.log" | head; bad "vectors with ZC_X25519_MONOCYPHER (log $OUT/vectors-mono.log)"; fi
}

# fetch_git DIR URL REV: a clone of URL at REV in DIR (once); 1 when it cannot be had
fetch_git() {
    local d=$1 url=$2 rev=$3
    if [ ! -d "$d/.git" ]; then
        rm -rf "$d"; mkdir -p "$(dirname "$d")"
        git clone -q --filter=blob:none "$url" "$d" 2>/dev/null || { rm -rf "$d"; return 1; }
    fi
    git -C "$d" -c advice.detachedHead=false checkout -q "$rev" 2>/dev/null ||
        { git -C "$d" fetch -q origin 2>/dev/null && git -C "$d" -c advice.detachedHead=false checkout -q "$rev"; }
}

part_upstream() {
    local B=${BEARSSL_SRC:-$CACHE/bearssl} M=${MONOCYPHER_SRC:-$CACHE/monocypher} O=${OPENSSH_SRC:-$CACHE/openssh} f n=0 diffs=0
    if ! fetch_git "$B" https://www.bearssl.org/git/BearSSL $BEARSSL_COMMIT; then skip upstream "BearSSL $BEARSSL_COMMIT cannot be fetched (set BEARSSL_SRC to a checkout)"; return; fi
    if ! fetch_git "$M" https://github.com/LoupVaillant/Monocypher $MONOCYPHER_COMMIT; then skip upstream "Monocypher $MONOCYPHER_COMMIT cannot be fetched (MONOCYPHER_SRC)"; return; fi
    if ! fetch_git "$O" https://github.com/openssh/openssh-portable $OPENSSH_TAG; then skip upstream "OpenSSH $OPENSSH_TAG cannot be fetched (OPENSSH_SRC)"; return; fi
    for f in $(cd crypto/bearssl && find inc src -type f | sort) LICENSE:LICENSE.txt; do
        n=$((n + 1)); cmp -s "crypto/bearssl/${f%%:*}" "$B/${f#*:}" || { bad "crypto/bearssl/${f%%:*} differs from BearSSL $BEARSSL_COMMIT"; diffs=$((diffs + 1)); }
    done
    for f in monocypher.c:src/monocypher.c monocypher.h:src/monocypher.h monocypher-ed25519.c:src/optional/monocypher-ed25519.c \
             monocypher-ed25519.h:src/optional/monocypher-ed25519.h LICENCE:LICENCE.md AUTHORS:AUTHORS.md; do
        n=$((n + 1)); cmp -s "crypto/monocypher/${f%%:*}" "$M/${f#*:}" || { bad "crypto/monocypher/${f%%:*} differs from Monocypher $MONOCYPHER_COMMIT"; diffs=$((diffs + 1)); }
    done
    for f in blowfish.c bcrypt_pbkdf.c blf.h; do
        n=$((n + 1)); cmp -s "crypto/openssh/$f" "$O/openbsd-compat/$f" || { bad "crypto/openssh/$f differs from OpenSSH $OPENSSH_TAG"; diffs=$((diffs + 1)); }
    done
    [ $diffs = 0 ] && say "upstream: $n imported files identical to BearSSL $BEARSSL_COMMIT, Monocypher $MONOCYPHER_COMMIT, OpenSSH $OPENSSH_TAG"
    # BearSSL's test_crypto, built from the upstream tree (the files above are identical to it), for the chosen
    # implementations and the hashes, HMAC, the DRBG and the TLS PRF
    $HOSTCC -std=gnu99 -O1 -w $BRDEF -I"$B/inc" -I"$B/src" -o "$OUT/test_crypto" "$B/test/test_crypto.c" \
        $(find "$B/src" -name '*.c' | sort) || { bad "BearSSL's test_crypto does not build"; return; }
    local t tests="SHA1 SHA224 SHA256 SHA384 SHA512 multihash HMAC HMAC_DRBG PRF AES_ct AES_CTRCBC_ct ChaCha20_ct Poly1305_ctmul32 GHASH_ctmul32 GCM RSA_i15 EC_prime_i15 EC_p256_m15 EC_c25519_m15 ECDSA_i15"
    # shellcheck disable=SC2086
    if "$OUT/test_crypto" $tests >"$OUT/test_crypto.log" 2>&1; then say "upstream: BearSSL's test_crypto PASS ($tests)"
    else tail -5 "$OUT/test_crypto.log"; bad "BearSSL's test_crypto (log $OUT/test_crypto.log)"; return; fi
    [ $diffs = 0 ] && say "upstream PASS"
}

part_wycheproof() {
    local W=${WYCHEPROOF_DIR:-$CACHE/wycheproof} f
    command -v curl >/dev/null || { skip wycheproof "no curl to fetch Wycheproof's vectors (or set WYCHEPROOF_DIR)"; return; }
    mkdir -p "$W"
    for f in x25519_test ed25519_test chacha20_poly1305_test aes_gcm_test ecdsa_secp256r1_sha256_test \
             ecdsa_secp256r1_sha256_p1363_test ecdsa_secp384r1_sha384_test rsa_signature_2048_sha256_test \
             rsa_signature_4096_sha512_test hmac_sha256_test; do
        [ -s "$W/$f.json" ] && continue
        curl -sSf -o "$W/$f.json.part" \
            "https://raw.githubusercontent.com/C2SP/wycheproof/$WYCHEPROOF_COMMIT/testvectors_v1/$f.json" &&
            mv "$W/$f.json.part" "$W/$f.json" || { rm -f "$W/$f.json.part"; skip wycheproof "Wycheproof's $f.json cannot be fetched"; return; }
    done
    build_test || return
    if python3 -I tools/host/crypto-test.py "$OUT/crypto-test" wycheproof "$W"; then say "wycheproof PASS"
    else bad "wycheproof (the FAIL lines above)"; fi
}

part_openssl() {
    build_test || return
    python3 -I tools/host/crypto-test.py "$OUT/crypto-test" openssl "${CRYPTO_DIFF_COUNT:-200}"
    case $? in
    0) say "openssl PASS" ;;
    3) skip openssl "Python's cryptography module is missing (it is the PC's OpenSSL here)" ;;
    *) bad "openssl (the FAIL lines above)" ;;
    esac
}

# net/ssh/: its parsers and refusals alone, then the PC client over it against a real OpenSSH sshd in a user, mount
# and network namespace (tools/host/ssh-test.py; --map-auto maps /etc/subuid's range, which sshd's privilege
# separation needs: setgroups is refused in a namespace that maps one uid only)
SSHC_SRCS="$SSH_SRCS $BCRYPT_SRCS"
part_ssh() {
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -Icrypto/openssh -o "$OUT/ssh-core-test" tools/host/ssh-core-test.c $SSHC_SRCS $LIB_SRCS &&
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -Icrypto/openssh -o "$OUT/ssh-client" tools/host/ssh-client.c $SSHC_SRCS $LIB_SRCS ||
        { bad "the ssh tests do not build"; return; }
    if "$OUT/ssh-core-test" >"$OUT/ssh-core.log" 2>&1; then say "ssh: $(tail -1 "$OUT/ssh-core.log")"
    else grep -a '^FAIL\|ERROR\|runtime error' "$OUT/ssh-core.log" | head -20; bad "ssh-core-test (log $OUT/ssh-core.log)"; return; fi
    local t
    for t in sshd ssh-keygen openssl ip mount; do
        command -v "$t" >/dev/null || [ -x "/usr/bin/$t" ] || { skip ssh "no $t on this PC for the sshd tests"; return; }
    done
    unshare --map-root-user --map-auto -nm true 2>/dev/null ||
        { skip ssh "unshare --map-root-user --map-auto fails (newuidmap and an /etc/subuid range are needed)"; return; }
    if unshare --map-root-user --map-auto -nm python3 -I tools/host/ssh-test.py "$OUT/ssh-client" "$OUT" >"$OUT/ssh-test.log" 2>&1; then
        while IFS= read -r l; do skip ssh-part "${l#SKIPPED ssh-test: }"; done < <(grep -a '^SKIPPED' "$OUT/ssh-test.log")
        say "ssh PASS: $(grep -ac '^PASS' "$OUT/ssh-test.log") checks against OpenSSH's sshd ($(/usr/bin/sshd -V 2>&1 | head -1))"
    else grep -a '^FAIL\|Traceback\|Error' "$OUT/ssh-test.log" | head -20; bad "ssh against sshd (log $OUT/ssh-test.log, sshd's in $OUT/ssh/)"; fi
}

part_sign() {
    command -v openssl >/dev/null || { skip sign "no openssl (tools/zsign.py signs with it)"; return; }
    build_test || return
    if python3 -I tools/host/crypto-test.py "$OUT/crypto-test" sign "$OUT"; then say "sign PASS"
    else bad "sign (the FAIL lines above)"; fi
}

part_anchors() {
    python3 -c 'import cryptography' 2>/dev/null || { skip anchors "Python's cryptography module is missing (it makes the test CA)"; return; }
    build_test || return
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN $BRDEF $CINC -o "$OUT/x509-test" tools/host/x509-test.c $LIB_SRCS || { bad "x509-test does not build"; return; }
    if python3 -I tools/host/crypto-test.py "$OUT/crypto-test" anchors "$OUT/x509-test" "$OUT"; then say "anchors PASS"
    else bad "anchors (the FAIL lines above)"; fi
}

# libFuzzer, one harness at a time with a memory limit (this PC's rule: knowledge base claude-code/): every parser of
# tools/host/crypto-fuzz.c, SECONDS shared out evenly; the seeds come from crypto-test.py (real keys, files, chains)
FUZZ_TARGETS="sshkex sshpkt sshopen keys kh config clean sig anchors x509 tls"
part_fuzz() {
    # (-fno-sanitize=function: BearSSL calls its vtables' functions through pointers whose context argument has
    # another struct type, as its object style does everywhere; clang's check flags each call, gcc has no such check)
    local secs=$1 n=0 t each FCC=${FUZZCC:-clang}
    local fl="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -fno-sanitize=function"
    local fd="$OUT/fuzz" found=0
    command -v "$FCC" >/dev/null || { skip fuzz "clang (libFuzzer) is missing"; return; }
    build_test || return
    for t in $FUZZ_TARGETS; do n=$((n + 1)); done
    each=$(( (secs + n - 1) / n ))
    mkdir -p "$fd"
    python3 -I tools/mkanchors.py "$fd/mozilla.anchors" >/dev/null || { bad "fuzz: mkanchors"; return; }
    [ -f "$OUT/chain.seed" ] || part_anchors >/dev/null
    python3 -I tools/host/crypto-test.py "$OUT/crypto-test" seeds "$fd" "$fd/mozilla.anchors" || { bad "fuzz: the seeds"; return; }
    [ -f "$OUT/chain.seed" ] && mkdir -p "$fd/x509/seeds" && cp "$OUT/chain.seed" "$fd/x509/seeds/chain"
    for t in $FUZZ_TARGETS; do
        local d="$fd/$t" T
        T=$(echo "$t" | tr a-z A-Z)
        mkdir -p "$d/corpus" "$d/artifacts" "$d/seeds"
        # shellcheck disable=SC2086
        $FCC $HCFLAGS $fl $BRDEF $CINC -Icrypto/openssh -DFUZZ_$T -DSSH_KEY_ROUNDS_MAX=2 -o "$d/fuzz" \
            tools/host/crypto-fuzz.c $SSHC_SRCS $LIB_SRCS 2>"$d/build.log" || { cat "$d/build.log" | tail -5; bad "fuzz $t does not build"; continue; }
        say "fuzz $t: $each s, one process, 2 GB memory limit (log $d/fuzz.log)"
        if CRYPTO_FUZZ_ANCHORS="$fd/mozilla.anchors" nice "$d/fuzz" -max_total_time="$each" -rss_limit_mb=2048 \
                -malloc_limit_mb=512 -max_len=16384 -timeout=20 -artifact_prefix="$d/artifacts/" -print_final_stats=1 \
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

# m68k sizes (D21 7.1): every source compiled for -m68020-60 at -Os and at -O2, then one program per row linked with
# --gc-sections from tools/host/crypto-size/entry.c; the libz stand-ins (stubs.c) are counted apart; each program
# also goes through tools/check-68020-60.sh (no 64-bit multiply or divide: the 68060 has none)
SIZE_ROWS="X25519_MONO X25519_BEAR ED25519_VERIFY SIG SSH_PRIMS SSH_STEP3 SSH TLS TLS_X509"
part_size() {
    local tp=${TOOLPREFIX:-}
    [ -z "$tp" ] && { if command -v m68k-elf-gcc >/dev/null; then tp=m68k-elf-; else tp=$HOME/opt/m68k-elf/bin/m68k-elf-; fi; }
    command -v "${tp}gcc" >/dev/null || { skip size "no m68k cross compiler (${tp}gcc; set TOOLPREFIX)"; return; }
    local opt d f o objs row lib total stubs sshtext
    # libgcc from the 68060 multilib, as the Makefile takes it (the default one uses the 64-bit mulu.l/divu.l itself)
    lib=$("${tp}gcc" -mcpu=68060 -msoft-float -print-libgcc-file-name)
    for opt in Os O2; do
        d="$OUT/m68k-$opt"
        rm -rf "$d"; mkdir -p "$d"
        objs=""
        for f in $LIB_SRCS $SSHC_SRCS; do
            o="$d/$(echo "$f" | tr / -).o"
            # shellcheck disable=SC2086
            "${tp}gcc" -m68020-60 -msoft-float -$opt -ffunction-sections -fdata-sections -Wall $BRDEF $CINC \
                -Icrypto/openssh -c "$f" -o "$o" 2>>"$d/build.log" || { tail -5 "$d/build.log"; bad "m68k -$opt: $f"; return; }
            objs="$objs $o"
        done
        "${tp}gcc" -m68020-60 -msoft-float -$opt -ffunction-sections -c tools/host/crypto-size/stubs.c -o "$d/stubs.o" || { bad "m68k stubs"; return; }
        stubs=$("${tp}size" "$d/stubs.o" | awk 'NR == 2 { print $1, $2 + $3 }')
        for row in $SIZE_ROWS; do
            # shellcheck disable=SC2086
            "${tp}gcc" -m68020-60 -msoft-float -$opt -ffunction-sections -fdata-sections $BRDEF $CINC -DROW_$row \
                -c tools/host/crypto-size/entry.c -o "$d/entry-$row.o" &&
            "${tp}gcc" -m68020-60 -msoft-float -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-e,_start \
                -Wl,-Map,"$d/$row.map" -o "$d/$row.elf" "$d/stubs.o" "$d/entry-$row.o" $objs "$lib" 2>>"$d/build.log" ||
                { tail -5 "$d/build.log"; bad "m68k -$opt: row $row does not link"; continue; }
            total=$("${tp}size" "$d/$row.elf" | awk 'NR == 2 { printf "%d %d", $1, $2 + $3 }')
            # net/ssh/'s share: its .text input sections in the memory map (not the discarded list before it)
            sshtext=$(awk '/^Linker script and memory map/ { on = 1 } !on { next }
                           /^ \.text/ && NF == 4 && $4 ~ /net-ssh-/ { s += strtonum($3) } /^ \.text/ && NF == 1 { sec = 1; next }
                           sec && NF == 3 && $3 ~ /net-ssh-/ { s += strtonum($2) } { sec = 0 } END { print s + 0 }' "$d/$row.map")
            printf 'crypto-test: size -%s %-15s code %6d%s, data+bss %d (the libz stand-ins not counted)\n' "$opt" "$row" \
                $(( ${total% *} - ${stubs% *} )) "$([ "$sshtext" -gt 0 ] && echo " (net/ssh/ $sshtext)")" $(( ${total#* } - ${stubs#* } ))
            TOOLPREFIX=$tp tools/check-68020-60.sh "$d/$row.elf" || bad "m68k -$opt: row $row has 64-bit multiplies or divides"
        done
    done
    say "size PASS (code: text bytes of the linked program without the stand-ins; libgcc included)"
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(vectors upstream wycheproof openssl ssh sign anchors size)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    vectors) part_vectors ;;
    long) part_vectors long ;;
    upstream) part_upstream ;;
    wycheproof) part_wycheproof ;;
    openssl) part_openssl ;;
    ssh) part_ssh ;;
    sign) part_sign ;;
    anchors) part_anchors ;;
    size) part_size ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 660; fi ;;
    *) echo "crypto-test: no part called $p"; exit 2 ;;
    esac
done
[ -n "$skips" ] && say "SKIPPED parts:$skips (not passed: install what their lines name)"
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
