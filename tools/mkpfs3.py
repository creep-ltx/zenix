#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""A small maker of PFS3 volumes for pfs3d's tests (DESIGN D32), written from the description of the format in
docs/d32-pfs3.md. It holds what a reader needs: the boot block, the root and its extension, super, index and anode blocks,
directories, file data. It makes no allocation bitmaps: its volumes are for reading only, and AmigaOS's handler must not
write to them.
usage as a library: v = Volume(sectors, name, bs=512, rb=1024, super_=False, split=True, large=False); v.add_file(path, data);
v.mkdir(path); v.soft_link(path, text); v.hard_link(path, target); v.rollover(path, data, roll, vsize); v.image() -> bytes."""
import struct

OPT = dict(harddisk=1, split=2, direxr=4, deldir=8, size=16, ext=32, date=64, super=128, superdel=256, xrov=512, longfn=1024, large=2048, geom=4096)
T_DIR, T_SOFT, T_LDIR, T_FILE, T_LFILE, T_ROLL = 2, 3, 4, 0xFD, 0xFC, 0xF0
DAY = (17000, 615, 1000)                                   # a date that says something: 2024-07-18 10:15:20


class Node:
    def __init__(self, name, kind, parent):
        self.name, self.kind, self.parent = name, kind, parent
        self.children = []
        self.an = 0
        self.prot = 0
        self.comment = b""
        self.date = DAY
        self.data = b""
        self.pieces = 1
        self.size = None
        self.sizehi = 0
        self.link = 0                                      # a file's head of links, or a link's object
        self.target = None
        self.roll = self.vsize = 0
        self.nolink = None


class Volume:
    def __init__(self, sectors, name="Pfstest", bs=512, rb=1024, super_=False, split=True, large=False, typ=None, reserved=128):
        self.bs, self.rb, self.sup, self.split, self.large = bs, rb, super_, split, large
        self.spb = bs // 512
        self.nblocks = sectors // self.spb
        self.name = name.encode("latin-1")
        self.rc = rb // bs
        self.typ = typ if typ is not None else (0x50465302 if rb > 1024 or bs > 512 or large else 0x50465301)
        self.res_first = 2
        self.res_last = self.res_first + self.rc * reserved - 1
        self.apb, self.ipb = (rb - 16) // 12, (rb - 12) // 4
        self.root = Node(b"", T_DIR, None)
        self.anodes = {}
        self.next_an = 6
        self.blocks = {}                                   # block number -> bytes
        self.next_res = self.res_first + self.rc * 2       # the root's cluster, then the extension
        self.next_data = self.res_last + 1

    # ---- the tree ----
    def _walk(self, path, make=True):
        parts = path.strip("/").split("/")
        cur = self.root
        for p in parts[:-1]:
            nm = p.encode("latin-1")
            nx = next((c for c in cur.children if c.name == nm), None)
            if nx is None:
                if not make:
                    raise KeyError(path)
                nx = Node(nm, T_DIR, cur)
                cur.children.append(nx)
            cur = nx
        return cur, parts[-1].encode("latin-1")

    def _add(self, path, kind):
        d, nm = self._walk(path)
        n = Node(nm, kind, d)
        d.children.append(n)
        return n

    def find(self, path):
        d, nm = self._walk(path, False)
        return next(c for c in d.children if c.name == nm)

    def mkdir(self, path):
        return self._add(path, T_DIR)

    def add_file(self, path, data, prot=0, comment=b"", date=DAY, pieces=1):
        n = self._add(path, T_FILE)
        n.data, n.prot, n.comment, n.date, n.pieces = data, prot, comment, date, pieces
        return n

    def soft_link(self, path, text):
        n = self._add(path, T_SOFT)
        n.data = text if isinstance(text, bytes) else text.encode("latin-1")
        return n

    def hard_link(self, path, target):
        t = self.find(target)
        n = self._add(path, T_LDIR if t.kind == T_DIR else T_LFILE)
        n.target = t
        return n

    def rollover(self, path, data, roll, vsize):
        n = self._add(path, T_ROLL)
        n.data, n.roll, n.vsize = data, roll, vsize
        return n

    # ---- blocks ----
    def raw_an(self, an):
        return (an // self.apb) << 16 | an % self.apb if self.split else an

    def _anode(self, cluster, block, nxt=0, an=None):
        if an is None:
            an = self.next_an
            self.next_an += 1
        self.anodes[an] = (cluster, block, nxt)
        return an

    def _res(self):
        b = self.next_res
        self.next_res += self.rc
        if b + self.rc - 1 > self.res_last:
            raise ValueError("the reserved area is full")
        return b

    def _put(self, block, data):
        self.blocks[block] = data

    def _store_data(self, n):
        """a file's data blocks, in PIECES runs with gaps between; its chain of anodes"""
        data = n.data
        bn = (len(data) + self.bs - 1) // self.bs
        n.size = len(data) if n.size is None else n.size
        if not bn:
            self._anode(0, 0, 0, n.an)
            return
        per = max(1, bn // n.pieces + (1 if bn % n.pieces else 0))
        at, an, first = 0, n.an, True
        while at < bn:
            k = min(per, bn - at)
            start = self.next_data
            if start + k > self.nblocks:
                raise ValueError("the volume is full")
            self.next_data = start + k + (1 if n.pieces > 1 else 0)
            for i in range(k):
                self._put(start + i, data[(at + i) * self.bs:(at + i + 1) * self.bs].ljust(self.bs, b"\0"))
            at += k
            nxt = self.next_an if at < bn else 0
            if at < bn:
                self.next_an += 1
            self._anode(k, start, self.raw_an(nxt) if nxt else 0, an)
            an = nxt

    def _entry(self, n):
        name = n.name
        size = n.size if n.size is not None else 0
        if n.kind == T_DIR:
            size = 0
        if n.kind in (T_LFILE, T_LDIR):
            size = 0
        words = [0] * 11
        link = n.link
        if link:
            words[0], words[1] = link >> 16, link & 0xFFFF
        if n.kind == T_ROLL:
            words[6], words[7] = n.vsize >> 16, n.vsize & 0xFFFF
            words[8], words[9] = n.roll >> 16, n.roll & 0xFFFF
        if n.sizehi:
            words[10] = n.sizehi
        flags, extra = 0, b""
        for i, w in enumerate(words):
            if w:
                flags |= 1 << i
                extra = struct.pack(">H", w) + extra
        body = struct.pack(">BBIIHHHBB", 0, n.kind, self.raw_an(n.an), size & 0xFFFFFFFF, *n.date, n.prot, len(name)) + name + bytes([len(n.comment)]) + n.comment
        if len(body) & 1:
            body += b"\0"
        total = len(body) + len(extra) + 2
        return bytes([total]) + body[1:] + extra + struct.pack(">H", flags)

    def _store_dir(self, d):
        """a directory's entries into blocks, each with an anode of its own"""
        ents = [self._entry(c) for c in d.children]
        blocks, cur = [], b""
        for e in ents:
            if 20 + len(cur) + len(e) > self.rb:
                blocks.append(cur)
                cur = b""
            cur += e
        blocks.append(cur)
        parent = 0 if d.parent is None else self.raw_an(d.parent.an)
        an = d.an
        for i, body in enumerate(blocks):
            b = self._res()
            nxt = self.next_an if i + 1 < len(blocks) else 0
            if nxt:
                self.next_an += 1
            self._anode(1, b, self.raw_an(nxt) if nxt else 0, an)
            head = struct.pack(">HHIHHII", 0x4442, 0, 1, 0, 0, self.raw_an(d.an), parent)
            self._put(b, (head + body).ljust(self.rb, b"\0"))
            an = nxt

    def _number(self, n):
        for c in n.children:
            c.an = self.next_an
            self.next_an += 1
            if c.kind == T_DIR:
                self._number(c)

    def _links(self, n):
        for c in n.children:
            if c.kind in (T_LFILE, T_LDIR):
                t = c.target
                self._anode(self.raw_an(t.parent.an), self.raw_an(n.an), self.raw_an(t.link_head) if getattr(t, "link_head", 0) else 0, c.an)
                t.link_head = c.an
                c.link = self.raw_an(t.an)
            elif c.kind == T_DIR:
                self._links(c)

    def _heads(self, n):
        for c in n.children:
            if getattr(c, "link_head", 0):
                c.link = self.raw_an(c.link_head)
            if c.kind == T_DIR:
                self._heads(c)

    def _store(self, n):
        for c in n.children:
            if c.kind == T_DIR:
                self._store(c)
            elif c.kind in (T_FILE, T_SOFT, T_ROLL):
                self._store_data(c)
        self._store_dir(n)

    def build(self):
        self.root.an = 5
        self._number(self.root)
        self._links(self.root)
        self._heads(self.root)
        self._store(self.root)
        # anode blocks, index blocks, super blocks
        by_seq = {}
        for an, val in self.anodes.items():
            by_seq.setdefault(an // self.apb, {})[an % self.apb] = val
        by_ix = {}
        for seq, nodes in sorted(by_seq.items()):
            b = self._res()
            d = bytearray(self.rb)
            d[:16] = struct.pack(">HHIII", 0x4142, 0, 1, seq, 0)
            for off, v in nodes.items():
                d[16 + 12 * off:28 + 12 * off] = struct.pack(">III", *v)
            self._put(b, bytes(d))
            by_ix.setdefault(seq // self.ipb, {})[seq % self.ipb] = b
        index = {}
        for ix, ents in sorted(by_ix.items()):
            b = self._res()
            d = bytearray(self.rb)
            d[:12] = struct.pack(">HHII", 0x4942, 0, 1, ix)
            for off, v in ents.items():
                d[12 + 4 * off:16 + 4 * off] = struct.pack(">I", v)
            self._put(b, bytes(d))
            index[ix] = b
        supers = {}
        if self.sup:
            by_sp = {}
            for ix, b in index.items():
                by_sp.setdefault(ix // self.ipb, {})[ix % self.ipb] = b
            for sp, ents in sorted(by_sp.items()):
                b = self._res()
                d = bytearray(self.rb)
                d[:12] = struct.pack(">HHII", 0x5342, 0, 1, sp)
                for off, v in ents.items():
                    d[12 + 4 * off:16 + 4 * off] = struct.pack(">I", v)
                self._put(b, bytes(d))
                supers[sp] = b
        # the extension
        ext = self.res_first + self.rc
        d = bytearray(self.rb)
        d[:16] = struct.pack(">HHIII", 0x4558, 0, 0, 1, 19 << 16)
        d[16:22] = struct.pack(">HHH", *DAY)
        d[56:58] = struct.pack(">H", 108)
        for sp, b in supers.items():
            d[64 + 4 * sp:68 + 4 * sp] = struct.pack(">I", b)
        self._put(ext, bytes(d))
        # the root
        opts = OPT["harddisk"] | OPT["direxr"] | OPT["size"] | OPT["ext"] | OPT["date"] | OPT["longfn"]
        if self.split:
            opts |= OPT["split"]
        if self.sup:
            opts |= OPT["super"]
        if self.large:
            opts |= OPT["large"]
        r = bytearray(max(self.rb, 512))
        r[:96] = struct.pack(">IIIHHHH", self.typ, opts, 1, *DAY, 0xF0) + bytes([len(self.name)]) + self.name.ljust(31, b"\0") + \
            struct.pack(">IIIHHIIIIIII", self.res_last, self.res_first, 0, self.rb, self.rc, self.nblocks - self.next_data, 0, 0, 0, self.nblocks * self.spb, ext, 0)
        if not self.sup:
            for ix, b in index.items():
                r[116 + 4 * ix:120 + 4 * ix] = struct.pack(">I", b)
        self._put(2, bytes(r))
        self._put(0, struct.pack(">I", self.typ).ljust(self.bs, b"\0"))

    def image(self):
        self.build()
        out = bytearray(self.nblocks * self.bs)
        for b, d in self.blocks.items():
            out[b * self.bs:b * self.bs + len(d)] = d
        return bytes(out)
