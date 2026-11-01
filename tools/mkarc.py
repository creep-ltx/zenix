#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Small LZX and LhA archives for the tests (DESIGN D33, docs/d33-archives.md): written from that note's description of the formats, with
# a plain greedy matcher. Also damaged and hostile ones, which no real archiver would make. Not a tool for users: Zenix unpacks only.
#   tools/mkarc.py OUTDIR     writes the whole test set and a file `list` of what each should do
import heapq, os, struct, sys, zlib


def payload(name):
    """Test data, the same every time: text, repetitive bytes, noise."""
    if name == "text":
        return b"".join(b"line %d of some text with words, words and more words\n" % i for i in range(700))
    if name == "noise":
        x, out = 12345, bytearray()
        for _ in range(9000):
            x = (x * 1103515245 + 12345) & 0x7FFFFFFF
            out.append((x >> 16) & 255)
        return bytes(out)
    if name == "zeros":
        return bytes(70000)
    if name == "mixed":
        x, out = 777, bytearray()
        for k in range(30):
            alpha = bytes(((k * 7 + i * 13) & 255) for i in range(4 + k * 3))
            for _ in range(1500):
                x = (x * 1103515245 + 12345) & 0x7FFFFFFF
                out.append(alpha[(x >> 16) % len(alpha)])
            out += b"abcabcabc" * 40
        return bytes(out)
    raise ValueError(name)


