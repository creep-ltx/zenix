# D51. TrueType fonts: drawn once a size into Amiga bitmap fonts

Roadmap block 5, "TrueType fonts (owner 2026-10-26; rendered once per size into bitmap fonts and cached, so a 68020
without an FPU pays only once; also usable by the console)", and docs/decisions.md "A place for fonts" (owner
2026-11-06: `/usr/share/fonts/truetype/` for TrueType). The direction is the partner's and fixed: a .ttf is drawn
once at a size into a bitmap font, the result is kept on the disk, and from then on the system uses it like any
Amiga font. This note is the details (delegated, as the rest of block 5). Built on branch ttf, 2026-11-13.

## 1. In short

- **A tool, not code in displayd:** `ttf2font FONT.ttf SIZE...` (display/ttf/ttf2font.c) draws the characters 32-126
  and 160-255 at SIZE lines and writes an AmigaOS bitmap font: `NAME/SIZE` and `NAME.font`, the files displayd's
  reader (display/server/amifont.c, D38 "Fonts") and AmigaOS's diskfont already read. Nothing new downstream.
- **Our own rasteriser** (display/ttf/ttf.c, 8.6 KB of 68k code), not stb_truetype (22.5 KB with the soft-float it
  needs, section 3): integer arithmetic only, no FPU and no soft float.
