#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The tests' own judge of an AmigaDOS volume (tools/ffs-host-test.py, tools/ffs-test.sh): the class Disk walks an image as the
format says and collects what it finds wrong.
usage: ffscheck.py IMAGE [BLOCKSIZE]    prints the files and what is wrong; exit status 1 if anything is"""
import struct
import sys


class Disk:
    """an image, read as the format says: the test's own judge. Everything found wrong goes into self.bad"""
    def __init__(self, path_or_bytes, bs=512, reserved=2):
        self.d = open(path_or_bytes, "rb").read() if isinstance(path_or_bytes, str) else path_or_bytes
        self.bs, self.res = bs, reserved
        self.longs = bs // 4
        self.ht = self.longs - 56
        self.n = len(self.d) // bs
        self.root = (reserved + self.n - 1) // 2
        self.dos = self.d[3]
        self.ofs = not self.dos & 1
        self.intl = self.dos >= 2
        self.ln = self.dos >= 6
        self.bad = []
        self.named = {}                                    # block -> what names it
        self.files = {}                                    # path (Latin-1 bytes) -> data block list, size, header
        self.leak = 0
        self.walked = False

    def L(self, b, i):
        return struct.unpack_from(">I", self.d, b * self.bs + 4 * i)[0]

    def good(self, b):
        return 0 <= b < self.n and sum(struct.unpack_from(f">{self.longs}I", self.d, b * self.bs)) & 0xFFFFFFFF == 0

    def name(self, b):
        p = b * self.bs + self.bs - (184 if self.ln and b != self.root else 80)
        return self.d[p + 1:p + 1 + self.d[p]]

    def hash(self, n):
        h = len(n)
        for c in n:
            if 97 <= c <= 122 or (self.intl and 224 <= c <= 254 and c != 247):
                c -= 32
            h = (h * 13 + c) & 0x7FF
        return h % self.ht

    def name_it(self, b, what):
        if b in self.named:
            self.bad.append(f"block {b} named twice: {self.named[b]}, {what}")
        elif not self.res <= b < self.n:
            self.bad.append(f"block {b} out of range: {what}")
        self.named[b] = what

    def table(self, b):
        return [self.L(b, 6 + self.ht - 1 - i) for i in range(self.L(b, 2))]

    def walk_file(self, h, path):
        size = self.L(h, self.longs - 47)
        blocks = self.table(h)
        c, steps = self.L(h, self.longs - 2), 0
        while c:
            self.name_it(c, f"extension of {path}")
            if not self.good(c) or self.L(c, 0) != 16 or self.L(c, 1) != c or self.L(c, self.longs - 3) != h:
                self.bad.append(f"bad extension {c} of {path}")
                break
            blocks += self.table(c)
            c = self.L(c, self.longs - 2)
            steps += 1
            if steps > self.n:
                self.bad.append(f"extension loop {path}")
                break
        pay = self.bs - 24 if self.ofs else self.bs
        if len(blocks) < (size + pay - 1) // pay:
            self.bad.append(f"{path}: {len(blocks)} blocks for {size} bytes")
        for b in blocks:
            self.name_it(b, f"data of {path}")
        self.files[path] = (blocks, size, h)

    def walk_dir(self, d, prefix, depth=0):
        if depth > 40:
            self.bad.append("too deep")
            return
        for s in range(self.ht):
            e, prev = self.L(d, 6 + s), 0
            while e:
                if not self.res <= e < self.n or not self.good(e) or self.L(e, 0) != 2 or self.L(e, 1) != e:
                    self.bad.append(f"bad header {e} under {prefix}")
                    break
                self.name_it(e, f"header in {prefix}")
                n = self.name(e)
                path = prefix + b"/" + n
                if self.L(e, self.longs - 3) != d:
                    self.bad.append(f"{path}: parent")
                if self.hash(n) != s:
                    self.bad.append(f"{path}: in chain {s}, its hash is {self.hash(n)}")
                if e <= prev:
                    self.bad.append(f"{path}: the chain is not in block order")
                sec = self.L(e, self.longs - 1)
                if sec == 2:
                    self.walk_dir(e, path, depth + 1)
                elif sec == 0xFFFFFFFD:
                    self.walk_file(e, path)
                elif sec == 3:
                    self.files[path] = ([], None, e)
                elif sec in (4, 0xFFFFFFFC):
                    self.files[path] = (None, None, e)
                else:
                    self.bad.append(f"{path}: secondary type {sec:#x}")
                prev, e = e, self.L(e, self.longs - 4)
                if len(self.named) > self.n:
                    self.bad.append("a loop")
                    return

    def bit_free(self, b):
        per = 32 * (self.longs - 1)
        k = b - self.res
        pg = self.pages[k // per]
        return self.L(pg, 1 + (k % per) // 32) >> (k % 32) & 1

    def walk(self):
        """the whole volume, the bitmap against it"""
        r = self.root
        if not self.good(r) or self.L(r, 0) != 2 or self.L(r, self.longs - 1) != 1:
            self.bad.append("the root")
            return self
        if self.L(r, self.longs - 50) != 0xFFFFFFFF:
            self.bad.append("the bitmap flag was cleared")
        self.name_it(r, "root")
        self.pages = [self.L(r, self.longs - 49 + i) for i in range(25)]
        per = 32 * (self.longs - 1)
        nbm = (self.n - self.res + per - 1) // per
        e = self.L(r, self.longs - 24)
        while e:
            self.name_it(e, "bitmap extension")
            self.pages += [self.L(e, i) for i in range(self.longs - 1)]
            e = self.L(e, self.longs - 1)
        self.pages = self.pages[:nbm]
        for p in self.pages:
            self.name_it(p, "bitmap")
            if not self.good(p):
                self.bad.append(f"bitmap block {p}: checksum")
        self.walk_dir(r, b"")
        if self.bad:
            return self
        for b in range(self.res, self.n):
            f = self.bit_free(b)
            if b in self.named and f:
                self.bad.append(f"block {b} ({self.named[b]}) is marked free")
            elif b not in self.named and not f:
                self.leak += 1
        self.walked = True
        return self

    def read(self, path):
        """a file's bytes, or None"""
        if path not in self.files or not self.files[path][0] and self.files[path][1] is None:
            return None
        blocks, size, _ = self.files[path]
        if size is None:
            return None
        if self.ofs:
            out = b""
            for i, b in enumerate(blocks):
                if len(out) >= size:
                    break
                if not self.good(b) or self.L(b, 0) != 8 or self.L(b, 2) != i + 1:
                    return None
                n = self.L(b, 3)
                out += self.d[b * self.bs + 24:b * self.bs + 24 + n]
            return out[:size]
        return b"".join(self.d[b * self.bs:(b + 1) * self.bs] for b in blocks)[:size]


if __name__ == "__main__":
    d = Disk(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 512).walk()
    for k in sorted(d.files):
        print(k.decode("latin-1"), d.files[k][1])
    print(f"lost blocks {d.leak}")
    for w in d.bad:
        print("BAD", w)
    sys.exit(1 if d.bad else 0)
