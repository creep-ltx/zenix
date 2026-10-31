#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The card of tools/pfs3-test.sh, as the owner's: an MBR, the ext2 root, a type 0x76 partition with an RDB holding PFS3
volumes. usage: pfs3-test-images.py ROOT CARD OUTDIR LS.ZX"""
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
from mkpfs3 import Volume

CYL = 504 * 1024


def main():
    root, card, out, prog = sys.argv[1:5]
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(900000))
    open(os.path.join(out, "big.bin"), "wb").write(big)

    def save(name, v):
        p = os.path.join(out, name)
        open(p, "wb").write(v.image())
        return p

    v = Volume(8 * CYL // 512, "Svensk")
    v.add_file("räksmörgås.txt", b"svenska-fran-amigaos\n", comment=b"en kommentar")
    v.add_file("Mapp/big.bin", big, pieces=40)
    v.add_file("Mapp/Under/text.txt", b"in-a-subdirectory\n")
    v.add_file("ls.bin", open(prog, "rb").read())
    v.add_file("Mapp/skyddad", b"protected\n", prot=4)      # the w bit set: refused
    v.hard_link("hardname", "Mapp/Under/text.txt")
    v.hard_link("hardmapp", "Mapp/Under")
    v.soft_link("Mapp/up", "/hardname")
    v.soft_link("fromroot", ":Mapp/Under/text.txt")
    v.soft_link("volroot", "Svensk:Mapp/Under/text.txt")
    v.soft_link("annan", "Annan:nowhere")
    for i in range(400):
        v.add_file(f"Lista/en-fil-med-ganska-langt-namn-{i:03}", b"x")
    a = save("a.img", v)
    v = Volume(4 * CYL // 512, "Stor", super_=True)
    v.add_file("super.txt", b"with-super-blocks\n")
    v.add_file("big.bin", big)
    b = save("b.img", v)
    # damaged: a file whose chain of anodes loops, a directory whose block is not one; the rest reads
    v = Volume(2 * CYL // 512, "Trasig")
    v.add_file("ok.txt", b"still-good\n")
    n1 = v.add_file("loop.bin", b"y" * 20000, pieces=4)
    v.add_file("Mapp/x", b"z\n")
    d = bytearray(v.image())
    # the chain's second anode names the first again: a loop
    ab = next(b for b in v.blocks if b > 2 and v.blocks[b][:2] == b"AB")
    second = n1.an
    cl, blk, nxt = v.anodes[n1.an]
    off = nxt & 0xFFFF
    struct.pack_into(">I", d, ab * 512 + 16 + 12 * off + 8, n1.an)
    dirblk = v.anodes[v.find("Mapp").an][1]
    d[dirblk * 512:dirblk * 512 + 2] = b"XX"
    c = os.path.join(out, "c.img")
    open(c, "wb").write(d)
    rdb = os.path.join(out, "rdb.img")
    subprocess.run([sys.executable, os.path.join(HERE, "mkrdb-multi.py"), rdb, f"{a}:pfs3:DH0", f"{b}:pfs3k4:DH1", f"{c}:pds3:DH2", "504K:ffs3:DH3"],
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
    # the PFS3 volumes, as the test compares them afterwards: nothing may have changed
    open(os.path.join(out, "layout"), "w").write(f"{s2 + 16 * 63} {8 * CYL} {4 * CYL} {2 * CYL}\n")


main()
