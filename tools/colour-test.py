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
    print(__doc__); return 2


sys.exit(main())
