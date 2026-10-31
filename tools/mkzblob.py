#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""ZINE's blob (DESIGN D29 section 3): the ZEXE image of user/zine/blob*.c with the CRC32 of the whole file written
into its "ZINB" header (offset 32: magic, layout, CRC; the CRC is taken with its own four bytes zero). /bin/zine
refuses a blob whose CRC is wrong.

usage: mkzblob.py in.zx out.blob
"""
import struct
import sys
import zlib

src, out = sys.argv[1:3]
d = bytearray(open(src, "rb").read())
if d[4:8] != b"ZEXE" or d[32:36] != b"ZINB":
    sys.exit(f"{src}: not ZINE's blob (no ZEXE and ZINB headers)")
d[40:44] = bytes(4)
d[40:44] = struct.pack(">I", zlib.crc32(bytes(d)) & 0xFFFFFFFF)
open(out, "wb").write(d)
image, mem = struct.unpack(">II", d[8:16])
print(f"{out}: ZINE's blob, image {image} bytes, memory {mem} bytes")
