#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checks for tools/screen-test.sh (DESIGN D55, docs/d55-screens.md) on screenshots taken by tools/shot.py, with the
geometry tools/colour-test.py columns found. Coordinates are the screen's own (lores: 320 across); colours 24-bit,
within a tolerance of 24 a channel (the screenshot's scaling blurs edges; the points are the pixels' middles).

  screen-test.py blocks SHOT GEOM W DEPTH PIC [FIRST LAST]
                                 dpytest's picture PIC: block (bx, by) of 40 x 32 (80 x 32 hires) in pen
                                 (bx + 8 by + PIC) % 2^DEPTH, in dpytest's palette (spal); lines FIRST..LAST only
  screen-test.py bars SHOT GEOM GUN  lines 0-31 at x 20: colour 0 a ramp y * 8 in GUN (r or g), the rest 0
  screen-test.py mirror SHOT GEOM    lines 128-255 show the picture's lines 127 back to 0 (a negative modulo)
  screen-test.py fetch SHOT GEOM W   dpytest's fetch pattern: line y lit only in the 8 pixels of byte (y / 4) % (W/8):
                                 every line starts where it should (a fetch too long shifts every line after)
  screen-test.py px SHOT GEOM W X,Y,RRGGBB ...   each point in that colour
  screen-test.py logo SHOT GEOM      zintro's logo (tools/mkintro.py) at lines 8-71: every 7th of its pixels that is not
                                 colour 0 (the bars) in its palette's colour (at most 1% off: one-pixel scratches
                                 blur into their neighbours in the screenshot)
  screen-test.py ham SHOT GEOM 6|8   ham.iff (HAM6) or pics8/ham8.iff (tools/mkpics.py) on a lores screen from its top left: every second
                                 pixel of every fourth line equal to the generator's own HAM decode
Prints what it found and exits 1 on a failure."""
import os
import sys
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def spal(i):
    return (((i * 7) & 15) * 17, ((i * 11 + 5) & 15) * 17, ((i * 13 + 9) & 15) * 17)


def load(shot, geom, w=320):
    x0, x1, yc, h = (float(v) for v in open(geom).read().split()[:4])
    pw, top, lh = (x1 - x0 + 1) / w, yc - 7.5 * h, h / 8
    im = Image.open(shot).convert("RGB")
    px = im.load()
    return lambda x, y: px[int(x0 + (x + 0.5) * pw), int(top + (y + 0.5) * lh)]


def near(a, b, tol=24):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def report(name, bad, n):
    print(f"{name}: {n - len(bad)} of {n} points right" + (f", e.g. {bad[:3]}" if bad else ""))
    return 1 if bad else 0


def blocks(a):
    w, depth, pic = int(a[2]), int(a[3]), int(a[4])
    first, last = (int(a[5]), int(a[6])) if len(a) > 6 else (0, 255)
    P = load(a[0], a[1], w)
    bw = w // 8
    bad, n = [], 0
    for by in range(8):
        for bx in range(8):
            x, y = bx * bw + bw // 2, by * 32 + 16
            if not first <= y <= last:
                continue
            want = spal((bx + 8 * by + pic) % (1 << depth))
            n += 1
            if not near(P(x, y), want):
                bad.append((x, y, P(x, y), want))
    return report("blocks", bad, n)


def bars(a):
    P = load(a[0], a[1])
    gun = "rgb".index(a[2])
    bad = []
    for y in range(32):
        want = [0, 0, 0]
        want[gun] = (y * 8) & 255
        if not near(P(20, y), want, 12):
            bad.append((y, P(20, y), tuple(want)))
    return report("bars", bad, 32)


def mirror(a):
    P = load(a[0], a[1])
    bad, n = [], 0
    for y in range(132, 256, 8):
        src = 127 - (y - 128)
        for bx in range(8):
            want = spal((bx + 8 * (src // 32)) % 16)
            n += 1
            if not near(P(bx * 40 + 20, y), want):
                bad.append((bx * 40 + 20, y, P(bx * 40 + 20, y), want))
    return report("mirror", bad, n)


def fetch(a):
    w = int(a[2])
    P = load(a[0], a[1], w)
    pl = w // 8
    on, off = spal(1), spal(0)
    bad, n = [], 0
    for y in range(0, 256):
        if y % 4 not in (1, 2):         # the middle lines of each run of four (the screenshot blurs lines together)
            continue
        b = (y // 4) % pl
        for x, want in ((b * 8 + 4, on), (((b + 2) % pl) * 8 + 4, off), (((b - 2) % pl) * 8 + 4, off)):
            n += 1
            if not near(P(x, y), want):
                bad.append((x, y, P(x, y), want))
    return report("fetch", bad, n)


def ham(a):
    import mkpics
    P = load(a[0], a[1])
    if a[2] == "8":
        w, h, idx, pal = mkpics.ham8_pic()
    else:
        w, h = 320, 64
        idx, pal = [], [(0, 0, 0)] * 16
        for y in range(h):
            for x in range(w):
                idx.append(0x20 | (x * 16 // w) if x % 2 == 0 else 0x10 | (y * 16 // h))
    exp = mkpics.exp_ham(w, h, idx, pal, a[2] == "8")
    bad, n = [], 0
    for y in range(0, h, 4):
        for x in (range(5, w, 8) if a[2] == "8" else range(1, w, 2)):   # HAM8: the middle of each green run
            want = tuple(exp[3 * (y * w + x):3 * (y * w + x) + 3])
            n += 1
            if not near(P(x, y), want):
                bad.append((x, y, P(x, y), want))
    r = report("ham", bad, n)           # (the left edge blurs into the border in the screenshot: at most 1%)
    return 0 if len(bad) * 100 <= n else r


def logo(a):
    import mkintro
    P = load(a[0], a[1])
    pix = mkintro.logo()
    bad, n = [], 0
    for y in range(mkintro.LH):
        for x in range((y * 3) % 7, mkintro.LW, 7):
            pen = pix[y][x]
            if not pen:
                continue
            c = mkintro.PAL[pen]
            want = (c >> 16, c >> 8 & 255, c & 255)
            n += 1
            if not near(P(x, 8 + y), want):
                bad.append((x, 8 + y, P(x, 8 + y), want))
    r = report("logo", bad, n)          # (single-pixel features blur into their neighbours in the screenshot's
    return 0 if len(bad) * 100 <= n else r  # scaling: at most 1% of the points may)


def px(a):
    P = load(a[0], a[1], int(a[2]))
    bad = []
    for spec in a[3:]:
        x, y, c = spec.split(",")
        want = tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))
        if not near(P(int(x), int(y)), want):
            bad.append((x, y, P(int(x), int(y)), want))
    return report("px", bad, len(a) - 3)


def main(a):
    f = {"px": px, "logo": logo, "blocks": blocks, "bars": bars, "mirror": mirror, "fetch": fetch, "ham": ham}.get(a[0] if a else "")
    if not f:
        sys.exit(__doc__)
    return f(a[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
