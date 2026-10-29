#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The console's Unicode font and width table (DESIGN D28 sections 6 and 9):
    mkconfont.py font OUT.zcf      the default font, zenix-8x8.zcf (ZCF1): box drawing, blocks and braille generated,
                                   CP437's other symbols from tools/font_8x8.c, the hand-drawn glyphs of
                                   tools/consolefont/symbols.glyphs, the stand-ins of tools/consolefont/standins
    mkconfont.py widths OUT.h      the kernel's width table (kernel/confont.c), from the C library's own tables
                                   (picolibc's combining.t and wide.t, libc/picolibc/newlib/libc/string)
    mkconfont.py check FILE.zcf    the kernel's checks of CON_UFONT (section 7), in Python: exit 1 and the reason
    mkconfont.py test              the tool's own tests (section 14): the generator against font_8x8.c (CP437's light
                                   box lines, blocks and shades), box arms meeting at the cell edges, the wanted list
                                   covered, the built file passing the checks, every glyph's width
The format, ZCF1 (all big-endian): a 32-byte header (magic, header size 32, flags 0, width 8, height 8, the glyph
count G 3-4,096, the half-page count P 0-255, reserved 0, the total size 32 + 512 + 256 P + 8 G, 12 reserved bytes
0), a 512-byte index (a byte per 128-code-point half-page of the BMP: 0 none, else 1 + its number; bytes 0 and 1
0), P half-pages of 128 16-bit entries (0 no glyph, 32-255 Topaz's glyph of that byte, 256 + n glyph n), and G
glyphs of 8 bytes (a byte a row, bit 7 the left pixel). Glyphs 0-2: the box, the wide box's left and right half."""
import os
import re
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
FONT_C = os.path.join(ROOT, "tools", "font_8x8.c")
CFDIR = os.path.join(ROOT, "tools", "consolefont")
PICO = os.path.join(ROOT, "libc", "picolibc", "newlib", "libc", "string")

HDR, INDEX = 32, 512
MAXG, MAXP = 4096, 255
MAXSIZE = HDR + INDEX + MAXP * 256 + MAXG * 8           # 98,848: CON_UFONT's bound


class Bad(Exception):
    pass


# ---- CP437 (font_8x8.c's order) as Unicode ----
CP437 = (
    "\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~⌂"
    "ÇüéâäàåçêëèïîìÄÅÉæÆôöòûùÿÖÜ¢£¥₧ƒáíóúñÑªº¿⌐¬½¼¡«»░▒▓│┤╡╢╖╕╣║╗╝╜╛┐└┴┬├─┼╞╟╚╔╩╦╠═╬╧╨╤╥╙╘╒╓╫╪┘┌█▄▌▐▀"
    "αßΓπΣσµτΦΘΩδ∞φε∩≡±≥≤⌠⌡÷≈°∙·√ⁿ²■\u00a0")
assert len(CP437) == 256


def read_font_8x8():
    data = []
    for n, line in enumerate(open(FONT_C, encoding="latin-1"), 1):
        m = re.match(r"\s*0x([0-9a-fA-F]{2}),\s*/\*\s*([01]{8})\s*\*/", line)
        if not m:
            continue
        v = int(m.group(1), 16)
        if v != int(m.group(2), 2):
            raise Bad(f"{FONT_C}:{n}: 0x{v:02x} does not match its picture")
        data.append(v)
    if len(data) != 2048:
        raise Bad(f"{FONT_C}: {len(data)} glyph bytes, not 2048")
    return [bytes(data[i * 8:i * 8 + 8]) for i in range(256)]


# ---- generated glyphs ----
def pic(rows):
    """a glyph from a set of (row, col) pixels"""
    g = [0] * 8
    for y, x in rows:
        g[y] |= 0x80 >> x
    return bytes(g)


def rect(y0, y1, x0, x1):
    return {(y, x) for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)}


