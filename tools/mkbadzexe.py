#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write corrupt copies of a good ZEXE program, one per header or relocation fault the loader must refuse
(audit B7). usage: tools/mkbadzexe.py good.zx outdir   -> outdir/badzexe1 ... badzexeN"""
import os
import struct
import sys

good, outdir = sys.argv[1:3]
img = bytearray(open(good, "rb").read())
image_size, mem_size, nreloc, reloc_off, stack = struct.unpack(">5I", img[8:28])
assert img[4:8] == b"ZEXE" and nreloc > 0


def variant(**fields):
    b = bytearray(img)
    vals = dict(image_size=image_size, mem_size=mem_size, nreloc=nreloc, reloc_off=reloc_off, stack=stack)
    vals.update({k: v for k, v in fields.items() if k in vals})
    b[8:28] = struct.pack(">5I", vals["image_size"], vals["mem_size"], vals["nreloc"], vals["reloc_off"], vals["stack"])
    if "reloc0" in fields:                       # the first relocation entry
        b[reloc_off:reloc_off + 4] = struct.pack(">I", fields["reloc0"])
    if "magic" in fields:
        b[4:8] = fields["magic"]
    if "cut" in fields:
        b = b[:fields["cut"]]
    return bytes(b)


cases = [
    variant(image_size=len(img) + 4096),              # 1 image bigger than the file
    variant(nreloc=0x40000001),                       # 2 nreloc * 4 wraps to 4
    variant(reloc_off=0xFFFFFFF0),                    # 3 table far outside the file
    variant(reloc0=0x7FFFFFF0),                       # 4 a relocation far outside the image
    variant(reloc0=image_size - 2),                   # 5 a relocation running past the end of the image
    variant(reloc0=0x101),                            # 6 an odd relocation offset
    variant(mem_size=0xFFFFFFF0),                     # 7 memory size near 4 GB (the span would wrap)
    variant(stack=0xFFFFFF00),                        # 8 stack size near 4 GB
    variant(mem_size=image_size - 4),                 # 9 memory smaller than the image
    variant(magic=b"ZEXF"),                           # 10 not a program
    variant(cut=20),                                  # 11 shorter than a header
]
os.makedirs(outdir, exist_ok=True)
for i, c in enumerate(cases, 1):
    with open(os.path.join(outdir, f"badzexe{i}"), "wb") as f:
        f.write(c)
print(f"{len(cases)} corrupt programs in {outdir}")
