# D43: RTG - the desktop on a graphics card's screen

Roadmap block 6, "RTG" (docs/ROADMAP.md "The order of work", approved by the owner 2026-10-25; the details delegated,
decided by the builder, as D40-D42's). Revision 1, 2026-11-12, branch `rtg`. Steps 1-3 (section 9) are built and
checked in FS-UAE against a pretend card; **nothing here has run on a real card**: the PiStorm's first run is
section 11. No kernel code changes (0 bytes); no system call, message number or error number changes.

## 1. In plain words

On the partner's PiStorm A1200, AmigaOS runs a 1280 x 960 Workbench through the Pi's own graphics (VideoCore.card, a
Picasso96 card driver). Zenix's desktop today is the chipset's 640 x 256 screen. With this:

- **`zenixboot RTG`** (from an AmigaOS whose front screen is an RTG screen, e.g. the Workbench) takes that screen's
  picture memory with it into Zenix: where it is, its size, its pixel format. AmigaOS set the mode up; Zenix keeps it.
- **The display server draws the desktop there**: the same windows, frames, bar, terminals and programs, on 1280 x 960
  (or whatever the screen was), in the same 16 colours. No program changes: they draw through the server as before.
- **The text consoles stay on the chipset** as they are (Amiga+F1..F10). On the partner's machine, where the chipset's
  picture reaches the HDMI through the C790 capture, switching consoles also switches the HDMI between the two
  pictures, as AmigaOS's VideoCore.card does when a native screen comes to the front.
- **Without `RTG`** (and on every machine without an RTG screen: the stock A1200, the Apollo 1260) nothing changes.

The kernel does not know about any of it: the record is written by zenixboot into the boot's hardware table, and the
display server reads it. Pi-only code is a single register write in the display server, used only when zenixboot found
the Pi's display list register.

## 2. Which hardware

| Machine | What it has | What D43 does there |
|---|---|---|
| PiStorm (Emu68), the partner's A1200 | VideoCore.card: the framebuffer in `vc4-mem` (Emu68's 16 MB reserved for the GPU, knowledge base `amiga/emu68-network-usb-rtg-hardware.md` 4), shown by the Pi's HVS through one display list; the C790 capture shows the chipset's picture through another list; register `SCALER_DISPLIST1` (bus 0x7E400024) says which | the first target: the screen AmigaOS set up, the two list offsets recorded by zenixboot, the display server flips the register when its console is shown or left |
| A1200 + Apollo 1260 | no RTG card (the 1260 has no video; the A1200 has no Zorro slots) | nothing: the chipset desktop. A Zorro card would need a big-box Amiga or a busboard |
| A big-box Amiga with a Zorro RTG card (Picasso, CyberVision, ...) | a Picasso96 driver under AmigaOS | the same hand-over works (any Picasso96 board with a hi- or true-colour screen); the card keeps showing its picture, as nothing in Zenix knows how to switch it (section 6). Not a test machine of ours: untried |
| FS-UAE's `uaegfx` | a Picasso96 board the emulator implements itself (knowledge base `amiga/rtg-uaegfx-and-window-macros.md`) | would work the same way under AmigaOS with Picasso96 installed (not part of the test suite: Picasso96 is not free to ship); the tests use a pretend card instead (section 10) |

Why not program the card ourselves: every card is different (the HVS's display lists, a Cirrus chip, uaegfx's traps).
Picasso96 already did the hard part, the mode and the monitor timing; taking over a running mode is what a boot loader
does for an OS on a PC (a "boot framebuffer"). Our own mode setting for the HVS (another size, a CLUT palette, the
HVS's sprite for the pointer) is a later step on the Pi only (section 12).

## 3. The hand-over: `struct hwfb` in the hardware table

zenixboot's new switch `RTG` (docs/man/zenixboot.txt). Just before AmigaOS is switched off (after the kernel and the
RAM disk are loaded and checked, so a failure still returns to AmigaOS):

1. Picasso96API.library (version 2 or later) is opened; the front screen's bitmap (`IntuitionBase->FirstScreen`, its
   RastPort's BitMap) is asked `p96GetBitMapAttr(ISP96)`; not an RTG screen, or no Picasso96: said ("zenixboot: RTG:
   the front screen is not an RTG screen: the desktop stays on the chipset") and Zenix starts as without `RTG`.
