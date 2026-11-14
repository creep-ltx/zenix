#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The terminal the BSD games' curses writes for (tools/games-host-test.sh): the escapes curses.c sends (CSI row;col H,
CSI H, CSI 2 J, CSI K, CSI m with 0 1 4 7, CSI ?25 l/h, CR, LF) and UTF-8 text, the cursor's wrap deferred at the
last column as the console's (kernel/term.c). Reads the output of tools/tests/games/cursestest.c and its frames:
each frame (ended by CSI 999 Z) must equal the screen, and between CSI 999 Z and CSI 998 Z nothing may be written.
  screen.py OUTPUT FRAMES LINES COLS   -> "screen: N frames equal, B bytes" or the first difference, exit 1"""
import re
import sys

out = open(sys.argv[1], "rb").read().decode("utf-8")
frames = open(sys.argv[2], encoding="latin-1").read().split("==\n")[:-1]
L, C = int(sys.argv[3]), int(sys.argv[4])
scr = [[" "] * C for _ in range(L)]
att = [["0"] * C for _ in range(L)]
y = x = 0
pend = False
a = 0
i = 0
n = 0
tok = re.compile(r"\x1b\[([?0-9;]*)([A-Za-z])|([\r\n])|(.)", re.S)
quiet = None
for m in tok.finditer(out):
    p, f, ctl, ch = m.groups()
    if quiet is not None and (f != "Z" or p != "998"):
        print("screen: frame %d: a refresh with nothing changed wrote %r" % (n, m.group(0)))
        sys.exit(1)
    if f:
        if f == "Z" and p == "999":
            want = frames[n].split("\n")
            for yy in range(L):
                got = "".join(scr[yy]) + "|~" + "".join(att[yy])
                if got != want[yy]:
                    print("screen: frame %d line %d differs:\n  want %r\n  got  %r" % (n, yy, want[yy], got))
                    sys.exit(1)
            n += 1
            quiet = n
        elif f == "Z" and p == "998":
            quiet = None
        elif f == "H":
            v = [int(t) for t in p.split(";")] if p else [1, 1]
            y, x, pend = v[0] - 1, v[1] - 1, False
            if not (0 <= y < L and 0 <= x < C):
                print("screen: a cursor move off the screen: %r" % m.group(0))
                sys.exit(1)
        elif f == "J" and p == "2":
            scr = [[" "] * C for _ in range(L)]
            att = [["0"] * C for _ in range(L)]
        elif f == "K":
            for xx in range(x, C):
                scr[y][xx], att[y][xx] = " ", "0"
        elif f == "m":
            a = 0
            for t in (p or "0").split(";"):
                a |= {"0": 0, "1": 2, "4": 4, "7": 1}[t]
        elif f in "lh" and p == "?25":
            pass
        else:
            print("screen: an escape this terminal does not know: %r" % m.group(0))
            sys.exit(1)
    elif ctl == "\r":
        x, pend = 0, False
    elif ctl == "\n":
        y = min(y + 1, L - 1)
        pend = False
    else:
        if pend:
            x, y, pend = 0, y + 1, False
            if y >= L:
                print("screen: the screen would scroll")
                sys.exit(1)
        scr[y][x] = ch.encode("latin-1", "replace").decode("latin-1")
        att[y][x] = "%d" % a
        if x == C - 1:
            pend = True
        else:
            x += 1
if n != len(frames):
    print("screen: %d frames in the output, %d meant" % (n, len(frames)))
    sys.exit(1)
print("screen: %d frames equal, %d bytes" % (n, len(out.encode("utf-8"))))
