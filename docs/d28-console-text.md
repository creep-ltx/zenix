# D28: UTF-8 in the console, and the console's fonts (design note, revision 2)

Status: **revision 2 after two independent reviews (section 18); steps 1-6 built on branch utf8, 2026-10-29 (section
19, "As built"); steps 7-9 not.** Roadmap block 3 ("A terminal
you can live in"): "UTF-8 in the console; PC (CP437) and other bitmap fonts for the console (the PC set comes earlier,
with telnet)". The PC set came with telnet (D20 step 1, `CON_FONT`). This note decides the rest:
- how the console decodes UTF-8 and what a cell holds;
- which glyphs exist and where they come from, and how a font with more than 256 glyphs is loaded;
- how D20's 8-bit mode and telnet's three modes live beside UTF-8;
- the widths, the locale (with D27), the drawing path's speed and the memory on the stock A1200.

The short version will be DESIGN.md D28.

**How the facts were established:**
- Facts about today's code are *read* from branch d28 at bf93534 (main at 722d6f6 plus D27's note), 2026-10-28.
- Figures marked *measured* were taken on the PC on 2026-10-28:
  - the kernel's `term.c` and `gfx.c` built with the project's cross compiler (gcc 16.1, the kernel's flags), before
    and after a throw-away prototype of sections 4-6 (in a scratch folder, not in the repository);
  - picolibc 1.8.10's width tables compiled on the PC and compared with glibc 2.44 for every code point;
  - a prototype of the font tool (section 6) run over the repository's `tools/font_8x8.c`;
  - the characters in Claude Code 2.1.296's Linux program file, counted.
- **Nothing was timed.** Speed is judged from the generated code (instruction counts) until the stock A1200 times it
  (CLAUDE.md: no speed claims from an emulator).

## 1. What it is for, in plain words

- **Today the console shows Latin-1:** one byte, one character, Topaz's glyphs from the Kickstart ROM.
  - UTF-8 text shows as junk: `â”€` for every line of a box.
  - It arrives from many places: a Linux machine, the web, `ssh`, Claude Code over `cu` or the network, and the
    block-3 programs (Sumi, Maki, Markdown, Ahiru: the owner's tui.library family already writes UTF-8).
- **After D28 the console reads UTF-8 and the keyboard types it.** The screen has the characters terminal programs draw
  with:
  - box lines: straight, heavy, double, rounded corners;
  - blocks and shades, braille dots (spinners and graphs), arrows, bullets, check marks;
  - the quotes and dashes of ordinary text, and the symbols Claude Code's screen uses.
- **A character the font lacks** shows as a small box, or as a look-alike (`ł` as `l`). Either way it takes the right
  number of columns, so the screen never tears.
- **Nothing changes for what works today:**
  - Topaz stays the console's look for every Latin-1 letter.
  - An old Latin-1 file still reads right: `cat` of an AmigaOS text shows its å ä ö.
  - telnet's PC and Amiga board modes look and run exactly as now, once telnet and the kernel are updated together
    (section 15).

## 2. Today (read)

- **The grid** (`term.c`):
  - Per console, `ch`, `at` and `co` are each 80 x 32 bytes: 7,680 bytes in all, and the console's struct is about
    7.8 KB, allocated when the console is first used.
  - The scrollback is 200 lines x 80 x 2 (characters and one colour byte, inverse folded in) = 32,000 bytes, allocated
    when the console first scrolls.
  - `put1` takes one byte: first the escape parser (ECMA-48, D14), then the controls, then the store and `draw` ->
    `gfx_cell(row, col, u8 ch, attr, colour)`.
  - Inside an escape sequence a byte of 0x80 or more behaves in two ways:
    - in `E_STR` (OSC, DCS and the other control strings: window titles) it is swallowed and the string goes on
      (`term.c` 509-515);
    - in `E_ESC`, `E_CSI` and `E_ESCI` it ends the sequence, and the sequence is swallowed.
  - `redraw_all` and `show_back` skip a cell whose byte is a space, whose `at` is 0 and whose colour is the default
    (`term.c` 119, 154, 161).
  - `reset_term` (`ESC c`) fills `ch` with spaces and `at` with 0.
  - `write_` calls `put1` once per byte. `tty_write` hands it pieces of at most 256 bytes and may stop early (D20 6),
    so a multi-byte character may be split between two calls.
- **The glyphs** (`gfx.c`):
  - Topaz 8 from the ROM, bytes 32-255 (`glyphs[224][8]`, indexed `ch - 32`; a byte below 32 shows `?`). It is read at
    boot through GfxBase and never shipped (it is the ROM's).
  - Above one plane there is a second copy with each line in all four bytes of a long (`rglyph`, 7 KB), so a glyph line
    is one read.
  - A console may have a loaded 8-bit font (D20, `CON_FONT`: PSF1, 256 glyphs). Its frozen meaning is "8-bit mode:
    each byte one glyph of this font, no UTF-8 while loaded". It has its own four-bytes table (8 KB) above one plane.
  - Only the shown console is drawn; `sfont` says whose glyphs.
- **The keyboard** (`kbd.c`): the keymaps give Latin-1 bytes (å = 0xE5 on the Swedish map), and `key_down` hands each
  byte to `tty_input` for the shown console (`con_cur`).
- **The tty** (`tty.c`): in cooked mode Backspace erases one byte and echoes `\b \b`.
- **Programs:**
  - `sh`'s line editor, `edit`, `less` and `ls` count one byte as one column.
  - **A bug:** `sh` (`readline`, `sh.c` ~421) uses the byte 0x80 as its own code for the Del key, **and acts on a raw
    0x80 byte from the input as Del** (*read*). With Latin-1 keys this is harmless. But a UTF-8 continuation byte 0x80
    (in `─` E2 94 80, `À` C3 80, `Ā` C4 80), typed or pasted, would delete a character.
  - **Taken by the main session on main, 2026-10-28**, as a "fix when found" item independent of D28 (section 15).
- **telnet** (D20, `net/telnet/`, `user/telnet.c`):
  - The PC mode loads `cp437-8x8.psf` with `CON_FONT`.
  - The Amiga mode writes the board's Latin-1 bytes for Topaz.
  - The Unix mode decodes the host's UTF-8 to Latin-1 (`?` for the rest), and its keys turn Latin-1 into UTF-8
    (`tk_keys`, `net/telnet/keys.c` 10-16).
  - The PC mode's fallback without the font maps Latin-1 keys to CP437 byte by byte (`tn_latin1_cp437`).
  - D20 4.2 left the Amiga mode's choice open: convert to UTF-8, or use an 8-bit mode. It also sketched block-3 Unicode
    fonts as "PSF2 with a Unicode table, told apart by their magic". D28 replaces that sketch (section 7).
- **The locale:** none; libz has no wide characters. D27 brings picolibc with `mb-capable` (UTF-8 only), `mbrtowc` and
  `wcwidth`, and the locales `C` and `C.UTF-8`.

## 3. The decisions in short

| Question | Decision (section) |
|---|---|
| Where UTF-8 is decoded | In the kernel's terminal, in `put1`: a pending character is one more parser state (`E_UTF`); ASCII pays one test of the byte it already holds (4, 8) |
| Bytes inside an escape sequence | Not decoded: they go through the parser exactly as today (4) |
| Malformed input | Each byte that is not part of a valid character is one cell: its Latin-1 glyph (0xA0-0xFF), else the replacement box; never a control (4) |
| C1 controls written as UTF-8 (U+0080-009F) | Ignored |
| The cell | A 16-bit value. Today's byte stays; an `A_HI` bit in `at` marks a cell whose high byte is in a plane of its own, allocated the first time the console stores a character above U+00FF (2,560 bytes). The scrollback keeps high rows only for the lines that need them (5) |
| Characters above U+FFFF | Their width kept; drawn as the replacement (emoji: a two-cell box) |
| Wide characters | Two cells (the character and a tail), with xterm's rules for wrapping and for overwriting half of one (5) |
| Zero-width characters | Not stored (combining marks dropped; composing é from e + U+0301 is an optional later step) |
| The font format | Zenix's own `ZCF1`: a fixed header, a half-page index, pages of glyph numbers and 8 x 8 glyphs, all big-endian; the kernel uses its copy of the file as its table (6) |
| The default font | A supplement to the ROM's Topaz: box drawing, blocks and braille generated by the tool, CP437's symbols from `font_8x8.c`, a hand-drawn set for the rest, stand-ins by mapping; 527 glyphs, 11,160 bytes (6) |
| Loading the font | One font for every console, loaded by init at boot from `/etc/console`'s `font` line; root only; new `SYS_console` op 4 `CON_UFONT` (7) |
| Widths | **The kernel's own**, a 2.5 KB table generated at build time from the same picolibc tables the C library's `wcwidth` is compiled from; the font has none (9) |
| 8-bit mode | One mode, two ways in: a `CON_FONT` (as frozen), or the new rule `CONR_8BIT` 128 (Topaz, no decoding). Keys are Latin-1 while the shown console is in it (7, 10) |
| Keyboard | UTF-8 (å = C3 A5), except in 8-bit mode (10) |
| tty | Cooked Backspace erases one character as the console decoded it, and the columns it took (10) |
| Locale | `LANG=C.UTF-8` set where `TERM` is set (the shell); D27's port teaches `setlocale(LC_ALL, "")` to read it (11) |
| telnet | PC mode as today; Amiga mode with `CONR_8BIT` (today's bytes and speed); Unix mode passes UTF-8 through; the keys follow (12) |
| Speed | ASCII: about 5 more instructions a character and one read (8) |
| Done separately, now | The per-character 32-bit multiplies and `sh`'s 0x80 bug: taken by the main session on main, 2026-10-28 (15) |

## 4. Decoding UTF-8

**Where.** In the kernel's terminal, because the console is where every program's bytes meet. D20 4.1 chose the same
place (choice A) for the same reason: one parse on the kernel's fast path, and nothing drawn twice. A user-space
filter cannot draw, and every program writing to a console would need it.

**How, in `put1`.** A pending character is a parser state, `E_UTF`, beside `E_ESC`, `E_CSI`, `E_STR` and `E_ESCI`.
- `put1` already tests `t->esc` first. With no state, a byte of 0x80 or more reaching the printable path (one test of
  the byte already in a register) enters the decoder: either it starts `E_UTF`, or it is a fallback cell at once.
- While `E_UTF` is set, each byte goes to the decoder. A finished character goes to `putcp(t, vis, cp)` (section 5).
- The decoder's state lives in `struct term` (8 bytes: the bytes still wanted, the bytes held, the allowed range of the
  next byte, and the code point so far). So a character split by `tty_write`'s pieces, by SLIP's frames or by two
  `write` calls is finished by the next write.
- `ESC c`, `term_panic`, loading or dropping a `CON_FONT`, setting or clearing `CONR_8BIT`, and `CON_UFONT` all clear
  it.

**Inside an escape sequence nothing is decoded.** While `t->esc` holds an escape state, every byte goes through the
parser exactly as today:
- a window title's UTF-8 (`OSC 0;✳ Claude Code BEL`, which Claude Code sends) is swallowed with its string, as now;
- a byte of 0x80 or more still ends a CSI and is swallowed.

So no escape sequence can be built or hidden by UTF-8 forms, and the parser keeps its behaviour byte for byte.

**The rules** (Unicode's well-formed byte sequences, chapter 3, Table 3-7):
- A lead C2-DF takes one continuation byte, E0-EF two, F0-F4 three. Continuations are 80-BF, except the byte right
  after these leads:
  - after E0: A0-BF (no overlong forms);
  - after ED: 80-9F (no surrogates);
  - after F0: 90-BF;
  - after F4: 80-8F (nothing above U+10FFFF).
- **A byte that cannot be part of a character is shown as one cell:** its Latin-1 glyph for 0xA0-0xFF, the replacement
  box for 0x80-0x9F. Such bytes are:
  - a lone continuation byte;
  - C0, C1 and F5-FF;
  - a lead whose sequence is broken by a byte out of range: another lead, ASCII, a control or ESC.
- A broken sequence shows its lead byte that way, then each byte held after it (one cell each). Then the breaking byte
  is taken afresh: it may start a new character, or be ASCII or a control that acts. A control or ESC that breaks a
  sequence acts after the fallback cells, exactly as without the sequence.
- **Why Latin-1, and not one U+FFFD per broken sequence** (Unicode's recommended practice, "maximal subparts"):
  - **The Amiga's text is Latin-1**, and a byte that is not UTF-8 is almost always a Latin-1 letter. `cat` of an
    AmigaOS file, a file name made before D28, or a Latin-1 board in a pipe shows å ä ö instead of boxes. The owner's
    utf8.library has the same rule ("a byte that is not UTF-8 is its Latin-1 letter", knowledge base
    `amiga/utf8-library.md`), so the family of programs and the console agree.
  - **One byte, one column** is what programs do when they meet bad UTF-8: `less` shows `<E5>` per byte, editors show
    one `?` per byte. One U+FFFD per maximal subpart would put the two bytes `E2 82` in one column while the program
    counted two, and the screen would tear.
- **Never a control.** The fallback gives a glyph or the box, never a byte below 0xA0. A decoded U+0080-009F (C1
  controls written as UTF-8, `C2 9B` included) is ignored: not acted on and not stored. `C0 9B` is two cells: `À` and
  the box.
- **Not decoded in 8-bit mode** (a `CON_FONT`, or `CONR_8BIT`: section 7). There bytes go to the store as today.
- **The kernel log** (`term_log`, kprintf's copy on the shown console) puts a pending character aside and back. A log
  line written in the middle of a program's character therefore does not break it.
- **A write's pieces end on a character boundary.** `tty_write` runs `kwork` between pieces, and its cooked echo goes
  to the same console. So `term_write_some`'s early stop, and the 256-byte cut, wait until no character is pending
  (at most 3 more bytes). Otherwise type-ahead echoed during `cat` would break a character in two.
- **The box for a bad byte 0x80-0x9F** is stored as the byte itself. After D28 no text is stored as 0x80-0x9F (C1 is
  ignored), so outside 8-bit mode such a cell draws the box. Stray bytes (a CP1252 file's `“”` = 93 94, `cat` of a
  binary) therefore never allocate the high planes.
- The serial mirror (`tty_mirror`) copies the bytes as written, unchanged.

**The known collision** (knowledge base `amiga/cterm-audit-2026-10-10.md`): a Latin-1 letter C2-DF followed by a byte
80-BF is a valid two-byte character (`Ä` + `°` is U+0130). Amiga *art* is full of such pairs (`Ü¯` is U+072F). So text
that really is Latin-1 must be marked, not guessed: telnet's Amiga mode uses 8-bit mode (section 12). The fallback is
for stray Latin-1 text, where such pairs are rare.

## 5. The cells

**The value.** A cell holds a 16-bit value:
- the code point, for U+0000-U+FFFD;
- `0xFFFE`, for a character above U+FFFF. Its width is kept: a wide one, as nearly every emoji is, takes two cells and
  shows the wide replacement;
- `0xFFFF`, for the right half (the tail) of a wide character.

U+FFFE and U+FFFF never arrive as text: they are noncharacters, so when decoded they are drawn as the box and stored as
U+FFFD. Values 0x00-0xFF mean what the byte means today: Topaz's glyph, or the `CON_FONT`'s glyph in 8-bit mode.

**Where the high byte lives.**
- Today's `ch` byte stays the low byte.
- **Bit `A_HI` (8) of `at`** says "this cell's high byte is in `hi`". `at` uses three of its bits today (`A_INV`,
  `A_UND`, `A_BOLD`).
- `hi` is a plane of its own, **allocated the first time the console stores a value above 0xFF** (2,560 bytes).
- Every ASCII or Latin-1 store already writes `at = t->attr`, which clears the mark, so it never touches `hi`.
- Every clear (`reset_term`, `blank_rows`, `clear_cells`, the new cells of `CSI @`/`P`) already writes `at` = 0, so a
  stale `hi` byte is never read.
- Moves of rows and cells (`move_rows`, `CSI @`/`P`) move `hi` with `ch` when the plane exists.
- `redraw_all`'s "skip blank cells" test already looks at `at`, so U+2520 `┠` (low byte 0x20) is never taken for a
  blank.
- The allocation never panics (unlike `get()`'s first allocation), and never happens while a panic prints.
- **If the plane cannot be had** (no memory), the character is stored as `?` (a wide one as `?` and a space), and the
  log says so once. A wrong glyph is never shown.
- A console that never shows a character above U+00FF pays nothing. So the RAM floppy, the BBS modes and plain shells
  cost what they cost today.

**The scrollback** keeps its two bytes a cell (character, colour) and, for each of its 200 lines, a slot byte:
- 0: no high row;
- else: the number of an 80-byte high row in a small pool. A pushed line with any `A_HI` cell takes a row, in which
  cells without the mark get 0.
- The pool grows in chunks of 16 rows (1,280 bytes) up to 200 rows. A row is given back when its line leaves the
  scrollback.
- `show_back` treats a cell as not blank when its line has a row and the row's byte is not 0. This mends revision 1's
  blank test, which would have hidden `┠`.
- If no row can be had, the line's high cells are kept as `?` (said once on the log).
- So a Claude Code session, whose scrollback is mostly ASCII text between boxes, pays a fraction of the 16,000 bytes a
  whole high plane would cost. The worst case is the same 16,000 plus the 200-byte slot index.

**Why not the alternatives** (memory per console that shows Unicode, on top of today's 7.8 KB + 32 KB):

| Choice | Grid | Scrollback | Notes |
|---|---|---|---|
| **`A_HI` mark, high plane and a scrollback row pool, on first use (chosen)** | +2,560 | +200, plus 80 a line that has such characters (worst 16,200) | nothing for consoles without Unicode; ASCII stores touch nothing new |
| A whole high plane for the scrollback (revision 1) | +2,560 | +16,000 at once | takes a Unicode console's scrollback to the 48 KB that D14 refused on a 2 MB machine |
| 16-bit `ch` always | +2,560 | +16,000 | every console pays, used or not |
| Glyph numbers instead of code points | +2,560 | as above | a font change could not redraw (the code point is lost: Linux keeps a second, code-point screen for that reason) |
| 32-bit code points | +7,680 | +48,000 | exact for all of Unicode, but the console cannot draw beyond the BMP anyway |

**Wide characters** (width 2, section 9: CJK, fullwidth forms, emoji) take two cells: the value and a tail, both with
the character's attributes and colours. The cursor moves two columns.
- **Wrapping:**
  - A wide character at column 78 (the 79th) fills 78 and 79 and leaves the cursor at 79 with the wrap pending, as an
    ordinary character in the last column does.
  - At column 79 a wide character wraps first: cell 79 is left as it is and the character goes to the next line, as in
    xterm.
  - With `CONR_WRAPNOW` the same.
- **Writing over half of a wide character blanks the other half**, as xterm and VTE do. The owner's tui.library shows
  `?` in the left half instead; it needs no change, because both keep the columns.
- **Orphaned halves:** erasing (`K`, `X`, `J`) and inserting or deleting characters (`@`, `P`) blank the orphaned half
  at their edges. Region scrolls and line moves keep a row's cells together, so they make no orphans.
- **Detecting a wide cell needs one read:** the cell being overwritten has `A_HI` (its old `at` is read), and only then
  is its value looked at.
- **Drawing is decided from the stored cells, not from today's width**, so a font change cannot orphan a half:
  - a lead whose next cell is a tail draws the font's glyph if it has one (the left half: the font has 8-pixel glyphs
    only); if it has none (entry 0, or the value 0xFFFE) it draws the wide box's left half (glyph 1);
  - its tail then draws the box's right half (glyph 2), and any other tail is blank;
  - the cursor on a tail is drawn there.

**Zero-width characters** (combining marks, ZWJ, variation selectors, U+200B) are not stored, and the cursor does not
move. The program, using the same widths, counted them as zero too.
- So a base letter plus a combining accent (decomposed text, NFD: macOS file names, some web text) shows without its
  accent.
- **Composing** them (e + U+0301 to é when the pair has a Latin-1 form: about 60 pairs, 300 bytes) is optional, step 8,
  if such text is seen.

**`putcp`**, the path of a decoded character:
1. U+0080-009F: ignored.
2. The width (section 9). Width 0: dropped.
3. Width 1 or 2: the deferred wrap; the overwritten-half rule; the store (with `hi`, allocating it first); the draw;
   the cursor.

ASCII never comes here.

## 6. Glyphs: the font, and where each glyph comes from

**One font for the whole console**, a supplement to the ROM's Topaz. Topaz stays the look of every Latin-1 character:
it has been the console's identity since the first boot, and it is an 8 x 8 font nobody may ship (D20 4.2). The font
adds what Topaz lacks. Only the shown console is drawn, as today, so one pointer in `gfx.c` serves every console.

**The format, `ZCF1`** (Zenix console font). It is simple to check in one pass, has no pointers, and is big-endian
because its reader is a 68k. The parts follow one another in this order, so every 16-bit field is at an even offset:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `ZCF1` |
| 4 | 2 | header size: 32 |
| 6 | 2 | flags: 0 (any other value -EINVAL: room for later meanings, which an older kernel then refuses cleanly) |
| 8 | 1 | glyph width: 8 |
| 9 | 1 | glyph height: 8 (anything else -EINVAL; room for a taller console later) |
| 10 | 2 | glyph count G: 3 to 4,096 |
| 12 | 2 | half-page count P: 0 to 255 |
| 14 | 2 | reserved: 0 |
| 16 | 4 | total size: 32 + 512 + 256 x P + 8 x G |
| 20 | 12 | reserved: 0 |
| 32 | 512 | the index: one byte for each 128-code-point half-page of the BMP (`cp >> 7`): 0 none, else 1 + its number. Bytes 0 and 1 (U+0000-00FF, Topaz's) must be 0 |
| 544 | 256 x P | the half-pages: 128 entries of 16 bits each |
| 544 + 256 x P | 8 x G | the glyphs: 8 bytes each, a byte a row, bit 7 the left pixel (PSF's order) |

- **A half-page entry** is one of:
  - 0: no glyph (the replacement box);
  - 32-255: Topaz's glyph of that Latin-1 byte. This is a stand-in: `ł` -> `l`, `‐` -> `-`. Entries 1-31 are invalid;
  - 256 + n, below 256 + G: glyph n of the file.
- **Glyphs 0, 1 and 2** are the replacement box and the left and right halves of the wide box.
- Entries for U+FFFE and U+FFFF are ignored (those values are the kernel's own, section 5).
- **Alignment:** 32 + 512 + 256 x P keeps the glyphs at an even offset, and `mem_alloc` gives 16-byte-aligned blocks.
  A 68020 reads unaligned words anyway; a 68000 never runs Zenix.

**Why not PSF2**, Linux's format with a Unicode table (and D20 4.2's sketch):
- PSF2 maps each of the file's glyphs to its code points. So a stand-in that is a Topaz glyph (`ł` drawn as Topaz's
  `l`) cannot be said without shipping a copy of Topaz's glyphs, or a Zenix-only convention inside a "standard" file.
- Its table is UTF-8 strings of variable length: more parsing of untrusted data in the kernel.
- A Linux PSF2 font can still be used: the host tool (and later a Zenix command) converts it to ZCF1.

**Why not built into the kernel:**
- the kernel image is short of room (section 8);
- a font is replaceable data;
- the RAM floppy, which has no room for the file, still works: without a font, every character above U+00FF is the box
  (built into `gfx.c`, 24 bytes for the three boxes), and widths stay right (section 9).

**The default font**, `/usr/share/consolefonts/zenix-8x8.zcf`, is built at `make` time by `tools/mkconfont.py`
(*measured* with the prototype tool, 2026-10-28):

| Glyphs | Count | Source |
|---|---|---|
| Box drawing U+2500-257F | 128 | the 40 that CP437 has: `font_8x8.c`'s own glyphs; the rest generated by the tool in the same geometry. Light lines: vertical two pixels wide at columns 3-4, horizontal one pixel at row 4, as `font_8x8.c` draws them. Heavy: four pixels wide vertically (columns 2-5) and two high horizontally (rows 3-4), since a two-pixel heavy vertical would look like the light one. Double: as CP437's. Also the four rounded corners `╭╮╯╰` (the corner pixel cut), the dashes and the diagonals. A test checks that the generator, run over the 40, gives `font_8x8.c`'s glyphs |
| Block elements U+2580-259F | 32 | generated: eighths are whole pixel rows and columns in 8 x 8; quadrants; the three shades as CP437's patterns. Checked against `font_8x8.c`'s `░▒▓█▄▀▌▐` |
| Braille U+2800-28FF | 256 | generated (2 x 4 dots); for spinners and text graphs (`⠋⠙⠹`, btop-style meters) |
| CP437's other symbols | 57 | from `font_8x8.c` (Linux, GPL-2.0, already in the repository for D20): `☺☻♥♦♣♠•◘○◙♂♀♪♫☼►◄↕‼▬↨↑↓→←∟↔▲▼⌂`, Greek and mathematics (`αΓπΣστΦΘΩδ∞φε∩≡≥≤⌠⌡≈∙√ⁿ■`), `₧ƒ⌐` |
| Hand-drawn | about 54 | `tools/consolefont/symbols.glyphs`, Zenix's own (GPL-2.0-only), drawn as `#`/`.` pictures like `font_8x8.c`'s comments: the terminal symbols CP437 lacks (below), `€`, `…`, the curly quotes, en and em dashes, and the three boxes |

**Claude Code's characters** (*measured*: counted in its program file, 2026-10-28):
- Everything below is width 1 in both picolibc's and glibc's tables, unless marked as wide:
  - `⏺` (U+23FA) on every message and `⎿` (U+23BF) on every result;
  - the spinner `·✢✳✶✻✽`;
  - `❯` and `⏵` (U+23F5);
  - `✓✔✗✘`, and `☐☒` (to-do items);
  - `◯◉◼◻●○◆◇▪■□` and `▶◀▲▼▸▾‣`;
  - `⇧⌘⏎` and `※★⧉⏸⧗∴⋮≈`;
  - the box set `─│╭╮╯╰├└┌┐┘━┃┄═║` and `▎▏`;
  - the blocks and quadrants of its logo `█▐▌▛▜▝▘▖▗▟▀▄`, and the shades;
  - `…•·—–’“”→←↑↓` and the braille spinners;
  - the emoji `⚡✨✅❌⏳`, which are wide (width 2): they show the wide box.
- All but the emoji get a glyph. The spinner's six get six distinct glyphs, so it still turns.
- The list is a test input of the tool (`tools/consolefont/wanted`, one code point per line with where it was seen).
  The tool fails when one has neither a glyph nor a stand-in.

**Stand-ins** (a half-page entry 32-255, or another glyph of the file), 578 in the prototype:
- the letters of Latin Extended-A/B and Latin Extended Additional whose decomposition starts with a Latin-1 letter:
  `ł ś ž ő ă ơ ẽ` -> `l s z o a o e`. Polish, Czech, Hungarian, Turkish and Vietnamese names stay readable; the accents
  are lost;
- the dash and quote variants;
- the space variants (U+2000-200A, 202F, 205F) as a space;
- fullwidth ASCII U+FF01-FF5E as the ASCII glyph in a two-cell character;
- `−` as `-`, `⋅` as `·` (`∙` has CP437's own glyph), `‹›` as `<>`.

The list is a checked-in data file of the tool (generated once from Unicode's decompositions, then reviewed), so the
build does not depend on the PC's Python version.

**Not covered:** Greek and Cyrillic text (CP437's few Greek letters are there, a whole alphabet is not), other scripts,
and colour emoji. They show as the box (wide ones as the wide box). A later font may add them (owner question 2).

**Sizes** (*measured*, prototype):
- glyphs: 527 x 8 = 4,216 bytes;
- 25 half-pages = 6,400 bytes, plus the 512-byte index and the 32-byte header;
- **11,160 bytes** in all, in the file and once in memory, not per console;
- without braille: 2,560 bytes less (2,048 of glyphs and two half-pages);
- with 256-entry pages instead of half-pages, the map would be 8,192 bytes.

Braille could be drawn from the code point's bits with no table. That is rejected: it would need a reserved entry
value in a format about to be frozen, and a second drawing path.

**Licences:** `font_8x8.c` is GPL-2.0 (its notice already ships, `/usr/share/licenses/font_8x8`); the rest is Zenix's
own.

**Other bitmap fonts** (the roadmap's "other bitmap fonts for the console"):
- Any ZCF1 file made by the tool from a PSF2 or BDF font whose licence allows it.
- Block 5's TrueType fonts, rendered at 8 pixels, become ZCF1 files the same way (ROADMAP block 5: "also usable by
  the console").
- A font that replaces Topaz's Latin-1 glyphs too is **not** promised: that would be a second, global way to do what
  `CON_FONT` does. If it is ever wanted, it comes with a flag bit, which today's kernel refuses.

## 7. The interface

**`SYS_console` op 4, `CON_UFONT`** (claimed in CLAUDE.md first): the console's Unicode font, for every console.

- **Op word:** exactly 4, with no descriptor, like `CON_COLOURS`. The font is not one console's. Op 4 with a descriptor
  in the op word is -EINVAL. `syscall.c` dispatches op 4 itself; today every op but 1 goes to `term_conop`, which would
  compute the descriptor -1.
- **Root only** (-EPERM).
- **Arguments:** a2 a pointer to the ZCF1 data, a3 its size. a2 = 0 drops the font: every character above U+00FF is
  then the box, and widths do not change. Answers 0.
- **Errors:**
  - -EINVAL: a3 above the largest legal size (32 + 512 + 255 x 256 + 4,096 x 8 = 98,848 bytes), or any check below;
  - -EFAULT: the data is not in the caller's memory;
  - -ENOMEM: no memory (nothing changes);
  - -ENODEV: no display.
- **The checks** (on the kernel's own copy, made after the size bound and before anything is used):
  - the magic; header size 32; flags 0; width and height 8; the reserved fields 0;
  - glyph count 3-4,096; half-page count 0-255; the total size equal to a3 and to the formula;
  - index bytes 0 and 1 are 0, and every index byte is at most the half-page count;
  - every entry is 0, 32-255, or below 256 + the glyph count (as built: 32-126 or 160-255, and no two index bytes
    alike: section 19).
- **The switch:**
  - the kernel writes each mapped code point's width class (section 9) into the top three bits of its copy's entries
    (the entries need 13 bits);
  - one pointer store switches to the new font (drawing runs in process context only);
  - the old copy is freed, every console's pending character is cleared, and the shown console is redrawn.
- **On a kernel before D28** op 4 is -EINVAL (`term_conop`'s op test). init then says "this kernel has no Unicode
  font", not "a bad file".
- **Where the code lives:** the ZCF1 checker and the width function are a kernel file of their own (`kernel/confont.c`),
  built into the kernel and into the host tests (`term-test`, the font tool's test). The mutation tests of section 14
  then run against the kernel's own code; `term-test.c` replaces `gfx.c` with a model and could not reach them there.
- **Not dropped when its setter ends:** unlike `CON_FONT` and `CON_RULES`, `term_release` leaves it. It belongs to the
  system, not to a session.
- **D20's sketch replaced:** D20 4.2 meant "Unicode fonts (PSF2) told apart by their magic" under `CON_FONT`. Block 3
  takes this op instead, and `CON_FONT` stays PSF1 only, as frozen. Under "coherence first", the record is here.

**The 8-bit mode: one mode, two ways in.**
- **`CON_FONT` keeps its frozen meaning:** "8-bit mode: each byte one glyph of this 256-glyph font; no UTF-8 while
  loaded".
- **New: `CON_RULES` bit `CONR_8BIT` (128)**, "8-bit mode with the console's own Topaz". The `CONR_*` bits are frozen as
  append-only, and `CONR_ALL` becomes 255. It is owned and dropped like the other rules (its last setter's end); `ESC c`
  keeps it.
- In 8-bit mode, whichever way it was entered:
  - nothing is decoded;
  - the keyboard sends the keymap's Latin-1 bytes, as before D28 (section 10);
  - a cell holding a value above 0xFF (written before the mode came) is drawn as `?`: Topaz's, or the `CON_FONT`'s.
    The cell is not changed, and when the mode ends the character shows again.
- A `CON_FONT` implies the mode whether the bit is set or not. So there is still one 8-bit mode, now reachable
  without shipping a copy of Topaz (section 12).
- telnet's PC mode, built on `CON_FONT`, is unchanged byte for byte.

**`/etc/console`'s `font` line** is an addition to D14's file. init already warns about words it does not know and goes
on (`init.c` 54), so an older init only says so.
- `font auto` (or no line) loads `/usr/share/consolefonts/zenix-8x8.zcf` if it is there.
- `font none` loads nothing and saves 11.2 KB; widths stay right.
- `font NAME` loads `/usr/share/consolefonts/NAME.zcf`.

- init reads it after `colours`, **after `mount -a`** (a separately mounted `/usr` holds the file), and before the
  first login.
- A missing `/etc/console` counts as `font auto` (today `console_setup` returns at once when the file is missing,
  `init.c` 41: that changes).
- The file is read into a buffer from the heap, freed after the call; init never ends, so a static buffer would be
  permanent chip RAM.

init says on the console what failed (no file, a bad file, no
memory). The console then shows boxes for what it cannot draw. The shipped file gets `font auto` and one comment line.

**Not added:**
- `ESC % G` / `ESC % @` (ISO 2022's switch to and from UTF-8, Linux's): they would be a third way into the 8-bit mode,
  so they stay swallowed as today (`E_ESCI`).
- An op to ask which font is loaded: nothing needs it.

## 8. Drawing and speed (the golden path)

The console is on the golden path: "input to screen ... these get the careful code and a measured budget". So: **ASCII
no slower than today, and a Unicode character at most twice the cost of an ASCII one**, both measured with `bench`'s
screen line on the stock A1200.

**Today's cost per character** (*measured*, from the listing; a count, not a run):
- `gfx_cell` at 3 planes is about 180 instructions: about 75 of set-up, then 8 glyph lines of about 13 (gcc doubles the
  loop body);
- `put1`'s store and the call add about 35.

**The ASCII path after D28** (*measured* in the prototype's code, then refined by the second review):
- `put1` tests the byte for 0x80 or more on its printable path, where the byte is already in a register: 2
  instructions.
- The store reads the old `at` and tests `A_HI` (the overwritten-half rule): 3 instructions and one read.
- `gfx_cell` tests `A_HI` in the `at` it is already given, in place of revision 1's compare.
- `write_` is unchanged (revision 1 tested the decoder's state there: 4 instructions and a read).
- So: **about 5 instructions and one read a character, with or without the high plane**, against about 215: **about
  2-3%** on the stock A1200, where code and data are in chip RAM.

**Found while measuring (taken by the main session on main, 2026-10-28, independent of D28):**
- With the kernel's flags gcc indexes the text grid with a 32-bit multiply. That costs one `muls.l` in `put1`'s common
  store path, with 80 kept in a stack slot (chip RAM on the stock A1200); gcc's copy of the same code after a deferred
  wrap has a second one. `gfx_cell` also does one `muls.l` by the variable `lstride` for the line's address. So **two
  32-bit multiplies a character**, up to about 44 cycles each on a 68020 (knowledge base
  `demoscene/68k-cycle-costs-and-idioms.md`).
- **`-mtune=68020-40` is not a remedy:** it removes `put1`'s multiplies (+234 bytes), but turns divisions by constants
  in `show_back`, `scroll_up` and `csi` into the 64-bit `mulu.l`/`muls.l` that a real 68060 lacks. The Apollo 1260
  would crash on a scroll. `tools/check-68020-60.sh` catches it.
- gcc tuned for the 060 also turns shift-and-add (`(cy<<4)+(cy<<6)`) back into `muls.l #80`, so offsets must come
  from a table or a stored value, not from arithmetic.
- **The bigger lever is the stack** (the second review's count): `put1` and `gfx_cell` keep `cx`, `cy`, the depth and
  the colours in stack slots, push 3 + 5 arguments, and compare the glyph loop's end against the stack every line:
  about 75 stack accesses a character, all chip RAM on the stock A1200. So does the code: about 900 bytes are touched
  per character, more than the 68020's 256-byte instruction cache. Step 0 may remove the spills too, each change timed.
- The fix the main session takes:
  - a 64-byte table of row offsets in `term.c` (`cy` changes at 14 places, so a pointer kept beside it is easy to get
    wrong; a table cannot be folded back into a multiply);
  - a per-row byte-offset table in `gfx.c`, rebuilt at a depth change, and the scroll offset kept in bytes;
  - `bench` before and after on the A1200.
  It speeds up today's console whether D28 comes or not.
- **Done (main, 2026-10-28):** `rowx` in `term.c` and `top`/`rowoff` in `gfx.c` (kept by `set_off`/`set_rows`;
  `gfx_panic` recomputes `top`); no `muls.l` left on the per-character path (objdump), the kernel 140 bytes bigger,
  `tools/check-68020-60.sh` clean, one independent review. `sh`'s Del key is code 0x100, so a typed 0x80 is text
  (`cmds2-test`). The stack spills and the A1200 `bench` timing are still open.

**The Unicode path** (an estimate from the prototype's code, to be timed):
- decoding: about 20 instructions;
- the width: free for every character the font maps (section 9);
- the store: as above, plus the high byte;
- the glyph lookup: two reads (the index byte and the entry);
- **the glyph drawn byte-wise**, as `draw1` does: each plane's byte is `(b & m) ^ n` with byte masks in registers,
  about 4 instructions a plane, 12 a line at 3 planes. That needs no replication of the glyph's byte into a long.
- **A trap** (*measured*, as D14 found): gcc 16 turns every C form of that replication (`v |= v << 8; v |= v << 16`, the
  shifted-or form, even a 16-bit form) into `muls.l #0x01010101` (or `muls.w #257`), 44 cycles a line. So byte-wise
  drawing it is: no table, no multiply, and nothing to rebuild at a depth change.
- A stand-in that is a Topaz glyph draws exactly as Topaz does.
- Box lines and blocks are glyphs, not blitter work: an 8 x 8 cell costs the CPU less than setting up a blit.
- **A measured question for step 2:** if the A1200 times byte-wise drawing as fast as `rglyph`'s long-per-line path,
  then one drawing path serves every font. Topaz's 7 KB `rglyph` and each `CON_FONT`'s 8 KB table could go. D18 kept
  `rglyph` "for drawing speed" only. Decided with that number, not before.

**Kernel size:**
- the unoptimised prototype: `term.o` 8,090 -> 9,316 bytes, `gfx.o` 5,630 -> 7,054 (*measured*);
- with the op, the tty, the keyboard and the 2,492-byte width table (section 9): **about +5.5 KB of kernel**;
- the cold parts (`putcp`, the op's checks) are built `-Os`, only the decoder and the lookup `-O2`.

**The room is not there** on the mount tests' floppy:
- D27 measured 4,988 bytes left on 2026-10-28;
- D24 queues about 0.9 KB, D23 about 0.3 KB, and D20's pseudo-terminals 2-3 KB against the same room.

So step 2 starts by making room, as D20 step 1 did (the main session's choice, measured then): the same lever as D20,
a smaller `RAMDISK_MOUNT_K` (the mount tests' RAM disk). The other two images (`zenix-ram.adf` with 24 KB left, the
full root images) have room.

## 9. Widths: one table, the kernel's, from the same source as the C library's

The console and the program must agree on every width, or the screen tears (knowledge base `amiga/utf8-library.md`:
"Program and terminal must agree on every width").

**One source: picolibc's own tables** (`combining.t` and `wide.t` in the vendored C library, D27 step 1):
- Unicode 15.0.0: 359 zero-width and 86 wide ranges (*measured*).
- picolibc's `wcwidth` is compiled from them.
- A build tool writes the kernel's table from them: merged and packed, 267 BMP ranges as 16-bit pairs and 178 above
  the BMP as 32-bit pairs, **2,492 bytes** (*measured*).
- **Widths are the system's, not a font's.** With `font none`, a failed load or the RAM floppy, the console still
  counts a wide character as two columns, as the program does. No font file can carry other widths. Revision 1 put the
  lists in the font, which broke exactly these cases.

**The rules are picolibc's:**
- printable ASCII: 1;
- controls and U+0080-009F: -1 (the console ignores them);
- zero-width ranges: 0; wide ranges: 2;
- everything else: 1, including the East Asian "ambiguous" characters (`─ … → ●`), as in every non-CJK Linux locale.

**Agreement with Linux** (*measured*: every code point, picolibc 1.8.10 against glibc 2.44 `C.UTF-8`):
- They differ on 269 code points that are assigned in Unicode 16 (by Python 3.14's `unicodedata`; Arabic number signs,
  I Ching trigrams, some enclosed CJK numbers), counting also those where glibc answers -1. Counted instead as "glibc
  not -1 and the two differ", the second review found 422; many are Unicode 16-17 additions that picolibc's 15.0 lacks.
  Unassigned code points also differ (picolibc 1, glibc -1).
- **They differ on none of Claude Code's characters.** So a program on a Linux machine over `ssh` or `cu` lays out its
  screen as the console draws it, except for rare symbols.
- When picolibc is updated, the table is rebuilt in the same `make`. A test compares the kernel's width function with
  the C library's `wcwidth` for all 1,114,112 code points: zero differences.

**Fast:**
- Below U+0300 every printable character is 1, with no lookup.
- For a code point the font maps, the width class is in the top bits of its entry (section 7). It comes with the glyph
  lookup, for no extra read.
- This matters because the 128-code-point blocks that hold Claude Code's symbols also hold wide emoji: U+2300-237F and
  U+2380-23FF (`⌚⏳`), U+25xx (`◽◾`), U+2700-277F (`✅✨❌`). A coarse "this block is all width 1" bitmap would send
  `⏺⎿⏵●○◆▲▼✢✳✶✻✽❯` to a 9-step search on every character.
- Only characters the font does not map (CJK, beyond the BMP, everything when no font is loaded) search the lists: at
  most 9 steps.

**A known limit, not ours to fix:** emoji written with a variation selector (`❤️` = U+2764 U+FE0F) are 1 column by
`wcwidth` and by the console, but 2 in some Linux terminals. Linux's own console and glibc do as Zenix.

## 10. The keyboard and the tty

- **Keys are UTF-8.** `key_down` hands a keymap byte of 0x80 or more to `tty_input` as its two UTF-8 bytes (å =
  `C3 A5`). The keymaps stay Latin-1 tables (AmigaOS's own, D20), so nothing else in `kbd.c` changes. A keymap byte in
  0x80-0x9F would be a C1 control in UTF-8; the generated keymaps hold none, and such a key is dropped.
- **In 8-bit mode** (the shown console has a `CON_FONT` or `CONR_8BIT`; input always goes to `con_cur`, the shown
  console) the byte goes as it is, as before D28.
  - telnet's PC mode translates Latin-1 keys to CP437 (D20 3.3) and keeps working unchanged.
  - Ctrl and the escape codes are ASCII and do not change.
- **Cooked mode** (`tty.c`):
  - Backspace and `^U` remove whole characters. The line's last character is found by the console's own rules: up to
    3 bytes back to a lead whose length agrees, else one byte.
  - So the Latin-1 fallback's one-byte cells (`x B0 B0`, shown as 3 cells) are erased one byte, one column at a time.
  - Each erase echoes `\b \b` once per column the character took: 1 or 2, from the kernel's widths; zero-width ones were
    not drawn and take none.
  - This is Linux's `IUTF8` behaviour, always on here: every Zenix console is UTF-8 unless it is in 8-bit mode, where
    the erase is one byte, as before.
  - The line's limit stays 511 bytes (a Swedish letter is two of them: said in the man page).
  - **Limits, as today and stated:** Backspace over a Tab moves back one column; Backspace over a zero-width character
    removes it with nothing visible changing.
- `tty_inject` (the `ESC[6n` answer) is ASCII and does not change.

## 11. The locale, LANG, and the C library (D27)

- **`LANG=C.UTF-8`** is set where `TERM` is (`sh.c`'s defaults: one fact in one place, and architecture.md says so).
  Zenix has one character set. `LC_ALL` and `LC_CTYPE` are honoured if set: a user may set `C` to get bytes.
- Only the shell sets it. Programs started by init or from `/etc/svctab` (the servers, `wasabid` later) run in "C"
  unless their line sets it; none of them draws text for a person.
- **A trap in picolibc** (*read*, `locale/locale_names.c` `__find_locale`, `locale_private.h` 147):
  `setlocale(LC_ALL, "")` does not look at the environment. It answers the build's `_DEFAULT_LOCALE`, which is "C".
  POSIX says "" means `LC_ALL`, then the category's own variable, then `LANG`.
- **D27's port** (`libc/zenix/`) gets a `setlocale` wrapper that reads the three in that order, then uses picolibc's own
  name matching:
  - `C` and `POSIX` give "C";
  - `C.UTF-8` and `sv_SE.UTF-8` give UTF-8, and so does a name with no charset (`sv_SE`);
  - an unknown charset fails, as POSIX allows (the program then stays in "C").
- **Why a wrapper, not `-D_DEFAULT_LOCALE=locale_UTF_8`:** the define would ignore a user's `LANG=C`.
- Ported programs that call `setlocale(LC_ALL, "")` (most of them) then use UTF-8. Programs that never call it stay in
  "C", where picolibc's `mbrtowc` refuses bytes above 0x7F (standard C).
- **Widths in programs:**
  - The C library's `wcwidth` is compiled from the same tables as the kernel's (section 9).
  - D27 measured `mbrtowc` + `wcwidth` at +4.5 KB per program. D27's port may replace picolibc's `wcwidth` with one
    built from the kernel's packed table: 2,492 bytes against picolibc's 3,560 ((359 + 86) x 8; its "ambiguous" table
    is already left out, because `__locale_cjk_lang()` is always 0 without JIS: *read*, `locale_private.h` 300), about
    1 KB less per program. That is the main session's call in D27's step; the agreement test covers both.
  - A way for programs to use the kernel's copy instead (no table in each program) would be a new interface: not
    proposed now, and noted for D27 stage 2.
- **The rule for invalid bytes in programs** (`sh`, `ls`, `less`, `edit`, the text-UI library): one invalid byte, one
  column, as the console shows it (section 4).

## 12. telnet, cu, ssh and Zenix's own programs

- **telnet's PC mode** is unchanged: `CON_FONT`, 8-bit mode, Latin-1 keys translated to CP437.
- **telnet's Amiga mode** (D20 4.2's open choice, decided) sets `CONR_8BIT` with its other rules.
  - Topaz then draws the board's bytes exactly as today: the same glyphs, 0x80-0x9F included, at today's speed.
  - The keys are Latin-1, as today. No conversion, no doubled bytes, no new code in telnet beyond the bit.
  - Revision 1 had telnet convert Latin-1 to UTF-8. That doubled every byte from 0xA0 to 0xFF on art-heavy screens and
    dropped what Topaz draws for 0x80-0x9F. Drawing is the limit there (D20 6: about 7 KB/s).
- **telnet's Unix mode** passes the host's UTF-8 through (today it folds it to Latin-1).
  - Its keys pass through too: `tk_keys` stops turning Latin-1 into UTF-8, because the keyboard already sends UTF-8. If
    it kept doing so, å would go out as `C3 83 C2 A5`.
  - `-x`'s TERMINAL-TYPE stays `XTERM-16COLOR`.
- **The PC mode's fallback**, when the console refuses `CON_FONT` or the output is not a console (D20 3.5):
  - the screen side's CP437-to-Latin-1 table becomes CP437-to-UTF-8 (box lines stay box lines in a UTF-8 file or
    terminal);
  - the keys arrive as UTF-8 (no 8-bit mode there), so `tk_keys` decodes them before `tn_latin1_cp437`;
  - the screen table keeps BEL, BS, LF, CR and ESC as controls.
  - Before this change, å = `C3 A5` would map byte by byte: `C3` rings the bell and `A5` becomes CP437 0x9D, `¥`.
- **Ships together:** steps 2, 3 and 5 of section 15 go out in one release. The `telnet` package (D22: its own package)
  requires that base version or later. Between the console's change and telnet's, every mode would be wrong in some way.
- **`cu`** passes bytes both ways untouched: Linux's UTF-8 shows right, and the keys go as UTF-8. The PC's `getty`
  should be told `LANG=C.UTF-8`; `cu`'s man page says so.
- **`ssh`** (D21) sends `LANG`, as OpenSSH does with Debian's default `SendEnv LANG LC_*`. A Linux host that accepts it
  (Debian's sshd does) answers in UTF-8.
- **Zenix's own programs** (step 3, with the keys):
  - `sh`'s line editor moves, deletes and draws by characters and columns;
  - `edit` and `less` count columns by width and never split a character;
  - `ls` lays out its columns by width;
  - libz gets the decoder (the console's rules, one small function) until D27's switch brings `mbrtowc`.
  - Until this step merges, Swedish letters typed into `sh` would be two bytes and two columns. That is why it comes
    with the keys, not after them.
- **File names are bytes, as on Linux:** nothing in the file systems changes. A Latin-1 name made before D28 shows
  right by the fallback rule, and can be renamed.
- **Block 3's ports** (tui.library's replacement, Sumi, Maki, Markdown, Ahiru):
  - they already write UTF-8 and place wide characters as a lead and a tail (`amiga/utf8-library.md`);
  - on Zenix they take widths from the C library's `wcwidth` instead of utf8.library (D27 section 9's table);
  - their box lines and stand-in tables can stay: the console draws the real characters.

## 13. Memory on the stock A1200 (2 MB chip RAM, no fast RAM)

| Part | Cost | When |
|---|---|---|
| The default font | 11,160 bytes (*measured*, prototype) | once, at boot (`font none`: nothing) |
| The width table | 2,492 bytes (*measured*) | always (kernel code and data) |
| Other kernel code | about +3 KB | always |
| A console's pending character | 8 bytes | always (inside its struct) |
| A console's high plane | 2,560 bytes | the first character above U+00FF on that console |
| Its scrollback's high rows | 200 bytes of slots, plus 80 bytes for each line with such characters (at most 16,200) | the first such line scrolled into the scrollback |

**An example day:** Claude Code on one console and a shell on another. Say a third of its 200 scrollback lines hold box
lines or symbols:
- the font, 11.2 KB;
- the kernel, about 5.5 KB;
- the console's high plane, 2.6 KB;
- its scrollback rows, 0.2 + 5.3 KB;
- in all, about **25 KB**, of the 1,345 KB free with one login (D20 step 1, FS-UAE, 2 MB chip-only): 2%.

Consoles that only show Latin-1 pay nothing more. Measured again on the stock A1200 with `free` at step 5 (the font
loaded, a Unicode screen scrolled).

## 14. Tests

- **On the PC, the terminal model** (`tools/term-test.sh`, which builds `term.c` against a model of `gfx.c` in
  `tools/term-test.c`):
  - **the decoder**, against a table of cases, each case's cells compared:
    - every valid length and boundary: U+0080, U+07FF, U+0800, U+FFFD, U+10000, U+10FFFF;
    - every malformed class: lone continuations, C0/C1, E0 80, ED A0 (surrogates), F0 80, F4 90, F5-FF;
    - a character truncated at the end of a write and finished by the next;
    - a sequence broken by ASCII, by a control that acts, and by ESC that starts a sequence;
    - C1 as UTF-8 ignored; `C0 9B` as two cells;
    - UTF-8 inside an OSC title swallowed whole (`OSC 0;✳ x BEL`), and inside a CSI as today;
    - the kernel log in the middle of a character;
  - **the cells:**
    - `┠` (U+2520) and `†` (U+2020) survive `redraw_all`, a console switch and `show_back`;
    - `ESC c` after a Unicode screen leaves no stale glyph;
  - **wide characters:**
    - placement at columns 0, 78 and 79, with and without `CONR_WRAPNOW`;
    - each way of overwriting or erasing half; `@`/`P` at the edges;
    - the wide box's halves, and drawing decided from the stored tail;
    - the scrollback pool (rows taken, given back, the out-of-memory `?`); region scrolls;
  - zero-width characters dropped;
  - **8-bit mode** by `CON_FONT` and by `CONR_8BIT`: no decoding, `?` for high cells, the high cells shown again after
    the mode ends, `CONR_8BIT` dropped at its setter's end;
  - **`CON_UFONT`'s checks:**
    - each field of a valid file mutated: one -EINVAL each, index bytes 0-1 included;
    - a3 above the bound refused before any allocation;
    - -EFAULT, -EPERM, a descriptor in the op word (-EINVAL), -ENOMEM with nothing changed;
    - a drop; the font kept at its setter's end;
  - the planes allocated only on first use, and the `?` fallback when they cannot be;
  - **a fuzz** (random bytes, random writes split anywhere, random font loads) after which the screen equals the grid
    and no tail is without its lead.
- **The widths:** a host test builds the kernel's width function and the C library's `wcwidth` and compares them for
  all 1,114,112 code points (zero differences). It also checks the width classes `CON_UFONT` writes into the entries
  against the same function.
- **The font tool:**
  - the generator against `font_8x8.c`, pixel for pixel: CP437's 40 box glyphs, its blocks and its shades;
  - box arms meeting at the cell edges: a line of `─` and a column of `│` continuous across cells;
  - the wanted list all covered;
  - the file's own consistency (the kernel's checks, written in Python).
- **FS-UAE screenshots** (`tools/colour-test.sh`, `termtest`):
  - a `utf8` page showing every glyph of the font at 1, 2, 3 and 4 planes, compared with the file's bitmaps; Topaz for
    Latin-1 and for the stand-ins;
  - a screen in the style of Claude Code's (a rounded box, `⏺`/`⎿` lines, the spinner's glyphs, the logo's blocks, a
    wide emoji's box), pixel-compared;
  - the PC and Amiga modes' existing screenshot tests unchanged (`bbs-test.sh`), the Amiga mode now through
    `CONR_8BIT`.
- **Keys and tty** (`kbd-test`):
  - å typed arrives as `C3 A5`, and in 8-bit mode as `E5`;
  - cooked Backspace over å removes two bytes and one column; over a wide character two columns; over fallback bytes one
    each;
  - `sh`'s line editor with å, and with a pasted `─` and `À` (the 0x80 bug's case);
  - telnet's key tests in each mode (Unix pass-through, the PC mode with and without the font, the Amiga mode).
- **The C library** (D27's `libctest`):
  - `setlocale(LC_ALL, "")` with `LANG` set to `C.UTF-8`, `sv_SE`, `C` and unset, and with `LC_ALL` overriding;
  - `mbrtowc` of the decoder's valid test cases (agreement).
- **The step-0 build check:** `tools/check-68020-60.sh` over the new kernel (no 64-bit multiply or divide).
- **On the real machines** (the owner's):
  - `bench`'s screen line before and after step 0, and after step 2 (the budgets of section 8);
  - byte-wise against `rglyph` drawing;
  - a Claude Code session over `cu` on the stock A1200 and on the PiStorm;
  - a board in the Amiga mode;
  - `free` with the font and a Unicode console.

  No speed claim from FS-UAE.

## 15. Build order (each step small, merged with its tests and the full suite)

0. **Taken by the main session on main, 2026-10-28** (fix-when-found items, independent of D28):
   - the per-character 32-bit multiplies of section 8 (offset tables; `bench` before and after on the A1200), perhaps
     with the stack spills;
   - `sh`'s 0x80-as-Del bug of section 2.
1. *Side (host only, no shared file):*
   - `tools/mkconfont.py` and `tools/consolefont/` (the hand-drawn glyphs, the stand-ins, the wanted list);
   - the font built into `build/zenix-8x8.zcf`;
   - the width table's generator and the widths host test.

   It needs picolibc's two `.t` files. Until D27 step 1 vendors them, a copy lives in `tools/consolefont/` (with
   picolibc's licence line), and a test compares it with the vendored files once they exist; then the copy goes.
2. *Main:* the kernel.
   - First, the room on the mount tests' floppy (section 8).
   - `term.c`: the decoder state `E_UTF`, `putcp`, `A_HI` with the high plane and the scrollback pool, the wide rules,
     `term_log`'s aside.
   - `gfx.c`: the lookup, byte-wise drawing and the built-in boxes.
   - The width table; `CON_UFONT` and `CONR_8BIT`.
   - The term host test.
3. *Main:* in one step:
   - the keyboard's UTF-8 and 8-bit mode, and the tty's erase;
   - **`sh`'s line editor, `edit`, `less` and `ls` by characters and columns**, with libz's decoder;
   - the kbd test.

   The keys never reach a program that splits them.
4. *Main:* init's `font` line and `/etc/console`; the font on the full root images and in the base package
   (`mkroot.sh`, the Makefile); `LANG` in `sh.c`; the screenshot tests.
5. *Main:* telnet: the Amiga mode's `CONR_8BIT`, the Unix mode's pass-through both ways, the PC fallback's keys and
   table. `bbs-test.sh` green. **Steps 2-5 ship as one release** (section 12). Then the A1200 measurements (memory and
   speed).
6. With D27: the port's `setlocale`, and (if chosen) the packed `wcwidth`.
7. If the A1200's numbers say so: one byte-wise drawing path, and `rglyph` and the `CON_FONT` tables dropped (section
   8).
8. Optional, if decomposed text shows up: composing the Latin-1 pairs.
9. Later: a `confont` command for root (loading another ZCF1, converting PSF2 on Zenix).

## 16. For the main session (claims; not owner questions)

- **CLAUDE.md, numbers:**
  - `SYS_console` op 4 `CON_UFONT`: no descriptor; root; a2 ZCF1 data, a3 its size; a2 0 drops;
  - `CONR_8BIT` 128 (`CONR_ALL` 255).
- **CLAUDE.md, frozen-interface wording:** the op word's "0 above the op: no descriptor, only `CON_COLOURS`" becomes
  "only `CON_COLOURS` and `CON_UFONT`".
- **CLAUDE.md, frozen once built:**
  - `CON_UFONT`'s number, arguments, errors, checks and size bound, and that `term_release` keeps it;
  - the ZCF1 format: the header's offsets, the order of the parts, the index (bytes 0-1 zero), the entry meanings
    0 / 32-255 / 256+n, glyphs 0-2, big-endian, flags 0;
  - the path `/usr/share/consolefonts/zenix-8x8.zcf`, and `/etc/console`'s `font auto|none|NAME`;
  - `CONR_8BIT`'s meaning, and the 8-bit mode's keys (Latin-1), for both of its ways in;
  - the console's UTF-8 rules of section 4 (no decoding inside escape sequences, the Latin-1 fallback, C1 ignored), as
    documented behaviour.
- **Kernel:**
  - `term.c`, `gfx.c`, `tty.c`, `kbd.c`, `syscall.c` (op 4's own dispatch);
  - `kernel.h`: the `gfx_cell` arguments, the new functions, `CONR_8BIT`, `A_HI` (internal);
  - the generated width table and its build rule.
- **Makefile:** the font's rule (`build/zenix-8x8.zcf` from the tool and its inputs), the width table's rule, the root
  images and the base package, and the mount floppy's room.
- **usys.h:** `CON_UFONT`, `CONR_8BIT` and `sys_conufont`.
- **Packages:** `telnet` requires the base version of steps 2-5.
- **Docs:**
  - architecture.md section 11: the terminal, the display, the keyboard and the tty;
  - features.md;
  - limits.md: wide, zero-width, beyond the BMP, the 511-byte line;
  - man pages `init` (the `font` line), `telnet`, `sh` and `cu`.
- **D27:** the `setlocale("")` trap (section 11) belongs in its port; the packed `wcwidth` is a choice for its step.
- **D21 (ssh):** send `LANG`.
- **An accepted change of meaning, recorded as audit3 B7 was** (CLAUDE.md's frozen list): the bytes a console
  shows and the keys it sends change from Latin-1 to UTF-8 for every program built before D28. Accepted because the
  programs that depend on it (`telnet`, `sh`'s editor) change in the same release (steps 2-5), the 8-bit mode keeps the
  old meaning for those who ask for it, and the fallback keeps old Latin-1 text readable. Any later change is a new
  rule bit.
- **D20 4.2's sketch** ("Unicode fonts ... PSF2 told apart by their magic") gets one line pointing here, in the main
  session's commit (one fact in one place).
- **Unchanged and checked:** `CON_FONT`, the `CONR_*` bits 1-64, the op-word rule for ops 2-3, telnet's PC mode on the
  wire and on the screen, and ext2 and ZenixFS names (bytes).

## 17. Questions for the owner (what Zenix should be; one line each, the recommendation in brackets)

1. Should Zenix speak UTF-8 everywhere, like Linux (screen, keyboard, file names), while old Amiga Latin-1 text still
   reads right? [yes]
2. Greek, Cyrillic and other alphabets on the console: a box for now, and an add-on font later if you want one? [yes:
   a box now; Western letters, box lines, blocks and symbols are in]

**Decided by the main session (technical, 2026-10-28):** the Unicode font is loaded at every boot, also on the stock
A1200 (11.2 KB), and `font none` in `/etc/console` turns it off.

## 18. Reviews

Two independent read-only reviews of revision 1 (2026-10-28), each run twice (the first runs reported after the
second ones; their findings overlapped and are all here):
- **Review 1:** correctness against the code and the frozen interfaces.
- **Review 2:** speed and memory on the stock A1200, and fit with D14, D18, D20 and D27.

Every finding is worked in:

| Finding | Where it went |
|---|---|
| (1) Revision 1 ended an escape sequence on a decoded character; inside OSC that would print the rest of every UTF-8 window title (`OSC 0;✳ Claude Code`) | nothing is decoded while a sequence is open; today's parser byte for byte (4) |
| (1) `redraw_all`/`show_back` take a cell whose low byte is a space for a blank: `┠` U+2520 would vanish | the `A_HI` mark in `at` (already tested); `show_back` tests the line's high row (5); a test (14) |
| (1) `reset_term` would leave a stale high plane | `at`'s clear makes `hi` unread (5); a test |
| (1) telnet's keys: the Unix mode would double-encode å, the PC fallback would map UTF-8 byte by byte | `tk_keys` passes through or decodes per mode (12); key tests (14) |
| (1) Old telnet on a new kernel and the gap between steps | steps 2-5 ship together, and `telnet` requires that base (12, 15, 16) |
| (1)(2) Widths in the font would vanish with `font none` or the RAM floppy, and a font could carry other widths | the kernel owns the width table, built from picolibc's tables; ZCF1 has no widths (9) |
| (1) A byte index cannot name 512 half-pages | at most 255 (6, 7) |
| (1) The wide box's right half could never be drawn | drawing decided from the stored tail (5) |
| (1) The frozen format lacked offsets and some checks; an unbounded copy | the header table, the order, at least 3 glyphs, the reserved zeros, a3 bounded before the copy (6, 7) |
| (1)(2) Latin-1 half-pages "later" inside a format frozen now; a second way to replace Topaz | index bytes 0-1 must be 0; any later meaning needs a flag bit (6) |
| (1) The grid's high plane's allocation failure; the built-in box | `?` and a log line; 24 bytes in `gfx.c` (5, 6) |
| (1) The op-word wording, op 4 with a descriptor, D20's PSF2 sketch, release at exit | (7, 16) |
| (1)(2) `sh`'s 0x80 bug and the editor before UTF-8 keys | the bug taken by the main session now; the editors in the keys' step (15) |
| (1) The replication table and depth changes | no table: byte-wise drawing (8) |
| (1) `term_log` and `term_panic` versus a pending character | the aside, and the clear (4) |
| (1) Wide characters at columns 78 and 79; keymap bytes 0x80-0x9F; the Amiga mode's 0x80-0x9F glyphs; the replication table missing from the memory sum | (5, 10, 12, 13) |
| (2) `-mtune=68020-40` brings 64-bit multiplies that crash a 68060 | dropped; offset tables; `check-68020-60.sh` (8) |
| (2) The floppy room was stale (4,988 bytes, already claimed by D23, D24 and the pseudo-terminals) | stated, and room made first in step 2 (8) |
| (2) The multiply count (one per character in `put1`, its copy after a wrap, one in `gfx_cell`); stack-slot locals | corrected; the main session's step 0 (8) |
| (2) A cheaper ASCII path | `E_UTF` in the parser state and `A_HI` in `at`: about 5 instructions and one read (4, 8) |
| (2) The baseline was too low (about 215 instructions, not 100-120) | corrected (8) |
| (2) The replication trap is wider (every C form, a 16-bit one too) | byte-wise drawing; a measured question about `rglyph` (8, 15) |
| (2) The width bitmap misses exactly Claude Code's blocks | width classes in the entries' top bits (7, 9) |
| (2) A whole scrollback high plane reaches the 48 KB D14 refused | the per-line row pool (5) |
| (2) The Amiga mode's conversion doubles bytes and slows art | `CONR_8BIT`: Topaz, no conversion (7, 12) |
| (2) Cooked erase over invalid bytes | the console's own rules backwards (10) |
| (2) `setlocale`: `sv_SE` gives UTF-8, an unknown charset fails; the define alternative | (11) |
| (2) Braille's size; drawing braille from its bits | 2,560 bytes; rejected, with the reason (6) |
| (2) The free-memory figure was from before the diet | 1,345 KB (13) |
| (1) `tty_write`'s pieces and `kwork`'s echo could break a character | pieces end on a character boundary (4) |
| (1)(2) How the box for 0x80-0x9F is stored; junk bytes allocating the planes | the byte itself, drawn as the box (4) |
| (1) The keys' change of meaning for older programs | recorded as an accepted change (16) |
| (1) init: a missing file, `/usr` mounted later, the read buffer; an older kernel's -EINVAL | (7) |
| (1) Tab and zero-width erase | stated limits (10) |
| (1) The host test cannot reach checks inside `gfx.c` | `kernel/confont.c` (7) |
| (1) A side session placing files in D27's tree | a copy in `tools/consolefont/` until then (15) |
| (1) No allocation while a panic prints | (5) |
| (1) The heavy vertical looked light; `∙` both glyph and stand-in | four pixels; the stand-in dropped (6) |
| (1) The PC fallback's screen table and controls; `LANG` outside the shell | (11, 12) |
| (2) picolibc's linked width tables are 3,560 bytes (no ambiguous table) | (11) |
| (2) How the glibc difference was counted (269 or 422) | (9) |
| (2) The stack traffic and the code fetches, a bigger lever than the multiplies; shift-and-add back to `muls.l` | (8) |
| (2) Braille from its bits saves 2.5 KB | still rejected: one drawing path and a frozen format without special values weigh more; the main session may revisit before step 2 freezes the format (6) |

From the coordinator, the same day: step 0 and `sh`'s bug are taken by the main session on main. The memory question
was technical, so it is decided by the main session (section 17).

Last checked against the code: 2026-10-28 (branch d28 at bf93534).

## Owner decisions (2026-10-28)

The owner answered **yes to every question above** (2026-10-28, "I think yes for all questions"): Zenix speaks UTF-8 everywhere, like Linux (screen, keyboard, file names), while old Amiga Latin-1 text still reads right; Greek, Cyrillic and other alphabets show as a box for now, an add-on font later if wanted. This is the yes to the note: built in block 3's order (after block 2's audit); step 0 is done on main.

## 19. As built (branch utf8, 2026-10-29)

Steps 1-6 of section 15, built in one branch (steps 2-5 ship together, section 12). What differs from the sections
above, and the choices the note left open, taken conservatively (the builder's, for the main session to confirm):

- **Step 1 on main's files, not a side copy:** D27 had vendored picolibc, so the width table is generated straight
  from `libc/picolibc/newlib/libc/string/combining.t` and `wide.t` (`tools/mkconfont.py widths`, into
  `$(BUILD)/conwidth.h`); `tools/consolefont/` holds the hand-drawn glyphs (`symbols.glyphs`), the stand-ins
  (`standins`, generated once with Python's `unicodedata` and checked in: 584, Latin Extended letters by their
  decomposition kept up to the longest Latin-1 letter, so `ǖ` is `ü`; `ł đ ħ ı` added by hand, they have none) and the
  wanted list (`wanted`, 103 characters).
- **The width table is 1,816 bytes, not 2,492:** the ranges above U+FFFF are stored as 16-bit pairs grouped by plane
  with an 18-byte index of each plane's first range (no range crosses a plane); the same 1,114,112 answers (the host
  test). One function, `kernel/cwidth.h`, is compiled into the kernel (`confont.c`) and libz (`zu8width`), so the
  console and Zenix's own programs share it; each program that lays out text pays the table once (sh, ls, less, edit:
  about +3.5-5 KB each with the code).
- **The default font:** 531 glyphs (128 box drawing: the 29 doubles are CP437's own, all of Unicode's are in CP437,
  the light and heavy ones generated and checked against CP437's 11 light ones; 32 blocks; 256 braille; 57 CP437
  symbols; 3 boxes and 55 hand-drawn), 22 half-pages, **10,424 bytes**.
- **Speed (instruction counts, not timings):** the compiled kernel's `put1` with `gfx_cell` run in an instruction-set
  emulator on the PC (Unicorn), 3 planes: an ASCII character 261 instructions before D28, **266 after** (+5, the
  budget); `─` or `⏺` from the font about **528** (1.98 times; the budget is twice), `å` as two UTF-8 bytes 362. Two
  traps fixed on the way: gcc split `gfx_cell` (an extra call on every character) once a second caller existed in
  `gfx.c` (`noipa` and an always-inline `put_glyph` keep it whole), and turned `b & m` with `m` known to be 0 or 0xFF
  into `muls.w` (the masks are made opaque with an empty `asm`). No timing on the A1200 yet.
- **Sizes:** the kernel 157,640 -> **166,852 bytes** (+9.2 KB: term 8,652 -> 12,558, gfx 6,086 -> 7,770, confont
  3,018 new, tty +368; budgets in `tools/ksize-budget`); the mount tests' RAM disk 716 -> **712 KB** for it (4,156
  bytes left for the kernel there, 25 free blocks on the disk); the RAM floppy 77,884 bytes left for the kernel;
  `sh` +4.2 KB, `ls` +3.1 KB, `less` +3.4 KB, `edit` +4.5 KB, `init` +1.3 KB (the width table and the code).
- **Memory** (FS-UAE's stock A1200, 2 MB chip RAM only, one login): free 1,360 KB before D28, **1,335 KB** after (the
  font 10.2 KB, the kernel, the bigger shell); 300 lines of Claude Code-like output (a box line with its marks in every
  third line) then take 41 KB, of which 32 KB is the scrollback every console takes when it first scrolls and about
  9 KB the high plane and the 100 scrollback rows: about 34 KB of D28 in all, 2.5% (emulator figures; `free` on the
  A1200 still to come).
- **Decisions the note did not take:**
  - `CON_READ` gets a second form, a3 1 (`CONRF_CELL4`, claimed): 4-byte cells with the 16-bit value first, which the
    tests (and later Wasabi's screen text) need; the 3-byte form gives the low byte and never shows `A_HI`.
  - In 8-bit mode a wide character's right half draws as a space (its left half `?`).
  - Writing over half of a wide character blanks both halves' cells, keeping each cell's colour.
  - The `/etc/console` font line takes a plain name only (no `/`, at most 23 letters); `font auto` with no file
    (the RAM floppy) is silent; init tells an old kernel from a bad file by asking op 4 to drop first.
  - Outside 8-bit mode Topaz's glyphs for 0x80-0x9F are swapped for the box when the shown console's mode changes
    (256 bytes kept aside), so the Latin-1 path pays nothing for the box.
  - telnet's keys in a BBS mode are decoded from UTF-8 whenever the console is not in 8-bit mode (the PC font refused,
    or the rules refused); a box line pasted into the PC mode goes as its CP437 byte; a character an Amiga board
    cannot take rings the bell. ssh's keys and screen pass UTF-8 through as the Unix mode does.
  - Step 6: `libc/zenix/locale.c` wraps picolibc's `setlocale` (compiled as `__pico_setlocale`): `""` reads `LC_ALL`,
    `LC_CTYPE`, `LANG`; a name with no charset is UTF-8. picolibc's own `wcwidth` stays (the packed one was optional);
    `ZENIX_VNUM` is not raised here (the release step's).
  - The text-UI library is built `TUI_UTF8=1` by default (Sumi, Maki, Markdown, tuitest: UTF-8 out, the C library's
    widths).
- **Tests:** `tools/term-test.sh` (the decoder's table, the cells, wide and zero-width characters, the scrollback pool,
  8-bit mode both ways, every `CON_UFONT` check against the built font, `CON_READ`'s 4-byte cells, a 300,000-piece
  fuzz; `build/conwidth-test`: every code point against picolibc's own `wcwidth.c`), `mkconfont.py test`
  (`confont-test` in `make check`), `tools/host/telnet-test.sh` (the CP437 table, the keys per mode), and in FS-UAE
  `tools/con-test.sh` (`termtest utf8`: every glyph of the font and a Claude Code-style screen read back from the
  display through `CON_LINE` at 2, 4, 8 and 16 colours; `termtest erase`: cooked Backspace by character; sh's editor;
  raw keys as UTF-8 and in 8-bit mode as Latin-1), `tools/kbd-test.sh`, `tools/libc-test.sh` (`setlocale`, `mbrtowc`
  against the decoder's cases), `tools/tui-test.sh` and the three ports' tests, `tools/bbs-test.sh`.
- **The review (one independent read-only review of the decoder, the cells and the pool, `CON_UFONT`'s checks):**
  fixed: the scrollback view drew its live rows' cells above 0xFF from their low byte; two index bytes naming one
  half-page passed the check (the widths of the wrong block, and on the host an entry read past the glyphs): now
  -EINVAL, in `mkconfont.py check` too, with a mutation test each; stand-in entries 0x7F-0x9F (whose Topaz glyphs are
  the box outside 8-bit mode) refused, so the check takes 32-126 and 160-255 (a tightening of sections 6 and 7,
  before the format is frozen); the scrollback's first allocation and `term_log` on a console never used no longer
  allocate while a panic prints. Left as stated limits: Backspace right after a character in the last column (the
  deferred wrap: `\b` goes to column 78, as on Linux's console; a wide character there takes one column too many,
  and before D28 a narrow one was off the same way), and a character whose bytes arrive split across two input runs
  (serial input, `CON_INPUT`) can be broken by output written between them, or a program's write ending inside a
  character by the next echo (only `tty_write`'s own pieces are kept whole).
- **Not done (later steps or the A1200):** step 7 (one drawing path; needs the A1200's timing of byte-wise drawing
  against `rglyph`), step 8 (composing accents), step 9 (`confont`); `bench` and `free` on the stock A1200.


## Main session, 2026-10-30: steps 1-6 merged

- **The version number** is bumped with the merge (`ZENIX_VNUM` 2026103000), so the telnet package's "needs this base"
  tells this base from an older one; the readable version stays 0.2 until a release step names one.
- **wasabid's TEXT** switches to the 4-byte `CON_READ` form (`CONRF_CELL4`) together with the Wasabi client learning
  UTF-8 text (a D24 section 11 addition), as one change; until then TEXT gives only the low byte of a character
  above U+00FF (the 3-byte cell form), so such characters read wrong over the remote control (read in term.c).
- **con-test** runs `termtest utf8` at 8 colours (the stock A1200's default) and 16 (AGA) only; `conops` still runs at
  all four depths. The suite's time goes back down (the four-depth glyph page took about 12 minutes).
- The helper's other decisions (section 19) are accepted as built.

## 20. Faster and without flicker (2026-11-04, branch con)

From the owner's partner's A1200 with the Apollo 1260 (68060 at 50 MHz, data cache on, release 2026110401):
`time seq 1 2000` took 4.503 s on the screen and 0.044 s to /dev/null; `time ls -laR /` (506 lines) 3.2 s and 0.94
s. And typing at the prompt made the `z` of `zenix` and the first character of the prompt flash.

- **The ring of rows (gfx.c).** The buffer was twice the screen's height: a scroll moved the Copper's pointers one
  row down, and every 32 scrolls the blitter copied the whole screen back to the top (80 KB at 4 planes; a stall of
  tens of ms, about 0.7 ms a line). Now the buffer is a ring of text rows (`nring`: the screen's rows plus as many
  spare ones with fast RAM, half as many without) and display row r is ring row (base + r) mod nring (`rowoff[]`, so
  `gfx_cell` stays one table read and an add). The Copper list has a second set of plane pointers (the ring's row 0)
  after a WAIT for the line where the ring wraps on the screen; a line from 256 on is two WAITs (`$FFDF`, then its
  low 8 bits), and when the ring does not wrap on the screen the WAIT is the picture's end. A full-screen scroll is
  base + 1 and one blitter fill of the incoming row, filled before `base` moves (the vertical blank cannot show it
  half done); nothing is ever copied. Region scrolls (CSI S/T/L/M, a scroll region) and fills are one blit per run
  of rows that lie in one piece of the ring (upwards from the first row, downwards from the last).
- **Never drawing where the display looks.** The display shows the ring from `hw_base` (the vertical blank applies
  `base`); with k scrolls in one frame the drawing is k rows ahead. The incoming row is out of sight exactly while
  1 <= k <= nring - trows, so a scroll beyond that waits for the vertical blank (`vb_room`: not with interrupts
  masked, 100 ms timeout said once, then no more waits). With fast RAM that is 32 scrolls a frame (1,600 lines a
  second on PAL) before anything waits; without, 16 (800 lines a second, far more than a stock 68020 draws). `ESC[2J`
  and redraws fill the screen's rows only (and those the display still shows, if it shows an older base).
- **Chip RAM:** unchanged with fast RAM (2 x trows rows: 160 KB at 4 planes, PAL); 25% less without (1.5 x trows:
  90 KB at 3 planes instead of 120 KB, PAL). The Copper list grows from 160 to 200 bytes.
- **Unchanged cells are not drawn (term.c).** A cell stored again with the same character, attributes and colour (not
  above U+00FF) is not drawn again; an erase (`ESC[K`, `ESC[X`, `ESC[2K`) skips cells already blank in its colour,
  and a whole row already blank is not filled. This rests on the rule that the shown console's planes equal its grid
  between writes (the cursor is hidden at a write's start): every change of font, 8-bit mode, depth or palette, the
  scrollback view, a console switch and the hand-over to a display server redraw from the grid; during a panic
  every cell is drawn. Glyphs were already written in one pass per plane (the final byte, never a clear first), so
  a cell drawn again never shows a blank; the flash came from the shell: it wrote `\r`, the prompt, the line and
  `ESC[K` as separate writes, and the cursor was drawn at the end of each, so at column 0 (the `z`) for a moment.
- **The shell's line editor (sh.c redraw)** keeps the line it drew (the text and the grey suggestion, the cursor's
  column) and writes only from the first byte that differs, `ESC[K` only when the line got shorter, then moves the
  cursor back, all in one write. A key typed at the end of the line writes that character alone. A prompt that leaves
  a colour on has its colour codes written again before such text. After Ctrl-L, a Tab list or Ctrl-R's line the
  whole line is drawn again. Output from another program onto the line is no longer repaired by the next key (as in
  bash; Ctrl-L redraws). sh: +864 bytes of code (shared), +952 bytes of memory per shell (the line kept, the output buffer).
- **The serial copy (serial.c)** waits for the port only while a byte was received in the last 10 s; after that it
  drops what does not fit its 4 KB ring and counts it, as before the first byte. A terminal that listens without
  typing for longer than 10 s loses output that does not fit (the request: a cable left in after its terminal closed
  must not hold the screen to 11.5 KB/s). FS-UAE sends without the baud delay, so the tests never see a drop.
  `conmirror off|on` (root, `SYS_ttymirror`) switches the copy off and on.
- **Tests:** `tools/scroll-test.sh` (screenshots, 4 planes with fast RAM and 3 without, DIWV=0x38, which also
  reaches the exactly-256 WAIT): after `seq 1 2000` the screen is CON_LINE's picture of the ring pixel by pixel and
  the grid holds 1970..2000; a page of scrollback after it is the same lines printed afresh; `termtest scroll`
  (region scrolls, inserted and deleted lines) from three places in the ring equals a redraw and CON_LINE's picture.
  `tools/term-test.sh`: unchanged cells and blank erases draw nothing, changes are drawn. `tools/serial-test.sh`: the
  10 s rule both ways. `tools/lineedit-test.sh`: typing at the end sends only the character. Not run: NTSC and the
  default DIW_V `$2C` build by screenshot (the same code; the WAIT pair for lines 256-300 there).
- **Sizes:** kernel.bin 180,720 -> 182,036 bytes (gfx +842, term +248, serial +201; tools/ksize-budget); sh +864
  bytes of shared code; conmirror 556 bytes.
- **Emulator figures** (FS-UAE, `time` inside Zenix; not hardware, and FS-UAE's serial port has no baud delay, so
  the serial copy's cost does not show): the stock A1200 (68020, 3 planes): `seq 1 2000` on the screen 13.14 ->
  12.01 s (to /dev/null 0.67 s), `ls -laR /` 19.48 -> 18.71 s (8.21 s); the 68060 matrix config (4 planes): `seq 1
  2000` 7.60 -> 6.04 s (0.20 s), `ls -laR /` 7.13 -> 6.58 s (2.74 s). The hardware "after" is the owner's partner's.

## 21. Drawing at the end of a write (2026-11-07, branch confast)

The real-world benchmark (docs/ROADMAP.md): `time ls -laR /` took 12.8 s on the text console against 4.36 s to
/dev/null on the A1200 with the 1260, and 1.37 s against 0.32 s in a desktop term window on the PiStorm. Where the
time went, measured in FS-UAE with a profiling build (the E-clock around each part; emulator figures, the stock A1200
with 4 MB of fast RAM, 4 planes, the serial copy off):

- **Every scroll drew its row and filled the next one at once**, so a write of 2,000 short lines drew 2,000 lines,
  most of them pushed off the screen moments later, and more than 32 scrolls in one frame waited for the vertical
  blank (1,600 lines a second at most with fast RAM, D28 20).
- **Every scroll moved the grid:** three `memmove`s of 31 rows of 80 bytes (7.4 KB a line).
- **Every glyph was one byte store per plane and line:** 32 chip RAM writes a cell at 4 planes, through a second
  glyph table that held each line four times in a long (7 KB, and 8 KB more for each loaded font).
- **The serial copy** (D28 20's rule: it waits for the port while a byte was received in the last 10 s) adds its own
  time when a terminal on the cable types; FS-UAE's port slows it too (measured once, 2026-11-07, each command typed
  on the serial line: the 600 long lines took 5.52 s on the stock A1200 with the copy waiting, 3.61 s without), so
  the note in section 20 that it has no baud delay is not right.

What changed (the D28 20 rule stands: between writes the shown console's planes equal its grid):

- **Notes during a write, drawing at its end (term.c).** While a write runs the terminal stores cells in its grids
  and, for the shown console only, notes the cells changed on each row (a span), the rows to draw whole (scrolled
  in, erased: a bit each) and the full-screen scrolls (a count). `flush` at the write's end brings the screen to the
  grid: one `gfx_jump` for all the scrolls, the whole rows filled by the blitter (runs of rows in one colour, the
  colour of the row's last cell; one blit for all planes when the background is black or white) and their cells not
  blank in that colour drawn, then each row's span. Anything that draws at once flushes first: the cursor, a region
  scroll or inserted/deleted lines (the blitter moves what is on the screen, so the screen must be the grid before
  the grid moves), the scrollback view; a console switch, a font, mode, depth or palette change redraws whole rows.
  The notes are a ring as the screen is (`LZ(y)`): a scroll turns it. A printable ASCII run on one row is stored by
  a loop of its own in `write_` (no call and no tests per byte beyond its range); `term_write_some`'s two-tick bound
  is checked after each run (a row at most) instead of each byte.
- **`gfx_jump(k)` and `gfx_done` (gfx.c).** The drawing has its own base (`dbase`), ahead of the one the vertical
  blank shows (`base`) until `gfx_done`, so no row is shown half drawn. Within the spare rows a jump only moves
  `dbase`. Beyond them, with fast RAM (as many spare rows as the screen's), the whole screen is drawn afresh in the
  half of the ring not on show: nothing ever waits, and however much scrolled past within one write, one screenful
  is drawn. Without fast RAM (16 spare rows) it waits for the vertical blank (only when something new is waiting to
  be shown; 100 ms timeout said once, never with interrupts masked); a jump of more than 16 rows then draws the whole
  screen 16 rows on, the rows it shares with the shown picture drawn while on show until the next frame (the 2 MB
  chip-only machine is best effort; the final picture is always right).
- **The grid is rows through a table (`rx`, term.c).** Display row y's cells are at `rx[y]` in each grid (and the
  high plane); a full-screen scroll turns the table (31 words), a region scroll turns its part; no cell is copied.
  A scrollback push copies a row without attributes as it is (the colour fold only for rows with inverse or high
  cells). CSI `@` and `P` are one loop over the grids.
- **Glyphs from their 8 bytes (gfx.c `gfx_cells`).** Four cells in one attribute and colour that start at a column
  that is a multiple of 4: each glyph line's four bytes gathered in a register, each plane one long-word store
  `(G & m) ^ n`, a quarter of the chip RAM writes; four spaces in Topaz without reading a glyph. Others, bold and
  underline, the cursor and the cells above U+00FF one by one (`put_bytes`, one small loop over the planes). The
  replicated glyph tables are gone (7 KB of RAM with Topaz, 8 KB more per loaded font). The CPU never reads chip RAM
  to draw. Measured in FS-UAE, the 16-bit blitter's fill of whole rows (2 slots a word) was faster than the 020's
  long-word stores to chip RAM there, so whole rows are still filled by the blitter.
- **The serial copy** is as section 20 left it (ROADMAP's item: done there, 2026-11-04): the screen waits for the port
  only while a byte was received in the last 10 s, and `conmirror off` (root) stops the copy. Not changed: a terminal
  that types on the cable is listening, and losing its output would break it and the test rig.

Interfaces: `gfx_scroll` is gone; `gfx_jump`, `gfx_done` and `gfx_cells` are new (kernel.h), and the two other
users of term.c follow: the desktop's term model (display/term/model.c: a whole row is still one fill order and the
cells up to its last one not blank, as before) and the host test's model (tools/term-test.c: the rows a jump brings
in are junk the terminal must draw over). Nothing a program sees changes (no system call, struct or message).

Room (tools/ksize-budget): kernel.bin 183,508 -> 182,820 bytes, 688 smaller (gfx 9,184 -> 8,010: the replicated
tables and the per-depth glyph code gone; term 12,946 -> 13,428); the mount tests' floppy has 1,500 bytes free
(800 must stay), so 700 bytes are there for later work (the console mode switch, CON_MODE, and ctop's CPU time need
about 1-1.5 KB: the rest must be found then). RAM: 7 KB less with more than one plane (the glyph table), 8 KB less
for each loaded font.

Tests: `tools/term-test.sh` (host): the model's jump fills the rows it brings in with junk, so every row the terminal
does not draw shows; 20,000 big writes of up to 300 random pieces (scrolls past the screen, region scrolls, erases),
a third of them with the whole-screen jump, each followed by the screen = grid check; `seq 1 2000` in a few writes:
one jump a write, at most a screenful drawn; the existing checks (unchanged cells not drawn, a blank row erased
again not filled, region scrolls one blit and one fill, the fuzz runs) pass. `tools/scroll-test.sh` (screenshots
with and without fast RAM: the ring after `seq 1 2000`, scrollback, region scrolls from three places) and the other
console tests in FS-UAE. `tools/conspeed-test.sh` (new, with `rootfs/tests/conspeed.sh`): the figures below, at
stock speed, each console part under a ceiling (today's figure and 30%; the figures before this change fail all
eight).

Emulator figures (FS-UAE, `time` inside Zenix, the serial copy off; not hardware), on the screen before -> after,
to /dev/null in brackets:

| Output | Stock A1200 + 4 MB fast (68020) | The 68060 config |
|---|---|---|
| `seq 1 2000` | 8.46 -> 2.74 s (0.38) | 5.86 -> 1.97 s (0.20) |
| 600 lines of 74 characters | 6.97 -> 3.62 s (0.20) | 3.95 -> 2.40 s (0.11) |
| the same, a colour every 10 characters | 8.56 -> 6.03 s (0.27) | 4.80 -> 3.63 s (0.15) |
| `ls -laR /` | 12.22 -> 9.58 s (5.93) | 6.98 -> 5.66 s (3.27) |

The console's own part (screen minus /dev/null) is 1.3 to 3.4 times smaller; most for many short lines, least for
colour changes (each SGR sequence goes through the parser byte by byte, unchanged here). The profiling build's
split after the change, the stock A1200: for `seq 1 2000` most of what is left is the blitter's fill of each new
screen (one per 256-byte piece of the write) and the scrollback's copy of each line; for the long lines the glyphs
(four reads from the glyph table and four long-word stores a plane and line for four cells). Not done here, for
later if wanted: bigger pieces than `tty_write`'s 256 bytes (fewer whole screens drawn for short lines; the
latency of D20 section 6 to be measured again), and a faster SGR path. Not yet on the A1200 or the PiStorm: there
the chip RAM writes the CPU no longer makes (a quarter of them for text, none for what scrolls past) should count
for more than in FS-UAE.

## 22. Console preferences: conprefs (2026-11-07, branch prefs)

The owner (2026-11-06, docs/decisions.md "Screen modes and preferences"): screen modes, fonts and the like for the
console too, with Test, Use, Save and Cancel. The desktop's half is docs/d38-desktop.md "Preferences".

**What the console can change while it runs (read from the code).** Its colours: `CON_COLOURS` (root) allocates the
new depth's screen, switches the Copper list and draws every row again (`gfx_set_colours`, built for D14 and used
only by init until now). Its Unicode font: `CON_UFONT` (root, section 7) swaps the font and draws the shown console
again. Topaz stays the look of Latin-1 (section 6: a font that replaces it is not promised; `CON_FONT`'s PSF fonts are
a program's 8-bit mode and end with it, so they are no setting). Its mode: `CON_MODE` since 2026-11-12 (below). So
`conprefs` offers the colours (auto, 2, 4, 8, 16), the Unicode font (auto, none, or any
`/usr/share/consolefonts/NAME.zcf`) and the mode (hires, hires-lace).

**The program: `conprefs`** (user/conprefs.c, 18 KB on the disk), a text screen in the console's own terms (escape
sequences, the tty in raw mode; Up and Down a setting, Left and Right its value, `t` `u` `s` `c` the buttons, Return
Use, Esc Cancel). `conprefs -q` prints the settings in effect in one line (for scripts and the tests).
- **The console is the system's** (one screen for the ten consoles, one font for all): applying is root's, as
  `/etc/console` is (decided here, as `CON_COLOURS` and `CON_UFONT` already are). Another user sees the settings and
  is told "Only root can change the console: sudo conprefs".
- **Test:** applied now, the screen stays. **Use:** applied, then the program ends; nothing saved, and the session's
  choice is written to `/var/run/console` (the same lines), which init removes at every boot: AmigaOS's `ENV:` beside
  its `ENVARC:`. It is there because the kernel does not say which font it has (section 7: "an op to ask which font
  is loaded: nothing needs it"); conprefs reads the font in effect from it, else from `/etc/console`. The colours it
  asks the kernel. **Save:** applied, then `/etc/console`'s `colours` and `font` lines replaced (the `login` line and
  the comments kept; a new file renamed over the old, user/cfgfile.c), `/var/run/console` removed. **Cancel:** the
  colours and the font as they were at the start, `/var/run/console` as it was. A refused change (`-EBUSY` while the
  desktop has the screen, `-ENOMEM` for a depth's screen, a font file that is not ZCF1) is said on the screen's last
  line, and Use and Save stop there.
- **auto colours** is what the kernel chooses at boot (section 13, D14): 16 with fast RAM (8 on OCS/ECS: the kernel
  shows the most it can), else 8. A 68020 machine with no fast RAM and less than 2 MB of chip RAM (the kernel's 2
  colours) is not one Zenix is made for; there auto asks for 8, which the kernel refuses with -ENOMEM if it cannot.

**At boot (the robust path).** init reads `/etc/console` as before (section 7, D14 section 11) and applies `colours`
at once and `font` after `mount -a`. What changed: a line it does not understand no longer gives a line each; the
first is said, once, at the end ("init: /etc/console: unknown setting bogus (and more lines not understood)"), and
every line it does understand still counts, so a broken file is one line and the defaults for what it broke. A
missing or unreadable font file is init's one line as before; the console then shows the box for what Topaz lacks.

**The console's mode (hires or interlaced): built 2026-11-12 (branch pubprep).** `CON_MODE` as designed in
docs/d38-desktop.md "Screen modes" ("The text console the same way"), read from the code:
- **The call:** `SYS_console` op 11 `CON_MODE`, no descriptor (every console's, as `CON_COLOURS`), a2 `CONM_HIRES` 0
  (32 rows PAL, 25 NTSC) or `CONM_LACE` 1 (64 and 50), `CONM_ASK` 0xFF asks; setting is root's (-EPERM). -> the mode
  shown, -EBUSY while a display server has the screen (D38), -ENOMEM when the screen's chip RAM or the consoles' grids
  cannot be had (the display stays as it was), -EINVAL another value (and on a kernel without it: conprefs' probe).
- **The switch (gfx.c `screen`):** one routine for the depth (`CON_COLOURS`) and the mode: the new ring's chip RAM and
  Copper lists first, then (a new number of rows) every used console's grids (`term_regrid(rows)`); nothing can be
  had: all given back, -ENOMEM. Then with interrupts masked the pointers, `trows` and the ring change and `COP1LC`
  names the new list; at once (interrupts only queue, nothing draws between) each console's text moves into its new
  grids (`term_regrid(0)`); one frame later the old memory goes and the shown console is drawn again.
- **Interlaced on the chips** (as displayd, d38 "Interlace on the chips"): two Copper lists, the long field's showing
  lines 0, 2, 4..., the short field's starting a line (`lstride`) further in; the modulos one line of all planes more,
  `BPLCON0`'s LACE bit; each list ends naming the long field's list, waits for line 312 (262 NTSC), only a long field
  has, and there names the short one's (`cop_point` sets the first WAIT of that pair to 255's end, or to 312's low
  bits when the ring's wrap already waited past 255). The ring of rows (section 20) is the same: 128 rows with fast RAM
  (64 shown, 64 spare), 96 without; its chip RAM is that of a hires screen of the same rows: 320 KB at 16 colours with
  fast RAM, 180 KB at 8 colours without. `CON_LINE` gives 512 lines (400 NTSC); `CON_GRAB`'s size stays the frame's
  (640 x 256 or 200: frozen), a display server sets its own mode.
- **The grids (term.c):** a console's three grids are one block of 3 x `trows` x 80 bytes (and its high plane
  `trows` x 80, as before on first use) allocated for the rows shown, not `[TROWS_MAX][COLS]` arrays: 7.5 KB a console
  hires, 15 KB interlaced. `TROWS_MAX` is 64 (kernel.h): the row table `rx`, the drawing notes' ring `lz_c` (64
  entries, a whole-row flag in each instead of a 32-bit mask) and gfx's `rowoff` are sized for it. At a switch the text
  keeps its place from the top; when the cursor's row would not fit (64 -> 32 with the cursor below row 32), the rows
  above it that do not fit go into the scrollback, so the cursor's row is the last one. The scroll region becomes the
  whole screen, a scrollback view ends, and each console's foreground gets `SIGWINCH` (`tty_winch_consoles`).
- **The settings:** `/etc/console`'s `mode hires|hires-lace|auto` (auto or no line: hires), applied by init after the
  colours; conprefs' third line "Screen mode", shown when `CON_MODE` answers, Use writing `mode` into
  `/var/run/console`, Save into `/etc/console`; `conprefs -q` adds `mode hires|hires-lace`. The demo floppy keeps
  hires (no `mode` line).
- **The Copper's own list changes (the review's findings, fixed):** an interlaced list names a list for the next
  field itself, so a switch away from interlace first makes the old lists name the new one (else the Copper would go
  on in freed chip RAM), and while a display server has the screen the kernel's lists name the server's list
  (`cop_next`; `reclaim` sets them back); the ring's wrap WAIT stays below line 312 (with `DIWV=0x38` the picture ends
  at 312, which a short field never reaches). The blitter is waited for before the grids are taken, so nothing draws
  (a timeout's kprintf) between the two steps of `term_regrid`; a console without its new grids is a panic.
- **Size (measured):** gfx.o +813 bytes, term.o +998, syscall +62, tty +44: kernel.bin +1.9 KB. The room came from
  fsd's printf (embedded in the kernel image) at -Os (user/libz.c `fmt_num` and `vfmt`, in every program): fsd.zx
  29,372 -> 27,472 bytes, so kernel.bin went 182,820 -> 182,896 with `CON_MODE`, and the mount tests' floppy has
  1,424 bytes free (800 kept); its RAM disk has at least 46 free blocks, 20 more than before (the smaller programs), room for
  a later 1-KB step down of `RAMDISK_MOUNT_K` if the kernel needs it. Speed: write_'s run of printable bytes steps
  three pointers through the grids (before: one pointer and offsets a grid apart); not measured on a real 68020.

**Tests (FS-UAE):** `tools/prefs-test.sh con` on the stock A1200 with 4 MB fast, typed on the serial line: 8 colours
and Test (in effect), Cancel (16 again); 8 colours and no font, Use (in effect, the block character drawn as the box in
the screenshot, `/var/run/console` written, `/etc/console` not); Cancel at once (nothing changes); 4 colours and the
font, Save (`/etc/console`'s lines replaced, its comments and login line kept); three broken lines added and a reboot:
init's one line, the saved settings in effect (the block solid: the font loaded at boot), `/var/run/console` gone.
Since 2026-11-12 also the mode: `termtest mode 1` (`CON_MODE`) under `termtest winch` in the foreground (`SIGWINCH`,
64 x 80), the kernel's log line, the grid's 64 rows with the text written before (`CON_READ`), a block at row 1 and
one at row 64 in the interlaced screenshot; conprefs' mode line back to hires with Use (`/var/run/console`); and
`mode hires-lace` in `/etc/console` applied by init at the reboot (the font's block checked on the interlaced screen).