2. Its width, height, bytes per row, bytes per pixel and `RGBFormat` are read (the memory's address comes last,
   step 5: Picasso96 switches no screen while a bitmap is locked). Formats taken: the 16-bit ones (`R5G6B5`, `R5G6B5PC`, `B5G6R5PC`,
   `R5G5B5`, `R5G5B5PC`, `B5G5R5PC`), 24-bit (`R8G8B8`, `B8G8R8`) and 32-bit (`A8R8G8B8`, `A8B8G8R8`, `R8G8B8A8`,
   `B8G8R8A8`). A palette screen (`CLUT`) is refused with a message: its palette is set through the card driver,
   which Zenix does not have (on the Pi it lives in the HVS's list memory: section 12).
3. A borderless backdrop window with a blank pointer is opened on that screen and activated, so the card's hardware
   pointer (the HVS sprite on the Pi) is empty when AmigaOS stops, not frozen over the desktop.
4. On a Pi (the hardware table's `HW_DT`): `SCALER_DISPLIST1` is read with the RTG screen in front (the RTG list's
   offset), then AmigaOS is told to show no view (`LoadView(NULL)`, which zenixboot always did) and after two
   frames read again (the chipset's list: on the partner's machine the C790's passthrough). Both are recorded with
   the register's address. Elsewhere the `LoadView(NULL)` is left out, so the card keeps showing its picture.
5. Then `p96LockBitMap` gives the memory's address (never unlocked: AmigaOS ends; locked after steps 3 and 4,
   which need Picasso96 free to switch). The record goes into the hardware table right after `board[]` (offset 408, `kernel/hwinfo.h`: the table grows
   only at its end, docs/frozen-interfaces.md):

```
struct hwfb {                   /* 32 bytes, at hwinfo + 408 */
    u32 magic;                  /* HWFB_MAGIC "ZFB1"; anything else: no framebuffer */
    u32 base;                   /* the first pixel's address */
    u16 width, height;          /* pixels */
    u32 bpr;                    /* bytes a row */
    u8  format;                 /* Picasso96's RGBFormat number (RGBFB_*), as zenixboot found it */
    u8  bpp;                    /* bytes a pixel: 2, 3 or 4 */
    u16 flags;                  /* HWFB_HVS 1: hvs, dl_rtg and dl_chip are valid; HWFB_TEST 2: a pretend card (tests) */
    u32 hvs;                    /* the Pi's SCALER_DISPLIST1 as the 68k sees it (soc_base + 0x400024), 0 none */
    u32 dl_rtg, dl_chip;        /* its value showing the card's picture, and the chipset's */
};
```

The kernel's stage0 clears the table first (it already does, `sizeof *hw`), so a kernel and a zenixboot of different
ages never meet a stale record: an older zenixboot leaves it zero, an older kernel's stage0 clears only the first 408
bytes and the new zenixboot writes its record after stage0. The floppy's bootblock never writes one (no Picasso96 is
running before a floppy boots).

The framebuffer is the card's memory, outside every memory list AmigaOS gave Zenix: no pool ever hands it out. On a
68040/68060 with D37's data cache, a Zorro board that is not RAM inside the cached block turns the cache off for
that block (D37 3, `CP_SHARED`), so card memory is never cached copyback.

## 4. The display server on a framebuffer

`displayd` reads the record from the table (`HWINFO_ADDR` + 408) after its grab of the console (from then on it is a
system process, D41 7: it may read any memory), checks it (magic, sizes up to 4096 x 4096, `bpr` at least the
width's bytes, a known format, the base not 0) and, if good, draws there. Nothing else in the design changes; what
differs:

- **The pens' picture in fast RAM.** The server draws into a bitmap of one byte a pixel (the pen, 0-15), in fast
  RAM: the screen's "shadow" (1,228,800 bytes at 1280 x 960). The drawing code gets a chunky path beside the planar
  one (`display/server/fb.c`): fill, invert, copy, the 8 x 8 glyphs, the desktop's font's masks, images (the clients'
  planar images read bit by bit), all clipped to the bitmap as before. Kept copies of covered windows (smart refresh)
  are chunky too, in fast RAM, with a budget of 2 MB instead of 256 KB of chip RAM (a 640 x 400 window is 256 KB).
- **Shown by changed rectangles.** Every drawing on the screen adds its rectangle to the damage list (draw.c's
  `dirty_add`, which the drawn-off-screen drag already has); at the end of each pass of the main loop the server
  converts those rectangles into the card's pixels through a 16-entry table (pen -> pixel bytes in the card's
  format, from the 12-bit palette: each nibble times 17). The card's memory is only ever written, never read: reading
  a framebuffer is the slow part on Emu68 (knowledge base `amiga/graphics-and-performance.md`, "RTG on PiStorm").
  So a whole-window drag is "drawn off the screen" by nature: `shadow_begin` costs nothing and every move is one
  conversion of what changed.
