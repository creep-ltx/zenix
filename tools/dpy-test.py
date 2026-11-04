#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Checks for tools/dpy-test.sh (DESIGN D38 steps 3-4) on screenshots taken by tools/shot.py, with the geometry that
tools/colour-test.py columns found ("x0 x1 yc h": an Amiga pixel is (x1 - x0 + 1) / 640 wide, a line h / 8 tall).

  dpy-test.py px SHOT GEOM X,Y,RGB ...   each Amiga pixel X, Y has the 12-bit colour RGB (within the screenshot's
                                         blur: the middle of the pixel, a tolerance of 40 a channel)
Prints what it found and exits 1 on a failure."""
import sys
from PIL import Image


def geometry(path):
    x0, x1, yc, h = (float(v) for v in open(path).read().split()[:4])
    return x0, (x1 - x0 + 1) / 640, yc - 7.5 * h, h / 8


def rgb12(v):
    v = int(v, 16)
    return tuple(((v >> s) & 15) * 17 for s in (8, 4, 0))


def main(a):
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
