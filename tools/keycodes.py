#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Text as Amiga raw key codes for `gcontest keys` (the desktop's tests, DESIGN D38): each character's key down and
up, with Shift around it when the keymap needs it, read from kernel/kbd.c's keymap.

usage: keycodes.py [-m sv|us] [-c N] TEXT  ->  lines of hex codes, each at most N codes (-c), split only between
                                               characters ("\\n" in TEXT is Return)"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def keymap(name):
    src = open(os.path.join(ROOT, "kernel", "kbd.c"), encoding="latin-1").read()
    i = src.index('{ "%s",' % name)
    body = src[i:]
    layers = []
    for m in re.finditer(r"\{ /\*[^*]*\*/(.*?)\}", body, re.S):
        vals = []
        for t in re.findall(r"'(?:\\\\|\\'|[^'])'|0x[0-9A-Fa-f]+|\b0\b", m.group(1)):
            if t.startswith("'"):
                c = t[1:-1]
                vals.append(ord(c[1]) if c.startswith("\\") else ord(c))
            else:
                vals.append(int(t, 16) if t.startswith("0x") else 0)
        layers.append(vals[:0x40])
        if len(layers) == 2:
            break
    return layers


def codes(text, name="sv", chunk=0):
    normal, shifted = keymap(name)
    out = []
    lines = []
    for ch in text:
        if chunk and len(out) + 4 > chunk:      # a line ends between two characters: never inside Shift's codes
            lines.append(out)
            out = []
        if ch == "\n":
            out += [0x44, 0xC4]
            continue
        if ch == " ":
            out += [0x40, 0xC0]
            continue
        o = ord(ch)
        if o in normal:
            k = normal.index(o)
            out += [k, k | 0x80]
        elif o in shifted:
            k = shifted.index(o)
            out += [0x60, k, k | 0x80, 0xE0]
        else:
            sys.exit("keycodes: %r is not on the %s keymap's plain or Shift layer" % (ch, name))
    lines.append(out)
    return "\n".join(" ".join("%02x" % c for c in l) for l in lines if l)


if __name__ == "__main__":
    a = sys.argv[1:]
    m, chunk = "sv", 0
    while a[:1] in (["-m"], ["-c"]):
        if a[0] == "-m":
            m = a[1]
        else:
            chunk = int(a[1])
        a = a[2:]
    print(codes(" ".join(a).replace("\\n", "\n"), m, chunk))
