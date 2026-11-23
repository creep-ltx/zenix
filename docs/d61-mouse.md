# D61. The desktop's mouse: precise slow moves, the screen's own pixels, and `mouseprefs`

The partner, 2026-11-23 (decisions.md "Screen settings and the mouse"): "the mouse handling in the desktop is sub-par
and has always been ... The mouse is difficult to hit the buttons with, it's fast and then when trying to be precise
it's choppy ... a real and good settings program would be nice... Like set left/right button, double click speed,
pointer speed, acceleration, pointer looks, and so on." Approved the same day: a rework of the desktop's mouse
handling, with a design note first. This is that note, revision 1, and what was built (branch mouse).

## 1. The path a move takes (read from the source, 2026-11-23)

1. **The counters.** A mouse in port 1 counts its quadrature pulses in JOY0DAT's two 8-bit counters (AHRM: vertical
   in the high byte, horizontal in the low). `kernel/input.c` reads them from the 100 Hz tick and, while a console is
   in graphics mode, also from the vertical blank (`gcon_vblank`); each read adds the signed 8-bit difference to `dx`,
   `dy`. A counter wraps after 127 counts each way, so no move is lost unless the mouse moves more than 127 counts
   between two reads (10 ms): far beyond any hand. **Nothing is dropped here.**
2. **The vertical blank** (`gcon_vblank`, 50 a second on PAL, 60 on NTSC) takes the counts since the last frame,
   moves the pointer and writes sprite 0's two control words before sprite DMA reads them. So on the chipset the
   pointer is on the glass in the frame after the move: **at most one frame of lag, whatever displayd is doing.**
3. **The arithmetic** (before D61): per axis, the counts beyond a threshold count a multiplier times (`CON_MOUSE`,
   zdesk's `/etc/zdesk/mouse`: `low`, the default, doubles the counts beyond 4 a frame), then **two hires pixels a
   count across and one line down**, in whole pixels. Interlaced, displayd doubles the line (two lace lines a count).
4. **kwork** puts a `GEV_POINTER` record in the grab's ring (merged into an unread one), and displayd reads it to
   find the window under the pointer, send motion and buttons, run drags and menus.
5. **On a graphics card** (D43) the sprite is not on the glass: displayd draws the pointer into the card with the CPU,
   at the kernel's place scaled from 640 x 256 to the card's size, and the card is updated by `fb_show` at the end of
   displayd's main loop pass, after every request waiting in its port has been served.

## 2. What was wrong, measured

In FS-UAE (the stock A1200 with 4 MB fast, `gcontest`, the mouse moved by `CON_INPUT` a record a frame,
2026-11-23, before any change):

| counts in a frame | 1 | 1 | 2 | 3 | 4 | 5 | 6 | 8 | 12 | 20 |
|---|---|---|---|---|---|---|---|---|---|---|
| pixels across (acceleration `low`) | 2 | 2 | 4 | 6 | 8 | 12 | 16 | 24 | 40 | 72 |

- **Choppy when slow:** the smallest step is **two hires pixels across** (one lores pixel) and one line down; the
  pointer can never stop on an odd column by moving slowly, and interlaced it moves by **two lines**. On the card at
  1280 x 960 it is worse: the kernel's 640 x 256 is scaled, so a count is **4 pixels across and 3.75 lines down**
  (`fb_scale`): the pointer jumps over small gadgets.
- **Fast:** a count is two hires pixels at the slowest already, and the acceleration doubles every count past 4 a
  frame on top (a hand moving 6 counts a frame goes 16 pixels): from careful to fast there is no slow, precise range.
- **Nothing is lost** (section 1.1), but the fraction a slower speed would need does not exist: the arithmetic is in
  whole pixels, so a speed under 1 pixel a count could not be offered at all.
- **The lag on a card:** the pointer waits for the end of displayd's pass; a pass that serves a big request (a
  terminal's output) delays it. Measured with the counters of section 6 (`DPY_STAT`): section 8.
- **Buttons:** the left/right buttons cannot be swapped; the double-click time is 0.5 s written into two programs
  (zdesk's icons and `files`), each with its own copy; there is no setting for it. The middle button (POTGOR) is read
  and passed on as button 3.

## 3. What changes, in short

- The pointer moves in **the screen's own pixels** (hires columns and lines, interlaced lines, a card's pixels), so
  a slow move goes by single pixels everywhere; the kernel keeps the pointer's place in **256ths of a pixel**, so a
  speed below one pixel a count works and no count is ever lost to rounding.
- **Speed** is "pixels of the screen a count at slow speed" (100% one pixel across), and **acceleration** is
  AmigaOS's Input preferences' kind (a threshold, then a multiplier), set apart: off, low, medium, high, or a
  threshold and multiplier of one's own. The default is 100% and low (the old default's acceleration on half its
  old speed: two hires pixels a count was the "fast" the partner felt).