- **No blitter.** The blitter reaches only chip RAM: every operation is the CPU's (the RTG machines are the PiStorm
  and big-box Amigas with fast CPUs). The kernel's blitter hand-over still works (the server answers at once).
- **The pointer is drawn by the server**: the same 16 x 16 arrow (on the chipset its pixels are lores, twice as wide:
  on a 1280 x 960 card it looks about half the size), black and white, over the converted pixels; a move
  marks the old and the new place damaged. It follows the kernel's pointer, which moves over the chipset's 640 x 256
  (`GEV_POINTER`), scaled to the card's size: at 1280 x 960 a mouse count moves it 4 pixels across (the kernel moves
  two hires pixels a count, doubled) and 3 or 4 down, so it stops only on every 4th column and every 3rd or 4th line
  (the same speed as on the chipset's screen). Exact 1-pixel steps need the kernel to clamp the pointer to the card's
  size, about 30 bytes of kernel code (section 13, question 1).
- **The chipset's screen** while the console is grabbed: a Copper list with no planes (`BPLCON0` 0x0200), colour 0
  the desktop's background, the sprite blank; it lives in the 1 KB of chip RAM the server always takes (the sprite and
  the list), and the 80-160 KB screen and the 2 KB of changing lists are not allocated.
- **Depth and modes.** Clients are told `depth 4` (16 pens) and the card's width and height (`DPY_HELLO`,
  `EV_SCREEN`), so they work unchanged; the palette is the same 16 pens. `DPY_MODE` answers `DPYM_RTG` (2, new) and
  changes nothing on a card's screen (hires and interlace are the chipset's); `/etc/zdesk/screen`'s `mode` line is
  the chipset's mode: the RTG choice is made at boot (`zenixboot RTG`), never in that file. `deskprefs` therefore
  shows no mode choice that works on RTG (a limit, section 12).
- **The chips' drag** (AGA dual playfield, D38 "Dragging with the chips") is the chipset's only; on a card the
  frame style's choice is kept (outline on a 68020/030, the whole window on 040/060 and Emu68).

## 5. Input, consoles, the switch

- Keys and the mouse come from the kernel's ring as before (`CON_GRAB`, D38 3.4): unchanged.
- **Amiga+F*n*** works as before: the kernel asks the server to let go (`GEV_RELEASE`), the server answers at once.
  With `HWFB_HVS` it writes `dl_chip` into the HVS's register on the release and `dl_rtg` on `GEV_ACQUIRE`, and
  `dl_chip` when it ends. That is the whole Pi-specific code (two lines and a check), the same register write
  VideoCore.card's "switch method CSI" does under AmigaOS.
- If the server is killed or crashes while its console is shown, nothing switches the HDMI back: the frozen desktop
  stays on it until a new display server starts and ends (or a reboot); the text consoles still work by keyboard,
  serial and telnet. The kernel cannot do it, as it knows nothing of the register (a limit of this step).
- Without `HWFB_HVS` (other cards, the pretend card) the card keeps its picture while a text console is shown; the
  console is on the chipset's output as always.

## 6. What a card needs that we do not do

The card's own registers are never touched: no mode setting, no palette, no hardware pointer, no blitter of the card,
no monitor switch except the Pi's register above. A Zorro card's monitor switch (a relay that passes the chipset's
picture when the card is "off") stays as Picasso96 left it: on.

## 7. Speed, memory and sizes

Emulator timings are not hardware timings, so none are claimed. What the work is: a full screen at 1280 x 960 is
1,228,800 pens converted (a byte read, a table look-up, 2-4 bytes written to the card) - only on a full repaint; a
window's change converts only its rectangles. The PiStorm's 68k (Emu68) and a 68060 do that in milliseconds; a
14 MHz 68020 with a Zorro card would be slow, and is not an RTG target of ours.

