# D14: the colour console (design note, revision 2)

Status: **built (2026-10-26, branch `d14`; section 10 says where the build differs)**; revision 2 (2026-10-25) was
ready to build after its independent review (a feature: one review). Revision 2
takes in every finding: FMODE 3 is a condition, not an option; AGA detection and registers; a 12-bit palette; the
scrollback number corrected; the default by memory too; the Copper list; background-colour erase; region scrolls by
blitter; a hardened escape parser; the test harness; `TERM=xterm-16color` with cursor-key mode; no runtime setting yet.
Approved by the owner on 2026-10-25. The short version is in [DESIGN.md](DESIGN.md) D14. Facts marked *read* come from
the source (kernel/gfx.c, kernel/term.c); hardware facts from the Amiga Hardware Reference Manual (*from the manual*).

## 1. What it is for

Colours in the text console, the way every Unix terminal has them: `ls` colouring directories and programs, `grep`
marking matches, a coloured prompt, `less` and the editor showing highlights, and programs that use the standard ANSI
colour codes (`ESC [ 30-37 m`, `40-47`, `90-97`, `100-107`, `39`, `49`) looking right instead of plain. Also the
foundation for running modern terminal programs on the console.

## 2. Today (read)

- One hires bitplane, 640 pixels wide: two colours, grey on black. One plane on purpose: the display takes chip-bus
  time from the CPU, which shares chip RAM on a stock A1200; one plane is the lightest (gfx.c's opening comment).
- Each cell has a character and an attribute byte (bold, underline, inverse: `at[][]` in term.c). Colour codes are
  ignored. The scrollback keeps characters only: bold/underline/inverse are lost when a line scrolls up.
- Scrolling is a pointer bump in the Copper list; the blitter copies the visible rows back only when the window
  reaches the end of its double-height buffer. Glyphs are drawn by the CPU, one byte per line per cell.

## 3. The choice: how many bitplanes

| Planes | Colours | Chip RAM (double-height buffer) | Display DMA |
|---|---|---|---|
| 1 (today) | 2 | 40 KB | 40 slots per line (FMODE 0) |
| 4 on AGA **with FMODE 3** (64-bit fetch) | **16, all of ANSI** | 160 KB | **40 slots per line: the same as today** |
| 4 on AGA at FMODE 0 | 16 | 160 KB | **all 160 slots of the fetch window: the CPU is locked out of chip RAM during the visible lines** (AHRM 3rd ed. p. 212, "locks out the 68000"; AGA at FMODE 0 behaves the same). **Never used.** |
| 3 on OCS/ECS | 8 | 120 KB | half of the CPU's slots during the visible lines (by the AHRM's slot figures; the knowledge base marks the parity reading uncertain) |

**The default, by chipset and memory:**
- **AGA** (detected, section 4) with fast RAM or at least 2 MB chip: **4 planes at FMODE 3**, the 16 ANSI colours at
  today's DMA cost. If FMODE 3 cannot be set up and verified: **1 plane**, never 4 at FMODE 0.
- **OCS/ECS with fast RAM:** 3 planes, 8 colours (bright shown as the colour plus bold). 2 planes is a possible later
  choice for chip-only OCS/ECS machines.
- **Chip-only with less than 2 MB:** 1 plane (the 1 MB machine keeps room for its one login; limits.md).
- **No runtime setting in the first build:** the machine decides, and a make variable selects 1/3/4 planes for the
  screenshot builds (as `DIWV` does). A setting (root-only, once by `init` before the first login, while chip RAM is
  unfragmented) would need a new system call (82) and the owner's word: proposed later if wanted, with the safe switch
  sequence the review gave (allocate, wait for the blitter, a new Copper list with BPLCON0/FMODE inside, switch with
  VERTB masked, wait a vblank with a timeout, free the old buffer, redraw).
