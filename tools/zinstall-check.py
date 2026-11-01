#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""What tools/zinstall-test.sh asks of an image file (DESIGN D35), read here by a parser of its own.
  zinstall-check.py parts IMG                    the table: NAME START SECTORS TYPE FLAGS PRI, a line each
  zinstall-check.py sums IMG NAME...             the SHA-256 of named partitions (a slot number for a PC partition: slot1)
  zinstall-check.py cut IMG NAME OUT             a partition into a file
  zinstall-check.py bytes IMG LBA N              the SHA-256 of sectors
"""
import hashlib
import struct
import sys


def table(path):
    f = open(path, "rb")

    def sec(n):
        f.seek(n * 512)
        return f.read(512)

    def rdb_at(base):
        for i in range(16):
            b = sec(base + i)
            if b[:4] == b"RDSK":
                L = struct.unpack(">128I", b)
                out = []
                n = L[7]
                while n != 0xFFFFFFFF and len(out) < 40:
                    c = sec(base + n)
                    P = struct.unpack(">128I", c)
                    e = P[32:]
                    cyl = e[3] * e[5] * (e[1] // 128)
                    pri = e[15] if e[15] < 128 else e[15] - (1 << 32)
                    out.append((c[37:37 + c[36]].decode("latin-1"), base + e[9] * cyl, (e[10] - e[9] + 1) * cyl, "%08x" % e[16], P[5], pri))
                    n = P[4]
                return out
        return None
    r = rdb_at(0)
    if r is not None:
        return r
    m = sec(0)
    out = []
    if m[510:512] != b"\x55\xaa":
        return out
    for i in range(4):
        act, typ, st, n = struct.unpack("<B3xB3xII", m[446 + 16 * i:462 + 16 * i])
        if typ:
            out.append(("slot%d" % i, st, n, "%02x" % typ, act, 0))
            if typ == 0x76:
                out += rdb_at(st) or []
    return out


def sha(path, st, n):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(st * 512)
        while n:
            k = min(n, 4096)
            b = f.read(k * 512)
            h.update(b)
            n -= k
    return h.hexdigest()


def main():
    a = sys.argv[1:]
    t = table(a[1])
    if a[0] == "parts":
        for p in t:
            print(*p)
    elif a[0] == "sums":
        for n in a[2:]:
            p = [x for x in t if x[0] == n]
            print(n, sha(a[1], p[0][1], p[0][2]) if p else "missing")
    elif a[0] == "cut":
        p = [x for x in t if x[0] == a[2]][0]
        with open(a[1], "rb") as f, open(a[3], "wb") as g:
            f.seek(p[1] * 512)
            n = p[2]
            while n:
                k = min(n, 4096)
                g.write(f.read(k * 512))
                n -= k
    elif a[0] == "bytes":
        print(sha(a[1], int(a[2]), int(a[3])))


main()
