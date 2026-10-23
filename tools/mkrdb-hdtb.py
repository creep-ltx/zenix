#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""An RDB disk laid out the way HDToolBox (or another Amiga tool) writes one, to test that fdisk keeps it exactly.
usage: mkrdb-hdtb.py make OUT.hdf [badsum]     16 MB disk: RDSK geometry 4 heads x 32 sectors (128 per cylinder),
           RDB area of 2 cylinders; DH0 (PFS\\3, cyl 2-65, RDSK geometry), DH1 (DOS\\3, cyl 100-149, its OWN geometry
           1 x 128), DH2 (DOS\\1, cyl 20-29 in 16 x 63 geometry = sectors 20160-30239); a FileSysHeader for PFS\\3 with
           a 3-block LSEG chain (blocks 3-6); DH2's PART block at 7. "badsum": DH1's checksum is wrong.
       mkrdb-hdtb.py check BEFORE.hdf AFTER.hdf    after fdisk add/add/del/boot: prints what changed, exit 0 if only
           what fdisk may change did (see tools/disk-host-test.sh); new partitions must be labelled ZX..."""
import struct
import sys

SEC = 512


def block(longs):
    longs = list(longs) + [0] * (128 - len(longs))
    longs[2] = 0
    longs[2] = (-sum(longs[:longs[1]])) & 0xFFFFFFFF
    return struct.pack(">128L", *longs)


def bstr(s):
    b = bytes([len(s)]) + s.encode() + b"\0" * (31 - len(s))
    return list(struct.unpack(">8L", b))


def part(nxt, name, flags, surfaces, bpt, lo, hi, dostype, maxtr=0x1FE00):
    p = [0x50415254, 64, 0, 7, nxt, flags, 0xFFFFFFFF, 0xFFFFFFFF, 0] + bstr(name) + [0] * 15
    env = [16, 128, 0, surfaces, 1, bpt, 2, 0, 0, lo, hi, 30, 0, maxtr, 0x7FFFFFFE, 0, dostype]
    return p + env


def make(out, badsum):
    ncyl = 256
    rdsk = [0x5244534B, 64, 0, 7, 512, 0x17, 0xFFFFFFFF, 1, 3, 0xFFFFFFFF] + [0xFFFFFFFF] * 6
    rdsk += [ncyl, 32, 4, 1, ncyl, 0, 0, 0, ncyl, ncyl, 3, 0, 0, 0, 0, 0]
    rdsk += [0, 255, 2, ncyl - 1, 128, 0, 7, 0]
    rdsk += list(struct.unpack(">8L", b"ZENIX   TEST HDTOOLBOX DISK     "))
    blocks = {0: block(rdsk)}
    blocks[1] = block(part(2, "DH0", 1, 4, 32, 2, 65, 0x50465303))
    b1 = bytearray(block(part(7, "DH1", 0, 1, 128, 100, 149, 0x444F5303)))
    if badsum:
        b1[11] ^= 1
    blocks[2] = bytes(b1)
    blocks[7] = block(part(0xFFFFFFFF, "DH2", 0, 16, 63, 20, 29, 0x444F5301, 0xFFFFFF))
    fshd = [0x46534844, 64, 0, 7, 0xFFFFFFFF, 0, 0, 0, 0x50465303, 0x00130002, 0x180, 0, 0, 0, 0, 0x1000, 0, 0, 4, 0xFFFFFFFF]
    blocks[3] = block(fshd)
    for i, b in enumerate((4, 5, 6)):
        data = [(0x9E3779B1 * (b * 131 + k)) & 0xFFFFFFFF for k in range(123)]
        blocks[b] = block([0x4C534547, 128, 0, 7, b + 1 if b < 6 else 0xFFFFFFFF] + data)
    img = bytearray(16 * 1024 * 1024)
    for n, d in blocks.items():
        img[n * SEC:(n + 1) * SEC] = d
    open(out, "wb").write(img)


def longs(img, n):
    return list(struct.unpack(">128L", img[n * SEC:(n + 1) * SEC]))


def sum_ok(lg):
    return lg[1] <= 128 and sum(lg[:lg[1]]) & 0xFFFFFFFF == 0


def parts(img):
    r = longs(img, 0)
    out, n = [], r[7]
    while n != 0xFFFFFFFF and len(out) < 40:
        p = longs(img, n)
        env = p[32:]
        nm = img[n * SEC + 36:n * SEC + 36 + 32]
        name = nm[1:1 + nm[0]].decode()
        cyl = env[3] * env[5] * env[1] * 4 // SEC
        out.append((n, name, env[9] * cyl, (env[10] - env[9] + 1) * cyl, p[5] & 1, sum_ok(p)))
        n = p[4]
    return out


def check(before, after):
    a, b = open(before, "rb").read(), open(after, "rb").read()
    ok = True

    def bad(msg):
        nonlocal ok
        ok = False
        print("CHANGED:", msg)
    for n in (3, 4, 5, 6):
        if a[n * SEC:(n + 1) * SEC] != b[n * SEC:(n + 1) * SEC]:
            bad(f"FileSysHeader/LSEG block {n}")
    ra, rb = longs(a, 0), longs(b, 0)
    if not sum_ok(rb):
        bad("RDSK checksum wrong")
    for i in range(128):
        if i not in (2, 38) and ra[i] != rb[i]:
            bad(f"RDSK long {i}")
    pa = {p[1]: p for p in parts(a)}
    pb = parts(b)
    names = [p[1] for p in pb]
    print("chain after:", " ".join(f"{p[1]}@{p[0]}:{p[2]}+{p[3]}{'*' if p[4] else ''}" for p in pb))
    for p in pb:
        if not p[5]:
            bad(f"{p[1]} checksum wrong")
    for nm in ("DH0", "DH1", "DH2"):
        if nm not in names:
            bad(f"{nm} is gone")
            continue
        old, new = pa[nm], [p for p in pb if p[1] == nm][0]
        if old[0] != new[0] or old[2] != new[2] or old[3] != new[3]:
            bad(f"{nm} moved: {old} -> {new}")
        la, lb = longs(a, old[0]), longs(b, new[0])
        for i in range(128):
            if i in (2, 4) or (i == 5 and (la[i] ^ lb[i]) == 1):
                continue
            if la[i] != lb[i]:
                bad(f"{nm} PART long {i}: {la[i]:#x} -> {lb[i]:#x}")
    for i, p in enumerate(pb):
        if p[2] < 256 or p[2] + p[3] > len(b) // SEC:
            bad(f"{p[1]} outside the disk or in the RDB area")
        for q in pb[i + 1:]:
            if p[2] < q[2] + q[3] and q[2] < p[2] + p[3]:
                bad(f"{p[1]} overlaps {q[1]}")
    for n in range(256):
        if n in (0, 3, 4, 5, 6) or n in [p[0] for p in pb] or n in [p[0] for p in parts(a)]:
            continue
        if a[n * SEC:(n + 1) * SEC] != b[n * SEC:(n + 1) * SEC]:
            if b[n * SEC:n * SEC + 4] == b"PART" and b[n * SEC + 37:n * SEC + 39] == b"ZX":
                continue                            # a PART block fdisk wrote for a test partition deleted again
            bad(f"RDB-area block {n} that no chain owns")
    if a[256 * SEC:] != b[256 * SEC:]:
        bad("data outside the RDB area")
    print("check:", "ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2], len(sys.argv) > 3 and sys.argv[3] == "badsum")
    else:
        sys.exit(check(sys.argv[2], sys.argv[3]))
