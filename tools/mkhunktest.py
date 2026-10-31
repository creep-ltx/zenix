#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's test programs (DESIGN D29 section 15): an assembled object (m68k-elf, position-independent code only, no
relocations) written as an AmigaOS hunk executable with one code hunk: HUNK_HEADER, HUNK_CODE, HUNK_END.

usage: mkhunktest.py in.o out
"""
import os
import struct
import subprocess
import sys
import tempfile

src, out = sys.argv[1:3]
prefix = os.environ.get("TOOLPREFIX", "m68k-elf-")
rel = subprocess.run([prefix + "readelf", "-r", src], capture_output=True, text=True, check=True).stdout
if "Relocation section" in rel:
    sys.exit(f"{src}: has relocations; ZINE's test programs are position-independent (PC-relative) code only")
with tempfile.TemporaryDirectory() as d:
    b = os.path.join(d, "text.bin")
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", src, b], check=True)
    code = open(b, "rb").read()
code += bytes(-len(code) % 4)
n = len(code) // 4
h = struct.pack(">IIIIII", 0x3F3, 0, 1, 0, 0, n)          # HUNK_HEADER: no names, 1 hunk, first 0, last 0, its size
h += struct.pack(">II", 0x3E9, n) + code + struct.pack(">I", 0x3F2)
open(out, "wb").write(h)
os.chmod(out, 0o755)
