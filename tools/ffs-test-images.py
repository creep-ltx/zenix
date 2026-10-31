#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The images of tools/ffs-test.sh, and its judgement of them afterwards.
usage: ffs-test-images.py ROOT CARD FLOPPY OUTDIR        make them
       ffs-test-images.py --judge OUTDIR LS.ZX           p3, p4, p5 and the floppy, cut from the card by the test"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ffscheck import Disk
from mkffs import Volume

CYL = 504 * 1024


def make(root, card, flop, out):
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(90000))
    open(os.path.join(out, "big.bin"), "wb").write(big)

    def save(name, v):
        p = os.path.join(out, name)
        open(p, "wb").write(v.image())
        return p

    v = Volume(2 * CYL // 512, "Svensk", True, 3)
    v.add_file("räksmörgås.txt", b"svenska-fran-linux\n")
    v.add_file("Mapp/big.bin", big)
    h = v.add_file("Mapp/Under/text.txt", b"in-a-subdirectory\n")
    v.add_file("gone.txt", b"to-be-removed\n")
    v.hard_link("hardname", h)
    v.hard_link("Mapp/Under/hard", h)
    v.soft_link("Mapp/up", b"/gone.txt")
    a = save("a.img", v)
    v = Volume(CYL // 512, "Gammal", False, 0)
    v.add_file("ofs.txt", b"old-file-system\n")
    v.add_file("big.bin", big)
    b = save("b.img", v)
    v = Volume(CYL // 1024, "Ettk", True, 3, 1024)
    v.add_file("onek.txt", b"one-kb-blocks\n")
    v.add_file("big.bin", big)
    c = save("c.img", v)
    v = Volume(CYL // 512, "Ogiltig", True, 3)
    v.add_file("invalid.txt", b"bitmap-marked-invalid\n")
    d = bytearray(v.image())
    r = v.root * 512
    struct.pack_into(">I", d, r + 512 - 200, 0)
    struct.pack_into(">I", d, r + 20, 0)
    struct.pack_into(">I", d, r + 20, -sum(struct.unpack_from(">128I", d, r)) & 0xFFFFFFFF)
    e = os.path.join(out, "d.img")
    open(e, "wb").write(d)
    rdb = os.path.join(out, "rdb.img")
    subprocess.run([sys.executable, os.path.join(HERE, "mkrdb-multi.py"), rdb, f"{a}:ffs3:DH0", f"{b}:ofs:DH1", f"{c}:ffs3k:DH2", f"{e}:ffs3:DH3"],
                   check=True, stdout=subprocess.DEVNULL)
    r0, p2 = open(root, "rb").read(), open(rdb, "rb").read()
    s1 = 2048
    s2 = (s1 + len(r0) // 512 + 2047) // 2048 * 2048
    total = (s2 + len(p2) // 512 + 2047) // 2048 * 2048
    mbr = bytearray(512)
    for i, (act, t, s, n) in enumerate(((0x80, 0x83, s1, len(r0) // 512), (0, 0x76, s2, len(p2) // 512))):
        mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", act, b"\xfe\xff\xff", t, b"\xfe\xff\xff", s, n)
    mbr[510:512] = b"\x55\xaa"
    img = bytearray(total * 512)
    img[:512] = mbr
    img[s1 * 512:s1 * 512 + len(r0)] = r0
    img[s2 * 512:s2 * 512 + len(p2)] = p2
    open(card, "wb").write(img)
    # the first partition inside the RDB starts at its cylinder 1 (mkrdb-multi): the test cuts the volumes out of the card
    open(os.path.join(out, "layout"), "w").write(f"{s2 + 16 * 63} {CYL}\n")
    v = Volume(1760, "Flop", False, 1)
    v.add_file("flop.txt", b"on-the-floppy\n")
    open(flop, "wb").write(v.image())


def judge(out, lszx):
    ls = open(lszx, "rb").read()
    bad = []

    def want(d, path, data):
        if d.read(path.encode("latin-1")) != data:
            bad.append(f"{path}: wrong")

    a = Disk(os.path.join(out, "p3.img")).walk()
    want(a, "/Nya/långt namn på svenska.txt", b"svenska\n")
    want(a, "/Nya/moved.txt", b"hello-from-zenix\n")
    want(a, "/ls.bin", ls)
    want(a, "/timer.txt", b"by-the-timer\n")
    want(a, "/after.txt", b"after-restart\n")
    if b"/Mapp/Under/TEXT.TXT" not in a.files:
        bad.append("the case change")
    if b"/gone.txt" in a.files or b"/new.txt" in a.files:
        bad.append("a removed name is there")
    b = Disk(os.path.join(out, "p4.img")).walk()
    want(b, "/flyttad.txt", b"old-file-system\n")
    want(b, "/ny.txt", b"ofs-written\n")
    want(b, "/ls.bin", ls)
    if b"/big.bin" in b.files:
        bad.append("OFS: big.bin is still there")
    c = Disk(os.path.join(out, "p5.img"), 1024).walk()
    want(c, "/ls.bin", ls)
    f = Disk(os.path.join(out, "df1.adf")).walk()
    want(f, "/skriven.txt", b"written-on-floppy\n")
    want(f, "/flop.txt", b"on-the-floppy\n")
    for n, d in (("DOS3", a), ("OFS", b), ("1 KB", c), ("floppy", f)):
        bad += [f"{n}: {w}" for w in d.bad]
        if not d.walked:
            bad.append(f"{n}: not walked")
        if d.leak:
            print(f"{n}: {d.leak} lost blocks (a server killed leaves some; the next -o rw gives them back)")
    print("\n".join(bad))
    return 1 if bad else 0


if sys.argv[1] == "--judge":
    sys.exit(judge(sys.argv[2], sys.argv[3]))
make(*sys.argv[1:5])
