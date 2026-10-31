#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Read an AmigaDOS FFS volume from Linux (the boot partition zenixboot wrote to, for tools/update-test.sh), and
put a partition of an RDB disk image in and out. Only what the tests need: DOS\\1 / DOS\\3, hash chains,
extension blocks; every checksum and pointer is checked.

usage: ffsread.py IMAGE [--part NAME] ls              every file: "PATH SIZE" (directories end in /)
       ffsread.py IMAGE [--part NAME] get PATH OUT     one file
       ffsread.py IMAGE [--part NAME] extract DIR      every file into DIR
       ffsread.py IMAGE [--part NAME] rootflag         the root block's bitmap flag (-1: valid, 0: needs validating)
       ffsread.py IMAGE [--part NAME] check            the whole volume (headers, chains, checksums, every block used
                                                       once, the bitmap): "ok, N leaked blocks", or each problem
       ffsread.py IMAGE --part NAME dump OUT           the partition's bytes
       ffsread.py IMAGE --part NAME load IN            write IN (the partition's size) back into the partition
IMAGE is an FFS volume, or a disk with an RDB (then --part names the partition, e.g. ZB0)."""
import os
import struct
import sys

BS = 512


def die(m):
    sys.exit("ffsread: " + m)


def part_range(img, name):
    """(offset, size) of partition NAME in an RDB image"""
    for b in range(16):
        if img[b * BS:b * BS + 4] == b"RDSK":
            break
    else:
        die("no RDB in the first 16 blocks")
    nxt = struct.unpack(">I", img[b * BS + 28:b * BS + 32])[0]
    seen = 0
    while nxt != 0xFFFFFFFF and seen < 64:
        p = img[nxt * BS:(nxt + 1) * BS]
        if p[:4] != b"PART":
            die("a broken partition list")
        L = struct.unpack(">128I", p)
        n = p[36 + 1:36 + 1 + p[36]].decode("latin-1")
        heads, secs, low, high = L[32 + 3], L[32 + 5], L[32 + 9], L[32 + 10]
        if n == name:
            return low * heads * secs * BS, (high - low + 1) * heads * secs * BS
        nxt = L[4]
        seen += 1
    die("no partition " + name)


class Vol:
    def __init__(self, data):
        self.d = data
        if data[:3] != b"DOS" or data[3] not in (1, 3):
            die("not an FFS volume (DOS\\1 or DOS\\3)")
        self.intl = data[3] == 3
        self.n = len(data) // BS
        self.root = (2 + self.n - 1) // 2

    def blk(self, b, header=True):
        if not 2 <= b < self.n:
            die("block %d out of range" % b)
        raw = self.d[b * BS:(b + 1) * BS]
        L = struct.unpack(">128I", raw)
        if header and sum(L) & 0xFFFFFFFF:
            die("block %d: checksum wrong" % b)
        return L, raw

    def name(self, raw):
        return raw[432 + 1:432 + 1 + raw[432]].decode("latin-1")

    def walk(self, d=None, path=""):
        d = self.root if d is None else d
        L, _ = self.blk(d)
        for h in L[6:6 + 72]:
            seen = 0
            while h and seen < 1000:
                E, raw = self.blk(h)
                if E[0] != 2 or E[1] != h:
                    die("block %d: not a header" % h)
                nm = path + self.name(raw)
                st = E[127]
                if st == 2:
                    yield nm + "/", 0, h
                    yield from self.walk(h, nm + "/")
                elif st == 0xFFFFFFFD:
                    yield nm, E[81], h
                h = E[124]
                seen += 1

    def read(self, hdr):
        L, _ = self.blk(hdr)
        size = L[81]
        out = bytearray()
        cur = L
        hops = 0
        while True:
            for i in range(cur[2]):
                out += self.blk(cur[77 - i], header=False)[1]
            if not cur[126]:
                break
            cur, _ = self.blk(cur[126])
            hops += 1
            if hops > 100000:
                die("an extension chain without an end")
        if len(out) < size:
            die("file shorter than its header says")
        return bytes(out[:size])


def upper_c(c, intl):
    return c - 32 if (97 <= c <= 122 or (intl and 224 <= c <= 254 and c != 247)) else c


def ffs_hash(name, intl):
    h = len(name)
    for c in name.encode("latin-1"):
        h = (h * 13 + upper_c(c, intl)) & 0x7FF
    return h % 72


class _Chk:
    def __init__(self, data):
        self.d, self.intl = data, data[3] == 3
        self.n = len(data) // BS

    def L(self, b, i):
        return struct.unpack(">I", self.d[b * BS + 4 * i:b * BS + 4 * i + 4])[0]

    def sum_ok(self, b):
        return sum(struct.unpack(">128I", self.d[b * BS:(b + 1) * BS])) & 0xFFFFFFFF == 0

    def check(self):
        prob, used, files = [], {}, {}

        def use(b, what):
            if not 2 <= b < self.n:
                prob.append("%s: block %d outside" % (what, b))
            elif b in used:
                prob.append("%s: block %d also %s" % (what, b, used[b]))
            else:
                used[b] = what

        root = (2 + self.n - 1) // 2
        if self.L(root, 0) != 2 or self.L(root, 127) != 1 or not self.sum_ok(root) or self.L(root, 78) != 0xFFFFFFFF:
            prob.append("the root block")
        use(root, "root")
        nbm = (self.n - 2 + 32 * 127 - 1) // (32 * 127)
        bms = [self.L(root, 79 + i) for i in range(nbm)]
        for b in bms:
            use(b, "bitmap")
            if not self.sum_ok(b):
                prob.append("bitmap block %d checksum" % b)

        def walk(d, path, depth):
            if depth > 10:
                prob.append("too deep")
                return
            for h in range(72):
                b, steps, last = self.L(d, 6 + h), 0, 0
                while b and steps < 10000:
                    steps += 1
                    nm = bytes(self.d[b * BS + 433:b * BS + 433 + self.d[b * BS + 432]]).decode("latin-1")
                    use(b, path + nm)
                    if self.L(b, 0) != 2 or self.L(b, 1) != b or not self.sum_ok(b) or self.L(b, 125) != d:
                        prob.append("%s%s: a bad header" % (path, nm))
                        break
                    if ffs_hash(nm, self.intl) != h:
                        prob.append("%s%s: in the wrong hash chain" % (path, nm))
                    st = self.L(b, 127)
                    if st == 2:
                        walk(b, path + nm + "/", depth + 1)
                    elif st == 0xFFFFFFFD:
                        size = self.L(b, 81)
                        want = (size + BS - 1) // BS
                        ptrs, x, first = [], b, True
                        while x:
                            if not first:
                                use(x, path + nm + " ext")
                                if self.L(x, 0) != 16 or self.L(x, 1) != x or not self.sum_ok(x) or self.L(x, 125) != b:
                                    prob.append("%s%s: a bad extension block" % (path, nm))
                                    break
                            hs = self.L(x, 2)
                            ptrs += [self.L(x, 77 - i) for i in range(min(hs, 72))]
                            x, first = self.L(x, 126), False
                        if len(ptrs) != want or (want and self.L(b, 4) != ptrs[0]):
                            prob.append("%s%s: %d pointers for %d bytes" % (path, nm, len(ptrs), size))
                        for q in ptrs:
                            use(q, path + nm + " data")
                        files[path + nm] = (b, b"".join(self.d[q * BS:(q + 1) * BS] for q in ptrs)[:size])
                    last = b
                    b = self.L(b, 124)
        walk(root, "", 0)
        leaks = 0
        for blk in range(2, self.n):
            k = blk - 2
            bm = bms[k // (32 * 127)]
            free = self.L(bm, 1 + (k % (32 * 127)) // 32) >> (k % 32) & 1
            if blk in used and free:
                prob.append("block %d (%s) in use but free in the bitmap" % (blk, used[blk]))
            if blk not in used and not free:
                leaks += 1
        return prob, leaks, files


def check_volume(data):
    """the whole volume: every header, hash chain, extension block and checksum, every block used once, the blocks in
    use against the bitmap. -> (problems, leaks: blocks marked in use that nothing names, {path: (header, bytes)})"""
    return _Chk(data).check()


def main(a):
    if len(a) < 2:
        die(__doc__)
    path = a.pop(0)
    img = bytearray(open(path, "rb").read())
    off, size = 0, len(img)
    if a[0] == "--part":
        off, size = part_range(img, a[1])
        a = a[2:]
    cmd = a[0]
    if cmd == "dump":
        open(a[1], "wb").write(img[off:off + size])
        return
    if cmd == "load":
        data = open(a[1], "rb").read()
        if len(data) != size:
            die("%s is %d bytes, the partition %d" % (a[1], len(data), size))
        with open(path, "r+b") as f:
            f.seek(off)
            f.write(data)
        return
    v = Vol(bytes(img[off:off + size]))
    if cmd == "ls":
        for n, s, _ in v.walk():
            print(n if n.endswith("/") else "%s %d" % (n, s))
    elif cmd == "check":
        prob, leaks, _ = check_volume(bytes(img[off:off + size]))
        for x in prob:
            print("problem: " + x)
        print("ok, %d leaked blocks" % leaks if not prob else "damaged")
        sys.exit(1 if prob else 0)
    elif cmd == "rootflag":
        L, _ = v.blk(v.root)
        print(struct.unpack(">i", struct.pack(">I", L[78]))[0])
    elif cmd in ("get", "extract"):
        found = False
        for n, s, h in v.walk():
            if n.endswith("/"):
                if cmd == "extract":
                    os.makedirs(os.path.join(a[1], n), exist_ok=True)
                continue
            if cmd == "get" and n.lower() == a[1].lower():
                open(a[2], "wb").write(v.read(h))
                found = True
            elif cmd == "extract":
                open(os.path.join(a[1], n), "wb").write(v.read(h))
        if cmd == "get" and not found:
            die("no file " + a[1])
    else:
        die(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
