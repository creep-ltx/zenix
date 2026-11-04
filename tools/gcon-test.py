#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checks for tools/gcon-test.sh (DESIGN D38 step 2), on screenshots taken by tools/shot.py, with the geometry that
tools/colour-test.py columns found ("x0 x1 yc h": an Amiga pixel is (x1 - x0 + 1) / 640 wide, a line h / 8 tall).

  gcon-test.py grabbed SHOT GEOM X Y   gcontest's screen: green everywhere but its 16 x 16 red pointer (32 hires pixels
                                       by 16 lines), whose top left corner is at hires pixel X, line Y (within one
                                       lores pixel and one line); prints where it is
  gcon-test.py text SHOT GEOM          the text console: no green and no red anywhere in the display window
Each prints what it found and exits 1 on a failure."""
import sys
from PIL import Image

GREEN, RED = (0, 255, 0), (255, 0, 0)


def near(a, b, tol=40):
    return all(abs(p - q) <= tol for p, q in zip(a, b))


def geometry(path):
    x0, x1, yc, h = (float(v) for v in open(path).read().split()[:4])
    return x0, (x1 - x0 + 1) / 640, yc - 7.5 * h, h / 8


def sample(px, g, X, Y):
    x0, pw, top, lh = g
    return px[int(x0 + (X + 0.5) * pw), int(top + (Y + 0.5) * lh)]


def grabbed(shot, geom, X, Y):
    im = Image.open(shot).convert("RGB")
    px, g = im.load(), geometry(geom)
    reds = [(x, y) for y in range(256) for x in range(0, 640, 2) if near(sample(px, g, x, y), RED)]
    if not reds:
        print("grabbed: no red pointer"); return 1
    xs, ys = [p[0] for p in reds], [p[1] for p in reds]
    bx, by, bw, bh = min(xs), min(ys), max(xs) - min(xs) + 2, max(ys) - min(ys) + 1
    print(f"grabbed: the pointer at {bx},{by}, {bw} x {bh} (wanted {X},{Y}, 32 x 16)")
    bad = 0
    if abs(bx - X) > 2 or abs(by - Y) > 1 or abs(bw - 32) > 2 or abs(bh - 16) > 1:   # the scaled screenshot blurs an edge line
        bad = 1
    other = [(x, y) for y in range(0, 256, 4) for x in range(0, 640, 8)
             if not (bx - 2 <= x < bx + bw + 2 and by - 1 <= y <= by + bh + 1) and not near(sample(px, g, x, y), GREEN)]
    if other:
        print(f"grabbed: {len(other)} samples are not green, e.g. {other[:3]} {sample(px, g, *other[0])}")
        bad = 1
    return bad


def text(shot, geom):
    im = Image.open(shot).convert("RGB")
    px, g = im.load(), geometry(geom)
    odd = [(x, y) for y in range(0, 256, 2) for x in range(0, 640, 4)
           if near(sample(px, g, x, y), GREEN, 60) or near(sample(px, g, x, y), RED, 60)]
    if odd:
        print(f"text: {len(odd)} samples green or red, e.g. {odd[:3]}"); return 1
    print("text: no green, no red")
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "grabbed":
        sys.exit(grabbed(a[1], a[2], int(a[3]), int(a[4])))
    if a[0] == "text":
        sys.exit(text(a[1], a[2]))
    sys.exit(__doc__)
