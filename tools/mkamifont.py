#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Make AmigaOS bitmap fonts for Zenix's tests (the reader: display/server/amifont.c; the format: docs/d38-desktop.md
"Fonts"): a NAME.font contents file and NAME/SIZE font files, AmigaDOS hunk executables holding a DiskFontHeader and
a TextFont, written the way the RKM's suits8 example lays one out. The glyphs are drawn here (blocks and bars), so
nothing of anyone else's font is in the repository.

usage: mkamifont.py fonts DIR     the test fonts for the desktop (tools/font-test.sh): ztest 11 (proportional, no
                                  "."), zfixed 9 (fixed width), zbad 11 (its file cut short: refused)
       mkamifont.py cases DIR     those, a colour font, the other hunk forms, and damaged files, with DIR/cases.txt
                                  for tools/amifont-host-test.c: one line a file,
                                    font FILE ok H BASE XSIZE PROP COLOUR NGLYPHS NBYTES CRC ADV(A) ADV(i) KERN(A)
                                    font FILE bad
                                    contents FILE SIZE ok NAME/SIZE   or   contents FILE SIZE bad
"""
import os
import struct
import sys

HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_RELOC32, HUNK_END = 0x3F3, 0x3E9, 0x3EA, 0x3EC, 0x3F2
HUNK_NAME, HUNK_SYMBOL, HUNK_DEBUG, HUNK_RELOC32SHORT = 0x3E8, 0x3F0, 0x3F1, 0x3FC
DFH_ID = 0x0F80
TF = 58
FPF_DISKFONT, FPF_REVPATH, FPF_PROPORTIONAL, FPF_DESIGNED = 0x02, 0x04, 0x20, 0x40
FSF_COLORFONT = 0x40


def block_glyph(c, h, base):
    """the test font's picture of character c: a block (c % 5) + 2 pixels wide on lines 1 .. base, with a notch in
    its top line at the left for odd c (left and right are told apart), and lower case one line shorter"""
    w = 2 + c % 5
    rows = []
    for y in range(h):
        on = 1 <= y <= base - (1 if 97 <= c <= 122 else 0)
        r = [1 if on else 0] * w
        if y == 1 and c % 2:
            r[0] = 0
        rows.append(r)
    return rows


def bar_glyph(c, h, w):
    """the fixed font's: a frame w - 1 wide with the code's low bits as a pattern inside"""
    rows = []
    for y in range(h):
        r = [0] * w
        if y in (0, h - 2):
            r[:w - 1] = [1] * (w - 1)
        elif y < h - 2:
            r[0] = r[w - 2] = 1
            if 1 <= y <= 5 and (c >> (y - 1)) & 1:
                r[2] = 1
        rows.append(r)
    return rows


class Font:
    def __init__(self, name, h, base, xsize, lo, hi, glyphs, prop=True, kern=None, space=None, flags=None,
                 style=0, planes=1):
        self.name, self.h, self.base, self.xsize, self.lo, self.hi = name, h, base, xsize, lo, hi
        self.glyphs = glyphs            # code -> rows (lists of 0/1, all the same width per glyph); missing: empty
        self.prop, self.style, self.planes = prop, style, planes
        self.kern = kern if kern is not None else {}
        self.space = space if space is not None else {}
        self.flags = flags if flags is not None else (FPF_DISKFONT | FPF_DESIGNED | (FPF_PROPORTIONAL if prop else 0))
        self.default = [[1] * 6 if y in (0, h - 1) else [1, 0, 0, 0, 0, 1] for y in range(h)]

    def entries(self):
        return [self.glyphs.get(c, []) for c in range(self.lo, self.hi + 1)] + [self.default]

    def width(self, rows):
        return len(rows[0]) if rows else 0

    def strike(self):
        """the strike (rows of modulo bytes, every glyph side by side) per plane, and the locations"""
        ents, loc, x = self.entries(), [], 0
        for g in ents:
            loc.append((x, self.width(g)))
            x += self.width(g)
        mod = (x + 15) // 16 * 2 or 2
        planes = []
        for p in range(self.planes):
            data = bytearray(mod * self.h)
            for (off, w), g in zip(loc, ents):
                for y in range(self.h):
                    for i in range(w):
                        # a colour font's pixel in one plane: plane 0 the left half, plane 1 the right
                        mine = self.planes == 1 or (p == 0) == (i < w // 2 or w == 1)
                        if g[y][i] and mine:
                            b = off + i
                            data[y * mod + b // 8] |= 0x80 >> (b % 8)
            planes.append(bytes(data))
        return mod, planes, loc

    def tables(self):
        n = self.hi - self.lo + 2
        sp, kn = [], []
        for i, g in enumerate(self.entries()):
            c = self.lo + i
            empty = not g and c not in self.space          # a gap in the range: no glyph, no width (the 8 x 8's)
            kn.append(self.kern.get(c, 0 if empty or not self.prop else 1))
            sp.append(self.space.get(c, 0 if empty else self.width(g) + 1 if self.prop else self.xsize))
        return sp[:n], kn[:n]

    def converted(self):
        """what the reader makes of it: per code point 0-255 with a glyph, rows of (w + 7) / 8 bytes; and the
        advance and kerning of each"""
        sp, kn = self.tables()
        out, adv, kerns, n = bytearray(), {}, {}, 0
        for i, g in enumerate(self.entries()[:-1]):
            c = self.lo + i
            w = self.width(g)
            a = sp[i] + kn[i] if self.prop else self.xsize
            if c < 32 or 0x7F <= c < 0xA0 or (not w and not a):
                continue
            n += 1
            adv[c], kerns[c] = a, kn[i]
            bpr = (w + 7) // 8
            for y in range(self.h):
                row = bytearray(bpr)
                for x in range(w):
                    if g[y][x]:
                        row[x // 8] |= 0x80 >> (x % 8)
                out += row
        return bytes(out), adv, kerns, n

    def hunk(self, nopointer_reloc=None, loc_ptr=None, extra=b""):
        """the hunk's bytes and its relocations (offsets of the 32-bit words holding hunk offsets)"""
        mod, planes, loc = self.strike()
        sp, kn = self.tables()
        colour = self.style & FSF_COLORFONT
        tf_end = TF + (96 if colour else 52)
        h = bytearray(tf_end)
        h[0:4] = bytes.fromhex("70FF4E75")                      # moveq #-1,d0; rts
        h[4 + 8] = 12                                           # ln_Type NT_FONT
        struct.pack_into(">I", h, 4 + 10, 26)                   # ln_Name: dfh_Name
        struct.pack_into(">HH", h, 18, DFH_ID, 1)
        nm = (self.name + str(self.h)).encode()[:31]
        h[26:26 + len(nm)] = nm
        h[TF + 8] = 12
        struct.pack_into(">I", h, TF + 10, 26)
        struct.pack_into(">HBBHHHH", h, TF + 20, self.h, self.style, self.flags, self.xsize, self.base, 1, 0)
        h[TF + 32], h[TF + 33] = self.lo, self.hi
        relocs = [4 + 10, TF + 10]
        body = bytearray()
        def put(b):
            off = tf_end + len(body)
            body.extend(b)
            while len(body) % 2:
                body.append(0)
            return off
        pdata = [put(p) for p in planes]
        ploc = put(b"".join(struct.pack(">HH", o, w) for o, w in loc))
        pspace = put(b"".join(struct.pack(">h", v) for v in sp)) if self.prop else 0
        pkern = put(b"".join(struct.pack(">h", v) for v in kn)) if self.prop else 0
        if loc_ptr is not None:
            ploc = loc_ptr
        struct.pack_into(">IHIII", h, TF + 34, 0 if colour else pdata[0], mod, ploc, pspace, pkern)
        relocs += [o for o, v in ((TF + 34, 0 if colour else pdata[0]), (TF + 40, ploc), (TF + 44, pspace),
                                  (TF + 48, pkern)) if v]
        if colour:
            struct.pack_into(">HBBBBBB", h, TF + 52, 1, self.planes, 1, 0, (1 << self.planes) - 1,
                             (1 << self.planes) - 1, 0)
            for k, o in enumerate(pdata):
                struct.pack_into(">I", h, TF + 64 + 4 * k, o)
                relocs.append(TF + 64 + 4 * k)
        data = bytes(h) + bytes(body) + extra
        data += bytes(-len(data) % 4)
        if nopointer_reloc is not None:
            relocs.remove(nopointer_reloc)
        return data, relocs