# Box drawing U+2500-257F: the arms up, down, left, right as weights (0 none, 1 light, 2 heavy, 3 double); the
# dashes, arcs and diagonals by name. Doubles (all 29 are in CP437) are CP437's own glyphs.
BOX_ARMS = (
    "0011 0022 1100 2200 d3l d3h D3l D3h d4l d4h D4l D4h 0101 0102 0201 0202 "
    "0110 0120 0210 0220 1001 1002 2001 2002 1010 1020 2010 2020 1101 1102 2101 1201 "
    "2201 2102 1202 2202 1110 1120 2110 1210 2210 2120 1220 2220 0111 0121 0112 0122 "
    "0211 0221 0212 0222 1011 1021 1012 1022 2011 2021 2012 2022 1111 1121 1112 1122 "
    "2111 1211 2211 2121 2112 1221 1212 2122 1222 2221 2212 2222 d2l d2h D2l D2h "
    "0033 3300 0103 0301 0303 0130 0310 0330 1003 3001 3003 1030 3010 3030 1103 3301 "
    "3303 1130 3310 3330 0133 0311 0333 1033 3011 3033 1133 3311 3333 aDR aDL aUL "
    "aUR x/ x\\ xX 0010 1000 0001 0100 0020 2000 0002 0200 0012 1200 0021 2100").split()
assert len(BOX_ARMS) == 128

DASHES = {2: "###.###.", 3: "##.##.#.", 4: "#.#.#.#."}      # along the line: the pixels of one cell


def box_glyph(cp, cp437):
    code = BOX_ARMS[cp - 0x2500]
    px = set()
    if code[0] in "dD":                         # dashes: d horizontal, D vertical; l light, h heavy
        pat = DASHES[int(code[1])]
        heavy = code[2] == "h"
        for i, c in enumerate(pat):
            if c != "#":
                continue
            if code[0] == "d":
                px |= rect(3 if heavy else 4, 4, i, i)
            else:
                px |= rect(i, i, 2 if heavy else 3, 5 if heavy else 4)
        return pic(px)
    if code[0] == "a":                          # arcs: a corner with its corner pixel cut
        d, h = code[1], code[2]
        px |= rect(5, 7, 3, 4) if d == "D" else rect(0, 3, 3, 4)
        px |= rect(4, 4, 4, 7) if h == "R" else rect(4, 4, 0, 3)
        return pic(px)
    if code[0] == "x":                          # diagonals, one pixel wide
        if code[1] in "/X":
            px |= {(y, 7 - y) for y in range(8)}
        if code[1] in "\\X":
            px |= {(y, y) for y in range(8)}
        return pic(px)
    u, d, l, r = (int(c) for c in code)
    if 3 in (u, d, l, r):
        return cp437[chr(cp)]                   # double lines: CP437's own (Unicode's 29 are all there)
    vcols = {1: (3, 4), 2: (2, 5)}
    hrows = {1: (4, 4), 2: (3, 4)}
    if u:
        px |= rect(0, 4, *vcols[u])
    if d:
        px |= rect(3 if d == 2 else 4, 7, *vcols[d])
    if l:
        px |= rect(*hrows[l], 0, 5 if l == 2 else 4)
    if r:
        px |= rect(*hrows[r], 2 if r == 2 else 3, 7)
    return pic(px)


SHADES = {0x2591: "2288228822882288", 0x2592: "55aa55aa55aa55aa", 0x2593: "77dd77dd77dd77dd"}


def block_glyph(cp):
    if cp in SHADES:
        return bytes.fromhex(SHADES[cp])
    q = {"UL": rect(0, 3, 0, 3), "UR": rect(0, 3, 4, 7), "LL": rect(4, 7, 0, 3), "LR": rect(4, 7, 4, 7)}
    n = cp - 0x2580
    if n == 0:
        return pic(rect(0, 3, 0, 7))                            # upper half
    if 1 <= n <= 8:
        return pic(rect(8 - n, 7, 0, 7))                        # lower eighths, 8 the full block
    if 9 <= n <= 15:
        return pic(rect(0, 7, 0, 7 - (n - 8)))                  # left 7/8 .. 1/8
    if n == 16:
        return pic(rect(0, 7, 4, 7))                            # right half
    if n == 0x14:
        return pic(rect(0, 0, 0, 7))                            # upper eighth
    if n == 0x15:
        return pic(rect(0, 7, 7, 7))                            # right eighth
    quads = {0x16: "LL", 0x17: "LR", 0x18: "UL", 0x19: "UL LL LR", 0x1A: "UL LR", 0x1B: "UL UR LL",
             0x1C: "UL UR LR", 0x1D: "UR", 0x1E: "UR LL", 0x1F: "UR LL LR"}
    px = set()
    for k in quads[n].split():
        px |= q[k]
    return pic(px)


