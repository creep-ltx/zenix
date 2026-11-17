# D50: the picture viewer, `show` (design note, revision 1)

Roadmap block 5, "the file view, viewers, preferences; TrueType fonts": this note is the viewers item's first part,
pictures (text has `less`, Maki and Markdown already). Written and built 2026-11-13 on branch `view`; the details
delegated. No number is claimed: `show` is an ordinary client of the display protocol (D38 section 4) and needs no
new message, port or system call.

## 1. In plain words

- **`show PICTURE`** opens a window on Zenix Desktop with the picture in it. The file view (`files`, D48) starts it
  for a picture (Return or a double click), so pictures no longer ask "Open with:".
- **The Amiga's own format first:** IFF ILBM, as Deluxe Paint, Brilliance, Personal Paint, ArtPRO and every grabber
  wrote it: 1 to 8 planes (2 to 256 colours), Extra Half-Brite, HAM6, HAM8, 24-bit (24 planes), lores, hires,
  super-hires and interlaced, compressed (ByteRun1) or not, AGA's 8-bit palette and the old 4-bit one. **And GIF**,
  the BBS and Aminet era's other picture format (section 3).
- **In the window, in the desktop's colours.** A window on the desktop has the screen's 16 pens (8 on OCS/ECS), the
  theme's colours. A picture with others is shown in those pens, dithered with a 4 x 4 pattern (fast) so a photo or
  a HAM picture keeps its shading; `d` switches to Floyd-Steinberg (finer, slower), then to the nearest pen. A
  picture whose colours are the pens' shows exactly.
  The picture on a screen of its own, with its real palette and mode as AmigaOS viewers do, needs a new request in
  the display protocol: section 4 says what it would take; it is a question for the partner.
- **The size it has on the glass.** A lores picture is twice as wide in hires pixels, an interlaced one half as tall
  on the desktop's 256 lines, a GIF's square pixels half as tall: the window shows the picture's real shape. A picture
  bigger than the window is shrunk to fit; `z` shows it at its real size, moved with the arrows or `h j k l`.
- **Next and previous in the folder:** `n` or Space, `p` or Backspace; files that are not pictures are passed over.
  A left click is the next picture, Shift and a click the previous. Every key is configurable (`~/.config/show/keys`,
  files' format and its parser, now shared).
- **Lean:** 36 KB on the disk (section 7), nothing loaded until it runs; the decoders and the renderer 9 KB.

## 2. The program

**The name: `show`.** `view` is vi's read-only mode on every Unix (a user who types `view notes.txt` expects an
editor), so the viewer is `show`, as in "show me the picture"; a later text or sound viewer can be another word.

**The parts:**
- `display/show/pic.c` (and `pic.h`): the decoders and the renderer, with no system calls and no libz, so the host
  test builds the same file (as `display/server/amifont.c`). Everything read is checked; a damaged or hostile file
  gives a reason and allocates nothing.
