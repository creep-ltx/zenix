#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Test pictures for show (DESIGN D50, docs/d50-viewers.md), made here from numbers: nobody else's pictures.

  mkpics.py cases DIR   IFF ILBM and GIF files for tools/show-host-test.c, each NAME.iff or NAME.gif with NAME.exp
                        (the line "W H MODE" and then W x H x 3 bytes of the RGB expected), and damaged files bad-*.*
                        with bad-*.why (the start of the reason expected, or "cut" for a picture cut short)
  mkpics.py pics DIR    the pictures FS-UAE's tools/show-test.sh opens: bars.iff (lores, 16 colours: blocks in the
                        flat theme's pens, so the screenshot's pixels are exact), ham.iff, wide.iff (hires lace,
                        bigger than a window), dots.gif, broken.iff (no BMHD: shown as refused), notes.txt

The ILBM writer follows the ILBM spec (BMHD, CMAP, CAMG, BODY, ByteRun1), HAM and EHB are worked out here by the
Amiga Hardware Reference Manual's rules and AGA's HAM8 (the top 6 bits of a gun, its low 2 kept), the GIF writer is
an LZW coder of its own (variable code size, a clear code when the table is full). An independent reference: the
C decoder is never consulted."""
import os
import struct
import sys

FLAT = [0x344, 0x112, 0xEEE, 0x223, 0x47B, 0xBBC, 0x455, 0xFFF, 0xCCD, 0xD54, 0x7C5, 0xEB4, 0x69E, 0xB7C, 0x5BC, 0x567]


def rgb12(v):
    return ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17)


def chunk(cid, data):
    return cid + struct.pack(">I", len(data)) + data + (b"\0" if len(data) & 1 else b"")


def byterun1(data):
    """ByteRun1 of a byte string: runs of 3 or more as runs, the rest as literals of at most 128"""
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        j = i
        while j + 1 < n and data[j + 1] == data[i] and j - i < 127:
            j += 1
        if j - i >= 2:
            out += bytes([257 - (j - i + 1), data[i]])
            i = j + 1
            continue
        k = i
        while k < n and k - i < 128 and not (k + 2 < n and data[k] == data[k + 1] == data[k + 2]):
            k += 1
        out += bytes([k - i - 1]) + data[i:k]
        i = k
    return bytes(out)


def planes_of(idx, w, h, depth):
    """chunky values -> ILBM rows: for each row, each plane's bits padded to 16-bit words"""
    rb = (w + 15) // 16 * 2
    rows = []
    for y in range(h):
        row = bytearray()
        for p in range(depth):
            b = bytearray(rb)
            for x in range(w):
                if idx[y * w + x] >> p & 1:
                    b[x >> 3] |= 0x80 >> (x & 7)
            row += b
        rows.append(bytes(row))
    return rows, rb


def ilbm(w, h, depth, idx, pal=None, camg=None, comp=1, mask=False, stream=False, four=False, nest=False,
         aspect=(10, 11), body_cut=None):
    rows, rb = planes_of(idx, w, h, depth)
    if mask:
        rows = [r + bytes([0xAA]) * rb for r in rows]
    body = b"".join(rows)
    if comp:
        body = byterun1(body) if stream else b"".join(byterun1(r) for r in rows)
    if body_cut is not None:
        body = body[:body_cut]
    bm = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, depth, 1 if mask else 0, comp, 0, 0, aspect[0], aspect[1], w, h)
    c = chunk(b"BMHD", bm)
    if pal is not None:
        c += chunk(b"CMAP", bytes(v & 0xF0 if four else v for t in pal for v in t))
    if camg is not None:
        c += chunk(b"CAMG", struct.pack(">I", camg))
    c += chunk(b"BODY", body)
    form = b"FORM" + struct.pack(">I", len(c) + 4) + b"ILBM" + c
    if nest:
        inner = form
        c = chunk(b"ANNO", b"nested") + inner
        form = b"FORM" + struct.pack(">I", len(c) + 4) + b"ILBM" + c
    return form