def braille_glyph(cp):
    bits = cp - 0x2800
    dots = [(0, 0), (1, 0), (2, 0), (0, 1), (1, 1), (2, 1), (3, 0), (3, 1)]      # dots 1-8: (row, column)
    px = set()
    for i, (r, c) in enumerate(dots):
        if bits >> i & 1:
            px |= rect(2 * r, 2 * r, 1 + 4 * c, 2 + 4 * c)
    return pic(px)


def read_symbols():
    """the hand-drawn glyphs: {"box"|"wide-left"|"wide-right"|code point: glyph}"""
    out, path = {}, os.path.join(CFDIR, "symbols.glyphs")
    lines = [l.rstrip("\n") for l in open(path, encoding="utf-8")]
    i = 0
    while i < len(lines):
        l = lines[i]
        if not l.strip() or l.startswith("#"):
            i += 1
            continue
        key = l.split()[0]
        if key.startswith("U+"):
            key = int(key[2:], 16)
        elif key not in ("box", "wide-left", "wide-right"):
            raise Bad(f"{path}:{i + 1}: not a glyph's name: {l}")
        rows = lines[i + 1:i + 9]
        if len(rows) != 8 or any(not re.fullmatch(r"[#.]{8}", r) for r in rows):
            raise Bad(f"{path}:{i + 1}: a glyph is eight lines of eight # or .")
        if key in out:
            raise Bad(f"{path}:{i + 1}: drawn twice")
        out[key] = bytes(int(r.replace("#", "1").replace(".", "0"), 2) for r in rows)
        i += 9
    return out


def read_cplist(name):
    """lines "U+XXXX ..." of a data file -> [(code point, rest of the line)]"""
    out = []
    for n, l in enumerate(open(os.path.join(CFDIR, name), encoding="utf-8"), 1):
        if not l.strip() or l.startswith("#"):
            continue
        m = re.match(r"U\+([0-9A-F]{4,6})\s*(.*)", l)
        if not m:
            raise Bad(f"{name}:{n}: not a line \"U+XXXX ...\"")
        out.append((int(m.group(1), 16), m.group(2)))
    return out


# ---- widths: picolibc's tables, the C library's wcwidth's own source ----
def read_t(name):
    s = open(os.path.join(PICO, name), encoding="utf-8").read()
    r = [(int(a, 16), int(b, 16)) for a, b in re.findall(r"\{\s*0x([0-9A-Fa-f]+),\s*0x([0-9A-Fa-f]+)\s*\}", s)]
    if not r:
        raise Bad(f"{name}: no ranges")
    return r


def merged(r):
    out = []
    for a, b in sorted(r):
        if out and a <= out[-1][1] + 1:
            out[-1] = (out[-1][0], max(out[-1][1], b))
        else:
            out.append((a, b))
    return out


def width_tables():
    zero, wide = merged(read_t("combining.t")), merged(read_t("wide.t"))
    return zero, wide


def width_of(cp, zero, wide):
    """picolibc's wcwidth in its non-CJK locale (the console's rules, section 9)"""
    if cp == 0:
        return 0
    if 0x20 <= cp < 0x7F:
        return 1
    if cp < 0xA0 or 0xD800 <= cp <= 0xDFFF:
        return -1
    for a, b in zero:
        if a <= cp <= b:
            return 0
    for a, b in wide:
        if a <= cp <= b:
            return 2
    return 1


