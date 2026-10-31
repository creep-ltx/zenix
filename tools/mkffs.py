#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a small AmigaDOS volume (OFS or FFS, DOS\\0 .. DOS\\3) from Linux, without root and without amitools: the boot
partition zenixboot starts from (S/Startup-Sequence, zenixboot, the kernel), and the test images of tools/ffs-host-test.py,
which uses the class Volume for what a command line does not say (links, protection bits).

usage: mkffs.py out.img SIZE NAME [--intl] [--dos N] [--bs BYTES] [--reserved N] HOSTFILE=AMIGA/PATH ...
SIZE in bytes (K/M suffixes allowed), a multiple of 512. --intl makes DOS\\3 (international names, what HDToolBox
and Kickstart's own format pick for FFS), else DOS\\1; --dos N says any of 0..3. --bs a block size over 512, --reserved
other than 2 reserved blocks. Directories in AMIGA/PATH are made as needed. Names may hold any Latin-1 letters.

Layout (the AmigaDOS disk format, as in the ADF FAQ and amitools; 512-byte blocks, 2 reserved boot blocks):
  block 0      'DOS' + type, the rest zero (a partition is not booted by its boot block)
  root         block (reserved + blocks - 1) / 2: type 2, hash table of (longs - 56), bitmap valid flag -1, bitmap pointers
  bitmap       after the root; bit set = free, block reserved + 32*long + bit (bit 0 = the lowest); past 25 bitmap blocks
               extension blocks (longs - 1 pointers and the next one)
  then each directory header, file header, extension blocks (as many data pointers as the hash table) and the data
  blocks (FFS: raw data; OFS: type 8, header, sequence, size, next, checksum, data). Hash chains in ascending block order.
  Checksums: the long at +20 (headers, OFS data) or +0 (bitmap) makes the sum of the block zero.
The result is checked by Kickstart in the FS-UAE test (tools/zenixboot-test.sh) and, where amitools is installed,
by its xdftool there; this tool itself needs only Python.
"""
import os
import struct
import sys
import time

T_HEADER, T_DATA, T_LIST = 2, 8, 16
ST_ROOT, ST_USERDIR, ST_SOFT, ST_LDIR, ST_FILE, ST_LFILE = 1, 2, 3, 4, -3 & 0xFFFFFFFF, -4 & 0xFFFFFFFF


def size_arg(s):
    mult = {"K": 1024, "M": 1 << 20}.get(s[-1:].upper(), 1)
    return int(s[:-1] if mult > 1 else s) * mult


def upper(c, intl):
    if 97 <= c <= 122 or (intl and 224 <= c <= 254 and c != 247):
        return c - 32
    return c


def amiga_date(t):
    days, rest = divmod(int(t) - 252460800, 86400)       # 1978-01-01 00:00 UTC
    return days, rest // 60, (rest % 60) * 50


class Volume:
    def __init__(self, nblocks, name, intl, dos=None, bs=512, reserved=2):
        self.bs, self.longs = bs, bs // 4
        self.ht = self.longs - 56                          # hash table size, data pointers per header/extension block
        self.n = nblocks
        self.dos = dos if dos is not None else 3 if intl else 1
        self.intl = self.dos >= 2
        self.ofs = not self.dos & 1
        self.reserved = reserved
        self.blocks = {}                                   # block number -> raw bytes
        self.used = set(range(reserved))
        self.root = (reserved + nblocks - 1) // 2
        self.used.add(self.root)
        per = 32 * (self.longs - 1)
        nbm = (nblocks - reserved + per - 1) // per
        self.bitmap = [self.root + 1 + i for i in range(nbm)]
        self.used.update(self.bitmap)
        self.next = self.root + 1 + nbm
        self.bmext = [self.alloc() for _ in range((max(nbm - 25, 0) + self.longs - 2) // (self.longs - 1))]
        self.date = amiga_date(int(os.environ.get("SOURCE_DATE_EPOCH", time.time())))
        self.dirs = {"": self.root}                       # path -> header block
        self.hash = {self.root: {}}                       # directory -> hash slot -> blocks of its chain
        self.name = name.encode("latin-1")
        self.entries = {}                                 # block -> header longs (filled at the end)

    def alloc(self):
        while self.next in self.used:
            self.next += 1
        if self.next >= self.n:
            sys.exit("mkffs: the volume is full")
        b = self.next
        self.used.add(b)
        return b

    def name_hash(self, name):
        h = len(name)
        for c in name:
            h = (h * 13 + upper(c, self.intl)) & 0x7FF
        return h % self.ht

    def link(self, parent, block, name):
        """the block joins its parent's chain; the chain gets its order at the end"""
        self.hash[parent].setdefault(self.name_hash(name), []).append(block)

    def header(self, block, parent, name, sec_type):
        if not name or len(name) > 30:
            sys.exit(f"mkffs: bad name {name!r} (1-30 characters)")
        L = [0] * self.longs
        L[0] = T_HEADER
        L[1] = block
        nb = bytes([len(name)]) + name
        for i, v in enumerate(struct.unpack(">8I", nb.ljust(32, b"\0"))):
            L[self.longs - 20 + i] = v                     # name: a BSTR at byte size - 80
        L[self.longs - 23:self.longs - 20] = self.date     # date at byte size - 92
        L[self.longs - 3] = parent                         # parent at byte size - 12
        L[self.longs - 1] = sec_type
        self.link(parent, block, name)
        return L

    def mkdir(self, path):
        if path in self.dirs:
            return self.dirs[path]
        parent = self.mkdir(os.path.dirname(path))
        name = os.path.basename(path).encode("latin-1")
        b = self.alloc()
        L = self.header(b, parent, name, ST_USERDIR)
        self.entries[b] = L
        self.hash[b] = {}
        self.dirs[path] = b
        return b

    def add_file(self, path, data, protect=0):
        parent = self.mkdir(os.path.dirname(path))
        name = os.path.basename(path).encode("latin-1")
        hdr = self.alloc()
        pay = self.bs - 24 if self.ofs else self.bs
        nblk = (len(data) + pay - 1) // pay
        data_blocks = [self.alloc() for _ in range(nblk)]
        for i, b in enumerate(data_blocks):
            part = data[i * pay:(i + 1) * pay]
            if self.ofs:
                nxt = data_blocks[i + 1] if i + 1 < nblk else 0
                D = struct.pack(">6I", T_DATA, hdr, i + 1, len(part), nxt, 0) + part.ljust(pay, b"\0")
                sum0 = sum(struct.unpack(f">{self.longs}I", D))
                D = D[:20] + struct.pack(">I", -sum0 & 0xFFFFFFFF) + D[24:]
                self.blocks[b] = D
            else:
                self.blocks[b] = part.ljust(self.bs, b"\0")
        L = self.header(hdr, parent, name, ST_FILE)
        L[self.longs - 48] = protect                       # protection at byte size - 192 (0 = rwed for everyone)
        L[self.longs - 47] = len(data)                     # byte size at byte size - 188
        ht = self.ht
        groups = [data_blocks[i:i + ht] for i in range(0, max(nblk, 1), ht)] if nblk else [[]]
        L[2] = len(groups[0])                              # high_seq: pointers in this block
        L[4] = data_blocks[0] if nblk else 0               # first data block
        for i, b in enumerate(groups[0]):
            L[6 + ht - 1 - i] = b                          # pointers from the end of the table backwards
        prev = L
        for g in groups[1:]:
            e = self.alloc()
            X = [0] * self.longs
            X[0] = T_LIST
            X[1] = e
            X[2] = len(g)
            for i, b in enumerate(g):
                X[6 + ht - 1 - i] = b
            X[self.longs - 3] = hdr                        # parent: the file header
            X[self.longs - 1] = ST_FILE
            prev[self.longs - 2] = e                       # extension at byte size - 8
            self.entries[e] = X
            prev = X
        self.entries[hdr] = L
        return hdr

    def soft_link(self, path, target):
        """target: bytes, an Amiga path, as it is on the disk"""
        parent = self.mkdir(os.path.dirname(path))
        b = self.alloc()
        L = self.header(b, parent, os.path.basename(path).encode("latin-1"), ST_SOFT)
        words = (self.bs - 224) // 4
        L[6:6 + words] = struct.unpack(f">{words}I", target.ljust(words * 4, b"\0"))
        self.entries[b] = L
        return b

    def hard_link(self, path, target_block):
        """a link to a file or directory header already made, at the head of its list"""
        parent = self.mkdir(os.path.dirname(path))
        b = self.alloc()
        T = self.entries[target_block]
        sec = ST_LFILE if T[self.longs - 1] == ST_FILE else ST_LDIR
        L = self.header(b, parent, os.path.basename(path).encode("latin-1"), sec)
        L[self.longs - 11] = target_block                  # the target at byte size - 44
        L[self.longs - 10] = T[self.longs - 10]            # the list through the file's header, at byte size - 40
        T[self.longs - 10] = b
        self.entries[b] = L
        return b

    def image(self):
        out = bytearray(self.n * self.bs)
        out[0:4] = b"DOS" + bytes([self.dos])
        R = [0] * self.longs
        R[0] = T_HEADER
        R[3] = self.ht
        R[self.longs - 50] = 0xFFFFFFFF                    # bitmap valid
        R[self.longs - 49:self.longs - 49 + min(len(self.bitmap), 25)] = self.bitmap[:25]
        if self.bmext:
            R[self.longs - 24] = self.bmext[0]
        R[self.longs - 23:self.longs - 20] = self.date     # root altered
        nb = bytes([len(self.name)]) + self.name
        for i, v in enumerate(struct.unpack(">8I", nb.ljust(32, b"\0"))):
            R[self.longs - 20 + i] = v
        R[self.longs - 10:self.longs - 7] = self.date      # volume altered (byte size - 40)
        R[self.longs - 7:self.longs - 4] = self.date       # created (byte size - 28)
        R[self.longs - 1] = ST_ROOT
        self.entries[self.root] = R
        for d, slots in self.hash.items():                 # directories: their tables; chains in ascending block order
            for h, chain in slots.items():
                chain = sorted(chain)
                self.entries[d][6 + h] = chain[0]
                for a, b in zip(chain, chain[1:]):
                    self.entries[a][self.longs - 4] = b
        for b, L in self.entries.items():
            L[5] = 0
            L[5] = (-sum(L)) & 0xFFFFFFFF
            out[b * self.bs:(b + 1) * self.bs] = struct.pack(f">{self.longs}I", *L)
        for b, data in self.blocks.items():
            out[b * self.bs:(b + 1) * self.bs] = data
        per = 32 * (self.longs - 1)
        for k, bb in enumerate(self.bitmap):               # free = 1; blocks past the end stay 0 (used)
            M = [0] * self.longs
            for j in range(per):
                blk = self.reserved + k * per + j
                if blk < self.n and blk not in self.used:
                    M[1 + j // 32] |= 1 << (j % 32)
            M[0] = (-sum(M)) & 0xFFFFFFFF
            out[bb * self.bs:(bb + 1) * self.bs] = struct.pack(f">{self.longs}I", *M)
        rest = self.bitmap[25:]
        for i, e in enumerate(self.bmext):                 # the bitmap's extension blocks: no checksum
            X = [0] * self.longs
            part = rest[i * (self.longs - 1):(i + 1) * (self.longs - 1)]
            X[:len(part)] = part
            if i + 1 < len(self.bmext):
                X[self.longs - 1] = self.bmext[i + 1]
            out[e * self.bs:(e + 1) * self.bs] = struct.pack(f">{self.longs}I", *X)
        return bytes(out)


def main():
    a = sys.argv[1:]
    opts = {"--dos": None, "--bs": "512", "--reserved": "2"}
    for k in opts:
        if k in a:
            i = a.index(k)
            opts[k] = a[i + 1]
            del a[i:i + 2]
    intl = "--intl" in a
    a = [x for x in a if x != "--intl"]
    if len(a) < 3:
        sys.exit(__doc__)
    out, size, name = a[0], size_arg(a[1]), a[2]
    bs = int(opts["--bs"])
    if size % bs or size < 64 * 512:
        sys.exit("mkffs: SIZE must be a multiple of the block size and at least 32 KB")
    v = Volume(size // bs, name, intl, None if opts["--dos"] is None else int(opts["--dos"]), bs, int(opts["--reserved"]))
    for spec in a[3:]:
        host, _, path = spec.partition("=")
        v.add_file(path, open(host, "rb").read())
    open(out, "wb").write(v.image())
    print(f"{out}: FFS volume {name} (DOS{v.dos}), {size // 1024} KB, {len(a) - 3} files, "
          f"{len(v.used)} blocks used")


if __name__ == "__main__":
    main()
