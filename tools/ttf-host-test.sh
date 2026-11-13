#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The TrueType rasteriser on the PC (DESIGN D51, docs/d51-truetype.md "Tests"): tools/ttf-host-test.c with
# AddressSanitizer and UndefinedBehaviorSanitizer, on the font Zenix ships (display/ttf/go.ttf): compared with
# FreeType's unhinted black-and-white glyphs when FreeType is installed (pkg-config freetype2), every size read back
# by the Amiga font reader, damaged files refused; the console's ZCF1 font checked by the kernel's checks
# (tools/mkconfont.py check).
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zenix-ttf.XXXXXX")
trap 'rm -rf "$T"' EXIT
FT=""
if pkg-config --exists freetype2 2>/dev/null; then FT="-DHAVE_FREETYPE $(pkg-config --cflags --libs freetype2)"; fi
# shellcheck disable=SC2086
cc -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Idisplay/ttf -Idisplay/server tools/ttf-host-test.c display/ttf/ttf.c display/ttf/ttfont.c \
    display/server/amifont.c $FT -o "$T/t" || { echo "FAIL build"; exit 1; }
ASAN_OPTIONS=detect_leaks=1 "$T/t" display/ttf/go.ttf "$T/go.zcf"; rc=$?
if [ -f "$T/go.zcf" ]; then
    python3 tools/mkconfont.py check "$T/go.zcf" >/dev/null 2>"$T/zcf.err"; z=$?
    if [ $z = 0 ]; then echo "PASS the console's font passes the kernel's checks (mkconfont.py check)"
    else echo "FAIL the console's font: $(cat "$T/zcf.err")"; rc=1; fi
fi
exit $rc
