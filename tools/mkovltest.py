#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's overlaid test program (docs/d29-zine.md 16.6; tools/tests/zine/zovl.S): an assembled object (m68k-elf,
position-independent sections, no ELF relocations) written as an overlaid AmigaOS hunk executable, laid out as the
AmigaDOS Manual ("hunk_overlay", "Hunk Overlay Table") and the Guru Book (22.2.10) describe it:

  root node     HUNK_HEADER (table size 4, hunks 0-1), .text as HUNK_CODE, .rootdata as HUNK_DATA
  HUNK_OVERLAY  its length (the upper bound: length + 1 longwords follow), the offset to the entries (levels + 2),
                the ordinate table (1 level) and its 0, three entries of 8 longwords (seek offset, 0, 0, level,
                ordinate, initial hunk, symbol hunk, symbol offset + 4): node A, node B, and one whose seek offset
                is the trailing data (no node there: the failing load's test)
  node A        HUNK_HEADER (hunk 2), .ova as HUNK_CODE, HUNK_BREAK
  node B        HUNK_HEADER (hunks 2-3), .ovb as HUNK_CODE with HUNK_RELOC32SHORT, a 16-byte chip HUNK_BSS,
                HUNK_BREAK
  16 bytes      "ZOVL-TRAILER-OK!" (data a program reads through its overlay file handle)

A longword to relocate is marked by a symbol R32_<hunk>_<name> in its section; it holds the offset in that hunk.

usage: mkovltest.py in.o out
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

src, out = sys.argv[1:3]
prefix = os.environ.get("TOOLPREFIX", "m68k-elf-")
rel = subprocess.run([prefix + "readelf", "-r", src], capture_output=True, text=True, check=True).stdout
if "Relocation section" in rel:
    sys.exit(f"{src}: has relocations; the sections must be position-independent (R32_ symbols mark relocations)")
syms = {}
for line in subprocess.run([prefix + "objdump", "-t", src], capture_output=True, text=True, check=True).stdout.splitlines():
    f = line.split()
    if len(f) >= 4 and re.fullmatch(r"[0-9a-f]{8}", f[0]):
        syms.setdefault(f[-3], {})[f[-1]] = int(f[0], 16)


def section(name):
    with tempfile.TemporaryDirectory() as d:
        b = os.path.join(d, "s.bin")
        subprocess.run([prefix + "objcopy", "-O", "binary", "-j", name, src, b], check=True)
        data = open(b, "rb").read()
    return data + bytes(-len(data) % 4)


def relocs(name):
    r = {}
    for s, v in syms.get(name, {}).items():
        m = re.fullmatch(r"R32_(\d+)_\w+", s)
        if m:
            r.setdefault(int(m.group(1)), []).append(v)
    return r


L = lambda *v: struct.pack(">" + "I" * len(v), *v)
HDR, CODE, DATA, BSS, R32, R32S, END, OVL, BRK = 0x3F3, 0x3E9, 0x3EA, 0x3EB, 0x3EC, 0x3FC, 0x3F2, 0x3F5, 0x3F6


def hunk(kind, name, short=False):
    d = section(name)
    b = L(kind, len(d) // 4) + d
    r = relocs(name)
    if r and not short:
        b += L(R32)
        for h, offs in sorted(r.items()):
            b += L(len(offs), h, *offs)
        b += L(0)
    elif r:
        w = []
        for h, offs in sorted(r.items()):
            w += [len(offs), h] + offs
        w.append(0)
        if len(w) % 2:
            w.append(0)                 # the padding word to a longword (the Guru Book: "must")
        b += L(R32S) + struct.pack(">" + "H" * len(w), *w)
    return b + L(END)


T = 4
text, rdata, ova, ovb = (section(s) for s in (".text", ".rootdata", ".ova", ".ovb"))
root = L(HDR, 0, T, 0, 1, len(text) // 4, len(rdata) // 4) + hunk(CODE, ".text") + hunk(DATA, ".rootdata")
node_a = L(HDR, 0, T, 2, 2, len(ova) // 4) + hunk(CODE, ".ova") + L(BRK)
node_b = L(HDR, 0, T, 2, 3, len(ovb) // 4, 4 | 1 << 30) + hunk(CODE, ".ovb", short=True) + L(BSS | 1 << 30, 4, END) + L(BRK)
levels = 1
entries = 3
ovl_words = (levels + 2) + 8 * entries          # the offset word, the ordinates, their 0, the entries
off_a = len(root) + 8 + 4 * ovl_words
off_b = off_a + len(node_a)
trail = off_b + len(node_b)
ent = lambda seek, ordinate, sym: L(seek, 0, 0, 1, ordinate, 2, 2, sym + 4)
table = L(levels + 2) + L(0) * levels + L(0)
table += ent(off_a, 1, syms[".ova"]["entA"]) + ent(off_b, 2, syms[".ovb"]["entB"]) + ent(trail, 3, 0)
assert len(table) == 4 * ovl_words
img = root + L(OVL, ovl_words - 1) + table + node_a + node_b + b"ZOVL-TRAILER-OK!"
assert len(img) == trail + 16
open(out, "wb").write(img)
os.chmod(out, 0o755)
