#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's test programs (DESIGN D29 section 15): an assembled object (m68k-elf, position-independent code only, no
relocations) written as an AmigaOS hunk executable with one code hunk: HUNK_HEADER, HUNK_CODE, HUNK_END. With a
section .ovldata, the way crunchers use the overlay mechanism (Spaceballs' Peek, docs/d29-zine.md 16.7): after the
root, HUNK_OVERLAY with a table of two longwords (the data's length, the length - 1), HUNK_BREAK, then those bytes.

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
    o = os.path.join(d, "ovl.bin")
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".ovldata", src, o], check=True)
    data = open(o, "rb").read() if os.path.exists(o) else b""
code += bytes(-len(code) % 4)
n = len(code) // 4
h = struct.pack(">IIIIII", 0x3F3, 0, 1, 0, 0, n)          # HUNK_HEADER: no names, 1 hunk, first 0, last 0, its size
h += struct.pack(">II", 0x3E9, n) + code + struct.pack(">I", 0x3F2)
if data:                                                    # HUNK_OVERLAY, length 1 (2 longwords follow), HUNK_BREAK
    h += struct.pack(">IIII", 0x3F5, 1, len(data), len(data) - 1) + struct.pack(">I", 0x3F6) + data
open(out, "wb").write(h)
os.chmod(out, 0o755)