- **Black and white, no grey levels** (section 4), **no hinting** (the partner's brief).
- **The cache is system-wide:** `/var/cache/fonts/NAME/SIZE`, made by displayd (root) when the desktop's font is a
  .ttf and its file is missing or stale; stale is a stamp in the file naming the .ttf's size and time and the
  renderer's version (section 6).
- **Found by name:** `font NAME SIZE` in `/etc/zdesk/screen` (and a user's own, D38 "Preferences") is an Amiga font
  in `/usr/share/fonts/amiga/` first, else `/usr/share/fonts/truetype/NAME.ttf`. deskprefs lists the TrueType fonts
  in eight sizes. The console takes one as its Unicode font through `ttf2font -c` (an 8 x 8 ZCF1 file, D28 7).
- **One font shipped:** Go Regular (BSD-3, GPL-2-compatible), cut to Western European letters and a few symbols:
  33,916 bytes, `/usr/share/fonts/truetype/go.ttf`; not on the DD demo floppy (section 9).

## 2. What a size means, and the layout

SIZE is the font's lines, `tf_YSize`, as an Amiga font's size is (Helvetica 11 is 11 lines), so `font go 11` and
`font helvetica 11` make the same bar height (D38: the bar and the title bars grow with the font's lines).

The em is chosen so the characters 32-255 fit those lines: the font's own ascent and descent (`hhea`), widened to the
highest and lowest glyph of those characters (from each glyph's box in its header, no drawing), so an accent on a
capital (Å, É) is not cut. The baseline is the line whose bottom edge is at the ascent, rounded. Go Regular at 11 lines:
an em of 9.5 pixels, the baseline on line 8 (three lines below it for descenders).

**The pixels' shape.** On the desktop's screen (640 x 256 PAL on a 4:3 picture) a pixel is 1.875 times as tall as
wide. A glyph drawn with square pixels looks squeezed there (tried: the letters narrow, the stems one thin column).
So a glyph's width is drawn 1.875 times its height's scale (`TTF_XR`, 120/64): it then looks as its designer drew it,
and its vertical stems are two columns, as the Amiga's own hires fonts are. NTSC's pixels (2.4) make it a little
narrow; interlace (square pixels) twice as wide as drawn, as a font for hires looks there (section 11, a question).

## 3. The rasteriser: our own, measured against stb_truetype

stb_truetype (public domain or MIT, GPL-2-compatible; 5,321 lines) was measured first: its renderer with `-Os
-m68020-60 -msoft-float` and a wrapper calling it as ttf2font would is **16.1 KB of code, 22.5 KB linked** with
libgcc's soft-float routines (and its maths functions `sqrt`, `floor`, `ceil`, `pow`, `cos`, `acos` stubbed: more
for the real ones). Every coordinate is a `float`; on a 68020 without an FPU each operation is a libgcc call of some
hundred cycles. It also carries CFF (PostScript outline) and SDF code the desktop does not need.

Ours, `display/ttf/ttf.c` (written from the TrueType format as Apple and Microsoft publish it, no other
rasteriser's code): **8,612 bytes** at -Os (the reader and the rasteriser), `ttfont.c` (the Amiga font, contents and
ZCF1 writers) 4,098, ttf2font's own part 3,439: **ttf2font 22,756 bytes on the disk** with libz linked, as every
program. Only while it runs: 48 KB of work area, about 20 KB for the glyphs of a size (11 lines), the .ttf itself.

**How it draws.** The tables `head`, `maxp`, `hhea`, `hmtx`, `loca`, `glyf`, `cmap` (formats 4, 12, 6 and 0: Unicode,
else Macintosh Roman) and `post` (fixed pitch). A glyph's points (simple glyphs; composite ones with their offsets and
2.14 scales, nested at most 6 deep) are scaled to 1/64 pixels with 32-bit multiplies and one divide each (nothing
wider: a 68060 has no 64-bit `mulu.l`). Each quadratic curve is cut into at most 16 lines, never more than 1/16 pixel
from the curve. Each line's crossings with the rows' centres are collected (counted, then filled: no lists), sorted
per row and walked with the nonzero rule; a pixel is set when its centre is inside or on the outline. A span with no
centre in it (a stroke thinner than a pixel) sets the pixel under its middle (dropout control) unless it is narrower
than 3/8 pixel (a stroke's slanted tip, which would grow a stray dot); the columns are scanned the same way, which
also sets a centre that lies exactly on a horizontal edge (the rows' half-open rule misses it). The instructions
(hinting) are skipped. Limits: 1,024 points a simple glyph; 256 parts and 16,384 points a glyph with all its parts
(a hostile composite's parts multiply with each level: four levels of 20 parts took a minute on the PC before this
budget, the review); 2,048 lines and 4,096 crossings a glyph; 64 x 64 pixels and an advance of 255 a glyph (a wider
one is left out at that size and said: the 8 x 8 stands in; a too-wide default glyph is written empty); a size's
glyphs at most 64 KB together (amifont.c's limit: such a size is refused with that reason, from about 57 lines in
wide fonts); a .ttf of at most 4 MB. Points are clamped to 8,192 pixels once scaled, so every product stays within
32 bits.

**Checked against FreeType** (section 10): FreeType's unhinted black-and-white glyphs differ in 11% of their pixels at
8 lines, 6% at 11, 2% at 16, 1% at 40. The differences are where the two rules meet ties (a centre exactly on an
edge, which pixel a dropout sets, FreeType's "stubs").

## 4. Grey levels: no (decided here)

The desktop draws text through one mask in one pen (D38 "Fonts": `bm_mask`, chip RAM written once); its 16 pens are
the theme's, chosen as a whole, and text lands on many backgrounds (bars, active and inactive titles, menus, windows).
Grey levels would need a mixed pen per background and per level (for a pair the theme did not plan, often no pen is
near), a second drawing path reading chip RAM back, and a cache that is no longer a plain Amiga font (a two-plane
colour font, which amifont.c reads as one colour). 1-bit keeps the format AmigaOS reads too, the drawing path
unchanged, and the cost nil. What it costs: unhinted black-and-white glyphs of a font designed for grey are rougher
than a hand-made bitmap font. The partner said so of FreeType's black-and-white output in CTerm (knowledge base,
amiga/graphics-and-performance.md, 2026-10-11: "they look terrible in bitmap"). The pixel shape (section 2) and the
dropout rules help; a font made for small sizes helps more. If grey is wanted, the way is a colour font in the cache
and pens mixed per theme: a question (section 11).

## 5. ttf2font

```
ttf2font [-q] [-o DIR] [-n NAME] FONT.ttf SIZE...   DIR/NAME/SIZE and DIR/NAME.font (DIR /var/cache/fonts)
ttf2font [-q] -c FONT.ttf [OUT.zcf]                  the console's Unicode font (/usr/share/consolefonts/NAME.zcf)
```

NAME is the file's name without `.ttf` (1-25 bytes, no spaces, slashes or colons). Sizes 4-64 lines. Each file is
written beside its place and renamed over it (a cut leaves the old or the new). `NAME.font` keeps the sizes it lists
and gains this one (at most 64). It prints each size's glyphs, lines, baseline and time; `-q` only errors. The
output is a real AmigaOS font: copied to an Amiga's `FONTS:` it works there too (`-o DIR`).

**The font file** (D38's layout, the RKM's): `HUNK_HEADER`, one `HUNK_CODE` (`moveq #-1,d0; rts`, the DiskFontHeader
named `NAME.font`, the TextFont at 58 with `FPF_DISKFONT | FPF_DESIGNED`, `FPF_PROPORTIONAL` unless every advance is the
same, `tf_LoChar` 32, `tf_HiChar` 255, the strike, CharLoc, CharSpace and CharKern; a character the .ttf lacks and the
control codes 127-159 are empty slots: the 8 x 8 stands in, as for any Amiga font; the default glyph is the .ttf's
glyph 0), `HUNK_RELOC32` for the six pointers, then the stamp: `HUNK_DEBUG` with 4 longs (`ZTTF`, the .ttf's size in
bytes, its time, `TTF_STAMP_VER << 16 | SIZE`), and `HUNK_END`. AmigaOS's loader skips a debug hunk, and amifont.c
reads past one. A font with no space gets an empty one a quarter em wide. `NAME.font` keeps each size's own style and
flags; a contents file of another kind (0x0f02 with tags, 0x0f03 an outline font's) is replaced by a plain one.
Before drawing, ttf2font makes the folder and tries a file there, so a disk that cannot take the result (read-only,
full, not allowed) costs no drawing time.

**The console's font** (`-c`): the code points past U+00FF the .ttf has, among the alphabets up to U+10FF (the
combining marks left out) and U+1E00-U+2E7F (Latin additions, punctuation, symbols, arrows, box lines, blocks), at most
4,096, drawn at 8 lines with the em fitted as above, square pixels (the console's cell is 8 x 8 whatever the screen),
centred in the cell. Written as ZCF1 (D28 7, frozen) with the box and the wide box's halves as glyphs 0-2. conprefs
lists every `/usr/share/consolefonts/NAME.zcf`, so it is chosen there; Topaz keeps Latin-1 (D28 6). Made by hand, once:
the console's font is root's and rarely changed. Go at 8 lines is small (an em of 7 pixels): legible for Latin
Extended-A, poor for box lines (the shipped zenix-8x8.zcf's are drawn for the cell).

## 6. The cache

- **Where:** `/var/cache/fonts/NAME/SIZE` and `NAME.font`, system-wide (decided here). displayd is root's and the
  only reader; one copy serves every user and dlogin's greeter; a user cannot write `/var/cache`, so a per-user cache
  would need displayd to read users' folders, which it does not (D38 "Preferences": it reads one setting). The files
  are small (Go: 3.1 KB at 8 lines, 4.1 KB at 11, 6.5 KB at 16).
- **Made when:** displayd's font loader (`font_ui_load`, at its start and on `DPY_FONT_SET`) finds no Amiga font of
  the name, finds `/usr/share/fonts/truetype/NAME.ttf`, and the cache file is missing or its stamp does not match the
  .ttf's size, time and the renderer's version. It runs `/bin/ttf2font -q NAME.ttf SIZE` and waits; then reads the file
  as any Amiga font (the stamp checked again: that, not ttf2font's exit status, decides, since the size's file can
  be written when `NAME.font` was not). The screen stands still while it draws (section 8), once a size.
- **Stale:** the .ttf replaced or touched (its size or time changed), or a ttf2font of another `TTF_STAMP_VER`: the
  file is drawn again and renamed over the old one. Nothing deletes old sizes; `rm -r /var/cache/fonts` is always safe.
- **Refused:** a .ttf ttf2font cannot read is said twice, by ttf2font (its reason) and by displayd in its usual one
  line ("displayd: /usr/share/fonts/truetype/NAME.ttf: ttf2font could not draw it: the built-in 8x8 is used"), and the
  desktop runs with the font it had. A read-only root (a CD): ttf2font cannot write, said the same way.

## 7. Hooks

- **The screen file** (frozen, D38): `font NAME SIZE` unchanged in form. The search: `/usr/share/fonts/amiga/NAME.font`,
  else `/usr/share/fonts/truetype/NAME.ttf` (the Amiga font wins a shared name). No `~/.fonts/` yet: displayd reads
  the system's places only (D38 "Preferences"); a user's own fonts there would need displayd to read and ttf2font to
  write as that user.
- **deskprefs:** after the Amiga fonts, each `/usr/share/fonts/truetype/NAME.ttf` (whose name no Amiga font has) in
  the sizes 8, 9, 10, 11, 12, 13, 14 and 16; the list holds 64 entries (48 before). Test draws the size the first
  time, which takes the time of section 8.
- **The console:** section 5, `ttf2font -c`, then conprefs (D28 22) or `/etc/console`'s `font NAME`.

## 8. Speed and size (measured)

FS-UAE's stock A1200 (68EC020 14 MHz, no FPU, 4 MB fast RAM, cycle-exact), `ttf2font` on go.ttf, 191 glyphs a size;
emulator figures, not a real machine's:

| lines | time | file |
|---|---|---|
| 8 | 1.53 s | 3,100 bytes |
| 11 | 1.84 s | 4,104 bytes |
| 13 | 2.06 s | 4,952 bytes |
| 16 | 2.44 s | 6,524 bytes |
| console 8 x 8 (195 glyphs) | 2.41 s | 3,920 bytes |

About 10 ms a glyph: Go's Latin-1 glyphs are simple outlines, a font with more points takes longer in proportion. A
256-glyph font at 8-16 lines would take about 2-3.5 s. displayd's first load of go 11 (the spawn, ttf2font, the read)
took 2.61 s: the screen stands still that long, once. Then nothing more is paid: the file is read as an Amiga font's
is (the cached go 11 takes 4.1 KB on the disk and 5,887 bytes in displayd once read, its pictures, mask and table, as an Amiga font of its size would).
displayd grew by 975 bytes of code (the loader; the stamp test is inline), deskprefs by 370 bytes of code and 580 of
lists (64 entries, 48 before); the DD demo floppy has 27,648 bytes free after it (about 28 KB before, D50).

## 9. The font shipped

**Go Regular** (Bigelow & Holmes for the Go project, 2016; the Go licence, BSD-3-Clause: a notice to keep, no
renaming rule, GPL-2-compatible). `tools/mkgofont.sh` cuts it with fontTools' `pyftsubset` to Latin-1, Latin
Extended-A, dashes, quotes, the bullet, ellipsis, euro, trade mark, arrows, box lines and blocks (387 glyphs), the
hinting left out (Zenix does not use it): 148,672 -> **33,916 bytes**, committed as `display/ttf/go.ttf`, its notice
`display/ttf/gofont.LICENSE` (installed as `/usr/share/licenses/gofont`). It has no composite glyphs (the host test
makes one). Why Go: a clear humanist sans with open shapes, a permissive licence with no reserved name, and it is
already on most developers' machines. Fonts under the SIL OFL (DejaVu-style or Noto) are also fine to ship beside
GPL-2 code (aggregation; OFL forbids selling the font alone and reserves names, so a cut copy would need renaming).

On the full images and in the desktop package (with ttf2font), not on the RAM floppy. **Not on the DD demo floppy:**
it has about 28 KB free (D50), and the font (34 KB, about 25 KB packed) and ttf2font (22 KB, about 12 KB packed)
together do not fit; its desktop uses Topaz. The HD floppy could carry both; left for the main session's next floppy
build (the Makefile's image lists are its).

## 10. Tests

- **`tools/ttf-host-test.sh`** (the PC, AddressSanitizer and UndefinedBehaviorSanitizer; in `tools/check.sh`'s host
  list): every glyph 33-255 of go.ttf against FreeType's unhinted black-and-white rendering (when FreeType is
  installed) at 8-14 and 16 lines in the desktop's shape and at 16 and 40 square. **The tolerance:** at most 13% of
  FreeType's ink pixels may differ in all at 8-10 lines, 8% at 11-12, 6% at 13-16, 2% at 40, and at most 24 in one
  glyph. Why: both sample pixel centres unhinted, so they differ only at ties (a centre exactly on an edge after each
  one's rounding) and in dropout control (which pixel a too-thin stroke gets; FreeType leaves "stubs" out by the
  outline's shape, we by a width): at most a pixel per stroke edge per row, so the share falls as the size grows
  (measured: 11%, 10%, 10%, 6%, 6%, 3-4%, 2%; square 2% and 1%). A real fault is far outside it: a row off by one,
  tried, fails ten sizes; curves cut into at most 2 lines fail three. Then: every size 4-64 written and read back by
  amifont.c (each glyph's picture as drawn, the metrics, the stamp's three checks); the contents file's merge and its
  64-size limit; a composite glyph (B made of A: unscaled it is A pixel for pixel at 8-40 lines, at half scale half as
  wide); the console's ZCF1 file through the kernel's checks (`tools/mkconfont.py check`); 16 damaged fonts refused
  with their exact reasons (CFF, a collection, tables past the end, no glyf, units per em 0, a loca entry past glyf,
  contours out of order, 2,000 points, a composite naming itself or glyph 65000, four levels of four parts, a glyph cut
  short, no glyphs, no metrics, an unknown loca format) and one read safely (units per em 16: every point far
  outside); every cut of the first 4 KB and 600 after (refused before the end of the tables
  read, read after it); 4,000 files with 1-8 bytes changed at random, half with the composite in: each read or refused,
  nothing read outside.
- **`tools/ttf-test.sh`** (FS-UAE, the stock A1200 with 4 MB fast; STOCK in `tools/check.sh`: screenshots at times on
  the PC's clock): ttf2font's four sizes and the console's font with their times, the files and their stamp; a cut
  .ttf named by the screen file: ttf2font's reason, displayd's one line, the 8 x 8; `font go 11` with no cache: drawn
  once, read, the bar 3 lines taller, a window's title "Zenix" in the screenshot pixel for pixel as the host's
  renderer draws it (`ttf-host-test text`: the same files); the desktop started again (the cache used, nothing drawn),
  then the .ttf touched (drawn again).

**The review** (one independent, opus, 2026-11-13; it also swept 80 fonts at sizes 4-64 and fuzzed each table of
three fonts under the sanitizers) found, all fixed: a composite whose parts fan out at each level ran for minutes or
days and displayd would have waited for ever (the 256-part, 16,384-point budget); signed overflow in the curve cutting
on huge coordinates (points clamped); glyph offsets kept in 16 bits, so a size past 64 KB of glyphs was written wrong
without a word (now refused with the desktop's 64 KB reason); a size refused whole for one too-wide default glyph or
one advance past 255 (left out instead, as the docs said); a read-only cache costing the whole drawing at every start
(the place tried first); displayd judging by ttf2font's exit status (now by the stamp); and smaller ones (format 12
looked up by halving, a font without a space, the contents file's per-size flags, `mn_Length` clamped, deskprefs
saying when its list is full, an unread `fstat` result). Not changed: displayd still waits for ttf2font with no
deadline (the budget bounds the work: at most 191 glyphs x 16,384 points), question 3.

## 11. Limits, and questions for the partner

Limits (also in docs/limits.md): characters 32-255 on the desktop (as every Amiga font); sizes 4-64 lines; a glyph at
most 64 x 64 pixels; a .ttf at most 4 MB; TrueType outlines only (no CFF/OpenType-PostScript, no collections, no
variable-font axes: the default instance); no kerning pairs (the Amiga font format has none); no hinting; the cache
is system-wide and only root's displayd fills it; the font drawn for hires pixels.

Questions:
1. **Grey levels** (section 4): 1-bit is decided for now. If the look is not good enough, a two-plane cache and pens
   mixed per theme are the way (a second drawing path in displayd): worth it?
2. **Interlace** (section 2): the glyphs are drawn for hires pixels, so in interlace they are twice as wide as
   designed (as hires Amiga fonts are there). A second cache per pixel shape, displayd choosing by the mode (and
   reloading at a mode switch), would fix it: wanted?
3. **The screen stands still while a size is drawn** (section 8, the first time only): acceptable, or should
   displayd draw in the background and switch when done?
4. **The HD demo floppy:** carry go.ttf and ttf2font (about 37 KB packed)?
