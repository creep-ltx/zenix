# D55: screens of their own (`DPY_SCREEN`), and the Zenix intro

Approved by the partner 2026-11-13 (docs/decisions.md, "Block 7's choices", item 3: the fun, "the Zenix intro after
them (it waits for D50's full-screen call)"); the main session read "D50's full-screen call" as the protocol request
D50 section 4 sketched and asked for it to be built with the intro, `show`'s own screen as an option, default off
(section 16: both reviewers read the words as the partner's answer to D50 question 1; the question is the partner's).
It changes what the display shows and where keys go, and adds a little kernel code (section 6): two independent
design reviews before building (section 17), one code review after. This is revision 2, after the reviews; built on
branch `intro`, 2026-11-14 (DESIGN D55 "As built": sizes, speed, tests).

## 1. In plain words

- **A program may open a screen of its own**, as AmigaOS programs open custom screens: its own mode (lores or hires,
  interlaced or not, 1 to 8 planes, HAM6, HAM8, Extra Half-Brite), its own palette (up to 256 colours of 24 bits on
  AGA, 32 of 12 bits on OCS/ECS), and bitplanes in its own chip RAM that it draws into directly, as fast as the CPU
  goes: no message per pixel.
- **The Copper, safely:** the program may give a list of Copper instructions in a form of Zenix's own (wait for a
  line, set a colour, point a plane, set the modulos, the scroll). displayd checks every one and writes the real
  Copper words itself, so a program can make copper bars, raster colours, split screens and double buffering, and
  cannot touch any other register, or make the display read memory that is not its own.
- **In time with the picture:** a kernel operation waits for the next vertical blank (section 6); every change of a
  screen says from which frame it shows. Colours change in place, at no cost; a new picture (double buffering) is
  one word for displayd to flip.
- **Front and back:** a new screen comes in front of the desktop. **Left Amiga+N** puts the desktop in front (the
  screen goes behind; its program is told and keeps running); Left Amiga+N again brings the screen back. Left
  Amiga+M on a screen does the same as N (AmigaOS's habit; on the desktop it stays zdesk's "minimise"). The program
  can ask for front or back too. Amiga+F1..F10 still switch to the text consoles.
- **Keys and the mouse** go to the screen's program while it is in front (no pointer on a screen).
- **The kernel keeps the planes alive** while they are on show: a program cannot free them under the display, and when
  it dies the kernel hands them to displayd, which takes them off the glass and only then gives them back (section 6).
- **Not on a graphics card** (D43): refused there with a reason (section 9).
- **`show -s`** opens a picture on a screen of its own with its real palette and mode (HAM in full colour); `s` in
  the window does the same. Off by default.
- **`zintro`**, the Zenix intro (section 13): a logo, copper bars, a starfield, a turning vector shape and a scroller,
  with a ProTracker module of our own through sndd; everything made by a script in the repository.

## 2. What exists (read from the code)

- **The kernel's contract** (kernel/kernel.h `CON_GRAB`; kernel/gcon.c, kernel/gfx.c): displayd hands the kernel one
  Copper list in its chip RAM; while the desktop's console is shown the kernel points `COP1LC` at it (`gfx_hand`).
  The list sets all eight sprite pointers every frame, keeps FMODE's sprite bits 0, fetches at least one plane, at
  most 4,096 moves; FMODE, BPLCON3/4, the modulos and DDF carry over from the kernel's list unless it sets them;
  BEAMCON0 alone. The kernel's list (back on Amiga+F*n* or a panic) writes all of those every frame (`build_cop`).
- **displayd's list** (`screen_up`, `dyn_build`): a fixed part a field (`copl[0]` long, `copl[1]` short: FMODE,
  BPLCON3, BPLCON4 on AGA, the colours, the pointer's, playfield 2's on AGA, the eight sprite pointers), then
  `COP2LC` and `COPJMP2` into the **changing part**, one of two buffers of a list a field (BPLCON0-2, the modulos,
  DDF, the planes; a chips' drag's playfield 2), in one block that never crosses 64 KB, so a change is **one word**,
  `COP2LCL`'s value (`cop_jmp[f]`). `dyn_flip` writes the other buffer, then that word; `dyn_settle` waits until the
  frame after the last flip has begun before a buffer may be written again.
