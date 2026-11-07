#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's stand-in for the ROM's topaz 8 (docs/d29-zine.md 16.12): user/zine/zfont.h, the printable ASCII glyphs
(32-126) of Linux's font_8x8.c (tools/font_8x8.c, GPL-2.0, its notice tools/font_8x8.COPYING) and a box after them
for every character the font lacks, as a ROM font draws one. The ROM's own topaz is Kickstart's, not ZINE's to copy.
    mkzfont.py SRC.c OUT.h       write it
    mkzfont.py --check SRC.c H   the committed header is what SRC makes"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkpsf import read_glyphs  # noqa: E402

BOX = bytes([0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00])


def make(src):
    g = read_glyphs(src)
    rows = [g[c * 8:(c + 1) * 8] for c in range(32, 127)] + [BOX]
    o = ["/* SPDX-License-Identifier: GPL-2.0-only */",
         "/* Made by tools/mkzfont.py from tools/font_8x8.c (Linux's lib/fonts/font_8x8.c, GPL-2.0; notice in",
         " * tools/font_8x8.COPYING): ZINE's topaz 8, the glyphs 32-126 and a box for the others. Do not edit. */",
         "#pragma once", "#define ZF_LO 32", "#define ZF_HI 126", "#define ZF_N 96", "#define ZF_ROWS 8",
         "#define ZF_BASE 6", "static const unsigned char zfont[ZF_N][ZF_ROWS] = {"]
    for i, r in enumerate(rows):
        o.append("    { " + ", ".join("0x%02x" % b for b in r) + " },%s" % ("" if i < 95 else "  /* the box */"))
    o.append("};")
    return "\n".join(o) + "\n"


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "--check":
        if open(a[2]).read() != make(a[1]):
            sys.exit("mkzfont: " + a[2] + " is not what " + a[1] + " makes")
    else:
        open(a[1], "w").write(make(a[0]))
