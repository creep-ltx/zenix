# D14: the colour console (design note, revision 1)

Status: **proposed, awaiting one review** (a feature, not deep core: one independent review, then building).
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

Each bitplane doubles the colours and adds a full copy of the screen to draw, scroll and keep in chip RAM.

| Planes | Colours | Chip RAM (double-height buffer) | Fits ANSI | Display DMA cost (hires) |
|---|---|---|---|---|
| 1 (today) | 2 | 40 KB | no colour | lightest |
| 2 | 4 | 80 KB | a reduced mapping | light |
| 3 | 8 | 120 KB | **the 8 standard colours** exactly | medium |
| 4 | 16 | 160 KB | **all 16** (normal and bright) | heaviest: on OCS/ECS hires 4 planes takes nearly every free display slot during the visible lines (*from the manual*); AGA's wider fetch (FMODE) makes it far lighter |

**Proposal: the depth is chosen at boot, default by machine.**
- **AGA (the A1200, A4000): 4 planes, 16 colours** (full ANSI with bright colours), with AGA's wider fetch mode so the
  display DMA stays light.
- **OCS/ECS (A500-class with an accelerator, A3000): 3 planes, 8 colours**, bright colours shown as their normal
  colour plus bold (as many terminals did).
- **A setting** (in `/etc/console`, read by `init` and passed to the kernel with a small console control, or a boot
  argument) chooses 1-4 planes. 1 keeps today's look and speed exactly, for anyone who wants every cycle.
Measured on real hardware before the default is final: the cost of 3 and 4 planes on the stock A1200 (the owner's DoM
weekend), CPU benchmark with the console idle and while text scrolls. *No speed claim from the emulator.*

## 4. How it works

- **Cells:** the attribute byte stays for bold/underline/inverse; a second byte per cell holds the colours (foreground
  4 bits, background 4 bits). 2.5 KB more per console grid.
- **Drawing:** a glyph is drawn into each plane: the bits where the foreground colour has a 1 get the glyph, where the
  background has a 1 get its inverse. On 4 planes that is 4 writes per glyph line instead of 1; the CPU loop stays
  simple (byte writes, no blitter for single cells). Clearing and the copy-back on scroll use the blitter per plane, as
  today for one.
- **Scrolling:** unchanged in kind: the Copper list gets one pointer pair per plane, all bumped at vertical blank.
- **Palette:** the 16 standard ANSI colours as Amiga colour registers (12-bit on OCS/ECS, 24-bit on AGA), chosen to be
  readable in Topaz on a real monitor (grey on black stays the default text, as today). A later setting may change them.
- **Escape codes** (term.c's `m` handling): `30-37`/`40-47` set the colours, `90-97`/`100-107` the bright ones, `39`/`49`
  restore the defaults, `0` resets everything, `1` bold stays bold (bold never changes the colour: one rule,
  documented). 256-colour and true-colour codes (`38;5;n`, `38;2;r;g;b`) are mapped to the nearest of the 16, so
  programs that use them still look sensible.
- **Scrollback:** keeps the colour and attribute bytes too (one more byte per character: 200 lines x 80 x 2 = 32 KB per
  console instead of 16 KB, allocated when first used, as today). Fixes today's loss of bold/inverse in the scrollback.
- **`TERM`:** set by `login` to a name programs understand for 16 colours (`ansi`-like); documented in the man page.

## 5. Memory, in total

On a 2 MB chip-only A1200 at 4 planes: the screen buffer grows from 40 KB to 160 KB of chip RAM, and each used console's
scrollback by 16 KB. Against D13's savings (about 83 KB per extra session) this is affordable, but not free; the setting
lets a small machine stay at 1 or 3 planes.

## 6. Programs that use colour (part of D14)

- `ls --color` (default when the output is a console, like GNU's `auto`): directories, programs, links; the scheme from
  `LS_COLORS`-style defaults kept small.
- `grep --color` (auto on a console): the match highlighted.
- The shell's prompt: `PS1` already supports escapes; the default prompt gets a colour.
- `less` and `edit`: colours passed through / used for the status line.
- `termtest`: a colour page (the 16 colours on both backgrounds, bright and bold), so a screenshot test can check them.

## 7. Tests

- term.c on the host (a small model, like the other host tests): each escape code sets the right cell colours; 256 and
  true colour map to the expected of the 16; reset and bold rules.
- Screenshots in FS-UAE (`tools/shot.py`, the cage rig, as kbd-test does): `termtest`'s colour page at 1, 3 and 4
  planes, colours compared against the palette; scrolling with colours leaves no garbage; Amiga+Up/Down scrollback
  shows colours.
- cmds-test style: `ls --color`/`grep --color` output compared with GNU's (`LC_ALL=C`, same colour defaults) where the
  codes are the same.
- The full suite at the default depth.

## 8. Order of work

1. The planes as a setting in gfx.c (1-4), the Copper list for N planes, the palette; 1 plane must look and test exactly
   as today.
2. Colour bytes in term.c, the escape codes, the scrollback with attributes; the host test.
3. `termtest`'s colour page and the screenshot tests.
4. `ls`, `grep`, the prompt, `less`, `edit`; `TERM`.
5. Docs (architecture.md section 11, features.md, limits.md, man pages).

## 9. Open questions for the review

1. Default depth on the stock A1200: 4 planes with AGA fetch, or 3 to stay light until measured?
2. Bold: keep bold as bold, or show bold as bright (the old terminal habit)? Proposed: bold stays bold; bright is
   bright.
3. Should the colour bytes and the planes count go into the D11-style saved state for a future display server (D4), or
   stay console-only? Proposed: console-only; the desktop's terminal window will have its own.

Last checked against the code: 2026-10-25.
