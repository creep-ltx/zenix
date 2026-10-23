#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Turn a ZEXE image (tools/mkzexe.py) into an AmigaOS executable (hunk format), for zenixboot.

usage: mkhunk.py in.zx out

A ZEXE is a flat image linked at address 0 plus a table of the 32-bit words that hold addresses; an AmigaOS
executable can say the same with one code hunk and a HUNK_RELOC32 block for that hunk. The hunk's memory size
covers the BSS too (the image is padded with zeros up to the ZEXE's mem_size, so nothing relies on LoadSeg clearing
memory). AmigaOS starts the program at offset 0 of the hunk: the ZEXE header's bra.w.

  HUNK_HEADER  0x3F3: no resident libraries, 1 hunk (0..0), its size in longs
  HUNK_CODE    0x3E9: size in longs, the image
  HUNK_RELOC32 0x3EC: count, hunk 0, offsets ...; 0
  HUNK_END     0x3F2
"""
import struct
import sys


def main():
    src, out = sys.argv[1:3]
    z = open(src, "rb").read()
    if z[4:8] != b"ZEXE":
        sys.exit(f"{src}: not a ZEXE image")
    image, mem, nrel, reloff = struct.unpack(">4I", z[8:24])
    if reloff + 4 * nrel > len(z) or image > len(z) or mem < image:
        sys.exit(f"{src}: damaged ZEXE header")
    relocs = list(struct.unpack(f">{nrel}I", z[reloff:reloff + 4 * nrel]))
    code = bytearray(z[:image])
    code += bytes(mem - image)          # the BSS, as zeros in the file
    while len(code) % 4:
        code.append(0)
    for r in relocs:
        if r + 4 > image:
            sys.exit(f"{src}: relocation at {r:#x} outside the image")
    n = len(code) // 4
    h = struct.pack(">IIIII", 0x3F3, 0, 1, 0, 0) + struct.pack(">I", n)
    h += struct.pack(">II", 0x3E9, n) + bytes(code)
    if relocs:
        h += struct.pack(">III", 0x3EC, len(relocs), 0) + b"".join(struct.pack(">I", r) for r in relocs)
        h += struct.pack(">I", 0)
    h += struct.pack(">I", 0x3F2)
    open(out, "wb").write(h)
    print(f"{out}: AmigaOS executable, {len(code)} bytes in one hunk, {len(relocs)} relocations")


main()
