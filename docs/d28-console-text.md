# D28: UTF-8 in the console, and the console's fonts (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 18); nothing built.** Roadmap block 3 ("A terminal
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
  - every entry is 0, 32-255, or below 256 + the glyph count.
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