def lzw(idx, mcs):
    clear, eoi = 1 << mcs, (1 << mcs) + 1
    out, acc, nb = bytearray(), 0, 0
    size, nxt = mcs + 1, clear + 2
    table = {}

    def emit(code):
        nonlocal acc, nb
        acc |= code << nb
        nb += size
        while nb >= 8:
            out.append(acc & 255)
            acc >>= 8
            nb -= 8
    emit(clear)
    cur = None
    for v in idx:
        if cur is None:
            cur = v
            continue
        k = (cur, v)
        if k in table:
            cur = table[k]
            continue
        emit(cur)
        if nxt < 4096:
            table[k] = nxt
            nxt += 1
            if nxt > (1 << size) and size < 12:
                size += 1
        else:
            emit(clear)
            table, nxt, size = {}, clear + 2, mcs + 1
        cur = v
    if cur is not None:
        emit(cur)
    emit(eoi)
    if nb:
        out.append(acc & 255)
    blocks = bytearray()
    for i in range(0, len(out), 255):
        part = out[i:i + 255]
        blocks += bytes([len(part)]) + part
    return bytes(blocks) + b"\0"


def gif(w, h, idx, pal, inter=False, local=False, ext=True, cut=None):
    n = 1
    while (1 << n) < len(pal):
        n += 1
    pal = list(pal) + [(0, 0, 0)] * ((1 << n) - len(pal))
    ct = bytes(v for t in pal for v in t)
    mcs = max(2, n)
    order = list(range(h))
    if inter:
        order = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
    data = [idx[y * w + x] for y in order for x in range(w)]
    hdr = b"GIF89a" + struct.pack("<HHBBB", w, h, (0 if local else 0x80) | (n - 1), 0, 0) + (b"" if local else ct)
    if ext:
        hdr += b"\x21\xf9\x04\x00\x00\x00\x00\x00" + b"\x21\xfe\x05hello\x00"
    img = b"\x2c" + struct.pack("<HHHHB", 0, 0, w, h, (0x40 if inter else 0) | ((0x80 | (n - 1)) if local else 0))
    img += ct if local else b""
    body = bytes([mcs]) + lzw(data, mcs)
    if cut is not None:
        body = body[:cut]
    return hdr + img + body + (b";" if cut is None else b"")


# ---- the expected pixels, worked out here ----
def exp_pal(w, h, idx, pal):
    return bytes(v for i in idx for v in (pal[i] if i < len(pal) else (0, 0, 0)))


def exp_ham(w, h, idx, pal, ham8):
    out = bytearray()
    for y in range(h):
        r, g, b = pal[0]
        for x in range(w):
            c = idx[y * w + x]
            ctl, d = (c >> 6, c & 63) if ham8 else (c >> 4 & 3, c & 15)
            if ctl == 0:
                r, g, b = pal[d] if d < len(pal) else (0, 0, 0)
            else:
                v = (d << 2) if ham8 else d * 17
                if ctl == 1:
                    b = (v | (b & 3)) if ham8 else v
                elif ctl == 2:
                    r = (v | (r & 3)) if ham8 else v
                else:
                    g = (v | (g & 3)) if ham8 else v
            out += bytes((r, g, b))
    return bytes(out)


def rnd(seed):
    s = seed
    while True:
        s = (s * 1103515245 + 12345) & 0x7FFFFFFF
        yield s >> 8


