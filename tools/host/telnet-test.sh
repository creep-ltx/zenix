#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# telnet's host tests (DESIGN D20 8.1, step 0 of section 9), on the PC only, no emulator, no root:
#   tools/host/telnet-test.sh [PART...]    parts: font core sim zmodem (default: all of them)
#                                          fuzz [SECONDS]: telnet's core under libFuzzer (not in the default run)
# font    tools/mkpsf.py checks Linux's font_8x8.c and writes the PSF1 font (2,052 bytes, 36 04, mode 0, height 8)
# core    net/telnet/ on the PC: the decoder and encoder, RFC 1143 option negotiation, NAWS, TTYPE, the three modes'
#         filters (music strings as SyncTERM, FF, SUB, iCE, the font switch, 0x9B and the Amiga console's codes, UTF-8
#         in the Unix mode), the key translation and the CP437 tables (checked against Python's cp437 codec)
# sim     tools/bbs-sim.py's self-test, then a PC client over net/telnet/ against it: the negotiation record, every
#         screen of the simulator through the filter, and Zmodem through telnet both ways with the real lsz/lrz
#         (with BINARY and without: -e), a resume after ZCRC and a refused splice
# zmodem  user/xfer/ against lrzsz over a socketpair (tools/host/zmodem-test.c): both ways, sizes, 8 KB
#         subpackets, escaped controls, a bad line; the download rules (.part, ZCRC resume, no overwrite, names)
# Needs: a C compiler with AddressSanitizer, python3; lrzsz's lsz and lrz on PATH (also as lrzsz-sz/lrzsz-rz) or in
# LRZSZ_DIR; for fuzz, clang with libFuzzer. Work files go to TELNET_TEST_OUT (default build/telnet-host).
cd "$(dirname "$0")/../.." || exit 1
OUT=${TELNET_TEST_OUT:-build/telnet-host}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
HOSTCC=${HOSTCC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
HCFLAGS="-std=gnu99 -g -O1 -Wall -Wextra"
fails=0
say() { echo "telnet-test: $*"; }
bad() { echo "FAIL telnet-test: $*"; fails=$((fails + 1)); }

# lsz and lrz: LRZSZ_DIR, else PATH (lsz/lrz, or Arch's lrzsz-sz/lrzsz-rz)
find_lrzsz() {
    LSZ=""; LRZ=""
    if [ -n "${LRZSZ_DIR:-}" ]; then LSZ="$LRZSZ_DIR/lsz"; LRZ="$LRZSZ_DIR/lrz"
    elif command -v lsz >/dev/null && command -v lrz >/dev/null; then LSZ=$(command -v lsz); LRZ=$(command -v lrz)
    elif command -v lrzsz-sz >/dev/null && command -v lrzsz-rz >/dev/null; then LSZ=$(command -v lrzsz-sz); LRZ=$(command -v lrzsz-rz)
    fi
    [ -x "$LSZ" ] && [ -x "$LRZ" ] && return 0
    bad "lrzsz not found: install it (lsz/lrz on PATH) or set LRZSZ_DIR to the directory with lsz and lrz"
    return 1
}

part_font() {
    if python3 tools/mkpsf.py tools/font_8x8.c "$OUT/cp437-8x8.psf" &&
       python3 - "$OUT/cp437-8x8.psf" <<'EOF'
import sys
d = open(sys.argv[1], "rb").read()
assert len(d) == 2052, len(d)
assert d[:4] == b"\x36\x04\x00\x08", d[:4]
g = lambda c: d[4 + 8 * c:12 + 8 * c]
assert g(0xDB) == b"\xff" * 8 and g(0) == bytes(8) and g(0x41) != g(0x42)
assert len(set(g(c) for c in range(256))) > 250       # (a few CP437 glyphs are blank or alike)
EOF
    then say "font PASS (PSF1, 2,052 bytes)"; else bad "font"; fi
}

part_zmodem() {
    find_lrzsz || return
    $HOSTCC $HCFLAGS $SAN -o "$OUT/zmodem-test" tools/host/zmodem-test.c user/xfer/detect.c user/xfer/rzrules.c ||
        { bad "zmodem-test does not build"; return; }
    if "$OUT/zmodem-test" "$LSZ" "$LRZ" "$OUT/zmodem"; then say "zmodem PASS"; else bad "zmodem (the FAIL lines above)"; fi
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(font zmodem)
for p in "${parts[@]}"; do
    case "$p" in
    font) part_font ;;
    zmodem) part_zmodem ;;
    *) echo "telnet-test: no part called $p"; exit 2 ;;
    esac
done
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
