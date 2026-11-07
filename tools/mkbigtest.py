#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's big test program (docs/d29-zine.md 16.12): zbig, whose file and hunks do not fit in memory together on the
test machine (A1200, 2 MB chip, 4 MB fast), so `zine` loads it from its file before the hand-over; and its folder,
too big for ZINE's volume there: a small data file (in), a large one and an archive (left out). From an assembled
object (zbig.S, position-independent; its longwords at 4 and 8 are relocated to the data hunks):
    hunk 0 code; hunk 1 data in chip RAM, CHIPKB KB, longword i = 0xC0000000 | i;
    hunk 2 data in fast RAM, FASTKB KB, longword i = 0xFA000000 | i; a BSS hunk of 64 KB (any memory)
usage: mkbigtest.py zbig.o OUTDIR      (writes OUTDIR/zbig, zbig.small, zbig.huge, zbig-pack.lha)"""
import os
import struct
import subprocess
import sys
import tempfile

CHIPKB, FASTKB = 1500, 1250
src, out = sys.argv[1:3]
prefix = os.environ.get("TOOLPREFIX", "m68k-elf-")
rel = subprocess.run([prefix + "readelf", "-r", src], capture_output=True, text=True, check=True).stdout
if "Relocation section" in rel:
    sys.exit(f"{src}: has relocations; the test program is position-independent (PC-relative) code only")
with tempfile.TemporaryDirectory() as d:
    b = os.path.join(d, "text.bin")
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", src, b], check=True)
    code = open(b, "rb").read()
code += bytes(-len(code) % 4)
nc, n1, n2 = len(code) // 4, CHIPKB * 256, FASTKB * 256
h = struct.pack(">IIIII", 0x3F3, 0, 4, 0, 3)
h += struct.pack(">IIII", nc, n1 | 1 << 30, n2 | 2 << 30, 16384)
h += struct.pack(">II", 0x3E9, nc) + code
h += struct.pack(">IIIIIIII", 0x3EC, 1, 1, 4, 1, 2, 8, 0) + struct.pack(">I", 0x3F2)
for n, tag, mem in ((n1, 0xC0000000, 1 << 30), (n2, 0xFA000000, 2 << 30)):
    h += struct.pack(">II", 0x3EA | mem, n)
    h += b"".join(struct.pack(">I", tag | i) for i in range(n)) + struct.pack(">I", 0x3F2)
h += struct.pack(">III", 0x3EB, 16384, 0x3F2)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "zbig"), "wb").write(h)
os.chmod(os.path.join(out, "zbig"), 0o755)
open(os.path.join(out, "zbig.small"), "wb").write(b"SMALL DATA FILE\n" * 64)
open(os.path.join(out, "zbig.huge"), "wb").write(b"HUGE" * (1536 * 256))
open(os.path.join(out, "zbig-pack.lha"), "wb").write(b"\x00-lh5-" + bytes(100 * 1024))