def pattern(w, h, n, seed=1, runs=True):
    r = rnd(seed)
    out = []
    for y in range(h):
        for x in range(w):
            if runs and (x // 7 + y // 3) % 3 == 0:
                out.append((x // 7 + y) % n)        # runs: ByteRun1 packs them
            else:
                out.append(next(r) % n)
    return out


def palette(n, seed=3):
    r = rnd(seed)
    return [(next(r) & 255, next(r) & 255, next(r) & 255) for _ in range(n)]


def cases(d):
    os.makedirs(d, exist_ok=True)
    n = 0

    def put(name, data, w, h, mode, exp):
        nonlocal n
        open(os.path.join(d, name), "wb").write(data)
        base = name.rsplit(".", 1)[0]
        open(os.path.join(d, base + ".exp"), "wb").write(f"{w} {h} {mode}\n".encode() + exp)
        n += 1
    PAL = 0x21000
    for depth in range(1, 9):
        for w in (1, 7, 16, 37):
            h = 5
            pal = palette(1 << depth, depth)
            idx = pattern(w, h, 1 << depth, depth * 10 + w)
            for comp in (0, 1):
                mode = f"{1 << depth} colours"
                put(f"d{depth}-w{w}-c{comp}.iff", ilbm(w, h, depth, idx, pal, PAL, comp), w, h, mode,
                    exp_pal(w, h, idx, pal))
    w, h = 45, 6
    pal = palette(32, 9)
    idx = pattern(w, h, 64, 11)
    full = pal + [(r >> 1, g >> 1, b >> 1) for r, g, b in pal]
    put("ehb.iff", ilbm(w, h, 6, idx, pal, PAL | 0x80), w, h, "EHB", exp_pal(w, h, idx, full))
    put("ehb-guess.iff", ilbm(w, h, 6, idx, pal, None), w, h, "EHB", exp_pal(w, h, idx, full))
    pal16 = palette(16, 12)
    idx = pattern(w, h, 64, 13)
    put("ham6.iff", ilbm(w, h, 6, idx, pal16, PAL | 0x800), w, h, "HAM6", exp_ham(w, h, idx, pal16, False))
    put("ham6-guess.iff", ilbm(w, h, 6, idx, pal16, None), w, h, "HAM6", exp_ham(w, h, idx, pal16, False))
    put("ham6-hires-lace.iff", ilbm(w, h, 6, idx, pal16, 0x8804), w, h, "HAM6, hires, lace",
        exp_ham(w, h, idx, pal16, False))
    pal64 = palette(64, 14)
    idx = pattern(w, h, 256, 15)
    put("ham8.iff", ilbm(w, h, 8, idx, pal64, PAL | 0x800), w, h, "HAM8", exp_ham(w, h, idx, pal64, True))
    put("ham8-stream.iff", ilbm(w, h, 8, idx, pal64, PAL | 0x800, stream=True), w, h, "HAM8",
        exp_ham(w, h, idx, pal64, True))
    # 24 planes: R then G then B, bit 0 first
    r = rnd(21)
    pix = [(next(r) & 255, next(r) & 255, next(r) & 255) for _ in range(w * h)]
    idx24 = [p[0] | p[1] << 8 | p[2] << 16 for p in pix]
    put("rgb24.iff", ilbm(w, h, 24, idx24, None, None), w, h, "24-bit", bytes(v for p in pix for v in p))
    # modes from CAMG and from the size
    pal = palette(16, 5)
    idx = pattern(w, h, 16, 6)
    e = exp_pal(w, h, idx, pal)
    put("hires.iff", ilbm(w, h, 4, idx, pal, PAL | 0x8000), w, h, "16 colours, hires", e)
    put("lace.iff", ilbm(w, h, 4, idx, pal, PAL | 0x4), w, h, "16 colours, lace", e)
    put("shres.iff", ilbm(w, h, 4, idx, pal, PAL | 0x8020), w, h, "16 colours, super-hires", e)
    put("vga.iff", ilbm(w, h, 4, idx, pal, 0x39024), w, h, "16 colours", e)       # VGA productivity: by size
    put("junk-camg.iff", ilbm(w, h, 4, idx, pal, 0x12340004), w, h, "16 colours", e)  # no 0x1000: no CAMG
    put("old-camg.iff", ilbm(w, h, 4, idx, pal, 0x8004 | 0x4000 | 0x0002), w, h, "16 colours, hires, lace", e)
    put("rtg-camg.iff", ilbm(w, h, 4, idx, pal, 0x50011000), w, h, "16 colours", e)
    put("aspect-hires.iff", ilbm(w, h, 4, idx, pal, None, aspect=(5, 11)), w, h, "16 colours, hires", e)
    put("mask.iff", ilbm(w, h, 4, idx, pal, PAL, mask=True), w, h, "16 colours", e)
    put("mask-plain.iff", ilbm(w, h, 4, idx, pal, PAL, comp=0, mask=True), w, h, "16 colours", e)
    put("stream.iff", ilbm(w, h, 4, idx, pal, PAL, stream=True), w, h, "16 colours", e)
    put("nested.iff", ilbm(w, h, 4, idx, pal, PAL, nest=True), w, h, "16 colours", e)
    four = [(v[0] & 0xF0 | v[0] >> 4, v[1] & 0xF0 | v[1] >> 4, v[2] & 0xF0 | v[2] >> 4) for v in pal]
    put("cmap4.iff", ilbm(w, h, 4, idx, pal, PAL, four=True), w, h, "16 colours", exp_pal(w, h, idx, four))
    short = pal[:5]                       # indices past the CMAP: black
    put("short-cmap.iff", ilbm(w, h, 4, idx, short, PAL), w, h, "16 colours", exp_pal(w, h, idx, short))
    gr = [(i * 255 // 15,) * 3 for i in range(16)]
    put("no-cmap.iff", ilbm(w, h, 4, idx, None, PAL), w, h, "16 colours", exp_pal(w, h, idx, gr))
    big = ilbm(640, 400, 2, pattern(640, 400, 4, 7), palette(4), None)
    put("guess-hires-lace.iff", big, 640, 400, "4 colours, hires, lace",
        exp_pal(640, 400, pattern(640, 400, 4, 7), palette(4)))
    # a FORM longer than the file (what is there is read)
    f = bytearray(ilbm(w, h, 4, idx, pal, PAL))
    f[4:8] = struct.pack(">I", len(f) + 1000)
    put("long-form.iff", bytes(f), w, h, "16 colours", e)
    # GIFs
    for bits, (gw, gh) in ((1, (9, 4)), (2, (13, 7)), (4, (33, 9)), (8, (64, 70))):
        pal = palette(1 << bits, 30 + bits)
        idx = pattern(gw, gh, 1 << bits, 40 + bits)
        e = exp_pal(gw, gh, idx, pal)
        put(f"g{bits}.gif", gif(gw, gh, idx, pal), gw, gh, f"{1 << bits} colours", e)
        put(f"g{bits}-inter.gif", gif(gw, gh, idx, pal, inter=True), gw, gh, f"{1 << bits} colours", e)
        put(f"g{bits}-local.gif", gif(gw, gh, idx, pal, local=True, ext=False), gw, gh, f"{1 << bits} colours", e)
    gw, gh = 300, 60                       # more than 4096 codes: a clear code mid-stream
    pal = palette(256, 50)
    idx = pattern(gw, gh, 256, 51, runs=False)
    put("gbig.gif", gif(gw, gh, idx, pal), gw, gh, "256 colours", exp_pal(gw, gh, idx, pal))
    # ---- damaged ----
    m = 0

    def bad(name, data, why):
        nonlocal m
        open(os.path.join(d, "bad-" + name), "wb").write(data)
        open(os.path.join(d, "bad-" + name.rsplit(".", 1)[0] + ".why"), "w").write(why)
        m += 1
    pal = palette(16, 5)
    idx = pattern(w, h, 16, 6)
    good = ilbm(w, h, 4, idx, pal, PAL)
    bad("empty.iff", b"", "not a picture")
    bad("text.iff", b"hello, this is text\n", "not a picture")
    bad("pbm.iff", good[:8] + b"PBM " + good[12:], "not a picture")
    bad("nobmhd.iff", good.replace(b"BMHD", b"XXXX"), "damaged (no BMHD)")
    bad("nobody.iff", good.replace(b"BODY", b"XXXX"), "a palette, no picture")
    g2 = bytearray(good); g2[20:22] = b"\0\0"
    bad("zerow.iff", bytes(g2), "damaged (no size)")
    g2 = bytearray(good); g2[20:22] = struct.pack(">H", 5000)
    bad("toowide.iff", bytes(g2), "too big")
    g2 = bytearray(good); g2[28] = 12
    bad("planes12.iff", bytes(g2), "not shown: its planes")
    g2 = bytearray(good); g2[30] = 2
    bad("comp2.iff", bytes(g2), "not shown: its compression")
    g2 = bytearray(good); g2[29] = 7
    bad("masking.iff", bytes(g2), "damaged (its masking)")
    bad("body0.iff", ilbm(w, h, 4, idx, pal, PAL, body_cut=0), "damaged (its BODY is cut short)")
    bad("bodycut.iff", ilbm(w, h, 4, idx, pal, PAL, body_cut=40), "cut")
    bad("plaincut.iff", ilbm(w, h, 4, idx, pal, PAL, comp=0, body_cut=40), "cut")
    bad("cut12.iff", good[:12], "damaged (no BMHD)")
    bad("formempty.iff", b"FORM\0\0\0\0ILBM", "damaged (its FORM is empty)")
    bmhd0 = chunk(b"BMHD", struct.pack(">HHhhBBBBHBBhh", 0, 0, 0, 0, 4, 0, 0, 0, 0, 10, 11, 320, 256))
    cm = chunk(b"CMAP", bytes(v for t in pal for v in t))
    bad("palette.iff", b"FORM" + struct.pack(">I", 4 + len(bmhd0) + len(cm)) + b"ILBM" + bmhd0 + cm,
        "a palette, no picture")
    bmhd1 = chunk(b"BMHD", struct.pack(">HHhhBBBBHBBhh", 4096, 4096, 0, 0, 24, 0, 1, 0, 0, 10, 11, 4096, 4096))
    body1 = chunk(b"BODY", b"\0\0")
    bad("huge.iff", b"FORM" + struct.pack(">I", 4 + len(bmhd1) + len(body1)) + b"ILBM" + bmhd1 + body1,
        "too big for memory")
    gp = palette(16, 5)
    gidx = pattern(33, 9, 16, 6)
    gg = gif(33, 9, gidx, gp)
    bad("gif-short.gif", gg[:10], "damaged (cut short in its header)")
    bad("gif-ct.gif", gg[:20], "damaged (cut short in its colours)")
    bad("gif-noimg.gif", gg[:13 + 48] + b";", "no picture in it")
    bad("gif-block.gif", gg[:13 + 48] + b"\x99", "damaged (an unknown block)")
    hdr = 13 + 48 + 8 + 9                    # header, colours, the two extensions
    bad("gif-zero.gif", gg[:hdr + 5] + b"\0\0" + gg[hdr + 7:], "damaged (no size)")
    bad("gif-mcs.gif", gg[:hdr + 10] + b"\x0c" + gg[hdr + 11:], "damaged (its code size)")
    bad("gif-cut.gif", gif(33, 9, gidx, gp, cut=30), "cut")
    bad("gif-cut2.gif", gif(33, 9, gidx, gp, cut=2), "damaged (no pixels)")
    # a first code past the colours (clear, then 31 with 16 colours)
    bad("gif-code.gif", gg[:hdr + 10] + bytes([4, 2, 0xF0, 0x03, 0]) + b";", "damaged (a code before its table)")
    return n, m


def pics(d):
    os.makedirs(d, exist_ok=True)
    pal = [rgb12(v) for v in FLAT]
    w, h = 320, 128
    idx = [(x // 40) + 8 * (y // 64) for y in range(h) for x in range(w)]      # 16 blocks of 40 x 64 lores pixels
    open(os.path.join(d, "bars.iff"), "wb").write(ilbm(w, h, 4, idx, pal, 0x21000))
    # HAM6: a smooth ramp of reds and blues (the base colour then modify-red, modify-blue)
    w, h = 320, 64
    idx = []
    for y in range(h):
        for x in range(w):
            idx.append(0x20 | (x * 16 // w) if x % 2 == 0 else 0x10 | (y * 16 // h))
    open(os.path.join(d, "ham.iff"), "wb").write(ilbm(w, h, 6, idx, [(0, 0, 0)] * 16, 0x21800))
    # hires lace, 800 x 600: bigger than any window (real size pans)
    w, h = 800, 600
    idx = [((x // 64) ^ (y // 64)) & 7 for y in range(h) for x in range(w)]
    open(os.path.join(d, "wide.iff"), "wb").write(ilbm(w, h, 3, idx, pal[:8], 0x29004))
    w, h = 160, 120
    idx = [(x // 20 + y // 30) % 16 for y in range(h) for x in range(w)]
    open(os.path.join(d, "dots.gif"), "wb").write(gif(w, h, idx, pal))
    open(os.path.join(d, "notes.txt"), "w").write("not a picture\n")
    good = ilbm(8, 8, 1, [0] * 64, [(0, 0, 0), (255, 255, 255)], 0x21000)
    open(os.path.join(d, "broken.iff"), "wb").write(good.replace(b"BMHD", b"XXXX"))


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("cases", "pics"):
        sys.exit(__doc__)
    if sys.argv[1] == "cases":
        n, m = cases(sys.argv[2])
        print(f"{n} pictures, {m} damaged files")
    else:
        pics(sys.argv[2])
