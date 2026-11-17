#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# mail's host tests (DESIGN D59 section 8), on the PC only, no emulator, no root, nothing beyond 127.0.0.1:
#   tools/host/mail-test.sh [PART...]     parts: units sessions size (default: all of them)
# units     tools/host/mail-core-test.c with the sanitizers: the encodings, charsets, RFC 2047 words, dates, MIME trees
#           (also broken and too deep ones), HTML made text, attachments' names, escape sequences never through, the
#           settings' errors, addresses, the message written, replies and forwards, modified UTF-7
# sessions  tools/host/mail-client.c (the core over sockets, with the crypto library's TLS) against
#           tools/host/mail-fake.py: tools/host/mail-test.py's IMAP, hostile servers, POP3 and SMTP sessions (a test
#           CA made at this run, its keys in the work folder only)
# size      the m68k program as built (build/mail.zx; its TLS helper is irc's, build/tlspipe.zx)
# Needs a C compiler with AddressSanitizer, python3 and its cryptography module. Work files go to MAIL_TEST_OUT
# (default build/mail-host). A missing tool is a SKIPPED line, never a pass.
cd "$(dirname "$0")/../.." || exit 1
OUT=${MAIL_TEST_OUT:-build/mail-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra -DMAIL_HOST"
BRDEF="-DBR_LE_UNALIGNED=0 -DBR_BE_UNALIGNED=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 -DBR_USE_UNIX_TIME=0 -DBR_RDRAND=0"
fails=0
bad() { echo "FAIL mail-test: $*"; fails=$((fails + 1)); }

part_units() {
    # shellcheck disable=SC2086
    $HOSTCC $HCFLAGS $SAN -o "$OUT/mail-core-test" tools/host/mail-core-test.c net/mail/*.c 2>"$OUT/core.build.log" ||
        { tail -20 "$OUT/core.build.log"; bad "mail-core-test does not build"; return; }
    "$OUT/mail-core-test" || bad "units"
}

part_sessions() {
    python3 -c 'import cryptography' 2>/dev/null || { echo "SKIPPED mail-test: sessions (Python's cryptography module)"; return; }
    local lib
    lib="$(ls crypto/zc/*.c) $(find crypto/bearssl/src -name '*.c' | sort)"
    if [ ! -x "$OUT/mail-client" ] || [ -n "$(find tools/host/mail-client.c net/mail crypto -newer "$OUT/mail-client" -print -quit)" ]; then
        # shellcheck disable=SC2086
        $HOSTCC $HCFLAGS $SAN $BRDEF -Icrypto/bearssl/inc -Icrypto/bearssl/src -o "$OUT/mail-client" \
            tools/host/mail-client.c net/mail/*.c $lib crypto/monocypher/monocypher.c crypto/monocypher/monocypher-ed25519.c \
            2>"$OUT/client.build.log" || { tail -20 "$OUT/client.build.log"; bad "mail-client does not build"; return; }
    fi
    rm -f "$OUT"/servers.log "$OUT"/*.out
    timeout 600 python3 -I tools/host/mail-test.py "$OUT" "$OUT/mail-client" || bad "sessions"
}

part_size() {
    local b=${ZENIX_BUILD:-build}
    if [ -f "$b/mail.zx" ]; then
        echo "mail-test: size: mail $(stat -c %s "$b/mail.zx") bytes (a ZEXE file; /lib/tls/tlspipe is the package tls)"
        [ "$(stat -c %s "$b/mail.zx")" -le 92160 ] || bad "mail is over 90 KB"
    else
        echo "SKIPPED mail-test: size (make first)"
    fi
}

parts=${*:-units sessions size}
for p in $parts; do "part_$p"; done
[ $fails = 0 ] && echo "mail-test: all parts passed" || { echo "mail-test: $fails part(s) failed"; exit 1; }
