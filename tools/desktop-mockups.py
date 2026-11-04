#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Draw the desktop looks of DESIGN D38 (docs/d38-desktop.md section 7) as real Amiga pixels.

    desktop-mockups.py [--font FILE] OUTDIR

Each look is drawn at 640x256 (PAL hires, the pixels twice as tall as wide) in at most 16 colours with an
8x8 bitmap font, the same scene every time: a terminal, a file list and a small about window, the
terminal active, the top bar, a few icons and the pointer. Two files per look:
    OUTDIR/NAME-native.png   the real 640x256 picture (what the Amiga shows, pixel for pixel)
    OUTDIR/NAME.png          the same for viewing: every line doubled (the Amiga's tall pixels), then 2x, nearest
--font FILE: a TrueSchool raw Amiga font (4,096 bytes: 256 glyphs x 16 rows, every row doubled; Topaz_a1200 from
github.com/rewtnull/amigafonts, GPL with the font exception), read, never copied into the repository. Without it
the PC font in tools/font_8x8.c is used (its notice: tools/font_8x8.COPYING). Never the Kickstart ROM's Topaz.
Only Pillow is needed. The pictures are proposals to choose from; nothing here is built into Zenix.
"""
import os
import sys

from PIL import Image

W, H = 640, 256
HERE = os.path.dirname(os.path.abspath(__file__))


# ---- the font ----
def load_font(path):
    if path:
        d = open(path, "rb").read()
        if len(d) != 4096:
            sys.exit(f"{path}: {len(d)} bytes, a TrueSchool raw font has 4096")
        return [bytes(d[c * 16:c * 16 + 16:2]) for c in range(256)]   # every second row: the 8x8 original
    sys.path.insert(0, HERE)
    import mkconfont
    return mkconfont.read_font_8x8()


# ---- the canvas: one byte a pixel, the palette index ----
class Canvas:
    def __init__(self, font, pal):
        self.px = bytearray(W * H)
        self.font = font
        self.pal = pal

    def put(self, x, y, c):
        if 0 <= x < W and 0 <= y < H:
            self.px[y * W + x] = c

    def fill(self, x, y, w, h, c):
        for yy in range(max(0, y), min(H, y + h)):
            a, b = max(0, x), min(W, x + w)
            if a < b:
                self.px[yy * W + a:yy * W + b] = bytes([c]) * (b - a)

    def hline(self, x, y, w, c):
        self.fill(x, y, w, 1, c)

    def vline(self, x, y, h, c):
        self.fill(x, y, 1, h, c)

    def box(self, x, y, w, h, c):
        self.hline(x, y, w, c); self.hline(x, y + h - 1, w, c)
        self.vline(x, y, h, c); self.vline(x + w - 1, y, h, c)

    def pattern(self, x, y, w, h, rows, cmap):
        """Fill with a repeating pattern: rows of characters, each a key of cmap (None: leave)."""
        for yy in range(y, y + h):
            r = rows[yy % len(rows)]
            for xx in range(x, x + w):
                c = cmap.get(r[xx % len(r)])
                if c is not None:
                    self.put(xx, yy, c)

    def text(self, x, y, s, fg, bg=None, bold=False):
        for ch in s:
            o = ord(ch)
            g = self.font[o if o < 256 else ord("?")]
            for r in range(8):
                bits = g[r]
                if bold:
                    bits |= bits >> 1          # the console's bold: the glyph smeared one pixel right
                for i in range(8):
                    if bits & (0x80 >> i):
                        self.put(x + i, y + r, fg)
                    elif bg is not None:
                        self.put(x + i, y + r, bg)
            x += 8
        return x

    def sprite(self, x, y, rows, cmap):
        for j, r in enumerate(rows):
            for i, ch in enumerate(r):
                c = cmap.get(ch)
                if c is not None:
                    self.put(x + i, y + j, c)

    def save(self, outdir, name):
        img = Image.frombytes("P", (W, H), bytes(self.px))
        flat = []
        for rgb in self.pal:
            flat += [(rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255]
        img.putpalette(flat + [0] * (768 - len(flat)))
        used = sorted(set(self.px))
        if len(self.pal) > 16 or max(used) >= len(self.pal):
            sys.exit(f"{name}: {len(self.pal)} palette entries, used {used}: more than 16 colours")
        img.save(os.path.join(outdir, name + "-native.png"), optimize=True)
        view = img.resize((W * 2, H * 4), Image.NEAREST)     # lines doubled (tall pixels), then 2x for viewing
        view.save(os.path.join(outdir, name + ".png"), optimize=True)
        return len(used)


# ---- the scene, the same in every look ----
# The windows: content rectangles in text cells (x in 8-pixel columns, so text stays byte-aligned: section 4.3).
FILES = dict(title="Files: /home/amiga", col=34, y=26, cols=36, rows=11)
ABOUT = dict(title="About Zenix", col=58, y=132, cols=20, rows=7)
TERM = dict(title="Shell 1: sh", col=10, y=114, cols=50, rows=14)

# terminal lines: spans of (text, role, bold)
P = [("zenix", "tgreen", True), (":", "tfg", False), ("~", "tblue", True), ("$ ", "tfg", False)]
TERM_LINES = [
    P + [("uname -a", "tfg", False)],
    [("Zenix amiga 0.1b1 m68k 68EC020 AGA", "tfg", False)],
    P + [("ls", "tfg", False)],
    [("Documents", "tblue", True), ("  ", "tfg", False), ("Music", "tblue", True), ("  ", "tfg", False),
     ("demos", "tblue", True), ("  bbs.txt  notes.md  ", "tfg", False), ("go.sh", "tgreen", True)],
    P + [("free", "tfg", False)],
    [("        total     used     free", "tfg", True)],
    [("chip    2048K     412K    1636K", "tfg", False)],
    [("fast    4096K    1180K    2916K", "tfg", False)],
    P + [("grep -c ", "tfg", False), ("ERROR", "tred", True), (" /var/log/boot", "tfg", False)],
    [("0", "tfg", False)],
    P + [("telnet bbs.example.net", "tfg", False)],
    [("Trying 192.0.2.10... ", "tfg", False), ("connected", "tyellow", True)],
    [("Escape: Ctrl-]", "tcyan", False)],
    P + [("", "tfg", False)],
]

FILES_ROWS = [
    ("..", "", "", False),
    ("Documents/", "-", "11-03", True),
    ("Music/", "-", "11-02", True),
    ("demos/", "-", "10-31", True),
    ("bbs.txt", "4.1K", "11-04", False),
    ("go.sh", "312", "10-30", False),
    ("notes.md", "1.2K", "11-04", False),
    ("zenix.ilbm", "38K", "10-29", False),
]
SELECTED = 3    # demos/

ABOUT_LINES = ["Zenix 0.1b1", "68EC020 14 MHz, AGA", "Chip 1636K free", "Fast 2916K free", "Up 0:42"]

ICONS = [("Shell", "term"), ("Files", "folder"), ("Prefs", "gear"), ("zenix", "disk")]

# 24 x 12 icons (the pixels are tall: 24 wide looks square). k outline, f face, a accent, w light, d dark
ICON_ART = {
    "term": [
        "kkkkkkkkkkkkkkkkkkkkkkkk",
        "kddddddddddddddddddddddk",
        "kdadddddddddddddddddddk.",
        "kddaddddddddddddddddddk.",
        "kdadddwwwwddddddddddddk.",
        "kddddddddddddddddddddddk",
        "kddddddddddddddddddddddk",
        "kkkkkkkkkkkkkkkkkkkkkkkk",
        ".........kkkkkk.........",
        "......kkkffffffkkk......",
        ".....kffffffffffffk.....",
        ".....kkkkkkkkkkkkkk.....",
    ],
    "folder": [
        "..kkkkkkkk..............",
        ".kaaaaaaaak.............",
        "kkkkkkkkkkkkkkkkkkkkkkk.",
        "kwwwwwwwwwwwwwwwwwwwwwak",
        "kwaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kaaaaaaaaaaaaaaaaaaaaaak",
        "kkkkkkkkkkkkkkkkkkkkkkkk",
    ],
    "gear": [
        "..........kkkk..........",
        "....kkk..kffffk..kkk....",
        "...kfffkkffffffkkfffk...",
        "....kffffffffffffffk....",
        "..kkfffffkkkkkkfffffkk..",
        "kkffffffk......kffffffkk",
        "kkffffffk......kffffffkk",
        "..kkfffffkkkkkkfffffkk..",
        "....kffffffffffffffk....",
        "...kfffkkffffffkkfffk...",
        "....kkk..kffffk..kkk....",
        "..........kkkk..........",
    ],
    "disk": [
        "kkkkkkkkkkkkkkkkkkkkkkk.",
        "kffffkwwwwwwwwwwwwkffffk",
        "kffffkwwwwwwwwwwwwkffffk",
        "kffffkwwwwwwwwwwwwkffffk",
        "kffffkkkkkkkkkkkkkkffffk",
        "kffffffffffffffffffffffk",
        "kfffkkkkkkkkkkkkkkkkfffk",
        "kfffkddddddaaddddddkfffk",
        "kfffkddddddaaddddddkfffk",
        "kfffkddddddddddddddkfffk",
        "kfffkddddddddddddddkfffk",
        "kkkkkkkkkkkkkkkkkkkkkkkk",
    ],
}

# the pointer (a hires sprite on AGA; tall pixels, so it is drawn narrow): o outline, i inside
POINTER = [
    "o.........",
    "oo........",
    "oio.......",
    "oiio......",
    "oiiio.....",
    "oiiiio....",
    "oiiiiio...",
    "oiiiiiio..",
    "oiiiiiiio.",
    "oiiiiiooo.",
    "oiioiio...",
    "oio.oiio..",
    "oo..oiio..",
    "o....oo...",
]


def win_box(w):
    """Outer rectangle of a window: content (cells) plus a 3-pixel margin, a 1-pixel border, the title bar."""
    cx, cy = w["col"] * 8, w["y"]
    cw, ch = w["cols"] * 8, w["rows"] * 8
    x0, x1 = cx - 4, cx + cw + 4            # border columns at x0 and x1-1
    y0 = cy - 4 - 10                        # the 10-line title bar above the content's margin
    y1 = cy + ch + 4
    return x0, y0, x1 - x0, y1 - y0, cx, cy, cw, ch


# ---- the looks ----
class Look:
    name = ""
    pal = []
    roles = {}
    mono = False                       # True: colours shown as bold only (the 2-colour look)
    bar_h = 11

    def c(self, role):
        return self.roles[role]

    # pieces each look may change
    def desktop(self, cv):
        cv.fill(0, 0, W, H, self.c("desk"))

    def bar(self, cv):
        cv.fill(0, 0, W, self.bar_h - 1, self.c("bar"))
        cv.hline(0, self.bar_h - 1, W, self.c("barline"))
        fg, y = self.c("barfg"), 2
        x = cv.text(8, y, self.logo, self.c("logo"), bold=True)
        for m in ("Window", "Tools"):
            x = cv.text(x + 16, y, m, fg)
        x += 24
        for t, cur in (("Shell", True), ("Files", False), ("About", False), ("sumi", None)):
            col = self.c("barcur") if cur else (self.c("bardim") if cur is None else fg)
            if cur:
                cv.fill(x - 4, 1, len(t) * 8 + 8, self.bar_h - 3, self.c("barcurbg"))
            x = cv.text(x, y, t, col, bold=bool(cur)) + 16
        cv.text(W - 8 * 28, y, "chip 1.6M  fast 2.9M", fg)
        cv.text(W - 8 * 6, y, "14:32", self.c("clock"), bold=True)

    def icons(self, cv):
        y = 28
        for label, art in ICONS:
            cx = 40
            cv.sprite(cx - 12, y, ICON_ART[art], self.icon_map())
            lx = cx - len(label) * 4
            if self.label_bg is not None:
                cv.fill(lx - 2, y + 15, len(label) * 8 + 4, 10, self.c(self.label_bg))
            cv.text(lx, y + 16, label, self.c("iconfg"))
            y += 44

    label_bg = None

    def icon_map(self):
        return {"k": self.c("ik"), "f": self.c("if"), "a": self.c("ia"), "w": self.c("iw"), "d": self.c("id")}

    def frame(self, cv, w, active):
        x, y, ww, hh, cx, cy, cw, ch = win_box(w)
        self.shadow(cv, x, y, ww, hh, active)
        cv.fill(x, y, ww, hh, self.c("winbg" if w is not TERM else "tbg"))
        cv.box(x, y, ww, hh, self.c("border_a" if active else "border"))
        tb = self.c("title_a" if active else "title")
        cv.fill(x + 1, y + 1, ww - 2, 9, tb)
        cv.hline(x + 1, y + 10, ww - 2, self.c("border_a" if active else "border"))
        cv.text(x + 6, y + 2, w["title"], self.c("titlefg_a" if active else "titlefg"), bold=active)
        self.gadgets(cv, x, y, ww, active)
        self.grip(cv, x + ww - 1, y + hh - 1, active)

    def shadow(self, cv, x, y, w, h, active):
        cv.hline(x + 1, y + h, w, self.c("shadow"))
        cv.vline(x + w, y + 1, h, self.c("shadow"))

    def gadgets(self, cv, x, y, w, active):
        fg = self.c("titlefg_a" if active else "titlefg")
        gx = x + w - 10
        # close: an x, 5 wide 4 tall (tall pixels), then maximise: a box, then minimise: a bar
        for i in range(5):
            cv.put(gx + i, y + 3 + i * 4 // 5, fg); cv.put(gx + 4 - i, y + 3 + i * 4 // 5, fg)
        cv.box(gx - 12, y + 3, 7, 5, fg)
        cv.hline(gx - 24, y + 7, 7, fg)

    def grip(self, cv, x, y, active):
        col = self.c("border_a" if active else "border")
        for d in (3, 6):
            for i in range(d + 1):
                cv.put(x - 2 - i, y - 1 - (d - i) // 2, col)

    def selection(self):
        return self.c("selbg"), self.c("selfg")

    def draw(self, font):
        cv = Canvas(font, self.pal)
        self.desktop(cv)
        self.icons(cv)
        for w, active in ((FILES, False), (ABOUT, False), (TERM, True)):
            self.frame(cv, w, active)
            getattr(self, "content_" + {id(FILES): "files", id(ABOUT): "about", id(TERM): "term"}[id(w)])(cv, w)
        self.bar(cv)
        self.pointer(cv)
        return cv

    def content_term(self, cv, w):
        x0, y = w["col"] * 8, w["y"]
        for line in TERM_LINES:
            x = x0
            for t, role, bold in line:
                if self.mono and role not in ("tfg",):
                    bold = True
                x = cv.text(x, y, t, self.c(role), bold=bold)
            y += 8
        cv.fill(x, y - 8, 8, 8, self.c("cursor"))      # the cursor after the last prompt

    def content_files(self, cv, w):
        x0, y = w["col"] * 8, w["y"]
        cv.text(x0, y, " Name               Size  Date", self.c("dim"))
        cv.hline(x0, y + 9, w["cols"] * 8, self.c("rule"))
        y += 11
        for i, (n, s, d, isdir) in enumerate(FILES_ROWS):
            line = " %-18s %5s  %s" % (n, s, d)
            line = line.ljust(w["cols"])
            if i == SELECTED:
                bg, fg = self.selection()
                cv.fill(x0, y - 1, w["cols"] * 8, 9, bg)
                cv.text(x0, y, line, fg, bold=True)
            else:
                cv.text(x0, y, line, self.c("dir" if isdir else "winfg"), bold=isdir)
            y += 8
        cv.hline(x0, y + 1, w["cols"] * 8, self.c("rule"))
        cv.text(x0, y + 3, " 8 items  1.2 MB free on /home", self.c("dim"))

    def content_about(self, cv, w):
        x0, y = w["col"] * 8, w["y"]
        for i, t in enumerate(ABOUT_LINES):
            cv.text(x0 + 8, y, t, self.c("winfg"), bold=(i == 0))
            y += 8
        self.button(cv, x0 + w["cols"] * 8 - 7 * 8, y + 2, " OK ")

    def button(self, cv, x, y, label):
        cv.fill(x - 4, y - 2, len(label) * 8 + 8, 12, self.c("btn"))
        cv.box(x - 4, y - 2, len(label) * 8 + 8, 12, self.c("border"))
        cv.text(x, y, label, self.c("btnfg"))

    def pointer(self, cv):
        cv.sprite(452, 15, POINTER, {"o": self.c("ptr_o"), "i": self.c("ptr_i")})


class Flat(Look):
    """(a) flat with a hint of depth: plain colours, a one-pixel shadow, a coloured title on the active window."""
    name = "a-flat"
    logo = "Zenix"
    pal = [0x343c48,   # 0 desktop slate
           0x15171b,   # 1 ink: borders, text on light, terminal background
           0xe8e6e1,   # 2 window body
           0x22272f,   # 3 shadow
           0x3f73bd,   # 4 accent: the active title, the selection
           0xb5b9bf,   # 5 inactive title, rules
           0x4b5059,   # 6 inactive title text, dim text
           0xffffff,   # 7 white
           0xc9cdd2,   # 8 terminal text
           0xd8574d,   # 9 red
           0x72c25e,   # 10 green
           0xe3bb4c,   # 11 yellow
           0x6c9be0,   # 12 terminal blue (lighter, for the dark terminal)
           0xb874cc,   # 13 magenta
           0x4fbcc4,   # 14 cyan
           0x5d6672]   # 15 mid grey: desktop icons' face
    roles = dict(desk=0, bar=1, barline=1, barfg=8, logo=7, barcur=7, barcurbg=4, bardim=6, clock=7,
                 iconfg=7, ik=1, if_=15, ia=4, iw=7, id=1,
                 winbg=2, winfg=1, border=1, border_a=1, title=5, titlefg=6, title_a=4, titlefg_a=7,
                 shadow=3, tbg=1, tfg=8, tred=9, tgreen=10, tyellow=11, tblue=12, tmagenta=13, tcyan=14,
                 cursor=8, dim=6, rule=5, dir=4, selbg=4, selfg=7, btn=5, btnfg=1, ptr_o=1, ptr_i=7)
    roles["if"] = 15


class Workbench(Look):
    """(b) Workbench 3.x, cleaner: the grey and blue, raised edges, close and depth gadgets, no stripes."""
    name = "b-workbench"
    logo = "Zenix"
    pal = [0x959595,   # 0 desktop grey (Workbench's)
           0x000000,   # 1 black
           0xffffff,   # 2 white (shine)
           0x6688bb,   # 3 Workbench blue: the active title, selection
           0xaaaaaa,   # 4 window grey
           0x555555,   # 5 dark grey (shadow)
           0xccbb99,   # 6 beige (icons)
           0x7a7a7a,   # 7 mid grey: inactive title
           0xbb3322,   # 8 red
           0x228833,   # 9 green
           0x997700,   # 10 yellow (dark, on grey)
           0x223399,   # 11 blue (dark, on grey)
           0x883388,   # 12 magenta
           0x117777,   # 13 cyan
           0xee9944,   # 14 orange accent (icons)
           0x333333]   # 15 near black text
    roles = dict(desk=0, bar=2, barline=1, barfg=1, logo=3, barcur=2, barcurbg=3, bardim=7, clock=1,
                 iconfg=1, ik=1, ia=14, iw=2, id=5,
                 winbg=4, winfg=1, border=1, border_a=1, title=7, titlefg=4, title_a=3, titlefg_a=1,
                 shadow=5, tbg=4, tfg=1, tred=8, tgreen=9, tyellow=10, tblue=11, tmagenta=12, tcyan=13,
                 cursor=3, dim=15, rule=5, dir=11, selbg=3, selfg=2, btn=4, btnfg=1, ptr_o=1, ptr_i=8)
    roles["if"] = 6
    label_bg = None

    def frame(self, cv, w, active):
        x, y, ww, hh, cx, cy, cw, ch = win_box(w)
        cv.fill(x, y, ww, hh, self.c("winbg"))
        # raised edge: white top/left, dark bottom/right; a black hairline outside (cleaner than 3.1's)
        cv.box(x - 1, y - 1, ww + 2, hh + 2, self.c("border"))
        cv.hline(x, y, ww, self.c("iw")); cv.vline(x, y, hh, self.c("iw"))
        cv.hline(x, y + hh - 1, ww, self.c("shadow")); cv.vline(x + ww - 1, y, hh, self.c("shadow"))
        tb = self.c("title_a" if active else "title")
        cv.fill(x + 1, y + 1, ww - 2, 9, tb)
        cv.hline(x + 1, y + 10, ww - 2, self.c("shadow"))
        # the close gadget on the left, as Workbench
        cv.box(x + 3, y + 2, 8, 7, self.c("border")); cv.fill(x + 6, y + 4, 2, 3, self.c("iw"))
        cv.text(x + 16, y + 2, w["title"], self.c("titlefg_a" if active else "iw"), bold=False)
        # depth gadget on the right: two overlapping boxes
        gx = x + ww - 14
        cv.box(gx, y + 2, 7, 5, self.c("border")); cv.fill(gx + 3, y + 4, 7, 5, self.c("iw" if active else "winbg"))
        cv.box(gx + 3, y + 4, 7, 5, self.c("border"))
        # minimise (iconify) next to it
        cv.box(gx - 12, y + 2, 9, 7, self.c("border")); cv.fill(gx - 10, y + 6, 3, 2, self.c("border"))
        # the content well: a recessed edge
        ex, ey = cx - 2, cy - 2
        if w is not TERM:
            cv.hline(ex, ey, cw + 4, self.c("shadow")); cv.vline(ex, ey, ch + 4, self.c("shadow"))
            cv.hline(ex, ey + ch + 3, cw + 4, self.c("iw")); cv.vline(ex + cw + 3, ey, ch + 4, self.c("iw"))
        # sizing gadget bottom right
        sx, sy = x + ww - 11, y + hh - 7
        cv.box(sx, sy, 9, 5, self.c("shadow"))
        cv.hline(sx + 2, sy + 2, 5, self.c("iw"))

    def content_files(self, cv, w):
        # Workbench lists sit on white
        cv.fill(w["col"] * 8 - 1, w["y"] - 1, w["cols"] * 8 + 2, w["rows"] * 8 + 2, self.c("iw"))
        Look.content_files(self, cv, w)

    def button(self, cv, x, y, label):
        bw = len(label) * 8 + 8
        cv.fill(x - 4, y - 2, bw, 12, self.c("btn"))
        cv.hline(x - 4, y - 2, bw, self.c("iw")); cv.vline(x - 4, y - 2, 12, self.c("iw"))
        cv.hline(x - 4, y + 9, bw, self.c("shadow")); cv.vline(x - 5 + bw, y - 2, 12, self.c("shadow"))
        cv.text(x, y, label, self.c("btnfg"))

    def bar(self, cv):
        Look.bar(self, cv)
        cv.hline(0, self.bar_h - 1, W, self.c("border"))


class Mono(Look):
    """(c) two colours, classic Mac and Atari ST: a dithered desktop, a striped active title, a drop shadow."""
    name = "c-mono"
    logo = "Zenix"
    mono = True
    pal = [0xffffff, 0x000000]
    roles = dict(desk=0, bar=0, barline=1, barfg=1, logo=1, barcur=0, barcurbg=1, bardim=1, clock=1,
                 iconfg=1, ik=1, ia=0, iw=0, id=0,
                 winbg=0, winfg=1, border=1, border_a=1, title=0, titlefg=1, title_a=0, titlefg_a=1,
                 shadow=1, tbg=0, tfg=1, tred=1, tgreen=1, tyellow=1, tblue=1, tmagenta=1, tcyan=1,
                 cursor=1, dim=1, rule=1, dir=1, selbg=1, selfg=0, btn=0, btnfg=1, ptr_o=0, ptr_i=1)
    roles["if"] = 0
    label_bg = "winbg"

    def desktop(self, cv):
        cv.pattern(0, 0, W, H, ["10", "01"], {"1": 1, "0": 0})   # the 50% grey of a one-plane screen

    def shadow(self, cv, x, y, w, h, active):
        cv.fill(x + 2, y + h, w, 1, 1)
        cv.fill(x + w, y + 1, 2, h, 1)

    def frame(self, cv, w, active):
        Look.frame(self, cv, w, active)
        x, y, ww, hh, cx, cy, cw, ch = win_box(w)
        cv.fill(x + 1, y + 1, ww - 2, 9, 0)
        if active:
            for yy in range(y + 2, y + 10, 2):           # pinstripes, with the title on a white patch
                cv.hline(x + 16, yy, ww - 32, 1)
            tw = len(w["title"]) * 8 + 12
            tx = x + (ww - tw) // 2
            cv.fill(tx, y + 1, tw, 9, 0)
            cv.text(tx + 6, y + 2, w["title"], 1, bold=True)
            cv.fill(x + 4, y + 2, 8, 7, 0); cv.box(x + 4, y + 2, 8, 7, 1)    # close box, left
        else:
            tw = len(w["title"]) * 8
            cv.text(x + (ww - tw) // 2, y + 2, w["title"], 1)

    def gadgets(self, cv, x, y, w, active):
        if active:
            cv.fill(x + w - 13, y + 2, 9, 7, 0); cv.box(x + w - 13, y + 2, 9, 7, 1)   # zoom box, right
            cv.box(x + w - 13, y + 2, 5, 4, 1)



class Cyber(Look):
    """(d) dark cyberpunk: near black, a faint grid, magenta and cyan neon, still plain readable text."""
    name = "d-cyber"
    logo = "ZENIX"
    pal = [0x07070d,   # 0 desktop black
           0x12121f,   # 1 grid lines
           0x0e0e18,   # 2 window body
           0xff2a6d,   # 3 neon magenta: the active title
           0x05d9e8,   # 4 neon cyan: the active border, selection
           0x2b2b45,   # 5 inactive border
           0x1a1a2c,   # 6 inactive title
           0x6e6e94,   # 7 dim text
           0xc4cadb,   # 8 text
           0x5a1030,   # 9 magenta glow (dim)
           0x39ff88,   # 10 acid green
           0xffd23f,   # 11 yellow
           0x4d7cff,   # 12 blue
           0xff3b3b,   # 13 red
           0x0a4a55,   # 14 cyan glow (dim)
           0xffffff]   # 15 white
    roles = dict(desk=0, bar=0, barline=3, barfg=7, logo=3, barcur=0, barcurbg=4, bardim=5, clock=4,
                 iconfg=8, ik=4, ia=3, iw=8, id=2,
                 winbg=2, winfg=8, border=5, border_a=4, title=6, titlefg=7, title_a=3, titlefg_a=0,
                 shadow=14, tbg=2, tfg=8, tred=13, tgreen=10, tyellow=11, tblue=12, tmagenta=3, tcyan=4,
                 cursor=3, dim=7, rule=5, dir=4, selbg=4, selfg=0, btn=6, btnfg=4, ptr_o=0, ptr_i=4)
    roles["if"] = 2

    def desktop(self, cv):
        cv.fill(0, 0, W, H, 0)
        for y in range(15, H, 16):
            cv.hline(0, y, W, 1)
        for x in range(0, W, 32):
            cv.vline(x, 0, H, 1)

    def shadow(self, cv, x, y, w, h, active):
        if active:     # a glow, one pixel around the active window
            cv.box(x - 1, y - 1, w + 2, h + 2, 14)
            cv.box(x - 2, y - 2, w + 4, h + 4, 9)

    def bar(self, cv):
        Look.bar(self, cv)
        cv.hline(0, self.bar_h - 1, W, 3)


class Platinum(Look):
    """(e) Mac OS 8-9's Platinum, Amiga-pixelled: light grey bevelled windows, a pinstriped active title with the
    close box on the left and the zoom and collapse boxes on the right, the title centred, a blue desktop."""
    name = "e-platinum"
    logo = "Zenix"
    pal = [0x6676a8,   # 0 desktop blue (OS 8's)
           0x000000,   # 1 black
           0xffffff,   # 2 white (bevel shine, lists)
           0xdddddd,   # 3 platinum: window body, menu bar
           0xa8a8a8,   # 4 stripes, inactive edges
           0x777777,   # 5 bevel shadow, dim text
           0x3355aa,   # 6 selection blue (OS 9 highlight)
           0x8a99c6,   # 7 desktop pattern's light dots
           0xbb2211,   # 8 red
           0x117722,   # 9 green
           0x886600,   # 10 dark yellow
           0x223399,   # 11 dark blue
           0x882288,   # 12 magenta
           0x116677,   # 13 cyan
           0xeec06a,   # 14 folder yellow
           0xbbbbbb]   # 15 inactive title bar
    roles = dict(desk=0, bar=3, barline=1, barfg=1, logo=1, barcur=2, barcurbg=6, bardim=5, clock=1,
                 iconfg=2, ik=1, ia=14, iw=2, id=5,
                 winbg=3, winfg=1, border=1, border_a=1, title=15, titlefg=5, title_a=3, titlefg_a=1,
                 shadow=5, tbg=2, tfg=1, tred=8, tgreen=9, tyellow=10, tblue=11, tmagenta=12, tcyan=13,
                 cursor=1, dim=5, rule=4, dir=11, selbg=6, selfg=2, btn=3, btnfg=1, ptr_o=2, ptr_i=1)
    roles["if"] = 14
    label_bg = "desk"

    def desktop(self, cv):
        cv.fill(0, 0, W, H, self.c("desk"))
        for y in range(0, H, 4):            # a fine dotted pattern, as OS 8's default desktop
            for x in range((y // 4) % 2 * 4, W, 8):
                cv.put(x, y, 7)

    def frame(self, cv, w, active):
        x, y, ww, hh, cx, cy, cw, ch = win_box(w)
        # a black outline, a one-pixel drop shadow, a bevelled platinum body
        cv.hline(x + 1, y + hh, ww, self.c("shadow")); cv.vline(x + ww, y + 1, hh, self.c("shadow"))
        cv.fill(x, y, ww, hh, self.c("winbg"))
        cv.box(x, y, ww, hh, self.c("border") if active else self.c("shadow"))
        if active:
            cv.hline(x + 1, y + 1, ww - 2, self.c("iw")); cv.vline(x + 1, y + 1, hh - 2, self.c("iw"))
            cv.hline(x + 1, y + hh - 2, ww - 2, self.c("shadow")); cv.vline(x + ww - 2, y + 1, hh - 2, self.c("shadow"))
            for ry in range(y + 2, y + 10, 2):   # the pinstripes (every other line: Amiga pixels are tall)
                cv.hline(x + 4, ry, ww - 8, 4)
            # the close box on the left; the zoom and collapse boxes on the right: bevelled squares
            for bx in (x + 6, x + ww - 26, x + ww - 15):
                cv.fill(bx - 1, y + 2, 11, 7, self.c("winbg"))
                cv.box(bx, y + 2, 9, 7, self.c("border"))
                cv.hline(bx + 1, y + 3, 7, self.c("iw")); cv.vline(bx + 1, y + 3, 5, self.c("iw"))
            cv.box(x + ww - 24, y + 4, 4, 3, self.c("border"))              # zoom: a small box inside
            cv.hline(x + ww - 13, y + 5, 5, self.c("border"))               # collapse: a bar
            cv.hline(x + ww - 13, y + 6, 5, self.c("border"))
        else:
            cv.fill(x + 1, y + 1, ww - 2, 9, self.c("title"))
        cv.hline(x + 1, y + 10, ww - 2, self.c("shadow"))
        t = w["title"]
        tx = x + (ww - len(t) * 8) // 2
        cv.fill(tx - 6, y + 1, len(t) * 8 + 12, 9, self.c("winbg") if active else self.c("title"))
        cv.text(tx, y + 2, t, self.c("titlefg_a" if active else "titlefg"), bold=active)
        # the content: white with a recessed edge
        cv.fill(cx - 1, cy - 1, cw + 2, ch + 2, self.c("iw"))
        cv.hline(cx - 2, cy - 2, cw + 4, self.c("shadow")); cv.vline(cx - 2, cy - 2, ch + 4, self.c("shadow"))
        # the grow box bottom right
        sx, sy = x + ww - 11, y + hh - 7
        cv.box(sx, sy, 9, 5, self.c("shadow")); cv.box(sx + 2, sy + 1, 5, 3, self.c("shadow"))

    def button(self, cv, x, y, label):
        bw = len(label) * 8 + 8
        cv.box(x - 6, y - 3, bw + 4, 14, self.c("border"))          # the default button's heavy ring
        cv.fill(x - 4, y - 2, bw, 12, self.c("btn"))
        cv.box(x - 4, y - 2, bw, 12, self.c("border"))
        cv.hline(x - 3, y - 1, bw - 2, self.c("iw")); cv.vline(x - 3, y - 1, 10, self.c("iw"))
        cv.text(x, y, label, self.c("btnfg"), bold=True)

    def bar(self, cv):
        Look.bar(self, cv)
        cv.hline(0, self.bar_h - 1, W, self.c("border"))


LOOKS = [Flat, Workbench, Mono, Cyber, Platinum]


def main(argv):
    font_path = None
    if len(argv) >= 2 and argv[0] == "--font":
        font_path, argv = argv[1], argv[2:]
    if len(argv) != 1:
        sys.exit(__doc__)
    out = argv[0]
    os.makedirs(out, exist_ok=True)
    font = load_font(font_path)
    for L in LOOKS:
        look = L()
        n = look.draw(font).save(out, look.name)
        size = os.path.getsize(os.path.join(out, look.name + ".png"))
        print(f"{look.name}: {n} colours, {size} bytes")
        if size > 100 * 1024:
            sys.exit(f"{look.name}.png is over 100 KB")


if __name__ == "__main__":
    main(sys.argv[1:])
