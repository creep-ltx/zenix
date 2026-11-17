#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The Zenix intro's data (DESIGN D55 section 13, docs/d55-screens.md), all made here from numbers: no third-party
graphics or music in the repository.

  tools/mkintro.py header OUT.h    the C header zintro.c includes: the logo's planes and palette, the scroller's font
                                   and text, a sine table, the module (each packed with ByteRun1 where it pays)
  tools/mkintro.py mod OUT.mod     the module alone (to listen to it on the PC, or with modplay)
  tools/mkintro.py logo OUT.png    the logo as a picture (to look at)

The logo: ZENIX in angular letters (polygons), bevelled steel in eight bands, a toxic green rim, a hard shadow and
scratches. The font: 16 x 16 blocky capitals. The module: 4 channels, A minor (Am F Dm E), 100 BPM: noise drums, a sub
bass, a lead with vibrato, a pad of arpeggios; the samples generated waves."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkmod  # noqa: E402

LW, LH = 320, 64                       # the logo's band: 320 x 64, 4 planes
PAL = [0x05080C, 0x0C1014, 0xD8E0EA, 0xB0BAC6, 0x8A95A2, 0x66707C, 0x9CA8B4, 0x707A86, 0x48515C, 0x2A3038,
       0xF4FAFF, 0x020304, 0x3CFF5A, 0x0E4A18, 0x1A2026, 0x000000]
# 0 the background (the Copper's bars), 1 a scratch, 2-9 the steel bands top to bottom (a chrome's horizon between 5
# and 6), 10 the lit edge, 11 the shadow, 12 the rim, 13 the rim's glow, 14 the dark edge


def letters():
    """ZENIX as polygons in a 48-line box; (width, polygons)"""
    Z = (56, [[(6, 0), (56, 0), (56, 12), (19, 36), (56, 36), (56, 42), (50, 48), (0, 48), (0, 36), (37, 12),
               (0, 12), (0, 6)]])
    E = (52, [[(6, 0), (52, 0), (52, 10), (14, 10), (14, 19), (42, 19), (42, 29), (14, 29), (14, 38), (52, 38),
               (52, 48), (0, 48), (0, 6)]])
    N = (56, [[(0, 0), (14, 0), (42, 28), (42, 0), (56, 0), (56, 48), (42, 48), (14, 20), (14, 48), (0, 48)]])
    I = (16, [[(0, 0), (16, 0), (16, 42), (10, 48), (0, 48)]])
    X = (58, [[(0, 0), (15, 0), (29, 16), (43, 0), (58, 0), (37, 24), (58, 48), (43, 48), (29, 32), (15, 48),
               (0, 48), (21, 24)]])
    return [Z, E, N, I, X]


def inside(poly, x, y):
    c = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            c = not c
    return c


