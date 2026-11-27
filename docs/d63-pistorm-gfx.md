# D63: the PiStorm's own graphics - screen modes on the Pi's HDMI

The partner, 2026-11-27 (decisions.md "A native graphics driver for the PiStorm"): "We need to make graphics drivers
for the PiStorm, so that I can list and select screen modes." Zenix's own driver for the Raspberry Pi's display
hardware (the HVS, the firmware's mailbox, the monitor's EDID), so screenprefs (D62) lists and sets the PiStorm's real
screen modes live, as AmigaOS does with VideoCore.card, without relying on the Picasso96 screen AmigaOS left (D43
section 12's "later" item). Optional like genet: nothing in the base depends on the Pi. Revision 3, 2026-11-28,
branch `pigfx`, after two independent reviews (section 15) and the building they changed.

## 1. In plain words

- On the PiStorm the HDMI signal is fixed when Emu68 starts (the firmware's mode, from `config.txt`). AmigaOS still
  changes screen modes freely: VideoCore.card gives the Pi's **hardware video scaler (HVS)** a picture of the size and
  depth the screen wants, and the HVS scales it to the fixed output. Zenix does the same.
- **The display server draws the desktop on the Pi's picture itself**: it writes the HVS's *display list* (the few
  words that say where a picture is, its size, depth and scaling) into the HVS's list memory and the picture into
  Emu68's picture memory (`vc4-mem`), then switches the HDMI to it. The kernel asks the firmware once at boot for the
  HDMI's size and the monitor's EDID (section 4).
- **Any size from 320 x 256 up to the HDMI's, in 8, 16 or 32 bits**, switched live, no reboot. The list offered: a set
  of common sizes that fit, the HDMI's own size, and the monitor's modes from its EDID that fit.
- **screenprefs** shows them as a new row, "Pi screen" (setting word `pi`): Test switches at once and asks to keep it
  (back by itself after 10 seconds, D62's rule), Use for the session, Save for good. `screenprefs pi 1280x720x16`
  from a script; `screenprefs --list` lists them.
- **`/bin/pidisplay`**, a small program for people: `pidisplay --list` (the HDMI, the monitor, the modes),
  `--dump` (the display list the HDMI shows), `test 640x480x16` (a test picture for 5 seconds, from a text console),
  `console` (the HDMI back to the console's picture after a killed desktop).
- **Without a Pi**, or with `pi off` (the default), nothing changes: the chipset's desktop, or D43's card when
  `zenixboot RTG` handed one over.
- **The text consoles stay on the chipset** (through the C790 capture on the partner's machine), and Amiga+F*n* flips
  the HDMI between the desktop and the console as D43 does. A console drawn by the HVS is a later step (section 13).

## 2. What is read, and from where (no AmigaOS source; clean room as CLAUDE.md)

- **VideoCore.card** (Michal Schulz, github.com/michalsc/VideoCore.card, read 2026-11-27 at 99455de), the Picasso96
  driver Emu68 machines use: `vc6.c` (the Pi 4's HVS: a plane's display list words, scaled and unscaled, the pixel
  formats and orders, the palette, the nearest-neighbour kernel's 11 words, the wait for the vertical blank before a
  list is changed), `main.c` (the mailbox, `vc4-mem`, the display's size, the frame rate), `buddyalloc.c` (the list
  words it uses: 0-0x2FF, its palette 0x300-0x3FF). Facts and numbers only; Zenix's code is its own.
- **Emu68** (github.com/michalsc/Emu68, read at 9b4379a): `vc4-mem` reserved below 1 GB (`mmu.c`), mapped 1:1 for
  the 68k with `MMU_ATTR_WRITETHROUGH` (`start.c`), whose MAIR attribute is 0xBB (`include/A64.h`): Normal memory,
  write-through, **read- and write-allocate**; the peripherals from 0xF2000000 (knowledge base
  `amiga/emu68-network-usb-rtg-hardware.md`).
- **Linux's vc4 driver** (`drivers/gpu/drm/vc4/vc4_regs.h`, `vc4_hvs.c`, v6.12): `SCALER_DISPLIST1` 0x24,
  `SCALER_DISPSTAT1` 0x58 (bits 31-30 the channel's mode: 2 run, 3 end of frame; 17-12 the frame count; 11-0 the
  line), the BCM2711's list memory at the HVS's offset 0x4000, 16 KB (4,096 words), the control word's bits, and the
  firmware's own list in the first 32 words (`HVS_BOOTLOADER_DLIST_END`).
- **The firmware's property interface** (the Raspberry Pi firmware wiki "Mailbox property interface"): tag 0x00040003
  (the physical display's width and height), 0x00030020 (an EDID block: in the block number; out the block number, a
  status, 128 bytes).
- **VESA E-EDID 1.4** (its public structure): the header, the checksum, the manufacturer, the established, standard
  and detailed timings, the monitor name descriptor.

## 3. Finding the Pi and its parts under Emu68

From the hardware table stage0 writes while AmigaOS lives (`kernel/hwinfo.h`, D19), and the kernel's record after it
(section 4):

| What | From | Note |
|---|---|---|
| a Pi 4 / CM4 under Emu68 | `flags` `HW_DT` and `HW_BCM2711` | a Pi 3 (VideoCore IV, its list memory at 0x2000) is no machine of ours: not taken |
| the HVS | `soc_base + 0x400000` (bus 0x7E400000) | `SCALER_DISPLIST1` +0x24, `SCALER_DISPSTAT1` +0x58, the list memory +0x4000 |
| the picture memory | `vc4_base`, `vc4_size` (`/emu68` `vc4-mem`, 16 MB by default) | below 1 GB, in no memory list AmigaOS gave Zenix |
| the HDMI's size, the EDID | `struct hwdisp` at `HWDISP_ADDR` (0x1100 + 440) | the kernel's, from the boot |
| D43's record | `fb` (`struct hwfb`) | when `zenixboot RTG` ran: `dl_chip`, the console's (the C790's) list |

One check for every user: `hvs_find()` in `display/server/hvslist.c` (displayd, screenprefs and pidisplay): a Pi 4
under Emu68, `vc4-mem` of at least 1 MB ending below 1 GB, the HDMI's size known and 320 x 256 to 4096 x 4096; else the
reason ("hvs: not a Raspberry Pi 4 under Emu68 (the hardware table: ...)"), and nothing is touched. The table is read
at 0x1100 directly: on the PiStorm no memory protection runs (Emu68 keeps no 68k MMU, knowledge base
`amiga/emu68-mmu.md`).

## 4. Memory: the mailbox, the HVS, the picture (MMU off and on)

**The mailbox at boot, by the kernel.** A mailbox message is read and written by the VideoCore in RAM, and the ARM's
data cache is not coherent with it: the message must be cleaned out of the cache before and dropped after, which only
supervisor code can do (the `cpushl`/`cinvl` Emu68 translates). `vc4-mem` does not help: Emu68 maps it read- and
write-allocate (section 2), so an answer read back there may be the request still in the cache. So the kernel, which
already talks to the mailbox at boot for the SD card (`kernel/emmc.c`, verified on the A1200 2026-10-23, its message
below 1 GB), asks two more tags there, once, on a Pi 4 whatever it boots from (`pi_mbox_init`, called by `blk_init`
before any disk: a PiStorm booting from a Gayle IDE drive never reaches the SD card's code): the display's size and the
EDID's block 0, the EDID's 40-word message in a block of its own below 1 GB on whole 64-byte lines (Emu68's `cinvl`
drops the ARM's whole line), freed after; the kernel log says "pi: hdmi WxH, answers N" (N: 1 the size, 2 the EDID, 3
both, 0 no answer). The answers go into `struct hwdisp` (136 bytes: width, height, flags
`HWD_SIZE` 1 and `HWD_EDID` 2, the 128 EDID bytes) right after the hardware table, in the same cleared 1 KB the boot
hands over (`HWDISP_ADDR`, 0x12B8-0x133F); not in `struct hwinfo`, so the kernel's own copy and `DRV_HWINFO` stay as
they are. Boot-only code (`KINIT`, freed when the idle loop starts); `mbox_call` now takes any value buffer (it used
to send 8 words whatever the tag's buffer: the clock rate's 3-word buffer is now sent with its own end tag; the builder
is `kernel/mbox.h`'s, tested on the PC, and refuses a message bigger than its buffer). The HDMI mode never changes
after boot, so nothing is asked again; nothing in user space touches the mailbox. The kernel's 590 bytes more of
`kernel.bin` are boot-only; the mount tests' RAM disk is 1 KB smaller for them (647 KB).

**Without an MMU** (every PiStorm) the 68k reaches the HVS's registers and list memory (device memory, uncached,
little-endian: each word byte-swapped, then a `nop`, Emu68's barrier, `hvs_rd`/`hvs_wr`) and `vc4-mem` directly.
`vc4-mem` is written write-through, so every pixel reaches memory for the HVS at once (D43 saw it: the desktop drawn
there on the A1200, colours right). Zenix maps nothing.

**With an MMU and full protection** (D60): no PiStorm has one. displayd, screenprefs and pidisplay then find no Pi
(`prot_full()`), as D43's card refuses ("not granted under full memory protection yet"); a later Emu68 with a 68k MMU
would need the HVS's window and `vc4-mem` granted to the display server (D60 stage 4's claims): said, not built.

**The list memory** (4,096 words). The firmware's list is in words 0-31; VideoCore.card used words 0-0x3FF (its
lists, the unicam (C790) list `dl_chip` points at, its palette at 0x300). Zenix uses only **words 0x800-0x9FF**, so the
C790's list and everything AmigaOS left stay intact and D43's flip to the console keeps working:

| Words | What |
|---|---|
| 0x800 | the blank list: one word, the end (0x80000000); the HDMI shows the HVS's background while a picture changes |
| 0x801, 0x802 | "ZCON" (0x5A434F4E) and the console's list: kept by the first desktop, so a later one (after a killed server) and `pidisplay console` know it. The VideoCore is not reset with the Amiga: the record and the register outlive a Ctrl-Amiga-Amiga, and after a reset from the desktop the next boot's picture relies on c790boot (or AmigaOS's VideoCore.card) writing the register again |
| 0x808-0x812 | the scaling kernel: nearest neighbour, 11 words, written with every list |
| 0x820-0x83F | list A (at most 19 words with its end) |
| 0x840-0x85F | list B |
| 0x900-0x9FF | the palette of an 8-bit picture: 256 words, 0xFFRRGGBB (Zenix uses entries 0-18) |

Every list written is read back word by word: a word that does not read back is "displayd: the HVS's list memory at
ADDR does not keep what is written" and the switch is refused (a wrong address seen at once, not as a dark screen).

**The picture** starts at `vc4_base`, one at a time (1920 x 1080 x 32 bits is 8.3 MB; two would not fit 16 MB). A mode
must fit in `vc4_size - 4096` (the last 4 KB left free: room for the pointer's picture later, section 13). `vc4-mem`
is in no memory pool, so nothing else is given it; AmigaOS's bitmaps there (D43's screen among them) are dead memory
once Zenix runs, and the display server writes the blank list before it draws over one that may be shown.

## 5. The display list: planes, depths, scaling

One plane, the picture, built as VideoCore.card's `vc6.c` builds its Picasso96 screen (in meaning, word for word):

| Depth | Zenix's pixel | HVS format, order | Picasso96's number (fb.c's table) |
|---|---|---|---|
| 8 bits | a pen, 0-18 | `PALETTE` 13, order 3, the palette's pointer word in the list | 1 (`CLUT`) |
| 16 bits | 5:6:5, low byte first | `RGB565` 4, order 2 (XRGB) | 4 (`R5G6B5PC`) |
| 32 bits | B, G, R, A in memory (0xAARRGGBB little-endian) | `RGBA8888` 7, order 2 (ARGB) | 9 (`B8G8R8A8`, the format D43 section 14 saw right on the A1200) |

24 bits is not offered (the partner asked for 8, 16 and 32; it saves nothing the 16-bit picture does not).

- **Size and place:** the picture W x H is scaled to the HDMI's OW x OH by one factor for both axes (square pixels,
  the larger of W/OW and H/OH as 16.16, `scale = 0x10000 * W / OW`, VideoCore.card's rule), centred; the scaled size
  (`dw x dh`) goes into the plane's "output size" word; the rest of the HDMI is the HVS's background (black).
- **Unity:** the HDMI's own size at 16 or 32 bits is the unscaled plane (8 words and the end: control with the unity
  bit, position, alpha, size, a word the HVS uses, address, a word the HVS uses, bytes a row). **8 bits always goes
  through the scaler**, at a factor of 1.0 too (as vc6.c: the unity plane has no word for the palette).
- **Scaled:** 17 words (18 with the palette's) and the end: control (valid, the word count, alpha and colour
  expansion, format, order; PPF both ways), position, alpha (fixed, opaque), the scaled size, the picture's size, a
  word the HVS uses, the address (0xC0000000 + the 68k address: the VideoCore's uncached alias of the first 1 GB), a
  word the HVS uses, the bytes a row, [the palette's address: 0xC0000000 + its byte offset in the list memory], the line
  buffer (0), the two PPF words (`scale << 8 | 0xC0000000 | 128`), a word the HVS uses, the kernel's offset four times.
- **Scaling:** the polyphase filter with the nearest-neighbour kernel: sharp pixels, as an Amiga picture should look;
  uneven where the factor is not whole. A smooth kernel or whole factors only are later, if wanted (section 14).
- **Bytes a row:** W x bytes a pixel, rounded up to 16 (VideoCore.card uses the exact width; the HVS reads any pitch:
  16 keeps rows on the bus's bursts and costs at most 15 bytes a row).
- The words are `hvs_list()`'s (`display/server/hvslist.c`), checked against values worked out by hand from vc6.c
  (section 11).

## 6. The mode list

`hvs_modes()` (pidisplay `--list`, screenprefs' row) offers each size that fits the HDMI (not wider, not taller) at
each depth that fits the memory:

1. **The common sizes:** 320 x 256, 640 x 480, 640 x 512, 800 x 600, 1024 x 768, 1280 x 720, 1280 x 960, 1280 x 1024,
   1440 x 900, 1600 x 1200, 1920 x 1080, 1920 x 1200, 2560 x 1440 (320 x 256 and 640 x 512 the Amiga's PAL lores and
   hires-lace sizes; the rest AmigaOS's usual RTG modes).
2. **The HDMI's own size** ("native").
3. **The monitor's sizes** from its EDID (a bad header or checksum: none, said): the detailed timings (the first is
   the monitor's preferred size, "preferred"), the standard and the established timings. Extension blocks (CEA video
   codes) are not read.

Sorted by area, then width; each once; at most 32 sizes. A size bigger than the HDMI is left out (the HVS could shrink
it, but "up to the output's size" is the rule); when the monitor's preferred size is not the HDMI's, `pidisplay --list`
says it (`preferred 1920x1200`: `config.txt` chose another HDMI mode). Any other size from 320 x 256 up is taken by
the command line (`screenprefs pi 1000x700x16`), not offered in the lists.

On a 1920 x 1080 HDMI with a 16 MB `vc4-mem`: 320x256, 640x480, 640x512, 800x600, 1024x768, 1280x720, 1280x960,
1440x900, 1280x1024, 1920x1080 (native), each at 8, 16 and 32 bits (1600x1200 and 1920x1200 are taller: left out), and
what the EDID adds that fits.

Sizes below 640 wide are offered as the partner asked; on them a window wider than the screen is made smaller and
kept on it, as on the chipset's screens (`win_screen`).

## 7. `/bin/pidisplay`

A plain program (`display/drv/pidisplay.c` with `hvslist.c`, -Os, 11 KB), for people; nothing depends on it. One
system's conventions (`--help`, `--version`, usage status 2, errors `pidisplay: object: reason`).

- `pidisplay --list` (`-l`), any user: `output 1920x1080 60` (the HDMI's size and frame rate, measured from
  `SCALER_DISPSTAT1`'s frame count against the Pi's 1 MHz system timer over two frames, at most 100 ms), `monitor NAME`,
  `preferred WxH` (only when it is not the HDMI's), `memory ADDR BYTES`, then a line per size: `mode 1280x720 8 16 32`
  (`native`, `preferred` after the depths).
- `pidisplay --dump [WORD]` (`-d`), any user: the display list the HDMI shows (or the one at WORD), a word a line,
  with the console's list kept. After `zenixboot RTG` it shows VideoCore.card's own list for that mode: a reference
  that hvslist.c's words can be compared with on the real machine (section 12, step 1).
- `pidisplay test WxHxBITS`, root, from a text console with no desktop running: bands of the eight primary colours in
  a white frame on the HDMI for 5 seconds (list A), then the register as it was. It checks the whole path (the list,
  the formats, the scaling) without the display server.
- `pidisplay console`, root: the console's list (kept at 0x801-0x802) into the register: the HDMI back to the text
  console after a display server was killed while the desktop was shown (a telnet session, or typed blind).

## 8. The display server and the desktop

displayd keeps D43's drawing exactly (the pens in fast RAM, the changed rectangles converted to the card, the pointer
drawn by the server); the Pi adds where the card is and how it changes.

- **At its start** (`screen_up`), in order: (1) `/etc/screen`'s `pi` line says a size (not `off`), `hvs_find` finds a
  Pi, and the list is written: the desktop is the Pi's picture at that size; (2) else D43's record (`zenixboot RTG`):
  the card AmigaOS left, as today; (3) else the chipset. A failure in (1) is one line ("displayd: the Pi's screen
  1280x720x16: REASON: the RTG screen" or "...: the chipset's screen") and the next way is taken. Before the first list:
  the channel must run (`SCALER_DISPSTAT1`'s mode 2 or 3), else "the HVS's channel 1 (the HDMI) is not running".
- **The console's picture:** D43's `dl_chip` when `zenixboot RTG` recorded it for this register; else what the register
  held when the first desktop started (the text console was in front then), kept at 0x801-0x802 unless it is 0 or one
  of Zenix's own words. Not known (nothing kept, the register held Zenix's own): the HDMI stays on the desktop at
  Amiga+F*n* and at the end, said once in the log, and `pidisplay console` has nothing to restore.
- **Showing it:** the new list is written into the one of A and B the desktop does not use now. Whenever the register
  shows anything but the console's picture (one of Zenix's lists, AmigaOS's RTG list, a desktop whose console is not
  known), the blank list is written first (its end word too) and the server waits until the HVS's frame count moves
  (at most 60 ms: the HVS has taken it, as vc6.c waits for the vertical blank), so no list is rewritten while the HVS
  reads it and no picture in `vc4-mem` is drawn over while shown; then the picture is drawn from the pens and the new
  list's offset written after that pass's conversion: the HDMI never shows a half-drawn picture or the old one at the
  new size. A list memory that refuses the words puts back what the register showed. While a text console is in
  front the register keeps the console's picture; the console flips (Amiga+F*n*, D43 5) write the console's list and
  the desktop's, and a list switched while a console was in front is shown only after its picture is drawn.
- **8 bits:** the pens are the picture's bytes as they are (the conversion is a copy; the pointer's three colours are
  palette entries 16-18), and displayd writes the 16 pens and the pointer's colours into the palette words at every
  palette change (`set_palette`: a theme, the pointer's colours), before the picture is shown. 16 and 32 bits: D43's
  table.
- **The depth is the picture's, not the desktop's:** programs still have 16 pens (`depth 4` in `DPY_HELLO` and
  `EV_SCREEN`, as on D43's card). 8 bits is the lightest (one byte a pixel: the least memory and the fastest
  conversion); 16 and 32 look the same with 16 pens. More pens on a card are later (section 13).
- **Live switch:** the request **`DPY_CARD` 22** (`display/proto.h`; `struct dpycard`, frozen, first size 16), any
  client of the desktop's session (as `DPY_MODE`): `a[0]` the struct (the client's memory), `a[1]` `DPYC_SET` 0 or
  `DPYC_GET` 1.

  ```
  struct dpycard {                /* 16 bytes, DPYCARD_FIRST */
      unsigned short size;        /* in: DPYCARD_FIRST or more */
      unsigned short flags;       /* out: DPYCF_LIVE 1 (SET changes the size now), DPYCF_PI 2 (the Pi's own picture) */
      unsigned short w, h;        /* SET in: the size; out: the size in effect */
      unsigned char bits, pad;    /* SET in: 8, 16 or 32; out: the picture's bits */
      unsigned short ow, oh;      /* out: the HDMI's size (0: not known, D43's card) */
      unsigned short pad2;
  };
  ```
  GET on a card: its size and bits (flags 0 on D43's card); on the chipset `-ENODEV`. SET on the Pi's picture:
  `-EBUSY` during a drag; the kernel's mouse records still queued taken first (D61: they are in the old size's
  units); a new pens' bitmap (`-ENOMEM`, nothing changes), the list written (`-E2BIG` a size the HDMI or the memory
  does not take, `-EIO` the list memory refused it, nothing changes), then: the damage list emptied, the pointer moved
  in proportion and its bounds the new size (D61's `struct umouse`), every client told (`EV_SCREEN`, the new size),
  the palette, the windows kept on the screen and drawn again (`win_screen`, as the chipset's mode switch), the old
  bitmap freed, the new list shown after the next conversion. `-ENODEV` on the chipset and on D43's card (its size
  counts from the next start). libdisplay: `dpy_card(struct dpycard *, int set)`.
- **At its end:** the console's list into the register (when known). D43's limit, narrower: a killed displayd leaves
  its picture (or, inside a switch, the blank list) on the HDMI; `pidisplay console` puts the console back; the
  consoles still work by keyboard, serial and telnet.
- `DPY_MODE` on the Pi's picture answers `DPYM_RTG` (as on D43's card): the chipset's modes do not apply.
- Sizes (measured, `make`, 2026-11-28; image bytes): displayd 77,800 -> 82,912 (+5.1 KB: the Pi's start, the switch,
  the palette words, hvslist.c's list words; `-DNO_RTG`'s none of it), on every machine with a full root; screenprefs
  49,816 -> 53,788 (+3.9 KB: the row, the command, the provider; the EDID reader and the mode list are pidisplay's,
  run only on a Pi); pidisplay 11.0 KB new, on the disk only; the kernel +590 bytes, boot-only. Splitting the Pi's
  part out of displayd (a module loaded on a Pi only) would need a loader of shared objects Zenix lacks: said, not
  done.

## 9. The files, the code

| File | What |
|---|---|
| `kernel/emmc.c` | `mbox_call` for any value buffer; `mbox_display()` at boot: the HDMI's size and the EDID into `struct hwdisp` |
| `kernel/hwinfo.h` | `struct hwdisp`, `HWDISP_ADDR`, `HWD_*`; `HWFB_PI` (a test's pretend Pi) |
| `display/server/hvslist.c`, `hvslist.h` | no system calls (built for the PC's test too): `hvs_find`, the EDID parser, the mode list, the geometry, the list's words, the console's word, the register access |
| `display/server/displayd.c` | the Pi's start, `DPY_CARD`, the list writes, the palette words; `fb.c`: the 8-bit format; `draw.c`: `clear_damage` |
| `display/lib/screenfile.c` | the `pi off\|WxHxBITS` line (`SF_PI`, `pw`, `ph`, `pbits` at the end of `struct dpyscreen`) |
| `display/prefs/screenprefs.c` | the provider, the row, the command (section 10) |
| `display/drv/pidisplay.c` | `/bin/pidisplay` (section 7); `docs/man/pidisplay.txt`, `docs/commands.md`, `docs/features.md` |
| `display/test/dpytest.c` | `-pi WxH` (a pretend Pi), `reg`, `dl N`, `card`, `cardset` |
| `rootfs/etc/screen` | one more commented line: `# pi off` |

Numbers claimed in docs/claims.md: `DPY_CARD` 22 with `DPYC_SET` 0, `DPYC_GET` 1, `DPYCF_LIVE` 1, `DPYCF_PI` 2,
`DPYCARD_FIRST` 16; `HWFB_PI` 4; `HWD_SIZE` 1, `HWD_EDID` 2. Frozen (docs/frozen-interfaces.md): those, `struct dpycard`,
`struct hwdisp` at `HWDISP_ADDR`, the list words 0x800-0x9FF and the "ZCON" record, the `pi` line of `/etc/screen`,
pidisplay's command line and its `--list` lines (new words only at a line's end, new lines of new kinds; readers skip
what they do not know). No system call, no message type of the kernel.

## 10. screenprefs: the provider and the row

D62 8.1's "how the Pi's driver plugs in", as it says: one more entry in `providers[]`, `{ "pi", pi_here, pi_modes }`.

- `pi_scan()`: on a Pi only (the hardware table's flags, or a test's pretend Pi), `/bin/pidisplay --list`'s lines
  (frozen; the EDID reader and the mode list stay in that program): one entry for each size and depth; `next` 0 when the desktop runs on the Pi's picture (`DPY_CARD` GET's `DPYCF_LIVE`), else 1 (from the desktop's
  next start).
- **The row "Pi screen"** (setting `pi`), shown on a Pi: `off`, then the entries ("1280 x 720, 16 bits", "(next
  start)" when not live). Test/Use: `DPY_CARD` SET when live, else kept for Save. A change of it is a mode change:
  "Keep this mode?" for 10 seconds, then `DPY_CARD` SET back to the size before (D62 8's fall-back, unchanged; the
  HDMI's timing never changes, so a picture the monitor cannot show is impossible: the keep test guards against a list
  of Zenix's own that shows nothing, and keeps D62's rule the same everywhere). `off` is never live (the desktop
  changes kind): "from the desktop's next start". Save: root's, the line in `/etc/screen`; Test and Use work for any
  user of the desktop, as its other rows (a user's Save says the Pi screen is root's). A fall-back that meets a drag
  waits for it (up to 5 s) rather than leave the untested size.
- **The `rtg` row** ("Card size", zenixboot's `RTGMODE` reminder) is shown on D43's card or when `/etc/screen` has an
  `rtg` line, not on the Pi's picture: the precedence is the Pi's picture's (section 8).
- **The command line:** `screenprefs [-s] [-y] pi off|WIDTHxHEIGHTxBITS` (live when the desktop runs on the Pi's
  picture; else `-s` saves it; any size from 320 x 256 to the HDMI's, not only those listed), `--list` adds `pi off`
  and `pi 1280x720x16 (live|next start)` lines with `*` at the one in effect, `-q` ends with `, pi ...` on a Pi only (its
  line elsewhere unchanged).

## 11. Tests (no emulator has a Pi)

QEMU's `raspi4b` machine answers some of the firmware's mailbox tags, but models no HVS or display list, and runs ARM
code, not a 68k under Emu68: nothing in it would check this driver. So:

- **`tools/hvs-host-test.sh`** (the PC, AddressSanitizer), `hvslist.c` compiled for the PC: the list words for every
  depth against hand-worked values (1920x1080x32 unity; 640x480x16 on 1920x1080: scale 0x71C7, 1440 x 1080 at x 240;
  320x256x8 with the palette's word; 1280x1024x16 on 1280x720; 1280x720x8 at the HDMI's size through the scaler); the
  kernel's 11 words; the palette words; the bytes a row; the geometry of 600 random sizes on three HDMIs (inside,
  centred, filling one way, square pixels, never shrunk); the mode list for three HDMIs and two memories (the sizes,
  sorted, once each, the memory rule, the cap, native and preferred); two EDID blocks made by hand (a 1920 x 1080
  monitor with a name, a range descriptor and standard and established timings; one with only standard and
  established timings and a 13-letter name) and broken ones (checksum, header, short); the sizes' parser; `hvs_find`
  (each refusal), `hvs_put` (every word read back, nothing written outside Zenix's words), the console's record,
  the channel's mode; the kernel's mailbox message (`kernel/mbox.h`: the display size's, the clock rate's 3-word one,
  the EDID's 40 words, a message too big refused). 3,814 checks.
- **`tools/rtg-host-test.sh`**: the 8-bit format's conversion (the pens as bytes, the pointer's colours 16-18).
- **`tools/rtg-test.sh pi`** (FS-UAE, the stock A1200 with 8 MB fast): `dpytest -pi 800x600` writes a pretend Pi's
  record (`HWFB_TEST | HWFB_PI`, its memory's size); displayd makes the HVS's window (32 KB, its register holding 0x2E8
  as AmigaOS's C790 list would) and 2 MB of `vc4-mem` in fast RAM. Checked: `pi off`: the chipset's desktop;
  screenprefs' list (off and 12 entries, from the next start); not live without the desktop; a size too big refused;
  `-s pi 640x480x16` written; the desktop starting on it: the hello's size, the log, `DPY_CARD` GET, list A shown only
  after the picture was drawn, the blank list and the console's 0x2E8 kept, list A's words, the pens' colours (16 bits);
  screenprefs live; `pi 800x600x8`: the new size to the clients, list B, its words, the palette words, the pens as
  bytes; `pi 800x600x32`: the unity plane in list A, the bytes B, G, R, A; a size too tall refused; the fall-back left
  unanswered goes back after 10 s; the next desktop on the saved size. 16 checks, passed (2026-11-28).
- **Not tested** (no emulator can): the mailbox itself, the cache, the real HVS reading the words, its frame count
  (the pretend Pi has no timer: the blank-then-wait order runs without the wait), the channel's mode check,
  `pidisplay test` and `console` (they need a real HVS), a desktop starting at 8 bits, the console's list unknown, the
  fall-back from the Pi's picture to D43's card or the chipset after a failure, `-EBUSY` and `-ENOMEM` of `DPY_CARD`,
  the window face's Pi row (the command line's is). `pidisplay --list` runs on the pretend Pi (screenprefs reads it).
- Run unchanged (tools/check.sh): rtg-test (32, 16), screenprefs-test, dpy-test, zdesk-test, mouse-test, mmu-test,
  cmds2-test (`pidisplay --help`, `--version`), genet-test, emmc-test, stage0-test, the host tests.

## 12. On the A1200 (the partner's checklist; nothing here has run on a Pi)

Open a telnet session from the PC first (the way back if the HDMI goes dark). On the PiStorm, from a text console:

0. This build changed the kernel's mailbox message for the SD card's clock (section 4). If Zenix no longer starts from
   the SD card, boot the old kernel (zenix.old) and send the boot log.
1. `dmesg | grep hdmi`: "pi: hdmi WxH, answers 3" (the HDMI's size; 3: the size and the EDID). `pidisplay --list`: the
   HDMI's size and frame rate, the monitor's name, the modes. If Zenix was started with `zenixboot RTG` from the
   Workbench, `pidisplay --dump` shows VideoCore.card's own list: send it with the lines above.
2. `sudo pidisplay test 1280x720x16`: for 5 seconds colour bands in a white frame, black bars left and right, then the
   console again. Again with the native size `--list` shows, at 32 bits (`test WxHx32`), and `test 640x480x8`. Note what looks wrong (colours, a stretched or
   shifted picture, nothing).
3. `sudo screenprefs -s pi 1280x720x16`, then start the desktop (`zdesk`): the desktop at 1280 x 720, scaled to the full
   height, colours right, the pointer following the mouse. A dark screen: Amiga+F2 (or the telnet session),
   `sudo screenprefs -s pi off`, `sudo pidisplay console`, and `dmesg | grep -i 'pi.s screen\|hvs'`.
4. In the desktop, screenprefs (Zenix menu, Preferences, Screen): the "Pi screen" row lists the modes; choose 640x480,
   8 bits, Test: the desktop switches at once; Keep. Again with the native size, 32 bits.
5. Choose 800x600 and Test but answer nothing: after 10 seconds it goes back by itself.
6. Amiga+F2 and back: the HDMI flips to the text console (the C790's picture) and back to the desktop. (If the monitor
   gets the chipset's picture another way and switches inputs by itself, as D43 section 14 saw, say what it does.)
7. Leave the desktop: the HDMI shows the text console.
8. How it feels at the native size at 32 bits (dragging a window, `ls -laR /` in a terminal) against 8 bits.

Every step leaves the machine usable by keyboard, the serial line and telnet; a reboot goes back to AmigaOS.

## 13. Later (not built)

The text console on the HVS (the kernel drawing into a chunky picture: a console of its own, D28's rules); the HVS's
sprite as the pointer (single-pixel steps without redrawing, in the last 4 KB of `vc4-mem`); a smooth or whole-number
scaling kernel; more than 16 pens on a card (D55's screens at 8 bits with their own palette); the desktop moving
between the chipset and the Pi's picture live (`pi off` and on without a new desktop); the CEA extension's modes; a
second HDMI.

## 14. Questions for the partner (each with what is done meanwhile)

1. **The default:** built is `pi off` unless a size is saved, so the machine boots as today. Should the Pi's picture be
   the default on a Pi once it has been seen working (at the HDMI's size, 32 bits)?
2. **Scaling:** square pixels, centred, sharp (nearest neighbour). Some prefer the picture stretched to fill the
   screen, or whole factors only (no uneven pixels). Built: the first.

## 15. The reviews

**The first (independent, on revision 1, 2026-11-27; taken):** the mailbox message in `vc4-mem` would read back stale
(Emu68 maps it read- and write-allocate), and user mode cannot clean the cache: the kernel asks at boot instead
(section 4), which also removed the lock in `vc4-mem` (AmigaOS's leftovers could have looked "taken"). The console's
list was lost after a restarted server (it would have written 0, the firmware's list): kept at 0x801-0x802. A list
could be rewritten while the HVS still read it: the blank list and a wait for the frame count first. 8 bits never on
the unity plane; the scaled size in the output word; list A's length counted with its end. The driver program spawned
for every switch, with a frozen answer line between two programs that ship together: displayd writes the lists
itself, and `pidisplay` is for people (`--list`, `--dump` against VideoCore.card's real list, `test`, `console`). The
live switch: the mouse's queued records first, the pointer in proportion, the damage list emptied, no `use_blit` test
(always 0 on a card), nothing written to the register while a console is in front. A check that the channel runs, and
a refusal for an HDMI of no size. The `rtg` row hidden on the Pi's picture. Left as asked by the partner: the 320 x 256
minimum (the review proposed 640 x 256); the EDID's standard and established timings kept (about 600 bytes, already
tested). Found with it while building: three test scripts (rtg-test, screenprefs-test, ttf-test) checked `$?` after a
message holding a `$(...)`, which set it: those checks could not fail; each now takes the status first.

**The second (independent, on revision 2, 2026-11-28; taken):** the HDMI's size was asked only in the SD card's code,
which a PiStorm booting from IDE never runs: the mailbox is set up by `blk_init` before any disk. The kernel said
nothing of its answers: a log line. The EDID's buffer on whole 64-byte lines (Emu68's `cinvl` drops the ARM's whole
line); the message builder in a header the PC tests, refusing a message bigger than its buffer. A failed first list
after the blank list left the HDMI on the background: what was shown is put back; the blank list's word written before
the register points at it; the blank whenever anything but the console's picture is shown (also a desktop whose
console is not known, and AmigaOS's RTG list); a list switched while a console was in front not shown before its
picture. The window face's fall-back left the Pi row on the refused size; a user's Save said nothing of the Pi row; a
fall-back during a drag waits for it. `pidisplay test` after a killed desktop would have rewritten the list shown and
"restored" it: refused (`pidisplay console` first), and its 5 seconds no longer cut by a signal. D43's path took a CLUT
record (now fb.c knows CLUT): refused there. screenprefs runs `pidisplay --list` instead of linking the EDID reader and
the mode list. `dpytest -pi` refuses to overwrite a real `zenixboot RTG` record. Stale comments, `mode_file` read once,
the frame rate down to 24 Hz, one-system wording, the checklist's sizes (the native one `--list` shows, not 1920 x 1080)
and a step 0 for the changed SD message. Left: the 320 x 256 minimum (the partner's), the EDID's timings (tested).
