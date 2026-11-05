#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checks for tools/dpy-test.sh (DESIGN D38 steps 3-4) on screenshots taken by tools/shot.py, with the geometry that
tools/colour-test.py columns found ("x0 x1 yc h": an Amiga pixel is (x1 - x0 + 1) / 640 wide, a line h / 8 tall).

  dpy-test.py px SHOT GEOM X,Y,RGB ...   each Amiga pixel X, Y has the 12-bit colour RGB (within the screenshot's
                                         blur: the middle of the pixel, a tolerance of 40 a channel)
  dpy-test.py same A B GEOM [-x X,Y,W,H]... [-in X,Y,W,H]
                                         every Amiga pixel of shot B as in shot A (a tolerance of 40), outside the
                                         -x rectangles; with -in: also some pixel inside that rectangle differs
  dpy-test.py whole SHOT GEOM RGB W H    the pixels of colour RGB form one rectangle of W x H: its box H lines tall
                                         and W pixels wide within 2 (the screenshot's scaling blurs the sides), at
                                         least 98% of it that colour - a window's inside whole at one place (W 0: only
                                         its place and size printed)
  dpy-test.py title SHOT GEOM X0 X1 Y    the title bar's lines Y+2 .. Y+9: columns X0 .. X1-1 (the gap after a
                                         gadget) one colour, and the 12 columns after X1 hold another (the title)
Prints what it found and exits 1 on a failure."""
import sys
from PIL import Image


def geometry(path):
    x0, x1, yc, h = (float(v) for v in open(path).read().split()[:4])
    return x0, (x1 - x0 + 1) / 640, yc - 7.5 * h, h / 8


def rgb12(v):
    v = int(v, 16)
    return tuple(((v >> s) & 15) * 17 for s in (8, 4, 0))


def load(path, geom):
    im = Image.open(path).convert("RGB")
    x0, pw, top, lh = geometry(geom)
    px = im.load()
    return lambda x, y: px[int(x0 + (x + 0.5) * pw), int(top + (y + 0.5) * lh)]


def rect(v):
    x, y, w, h = (int(t) for t in v.split(","))
    return x, y, x + w, y + h


def same(a):
    A, B = load(a[0], a[2]), load(a[1], a[2])
    ex, inside, i = [], None, 3
    while i < len(a):
        if a[i] == "-x":
            ex.append(rect(a[i + 1]))
        elif a[i] == "-in":
            inside = rect(a[i + 1])
        i += 2
    bad, changed = [], 0
    for y in range(256):
        for x in range(640):
            pa, pb = A(x, y), B(x, y)
            diff = any(abs(p - q) > 40 for p, q in zip(pa, pb))
            if inside and inside[0] <= x < inside[2] and inside[1] <= y < inside[3]:
                changed += diff
                continue
            if diff and not any(r[0] <= x < r[2] and r[1] <= y < r[3] for r in ex):
                bad.append((x, y, pa, pb))
    print(f"same: {len(bad)} pixels differ outside" + (f", {changed} inside" if inside else "") +
          (f", e.g. {bad[:3]}" if bad else ""))
    return 1 if bad or (inside and not changed) else 0


def title(a):
    P = load(a[0], a[1])
    x0, x1, y = int(a[2]), int(a[3]), int(a[4])
    gap = {P(x, yy) for x in range(x0, x1) for yy in range(y + 2, y + 10)}
    ref = P(x0, y + 5)
    after = [P(x, yy) for x in range(x1, x1 + 12) for yy in range(y + 2, y + 10)]
    other = [c for c in after if any(abs(p - q) > 40 for p, q in zip(c, ref))]
    near = all(all(abs(p - q) <= 40 for p, q in zip(c, ref)) for c in gap)
    print(f"title: the gap {x0}..{x1 - 1} {'one colour' if near else 'not one colour: ' + str(sorted(gap)[:4])}, "
          f"{len(other)} title pixels after it")
    return 0 if near and other else 1


def whole(a):
    P = load(a[0], a[1])
    want, w, h = rgb12(a[2]), int(a[3]), int(a[4])
    xs, ys, n = [], [], 0
    for y in range(256):
        for x in range(640):
            if all(abs(p - q) <= 40 for p, q in zip(P(x, y), want)):
                xs.append(x)
                ys.append(y)
                n += 1
    if not n:
        print("whole: no pixel of that colour")
        return 1
    x0, y0, x1, y1 = min(xs), min(ys), max(xs) + 1, max(ys) + 1
    fill = n / ((x1 - x0) * (y1 - y0))
    print(f"whole: at {x0},{y0} {x1 - x0}x{y1 - y0} ({fill:.0%} of it the colour)")
    return 0 if abs(x1 - x0 - w) <= 2 and y1 - y0 == h and fill >= 0.98 else 1


def main(a):
    if a[0] == "whole":
        return whole(a[1:])
    if a[0] == "same":
        return same(a[1:])
    if a[0] == "title":
        return title(a[1:])
    if a[0] != "px":
        sys.exit(__doc__)
    im = Image.open(a[1]).convert("RGB")
    px, (x0, pw, top, lh) = im.load(), geometry(a[2])
    bad = 0
    for spec in a[3:]:
        x, y, want = spec.split(",")
        got = px[int(x0 + (int(x) + 0.5) * pw), int(top + (int(y) + 0.5) * lh)]
        ok = all(abs(p - q) <= 40 for p, q in zip(got, rgb12(want)))
        if not ok:
            print(f"px: {x},{y} is {got}, wanted {rgb12(want)} ({want})")
            bad = 1
    if not bad:
        print(f"px: {len(a) - 3} pixels as wanted")
    return bad


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