def hunkfile(data, relocs, kind=HUNK_CODE, short=False, extras=(), table=None, reloc_hunk=0):
    L = lambda *v: struct.pack(">%dI" % len(v), *v)
    nl = len(data) // 4
    out = L(HUNK_HEADER, 0) + (table if table is not None else L(1, 0, 0, nl))
    for e in extras:
        out += e
    out += L(kind, nl) + data
    if short:
        w = struct.pack(">HH", len(relocs), reloc_hunk) + b"".join(struct.pack(">H", r) for r in relocs) + \
            struct.pack(">H", 0)
        w += bytes(-len(w) % 4)
        out += L(HUNK_RELOC32SHORT) + w
    elif relocs:
        out += L(HUNK_RELOC32, len(relocs), reloc_hunk, *relocs) + L(0)
    return out + L(HUNK_END)


def contents(entries, fid=0x0F00, tags=None):
    """entries: (file name, size, style, flags)"""
    out = struct.pack(">HH", fid, len(entries))
    for name, size, style, flags in entries:
        nb = name.encode()
        if fid == 0x0F00:
            out += nb.ljust(256, b"\0")
        else:
            t = tags or []
            field = bytearray(nb.ljust(254, b"\0"))
            for i, (tg, v) in enumerate(t):
                struct.pack_into(">II", field, 254 - 8 * len(t) + 8 * i, tg, v)
            out += bytes(field) + struct.pack(">H", len(t))
        out += struct.pack(">HBB", size, style, flags)
    return out


