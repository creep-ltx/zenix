#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Write a small AmigaDOS Fast File System volume (FFS, DOS\\1 or DOS\\3) from Linux, without root and without
amitools: the boot partition zenixboot starts from (S/Startup-Sequence, zenixboot, the kernel).

usage: mkffs.py out.img SIZE NAME [--intl] HOSTFILE=AMIGA/PATH ...
SIZE in bytes (K/M suffixes allowed), a multiple of 512. --intl makes DOS\\3 (international names, what HDToolBox
and Kickstart's own format pick for FFS), else DOS\\1. Directories in AMIGA/PATH are made as needed.

Layout (the AmigaDOS disk format, as in the ADF FAQ and amitools; 512-byte blocks, 2 reserved boot blocks):
  block 0      'DOS' + type, the rest zero (a partition is not booted by its boot block)
  root         block (reserved + blocks - 1) / 2: type 2, hash table of 72, bitmap valid flag -1, bitmap pointers
  bitmap       after the root; bit set = free, block 2 + 32*long + bit (bit 0 = the lowest)
  then each directory header, file header, extension blocks (72 data pointers each) and the data blocks (FFS:
  raw data, no headers). Checksums: the long at +20 (headers) or +0 (bitmap) makes the sum of the block zero.
The result is checked by Kickstart in the FS-UAE test (tools/zenixboot-test.sh) and, where amitools is installed,
by its xdftool there; this tool itself needs only Python.
"""
import os
import struct
import sys
import time

BS = 512
LONGS = BS // 4
HT = 72                                   # hash table size, data pointers per header/extension block
RESERVED = 2
T_HEADER, T_LIST = 2, 16
ST_ROOT, ST_USERDIR, ST_FILE = 1, 2, -3 & 0xFFFFFFFF


def size_arg(s):
    mult = {"K": 1024, "M": 1 << 20}.get(s[-1:].upper(), 1)
    return int(s[:-1] if mult > 1 else s) * mult


def upper(c, intl):
    if 97 <= c <= 122 or (intl and 224 <= c <= 254 and c != 247):
        return c - 32
    return c


def name_hash(name, intl):
    h = len(name)
    for c in name:
        h = (h * 13 + upper(c, intl)) & 0x7FF
    return h % HT


def amiga_date(t):
    days, rest = divmod(int(t) - 252460800, 86400)       # 1978-01-01 00:00 UTC
    return days, rest // 60, (rest % 60) * 50


class Volume:
    def __init__(self, nblocks, name, intl):
        self.n = nblocks
        self.intl = intl
        self.blocks = {}                                   # block number -> list of 128 longs or raw bytes
        self.used = set(range(RESERVED))
        self.root = (RESERVED + nblocks - 1) // 2
        self.used.add(self.root)
        nbm = (nblocks - RESERVED + 32 * (LONGS - 1) - 1) // (32 * (LONGS - 1))
        if nbm > 25:
            sys.exit("mkffs: volume too big for a bitmap without extension blocks (25 bitmap blocks, about 50 MB)")
        self.bitmap = [self.root + 1 + i for i in range(nbm)]
        self.used.update(self.bitmap)
        self.next = self.root + 1 + nbm
        self.date = amiga_date(int(os.environ.get("SOURCE_DATE_EPOCH", time.time())))
        self.dirs = {"": self.root}                       # path -> header block
        self.hash = {self.root: [0] * HT}                 # directory -> its hash table (first block of each chain)
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

    def link(self, parent, block, name):
        """put header block into its parent's hash table (new entries go to the head of the chain)"""
        h = name_hash(name, self.intl)
        chain = self.hash[parent][h]
        self.hash[parent][h] = block
        return chain

    def header(self, block, parent, name, sec_type, chain):
        L = [0] * LONGS
        L[0] = T_HEADER
        L[1] = block
        nb = bytes([len(name)]) + name
        for i, v in enumerate(struct.unpack(">8I", nb.ljust(32, b"\0"))):
            L[108 + i] = v                                 # name: a BSTR at byte 432
        L[105:108] = self.date                             # date at byte 420
        L[124] = chain                                     # hash chain at byte 496
        L[125] = parent                                    # parent at byte 500
        L[127] = sec_type
        return L

    def mkdir(self, path):
        if path in self.dirs:
            return self.dirs[path]
        parent = self.mkdir(os.path.dirname(path))
        name = os.path.basename(path).encode("latin-1")
        b = self.alloc()
        L = self.header(b, parent, name, ST_USERDIR, self.link(parent, b, name))
        self.entries[b] = L
        self.hash[b] = [0] * HT
        self.dirs[path] = b
        return b

    def add_file(self, path, data, protect=0):
        parent = self.mkdir(os.path.dirname(path))
        name = os.path.basename(path).encode("latin-1")
        if not name or len(name) > 30:
            sys.exit(f"mkffs: bad file name {path!r} (1-30 characters)")
        hdr = self.alloc()
        nblk = (len(data) + BS - 1) // BS
        data_blocks = [self.alloc() for _ in range(nblk)]
        for i, b in enumerate(data_blocks):
            self.blocks[b] = data[i * BS:(i + 1) * BS].ljust(BS, b"\0")
        L = self.header(hdr, parent, name, ST_FILE, self.link(parent, hdr, name))
        L[80] = protect                                    # protection at byte 320 (0 = rwed for everyone)
        L[81] = len(data)                                  # byte size at byte 324
        groups = [data_blocks[i:i + HT] for i in range(0, max(nblk, 1), HT)] if nblk else [[]]
        L[2] = len(groups[0])                              # high_seq: pointers in this block
        L[4] = data_blocks[0] if nblk else 0               # first data block
        for i, b in enumerate(groups[0]):
            L[77 - i] = b                                  # pointers from the end of the table backwards
        prev = L
        for g in groups[1:]:
            e = self.alloc()
            X = [0] * LONGS
            X[0] = T_LIST
            X[1] = e
            X[2] = len(g)
            for i, b in enumerate(g):
                X[77 - i] = b
            X[125] = hdr                                   # parent: the file header
            X[127] = ST_FILE
            prev[126] = e                                  # extension at byte 504
            self.entries[e] = X
            prev = X
        self.entries[hdr] = L

    def image(self):
        out = bytearray(self.n * BS)
        out[0:4] = b"DOS" + bytes([3 if self.intl else 1])
        R = [0] * LONGS
        R[0] = T_HEADER
        R[3] = HT
        R[6:6 + HT] = self.hash[self.root]
        R[78] = 0xFFFFFFFF                                 # bitmap valid
        R[79:79 + len(self.bitmap)] = self.bitmap
        R[105:108] = self.date                             # root altered
        nb = bytes([len(self.name)]) + self.name
        for i, v in enumerate(struct.unpack(">8I", nb.ljust(32, b"\0"))):
            R[108 + i] = v
        R[118:121] = self.date                             # volume altered (byte 472)
        R[121:124] = self.date                             # created (byte 484)
        R[127] = ST_ROOT
        self.entries[self.root] = R
        for d, ht in self.hash.items():                    # directories: their final hash tables
            if d != self.root:
                self.entries[d][6:6 + HT] = ht
        for b, L in self.entries.items():
            L[5] = 0
            L[5] = (-sum(L)) & 0xFFFFFFFF
            out[b * BS:(b + 1) * BS] = struct.pack(">128I", *L)
        for b, data in self.blocks.items():
            out[b * BS:(b + 1) * BS] = data
        for k, bb in enumerate(self.bitmap):              # free = 1; blocks past the end stay 0 (used)
            M = [0] * LONGS
            for j in range(32 * (LONGS - 1)):
                blk = RESERVED + k * 32 * (LONGS - 1) + j
                if blk < self.n and blk not in self.used:
                    M[1 + j // 32] |= 1 << (j % 32)
            M[0] = (-sum(M)) & 0xFFFFFFFF
            out[bb * BS:(bb + 1) * BS] = struct.pack(">128I", *M)
        return bytes(out)


def main():
    a = sys.argv[1:]
    intl = "--intl" in a
    a = [x for x in a if x != "--intl"]
    if len(a) < 3:
        sys.exit(__doc__)
    out, size, name = a[0], size_arg(a[1]), a[2]
    if size % BS or size < 64 * BS:
        sys.exit("mkffs: SIZE must be a multiple of 512 and at least 32 KB")
    v = Volume(size // BS, name, intl)
    for spec in a[3:]:
        host, _, path = spec.partition("=")
        v.add_file(path, open(host, "rb").read())
    open(out, "wb").write(v.image())
    print(f"{out}: FFS volume {name} ({'DOS3' if intl else 'DOS1'}), {size // 1024} KB, {len(a) - 3} files, "
          f"{len(v.used)} blocks used")


main()
