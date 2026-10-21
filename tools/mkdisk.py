#!/usr/bin/env python3
"""Raw boot disk until the filesystem exists: sector 0 = ZDIR table
(magic, count, 15 x {name[20], lba, bytes}), files from sector 1 on.
usage: mkdisk.py out.hdf file.zx ...   (entry name = file name minus .zx)"""
import os
import struct
import sys

out, files = sys.argv[1], sys.argv[2:]
ents, data, lba = [], b"", 1
for f in files:
    d = open(f, "rb").read()
    name = os.path.basename(f).removesuffix(".zx").encode()
    ents.append(struct.pack(">20sII", name, lba, len(d)))
    pad = (-len(d)) % 512
    data += d + b"\0" * pad
    lba += (len(d) + pad) // 512
hdr = struct.pack(">II", 0x5a444952, len(ents)) + b"".join(ents)
hdr += b"\0" * (512 - len(hdr))
img = hdr + data
img += b"\0" * ((8 * 1024 * 1024) - len(img))
open(out, "wb").write(img)
print(f"{out}: {len(ents)} programs")