- **A bug found by the reviews** (fixed in this step, fix-when-found): `dyn_flip` read the beam *before* writing the
  jump word. Preempted in between, displayd could write the word after the next frame's Copper had read it, and
  `dyn_settle` would call the old buffer free while the Copper ran it for a whole frame; the next flip would rewrite a
  running list (a torn list can write any register from `$40` up). Now: the words first, then the beam, then the time;
  a late reading only makes the wait longer.
- **What clients can reach:** a client is a protected process under D41 (its own memory only; no custom chips);
  displayd is a system process. The blitter is displayd's and the kernel's.

## 3. The protocol

Numbers in docs/claims.md; structs and meanings frozen (docs/frozen-interfaces.md). A client has **at most one
screen**; its events carry `win` 0 (no window has id 0), so no new id space is needed. `DPY_VERSION` stays 1 (the
requests are new numbers; an older displayd answers -ENOSYS).

**`DPY_SCREEN` 18** - a[0] `struct dpyscr *` -> the vertical-blank count from which it shows (section 4), or -errno.

```c
struct dpyscr {              /* DPY_SCREEN (first size DPYSCR_FIRST 36) */
    unsigned size;
    unsigned short w, h;     /* pixels: w 320 (lores) or 640 (hires); h 1 .. the display's lines (dpyhello's height
                              * at the desktop's hires: 256 PAL, 200 NTSC; twice that with DPYSF_LACE, then even) */
    unsigned short depth;    /* planes, 1-8 (OCS/ECS: lores 1-6, hires 1-4) */
    unsigned short flags;    /* DPYSF_* */
    unsigned planes;         /* the planes' memory: a chip RAM region the client's memalloc gave (its base) */
    unsigned bytes;          /* the part of it the screen may show: every fetch stays inside [planes, planes+bytes) */
    unsigned pstride;        /* bytes from plane k to plane k + 1 (interleaved: a plane's line; else bpr * h) */
    unsigned short bpr;      /* bytes from a line of a plane to its next line */
    unsigned short ncop;     /* the most instructions SS_COPPER will give, 0 .. DPYSCR_COPMAX (1024) */
    unsigned short ncol;     /* colours in palette, 0 .. 256 (32 on OCS/ECS) */
    unsigned short pad;
    unsigned palette;        /* ncol colours, 0x00RRGGBB each (the client's memory) */
};
#define DPYSF_LACE    1      /* interlaced */
#define DPYSF_HAM     2      /* HAM6 (6 planes) or HAM8 (8 planes, AGA) */
#define DPYSF_EHB     4      /* Extra Half-Brite (6 planes, not HAM) */
#define DPYSF_SCROLL  8      /* one more fetch unit on the left of every line (2 bytes OCS/ECS, 8 AGA) */
#define DPYSF_BACK    16     /* opened behind the desktop (set the Copper list, then SS_FRONT) */
#define DPYSF_MOTION  32     /* EV_MOTION also without a button held */
```

Checked: the size; w; h (even when interlaced); the depth for the chips (section 7); HAM with 6 or 8 planes (8 only
on AGA), EHB with 6, not both; on OCS/ECS 6 lores planes need HAM or EHB; `planes` a region of the client's (the
kernel's `CON_HOLD` answers its size, and `bytes` must fit it; section 6) in chip RAM; `bpr`, `pstride` and the
offsets even, multiples of 8 on AGA (64-bit fetch); the planes' fetch inside `bytes` (section 5); `ncop`, `ncol` in
range; the palette readable. -EINVAL (any of those), -EFAULT (the palette unreadable), -EBUSY (a screen already; a drag
under way; four regions held), -ENODEV (RTG; a mode the chips lack), -ENOMEM (no chip RAM for the lists, or past
displayd's screens' budget `SCR_BUDGET` 160 KB), -ENOSPC (four screens open: `NSCR`).

**`DPY_SCR_SET` 19** - a[0] what (`SS_*`), a[1], a[2] -> the vertical-blank count from which it shows, or -errno
(-ENOENT: no screen):

