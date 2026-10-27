#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make the PC font of DESIGN D20 4.2 (/usr/share/consolefonts/cp437-8x8.psf) from Linux's font_8x8.c:
    mkpsf.py SRC.c OUT.psf        write it (PSF1: 36 04, mode 0, height 8, 256 x 8 bytes: 2,052 bytes)
    mkpsf.py --check SRC.c        only check the source (exit 1 and the reason when it is wrong)
The source's glyph lines look like "0x18, /* 00011000 */": every hex byte is checked against its bit picture, there
must be exactly 2,048 of them (256 glyphs of 8 rows, CP437 order), and a few glyphs are checked by their known shape
(0x00 and 0xFF blank, 0xDB the full block, 0x84 a-umlaut, 0x86 a-ring, 0xB0 the light shade, 0x01 the smiley)."""
import re
import sys

PSF1_MAGIC = b"\x36\x04"
GLYPHS, HEIGHT = 256, 8


def read_glyphs(path):
    data = []
    for n, line in enumerate(open(path, encoding="latin-1"), 1):
        m = re.match(r"\s*0x([0-9a-fA-F]{2}),\s*/\*\s*([01]{8})\s*\*/", line)
        if not m:
            continue
        v = int(m.group(1), 16)
        if v != int(m.group(2), 2):
            raise ValueError(f"{path}:{n}: 0x{v:02x} does not match its picture {m.group(2)}")
        data.append(v)
    if len(data) != GLYPHS * HEIGHT:
        raise ValueError(f"{path}: {len(data)} glyph bytes, not {GLYPHS * HEIGHT}")
    g = [bytes(data[i * HEIGHT:(i + 1) * HEIGHT]) for i in range(GLYPHS)]
    checks = {                                                  # the pictures, as drawn above in the source
        0x00: "0000000000000000", 0xFF: "0000000000000000",      # blank
        0xDB: "ffffffffffffffff",                               # the full block
        0xB0: "2288228822882288",                               # the light shade
        0x84: "c600780c7ccc7600", 0x86: "3030780c7ccc7600",      # a-umlaut, a-ring
        0x01: "7e81a581bd99817e", 0x41: "386cc6fec6c6c600",      # the smiley, 'A'
    }
    for c, ok in checks.items():
        if g[c].hex() != ok:
            raise ValueError(f"{path}: glyph 0x{c:02x} is not the expected CP437 shape: {g[c].hex()}")
    return b"".join(g)


def main(argv):
    if len(argv) == 3 and argv[1] == "--check":
        read_glyphs(argv[2])
        return 0
    if len(argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    glyphs = read_glyphs(argv[1])
    out = PSF1_MAGIC + bytes([0, HEIGHT]) + glyphs              # mode 0: 256 glyphs, no Unicode table
    assert len(out) == 2052
    with open(argv[2], "wb") as f:
        f.write(out)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except ValueError as e:
        print(f"mkpsf: {e}", file=sys.stderr)
        sys.exit(1)
