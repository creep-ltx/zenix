#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The TrueType font Zenix ships (DESIGN D51, docs/d51-truetype.md "The font shipped"): Go Regular by Bigelow & Holmes
# (the Go project's fonts, BSD-3-Clause: display/ttf/gofont.LICENSE), cut with fontTools' pyftsubset to Latin-1,
# Latin Extended-A, the dashes, quotes, bullet, ellipsis, euro, trade mark, arrows, box lines and blocks; its hinting
# left out (Zenix draws without it). Run once by hand; the result is committed as display/ttf/go.ttf.
#   tools/mkgofont.sh Go-Regular.ttf      (from golang.org/x/image/font/gofont/ttfs; pyftsubset on PATH)
set -e
[ -f "$1" ] || { echo "usage: tools/mkgofont.sh Go-Regular.ttf" >&2; exit 2; }
cd "$(dirname "$0")/.."
pyftsubset "$1" --unicodes='U+0020-007E,U+00A0-017F,U+2013-2014,U+2018-201E,U+2020-2022,U+2026,U+20AC,U+2122,U+2190-2193,U+2500-257F,U+2580-259F' \
    --no-hinting --drop-tables+=DSIG,GPOS,GSUB,GDEF,kern,FFTM --output-file=display/ttf/go.ttf
ls -l display/ttf/go.ttf