| what | a[1], a[2] | |
|---|---|---|
| `SS_PALETTE` 1 | first \| count << 16, the colours | the base palette, written in place (no flip): first + count within the mode's colours |
| `SS_COPPER` 2 | the instructions, their count (0 .. `ncop`) | the list replaced (a flip); -EINVAL with the bad instruction's index in a[3] |
| `SS_PLANES` 3 | the offset of plane 0 in `planes` | double buffering (a flip); every fetch checked again |
| `SS_FRONT` 4 | 1 in front, 0 behind | -EBUSY during a drag |
| `SS_COLOURS` 5 | first index \| count << 16, count colours | new values for the next count `CO_COLOUR` instructions from instruction first on (other instructions passed over: one call for a colour a line), written in place (no flip); -EINVAL with a[3] the colours set if the list has fewer |

In place means: the value words of colour moves are rewritten in the lists the Copper may be running. A colour word
can do no harm; a frame may show half of the old colours and half of the new. Everything else (pointers, waits,
the list's shape) only ever changes by a flip.

**`DPY_SCR_CLOSE` 20** -> 0. `DPY_BYE` and the client's end do the same: the desktop in front if the screen was,
then (after the Copper is off them) the lists freed and the planes let go.

**The Copper instructions** (`SS_COPPER`), 8 bytes each, in screen order:

```c
struct dpycop { unsigned short op, a, b, c; };
#define CO_WAIT   1   /* a the line (0 .. h-1; interlaced: frame line a / 2 in both fields), b the pixel across
                       * (0: the line's start, before the picture) */
#define CO_COLOUR 2   /* a the colour register (0 .. 255 AGA, 0 .. 31 OCS/ECS), b << 16 | c 0x00RRGGBB */
#define CO_PLANE  3   /* a the plane (0 .. depth-1), b << 16 | c its offset from SS_PLANES' offset, from this line */
#define CO_MOD    4   /* a, b the odd and the even planes' modulos (signed bytes, the hardware's, non-interlaced) */
#define CO_SCROLL 5   /* a BPLCON1 (OCS/ECS: its low 8 bits) */
```

**Events:** `EV_KEY`, `EV_BUTTON`, `EV_MOTION` (the screen's pixels; motion while a button is held or with
`DPYSF_MOTION`), `EV_FOCUS` (arg 1: in front and shown; 0: behind, the console switched away, another screen; every
key counts as up then) - all with `win` 0. No new event type.

**libdisplay** (display/display.h): `dpy_screen`, `dpy_scr_set`, `dpy_scr_close`, and `dpy_vwait(n)` (the kernel's
`CON_VBLANK`; `dpy_vcount()` asks).

## 4. displayd: the screen's Copper lists

- **Trampolines:** the changing part's block grows (3 KB allocated instead of 2 KB; the 1.5 KB part that crosses no
  64 KB boundary is used): the four changing-part lists, then for each of the `NSCR` screen slots, each of two
  buffers and two fields, a trampoline of three moves, `COP2LCH`, `COP2LCL` (that buffer's list), `COPJMP2`, written
  once when the screen opens, and an empty line of 96 bytes (8-aligned): the empty sprite and the planes below a short
  screen. To show a screen buffer, `cop_jmp[f]` names its trampoline: still one word. The desktop back: `cop_jmp[f]`
  names the changing part's list in use.
- **The screen's list** (displayd's chip RAM: two buffers; two fields only when interlaced), each a field:
  1. AGA: FMODE 3 (64-bit planes, sprite bits 0), BPLCON3 `$0C00`, BPLCON4 `$0011`.
  2. BPLCON0 (hires, planes, BPU3 for 8, HAM, LACE, colour burst), BPLCON1 0, BPLCON2 `$0024` (AGA: and `KILLEHB`
     `$0200` unless EHB), the modulos, DDFSTRT, DDFSTOP (section 7).
  3. Sprite 0's pointer to the empty line (no pointer on a screen; sprites 1-7 are empty already).
  4. The palette: OCS/ECS 12-bit; AGA a bank of 32 at a time, `BPLCON3` with `LOCT` 0 then the high nibbles, `LOCT`
     1 then the low nibbles (always both, so a later in-place change can set any colour), `BPLCON3` `$0C00` last. Only
     the colours the mode shows (`1 << depth`; HAM6 16, HAM8 64, EHB 32).
  5. The plane pointers: plane k at `planes + offset + k * pstride`; the short field's one line on (`+ fetch + mod`).
  6. The program's instructions, translated (section 5).
  7. A screen shorter than the display: after its last line, every plane on the empty line with the modulos
     `-fetch` (colour 0 below; one plane still fetched, the kernel's contract).
  8. The end: not interlaced, `COP1LC` = `copl[0]`; interlaced, the desktop's field scheme (`COP1LC` = the long
     field's list, a WAIT for the line only a long field has, then the short field's). The desktop's changing part
     gains `COP1LC` = `copl[0]` at its end when not interlaced, so a desktop after an interlaced screen runs its one
     list again.