def crc(b):
    v = 0x811C9DC5
    for x in b:
        v = ((v ^ x) * 0x01000193) & 0xFFFFFFFF
    return v


def ztest():
    h, base = 11, 8
    gl = {c: block_glyph(c, h, base) for c in range(33, 127) if c != ord(".")}     # no "." (the 8 x 8 stands in)
    gl.update({c: block_glyph(c, h, base) for c in (0xC4, 0xD6, 0xE4, 0xE5, 0xF6)})     # Ä Ö ä å ö
    gl[32] = []                                                                     # the space: no picture
    sp = {32: 4}
    kn = {32: 0, ord("W"): 2}
    return Font("ztest", h, base, 8, 32, 0xF6, gl, prop=True, kern=kn, space=sp)


def zfixed():
    h, w = 9, 7
    return Font("zfixed", h, 6, w, 32, 126, {c: bar_glyph(c, h, w) for c in range(32, 127)}, prop=False)


def write(path, b):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    open(path, "wb").write(b)


def write_font(d, f, sizes_name=None):
    data, relocs = f.hunk()
    write(os.path.join(d, f.name, str(f.h)), hunkfile(data, relocs))
    write(os.path.join(d, f.name + ".font"),
          contents([(f"{f.name}/{f.h}", f.h, f.style, f.flags | 0x80)]))


def expect_ok(f):
    bits, adv, kern, n = f.converted()
    base = min(f.base, f.h - 1)
    return "ok %d %d %d %d %d %d %d %08x %d %d %d" % (
        f.h, base, f.xsize, 1 if f.prop else 0, 1 if f.style & FSF_COLORFONT else 0, n, len(bits), crc(bits),
        adv.get(ord("A"), -1), adv.get(ord("i"), -1), kern.get(ord("A"), 0))