def write_widths(out):
    """the kernel's table (kernel/cwidth.h's arrays): the BMP's ranges as 16-bit pairs, the ranges above it as 16-bit
    pairs too, grouped by plane with a byte index of each plane's first range (no range crosses a plane)"""
    zero, wide = width_tables()
    lines = ["/* Generated by tools/mkconfont.py widths (DESIGN D28 section 9): the console's widths, from the C library's",
             " * own tables (picolibc's combining.t and wide.t), merged. Do not edit. Above U+FFFF the ranges are the low 16",
             " * bits, grouped by plane: cw_NAMEp[p] is the first range of plane p (p 1-16; [17] the end). */"]
    total = 0
    for name, r in (("zero", zero), ("wide", wide)):
        lo = [x for x in r if x[1] <= 0xFFFF]
        hi = [x for x in r if x[1] > 0xFFFF]
        if any(a <= 0xFFFF < b for a, b in r) or any(a >> 16 != b >> 16 for a, b in hi):
            raise Bad("a range crosses U+FFFF or a plane")
        if len(hi) > 255:
            raise Bad("more than 255 ranges above U+FFFF")
        lines.append(f"static const unsigned short cw_{name}16[{len(lo)}][2] = {{")
        lines += ["    " + ", ".join(f"{{ 0x{a:04X}, 0x{b:04X} }}" for a, b in lo[i:i + 6]) + "," for i in range(0, len(lo), 6)]
        lines.append("};")
        lines.append(f"static const unsigned short cw_{name}A[{len(hi)}][2] = {{")
        lines += ["    " + ", ".join(f"{{ 0x{a & 0xFFFF:04X}, 0x{b & 0xFFFF:04X} }}" for a, b in hi[i:i + 6]) + ","
                  for i in range(0, len(hi), 6)]
        lines.append("};")
        first = [next((i for i, (a, _) in enumerate(hi) if a >> 16 >= p), len(hi)) for p in range(18)]
        lines.append(f"static const unsigned char cw_{name}p[18] = {{ " + ", ".join(str(x) for x in first) + " };")
        total += 4 * len(lo) + 4 * len(hi) + 18
    lines.append(f"#define CW_BYTES {total}")
    data = "\n".join(lines) + "\n"
    if not os.path.exists(out) or open(out).read() != data:
        with open(out, "w") as f:
            f.write(data)
    return total


# ---- the font ----
def build_font():
    """-> (glyphs list, {code point: entry}, {code point: source})"""
    f8 = read_font_8x8()
    cp437 = {CP437[i]: f8[i] for i in range(256)}
    sym = read_symbols()
    glyphs = [sym["box"], sym["wide-left"], sym["wide-right"]]
    gl = {}                                     # code point -> glyph bytes
    src = {}

    def add(cp, g, where):
        if cp <= 0xFF:
            raise Bad(f"U+{cp:04X} ({where}): Latin-1 is Topaz's")
        if cp in gl:
            raise Bad(f"U+{cp:04X} ({where}): has a glyph already ({src[cp]})")
        gl[cp] = g
        src[cp] = where

    for cp in range(0x2500, 0x2580):
        add(cp, box_glyph(cp, cp437), "box drawing")
    for cp in range(0x2580, 0x25A0):
        add(cp, block_glyph(cp), "blocks")
    for cp in range(0x2800, 0x2900):
        add(cp, braille_glyph(cp), "braille")
    for i, ch in enumerate(CP437):
        cp = ord(ch)
        if cp <= 0xFF or 0x2500 <= cp < 0x25A0:
            continue
        add(cp, f8[i], "CP437")
    for k, g in sym.items():
        if isinstance(k, int):
            add(k, g, "hand-drawn")
    order = sorted(gl)
    entry = {}
    for cp in order:
        entry[cp] = 256 + len(glyphs)
        glyphs.append(gl[cp])
    for cp, rest in read_cplist("standins"):
        m = re.match(r"U\+([0-9A-F]{4,6})", rest)
        if not m:
            raise Bad(f"standins: U+{cp:04X}: no target")
        t = int(m.group(1), 16)
        if cp in entry:
            raise Bad(f"standins: U+{cp:04X} has a glyph of its own")
        if 0x20 <= t <= 0x7E or 0xA0 <= t <= 0xFF:
            entry[cp] = t
        elif t in entry and entry[t] >= 256:
            entry[cp] = entry[t]
        else:
            raise Bad(f"standins: U+{cp:04X}'s target U+{t:04X} is neither Latin-1 nor a glyph of the font")
        src[cp] = "stand-in"
    for cp in entry:
        if cp > 0xFFFD or cp <= 0xFF:
            raise Bad(f"U+{cp:04X}: outside U+0100-FFFD")
    if len(glyphs) > MAXG:
        raise Bad(f"{len(glyphs)} glyphs, more than {MAXG}")
    return glyphs, entry, src


def encode(glyphs, entry):
    hps = sorted({cp >> 7 for cp in entry})
    if len(hps) > MAXP:
        raise Bad(f"{len(hps)} half-pages, more than {MAXP}")
    index = bytearray(INDEX)
    pages = bytearray(256 * len(hps))
    for n, hp in enumerate(hps):
        index[hp] = n + 1
    for cp, e in entry.items():
        n = index[cp >> 7] - 1
        struct.pack_into(">H", pages, 256 * n + 2 * (cp & 127), e)
    total = HDR + INDEX + len(pages) + 8 * len(glyphs)
    hdr = b"ZCF1" + struct.pack(">HHBBHHHI", HDR, 0, 8, 8, len(glyphs), len(hps), 0, total) + bytes(12)
    data = hdr + bytes(index) + bytes(pages) + b"".join(glyphs)
    assert len(data) == total
    return data


