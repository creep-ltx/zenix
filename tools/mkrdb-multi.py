#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make an Amiga disk image with an RDB and several partitions, as a real hard disk or CF card has.
usage: mkrdb-multi.py out.hdf image:type[:label] [image:type[:label] ...]
type is ext2 or zenixfs. The first partition is marked bootable. An image of "-" or a size such as "4M" makes an empty partition
(for a filesystem to be made inside Zenix); a real file is copied in."""
import os
import struct
import sys

TYPES = {"ext2": 0x4C4E5800, "zenixfs": 0x5A4E4601}
HEADS, SECS = 16, 63
CYL = HEADS * SECS * 512


def block(longs):
    longs = list(longs) + [0] * (128 - len(longs))
    n = longs[1]
    longs[2] = 0
    longs[2] = (-sum(longs[:n])) & 0xFFFFFFFF
    return struct.pack(">128L", *longs)


def main():
    out = sys.argv[1]
    parts = []
    for i, spec in enumerate(sys.argv[2:]):
        f = spec.split(":")
        src, typ = f[0], f[1]
        label = f[2] if len(f) > 2 else f"ZX{i}"
        if os.path.exists(src):
            data = open(src, "rb").read()
        else:
            mult = {"K": 1024, "M": 1 << 20}.get(src[-1:].upper(), 1)
            data = bytes(int(src[:-1] if mult > 1 else src) * mult)
        parts.append((data, TYPES[typ], label))
    n = len(parts)
    lowcyl = 1
    cyls = []
    for data, _, _ in parts:
        c = max(1, (len(data) + CYL - 1) // CYL)
        cyls.append((lowcyl, lowcyl + c - 1))
        lowcyl += c
    total = lowcyl
    rdb = [0x5244534B, 64, 0, 7, 512, 0x12, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF] + [0xFFFFFFFF] * 6
    rdb += [total, SECS, HEADS, 0, total] + [0] * 3 + [total] * 3 + [0] * 5
    rdb += [0, n + 1, cyls[0][0], total - 1, HEADS * SECS, 0, n]
    img = block(rdb)
    for i, (data, dostype, label) in enumerate(parts):
        nxt = i + 2 if i + 1 < n else 0xFFFFFFFF
        name = bytes([len(label)]) + label.encode() + b"\0" * (31 - len(label))
        part = [0x50415254, 64, 0, 7, nxt, 1 if i == 0 else 0, 0, 0]       # PART, size, checksum, host id, next block, flags (1 = bootable)
        part += [0]
        part += list(struct.unpack(">8L", name))
        part += [0] * 15
        part += [16, 128, 0, HEADS, 1, SECS, 2, 0, 0, cyls[i][0], cyls[i][1], 30, 0, 0x7FFFFFFF, 0xFFFFFFFE, 0, dostype]
        img += block(part)
    img += b"\0" * (cyls[0][0] * CYL - len(img))
    for (data, _, _), (lo, hi) in zip(parts, cyls):
        img += data
        img += b"\0" * ((hi - lo + 1) * CYL - len(data))
    open(out, "wb").write(img)
    print(f"{out}: RDB + {n} partition(s), {len(img) // 1024} KB")


main()
