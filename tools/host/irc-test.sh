#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# irc's host tests (DESIGN D58 section 8), on the PC only, no emulator, no root, nothing beyond 127.0.0.1:
#   tools/host/irc-test.sh [PART...]     parts: units sessions size (default: all of them)
# units     tools/host/irc-core-test.c with the sanitizers: the text made clean (ISO-8859-1, UTF-8, controls, mIRC
#           codes, never an escape), RFC 1459's case, base64, the configuration's errors, the login and SASL's
#           lines, the autojoin, a long typed line in pieces, the scrollback's ring, highlights, queries
# sessions  tools/host/irc-client.c (the core over sockets, with the crypto library's TLS) against
#           tools/host/irc-fake.py: tools/host/irc-test.py's plain, SASL and TLS sessions (a test CA made at this run,
#           its keys in the work folder only)
# size      the m68k program and its TLS helper, as built (build/irc.zx, build/tlspipe.zx)
# Needs a C compiler with AddressSanitizer, python3 and its cryptography module. Work files go to IRC_TEST_OUT
# (default build/irc-host). A missing tool is a SKIPPED line, never a pass.
cd "$(dirname "$0")/../.." || exit 1
OUT=${IRC_TEST_OUT:-build/irc-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra -DIRC_HOST"
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0"
fails=0
bad() { echo "FAIL irc-test: $*"; fails=$((fails + 1)); }

part_units() {
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN -o "$OUT/irc-core-test" tools/host/irc-core-test.c net/irc/irc.c 2>"$OUT/core.build.log" ||
        { tail -20 "$OUT/core.build.log"; bad "irc-core-test does not build"; return; }
    "$OUT/irc-core-test" || bad "units"
}

part_sessions() {
    python3 -c 'import cryptography' 2>/dev/null || { echo "SKIPPED irc-test: sessions (Python's cryptography module)"; return; }
    local lib
    lib="$(ls crypto/zc/*.c) $(find crypto/bearssl/src -name '*.c' | sort)"
    if [ ! -x "$OUT/irc-client" ] || [ -n "$(find tools/host/irc-client.c net/irc crypto -newer "$OUT/irc-client" -print -quit)" ]; then
        # shellcheck disable=SC2086
        $HOSTCC $HCFLAGS $SAN $BRDEF -Icrypto/bearssl/inc -Icrypto/bearssl/src -o "$OUT/irc-client" \
            tools/host/irc-client.c net/irc/irc.c $lib crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c \
            2>"$OUT/client.build.log" || { tail -20 "$OUT/client.build.log"; bad "irc-client does not build"; return; }
    fi
    rm -f "$OUT"/*.log.txt "$OUT"/plain.log "$OUT"/sasl.log "$OUT"/tls.log "$OUT"/tls2.log
    timeout 600 python3 -I tools/host/irc-test.py "$OUT" "$OUT/irc-client" || bad "sessions"
}

part_size() {
    local b=${ZENIX_BUILD:-build}
    if [ -f "$b/irc.zx" ] && [ -f "$b/tlspipe.zx" ]; then
        echo "irc-test: size: irc $(stat -c %s "$b/irc.zx") bytes, /lib/irc/tlspipe $(stat -c %s "$b/tlspipe.zx") bytes (ZEXE files)"
        [ "$(stat -c %s "$b/irc.zx")" -le 61440 ] || bad "irc is over 60 KB"
    else
        echo "SKIPPED irc-test: size (make first)"
    fi
}

parts=${*:-units sessions size}
for p in $parts; do "part_$p"; done
[ $fails = 0 ] && echo "irc-test: all parts passed" || { echo "irc-test: $fails part(s) failed"; exit 1; }