def check(data):
    """the kernel's checks of CON_UFONT (DESIGN D28 section 7) -> None, or Bad with the reason"""
    n = len(data)
    if n > MAXSIZE:
        raise Bad(f"{n} bytes, more than {MAXSIZE}")
    if n < HDR + INDEX:
        raise Bad("shorter than the header and index")
    magic = data[0:4]
    hsz, flags, w, h, g, p, res, total = struct.unpack(">HHBBHHHI", data[4:20])
    if magic != b"ZCF1":
        raise Bad("not ZCF1")
    if hsz != HDR or flags != 0 or w != 8 or h != 8 or res != 0 or any(data[20:32]):
        raise Bad("header: size, flags, width, height or reserved")
    if not 3 <= g <= MAXG or p > MAXP:
        raise Bad("glyph or half-page count")
    if total != n or total != HDR + INDEX + 256 * p + 8 * g:
        raise Bad("total size")
    index = data[HDR:HDR + INDEX]
    if index[0] or index[1]:
        raise Bad("index bytes 0-1 (Latin-1 is Topaz's)")
    if any(b > p for b in index):
        raise Bad("an index byte above the half-page count")
    used = [b for b in index if b]
    if len(used) != len(set(used)):
        raise Bad("two index bytes name one half-page")
    for i in range(128 * p):
        e = struct.unpack_from(">H", data, HDR + INDEX + 2 * i)[0]
        if not (e == 0 or 32 <= e <= 126 or 160 <= e <= 255 or 256 <= e < 256 + g):
            raise Bad(f"half-page entry {i}: {e}")


# ---- tests ----
def picture(g):
    return "\n".join("".join("#" if b >> (7 - x) & 1 else "." for x in range(8)) for b in g)