- `display/show/show.c`: the window, the keys, the folder, the drawing.
- `display/lib/keys.c` (`display/keys.h`): the keys file's parser, moved out of `files` so both programs share one
  (D13's rule: no copies). `files` behaves as before.

**`files`' open rule** (D48 section 5, a row added): a file whose first bytes say IFF ILBM (`FORM` .... `ILBM`) or GIF
(`GIF87a`, `GIF89a`) is opened with `/bin/show PATH`, started bare (no terminal: `show` draws its own window), in the
folder shown; without `/bin/show` it asks "Open with:" as before. `i` still views it with `less`.

## 3. The formats

**IFF ILBM** (EA IFF 85 and the ILBM spec, both public; the knowledge base's `amiga/iff-ilbm-viewer.md` for what
real files do):
- `BMHD`, `CMAP`, `CAMG`, `BODY` read; other chunks skipped. One nested `FORM ILBM` inside a `FORM ILBM` without its
  own `BMHD` (the MagicWB-era patterns' `FORM ILBM { ANNO, FORM ILBM {...} }`). A `FORM` longer than the file: what
  is there is read.
- Planes 1-8 and 24; masking none, a mask plane (skipped), a transparent colour or lasso (drawn as the picture);
  compression none or ByteRun1. **ByteRun1 is one stream over the whole BODY**: a run may go on into the next plane's
  row (some packers do), so it is never decoded row by row.
- **The palette:** `CMAP` (at most 256); a 4-bit CMAP (every low nibble 0) scaled `v | v >> 4`, so `$F0` is white;
  no `CMAP`: grey. Colours past the CMAP are black.
- **The mode from `CAMG`,** cleaned as the old readers did: a 16-bit CAMG loses the bits that are not modes
  (`$1000 $4000 $0100 $0002 $2000`); a high word without the `$1000` bit is junk (no CAMG). HAM (`$800`) and EHB
  (`$80`) count on every native monitor; hires (`$8000`), super-hires (`$20`) and lace (`$4`) only on the default,
  NTSC, PAL, Euro36, DblNTSC and DblPAL monitors (a VGA productivity picture is 640 x 480, not super-hires). Other
  monitors (graphics cards' mode ids) and no CAMG: hires when 640 or more wide (super-hires at 1280) or narrow pixels
  in the BMHD's aspect, lace when 400 or more tall or wide pixels; and with no CAMG, 6 planes with at most 16 colours
  are HAM6 (DigiView), with 17-32 EHB.
- **EHB:** colours 32-63 are always the halves of 0-31.
- **HAM6:** each pixel's top two bits: 00 a palette colour (0-15), 01 blue, 10 red, 11 green set to the 4 data bits
  (times 17); every row starts from colour 0. **HAM8** (AGA): the same with 6 data bits and 64 base colours; a
  modify sets the gun's top 6 bits and keeps its low 2 (the AGA notes' rule; AmigaOS 3.2's picture.datatype instead
  repeats the top bits into the low 2, a difference of at most 3 a gun: knowledge base `amiga/datatypes-and-media.md`).
  The control bits are the file's top two planes (6 and 7 of HAM8, 4 and 5 of HAM6), as the ILBM readers take them
  (picture.datatype's HAM decoded the same way, knowledge base). Checked 2026-11-13 against a real 1993 AGA HAM8
  file, Aminet pix/trace/TallShip (896 x 628 HAM8, hires lace, shipped with a 256-colour GIF of the same frame):
  `pic.c`'s decode matches the GIF to within 7-8 of 255 a gun on average (the GIF's own 256-colour loss), with no
  colour fringes, so the files carry the control bits in their top two planes (the hardware's own are bitplanes
  1 and 2; the readers and writers swap them). The file is not in the repository (someone else's picture).
- **24 planes:** red, green, blue, 8 planes each, bit 0 first.
- Refused, with the reason in the window: other compressions, other plane counts, `PBM`/`ACBM`/`DEEP`, a palette-only
  file ("a palette, no picture"), a picture over 4096 x 4096 or over 4 MB decoded. A BODY cut short shows the rows
  that are whole, the rest colour 0, and the title says "cut short".

**GIF** (CompuServe's GIF89a, public; 87a too): the first image (an animation's first frame), the global or the
image's own colour table, interlaced rows, LZW with codes up to 12 bits and clear codes; extensions skipped;
transparency not shown (the colour as it is). A code past the table, a table that loops, a code size outside 2-8
are refused; data cut short shows what came.

**Why GIF and not PNG (decided here).** One more format was worth it if it is small: GIF's LZW is about 1 KB of
code and its 256-colour palette goes the ILBM path, and GIF was what Amiga BBSes and Aminet carried besides IFF. PNG
is today's common format, but needs inflate: Zenix has miniz's `tinfl` (used by `gzip`, `zup` and the demo floppy's
loader), 7.7 KB at -O2 and about 5 KB at -Os, linked statically into each program that uses it (no shared libraries
yet, D13 stage 2), plus about 2 KB for PNG's filters, its colour types and Adam7: `show` would grow by about a fifth
for one format. Later, when it is asked for, PNG and JPEG are best as **decoders loaded on demand** (a small program
per format, `/lib/show/png`, run by `show` for a file it does not read itself, writing a 24-plane ILBM to a pipe that
`show` reads as it reads any ILBM), so `show` stays its size and a format costs only those who view it.

## 4. Colours: the desktop's pens, or a screen of its own

**What the protocol allows today** (D38 section 4, `display/proto.h`): a window draws in the screen's pens, 0-15
(the theme's colours; 0-7 on OCS/ECS); the palette is the window manager's (`DPY_WM_STYLE`), read only for a client
(`DPY_PALETTE`); images are planar of depth 1-4 (`DPY_IMAGE`), at most 640 x 256 each and 64 KB of chip RAM per
client together. On a graphics card (D43) the desktop is still 16 pens, converted to the card's pixels. So in a window
a picture is shown in those pens:

- **The pens** through a table of 4,096 entries (12-bit colour to pen, weighted 3:4:2 for red, green and blue
  as the eye sees them), made once from the screen's palette (again when the screen changes, `EV_SCREEN`).
- **Ordered dithering** (the default): a 4 x 4 Bayer pattern added before the nearest pen. For a picture of colour
  numbers (every ILBM but HAM and 24-bit, every GIF) the pattern's strength is each colour's distance from its
  nearest pen (at most 32 of 255), worked out once for each colour and each of the 16 places, so a pixel is one
  table look-up and a colour that is a pen stays exactly that pen (the test's pictures are made so); HAM and 24-bit
  pictures get the pattern at 30 for every pixel. A regular texture rather than a grain.
- **Floyd-Steinberg** (`d`, or `-f`): each pixel's error spread right and below: finer shading, about three times
  the time of the pattern for HAM and 24-bit and more for a palette picture (DESIGN D50 "As built"). Pens that are
  the picture's own colours give no error, so such a picture shows exactly here too.
- **The nearest pen** (`d` again, or `-n`): no pattern.
- The flat theme's 16 pens hold a red, green, yellow, blue, magenta and cyan besides its greys, so a photo is
  recognisable but grainy; the workbench theme (greys and blues) less so. A picture's own palette cannot be set.

**A screen of its own** (AmigaOS's way: a picture viewer opens a screen with the picture's mode and palette, and
Amiga+M or Left Amiga+N flips back to Workbench): **not possible in protocol version 1.** What it would take (a
design of its own, proposed here, not built):
- one request, `DPY_SCREEN` (the next free number, claimed then): a client asks for a screen of its own with a mode
  (lores, hires, super-hires; lace; 1-8 planes; HAM6, HAM8, EHB: BPLCON0's bits) and a palette of up to 256 colours
  of 24 bits (AGA's), and gives its planes as the picture (chip RAM, the client's: no copy);
- displayd builds a second Copper list for it (as it builds the lace fields' lists) and shows it while it is in
  front; a key (Amiga+M, zdesk's grab) or the client flips back to the desktop, whose screen and windows are kept as
  they are; keys and the mouse go to the screen's client while it is shown; displayd draws nothing on the desktop
  meanwhile and catches up when it is shown again;
- OCS/ECS: up to 6 planes, HAM6 and EHB, 12-bit colours; AGA all; a graphics card (D43): the picture in 24 bits
  through the card's own format, if displayd learns a second surface there, else refused;
- the cost: displayd about 1.5-2.5 KB of code (an estimate), the picture's chip RAM (a 320 x 256 HAM8 picture 80 KB,
  640 x 512 x 8 320 KB), a protocol growth (frozen-interfaces.md), two reviews (it changes what the screen shows and
  where keys go).
Then `show` would offer both: the window by default (the desktop stays in view), `Return` or a menu item for the
picture's own screen with its real colours. **Question for the partner** (section 9, 1). Until then: the window.
**Built since** (DESIGN D55, docs/d55-screens.md, branch intro 2026-11-14): `DPY_SCREEN`, and in `show` as an option
(`-s`, the key `s`, the menu's Own screen), the window still the default.

## 5. Keys and the mouse

Configurable in `~/.config/show/keys` (files' format, D48 section 4; written with the built-in keys when missing; a
line not understood is said in the title at start and the others still work):

| Keys (built in) | Action |
|---|---|
| `n` `<Space>` | the next picture in the folder (by name without case, as `files` sorts; hidden names and files that are not pictures passed over) |
| `p` `<BS>` | the previous picture |
| `gg`, `G` | the first, the last |
| `z` | zoom: shrunk to fit the window, or the real size |
| `d` | the pattern, Floyd-Steinberg, the nearest pen, and round again |
| `h` `<Left>`, `l` `<Right>`, `k` `<Up>`, `j` `<Down>` | move a picture bigger than the window by a quarter of it; Left and Right are the previous and next picture when the picture's width fits the window |
| `?` `<Help>` | the keys, in the window (any key or a click goes back) |
| `q` `<Esc>` | quit (the close gadget too) |

**The mouse:** a left click is the next picture, Shift and a click the previous. **The bar** has a menu "Show" (Next,
Previous, Fit or real size, Dither, Keys, Quit). The window can be resized and zoomed; a fitted picture is drawn again
for the new size.

## 6. How it draws

- **The window** is asked at the picture's size on the glass (at most the screen less 24 pixels across and 48 down;
  at least 160 x 40); zdesk places it. Next and previous keep the window's size.
- **The canvas:** the picture is decoded once into memory (an ILBM as its planes, a GIF a byte a pixel), then
  rendered into a canvas in the screen's pens, planar, in fast RAM: scaled to the size shown (nearest pixel), the
  colours to pens. The canvas is kept, so an expose or a move inside a real-size picture draws from it without
  rendering again; it is rendered again on zoom, dither, a resize (fitted), a screen change.
- **On the screen:** the visible part of the canvas goes to the window as planar images (`DPY_IMAGE`) in strips of
  at most 16 KB, each made, drawn (`O_IMAGE`) and freed at once, so the program holds no chip RAM while it shows; the
  border around a smaller picture is filled in the darkest pen, each pixel once (D38's bar: nothing drawn twice, no
  clearing first).
- **Speed:** the inner loops are built -O2 (the rest -Os) and tuned for the 68020: planes to pixels through a table
  (two long ORs a plane for 8 pixels: the knowledge base's measured trick), HAM in registers, the gun held to 0-255
  by a table, the pen from the 4,096-entry table, eight pens shifted into one long that holds the four planes'
  bytes (two instructions a pixel, four byte writes for eight); Floyd-Steinberg's errors kept in registers, the
  shares made of shifts and adds. Tuning matters here: built for the 68060's timings gcc turns `7 * e` and the
  shifts back into `muls.l` (2 cycles on a 060, 28 or more on a 68020), and the first build took 6.2 s in FS-UAE's
  stock A1200 for a 616 x 123 window; `-mtune=68020` keeps the shifts (and needs pic.c's constant divisions out of
  line: tuned for the 68020 gcc makes them 64-bit multiplies, which a 68060 lacks and `check-68020-60.sh` refuses).
  Rendering is still the part one waits for on a 14 MHz 68020; its time is printed by `show -v` and by the test
  (emulator figures, DESIGN D50 "As built"); the real A1200's are for its first run.

## 7. Memory and sizes

- **Disk:** `show` 36,444 bytes (its own code: `pic.c` 9.0 KB, the decoders and the renderer; `show.c` 8.6 KB with its
  texts and built-in keys; the keys' parser 3.0 KB, shared with `files`; libz and libdisplay linked statically as
  every client). `files` 41,736 -> 42,608 bytes (+872: the parser as a module, the open rule).
- **Memory while showing:** the program (about 54 KB with its data, the tables of pens and the pattern 8 KB), the file while it is
  decoded (then freed), the decoded picture (a 320 x 256 x 5 ILBM 50 KB, 640 x 512 x 8 320 KB, a 640 x 480 GIF
  300 KB; at most 4 MB), the canvas (the window's size in pens: 616 x 208 x 4 planes 64 KB; at its real size at most
  1 MB, a bigger picture's real size shrunk to that), one strip of chip RAM (16 KB) for a moment while drawing.
- **The demo floppies (D25):** see "As built" in DESIGN D50 (what `make demo` said).

## 8. Tests

- **`tools/show-host-test.sh`** (the host, AddressSanitizer and UBSan): `tools/mkpics.py cases` makes 102 pictures
  from numbers (1-8 planes at widths 1, 7, 16 and 37, compressed and plain; EHB and HAM6 from CAMG and guessed; HAM6
  hires lace; HAM8, also packed as one stream across rows; 24 planes; hires, lace, super-hires, a VGA monitor's CAMG,
  a junk CAMG, an old 16-bit CAMG, an RTG mode id, an aspect-only hires; a mask plane packed and plain; a nested FORM;
  a 4-bit CMAP, a short CMAP, none; a FORM longer than the file; 640 x 400 guessed hires lace; GIFs of 2, 4, 16 and
  256 colours, interlaced, with a local table, and one past 4,096 codes) with the RGB the generator worked out itself
  (its own HAM and LZW, never the decoder's): each decoded and compared pixel by pixel, its mode in words too; 26
  damaged files each refused with its reason or shown cut; the renderer (the pens' own colours give their pens with
  the nearest pen, the pattern and Floyd-Steinberg, whole, halved and 37 across with the row's padding 0; a grey in
  one pen nearest and in more with either dither); 20,000 mutations, each decoded and drawn (shrunk and enlarged, the
  three ways in turn) or refused, nothing read outside, every allocation given back.
- **`tools/show-test.sh`** (FS-UAE, the stock A1200 with 4 MB of fast RAM through the cage rig, in check.sh's STOCK:
  screenshots on the PC's clock): `tools/mkpics.py pics` on the test images (`/tests/pics`): bars.iff's blocks in the
  flat theme's own pens shrunk to fit, pixel for pixel where the scaling puts them; next through a damaged file
  (refused in the window), a GIF (half as tall, its pixels checked), HAM6, a text file passed over, a hires lace
  picture, the end; the real size and a move; the nearest pen; the first again; a click; the keys file written, a
  changed one (a bad line said, a new key working); then `files` opening bars.iff: `/bin/show` started without a
  terminal and its window showing the blocks.

## 9. Questions for the partner (each with the default built)

1. **A screen of its own for pictures** (section 4: a protocol request, about 2 KB in displayd, two reviews): the
   real palette, HAM and the picture's mode, the way AmigaOS viewers show them, while the window stays the quick
   look. Default: the window only, in the desktop's pens, dithered.
2. **One more format later:** PNG (today's screenshots and the web), JPEG (photos), both, or neither? Default:
   ILBM and GIF; PNG and JPEG would be decoders loaded on demand (section 3).
3. **The name `show`** (not `view`, which is vi's read-only mode on Unix). Default: `show`.

## 10. Not in this step

A screen of its own (section 4), PNG and JPEG (section 3), animations (ANIM, an animated GIF's frames), colour
cycling (CRNG/CCRT ranges), saving or converting, a slide show, thumbnails in `files`, the text viewer (`less`,
Maki and Markdown are there), sound (`play`, `modplay`).

---
Revision 1, 2026-11-13: written and built on branch view; one independent review, its findings fixed (DESIGN D50
"As built").
