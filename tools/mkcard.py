#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make a disk image that boots Zenix with no floppy: Kickstart boots a small AmigaDOS FFS partition whose
S:Startup-Sequence runs zenixboot, which starts the kernel; Zenix then mounts the ext2 root partition.

usage: mkcard.py sd  out.img boot-ffs.img root-ext2.img     the SD card of an Emu68 Amiga (PiStorm):
         MBR  p1  type 0x0c FAT32, 512 MB at sector 8192: left EMPTY here (tools/write-card.sh puts Emu68's files
                  there, or keeps the card's own)
              p2  type 0x76 (Emu68's "Amiga RDB inside"): an RDB with ONE partition, ZB0, the FFS boot volume
                  (DOS\\1, bootable, boot priority 0)
              p3  type 0x83: the ext2 root
       mkcard.py ide out.hdf boot-ffs.img root-ext2.img     a hard disk or CF card of any Amiga: an RDB (no MBR)
              with ZB0 (the same FFS boot volume) and ZR0, the ext2 root (dostype LNX\\0, flag "no mount", so that
              AmigaOS leaves it alone)
The FFS image must fill whole RDB cylinders (16 x 63 sectors = 504 KB; tools/mkffs.py with SIZE n*504K). The SD image
is written sparse (p1 is a hole), so it takes about the space of p2 and p3 on the PC.
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
P1_START, P1_SIZE = 8192, 1048576               # as on the Zenix test card made 2026-10-23 (tools/write-card.sh)
ALIGN = 2048                                    # partitions start on 1 MB boundaries


def rdb(out, *parts):
    subprocess.run([sys.executable, os.path.join(HERE, "mkrdb-multi.py"), out, *parts], check=True,
                   stdout=subprocess.DEVNULL)


def write_sparse(f, offset, data):
    """write data at offset, leaving 64 KB runs of zeros as holes"""
    step = 65536
    zero = bytes(step)
    for i in range(0, len(data), step):
        chunk = data[i:i + step]
        if chunk != zero[:len(chunk)]:
            f.seek(offset + i)
            f.write(chunk)


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ("sd", "ide"):
        sys.exit(__doc__)
    kind, out, boot, root = sys.argv[1:]
    if kind == "ide":
        rdb(out, f"{boot}:ffs:ZB0:boot", f"{root}:ext2:ZR0:nomount")
        print(f"{out}: RDB: ZB0 = FFS boot partition (zenixboot), ZR0 = ext2 root, {os.path.getsize(out) // 1024} KB")
        return
    with tempfile.TemporaryDirectory() as t:
        r = os.path.join(t, "rdb.img")
        rdb(r, f"{boot}:ffs:ZB0:boot")
        p2 = open(r, "rb").read()
    rootdata = open(root, "rb").read()
    p2_start = P1_START + P1_SIZE
    p2_len = (len(p2) + 511) // 512
    p3_start = (p2_start + p2_len + ALIGN - 1) // ALIGN * ALIGN
    p3_len = (len(rootdata) + 511) // 512
    total = (p3_start + p3_len + ALIGN - 1) // ALIGN * ALIGN
    mbr = bytearray(512)
    for slot, (active, ptype, start, n) in enumerate([(0x80, 0x0C, P1_START, P1_SIZE), (0, 0x76, p2_start, p2_len),
                                                      (0, 0x83, p3_start, p3_len)]):
        mbr[446 + 16 * slot:462 + 16 * slot] = struct.pack("<B3sB3sII", active, b"\xfe\xff\xff", ptype,
                                                          b"\xfe\xff\xff", start, n)
    mbr[510:512] = b"\x55\xaa"
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.truncate(total * 512)
        f.write(mbr)
        write_sparse(f, p2_start * 512, p2)
        write_sparse(f, p3_start * 512, rootdata)
    os.replace(tmp, out)
    print(f"{out}: MBR: p1 FAT32 (empty) at {P1_START}, p2 0x76 RDB+FFS boot at {p2_start} ({p2_len} sectors), "
          f"p3 0x83 ext2 root at {p3_start} ({p3_len} sectors); {total * 512 // (1 << 20)} MB")


main()
