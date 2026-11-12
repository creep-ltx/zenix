#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Writes files and directories into an exFAT volume that mkfs.exfat (exfatprogs) made, from Microsoft's exFAT
specification (2019): the PC has no user-space exFAT writer and the tests run without root, so they cannot mount it.
Written for the tests of DESIGN D47 (tools/exfat-host-test.py, tools/exfat-test.sh); the PC's fsck.exfat checks the
result. What it can make: contiguous files (NoFatChain) and FAT-chained ones, fragmented on purpose; directories of
either kind; a valid data length below the size (the rest of the clusters filled with 0xAA, which a reader must show as
zeros); read-only files; times with a UTC offset; extra benign entries (a vendor extension in a set, a GUID entry in the
root); deleted sets; a name the path cannot carry (opts "rawname"); clusters taken from a place asked for (opts "at",
a cluster number: files 32 GB apart on a big volume); a file's clusters taken in the bitmap without its data written (a sparse 5 GB file).
usage: tools/exfat-img.py IMAGE SRCDIR [--chained]    (write SRCDIR's tree into the volume IMAGE, at its byte 0)
       or import it: build(image, items, offset=0); items: (path, data or None for a directory, {options})"""
import os, struct, sys

def _sum16(b, skip=(2, 3), s=0):
    for i, x in enumerate(b):
        if i in skip:
            continue
        s = (((s & 1) << 15) | (s >> 1)) + x & 0xFFFF
    return s


class Volume:
    def __init__(self, path, offset=0):
        self.f = open(path, "r+b")
        self.base = offset
        b = self.rd(0, 512)
        assert b[3:11] == b"EXFAT   ", "not exFAT"
        self.ss = 1 << b[108]
        self.cb = 1 << (b[108] + b[109])
        self.fat = struct.unpack_from("<I", b, 80)[0] * self.ss
        self.heap = struct.unpack_from("<I", b, 88)[0] * self.ss
        self.nclus = struct.unpack_from("<I", b, 92)[0]
        self.root = struct.unpack_from("<I", b, 96)[0]
        self.root_chain = self.chain(self.root)
        rootd = b"".join(self.rd(self.cl(c), self.cb) for c in self.root_chain)
        self.up = list(range(65536))
        for o in range(0, len(rootd), 32):
            t = rootd[o]
            if t == 0x81:
                self.bm_first, self.bm_len = struct.unpack_from("<IQ", rootd, o + 20)
            elif t == 0x82:
                f, n = struct.unpack_from("<IQ", rootd, o + 20)
                self._upcase(b"".join(self.rd(self.cl(c), self.cb) for c in self.chain(f))[:n])
        self.bitmap = bytearray(b"".join(self.rd(self.cl(c), self.cb) for c in self.chain(self.bm_first))[:self.bm_len])
        self.hint = 2

    def rd(self, at, n):
        self.f.seek(self.base + at)
        return self.f.read(n)

    def wr(self, at, data):
        self.f.seek(self.base + at)
        self.f.write(data)

    def cl(self, c):
        return self.heap + (c - 2) * self.cb

    def fatget(self, c):
        return struct.unpack("<I", self.rd(self.fat + c * 4, 4))[0]

    def fatset(self, c, v):
        self.wr(self.fat + c * 4, struct.pack("<I", v))

    def chain(self, c):
        out = [c]
        while True:
            n = self.fatget(c)
            if n >= 0xFFFFFFF7:
                return out
            out.append(n)
            c = n

    def _upcase(self, t):
        """the up-case table (spec 7.2.5), compressed or not"""
        i = ch = 0
        while i + 1 < len(t) and ch < 65536:
            v = struct.unpack_from("<H", t, i)[0]
            i += 2
            if v == 0xFFFF and i + 1 < len(t):
                ch += struct.unpack_from("<H", t, i)[0]
                i += 2
                continue
            self.up[ch] = v
            ch += 1

    def used(self, c):
        return self.bitmap[(c - 2) >> 3] >> ((c - 2) & 7) & 1

    def take(self, c):
        self.bitmap[(c - 2) >> 3] |= 1 << ((c - 2) & 7)

    def alloc(self, n, contiguous=True):
        """N clusters: one run, or scattered (every few clusters one left out for a later file, so the chain is not one
        run but has runs of 1-3 in it)"""
        if n == 0:
            return []
        if contiguous:
            c = self.hint
            while c + n - 1 <= self.nclus + 1:
                if all(not self.used(x) for x in range(c, c + n)):
                    for x in range(c, c + n):
                        self.take(x)
                    self.hint = c + n
                    return list(range(c, c + n))
                c += 1
            raise SystemExit("exfat-img: no room for %d clusters in a row" % n)
        out, c, k = [], self.hint, 0
        while len(out) < n:
            if c > self.nclus + 1:
                raise SystemExit("exfat-img: full")
            if not self.used(c):
                k += 1
                if k % 4 != 0:                      # every fourth free one skipped
                    self.take(c)
                    out.append(c)
            c += 1
        self.hint = c
        return out

    def link(self, clusters):
        for a, b in zip(clusters, clusters[1:]):
            self.fatset(a, b)
        if clusters:
            self.fatset(clusters[-1], 0xFFFFFFFF)

    def put(self, clusters, data):
        for i, c in enumerate(clusters):
            piece = data[i * self.cb:(i + 1) * self.cb]
            if piece:
                self.wr(self.cl(c), piece)

    def hash(self, name):
        h = 0
        units = struct.unpack("<%dH" % (len(name.encode("utf-16-le")) // 2), name.encode("utf-16-le"))
        for u in units:
            u = self.up[u]
            for x in (u & 0xFF, u >> 8):
                h = (((h & 1) << 15) | (h >> 1)) + x & 0xFFFF
        return h

    def close(self):
        """the bitmap back, and the boot sector's PercentInUse (left out of the boot checksum, spec 3.1.18)"""
        chain = self.chain(self.bm_first)
        for i, c in enumerate(chain):
            self.wr(self.cl(c), bytes(self.bitmap[i * self.cb:(i + 1) * self.cb]))
        usedn = sum(bin(b).count("1") for b in self.bitmap)
        self.wr(112, bytes([min(100, usedn * 100 // self.nclus)]))
        self.f.close()


def ts(t, utc_q):
    """Unix time T as an exFAT timestamp (spec 7.4.8) in the local time of UTC + UTC_Q x 15 minutes"""
    import time
    g = time.gmtime(t + utc_q * 900)
    return ((g.tm_year - 1980) << 25 | g.tm_mon << 21 | g.tm_mday << 16 | g.tm_hour << 11 | g.tm_min << 5 | g.tm_sec // 2,
            (g.tm_sec & 1) * 100)


def entry_set(v, name, attr, first, length, vdl, nofat, mtime, utc_q=4, vendor=False):
    """the File entry, its Stream Extension, its File Name entries (spec 7.4, 7.6, 7.7) and, with VENDOR, a vendor
    extension entry (7.8): its checksum (6.3.3) over all of them"""
    u = name.encode("utf-16-le")
    n = len(u) // 2
    assert 1 <= n <= 255
    names = [u[i:i + 30].ljust(30, b"\0") for i in range(0, len(u), 30)]
    sec = 1 + len(names) + (1 if vendor else 0)
    t, ms = ts(mtime, utc_q)
    tc, _ = ts(mtime - 1000, utc_q)                # created before, accessed after: a reader taking the wrong one shows
    ta, _ = ts(mtime + 1000, utc_q)
    off = (utc_q & 0x7F) | 0x80
    f = bytearray(32)
    f[0] = 0x85; f[1] = sec
    struct.pack_into("<H", f, 4, attr)
    struct.pack_into("<III", f, 8, tc, t, ta)
    f[20] = ms; f[21] = ms; f[22] = f[23] = f[24] = off
    s = bytearray(32)
    s[0] = 0xC0; s[1] = 1 | (2 if nofat else 0); s[3] = n
    struct.pack_into("<H", s, 4, v.hash(name))
    struct.pack_into("<Q", s, 8, vdl)
    struct.pack_into("<IQ", s, 20, first, length)
    out = bytes(f) + bytes(s) + b"".join(b"\xC1\0" + p for p in names)
    if vendor:
        out += b"\xE0\0" + bytes(range(1, 17)) + bytes(14)
    cs = _sum16(out)
    return out[:2] + struct.pack("<H", cs) + out[4:]


class Node:
    def __init__(self, name, data=None, opts=None):
        self.name, self.data, self.opts, self.kids = name, data, opts or {}, []


def build(img, items, offset=0):
    """write ITEMS into the volume at byte OFFSET of IMG; returns the Volume's numbers for the tests"""
    v = Volume(img, offset)
    root = Node("")
    nodes = {"": root}
    for path, data, opts in items:
        parts = path.strip("/").split("/")
        for i in range(1, len(parts) + 1):
            p = "/".join(parts[:i])
            if p not in nodes:
                last = i == len(parts)
                nodes[p] = Node(parts[i - 1], data if last else None, opts if last else {})
                nodes["/".join(parts[:i - 1])].kids.append(nodes[p])
    def place(node, is_root):
        """the node's own clusters (a file's data, a directory's entries), children first"""
        for k in node.kids:
            if k.data is None:
                place(k, False)
            else:
                o = k.opts
                n = (len(k.data) + v.cb - 1) // v.cb if "size" not in o else (o["size"] + v.cb - 1) // v.cb
                k.nofat = o.get("nofat", True)
                if "at" in o:
                    v.hint = o["at"]
                k.clusters = v.alloc(n, contiguous=k.nofat or not o.get("fragment", False))
                if not k.nofat:
                    v.link(k.clusters)
                if "size" not in o:
                    vdl = o.get("vdl", len(k.data))
                    v.put(k.clusters, k.data[:vdl] + b"\xAA" * (len(k.clusters) * v.cb - vdl))
                k.length = o.get("size", len(k.data))
                k.vdl = o.get("vdl", k.length)
        sets = []
        for k in node.kids:
            if k.data is None:
                attr = 0x10
            else:
                attr = 0x20 | (1 if k.opts.get("ro") else 0)
            first = k.clusters[0] if k.clusters else 0
            sets.append(entry_set(v, k.opts.get("rawname", k.name), attr, first, k.length, k.vdl, k.nofat and bool(k.clusters),
                                  k.opts.get("mtime", 1700000000), k.opts.get("utc", 4), k.opts.get("vendor", False)))
            if k.opts.get("deleted_before"):         # a deleted set before it: its types with InUse cleared
                d = bytearray(entry_set(v, "gone-" + k.name, 0x20, 0, 0, 0, False, 1700000000))
                for i in range(0, len(d), 32):
                    d[i] &= 0x7F
                sets.insert(len(sets) - 1, bytes(d))
        body = b"".join(sets)
        if is_root:
            rootd = b"".join(v.rd(v.cl(c), v.cb) for c in v.root_chain)
            used = 0
            while used < len(rootd) and rootd[used]:
                used += 32
            body = rootd[:used] + body
            need = max(1, (len(body) + 32 + v.cb - 1) // v.cb)
            if need > len(v.root_chain):
                extra = v.alloc(need - len(v.root_chain), contiguous=False)
                v.root_chain += extra
                v.link(v.root_chain)
            v.put(v.root_chain, body.ljust(len(v.root_chain) * v.cb, b"\0"))
            return
        o = node.opts
        node.nofat = o.get("nofat", True)
        if "at" in o:
            v.hint = o["at"]
        n = max(1, (len(body) + v.cb - 1) // v.cb + o.get("spare", 0))
        node.clusters = v.alloc(n, contiguous=node.nofat or not o.get("fragment", False))
        if not node.nofat:
            v.link(node.clusters)
        v.put(node.clusters, body.ljust(n * v.cb, b"\0"))
        node.length = node.vdl = n * v.cb
    place(root, True)
    out = {"cb": v.cb, "nclus": v.nclus, "heap": v.heap, "fat": v.fat, "root": v.root_chain[:], "ss": v.ss}
    v.close()
    return out


def from_dir(src, chained=False):
    items = []
    for d, subs, files in os.walk(src):
        rel = os.path.relpath(d, src)
        rel = "" if rel == "." else rel + "/"
        for s in sorted(subs):
            items.append((rel + s, None, {"nofat": not chained, "fragment": chained}))
        for f in sorted(files):
            st = os.stat(os.path.join(d, f))
            items.append((rel + f, open(os.path.join(d, f), "rb").read(),
                          {"nofat": not chained, "fragment": chained, "mtime": int(st.st_mtime)}))
    return items


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    build(sys.argv[1], from_dir(sys.argv[2], "--chained" in sys.argv))
