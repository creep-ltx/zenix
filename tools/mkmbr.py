#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Build a test disk with a PC partition table (MBR), like the SD card of an Emu68 machine:
partition 1 = FAT (type 0x0c, with a few fake boot files), partition 2 = Zenix's ext2 root.
usage: mkmbr.py out.hdf root-ext2.img [rdb]
With "rdb" partition 2 is type 0x76 and holds an Amiga RDB whose one partition is the ext2 root
(Emu68's way of putting Amiga partitions on a PC-partitioned card)."""
import os
import struct
import subprocess
import sys
import tempfile

out, root = sys.argv[1], sys.argv[2]
use_rdb = len(sys.argv) > 3 and sys.argv[3] == "rdb"
here = os.path.dirname(os.path.abspath(__file__))

fat_sectors = 32768                                      # 16 MB
with tempfile.TemporaryDirectory() as t:
    fat = os.path.join(t, "fat.img")
    subprocess.run(["dd", "if=/dev/zero", f"of={fat}", "bs=512", f"count={fat_sectors}", "status=none"], check=True)
    subprocess.run(["mkfs.fat", "-F", "16", "-n", "EMU68BOOT", fat], check=True, stdout=subprocess.DEVNULL)
    for name in ("config.txt", "kick.rom"):
        src = os.path.join(t, name)
        open(src, "wb").write(b"fake " + name.encode() + b"\n")
        subprocess.run(["mcopy", "-i", fat, src, "::" + name], check=True)
    fat_data = open(fat, "rb").read()
    if use_rdb:
        rdbimg = os.path.join(t, "rdb.img")
        subprocess.run([sys.executable, os.path.join(here, "mkrdb.py"), root, rdbimg], check=True, stdout=subprocess.DEVNULL)
        data2 = open(rdbimg, "rb").read()
    else:
        data2 = open(root, "rb").read()

START1 = 2048
START2 = START1 + fat_sectors
len2 = (len(data2) + 511) // 512
total = (START2 + len2 + 63) // 64 * 64
mbr = bytearray(512)
mbr[0:3] = b"\xeb\xfe\x90"                               # a harmless boot stub
def entry(slot, active, ptype, start, sectors):
    mbr[446 + 16 * slot: 446 + 16 * slot + 16] = struct.pack("<B3sB3sII", active, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, sectors)
entry(0, 0x80, 0x0C, START1, fat_sectors)
entry(1, 0, 0x76 if use_rdb else 0x83, START2, len2)
mbr[510:512] = b"\x55\xaa"
img = bytearray(total * 512)
img[0:512] = mbr
img[START1 * 512: START1 * 512 + len(fat_data)] = fat_data
img[START2 * 512: START2 * 512 + len(data2)] = data2
open(out, "wb").write(img)
print(f"{out}: MBR, FAT at sector {START1}, {'RDB-in-MBR (0x76)' if use_rdb else 'ext2 (0x83)'} at sector {START2}, {total} sectors")