Memory at 1280 x 960: the pens 1,228,800 bytes of fast RAM, kept copies up to 2 MB, the pens' table 76 bytes; chip
RAM about 82 KB less than on the chipset (no screen); the card's memory is the card's. Measured in FS-UAE (procinfo,
`dpytest mem`, two windows): displayd 606 KB with an 800 x 600 card (480,000 bytes of pens), 905 KB with 1024 x 768
(786,432). On the PiStorm (2 GB) none of this matters; the minimum machine (4 MB fast) has no card.

Sizes (measured, `make`, 2026-11-12): the kernel 0 bytes; `displayd` 65,184 -> 70,536 bytes on the disk (+5,352:
fb.c 1,976 bytes of code at -Os, the chunky branches in draw.c about 1,100, the card's start, the pointer and the
Pi's switch in displayd.c about 1,600, the relocations); every chipset desktop carries it. `zenixboot` 53,612 ->
55,960 (+2,348). `dpytest` (a test program) 12,076 -> 13,616. The DD demo floppy: 64,000 -> 60,416 bytes free
(-3,584, packed). Since the block 6 audit (2.3) the demo floppies carry a displayd built without the card
(`-DNO_RTG`, Makefile `displayd-nortg`): 65,236 bytes on the disk, 96,076 in memory (the full one 70,536 and
101,032); the DD demo floppy 57,856 -> 60,928 bytes free (measured, `make demo`, 2026-11-12).

## 8. What does not change

The display protocol (only `DPYM_RTG` added), libdisplay, zdesk, term, dlogin, deskprefs and every client; the
kernel (0 bytes, kernel.bin 190,684 as before: the record's struct grows in `kernel/hwinfo.h` only; stage0 clears
440 bytes instead of 408, the same instructions; `DRV_HWINFO` still copies at most the first 408, as letting it copy
more cost 64 bytes of code); the bootblock; the text consoles; the chipset desktop when there is no record.

## 9. The order of building (as built)

1. `kernel/hwinfo.h`: `struct hwfb` after `board[]`, `HWFB_*`. docs/frozen-interfaces.md.
2. `display/server/fb.c` (the chunky drawing, the pens' table, the conversion, the pointer) and the chunky branches
   in draw.c; displayd's framebuffer start (`fb_screen`), the present at each pass, the pointer, the HVS switch;
   win.c's kept copies sized for either kind. The Makefile names `fb` among displayd's objects (one word, a separate
   commit: the Makefile is a shared file; fb at -Os, as displayd, win and amifont).
3. The tests: `tools/rtg-host-test.sh` (the PC, every drawing operation against a pixel-by-pixel model and every
   format's conversion) and `tools/rtg-test.sh` (FS-UAE, a pretend card: section 10). `dpytest -fb` and `peek`.
4. `zenixboot RTG` (section 3). Read from the Picasso96 interface files and VideoCore.card's source, not run: there
   is no Picasso96 in the test suite.

## 10. Tests

- **`tools/rtg-host-test.sh`** (the PC, AddressSanitizer): fb.c compiled for the PC; 4,000 random fills, inverts,
  copies (also overlapping, every direction), glyphs at any column (partly off the bitmap) with bold and underline,
  font masks and planar images with and without masks, each compared pixel for pixel with a plain model; every one of
  the twelve formats' bytes for a known colour; 300 rectangles a format converted into a card with the pointer over
  them, clipped at the card's edges, nothing written outside (guard rows, row padding). 7,028 checks, passed; four
  deliberate mutations of fb.c each caught.
- **`tools/rtg-test.sh`** (FS-UAE, the stock A1200's 68EC020 with 8 MB of fast RAM, through the cage rig like
  `dpy-test.sh`): `dpytest -fb WxHxB` (root's) writes a pretend card's record into the hardware table (`HWFB_TEST`,
  its address 0), then opens the display; displayd allocates the card in its own fast RAM (so it lives exactly as long
  as it is drawn on), writes the address back, takes it by the same path as zenixboot's, and clears the record at its
  end. Two setups: 800 x 600 A8R8G8B8 and 1024 x 768 R5G6B5 (the 16-bit packing on the 68k). Checked: the
  hello says the card's size, depth 4; displayd's log names the card; every pixel shown (the corners are the desktop's
  colour, not the card's fill); windows placed as on the chipset and kept on the larger screen; the card's pixels
  (`dpytest peek X Y`, read from the card's memory) are the pens' colours inside the windows, on the title and on the
  desktop, after a move, a raise, a lower, a hide and show, a scroll and a close; each program asked to draw once
  (smart refresh in fast RAM); the pointer's arrow at the middle, then at its new place after the mouse moved
  (`CON_INPUT`), the old place the desktop again; `DPY_MODE` answers 2 and changes nothing; the chipset's screen one
  colour (a screenshot); after the last client the console is text again, and the next desktop is the chipset's
  (the record cleared). **Passed, both setups (2026-11-12).**
- **`tools/zenixboot-test.sh errors`**: `zenixboot CHECK RTG` and `zenixboot RAM=ramdisk RTG` on an AmigaOS without
  Picasso96 say so and go on (Zenix boots). Passed (2026-11-12).
- The dpy, zdesk, gcon and lace tests (the chipset's desktop) must pass unchanged.

## 11. What only the real PiStorm can show (the first trial)

From the partner's Workbench (1280 x 960, a 16- or 32-bit mode): `zenixboot RTG` in a Shell. To look at:
1. zenixboot's lines: the format found, the two list offsets ("dl_rtg 0x... dl_chip 0x...").
2. While Zenix boots: is the C790's chipset picture on the HDMI (the text console)? Does it keep updating without
   AmigaOS (unicam.resource's capture keeps running by itself, or needed an interrupt)? If it freezes, the text
   consoles are not visible on the HDMI at all, with or without D43 - worth knowing either way.
3. `zdesk` (or a graphical login): the HDMI switches to the desktop at 1280 x 960; colours right (a format whose bytes
   we got wrong shows wrong colours: the format number is in the log); the AmigaOS pointer gone (section 3, step 3);
   the Zenix pointer follows the mouse.
4. Amiga+F2 and back: the HDMI flips to the text console and back to the desktop.
5. Leaving the desktop: the HDMI shows the text console.
6. How it feels: dragging a window, a full terminal scrolling (`ls -laR /` in a term) - the speed figures only the
   machine gives.
A failure in 3-5 leaves the machine usable over the serial cable or telnet; a reboot goes back to AmigaOS.

## 12. Later (not built)

On the Pi only, and only if the partner wants them: our own HVS display list (any size, a CLUT screen with the
palette in the list memory, the HVS's sprite as the pointer: no software pointer, 1-pixel steps), the mode chosen in
`deskprefs`; the desktop moving between the card and the chipset without a reboot. A real Zorro card's switch.

## 13. Questions for the partner (each with what is done meanwhile)

1. **The pointer's precision on a card**: today it moves in steps of 4 pixels at 1280 x 960 (the kernel's
   pointer is the chipset's 640 x 256, scaled). Exact steps need about 30 bytes of kernel code (a grab may name the
   pointer's bounds: `struct ucongrab` grows at its end), and the mount tests' floppy has 4 bytes of room beyond the
   800 kept. Meanwhile: scaled.
2. **Taking the front screen as it is** (the Workbench's own picture memory) versus zenixboot opening a screen of a
   size asked for (`RTG=1280x960`): built is the first, the simplest; the second is a few lines more in zenixboot.

## 14. The first run on the partner's PiStorm (2026-11-23, verified on the A1200)

From the Workbench (1280 x 960, 32 bits): `zenixboot CHECK RTG` printed "RTG: the front screen, 1280x960, format 9, 4
bytes a pixel, 5120 a row, the Pi's HDMI switched with the consoles" and "everything is loaded and checked". Then
`zenixboot RTG`: the Zenix desktop on the HDMI at 1280 x 960, the colours right, the Zenix pointer following the mouse and
no AmigaOS pointer; Amiga+F2..F10 switch to the text consoles as normal (the partner's monitor switches inputs by
itself, so which output showed the consoles was not seen). The pointer's 4-pixel steps (section 13 question 1) still to
be judged. The first zenixboot on the card predated D43 ("RTG: object not found"): zenixboot is never updated by a
bundle and was copied by hand (BOOT:zenixboot, the old one kept as zenixboot.old).
