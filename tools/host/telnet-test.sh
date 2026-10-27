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

part_core() {
    $HOSTCC $HCFLAGS $SAN -o "$OUT/telnet-core-test" tools/host/telnet-core-test.c net/telnet/*.c ||
        { bad "telnet-core-test does not build"; return; }
    if "$OUT/telnet-core-test" "$OUT/cp437-tables.bin" && python3 - "$OUT/cp437-tables.bin" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
fb, keys = d[:256], d[256:]
bad = []
for b in range(256):                     # Latin-1 -> CP437 for keys: exactly Python's codec (0: none)
    want = 0
    if b >= 0xA0:
        try:
            want = bytes([b]).decode("latin-1").encode("cp437")[0]
        except UnicodeEncodeError:
            pass
    if keys[b] != want:
        bad.append(f"keys {b:02x}: {keys[b]:02x}, cp437 says {want:02x}")
for b in range(256):                     # CP437 -> Latin-1: what Latin-1 has is kept, ASCII as it is, the rest plain
    u = ord(bytes([b]).decode("cp437"))
    if 0x20 <= b < 0x7F and fb[b] != b:
        bad.append(f"fallback {b:02x}: ASCII changed")
    elif b >= 0x80 and 0xA0 <= u <= 0xFF and fb[b] != u:
        bad.append(f"fallback {b:02x}: {fb[b]:02x}, Latin-1 has {u:02x}")
    elif b >= 0x80 and not 0xA0 <= u <= 0xFF and not (0x20 <= fb[b] < 0x7F or fb[b] == 0xB7):
        bad.append(f"fallback {b:02x}: {fb[b]:02x} is not plain")
for b in (0x07, 0x08, 0x09, 0x0A, 0x0D, 0x1B):
    if fb[b] != b:
        bad.append(f"fallback {b:02x}: a working control changed")
if bad:
    print("\n".join(bad))
    sys.exit(1)
print("telnet-test: the CP437 tables agree with Python's cp437 codec")
PY
    then say "core PASS"; else bad "core (the FAIL lines above)"; fi
}

# libFuzzer, one process at a time with a memory limit (this PC's rule: knowledge base claude-code/, memory)
part_fuzz() {
    local secs=$1 d="$OUT/fuzz" FCC=${FUZZCC:-clang} fl="-fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
    command -v "$FCC" >/dev/null || { bad "fuzz needs clang (libFuzzer)"; return; }
    mkdir -p "$d/corpus" "$d/artifacts" "$d/seeds"
    $FCC $HCFLAGS $fl -o "$d/telnet-fuzz" tools/host/telnet-fuzz.c net/telnet/*.c || { bad "telnet-fuzz does not build"; return; }
    # seeds: each mode with negotiation, music, the font switch, the Amiga console's codes, UTF-8
    printf '\x11\x07\xff\xfd\x18\xff\xfa\x18\x01\xff\xf0\xff\xfb\x01\xff\xfd\x1f\x1b[NCDE\x0e\x1b[ D' >"$d/seeds/pc"
    printf '\x12\x03\x9b0 q\x9b80u\x9b0 p\x1b[0;42 D\x0c\x1a\x1b[?33h\x1b[=2M\x1b[MABC' >"$d/seeds/amiga"
    printf '\x03\x40\xc3\xa5\xe2\x94\x80\xff\xff\x1b[31m\r\x00' >"$d/seeds/unix"
    say "fuzz: $secs s, one process, 2 GB memory limit (log $d/fuzz.log)"
    if nice "$d/telnet-fuzz" -max_total_time="$secs" -rss_limit_mb=2048 -malloc_limit_mb=512 -max_len=8192 -timeout=10 \
            -artifact_prefix="$d/artifacts/" -print_final_stats=1 "$d/corpus" "$d/seeds" >"$d/fuzz.log" 2>&1; then
        say "fuzz: $(grep -a 'stat::number_of_executed_units' "$d/fuzz.log" | awk '{ print $2 }') inputs run, $(ls "$d/corpus" | wc -l) in the corpus, $(grep -a 'cov:' "$d/fuzz.log" | tail -1 | sed 's/.*cov: \([0-9]*\).*/\1/') edges covered"
        say "fuzz PASS: nothing found"
    else
        grep -a 'ERROR\|SUMMARY\|Test unit written' "$d/fuzz.log" | head -20
        bad "fuzz found something: $d/artifacts"
    fi
}

parts=("$@")
[ ${#parts[@]} = 0 ] && parts=(font core zmodem)
for ((i = 0; i < ${#parts[@]}; i++)); do
    p=${parts[i]}
    case "$p" in
    font) part_font ;;
    core) part_core ;;
    zmodem) part_zmodem ;;
    fuzz) if [[ "${parts[i + 1]:-}" =~ ^[0-9]+$ ]]; then part_fuzz "${parts[i + 1]}"; i=$((i + 1)); else part_fuzz 60; fi ;;
    *) echo "telnet-test: no part called $p"; exit 2 ;;
    esac
done
[ $fails = 0 ] && { say "all passed"; exit 0; }
say "$fails FAILED"; exit 1
