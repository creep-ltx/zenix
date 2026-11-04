#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's test programs (DESIGN D29 section 15): an assembled object (m68k-elf, position-independent code only, no
relocations) written as an AmigaOS hunk executable with one code hunk: HUNK_HEADER, HUNK_CODE, HUNK_END. With a
section .ovldata, the way crunchers use the overlay mechanism (Spaceballs' Peek, docs/d29-zine.md 16.7): after the
root, HUNK_OVERLAY with a table of two longwords (the data's length, the length - 1), HUNK_BREAK, then those bytes.

With -r the code may hold absolute 32-bit addresses (a library's RomTag and tables: docs/d29-zine.md 16.8): the
object is linked at 0 with its relocations kept (ld -q), and each R_68K_32 in the code becomes a HUNK_RELOC32 entry
to the hunk itself.

usage: mkhunktest.py [-r] in.o out
"""
import os
import struct
import subprocess
import sys
import tempfile

args = sys.argv[1:]
reloc = args[:1] == ["-r"]
if reloc:
    args = args[1:]
src, out = args[:2]
prefix = os.environ.get("TOOLPREFIX", "m68k-elf-")
rel = subprocess.run([prefix + "readelf", "-r", src], capture_output=True, text=True, check=True).stdout
offs = []
if "Relocation section" in rel and not reloc:
    sys.exit(f"{src}: has relocations; ZINE's test programs are position-independent (PC-relative) code only")
with tempfile.TemporaryDirectory() as d:
    if reloc:                                               # linked at 0, the relocations kept
        e = os.path.join(d, "linked.elf")
        subprocess.run([prefix + "ld", "-q", "-Ttext", "0", "-e", "0", "-o", e, src], check=True)
        rel = subprocess.run([prefix + "readelf", "-rW", e], capture_output=True, text=True, check=True).stdout
        sect = ""
        for line in rel.splitlines():
            if line.startswith("Relocation section"):
                sect = line.split("'")[1]
                continue
            f = line.split()
            if len(f) > 2 and f[2].startswith("R_68K_"):
                if sect != ".rela.text":
                    sys.exit(f"{src}: a relocation outside .text ({sect})")
                if f[2] == "R_68K_32":
                    offs.append(int(f[0], 16))
                elif f[2] not in ("R_68K_PC8", "R_68K_PC16", "R_68K_PC32"):
                    sys.exit(f"{src}: relocation {f[2]} is not one a hunk file can hold")
        src = e
    b = os.path.join(d, "text.bin")
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".text", src, b], check=True)
    code = open(b, "rb").read()
    o = os.path.join(d, "ovl.bin")
    subprocess.run([prefix + "objcopy", "-O", "binary", "-j", ".ovldata", src, o], check=True)
    data = open(o, "rb").read() if os.path.exists(o) else b""
code += bytes(-len(code) % 4)
n = len(code) // 4
h = struct.pack(">IIIIII", 0x3F3, 0, 1, 0, 0, n)          # HUNK_HEADER: no names, 1 hunk, first 0, last 0, its size
h += struct.pack(">II", 0x3E9, n) + code
if offs:                                                    # HUNK_RELOC32: the offsets, all to hunk 0
    h += struct.pack(">III", 0x3EC, len(offs), 0) + b"".join(struct.pack(">I", o) for o in sorted(offs))
    h += struct.pack(">I", 0)
h += struct.pack(">I", 0x3F2)
if data:                                                    # HUNK_OVERLAY, length 1 (2 longwords follow), HUNK_BREAK
    h += struct.pack(">IIII", 0x3F5, 1, len(data), len(data) - 1) + struct.pack(">I", 0x3F6) + data
open(out, "wb").write(h)
os.chmod(out, 0o755)