- **On the way back** the desktop's lists set everything a screen changes (FMODE, BPLCON3/4, the colours and all
  sprite pointers in the fixed part; BPLCON0-2, the modulos, DDF in the changing part; COP1LC as above), and the
  kernel's list sets everything; DIW is never changed.
- **Two buffers, kept the same:** a buffer holds the list of some version of the screen's shape (the palette's size,
  the instructions); colours are written in place into every buffer of the current shape. `SS_PLANES` writes the
  pointer words into the buffer not in use (rebuilding it whole only if it holds an older shape) and flips;
  `SS_COPPER` rebuilds that buffer and flips. The word offsets of every colour value and every pointer are recorded at
  translation (the same in both buffers and fields), so the in-place and the pointer writes are a few stores each.
- **Settling:** before a buffer is written (a rebuild or pointers), `dyn_settle`: the frame after the last flip of
  that word has begun. A program that changes its picture once a frame, after the vertical blank, never waits; a
  second flip in the same frame waits for the next frame (displayd spins at most a frame; only the front screen's
  owner can cause it, and the desktop is not on show then). Documented as "one new picture a frame".
- **From which frame:** after a flip, displayd reads the kernel's count, the beam and the count again (again while
  the two counts differ): count + 1, or count + 2 when the beam is within the frame's first 16 lines (the
  vertical-blank interrupt may not have counted yet, or the Copper may not have read the jump). A late reading only
  answers later.
- **While a screen is in front** displayd goes on drawing the desktop into its bitmap as always (nothing to catch up);
  a drag's or a mode change's flip builds the desktop's buffer but leaves the jump on the screen. Bringing a screen in
  front ends a held button's window, a menu's pointer grab and a frame press (no stuck menu).
- **Front and back:** one screen in front at most. Left Amiga+N, and Left Amiga+M while a screen is in front, are
  displayd's, before zdesk's grabs (zdesk's keys are not delivered while a screen is in front; zdesk refuses a
  `lamiga+n` line in its keys file, as it refuses Amiga+F1..F10). Left Amiga+N with the desktop in front brings back
  the screen last in front. A new screen comes in front unless `DPYSF_BACK`.
- **The end of a screen:** `DPY_SCR_CLOSE`, `DPY_BYE`, the client dropped, or the kernel's `GEV_GONE` (the owner of
  a held region ended: section 6). The desktop in front if needed, `dyn_settle`, then the lists freed and the planes
  let go (`CON_HOLD` 0), in that order.

## 5. The checks on a program's Copper list

The first bad instruction refuses the whole list (the old one stays); each translated instruction is written with
the buffer's room checked.