Measured on the real A1200 before the default is final (the owner's DoM weekend): CPU benchmark with the console idle
and while text scrolls, at 1 and 4 planes. *No speed claim from the emulator.*

## 4. How it works

- **AGA detection and registers:** `gfx_init` detects AGA by DENISEID (`$DFF07C`, Lisa reads `$F8`; read several
  times: OCS has no such register). The display writes FMODE, BPLCON3 = `$0C00` and **BPLCON4 = `$0011`** itself, in the
  Copper list, every frame: today nobody resets BPLCON4, whose BPLAM bits XOR every colour index and survive a warm
  reset (knowledge base aga-specifics).
- **FMODE 3 requirements:** plane pointers and modulos multiples of 8 bytes (`mem_alloc` gives 16-byte-aligned blocks;
  80-byte rows); the 640-pixel line is 10 fetch units. The DDFSTRT/DDFSTOP/BPLCON1 values for hires at FMODE 3 are
  **not established anywhere yet**: found by a screenshot test (distinct first and last columns, all 80 visible,
  nothing repeated), then confirmed on the A1200 (Emu68 drives the real chipset, so the PiStorm shows the real display).
  The values go into the knowledge base.
- **Interleaved planes:** the planes of a line lie one after another (modulo (N-1)*80), so clear and copy-back are one
  blit each and one base pointer serves all planes (31 rows x 8 lines x 4 planes = 992 blitter rows, within BLTSIZE's
  1024; a whole-buffer clear is two blits).
- **The palette:** 12-bit on every chipset (on AGA with LOCT=0 the 12-bit value fills the low nibbles), the 16 ANSI
  colours chosen readable in Topaz on a real monitor; grey on black stays the default text. 24-bit only if a later
  palette setting needs it.
- **The Copper list:** resized for the AGA registers, BPLCON0, 16 colours and N pointer pairs (about 104+ bytes, the
  indices recomputed). A Copper WAIT at line `$20` (before the display on the `$2C` and the `$38` test builds, PAL and
  NTSC) comes before the pointer moves; `gfx_vblank` writes the pointers only while the beam is well before that line,
  else at the next vblank, so all planes always move together (this also closes the same race that exists today for the
  high/low word pair). `gfx_panic` sets the new layout's background to dark red and the text to white.
- **Cells:** the attribute byte stays (bold, underline, inverse); a second byte holds the colours (foreground and
  background, 4 bits each). 2.5 KB more per console grid. If foreground equals background, the inverse cursor falls
  back to the default inverse so it stays visible.
- **Drawing a glyph:** one byte per plane per glyph line, branch-free: `(glyph & m1[p]) ^ m2[p]` (m1 = 0xFF where the
  foreground and background bits differ, m2 = 0xFF where the background bit is set; inverse swaps them first). The
  loop is specialised per depth with the masks in registers and the glyph read as two longwords (gcc otherwise keeps the
  masks on the stack, which is chip RAM on a stock A1200). Estimate, not measured: about 40 chip accesses per cell at 4
  planes against 16 at 1, roughly +10-15% on plain text output (with FMODE 3).
- **Region scrolls by blitter:** `CSI S/T/L/M` and scrolling inside a region (what `less` and `edit` do) become one
  blitter copy of the region (descending for downward) plus drawing only the new row, instead of the CPU redrawing every
  row of the region. Faster at 1 plane too.
- **Erase with the background colour** (as Linux and xterm terminals do): erased cells and the new line of a scroll
  take (default foreground, current background): a blitter fill writes each plane's constant (0x00 or 0xFF), the CPU
  does single cells. `redraw_all`'s and `show_back`'s "skip blank cells" tests count non-default colours too.
- **Scrolling:** unchanged in kind, the Copper pointer bump at vertical blank. The copy-back (once per 32 scrolled
  lines on PAL, 39 on NTSC) costs about 22 ms at 4 planes against 5.6 ms (estimate), about 0.7 ms per line amortised.
- **Scrollback:** each line keeps its characters and **one colour byte with inverse folded in** (foreground and
  background swapped when the line is pushed): 200 lines x 80 x 2 = **32 KB per console** (16 KB today), allocated when
  the console is first used. Bold and underline are not kept in the scrollback (no worse than today). Keeping every
  attribute exactly would be 48 KB per console; not worth it on a 2 MB machine.
- **The escape parser, hardened to ECMA-48:** parameter bytes 0x30-0x3F (up to 16 parameters, values clamped, digits
  past the limit ignored), intermediate bytes 0x20-0x2F, a final byte 0x40-0x7E. Unknown sequences are swallowed whole
  (today they abort and the rest is printed as text); SGR applies only without a private marker (`>`, `=`, `<`, `?`);
  `:` sub-parameters (`38:5:196`) accepted; OSC (`ESC ] ... BEL` or `ESC \`, window titles) swallowed; `ESC ( x`
  swallowed (line drawing later: Topaz has no box glyphs). SGR: `30-37`/`40-47`, `90-97`/`100-107`, `39`/`49`, `0` and a
  bare `ESC[m` reset everything (so does `ESC c`), `1` bold stays bold and never changes the colour, `38;5;n`/`48;5;n`
  and `38;2;r;g;b`/`48;2;...` map to the nearest of the 16; `2 3 5 8 9` are ignored (listed in the man page). `ESC 7`/`8`
  also save and restore the attributes and colours, as xterm does.
- **`TERM=xterm-16color`,** set where `TERM` is set today (the shell, sh.c), one fact in one place. The keyboard already
  speaks xterm (F1 = `ESC O P`, modified keys `ESC[1;5A`); added: **cursor-key mode (DECCKM, `ESC[?1h`/`ESC[?1l`)**, a
  per-console flag `kbd.c` reads so arrows are sent as `ESC O A` when a program asks. This fixes a bug that exists today:
  full-screen programs on another computer (over `cu` or later the network) switch it on and then misread the arrows.
  Over a serial line the remote side's getty picks `TERM`, not Zenix (said in the docs). `vt100` today becomes
  `xterm-16color`; `linux` was the alternative (arrows match, but 8 colours and different F-keys).

## 5. Memory, in total

On a 2 MB chip-only A1200 at 4 planes: the screen buffer grows from 40 KB to 160 KB of chip RAM, each used console's
scrollback from 16 KB to 32 KB, its grid by 2.5 KB. Against D13's saving (about 83 KB per extra session) this is
affordable; machines with less than 2 MB of chip RAM and no fast RAM stay at 1 plane.

## 6. Programs that use colour (part of D14)

- **The `auto` rule** (one libz helper used by every program): colour when the output is a terminal, `TERM` is set and
  not `dumb`, and `NO_COLOR` is unset (no-color.org).
- **`ls --color=auto|always|never`** and **`grep --color=...`**: GNU's own default is `never` and distributions turn
  `auto` on with an alias; **Zenix's default is `auto`, a deliberate difference** (documented). The codes are GNU's exact
  sequences so tests can compare bytes: `ls`: `ESC[0m` before the first coloured name, then `ESC[01;34m`name`ESC[0m`;
  `grep`: `ESC[01;31mESC[K`match`ESC[mESC[K`, with GNU's defaults for file names (35), line numbers (32) and separators
  (36).
- **The shell's prompt:** the default prompt gets a colour (`PS1` already supports escapes).
- **`less`** passes SGR codes through only (like GNU less's `-R`) and counts them as zero width when it wraps lines;
  **`edit`** uses colour for its status line.
- **`termtest`:** a colour page (the 16 colours on both backgrounds, bold, inverse) for the screenshot tests.

## 7. Tests

- **The harness first, in the same commit as the coloured prompt:** `serial-tap.py` and the scripts' shared `clean()`
  strip `ESC[...m` (today they strip only `ESC[K`); one colour test checks the raw bytes; then `make check`.
- **term.c on the host** (a model, like the other host tests): each escape sets the right cell colours; 256 and true
  colour map as expected; reset and bold rules; long and unknown sequences, OSC, private markers and `:` sub-parameters
  are swallowed without printing anything; DECCKM switches the arrow keys.
- **Screenshots in FS-UAE** (`tools/shot.py`, the cage rig): the FMODE 3 test (all 80 columns, first and last distinct,
  nothing repeated); `termtest`'s colour page at 1, 3 and 4 planes, colours compared with the palette; scrolling with
  colours, region scrolls and background-colour erase leave no garbage; Amiga+Up/Down scrollback shows colours.
- **`ls`/`grep`:** both GNU and Zenix with `--color=always`, `LC_ALL=C`, `LS_COLORS`/`GREP_COLORS` unset, compared byte
  for byte; `auto` through a pipe gives no codes.
- The full suite at the default depth.

## 8. Order of work

0. Found while reviewing (fixed first): `blit_wait` gets a timeout (CLAUDE.md: every wait on hardware has one), and the
   escape parser's hardening (it prints garbage for sequences it does not know today).
1. gfx.c: AGA detection, the registers in the Copper list, interleaved planes, 1/3/4 planes by the default rule, the
   palette, the WAIT before the pointers, `gfx_panic`; FMODE 3's display values found by the screenshot test; 1 plane
   must look and test exactly as today.
2. term.c: the colour byte, SGR, background-colour erase, region scrolls by blitter, the scrollback with colours, DECCKM
   (with kbd.c), `ESC 7/8`; the host test.
3. The harness change, `termtest`'s colour page, the screenshot tests.
4. The `auto` helper, `ls`, `grep`, the prompt, `less`, `edit`; `TERM=xterm-16color`.
5. Docs (architecture.md section 11, features.md line "Colours", limits.md, man pages). The FMODE 3 values and the
   BPLCON4 trap go into the knowledge base.

## 9. The review's answers to the open questions

1. Default on the stock A1200: 4 planes, only with FMODE 3 verified (same DMA as today); otherwise 1. Never 4 at FMODE 0.
2. Bold: stays bold (pixel smear); bright is bright, as VTE and xterm do today.
3. The display server (D4): console-only; the desktop's terminal window will have its own.
The "no desktop features in the text console" rule is kept: 16 colours, a palette and a depth are what the Linux
console has; no bar, themes or menus.

## 10. As built (2026-10-26, branch `d14`)

Steps 0-5 are built as section 8 orders them (one commit each). Where the build differs from the sections above, and
why:

- **FMODE 3's values:** `DDFSTRT` `$38`, `DDFSTOP` `$C0`, `BPLCON1` 0, modulo (N-1) x 80 with no adjustment. Found by
  the screenshot test (`termtest columns`, `tools/colour-test.sh`): 0..79 decoded in order and the 4-plane picture
  pixel-identical to the 1-plane `$3C`/`$D4` one; `$38`/`$D0`, `$28`/`$A8`, `$30`/`$B0`, `$38`/`$B8`, `$40`/`$C0` and
  `$38`/`$B0` all gave repeated 64-pixel blocks. **Seen in FS-UAE only;** the A1200 (the PiStorm drives the real
  chipset) must confirm them before the 4-plane default is trusted there (knowledge base
  `amiga/zenix-colour-console.md`).
- **The Copper list writes more every frame** than section 4 lists: also `BPLCON1`, `BPLCON2`, both modulos and the
  fetch window, so nothing a previous program left in those registers matters either.
- **AGA detection:** the low byte of `DENISEID` must be exactly `$F8` in all of 16 reads (not only its low nibble):
  a Lisa revision that reads otherwise gets 3 planes at the old fetch, which is safe.
- **`make GFXDEPTH=4` on OCS/ECS gives 3; `GFXDEPTH=3` on AGA uses the 16-bit fetch** (as OCS/ECS would). If the chip
  RAM for the screen is not there, the next smaller depth is tried (4, 3, then 1).
- **3 planes: bright black (8) is shown as grey (7)**, not as black plus bold, which would be invisible on the black
  background; the other bright colours are the colour plus bold as section 3 says.
- **Drawing:** gcc 15 at `-O2` turned the byte replication of "the glyph read as two longwords" into a 32-bit multiply
  and kept masks on the stack. So with more than one plane a second glyph table holds each line already in all four
  bytes of a long (7 KB, built at boot), and one small function per depth takes the masks as arguments: per glyph line
  one long read, an AND, an EOR and a byte store per plane.
- **Room on the RAM floppy:** step 1 and 2 cost 4.3 KB of kernel at `-O2`; `gfx.c` and `term.c` are now built `-Os`
  with the drawing and the per-character path `-O2` (a function attribute). The kernel is 176440 of 179200 bytes after
  D14 (2760 left; 4584 before it).
- **The scrollback's 32 KB** are allocated when the console first scrolls, as the 16 KB were (not when it is first
  used).
- **`NO_COLOR`** counts when it is set to something (no-color.org's own rule: an empty value is as unset).
- **The `auto` helper** is `color_auto(fd)` in libz with `color_rule(fd, term, no_color)` under it (the shell passes its
  own variables) and `color_arg()` for `--color[=WHEN]` (also `--colour`, and GNU's yes/no/force/none/tty/if-tty);
  `ls --color` alone means always, `grep --color` alone auto, as in GNU.
- **`grep`, found while comparing bytes with GNU:** it printed no `--` between groups of context lines (GNU does, also
  between files and with `-A0`); added. `-o` looked for the next match in the rest of the line as if it were a new line,
  so `^` matched again; it now searches from the offset.
- **`less`** ends each row with a colour reset and starts a long line's continuation rows with the line's codes. Into a
  pipe or file it now writes every byte unchanged, as its page always said ("acts like cat") and GNU less does; before,
  it wrapped long lines and showed control characters as `^X` there too (found by the byte comparison with GNU).
- **`edit`'s status line** is written as the inverse of blue on white, so it is white on blue with colours and still
  inverse on a 1-plane display, which shows no colours.
- **`gfx_panic`** makes every colour but the background white (text on a coloured background then sits in a white
  block).
- **The harness change** went into the commit with the coloured prompt (section 7; section 8 lists it under step 3):
  `serial-tap.py`'s `wait`, `run-test.sh`'s output and every script's cleaning strip `ESC[...m`; `colour-test.sh`
  checks the raw bytes. A script line `shot NAME` (serial-tap.py with shot.py) takes a screenshot when the script gets
  there.
- **Tests:** `tools/term-test.sh` (host), `tools/colour-test.sh` (screenshots at 1, 3 and 4 planes), the colour lines
  of `rootfs/tests/cmds.sh` (byte for byte against GNU), the matrix (the depth on every machine); a 1-plane D14 build's
  `termtest` page is pixel-identical to the one before D14. The full suite passed at the default depth (`make check`,
  2026-10-26, 28 min 15 s).
- Not built here, as decided: the runtime setting (system call 82), 24-bit palettes, line drawing for `ESC ( 0`, the A1200
  measurements (the owner's DoM weekend).

Last checked against the code: 2026-10-26.
