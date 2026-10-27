#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checks for tools/colour-test.sh (DESIGN D14), on screenshots of the colour console taken by tools/shot.py.

  colour-test.py columns SHOT [EDGES]   `termtest columns`: rows 1-7 must number the 80 columns 0..79 in binary (all
                                        visible, in order, nothing repeated), row 8 lit across; prints the geometry
                                        "x0 x1 yc h"; with EDGES (another run's geometry, one plane) the edges must
                                        be the same pixels
  colour-test.py palette SHOT GEOM DEPTH  `termtest colours`: the 16 blocks of rows 3-4 against kernel/gfx.c's
                                        palette as DEPTH planes show it (4: all 16; 3: colour & 7; 2: the four
                                        colours by gfx.c's map2; 1: none, black)
  colour-test.py same A B               the two screenshots are identical (an incremental screen = a redraw)
  colour-test.py rows SHOT GEOM DEPTH   `termtest scroll` scrolled back a page: the first 31 rows hold "line N" in
                                        colour (N % 7) + 1 with N counting up (the scrollback keeps the colours)
  colour-test.py pixel SHOT GEOM ROW COL RGB   the middle of text cell ROW, COL (1-based) has the 12-bit colour RGB
  colour-test.py serial LOG             the raw serial bytes: the prompt's colour codes, and less showing a line
                                        with colour codes and 79 visible characters as one row, the codes passed on
  colour-test.py pcfont SHOT GEOM DEPTH [PSF]   `termtest pcfont` (DESIGN D20 step 1): every cell's 8 x 8 pixels
                                        against the loaded font (PSF, or termtest's own test font): the 256 bytes as
                                        glyphs, CLRHOME, WRAPNOW, LFONLY; the colour blocks in the CGA palette, the
                                        bright foreground and iCE background, as DEPTH planes show them
  colour-test.py pcgone SHOT GEOM DEPTH  the same page after termtest ended without cleaning up: the kernel dropped
                                        the font (the grid no longer in it) and the rules (Zenix's palette again)
  colour-test.py cleared SHOT GEOM      rows 3-29 empty (termtest cleared the screen before it dropped the font)
Each prints what it found and exits 1 on a failure."""
import os
import re
import sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def rgb12(v):
    return tuple(((int(v, 16) >> s) & 15) * 17 for s in (8, 4, 0))


def palette():
    text = open(os.path.join(ROOT, "kernel", "gfx.c")).read()
    body = re.search(r"gfx_palette\[16\] = \{([^}]*)\}", text).group(1)
    return [rgb12(v) for v in re.findall(r"0x[0-9A-Fa-f]+", body)]


def shown(depth):
    """the colour each of the 16 indices shows as at DEPTH planes (as a background; 1 plane: black)"""
    text = open(os.path.join(ROOT, "kernel", "gfx.c")).read()
    pal = palette()
    if depth == 4:
        return pal
    if depth == 3:
        return [pal[i & 7] for i in range(16)]
    if depth == 2:
        m = [int(v) for v in re.search(r"map2\[8\] = \{([^}]*)\}", text).group(1).split(",")]
        p2 = [rgb12(v) for v in re.findall(r"0x[0-9A-Fa-f]+", re.search(r"gfx_palette2\[4\] = \{([^}]*)\}", text).group(1))]
        return [p2[m[i & 7]] for i in range(16)]
    return [(0, 0, 0)] * 16


def cga(depth):
    """the CGA palette (CONR_CGAPAL) as DEPTH planes show it: 2 planes and 1 plane are not changed by it"""
    text = open(os.path.join(ROOT, "kernel", "gfx.c")).read()
    pal = [rgb12(v) for v in re.findall(r"0x[0-9A-Fa-f]+", re.search(r"cga_palette\[16\] = \{([^}]*)\}", text).group(1))]
    if depth == 4:
        return pal
    if depth == 3:
        return [pal[i & 7] for i in range(16)]
    return shown(depth)


def font(path):
    """256 glyphs of 8 rows: a PSF1 file's, or termtest's test font (the byte's two hex digits as bits 2 pixels wide)"""
    if path:
        d = open(path, "rb").read()
        assert d[:4] == b"\x36\x04\x00\x08" and len(d) == 2052, "not a PSF1 font of mode 0, height 8"
        return [list(d[4 + 8 * c:12 + 8 * c]) for c in range(256)]
    wide = [sum(3 << (2 * k) for k in range(4) if n >> k & 1) for n in range(16)]
    return [[0, wide[c >> 4], wide[c >> 4], wide[c >> 4], wide[c & 15], wide[c & 15], wide[c & 15], 0] for c in range(256)]


class Cells:
    """the pixels of text cells, from a columns geometry"""
    def __init__(self, path, geom):
        self.im, self.px, _ = load(path)
        self.x0, self.x1, self.yc, self.h = geom
        self.w = (self.x1 - self.x0 + 1) / 80

    def at(self, row, col, j, k):
        """pixel k of glyph line j in cell (row, col), 0-based"""
        top = self.yc - (7 - row) * self.h - self.h / 2
        return self.px[int(self.x0 + (col + (k + 0.5) / 8) * self.w), int(top + (j + 0.5) * self.h / 8)]

    def levels(self, row, col):
        """the cell's 8 x 8 pixels as brightness 0..1 of the cell's brightest, each read at the pixel's middle between
        the four nearest screen pixels (the screenshot is scaled by about 1.28 x 2.56 and smoothed)"""
        top = self.yc - (7 - row) * self.h - self.h / 2
        def b(x, y):
            xi, yi = int(x - 0.5), int(y - 0.5)
            fx, fy = x - 0.5 - xi, y - 0.5 - yi
            q = lambda a, c: sum(self.px[a, c])
            return ((q(xi, yi) * (1 - fx) + q(xi + 1, yi) * fx) * (1 - fy) +
                    (q(xi, yi + 1) * (1 - fx) + q(xi + 1, yi + 1) * fx) * fy)
        v = [[b(self.x0 + (col + (k + 0.5) / 8) * self.w, top + (j + 0.5) * self.h / 8) for k in range(8)] for j in range(8)]
        ref = max(max(r) for r in v)
        return [[x / ref if ref >= 150 else 0 for x in r] for r in v]

    def glyph(self, row, col, glyphs, among=range(256)):
        """the glyph of `among` that the cell looks most like, and how far the next best was"""
        v = self.levels(row, col)
        def d(b):
            return sum((v[j][k] - (glyphs[b][j] >> (7 - k) & 1)) ** 2 for j in range(8) for k in range(8))
        ds = sorted((d(b), b) for b in among)
        return ds[0][1], (ds[1][0] - ds[0][0]) if len(ds) > 1 else 99

    def bits(self, row, col):
        return [sum(1 << (7 - k) for k in range(8) if r[k] > 0.5) for r in self.levels(row, col)]

    def fg(self, row, col):
        """the most common lit colour in a cell"""
        n = {}
        for j in range(8):
            for k in range(8):
                p = self.at(row, col, j, k)
                if sum(p) > 60:
                    n[p] = n.get(p, 0) + 1
        return max(n, key=n.get) if n else (0, 0, 0)


ACTS = (0, 7, 8, 9, 10, 13, 27)


def pcfont(cells, depth, glyphs):
    bad = []
    blank = [0] * 8
    acts = list(range(1, 256)) + [256]               # 256: an empty cell (an erase, or a space not drawn again); NUL is
    g = glyphs + [blank]                             # never stored
    is_ = lambda r, c, b: cells.glyph(r, c, g, acts)[0] == b
    for b in range(256):
        r, c = 2 + b // 16, b % 16
        got = cells.glyph(r, c, g, acts)[0]
        if b in ACTS or b == 32:
            if got not in (32, 256):
                bad.append(f"cell {r + 1},{c + 1} (byte {b}, which acts): looks like byte {got}, not a space")
        elif got != b:
            bad.append(f"cell {r + 1},{c + 1}: looks like byte {got} ({cells.bits(r, c)}), not byte {b}")
    if not is_(0, 0, ord("H")):
        bad.append("CLRHOME: no H at 1,1")
    if is_(4, 4, ord("Z")):
        bad.append("CLRHOME: the Z at 5,5 was not erased")
    if not all(is_(23, c, ord("W")) for c in range(80)):
        bad.append("WRAPNOW: row 24 is not 80 W")
    if is_(24, 0, ord("X")) or not is_(25, 0, ord("X")):
        bad.append("WRAPNOW: X is not at 26,1 after a blank row 25")
    if not is_(26, 0, ord("A")) or not is_(27, 1, ord("B")):
        bad.append("LFONLY: B is not at 28,2 under A")
    want = cga(depth)
    for i in range(16):
        for row in (19, 20):
            got = cells.at(row, 5 * i + 2, 5, 4)
            if not near(got, want[i]):
                bad.append(f"CGA palette: block {i} row {row + 1}: {got}, wanted {want[i]}")
    bright = {4: want[9], 3: want[1]}.get(depth, want[1] if depth == 2 else rgb12("BBB"))
    plain = want[1] if depth > 1 else rgb12("BBB")
    if not near(cells.fg(22, 0), bright):
        bad.append(f"BRIGHT: R is {cells.fg(22, 0)}, wanted {bright}")
    if not near(cells.fg(22, 1), plain):
        bad.append(f"BRIGHT: r is {cells.fg(22, 1)}, wanted {plain}")
    ice = want[12] if depth == 4 else want[4]
    for c, w in ((2, ice), (3, ice), (4, want[4]), (5, want[4])):
        if not near(cells.at(22, c, 5, 4), w):
            bad.append(f"iCE: cell 23,{c + 1} background {cells.at(22, c, 5, 4)}, wanted {w}")
    return bad


def load(path):
    im = Image.open(path).convert("RGB")
    return im, im.load(), im.size


def columns(path):
    im, px, (W, H) = load(path)
    lit = lambda x, y: sum(px[x, y]) > 200
    best = None                                   # the full row: the widest lit run in the top part, the lowest one
    for y in range(int(H * 0.4)):
        xs = [x for x in range(W) if lit(x, y)]
        if xs and (best is None or len(xs) >= best[0] - 2):
            best = (max(len(xs), best[0] if best else 0), y, xs[0], xs[-1])
    if not best:
        return None
    _, yb, x0, x1 = best
    w = (x1 - x0 + 1) / 80
    # the row height, exactly: column 63 (binary 0111111) is lit from the top of row 0 to the bottom of row 5
    xm = int(x0 + 63.5 * w)
    ya = 0
    while ya < yb and not lit(xm, ya):
        ya += 1
    ye = ya
    while ye < yb and lit(xm, ye + 1):
        ye += 1
    h = (ye - ya + 1) / 6
    yc = ya + 7.5 * h                             # the middle of text row 7 (0-based)
    got = []
    for c in range(80):
        x, v = int(x0 + (c + 0.5) * w), 0
        for k in range(7):
            if lit(x, int(yc - (7 - k) * h)):
                v |= 1 << k
        got.append(v)
    return got, x0, x1, yc, h


def cell(geom, row, col):
    """the middle of text cell (row, col), 0-based, from a columns geometry"""
    x0, x1, yc, h = geom
    return int(x0 + (col + 0.5) * (x1 - x0 + 1) / 80), int(yc - (7 - row) * h)


def near(a, b, tol=24):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def main():
    what = sys.argv[1]
    if what == "columns":
        r = columns(sys.argv[2])
        if not r:
            print("columns: nothing lit found"); return 1
        got, x0, x1, yc, h = r
        ok = got == list(range(80))
        print(f"{x0} {x1} {yc} {h}")
        if not ok:
            print("columns: not 0..79 in order:", got)
        if len(sys.argv) > 3:
            e = open(sys.argv[3]).read().split()
            if (int(e[0]), int(e[1])) != (x0, x1):
                print(f"columns: edges {x0}..{x1}, one plane's {e[0]}..{e[1]}"); ok = False
        return 0 if ok else 1
    if what == "palette":
        im, px, _ = load(sys.argv[2])
        geom = [float(v) for v in open(sys.argv[3]).readline().split()]
        depth, bad = int(sys.argv[4]), 0
        sh = shown(depth)
        for i in range(16):
            want = sh[i]
            for row in (2, 3):
                got = px[cell(geom, row, 5 * i + 2)]
                if not near(got, want):
                    print(f"palette: block {i} row {row + 1}: {got}, wanted {want}"); bad += 1
        if not bad:
            print(f"palette: the 16 blocks as {depth} planes show them")
        return 1 if bad else 0
    if what == "same":
        a, b = Image.open(sys.argv[2]).convert("RGB"), Image.open(sys.argv[3]).convert("RGB")
        if a.size != b.size or a.tobytes() != b.tobytes():
            diff = sum(1 for p, q in zip(a.getdata(), b.getdata()) if p != q)
            print(f"same: {diff} pixels differ"); return 1
        print("same: identical"); return 0
    if what == "rows":
        im, px, _ = load(sys.argv[2])
        geom = [float(v) for v in open(sys.argv[3]).readline().split()]
        depth = int(sys.argv[4])
        pal = shown(depth)
        seq = []
        for row in range(31):                     # the colour of the word "line": its brightest non-black pixels
            counts = {}
            x0, _ = cell(geom, row, 0)
            x1, _ = cell(geom, row, 3)
            _, yc = cell(geom, row, 0)
            h = geom[3]
            for y in range(int(yc - h / 3), int(yc + h / 3)):
                for x in range(int(x0 - 3), int(x1 + 4)):
                    p = px[x, y]
                    if sum(p) > 120:
                        counts[p] = counts.get(p, 0) + 1
            if not counts:
                print(f"rows: row {row + 1} has no text"); return 1
            seq.append(max(counts, key=counts.get))
        # the rows count up through the colours 1-7 (as DEPTH planes show them) from some start
        starts = [k for k in range(7) if all(near(seq[r], pal[(k + r) % 7 + 1]) for r in range(31))]
        names = "".join(str(next((i for i in range(1, 8) if near(c, pal[i])), 0)) for c in seq)
        print("rows: colours", names)
        if not starts:
            print("rows: the colours do not count up 1-7 as these planes show them"); return 1
        return 0
    if what == "pixel":
        im, px, _ = load(sys.argv[2])
        geom = [float(v) for v in open(sys.argv[3]).readline().split()]
        row, colm, rgb = int(sys.argv[4]) - 1, int(sys.argv[5]) - 1, sys.argv[6]
        want = tuple(int(c, 16) * 17 for c in rgb)
        got = px[cell(geom, row, colm)]
        print(f"pixel: cell {row + 1},{colm + 1} is {got}, wanted {want}")
        return 0 if near(got, want) else 1
    if what == "serial":
        raw = open(sys.argv[2], "rb").read()
        ok = True
        if b"\x1b[01;32mzenix\x1b[0m:\x1b[01;34m/\x1b[0m# " not in raw:
            print("serial: the prompt's colour codes are not on the serial port"); ok = False
        if not re.search(rb"\x1b\[31mx{40}\x1b\[0my{39}\x1b\[m\x1b\[K\r*\nSECOND", raw):
            print("serial: less did not show the coloured 79-column line as one row with its codes"); ok = False
        if ok:
            print("serial: the prompt's codes; less passes colour codes on and counts them as no width")
        return 0 if ok else 1
    if what in ("pcfont", "pcgone", "cleared"):
        geom = [float(v) for v in open(sys.argv[3]).readline().split()]
        cells = Cells(sys.argv[2], geom)
        if what == "cleared":
            lit = [r + 1 for r in range(2, 29) for c in range(80) if any(sum(cells.at(r, c, j, 4)) > 60 for j in (1, 4, 6))]
            print("cleared: rows 3-29 empty" if not lit else f"cleared: something on rows {sorted(set(lit))[:8]}")
            return 1 if lit else 0
        depth = int(sys.argv[4])
        glyphs = font(sys.argv[5] if len(sys.argv) > 5 else None)
        if what == "pcfont":
            bad = pcfont(cells, depth, glyphs)
            for b in bad[:12]:
                print("pcfont:", b)
            if not bad:
                print(f"pcfont: 256 glyphs, CLRHOME, WRAPNOW, LFONLY, the CGA palette, bright and iCE as {depth} planes show them")
            return 1 if bad else 0
        same = sum(1 for b in range(256) if b not in ACTS and cells.bits(2 + b // 16, b % 16) == glyphs[b])   # exact only
        sh, bad = shown(depth), 0
        for i in range(16):
            got = cells.at(19, 5 * i + 2, 5, 4)
            if not near(got, sh[i]):
                print(f"pcgone: block {i}: {got}, wanted Zenix's {sh[i]}"); bad += 1
        if same > 8:
            print(f"pcgone: {same} cells still in the loaded font"); bad += 1
        if not bad:
            print(f"pcgone: the font dropped ({same} cells look like it), Zenix's palette again")
        return 1 if bad else 0
    print(__doc__); return 2


sys.exit(main())
