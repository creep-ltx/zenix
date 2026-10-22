#!/usr/bin/env python3
"""Wrap a filesystem image in an Amiga RDB (one partition), as a real
hard disk or CF card has. usage: mkrdb.py fs.img out.hdf [ext2|zenixfs]"""
import struct
import sys

fs, out = sys.argv[1:3]
DOSTYPE = {"ext2": 0x4C4E5800, "zenixfs": 0x5A4E4601}[sys.argv[3] if len(sys.argv) > 3 else "ext2"]
data = open(fs, "rb").read()
HEADS, SECS = 16, 63
CYL = HEADS * SECS * 512
lowcyl = 1
ncyl = lowcyl + (len(data) + CYL - 1) // CYL


def block(longs):
    longs = list(longs) + [0] * (128 - len(longs))
    n = longs[1]
    longs[2] = 0
    longs[2] = (-sum(longs[:n])) & 0xFFFFFFFF
    return struct.pack(">128L", *longs)


rdb = [0x5244534B, 64, 0, 7, 512, 0x12, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF] + [0xFFFFFFFF] * 6
rdb += [ncyl, SECS, HEADS, 0, ncyl] + [0] * 3 + [ncyl] * 3 + [0] * 5
rdb += [0, 2, lowcyl, ncyl - 1, HEADS * SECS, 0, 1]
part = [0x50415254, 64, 0, 7, 0xFFFFFFFF, 1] + [0] * 2 + [0] * 1
name = b"\x03ZX0" + b"\0" * 28
part = part[:9] + list(struct.unpack(">8L", name)) + [0] * 15
part += [16, 128, 0, HEADS, 1, SECS, 2, 0, 0, lowcyl, ncyl - 1, 30, 0, 0x7FFFFFFF, 0xFFFFFFFE, 0, DOSTYPE]
img = block(rdb) + block(part)
img += b"\0" * (lowcyl * CYL - len(img))
img += data
img += b"\0" * (ncyl * CYL - len(img))
open(out, "wb").write(img)
print(f"{out}: RDB + partition at sector {lowcyl * HEADS * SECS} ({len(data)//1024} KB)")
