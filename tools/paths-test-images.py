#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The card of tools/paths-test.sh: an MBR, the ext2 root, a type 0x76 partition with an RDB holding an FFS volume (written
to), a PFS3 volume (read-only) and an empty partition for ZenixFS. Each volume has ok/ (a tree that fits in 255 bytes from
its mount point) and over/ (one that does not); the FFS volume also skadad/, a folder with a damaged entry.
usage: paths-test-images.py ROOT CARD OUTDIR
       paths-test-images.py --judge OUTDIR       p3, cut from the card by the test"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
from ffscheck import Disk
from mkffs import Volume as Ffs
from mkpfs3 import Volume as Pfs

CYL = 504 * 1024
D = "Mapp-" + "a" * 25                  # 30 bytes: 31 with its slash, seven levels 216


def fill(add):
    add("ok/" + "/".join([D] * 7) + "/fil.txt", b"deep-file\n")
    add("ok/" + "/".join([D] * 3) + "/stor.bin", bytes(i & 255 for i in range(30000)))
    add("over/" + "/".join([D] * 9) + "/over.txt", b"beyond\n")


def make(root, card, out):
    a = Ffs(2 * CYL // 512, "Djup", True, 3)
    fill(a.add_file)
    a.add_file("skadad/hel.txt", b"whole\n")
    bad = a.add_file("skadad/trasig.txt", b"damaged\n")
    img = bytearray(a.image())
    img[bad * 512 + 20] ^= 0xFF                 # the entry's checksum: listing the folder fails there
    pa = os.path.join(out, "a.img")
    open(pa, "wb").write(img)
    b = Pfs(2 * CYL // 512, "Djupa")
    fill(b.add_file)
    pb = os.path.join(out, "b.img")
    open(pb, "wb").write(b.image())
    pc = os.path.join(out, "c.img")
    open(pc, "wb").write(bytes(4 * CYL))
    rdb = os.path.join(out, "rdb.img")
    subprocess.run([sys.executable, os.path.join(HERE, "mkrdb-multi.py"), rdb, f"{pa}:ffs3:DH0", f"{pb}:pfs3:DH1",
                    f"{pc}:zenixfs:DH2"], check=True, stdout=subprocess.DEVNULL)
    r0, p2 = open(root, "rb").read(), open(rdb, "rb").read()
    s1 = 2048
    s2 = (s1 + len(r0) // 512 + 2047) // 2048 * 2048
    total = (s2 + len(p2) // 512 + 2047) // 2048 * 2048
    mbr = bytearray(512)
    for i, (act, t, s, n) in enumerate(((0x80, 0x83, s1, len(r0) // 512), (0, 0x76, s2, len(p2) // 512))):
        mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", act, b"\xfe\xff\xff", t, b"\xfe\xff\xff", s, n)
    mbr[510:512] = b"\x55\xaa"
    c = bytearray(total * 512)
    c[:512] = mbr
    c[s1 * 512:s1 * 512 + len(r0)] = r0
    c[s2 * 512:s2 * 512 + len(p2)] = p2
    open(card, "wb").write(c)
    open(os.path.join(out, "layout"), "w").write(f"{s1} {len(r0) // 512} {s2 + 16 * 63} {2 * CYL}\n")


def judge(out):
    d = Disk(os.path.join(out, "p3.img"))
    d.walk()
    # the damaged entry is the test's own: the judge may name that folder and nothing else
    bad = [w for w in d.bad if "skadad" not in w and "trasig" not in w]
    if d.read(("/ok/" + "/".join([D] * 7) + "/fil.txt").encode()) != b"deep-file\n":
        bad.append("the deep file")
    if any(n.startswith(b"/kopia") for n in d.files):
        bad.append("rm -r left a part of its copy")
    print("\n".join(bad))
    return not bad


if sys.argv[1] == "--judge":
    sys.exit(0 if judge(sys.argv[2]) else 1)
make(*sys.argv[1:4])