def huff_lengths(freq, maxlen):
    """Code lengths for these counts, at most maxlen; the counts are flattened until they fit."""
    n = len(freq)
    freq = list(freq)
    while True:
        heap = [(f, i, None) for i, f in enumerate(freq) if f]
        heapq.heapify(heap)
        lens = [0] * n
        if len(heap) == 1:
            lens[heap[0][1]] = 1
            return lens
        if not heap:
            return lens
        c = n
        while len(heap) > 1:
            a = heapq.heappop(heap)
            b = heapq.heappop(heap)
            heapq.heappush(heap, (a[0] + b[0], c, (a, b)))
            c += 1
        stack = [(heap[0], 0)]
        while stack:
            (f, i, kids), d = stack.pop()
            if kids is None:
                lens[i] = d
            else:
                stack += [(kids[0], d + 1), (kids[1], d + 1)]
        if max(lens) <= maxlen:
            return lens
        freq = [(f + 1) // 2 if f else 0 for f in freq]


def canon(lens):
    """symbol -> (code, length): shorter first, by symbol."""
    out, code = {}, 0
    for l in range(1, 17):
        for s, x in enumerate(lens):
            if x == l:
                out[s] = (code, l)
                code += 1
        code <<= 1
    return out


def matches(data, maxlen, maxdist, start=0, minlen=3):
    """Tokens of a greedy parse: a byte, or (length, distance)."""
    table, i, out = {}, start, []
    n = len(data)
    while i < n:
        best, bd = 0, 0
        if i + minlen <= n:
            for p in table.get(data[i:i + minlen], [])[-16:]:
                if i - p > maxdist:
                    continue
                l = minlen
                while l < maxlen and i + l < n and data[p + l] == data[i + l]:
                    l += 1
                if l >= best:
                    best, bd = l, i - p
        if best >= minlen:
            out.append((best, bd))
            step = best
        else:
            out.append(data[i])
            step = 1
        for j in range(i, min(i + step, n - minlen + 1)):
            table.setdefault(data[j:j + minlen], []).append(j)
        i += step
    return out


# ---- LZX ----
class LzxBits:
    """16-bit big-endian words, filled from the low bit up."""
    def __init__(self):
        self.out = bytearray()
        self.w = 0
        self.n = 0

    def put(self, v, k):
        for i in range(k):
            self.w |= ((v >> i) & 1) << self.n
            self.n += 1
            if self.n == 16:
                self.out += struct.pack(">H", self.w)
                self.w, self.n = 0, 0

    def code(self, c, l):
        for i in range(l - 1, -1, -1):
            self.put((c >> i) & 1, 1)

    def done(self):
        if self.n:
            self.out += struct.pack(">H", self.w)
            self.w, self.n = 0, 0
        return bytes(self.out)


SLOT_BITS = [0, 0, 0, 0] + [(s - 2) // 2 for s in range(4, 32)]
SLOT_BASE = []
_x = 0
for _e in SLOT_BITS:
    SLOT_BASE.append(_x)
    _x += 1 << _e
LEN_BITS = [0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6]
LEN_BASE = []
_x = 3
for _e in LEN_BITS:
    LEN_BASE.append(_x)
    _x += 1 << _e


def slot_of(d):
    for s in range(31, 0, -1):
        if d >= SLOT_BASE[s]:
            return s
    return 1


def len_of(l):
    for h in range(15, -1, -1):
        if l >= LEN_BASE[h]:
            return h
    return 0


def lzx_half(b, new, old, second):
    """One half of the main table: changes and runs through a small table of its own."""
    n = len(new)
    r17, r18, r19 = (3, 19, 3) if second else (4, 20, 4)
    w18 = 6 if second else 5
    items, i = [], 0
    while i < n:
        d = (old[i] - new[i]) % 17
        if new[i] == 0:
            k = i
            while k < n and new[k] == 0:
                k += 1
            run = k - i
            if run >= r18:
                m = min(run, r18 + (1 << w18) - 1)
                items.append((18, m - r18, w18))
                i += m
                continue
            if run >= r17:
                m = min(run, r17 + 15)
                items.append((17, m - r17, 4))
                i += m
                continue
        k = i
        while k < n and (old[k] - new[k]) % 17 == d and old[k] == old[i]:
            k += 1
        if k - i >= r19 and new[i] != 0 and old[i] == 0:
            m = min(k - i, r19 + 1)
            items.append((19, m - r19, 1))
            items.append((d, 0, 0))
            i += m
            continue
        items.append((d, 0, 0))
        i += 1
    f = [0] * 20
    for s, _, _ in items:
        f[s] += 1
    pl = huff_lengths(f, 15)
    for x in pl:
        b.put(x, 4)
    pc = canon(pl)
    for s, v, w in items:
        b.code(*pc[s])
        if w:
            b.put(v, w)


def lzx_stream(data, block=20000, aligned=False):
    """A whole group's stream. aligned: type 3 blocks."""
    toks = matches(data, 258, 65535)
    b = LzxBits()
    old = [0] * 768
    pos = 0
    last = 1
    ti = 0
    while ti < len(toks):
        # a block: tokens making up to `block` bytes
        j, size = ti, 0
        while j < len(toks) and size < block:
            size += toks[j][0] if isinstance(toks[j], tuple) else 1
            j += 1
        sel = toks[ti:j]
        ty = 3 if aligned else 2
        syms, f, af, tl = [], [0] * 768, [0] * 8, last
        for t in sel:
            if isinstance(t, tuple):
                l, d = t
                h = len_of(l)
                if d == tl:
                    sl, ex = 0, 0
                else:
                    sl = slot_of(d)
                    ex = d - SLOT_BASE[sl]
                    tl = d
                s = 256 + (h << 5) + sl
                f[s] += 1
                if ty == 3 and SLOT_BITS[sl] >= 3:
                    af[ex & 7] += 1
                syms.append((s, sl, ex, h, l - LEN_BASE[h]))
            else:
                f[t] += 1
                syms.append((t,))
        lens = huff_lengths(f, 16)
        b.put(ty, 3)
        al = None
        if ty == 3:
            al = huff_lengths([x + 1 for x in af], 7)
            for x in al:
                b.put(x, 3)
        b.put(size >> 16, 8)
        b.put((size >> 8) & 255, 8)
        b.put(size & 255, 8)
        lzx_half(b, lens[:256], old[:256], False)
        lzx_half(b, lens[256:], old[256:], True)
        old = lens
        mc = canon(lens)
        ac = canon(al) if al else None
        for t in syms:
            b.code(*mc[t[0]])
            if len(t) > 1:
                _, sl, ex, h, lx = t
                e = SLOT_BITS[sl]
                if sl:
                    if ty == 3 and e >= 3:
                        b.put(ex >> 3, e - 3)
                        b.code(*ac[ex & 7])
                    elif e:
                        b.put(ex, e)
                if LEN_BITS[h]:
                    b.put(lx, LEN_BITS[h])
        last = tl
        ti = j
        pos += size
    return b.done()


def lzx_header(name, size, pack, mode, merged, data_crc, comment=b"", attr=0x0F, date=0x0DA821B9):
    h = bytearray(31)
    h[0] = attr
    h[2:6] = struct.pack("<I", size)
    h[6:10] = struct.pack("<I", pack)
    h[10] = 10
    h[11] = mode
    h[12] = 1 if merged else 0
    h[14] = len(comment)
    h[15] = 10
    h[18:22] = struct.pack(">I", date)
    h[22:26] = struct.pack("<I", data_crc)
    h[30] = len(name)
    crc = zlib.crc32(bytes(h) + name + comment)
    h[26:30] = struct.pack("<I", crc)
    return bytes(h) + name + comment


LZX_HEAD = b"LZX\0\x0c\0\x0a\x04\0\0"


def lzx_member(name, data, mode=2, aligned=False, **kw):
    """A member on its own: mode 0 stored, 2 normal."""
    if mode == 0:
        return lzx_header(name, len(data), len(data), 0, False, zlib.crc32(data), **kw) + data
    s = lzx_stream(data, aligned=aligned)
    return lzx_header(name, len(data), len(s), 2, False, zlib.crc32(data), **kw) + s


def lzx_group(members, aligned=False):
    """A merged group: every member's header, then one stream."""
    s = lzx_stream(b"".join(d for _, d in members), aligned=aligned)
    out = b""
    for i, (name, data) in enumerate(members):
        last = i == len(members) - 1
        out += lzx_header(name, len(data), len(s) if last else 0, 2, True, zlib.crc32(data))
    return out + s


# ---- LhA ----
class LhBits:
    """Most significant bit first."""
    def __init__(self):
        self.out = bytearray()
        self.w = 0
        self.n = 0

    def put(self, v, k):
        for i in range(k - 1, -1, -1):
            self.w = self.w << 1 | ((v >> i) & 1)
            self.n += 1
            if self.n == 8:
                self.out.append(self.w)
                self.w, self.n = 0, 0

    def done(self):
        while self.n:
            self.put(0, 1)
        return bytes(self.out)


def crc16(data):
    c = 0
    for x in data:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def lh_small(b, lens, nbits, skip):
    n = len(lens)
    while n and not lens[n - 1]:
        n -= 1
    b.put(n, nbits)
    i = 0
    while i < n:
        l = lens[i]
        if l < 7:
            b.put(l, 3)
        else:
            b.put(7, 3)
            for _ in range(l - 7):
                b.put(1, 1)
            b.put(0, 1)
        i += 1
        if i == skip:
            k = 0
            while k < 3 and i + k < n and not lens[i + k]:
                k += 1
            b.put(k, 2)
            i += k


def lh_stream(data, method, block=8000):
    """-lh4- to -lh7-: blocks with their tables."""
    wbits = {4: 12, 5: 13, 6: 15, 7: 16}[method]
    np, pbits = {4: (14, 4), 5: (14, 4), 6: (16, 5), 7: (17, 5)}[method]
    toks = matches(data, 256, (1 << wbits) - 1)
    b = LhBits()
    for st in range(0, len(toks), block):
        sel = toks[st:st + block]
        cf, pf = [0] * 510, [0] * np
        for t in sel:
            if isinstance(t, tuple):
                l, d = t
                cf[l + 253] += 1
                d -= 1
                p = 0
                while d >> p:
                    p += 1
                pf[p] += 1
            else:
                cf[t] += 1
        cl = huff_lengths(cf, 16)
        pl = huff_lengths(pf, 16)
        if sum(1 for x in pf if x) == 1:
            pl = [0] * np
        # the main table through the small one
        items, i, n = [], 0, 510
        while n and not cl[n - 1]:
            n -= 1
        while i < n:
            if cl[i]:
                items.append((cl[i] + 2, 0, 0))
                i += 1
                continue
            k = i
            while k < n and not cl[k]:
                k += 1
            run = k - i
            if run >= 20:
                m = min(run, 20 + 511)
                items.append((2, m - 20, 9))
            elif run >= 3:
                m = min(run, 3 + 15)
                items.append((1, m - 3, 4))
            else:
                m = 1
                items.append((0, 0, 0))
            i += m
        tf = [0] * 19
        for s, _, _ in items:
            tf[s] += 1
        tl = huff_lengths(tf, 16)
        b.put(len(sel), 16)
        if sum(1 for x in tf if x) == 1:
            b.put(0, 5)
            b.put([s for s in range(19) if tf[s]][0], 5)
            tc = None
        else:
            lh_small(b, tl, 5, 3)
            tc = canon(tl)
        b.put(n, 9)
        for s, v, w in items:
            if tc:
                b.put(tc[s][0], tc[s][1])
            if w:
                b.put(v, w)
        if not any(pf):
            b.put(0, pbits)
            b.put(0, pbits)
            pc = None
            ponly = 0
        elif sum(1 for x in pf if x) == 1:
            ponly = [s for s in range(np) if pf[s]][0]
            b.put(0, pbits)
            b.put(ponly, pbits)
            pc = None
        else:
            lh_small(b, pl, pbits, -1)
            pc = canon(pl)
        cc = canon(cl)
        for t in sel:
            if isinstance(t, tuple):
                l, d = t
                b.put(*cc[l + 253])
                d -= 1
                p = 0
                while d >> p:
                    p += 1
                if pc:
                    b.put(*pc[p])
                if p > 1:
                    b.put(d & ((1 << (p - 1)) - 1), p - 1)
            else:
                b.put(*cc[t])
    return b.done()


DOS_DATE = 0x5D6112DC


def lha_member(name, data, method=b"-lh0-", level=1, stream=None, crc=None, size=None, attr=0x00, os=b"A", comment=None, dirpart=None):
    """One member. name: bytes as stored in the name field, dirpart: a directory extended header (level 1, 2)."""
    body = data if stream is None else stream
    orig = len(data) if size is None else size
    c = crc16(data) if crc is None else crc
    if level == 0:
        nm = name + (b"\0" + comment if comment else b"")
        rest = method + struct.pack("<IIIBB", len(body), orig, DOS_DATE, attr, 0) + bytes([len(nm)]) + nm + struct.pack("<H", c)
        return bytes([len(rest), sum(rest) & 255]) + rest + body
    if level == 1:
        # each extended header: type, data, the next one's size; a size counts all of that
        parts = []
        if comment:
            parts.append(b"\x3f" + comment)
        if dirpart:
            parts.append(b"\x02" + dirpart)
        ext = b""
        for i, p in enumerate(parts):
            nx = 2 + len(parts[i + 1]) if i + 1 < len(parts) else 0
            ext += p + struct.pack("<H", nx)
        firstnext = 2 + len(parts[0]) if parts else 0
        rest = method + struct.pack("<IIIBB", len(body) + len(ext), orig, DOS_DATE, attr, 1) + bytes([len(name)]) + name + struct.pack("<H", c) + os + struct.pack("<H", firstnext)
        return bytes([len(rest), sum(rest) & 255]) + rest + ext + body
    parts = [b"\x01" + name]
    if dirpart:
        parts.append(b"\x02" + dirpart)
    if comment:
        parts.append(b"\x71" + comment)
    ext = b""
    for i, p in enumerate(parts):
        nx = 2 + len(parts[i + 1]) if i + 1 < len(parts) else 0
        ext += p + struct.pack("<H", nx)
    total = 26 + len(ext)
    head = struct.pack("<H", total) + method + struct.pack("<IIIBB", len(body), orig, 1700000000, attr, 2) + struct.pack("<H", c) + os + struct.pack("<H", 2 + len(parts[0]))
    return head + ext + body


def lha_packed(name, data, method, level=1, **kw):
    """Packed with `method` (4 to 7)."""
    return lha_member(name, data, b"-lh%d-" % method, level, stream=lh_stream(data, method), **kw)


def write_set(d):
    """(file name, expected) - expected: ok, or the words the test looks for; `want` files hold what it should unpack."""
    os.makedirs(d, exist_ok=True)
    txt, mix, noise = payload("text"), payload("mixed"), payload("noise")
    cases = []

    def put(fn, data, expect):
        with open(os.path.join(d, fn), "wb") as f:
            f.write(data)
        cases.append((fn, expect))
    # LZX: stored, normal, aligned, merged, a filenote, a directory
    a = LZX_HEAD + lzx_member(b"stored.txt", txt, 0) + lzx_member(b"normal.txt", txt) + lzx_member(b"aligned.bin", mix, aligned=True)
    a += lzx_group([(b"merged/a.txt", txt[:9000]), (b"merged/b.bin", mix[:16000]), (b"merged/c.bin", noise)])
    a += lzx_group([(b"second/z.bin", payload("zeros"))], aligned=True)
    a += lzx_member(b"note.txt", b"with a note\n", 0, comment=b"a filenote") + lzx_header(b"empty/", 0, 0, 0, False, 0)
    a += lzx_member(b"r\xe4ksm\xf6rg\xe5s.txt", b"svenska\n", 0)
    put("all.lzx", a, "ok")
    # LhA: each method and level the encoder here makes
    for m in (4, 5, 6, 7):
        for lev in (0, 1, 2):
            put("lh%d-%d.lzh" % (m, lev), lha_packed(b"text.txt", txt, m, lev) + lha_packed(b"mixed.bin", mix, m, lev) + b"\0", "ok")
    a = lha_member(b"stored.txt", txt) + lha_member(b"", b"", b"-lhd-", 1, dirpart=b"dir\xffdeep\xff") + lha_member(b"inner.txt", b"inner\n", dirpart=b"dir\xffdeep\xff")
    a += lha_member(b"r\xe4ksm\xf6rg\xe5s.txt", b"svenska\n", b"-lz4-") + lha_member(b"note.txt", b"with a note\n", level=0, comment=b"a filenote") + lha_member(b"win\\path.txt", b"x\n", level=0) + b"\0"
    put("mixed.lzh", a, "ok")
    # refusals: the member is refused, the rest is unpacked
    for fn, nm in (("dotdot", b"../escaped.txt"), ("deep", b"a/../../escaped.txt"), ("abs", b"/tmp/escaped.txt"), ("long", b"x" * 240 + b"\xe4" * 15)):
        put(fn + ".lzh", lha_member(nm, b"evil\n", level=2) + lha_member(b"good.txt", b"good\n") + b"\0", "refused")
        put(fn + ".lzx", LZX_HEAD + lzx_member(nm, b"evil\n", 0) + lzx_member(b"good.txt", b"good\n", 0), "refused")
    put("bs.lzh", lha_member(b"..\\escaped.txt", b"evil\n", level=0) + lha_member(b"good.txt", b"good\n") + b"\0", "refused")
    put("ffsep.lzh", lha_member(b"", b"", b"-lhd-", 1, dirpart=b"..\xff") + lha_member(b"good.txt", b"good\n") + b"\0", "refused")
    put("link.lzh", lha_member(b"", b"", b"-lhd-", 1, dirpart=b"lnk|/etc/passwd\xff") + lha_member(b"good.txt", b"good\n") + b"\0", "refused")
    # a checksum wrong: unpacked, but said, and no file left
    put("badcrc.lzh", lha_member(b"bad.txt", b"evil\n", crc=0x1234) + lha_member(b"good.txt", b"good\n") + b"\0", "checksum")
    put("badcrc2.lzh", lha_packed(b"bad.txt", txt, 5, crc=0x1234) + lha_member(b"good.txt", b"good\n") + b"\0", "checksum")
    put("badcrc.lzx", LZX_HEAD + lzx_header(b"bad.txt", 5, 5, 0, False, 0x1234) + b"evil\n" + lzx_member(b"good.txt", b"good\n", 0), "checksum")
    s = lzx_stream(txt)
    put("badcrc2.lzx", LZX_HEAD + lzx_header(b"bad.txt", len(txt), len(s), 2, False, 0x1234) + s + lzx_member(b"good.txt", b"good\n", 0), "checksum")
    # a damaged header
    g = bytearray(LZX_HEAD + lzx_member(b"a.txt", b"hello\n", 0))
    g[10 + 3] ^= 1
    put("badhdr.lzx", bytes(g), "damaged")
    g = bytearray(lha_member(b"a.txt", b"hello\n") + b"\0")
    g[5] ^= 1
    put("badhdr.lzh", bytes(g), "damaged")
    # methods and levels not done
    put("other.lzh", lha_member(b"x.txt", b"data\n", b"-lz5-") + lha_member(b"good.txt", b"good\n") + b"\0", "not supported")
    g = bytearray(lha_member(b"a.txt", b"hello\n", level=2))
    g[20] = 3
    put("level3.lzh", bytes(g), "does not read")
    put("mode.lzx", LZX_HEAD + lzx_header(b"x.txt", 5, 5, 3, False, 0) + b"evil\n" + lzx_member(b"good.txt", b"good\n", 0), "not supported")
    put("nolzx.lzx", b"hello, not an archive at all", "damaged")
    put("nolha.lzh", b"hello, not an archive at all", "damaged")
    # bombs: small, promising more than the limit
    put("bomb.lzx", LZX_HEAD + lzx_header(b"big.bin", 0xF0000000, 8, 2, False, 0) + bytes(8), "over the limit")
    put("bomb.lzh", lha_member(b"big.bin", b"", b"-lh5-", 1, stream=bytes(8), size=0xF0000000) + b"\0", "over the limit")
    # a stream that says it makes more than its header promised: a match past the promise is damage
    st = lh_stream(txt, 5)
    put("short.lzh", lha_member(b"s.txt", txt, b"-lh5-", 1, stream=st, size=1000, crc=crc16(txt[:1000])) + b"\0", "damaged")
    # found by the review of 2026-11-01. A damaged group with many members after the damage: each must not decode it again.
    big = bytes((i * 7) & 255 for i in range(300000))
    st = lzx_stream(big + bytes(50))
    g = lzx_header(b"big.bin", len(big), 0, 2, True, zlib.crc32(big))
    for i in range(400):
        g += lzx_header(b"m%04d" % i, 1, 0 if i < 399 else len(st) * 9 // 10, 2, True, 0)
    put("regroup.lzx", LZX_HEAD + g + st[:len(st) * 9 // 10], "damaged")
    # stored members in a merged group
    d1, d2 = b"first\n", b"second\n"
    put("storedgroup.lzx", LZX_HEAD + lzx_header(b"a.txt", len(d1), 0, 0, True, zlib.crc32(d1)) + lzx_header(b"b.txt", len(d2), len(d1 + d2), 0, True, zlib.crc32(d2)) + d1 + d2, "ok")
    # a zero byte may not hide what follows it; a file of zeros is no archive
    put("midzero.lzh", lha_member(b"a.txt", b"hello\n") + b"\0" + lha_member(b"b.txt", b"hidden\n") + b"\0", "damaged")
    put("allzero.lzh", bytes(100), "damaged")
    put("padded.lzh", lha_member(b"a.txt", b"hello\n") + b"\0" + b"\x1a" * 60 + bytes(60), "ok")
    put("junk.lzh", lha_member(b"a.txt", b"hello\n") + b"\0\xc4\x8e\xd8)\x12\xd3\\\xa44\xf9\xb4\xaf\x8e", "ok")
    # a name cut by a buffer must not land as another file
    put("cut.lzh", lha_member(b"a/" + b"./" * 149 + b"realname", b"evil\n", level=2) + lha_member(b"good.txt", b"good\n") + b"\0", "refused")
    # U+009B, a terminal's CSI, in valid UTF-8; a date past 2038 as a signed number
    put("c1.lzx", LZX_HEAD + lzx_member(b"x\xc2\x9b2Jy", b"data\n", 0), "ok")
    put("d2040.lzx", LZX_HEAD + lzx_member(b"later.txt", b"data\n", 0, date=(9 << 27) | (5 << 23) | (6 << 17) | (12 << 12)), "ok")
    # a directory that is only "./": this folder, not a refusal
    put("dotdir.lzh", lha_member(b"./", b"", b"-lhd-", 2) + lha_member(b"a.txt", b"hello\n") + b"\0", "ok")
    put("dotdir.lzx", LZX_HEAD + lzx_header(b"./", 0, 0, 0, False, 0) + lzx_member(b"a.txt", b"hello\n", 0), "ok")
    # a distance before the group's first byte reads zeros (found on real archives of the older header)
    bw = LzxBits()
    lens = [0] * 768
    lens[ord("A")] = 1
    lens[256 + (1 << 5) + 8] = 1        # length index 1 (4 bytes), slot 8 (distance 16 to 23)
    bw.put(2, 3)
    bw.put(0, 8)
    bw.put(0, 8)
    bw.put(5, 8)
    lzx_half(bw, lens[:256], [0] * 256, False)
    lzx_half(bw, lens[256:], [0] * 512, True)
    cw = canon(lens)
    bw.code(*cw[ord("A")])
    bw.code(*cw[256 + (1 << 5) + 8])
    bw.put(0, 3)                        # distance 16: before the start
    pre = bw.done()
    want = b"A\0\0\0\0"
    put("prezero.lzx", LZX_HEAD[:4] + b"\0\0\x0a\0\0\0" + lzx_header(b"p.bin", 5, len(pre), 2, False, zlib.crc32(want)) + pre, "ok")
    # empty members of the largest window cost no window
    put("empties.lzh", b"".join(lha_member(b"e%04d" % i, b"", b"-lh7-", 1) for i in range(3000)) + b"\0", "ok")
    return cases


if __name__ == "__main__":
    for fn, ex in write_set(sys.argv[1]):
        print(fn, ex)
