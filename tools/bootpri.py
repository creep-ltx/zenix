#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Show or change the boot priority of a partition in an Amiga RDB, on a disk image or a device (an SD card's RDB
# partition, e.g. /dev/sdX2). Only the one PART block's DosEnvec de_BootPri and its checksum are written.
#
# usage: tools/bootpri.py DISK                 list the partitions, their boot flag and priority
#        tools/bootpri.py DISK NAME PRI        set partition NAME's boot priority to PRI (-128..127)
# Kickstart's floppy boots at priority 5 and loses a tie with a disk partition at 5: a partition that should leave
# DF0 first needs 4 or lower.
import struct, sys

def checksum(blk):
    n = struct.unpack_from(">I", blk, 4)[0]
    return sum(struct.unpack_from(f">{n}I", blk)) & 0xFFFFFFFF

def main():
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__ or "usage: bootpri.py DISK [NAME PRI]")
    disk = sys.argv[1]
    with open(disk, "r+b" if len(sys.argv) == 4 else "rb") as f:
        for b in range(16):
            f.seek(b * 512)
            blk = f.read(512)
            if blk[:4] == b"RDSK":
                break
        else:
            sys.exit(f"{disk}: no RDB in the first 16 blocks")
        bs = struct.unpack_from(">I", blk, 0x10)[0]
        nxt = struct.unpack_from(">I", blk, 0x1C)[0]
        parts = []
        while nxt != 0xFFFFFFFF and len(parts) < 64:
            f.seek(nxt * bs)
            p = bytearray(f.read(bs))
            if p[:4] != b"PART" or checksum(p) != 0:
                sys.exit(f"{disk}: bad PART block at {nxt}")
            name = p[0x25:0x25 + p[0x24]].decode("latin-1")
            flags = struct.unpack_from(">I", p, 0x14)[0]
            pri = struct.unpack_from(">i", p, 0x80 + 15 * 4)[0]
            parts.append((nxt, name, flags, pri, p))
            nxt = struct.unpack_from(">I", p, 0x10)[0]
        for _, name, flags, pri, _ in parts:
            print(f"{name:8} {'bootable' if flags & 1 else '-       '} pri {pri:4}{'  nomount' if flags & 2 else ''}")
        if len(sys.argv) == 2:
            return
        want, pri = sys.argv[2], int(sys.argv[3])
        if not -128 <= pri <= 127:
            sys.exit("priority must be -128..127")
        hit = [x for x in parts if x[1].upper() == want.upper()]
        if not hit:
            sys.exit(f"no partition {want}")
        blkno, name, _, old, p = hit[0]
        struct.pack_into(">i", p, 0x80 + 15 * 4, pri)
        struct.pack_into(">I", p, 8, 0)
        struct.pack_into(">I", p, 8, (-checksum(p)) & 0xFFFFFFFF)
        f.seek(blkno * bs)
        f.write(p)
        f.flush()
        print(f"{name}: boot priority {old} -> {pri}")

main()