def cases(d):
    lines = []
    def font(name, b, exp):
        write(os.path.join(d, name), b)
        lines.append(f"font {name} {exp}")
    zt, zf = ztest(), zfixed()
    for f in (zt, zf):
        data, relocs = f.hunk()
        font(f"{f.name}-{f.h}", hunkfile(data, relocs), expect_ok(f))
    data, relocs = zt.hunk()
    font("short-relocs", hunkfile(data, relocs, short=True), expect_ok(zt))
    font("data-hunk", hunkfile(data, relocs, kind=HUNK_DATA), expect_ok(zt))
    L = lambda *v: struct.pack(">%dI" % len(v), *v)
    font("with-name-debug", hunkfile(data, relocs, extras=(L(HUNK_NAME, 1) + b"font", L(HUNK_DEBUG, 2, 1, 2))),
         expect_ok(zt))
    col = Font("zcolour", 8, 6, 8, 65, 70, {c: block_glyph(c, 8, 6) for c in range(65, 71)},
               style=FSF_COLORFONT, planes=2)
    data_c, relocs_c = col.hunk()
    font("colour-8", hunkfile(data_c, relocs_c), expect_ok(col))
    big = Font("zbase", 6, 9, 6, 65, 66, {65: block_glyph(65, 6, 5), 66: block_glyph(66, 6, 5)})
    data_b, relocs_b = big.hunk()
    font("baseline-past-height", hunkfile(data_b, relocs_b), expect_ok(big))
    # damaged and hostile files: every one refused
    good = hunkfile(data, relocs)
    font("empty", b"", "bad")
    font("cut-half", good[:len(good) // 2], "bad")
    font("cut-header", good[:16], "bad")
    font("not-hunk", b"\0\0\x03\xf2" + good[4:], "bad")
    font("two-hunks", hunkfile(data, relocs, table=L(2, 0, 1, len(data) // 4, 0)), "bad")
    font("reloc-other-hunk", hunkfile(data, relocs, reloc_hunk=1), "bad")
    font("reloc-outside", hunkfile(data, relocs + [len(data)]), "bad")
    font("reloc-count-huge", good.replace(L(HUNK_RELOC32, len(relocs)), L(HUNK_RELOC32, 0x7FFFFFFF)), "bad")
    font("code-size-huge", good.replace(L(HUNK_CODE, len(data) // 4), L(HUNK_CODE, 0x3FFFFFFF)), "bad")
    bad = bytearray(data); struct.pack_into(">H", bad, 18, 0x1234)
    font("no-dfh-id", hunkfile(bytes(bad), relocs), "bad")
    d1, r1 = zt.hunk(nopointer_reloc=TF + 40)
    font("loc-not-relocated", hunkfile(d1, r1), "bad")
    d2, r2 = zt.hunk(loc_ptr=0x7FFF0)
    font("loc-outside", hunkfile(d2, r2), "bad")
    d3, r3 = zt.hunk(loc_ptr=len(data) - 8)
    font("loc-table-past-end", hunkfile(d3, r3), "bad")
    for name, off, fmt, v in (("height-0", TF + 20, ">H", 0), ("height-100", TF + 20, ">H", 100),
                              ("revpath", TF + 23, ">B", FPF_REVPATH | FPF_PROPORTIONAL),
                              ("lo-after-hi", TF + 32, ">BB", (200, 100)), ("modulo-0", TF + 38, ">H", 0),
                              ("modulo-huge", TF + 38, ">H", 0xFFFF), ("data-null", TF + 34, ">I", 0)):
        b = bytearray(data)
        struct.pack_into(fmt, b, off, *(v if isinstance(v, tuple) else (v,)))
        font(name, hunkfile(bytes(b), relocs), "bad")
    # a glyph's location past its row, a huge width, a huge kerning
    mod, planes, loc = zt.strike()
    ploc = struct.unpack_from(">I", data, TF + 40)[0]
    pkern = struct.unpack_from(">I", data, TF + 48)[0]
    for name, off, v in (("glyph-outside", ploc + 4 * 10, (mod * 8 - 1, 5)), ("glyph-wide", ploc + 4 * 3, (0, 200))):
        b = bytearray(data); struct.pack_into(">HH", b, off, *v)
        font(name, hunkfile(bytes(b), relocs), "bad")
    b = bytearray(data); struct.pack_into(">h", b, pkern + 2 * 5, 1000)
    font("kern-huge", hunkfile(bytes(b), relocs), "bad")
    b = bytearray(data_c); b[TF + 54] = 9
    font("colour-depth-9", hunkfile(bytes(b), relocs_c), "bad")
    # contents files
    def cont(name, b, size, exp):
        write(os.path.join(d, name), b)
        lines.append(f"contents {name} {size} {exp}")
    cont("c-plain", contents([("ztest/11", 11, 0, 0x62)]), 11, "ok ztest/11")
    cont("c-sizes", contents([("helv/9", 9, 0, 0x62), ("helv/11b", 11, 2, 0x62), ("helv/11", 11, 0, 0x62)]), 11,
         "ok helv/11")
    cont("c-styled-only", contents([("helv/11b", 11, 2, 0x62)]), 11, "ok helv/11b")
    cont("c-tags", contents([("ztest/11", 11, 0, 0x62)], fid=0x0F02, tags=[(0x80000002, 0x00440044), (0, 0)]), 11,
         "ok ztest/11")
    cont("c-outline", contents([("CGTimes/15", 15, 0, 0x62)], fid=0x0F03), 15, "ok CGTimes/15")
    cont("c-no-size", contents([("ztest/11", 11, 0, 0x62)]), 13, "bad")
    cont("c-bad-id", b"\x0f\x01" + contents([("ztest/11", 11, 0, 0x62)])[2:], 11, "bad")
    cont("c-cut", contents([("a/8", 8, 0, 0), ("a/11", 11, 0, 0)])[:300], 11, "bad")
    cont("c-zero", struct.pack(">HH", 0x0F00, 0), 8, "bad")
    cont("c-dotdot", contents([("../../etc/passwd", 11, 0, 0)]), 11, "bad")
    cont("c-absolute", contents([("/etc/passwd", 11, 0, 0)]), 11, "bad")
    cont("c-device", contents([("FONTS:ztest/11", 11, 0, 0)]), 11, "bad")
    cont("c-no-end", struct.pack(">HH", 0x0F00, 1) + b"x" * 256 + struct.pack(">HBB", 11, 0, 0), 11, "bad")
    cont("c-tags-huge", struct.pack(">HH", 0x0F02, 1) + b"a/11".ljust(254, b"\0") + struct.pack(">HHBB", 40, 11, 0, 0),
         11, "bad")
    write(os.path.join(d, "cases.txt"), ("\n".join(lines) + "\n").encode())


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("fonts", "cases"):
        sys.exit(__doc__)
    d = sys.argv[2]
    os.makedirs(d, exist_ok=True)
    if sys.argv[1] == "fonts":
        write_font(d, ztest())
        write_font(d, zfixed())
        f = ztest()
        f.name = "zbad"
        write_font(d, f)
        p = os.path.join(d, "zbad", "11")
        b = open(p, "rb").read()
        open(p, "wb").write(b[:len(b) // 2])
    else:
        cases(d)


if __name__ == "__main__":
    main()