def logo():
    """-> 64 rows of 320 pens"""
    ls = letters()
    gap = 10
    total = sum(w for w, _ in ls) + gap * (len(ls) - 1)
    x0, y0 = (LW - total) // 2, 6
    mask = [[0] * LW for _ in range(LH)]
    x = x0
    for w, polys in ls:
        for yy in range(48):
            for xx in range(w):
                if any(inside(p, xx + 0.5, yy + 0.5) for p in polys):
                    mask[y0 + yy][x + xx] = 1
        x += w + gap
    m = lambda xx, yy: 0 <= xx < LW and 0 <= yy < LH and mask[yy][xx]
    rnd = 0x1234567
    out = [[0] * LW for _ in range(LH)]
    for yy in range(LH):
        for xx in range(LW):
            if m(xx, yy):
                band = min(7, max(0, (yy - y0) * 8 // 48))
                pen = 2 + band
                if not m(xx, yy - 1) or not m(xx - 1, yy):
                    pen = 10
                elif not m(xx, yy + 1) or not m(xx + 1, yy):
                    pen = 14
                else:
                    rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
                    if (rnd >> 16) % 61 == 0 or ((xx * 7 + yy * 3) % 97 == 0):
                        pen = 1         # scratches: the steel is old
                out[yy][xx] = pen
            elif any(m(xx + dx, yy + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1)):
                out[yy][xx] = 12        # the toxic rim
            elif any(m(xx + dx, yy + dy) for dx in (-2, 0, 2) for dy in (-2, 0, 2)):
                out[yy][xx] = 13
            elif m(xx - 4, yy - 3):
                out[yy][xx] = 11        # a hard shadow, down and right
    return out


def planes(pix, depth):
    """rows of pens -> interleaved planes (each line: plane 0's bytes, then plane 1's ...)"""
    w = len(pix[0])
    out = bytearray()
    for row in pix:
        for k in range(depth):
            b = bytearray(w // 8)
            for x, p in enumerate(row):
                if p >> k & 1:
                    b[x >> 3] |= 0x80 >> (x & 7)
            out += b
    return bytes(out)


def byterun1(data):
    out, i, n = bytearray(), 0, len(data)
    while i < n:
        j = i
        while j + 1 < n and data[j + 1] == data[i] and j - i < 127:
            j += 1
        if j - i >= 2:
            out += bytes([(257 - (j - i + 1)) & 255, data[i]])
            i = j + 1
            continue
        j = i
        while j < n and j - i < 128 and not (j + 2 < n and data[j] == data[j + 1] == data[j + 2]):
            j += 1
        out += bytes([j - i - 1]) + data[i:j]
        i = j
    return bytes(out)


# ---- the font: 16 x 16, drawn from 8 x 8 strings doubled, with a cut corner ----
GLYPHS = {
    'A': ["..####..", ".##..##.", "##....##", "##....##", "########", "##....##", "##....##", "##....##"],
    'B': ["######..", "##...##.", "##...##.", "######..", "##...##.", "##....##", "##...##.", "######.."],
    'C': ["..#####.", ".##...##", "##......", "##......", "##......", "##......", ".##...##", "..#####."],
    'D': ["#####...", "##..##..", "##...##.", "##....##", "##....##", "##...##.", "##..##..", "#####..."],
    'E': ["########", "##......", "##......", "######..", "##......", "##......", "##......", "########"],
    'F': ["########", "##......", "##......", "######..", "##......", "##......", "##......", "##......"],
    'G': ["..#####.", ".##.....", "##......", "##..####", "##....##", "##....##", ".##...##", "..#####."],
    'H': ["##....##", "##....##", "##....##", "########", "##....##", "##....##", "##....##", "##....##"],
    'I': ["..####..", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "..####.."],
    'J': ["....####", "......##", "......##", "......##", "......##", "##....##", ".##..##.", "..####.."],
    'K': ["##...##.", "##..##..", "##.##...", "####....", "##.##...", "##..##..", "##...##.", "##....##"],
    'L': ["##......", "##......", "##......", "##......", "##......", "##......", "##......", "########"],
    'M': ["##....##", "###..###", "########", "##.##.##", "##....##", "##....##", "##....##", "##....##"],
    'N': ["##....##", "###...##", "####..##", "##.##.##", "##..####", "##...###", "##....##", "##....##"],
    'O': ["..####..", ".##..##.", "##....##", "##....##", "##....##", "##....##", ".##..##.", "..####.."],
    'P': ["######..", "##...##.", "##....##", "##...##.", "######..", "##......", "##......", "##......"],
    'Q': ["..####..", ".##..##.", "##....##", "##....##", "##.##.##", "##..###.", ".##..##.", "..###.##"],
    'R': ["######..", "##...##.", "##....##", "##...##.", "######..", "##.##...", "##..##..", "##...###"],
    'S': [".######.", "##......", "##......", ".#####..", "......##", "......##", "......##", "#######."],
    'T': ["########", "...##...", "...##...", "...##...", "...##...", "...##...", "...##...", "...##..."],
    'U': ["##....##", "##....##", "##....##", "##....##", "##....##", "##....##", ".##..##.", "..####.."],
    'V': ["##....##", "##....##", "##....##", "##....##", ".##..##.", ".##..##.", "..####..", "...##..."],
    'W': ["##....##", "##....##", "##....##", "##.##.##", "########", "###..###", "##....##", "##....##"],
    'X': ["##....##", ".##..##.", "..####..", "...##...", "..####..", ".##..##.", "##....##", "##....##"],
    'Y': ["##....##", "##....##", ".##..##.", "..####..", "...##...", "...##...", "...##...", "...##..."],
    'Z': ["########", ".....##.", "....##..", "...##...", "..##....", ".##.....", "##......", "########"],
    '0': ["..####..", ".##..##.", "##...###", "##..####", "####..##", "###...##", ".##..##.", "..####.."],
    '1': ["...##...", "..###...", ".####...", "...##...", "...##...", "...##...", "...##...", ".######."],
    '2': [".#####..", "##...##.", ".....##.", "....##..", "...##...", "..##....", ".##.....", "#######."],
    '3': [".#####..", "##...##.", ".....##.", "...###..", ".....##.", ".....##.", "##...##.", ".#####.."],
    '4': ["....##..", "...###..", "..####..", ".##.##..", "##..##..", "#######.", "....##..", "....##.."],
    '5': ["#######.", "##......", "######..", ".....##.", ".....##.", ".....##.", "##...##.", ".#####.."],
    '6': ["..####..", ".##.....", "##......", "######..", "##...##.", "##...##.", "##...##.", ".#####.."],
    '7': ["#######.", ".....##.", "....##..", "...##...", "..##....", "..##....", "..##....", "..##...."],
    '8': [".#####..", "##...##.", "##...##.", ".#####..", "##...##.", "##...##.", "##...##.", ".#####.."],
    '9': [".#####..", "##...##.", "##...##.", ".######.", ".....##.", ".....##.", "....##..", ".####..."],
    '.': ["........", "........", "........", "........", "........", "........", "...##...", "...##..."],
    ',': ["........", "........", "........", "........", "........", "...##...", "...##...", "..##...."],
    '!': ["...##...", "...##...", "...##...", "...##...", "...##...", "........", "...##...", "...##..."],
    '?': [".#####..", "##...##.", ".....##.", "....##..", "...##...", "........", "...##...", "...##..."],
    '-': ["........", "........", "........", ".######.", "........", "........", "........", "........"],
    ':': ["........", "...##...", "...##...", "........", "........", "...##...", "...##...", "........"],
    '/': [".......#", "......##", ".....##.", "....##..", "...##...", "..##....", ".##.....", "##......"],
    "'": ["...##...", "...##...", "..##....", "........", "........", "........", "........", "........"],
    ' ': ["........"] * 8,
}
FONT_CHARS = "".join(sorted(GLYPHS))


def font():
    """-> FONT_CHARS' glyphs, 32 bytes each: 16 rows of 16 bits (each 8 x 8 pixel doubled)"""
    out = bytearray()
    for ch in FONT_CHARS:
        g = GLYPHS[ch]
        for row in g:
            v = 0
            for x, c in enumerate(row):
                if c == '#':
                    v |= 3 << (14 - 2 * x)
            out += struct.pack('>H', v) * 2
    return bytes(out)


TEXT = ("ZENIX  ...  NO KICKSTART, NO WORKBENCH, NO MERCY.  THE MACHINE WAKES IN THE DARK AND FINDS NOTHING "
        "ABOVE IT.  YOUR CODE RUNS WHERE THE OLD GODS USED TO LIVE.  THE COPPER STILL BURNS, THE BLITTER STILL "
        "WAITS, PAULA STILL SINGS TO NOBODY.  EVERY BYTE CHECKED, EVERY POINTER COUNTED.  NOTHING HERE IS "
        "FORGIVEN.  PRESS ESC TO CRAWL BACK TO THE DESKTOP.  OR STAY.  THE MACHINE DOES NOT CARE.  ...  "
        "MADE ON ZENIX, FOR THE AMIGA, IN THE YEAR THE LIGHTS WENT OUT.        ")


# ---- the module ----
P = {}
for o, row in enumerate([[856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453],
                         [428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226],
                         [214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]]):
    for i, n in enumerate(["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]):
        P[f"{n}{o + 1}"] = row[i]

RATE = 3546895 // 428                  # a sample's own rate at C-2


def s8(v):
    return max(-127, min(127, int(round(v)))) & 255


def samples():
    rnd = [0x5EED]

    def noise():
        rnd[0] = (rnd[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return ((rnd[0] >> 16) & 255) - 128
    kick = bytearray()
    ph = 0.0
    for i in range(1800):
        t = i / RATE
        f = 45 + 160 * math.exp(-t * 28)
        ph += 2 * math.pi * f / RATE
        kick.append(s8(126 * math.sin(ph) * math.exp(-t * 7) + (noise() * 0.3 if i < 40 else 0)))
    snare = bytearray()
    lp = 0.0
    for i in range(1900):
        t = i / RATE
        lp = lp * 0.4 + noise() * 0.6
        snare.append(s8((lp * 0.9 + 60 * math.sin(2 * math.pi * 185 * t)) * math.exp(-t * 14)))
    hat = bytearray()
    prev = 0
    for i in range(520):
        n = noise()
        hat.append(s8((n - prev) * 0.5 * math.exp(-i / 90)))
        prev = n
    # a sub bass: one cycle of 64, a square with a rounded saw under it (gritty, not clean)
    bass = bytes(s8(70 * (1 if i < 32 else -1) + 40 * math.sin(2 * math.pi * i / 64) + (i - 32) * 0.9)
                 for i in range(64))
    # the lead: one cycle of 64, a narrow pulse with a saw: thin and cold
    lead = bytes(s8((90 if i < 18 else -40) + (i - 32) * 1.2) for i in range(64))
    # the pad: 4096 samples holding 64 and 65 cycles of a soft saw: a slow chorus that loops cleanly
    pad = bytes(s8(38 * (((i * 64 / 4096) % 1) * 2 - 1) + 38 * (((i * 65 / 4096) % 1) * 2 - 1))
                for i in range(4096))
    for d in (kick, snare, hat):
        d[0] = d[1] = 0
        if len(d) & 1:
            d.append(0)
    # (data, finetune, volume, loop start, loop length in bytes)
    return [(bytes(kick), 0, 64, 0, 0), (bytes(snare), 0, 52, 0, 0), (bytes(hat), 0, 30, 0, 0),
            (bass, 0, 58, 0, 64), (lead, 0, 40, 0, 64), (pad, 0, 34, 0, 4096)]


KICK, SNARE, HAT, BASS, LEAD, PAD = 1, 2, 3, 4, 5, 6
CHORDS = [("A-1", 0x37), ("F-1", 0x47), ("D-1", 0x37), ("E-1", 0x47)]   # Am F Dm E: the harmonic minor's dark turn
MEL_A = {0: "A-2", 6: "C-3", 8: "E-3", 12: "D-3", 16: "C-3", 22: "A-2", 24: "F-2", 28: "G-2",
         32: "F-2", 36: "A-2", 40: "D-3", 44: "C-3", 48: "B-2", 52: "G#2", 56: "E-2", 60: "G#2"}
MEL_B = {0: "E-3", 4: "D-3", 6: "C-3", 8: "E-3", 12: "A-3", 16: "G-3", 20: "F-3", 22: "E-3", 24: "C-3",
         32: "D-3", 36: "F-3", 40: "A-3", 44: "G-3", 46: "F-3", 48: "E-3", 52: "G#3", 56: "B-3", 60: "E-3"}


def up(note):                          # an octave up: half the period
    return P[note] // 2


def pattern(cells, p, drums, bass, lead, pad):
    for r in range(64):
        root, arp = CHORDS[r // 16]
        b = r % 16
        if drums:
            if b in (0, 6, 10) or (drums > 1 and b == 14):
                cells[(p, r, 0)] = (KICK, 428, 0, 0)
            elif b in (4, 12):
                cells[(p, r, 0)] = (SNARE, 428, 0, 0)
            elif drums > 1 or b % 4 == 2:
                cells[(p, r, 0)] = (HAT, 428 if b % 4 == 2 else 404, 0, 0)
        if bass and r % 2 == 0:
            per = up(root) if (r % 8 == 6 and bass > 1) else P[root]
            cells[(p, r, 1)] = (BASS, per, 0xC, 52 if r % 4 else 60)
        if lead:
            mel = MEL_A if lead == 1 else MEL_B
            if r in mel:
                cells[(p, r, 2)] = (LEAD, P[mel[r]], 0, 0)
            else:
                cells[(p, r, 2)] = (0, 0, 4, 0x34)      # vibrato while it holds
        if pad:
            if b == 0:
                cells[(p, r, 3)] = (PAD, P[root.replace("-1", "-2")], 0, arp)   # an octave above the bass
            else:
                cells[(p, r, 3)] = (0, 0, 0, arp)       # the arpeggio goes on: the chord on one channel


def module():
    cells = {}
    pattern(cells, 0, 0, 1, 0, 1)                   # the machine wakes: bass and pad
    pattern(cells, 1, 1, 1, 0, 1)                   # the drums come in
    pattern(cells, 2, 1, 2, 1, 1)                   # the lead
    pattern(cells, 3, 2, 2, 2, 1)                   # higher, heavier
    pattern(cells, 4, 0, 2, 0, 1)                   # the breakdown: bass, pad
    pattern(cells, 5, 2, 2, 1, 0)                   # no pad: bare
    cells[(0, 0, 0)] = (0, 0, 0xF, 0x64)            # 100 BPM (speed 6)
    order = [0, 1, 2, 3, 4, 1, 2, 5, 3]
    return mkmod.module(cells, samples(), order, title=b'zenix: no mercy')


def sine():
    return [int(round(16384 * math.sin(2 * math.pi * i / 256))) for i in range(256)]


def carr(name, data, kind="unsigned char"):
    lines = [f"static const {kind} {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(str(v) for v in data[i:i + 24]) + ",")
    lines.append("};")
    return "\n".join(lines)


def header(out):
    lg = planes(logo(), 4)
    mod = module()
    f = font()
    plg, pmod = byterun1(lg), byterun1(mod)
    with open(out, "w") as o:
        o.write("/* made by tools/mkintro.py (DESIGN D55 13): do not edit */\n")
        o.write(f"#define LOGO_W {LW}\n#define LOGO_H {LH}\n#define LOGO_BYTES {len(lg)}\n#define MOD_BYTES {len(mod)}\n")
        o.write(f"#define FONT_N {len(FONT_CHARS)}\n")
        o.write(carr("logo_pk", plg) + "\n")
        o.write(carr("logo_pal", PAL, "unsigned") + "\n")
        o.write(carr("mod_pk", pmod) + "\n")
        o.write(carr("font16", f) + "\n")
        o.write("static const char font_chars[] = " + '"' + FONT_CHARS.replace("'", "\\'") + '"' + ";\n")
        o.write("static const char scroll_text[] = \"" + TEXT + "\";\n")
        o.write(carr("sine", sine(), "short") + "\n")
    print(f"{out}: logo {len(lg)} -> {len(plg)} bytes, module {len(mod)} -> {len(pmod)} bytes, font {len(f)} bytes")


def png(out):
    from PIL import Image
    pix = logo()
    im = Image.new("RGB", (LW * 2, LH * 2))
    for y in range(LH):
        for x in range(LW):
            c = PAL[pix[y][x]]
            for dx in (0, 1):
                for dy in (0, 1):
                    im.putpixel((2 * x + dx, 2 * y + dy), (c >> 16, c >> 8 & 255, c & 255))
    im.save(out)


if __name__ == "__main__":
    if len(sys.argv) != 3 or sys.argv[1] not in ("header", "mod", "logo"):
        sys.exit(__doc__)
    if sys.argv[1] == "header":
        header(sys.argv[2])
    elif sys.argv[1] == "mod":
        open(sys.argv[2], "wb").write(module())
    else:
        png(sys.argv[2])
