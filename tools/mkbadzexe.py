#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write corrupt copies of a good ZEXE program, one per header or relocation fault the loader must refuse
(audit B7). usage: tools/mkbadzexe.py good.zx outdir   -> outdir/badzexe1 ... badzexeN
Given a ZEX2 program (DESIGN D13) it writes outdir/badzex2_1 ... badzex2_N instead."""
import os
import struct
import sys

good, outdir = sys.argv[1:3]
img = bytearray(open(good, "rb").read())
os.makedirs(outdir, exist_ok=True)

if img[4:8] == b"ZEX2":
    # a program with shared code (DESIGN D13): outdir/badzex2_1 ... N, each a header or relocation fault the loader must
    # refuse (docs/d13-shared-code.md section 11)
    F = ["text", "data", "mem", "nc", "nd", "ro", "stack", "res"]
    h = dict(zip(F, struct.unpack(">8I", img[8:40])))
    assert h["nc"] > 0 and h["nd"] > 0
    rel = lambda i: struct.unpack(">I", img[h["ro"] + 4 * i:h["ro"] + 4 * i + 4])[0]

    def v2(word=None, **f):
        b = bytearray(img)
        vals = dict(h, **{k: v for k, v in f.items() if k in h})
        b[8:40] = struct.pack(">8I", *(vals[k] for k in F))
        for i, v in f.get("rel", {}).items():       # relocation entry i (code list first)
            b[h["ro"] + 4 * i:h["ro"] + 4 * i + 4] = struct.pack(">I", v)
        if word:                                    # the word a relocation names gets another value
            at, v = word
            b[at:at + 4] = struct.pack(">I", v)
        return bytes(b)

    ts, ds = h["text"], h["data"]
    first_code, first_data = rel(0), rel(h["nc"])
    cases2 = [
        v2(text=len(img) + 16),                     # 1 shared part bigger than the file
        v2(text=16),                                # 2 shared part shorter than the header
        v2(text=ts + 4),                            # 3 shared part not 16-aligned
        v2(data=len(img)),                          # 4 data image outside the file
        v2(ro=ts),                                  # 5 the relocation table overlaps the data image
        v2(nc=0x40000001),                          # 6 nrel_code * 4 wraps
        v2(nd=(len(img) - h["ro"]) // 4),           # 7 the two lists together run past the file
        v2(mem=ds - 4),                             # 8 memory smaller than the data image
        v2(rel={0: 0x10}),                          # 9 a relocation placed in the shared part
        v2(rel={0: ts + ds}),                       # 10 a relocation past the data image
        v2(rel={0: first_code + 1}),                # 11 an odd relocation
        v2(word=(first_code, ts + 4)),              # 12 a to-code relocation whose word points into the data part
        v2(word=(first_data, 0x7FFFFFF0)),          # 13 a to-data relocation whose word points far outside
        v2(stack=0xFFFFFF00),                       # 14 stack size near 4 GB
    ]
    for i, c in enumerate(cases2, 1):
        with open(os.path.join(outdir, f"badzex2_{i}"), "wb") as f:
            f.write(c)
    print(f"{len(cases2)} corrupt ZEX2 programs in {outdir}")
    sys.exit(0)

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