- **Buttons swapped**, **the double-click time** (one value for every program: displayd marks a double click in the
  button's event), **the pointer's look** (pointer files, colours, a 16 x 16 editor).
- **The pointer on a card is put on the glass at once** when it moves, not after the pass.
- **`mouseprefs`**, a desktop window with Test, Use, Save and Cancel, beside `deskprefs`.

## 4. The kernel: `CON_MOUSE` grows (about 440 bytes)

d43 section 13 question 1 proposed growing `struct ucongrab` with the pointer's bounds (about 30 bytes). That does not
reach far enough: the bounds change when the desktop's mode changes (hires to interlace, `DPY_MODE`, live), and the
speed and acceleration must be in the same units. So `CON_MOUSE`, which the server already calls on its grab, grows
instead: its a3, 0 until now (every caller passed 0), may point at a **`struct umouse`** (kernel/kernel.h, 20 bytes):

| field | meaning |
|---|---|
| `size` | `UMOUSE_FIRST` 20 or more (the struct grows at its end) |
| `gainx`, `gainy` | the pointer's units a count across and down, in 256ths (1 .. 0x1000) |
| `mult`, `thresh` | in a frame, the counts beyond `thresh` (0..127) along an axis count `mult`/256 times (0x100..0x800) |
| `shift` | the sprite's place: `x >> (shift & 3)` is its hires pixel, `y >> (shift >> 2)` its frame line |
| `w`, `h` | the pointer stays in 0..w-1, 0..h-1 (1..4096); the ring's records are in these units |
| `x`, `y` | where the pointer goes now (clamped; the pixel's middle, so the next pixel is as near both ways); negative keeps it |
| `hotx`, `hoty` | the sprite's hot spot, hires pixels and frame lines (a new pointer image may have its own) |

The arithmetic is one function, `kernel/pointer.h` `ptr_step`, used by the vertical blank and by the host test: the
place in 256ths; the counts cut to 255 a frame (more is an injected jump, never a hand); the threshold and multiplier
in 256ths; then the gain; clamped to the bounds. With the largest values (255 counts, a multiplier of 8, a gain of 16)
the product is 2,139,095,040, inside 32 bits unsigned, and the 68020's `mulu.l` (32 x 32 -> 32) does it, no 64-bit
multiply (the 68060 has none). It costs two multiplies a frame while the mouse moves, in the vertical blank as before.

**Old servers keep working:** a2's form (a3 0) sets the threshold and the multiplier as before (multiplier x 256),
and a grab starts as D38's: hires pixels and frame lines, `gainx` 0x200, `gainy` 0x100, no acceleration. The old
moves come out exactly as before: the host test replays the table of section 2 and `gcon-test` still passes.

The struct is read once (copied, then checked), a2 must then be 0 (kept for later), and the setting is applied with
interrupts masked (the vertical blank sees all of the old or all of the new); the sprite moves to the new place at
once and kwork tells the ring. The sprite's control words are clamped to their 9 bits (HSTART, VSTART at most 510), so
a card's units or a hot spot can never wrap the sprite. `gcon` grew from 4,456 to 4,956 bytes (the struct's checks
and copy, the step out of line); `kernel.bin` 227,824 -> 228,328, the mount tests' floppy 1,552 -> 1,048 bytes left
(the 800 kept).

**The reviews** (two independent, 2026-11-23, on the built code): no bug that crashes; taken: the sprite's words
clamped (a hot spot or a card's width could wrap HSTART, a tall screen VSTART past VSTOP); the struct read once into
a copy; a2 required 0 with a3; "negative keeps the place"; the size asserted in kernel.h as in usys.h; a new place in
the pixel's middle (else the first slow count one way moves at once and the other way only at the second); the host
test's there-and-back checks its midpoint and the sweep back. Also from them, done in displayd (section 5): the ring
read empty before the units change (records still unread are in the old units), and the threshold scaled to 50 a
second on NTSC (the vertical blank is 60 there).

## 5. displayd

- **`DPY_MOUSE`** (request 21, claimed): a[0] a `struct dpymouse` (display/proto.h) in the client's memory, a[1] 0
  sets, 1 gets. Any client of the session's user may call it (as `DPY_MODE`): the speed (percent), the threshold, the
  multiplier (percent), the flags (`DPYMF_SWAP`: left and right swapped), the double-click time (ticks), the pointer's
  image (16 rows of sprite 0's two words), its three colours and its hot spot; a get also gives the pointer's place.
- **The units:** displayd turns the setting into a `struct umouse` for the screen it shows: hires 640 x 256 (200
  NTSC), interlaced 640 x 512 (400) with the sprite's line y / 2, a card's width and height with the sprite scaled by
  powers of two into the chipset's frame (unseen there). `gainx` is the speed (100% = 0x100); `gainy` is the same
  distance on the glass: half of it on a hires screen (a hires pixel is about half as wide as a line is tall), the
  same interlaced, and on a card the card's own ratio (960 lines over 1280 columns, against 256 over 640: 0.9375 of
  it). On a mode change (`DPY_MODE`) the setting is sent again with the pointer's place carried over.
- **Buttons swapped** are swapped where the kernel's qualifiers come in (pointer and key records alike), so every
  window, the window manager and drags see the swapped ones; the kernel and the boot's left-button check (cache.c,
  mmu.c: "hold the left button") keep the real button.
- **Double clicks:** a press of the same button within the double-click time and within 4 pixels of the last press
  gets `arg` 2 in its `EV_BUTTON` (0 before: `arg` was unused there); a third press counts as a first. zdesk's icons
  and `files` use it instead of their own 0.5 s.
- **The pointer on a card** is put on the glass as soon as it moved (`fb_show` right after the kernel's records), and
  between two requests when a tick passed, so a long pass no longer holds it.
- `DPY_WM_MOUSE` (old zdesk) still sets the acceleration, its multiplier taken as whole times.

## 6. The settings file and the programs

`~/.config/zdesk/mouse` (a user's) or `/etc/zdesk/mouse` (the system's, root's Save and the users without their own),
`setting value` lines with `#` comments, read by one parser, `display/lib/mousefile.c`, linked into zdesk and
mouseprefs:

```
speed 100            # percent: 100 moves the pointer one pixel of the screen a count at slow speed (10..800)
acceleration low     # none, low, medium or high: AmigaOS's kind; or a threshold and multiplier of one's own:
threshold 4          # counts in a frame (a fiftieth of a second) before the multiplier (0..127)
multiplier 200       # percent (100..800)
buttons normal       # or swapped (for the left hand)
doubleclick 50       # hundredths of a second (10..300)
pointer arrow        # NAME.pointer in ~/.config/zdesk/pointers/ or /etc/zdesk/pointers/
```

`acceleration none|low|high` files of before keep their meaning (none 1x, low beyond 4 doubled, high beyond 2
tripled; medium is beyond 3 x 2.5). A pointer file is a 16 x 16 picture in text, as AmigaOS's pointer is a 16-pixel
sprite of 3 colours:

```
hot 0 0              # the hot spot, in the picture's pixels
colours 000 fff 888  # colours 1-3, 12-bit RGB
o...............     # 16 rows of 16: . clear, o colour 1, w colour 2, x colour 3
```

zdesk applies the file when a session starts (after asking displayd what it had) and puts back what it found at Log
out, as it does for the screen's mode and font (D38 "Preferences"). mouseprefs applies a choice at once with
`DPY_MOUSE`.

## 7. `mouseprefs`

A window in the style of `deskprefs` (display/prefs/, the Zenix menu's Preferences submenu): rows of choices changed
with a click (Shift-click backwards) or Left/Right, Up/Down between rows; Test (applied now, the window stays), Use
(applied for the session), Save (applied and written: the user's `~/.config/zdesk/mouse`, root's `/etc/zdesk/mouse`),
Cancel (everything as it was when the window opened), as AmigaOS's preference editors. The rows: speed, acceleration
(off, low, medium, high, own), threshold, multiplier, buttons, double-click time, pointer. A double-click test area
answers a double click at the chosen time; the pointer's editor shows its 16 x 16 pixels large: a click paints the
chosen colour, the three colours change with their gadgets, and Save writes the picture as `custom.pointer` beside
the settings. The window's parts shared with deskprefs (the rows, the four buttons, the message line, the keys)
moved to `display/lib/prefswin.c`, so the screen-mode settings program to come uses the same.

No wheel: the kernel reads no wheel (a NewMouse wheel on AmigaOS comes from a driver turning the mouse's extra lines
into key codes; Zenix has none), so there is no scroll-wheel setting yet.

## 8. Measurements after

(filled in as built: section 10)

## 9. Tests

- `tools/mouse-host-test.sh`: kernel/pointer.h on the PC (D38's old moves exactly; single pixels at a gain of 1; a
  fractional gain loses no count; there and back lands on the same 256th; the threshold and multiplier; the edges;
  the largest values).
- `tools/mouse-test.sh k`: the kernel in FS-UAE through `gcontest m SET`: a wrong setting refused, a slow sweep in
  single pixels with the half lines kept, the acceleration's moves against an independent computation, interlaced
  and card units with the sprite where the ring says, the edges.
- `tools/mouse-test.sh d`, `p`: displayd and mouseprefs (section 10).
- The desktop's tests (dpy, zdesk, drag, files, lace, prefs, rtg, gcon) pass with their settings written out
  (`speed 200`, `acceleration none`: two hires pixels a count, as their clicks count).

## 10. As built

(section filled in as the branch is built)