def selftest():
    bad = 0

    def ok(cond, msg):
        nonlocal bad
        print(("ok:   " if cond else "FAIL: ") + msg)
        if not cond:
            bad += 1

    f8 = read_font_8x8()
    cp437 = {CP437[i]: f8[i] for i in range(256)}
    # the generator against CP437's own glyphs: light lines (doubles are CP437's), blocks, shades
    light = "─│┌┐└┘├┤┬┴┼"
    for ch in light:
        g = box_glyph(ord(ch), {})
        ok(g == cp437[ch], f"generated {ch} U+{ord(ch):04X} is font_8x8.c's" + ("" if g == cp437[ch] else
           "\n" + picture(g) + "\n  font_8x8.c:\n" + picture(cp437[ch])))
    doubles = [c for c in CP437 if 0x2550 <= ord(c) <= 0x256C]
    ok(len(doubles) == 29, "all 29 double box characters are in CP437 (taken from there)")
    for ch in "░▒▓█▄▀▌▐":
        ok(block_glyph(ord(ch)) == cp437[ch], f"generated {ch} U+{ord(ch):04X} is font_8x8.c's")
    # box arms meet at the cell edges: a line of ─ and a column of │, heavy too
    for h, v in (("─", "│"), ("━", "┃")):
        gh, gv = box_glyph(ord(h), {}), box_glyph(ord(v), {})
        ok(all((b & 0x80) and (b & 1) for b in gh if b), f"{h} reaches both side edges")
        ok(gv[0] and gv[7] and gv[0] == gv[7], f"{v} reaches the top and bottom edges")
    for ch, edge in (("┌", "rd"), ("┐", "ld"), ("└", "ru"), ("┘", "lu"), ("╭", "rd"), ("╮", "ld"), ("╯", "lu"),
                     ("╰", "ru"), ("┼", "lrud"), ("┏", "rd"), ("╋", "lrud")):
        g = box_glyph(ord(ch), cp437)
        hrow = g[4]                             # row 4: on both the light and the heavy line
        good = (("r" not in edge) or hrow & 1) and (("l" not in edge) or hrow & 0x80) and \
               (("u" not in edge) or g[0] == box_glyph(0x2502 if g[0] == 0x18 else 0x2503, {})[0]) and \
               (("d" not in edge) or g[7] == box_glyph(0x2502 if g[7] == 0x18 else 0x2503, {})[7])
        ok(bool(good), f"{ch}'s arms meet the lines at the cell edges")
    # the font: built, checked, every wanted character covered, glyph widths, distinct spinner glyphs
    glyphs, entry, src = build_font()
    data = encode(glyphs, entry)
    try:
        check(data)
        ok(True, f"the font passes the kernel's checks ({len(glyphs)} glyphs, {data[13]} half-pages, {len(data)} bytes)")
    except Bad as e:
        ok(False, f"the font passes the kernel's checks: {e}")
    missing = [cp for cp, _ in read_cplist("wanted") if cp not in entry and not (0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF)]
    ok(not missing, "every wanted character has a glyph or a stand-in" +
       ("" if not missing else ": missing " + " ".join(f"U+{c:04X}" for c in missing)))
    spin = [entry[ord(c)] if ord(c) > 0xFF else ord(c) for c in "·✢✳✶✻✽"]
    ok(len(set(spin)) == 6, "the spinner's six characters have six distinct glyphs")
    zero, wide = width_tables()
    w0 = [cp for cp in entry if width_of(cp, zero, wide) == 0]
    ok(not w0, "no glyph for a zero-width character" + ("" if not w0 else ": " + " ".join(f"U+{c:04X}" for c in w0)))
    w2 = [cp for cp in entry if width_of(cp, zero, wide) == 2 and not 0xFF01 <= cp <= 0xFF5E]
    ok(not w2, "no glyph for a wide character but the fullwidth forms" + ("" if not w2 else ": " +
       " ".join(f"U+{c:04X}" for c in w2)))
    # the checks themselves refuse each damaged field (the kernel's mutation test is tools/confont-test.c)
    def refused(d):
        try:
            check(bytes(d))
            return False
        except Bad:
            return True
    for off, val, what in ((0, 0x58, "magic"), (5, 31, "header size"), (7, 1, "flags"), (8, 9, "width"),
                           (9, 16, "height"), (15, 1, "reserved"), (20, 1, "reserved bytes"), (HDR, 1, "index byte 0"),
                           (HDR + 1, 1, "index byte 1"), (HDR + 2, 255, "an index byte above the count"),
                           (HDR + next(i for i in range(2, 512) if not data[HDR + i]),
                            data[HDR + next(i for i in range(2, 512) if data[HDR + i])], "two index bytes alike"),
                           (19, data[19] ^ 1, "total size")):
        d = bytearray(data)
        d[off] = val
        ok(refused(d), f"check refuses a bad {what}")
    d = bytearray(data)
    struct.pack_into(">H", d, HDR + INDEX, 5)
    ok(refused(d), "check refuses an entry 1-31")
    struct.pack_into(">H", d, HDR + INDEX, 0x85)
    ok(refused(d), "check refuses an entry 0x7F-0x9F (those bytes are the box)")
    struct.pack_into(">H", d, HDR + INDEX, 256 + len(glyphs))
    ok(refused(d), "check refuses an entry past the glyphs")
    print("mkconfont test: " + ("all passed" if not bad else f"{bad} FAILED"))
    return 1 if bad else 0


def main(argv):
    if len(argv) == 3 and argv[1] == "font":
        glyphs, entry, _ = build_font()
        missing = [cp for cp, _ in read_cplist("wanted") if cp not in entry and not (0x20 <= cp <= 0x7E or 0xA0 <= cp <= 0xFF)]
        if missing:
            raise Bad("wanted but neither a glyph nor a stand-in: " + " ".join(f"U+{c:04X}" for c in missing))
        data = encode(glyphs, entry)
        check(data)
        with open(argv[2], "wb") as f:
            f.write(data)
        print(f"{argv[2]}: {len(glyphs)} glyphs, {data[13]} half-pages, {len([e for e in entry.values() if e < 256])} "
              f"Topaz stand-ins, {len(data)} bytes")
        return 0
    if len(argv) == 3 and argv[1] == "widths":
        n = write_widths(argv[2])
        print(f"{argv[2]}: the width table, {n} bytes")
        return 0
    if len(argv) == 3 and argv[1] == "check":
        check(open(argv[2], "rb").read())
        return 0
    if len(argv) == 2 and argv[1] == "test":
        return selftest()
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Bad as e:
        print(f"mkconfont: {e}", file=sys.stderr)
        sys.exit(1)