- **CO_WAIT:** the line within the screen, never before the previous WAIT's line and position. Written as the beam's
  line (the display window's first line + the frame line; `$FFDF` once when it passes 255) and position (b: the
  display window's start + b / 2 colour clocks, lores pixels; hires b / 4).
- **CO_PLANE, CO_MOD** only right after a CO_WAIT whose b is 0 (any order among themselves, at most 12 moves of
  them), or before the first WAIT. They are written after a WAIT for the end of the previous line (position `$D9`,
  after its fetch, as `dyn_build` does), so they take effect exactly at their line, never part-way through one.
- **The fetch, worked out exactly:** for each plane, from each change (its pointer, the modulos) to the next or the
  screen's end, the first and the last line's fetch (with the line's advance `fetch + modulo`, positive or negative;
  interlaced, each field's own: the field modulo `fetch + 2 x modulo`, the short field a line on) lie inside
  `[planes, planes + bytes)`. `fetch` is the bytes the hardware fetches a line: w/8 (and the scroll unit with
  `DPYSF_SCROLL`); section 7's test measures it. The extent of the whole list, relative to `SS_PLANES`' offset, is
  kept, so `SS_PLANES` checks a new offset with two comparisons. Every modulo written is checked to fit 16 bits.
- **The Copper's time:** a group of moves must finish before the next WAIT's line: at most 14 moves for each line
  between them (a conservative rate for every mode: a move takes 4 colour clocks, fewer slots are free during an
  OCS hires fetch), the last group before the screen's end likewise, and the base part with the moves before the
  first WAIT within the vertical blank (40 moves a line for its lines above the display window). A list that the
  Copper could not run in time is refused (its index), so no change lands late on a line the check did not see.
- **CO_COLOUR:** the register within the chips'; AGA 4 moves (high nibbles at LOCT 0, low at LOCT 1, BPLCON3's
  other bits the screen's, the bank as needed), OCS/ECS 1.
- **CO_SCROLL:** any value on AGA (all of BPLCON1's bits are scroll); the low 8 on OCS/ECS.
- **The total** of a translated list stays within the kernel's 4,096 moves less the fixed part's (64).
- **What it cannot do:** write any other register (DMACON, INTENA, COPxLC, the blitter's, the sprites', BEAMCON0,
  DIW, FMODE), jump, skip, wait on the blitter. The words are displayd's, in displayd's memory.

## 6. The kernel: the vertical blank's count, and planes held while shown

**`CON_VBLANK`** (`SYS_console` op 12, no descriptor, any process): a2 the count to wait for -> the count now (31
bits). The vertical-blank interrupt adds one to the count; when a process waits it sets `kwork_pending`, and kwork
wakes the waiters (D16's `ev_wake`: producers run in process context, event.c's rule). A wait is `ev_arm`, the test,
`ev_block` (the check-then-block order); it ends when the count reaches a2 (`(s32)((now - a2) << 1) >= 0`), after 100
ms at most (no hang if the display stops), or with -EINTR for a signal (not restarted). a2 with bit 31 set: only ask.
-ENODEV without a display; on a graphics card (D43) the count is still the chipset's. Latency: the waiter runs at the
next kwork, at once on an idle machine, up to a tick (10 ms) later while another program keeps the CPU (said in the
man page). Cost: an increment and a test of an empty mask a frame.

**`CON_HOLD`** (op 13, on the grab's descriptor, the grab's process only): a2 a pid, a3 the base of a region that
pid's `memalloc` gave, in chip RAM -> the region's size: the region is held (at most `NHOLD` 4). a2 0: let go of the
region at a3 (-EINVAL if none). While held:
- the owner's `memfree` of it answers -EBUSY (as for the grab's own Copper list);
- when the owner ends, `proc_exit` makes the grab's process the block's owner (it leaves the dead process's regions,
  so nothing else frees it) and puts a `GEV_GONE` record (6) in the grab's ring: `x` the hold's slot, `ch` the base
  (big-endian), so displayd knows at once (also for a zombie, whose memory is otherwise freed at exit);
- displayd then takes the screen off the glass, settles, and lets go: the kernel frees the block;
- the grab ending frees every block it was handed and ends every hold.
Without this a program could free its planes (or die) and the display would show whatever the next owner wrote
there, with no time limit. Cost: about 300 bytes of kernel code; the ring's `GEV_GONE` uses a reserved control slot.

## 7. Modes on the chips

| | OCS/ECS | AGA |
|---|---|---|
| lores, `w` 320 | 1-6 planes (6: HAM6 or EHB), 16-bit fetch, DDF `$38`-`$D0` | 1-8 planes, HAM6, HAM8, EHB; 64-bit fetch, DDF `$38`-`$B0` (measured)* |
| hires, `w` 640 | 1-4 planes, 16-bit fetch, DDF `$3C`-`$D4` | 1-8 planes, HAM6, HAM8, EHB; 64-bit fetch, DDF `$38`-`$C0` (the kernel's) |
| `DPYSF_SCROLL` | DDFSTRT one unit earlier (`$30` lores, `$38` hires), fetch + 2 | one 64-bit unit earlier (`$18` lores, `$28` hires: displayd's drags), fetch + 8 |
| colours | 32 of 12 bits | 256 of 24 bits |

\* `demoscene/aga-specifics.md` 4.3 lists `$B0`, `$B8` and `$C0` from different sources for 320 pixels at 64-bit
fetch. The screen test measures the bytes fetched a line (a pattern unique to each line with modulo 0: a wrong fetch
shifts every line after the first) and the window shows all 320 pixels, none twice: `$B0` passes, pixel-exact
(2026-11-14, FS-UAE's A1200), hires' `$C0` likewise. OCS/ECS values are the hardware manual's (read); the tests run on FS-UAE's A1200 (AGA).

64-bit fetch for every AGA screen: 8 lores planes at the 16-bit fetch take every bus slot during the picture, which
would stop a stock A1200's CPU (it runs from chip RAM) for most of each frame. PAL and NTSC: the height and the field
lines come from the console's (`dpyhello`), never a fixed 256.

## 8. Safety

- A program reaches only its own memory (the planes; the Copper words are displayd's), through checked instructions;
  the Copper runs only displayd's words (no program jump, no danger bit; every register written is one of: the
  colours, BPLCON0-4, BPL1MOD/BPL2MOD, BPLxPT, DDFSTRT/STOP, FMODE, SPR0PT, COP2LC, COPJMP2, COP1LC).
- A half-written list never runs: the flip is one word, sampled after it is written; the buffer written is never
  one the Copper may run (`dyn_settle`); colour value words are the only ones written in place.
- The planes cannot be freed under the display, and a dead owner's planes stay held until they are off the glass
  (section 6).
- The kernel's contract stays: all eight sprite pointers every frame, FMODE's sprite bits 0, at least one plane
  fetched, at most 4,096 moves, BEAMCON0 and DIW untouched.
- Keys: the console switches stay the kernel's; Left Amiga+N always reaches displayd, so a screen whose program
  stopped reading events can still be put behind and the program killed from the desktop.

## 9. RTG (D43): refused

With the desktop on a graphics card, `DPY_SCREEN` answers -ENODEV; `show -s` and `zintro` say "a screen of its own
needs the chipset's display (the desktop is on a graphics card)". Not emulated: converting planes and HAM into the
card's pixels every frame in the CPU, with the Copper's per-line colours, would be slow and large, for a case that
has a better answer later: on the PiStorm the HDMI can show the chipset's picture (the text consoles do it, D43's HVS
switch), so a later step may switch it for a screen. That needs the partner's PiStorm to test.

## 10. Memory and size

- displayd: about 3.5 KB of code estimated, 8.3 KB measured (DESIGN D55 "As built"; the demo floppies' displayd is
  built without it), four screens' records and the
  translation's offsets in fast RAM (at most about 6 KB for a 1,024-instruction list); the changing part's block 3 KB
  of chip RAM instead of 2 KB.
- A screen's lists: 2 buffers x fields x (base + translated instructions + 30) moves x 4 bytes: a 16-colour lores
  screen with a 512-instruction list (a colour a line) about 2 x (70 + 1,300 + 30) x 4 = 11 KB of chip RAM; the worst,
  256 colours, interlaced, a full list about 2 x 2 x 4,032 x 4 = 64 KB; at most `SCR_BUDGET` 160 KB for all screens.
- The kernel: about 450 bytes (section 6).

## 11. `show -s`: a picture on its own screen

- `show -s FILE` opens the picture on a screen in its mode (lores or hires, interlaced, its planes, HAM6, HAM8, EHB)
  and palette; `s` in the window does the same for the picture shown; on the screen `s`, `q`, Esc or a click go back
  to the window; `n`, `p` show the next and the previous picture on the screen; the arrows move a larger picture.
- The planes are copied row by row into chip RAM in the screen's interleaved layout (each row padded to the fetch's
  alignment: a 352-pixel row is 44 bytes, 48 on AGA); a picture narrower or lower than the screen sits top left on
  colour 0. A GIF (up to 256 colours) becomes planes (AGA: as many as its colours need; OCS/ECS: at most 32 colours,
  else the window says why).
- HAM8: the hardware's control bits are bitplanes 1 and 2 (AGA's documentation); show's decoder takes the file's top
  two planes (D50 section 3). The screen opens behind (`DPYSF_BACK`) with a Copper list whose first line points the
  planes in that order, then comes in front: the copy stays one block. A test compares a HAM8 file's screen with the
  generator's own decode.
- Larger pictures: the top left, the arrows move it (lines exactly; across in steps of 16 pixels, 64 on AGA).
- Refused, said in the window: 24-bit pictures (no native mode), super-hires, RTG.

## 12. Tests

- **`tools/screen-test.sh`** (FS-UAE, the stock A1200 with 4 MB fast through the cage rig, STOCK): `dpytest`'s
  screen commands: a lores 4-plane screen with a known pattern, its pixels compared in the screenshot; the fetch test
  (section 7, lores and hires); a palette change in place; a Copper list of a colour a line, compared line by line;
  `SS_COLOURS`; `SS_PLANES` to a second picture; the bad lists each refused with its index (a WAIT going back, a plane
  outside the memory, a negative modulo running out of it, an odd offset, a plane after a colour, too many moves for
  the lines, an unknown op, more than `ncop`); memory that is not the client's region refused; the region's
  `memfree` refused while shown; Left Amiga+N: the desktop's pixels back, N again: the screen; Amiga+F1 and back with
  the screen in front; the close; `kill -9`: the desktop back at once (`GEV_GONE`) and the chip RAM back.
- **`CON_VBLANK`**: 100 frames counted against the E-clock (PAL 50 a second within 2%), a signal ending a wait,
  only-ask.
- **`show -s`** (in show-test): ham.iff (HAM6) on its own screen, every pixel of a sample equal to the generator's own
  HAM decode; HAM8 likewise; `s` back to the window.
- **`tools/intro-test.sh`** (STOCK): `zintro` for 20 s: the logo's pixels in the screenshot, sndd's state saying a
  module plays, the frames it drew and missed, Esc: the desktop back; again, ended by a click; again, killed: the
  desktop back.
- The desktop's tests still pass: dpy-test, zdesk-test, files-test, show-test.

## 13. The intro, `zintro`

- **What it shows** (lores, 4 planes, the whole display): the **ZENIX logo** at the top (bevelled steel letters with
  a toxic green edge, drawn by the script); **copper bars** of cold colours sweeping through the background (colour 0
  every second line, the whole height: half the colours to send each frame); a **starfield** of three speeds in the middle; a **turning wireframe shape** in a
  dirty red over the stars; a **scroller** at the bottom, its letters shaded line by line by the Copper. No human
  figures. The text is written for it: dark, indifferent, about the machine.
- **The frame:** `dpy_vwait`; the bars' colours with one `SS_COLOURS` (in place); the back picture drawn; `SS_PLANES`
  to it. Two pictures in chip RAM (40 KB each); the logo drawn into both once; the stars and the shape erased where
  they were two frames ago; the scroller shifted in fast RAM and copied in.
- **Sound:** a 4-channel ProTracker module composed by the script (A minor, slow and heavy: a sub bass, a lead, a
  pad, noise drums), played through sndd as a frames stream (the replayer `user/modrep.c`, as Lua's zenix module does:
  made 100 ms ahead of the E-clock, never blocking a frame). No sound server, or the channels busy (`modplay`
  running): silent, said once.
- **Quitting:** Esc, `q`, a click: the module stops, the screen closes, the desktop is back. Killed: the kernel hands
  the planes to displayd (section 6), sndd drops the stream.
- **Assets:** `tools/mkintro.py` writes the logo's planes, the scroller's font (our own 16 x 16 letters), the
  palette, the text and the module into a C header in the build folder; nothing third-party.
- **Package:** the `games` family (tools/mkzup.py), next to gridrun.

## 14. Numbers claimed

`DPY_SCREEN` 18, `DPY_SCR_SET` 19, `DPY_SCR_CLOSE` 20; `SS_PALETTE` 1 .. `SS_COLOURS` 5; `struct dpyscr` (first size
36) with `DPYSF_*` 1-32; `struct dpycop` (8 bytes) and `CO_WAIT` 1 .. `CO_SCROLL` 5; `DPYSCR_COPMAX` 1024; events with
`win` 0 as the screen's; `SYS_console` op 12 `CON_VBLANK` (a2 bit 31: only ask) and op 13 `CON_HOLD`; the grab's
record `GEV_GONE` 6; `NHOLD` 4.

## 15. Not in this step

The blitter for screen programs (a queue of checked blits is a design of its own), sprites on a screen, super-hires,
overscan and dual-playfield screens, a screen on a graphics card (section 9), several screens shown at once
(dragging a screen down), colour cycling in `show`, ANIM.

## 16. For the partner

- **The approval's reading:** both reviewers read "it waits for D50's full-screen call" as waiting for your answer to
  D50 question 1 (a screen of its own for pictures). The main session read it as the protocol request. Built on
  branch `intro`, not merged: yours to say.
- `show`'s own screen is an option (`-s`, `s`); the default is still the window (D50 question 1).
- Left Amiga+N for the desktop (AmigaOS's "Workbench to front"), Left Amiga+M as well while a screen is in front;
  zdesk keeps Left Amiga+M for minimise on the desktop.

## 17. The design reviews (2026-11-14, two independent, before building)

Both found the shape sound (displayd translating a checked list, a one-word flip, a kernel frame counter) and asked
for these, all taken into revision 2:
- **The flip's race** (both): `dyn_flip` sampled the beam before writing the word, so a preempted displayd could
  rewrite a running list; an existing bug that one flip a frame would make routine. Fixed for drags too (section 2).
- **Freed planes on show** (both): a program's `memfree`, or its death (and a zombie, which `kill(pid, 0)` counts as
  alive), left its memory on the glass. The kernel now holds the planes (section 6, `CON_HOLD`, `GEV_GONE`).
- **The wake-up from the interrupt** (both): producers run in process context; the count's waiters are woken by kwork.
- **The cost of a list a frame** (review 1): rebuilding and checking a 300-500 instruction list each frame would eat
  a stock A1200's frame. Now colours change in place (`SS_COLOURS`, `SS_PALETTE`), pointers by writing a few words
  and a flip; only a new list's shape is translated.
- **Mid-line and late changes** (both): pointers and modulos written part-way through a line, or a long group of
  moves landing lines late, broke the fetch check. Now pointers and modulos only at a line's start, written after the
  previous line's fetch; a Copper time budget refuses lists that cannot run in time; the base part fits the vertical
  blank; the total stays under the kernel's 4,096 moves.
- **Interlace** (review 2): the field modulo is `fetch + 2 x modulo` and the short field a line on, not `+ bpr`.
- **`DPYSF_SCROLL`'s pointer** (review 2): the client's line includes the extra unit; the pointer is its offset.
- **The fetch at 64 bits** (both): measured by the test as bytes a line, not only columns shown.
- **The struct** (both): `struct dpyscreen` exists already (display/screenfile.h): renamed `dpyscr`; 36 bytes, not 40.
- **HAM8's plane order** (both): a HAM8 screen test against the generator's decode.
- **Smaller:** a chip budget for screens, the second field's lists only when interlaced, the palette's range checked
  against the base part's room; close order (settle before freeing); the frame answer read count-beam-count; key-ups
  on `EV_FOCUS` 0, grabs reset when a screen comes in front; zdesk refuses `lamiga+n`; `SS_FRONT` and -EBUSY; the
  height from the console, not the desktop's mode; `zintro` silent when sndd's channels are busy; docs and tests the
  rules require.
- **Not taken:** pointers patched in place (a torn high/low pair could fetch anywhere: they flip); a third review
  (the code review after building covers the changes).

## 18. The code review

(to be filled after the review)
