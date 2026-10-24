# Zenix: closing report of the audit round (2026-10-22 to 2026-10-24)

The audit of 2026-10-22 (`audit-2026-10-22.md`) found the problems of two days of fast parallel building in an emulator.
This report closes that round: what was fixed, what is now proven on the owner's real machine, and what is left.
The details, item by item with commits, are in the audit file.

## In plain words
- **Everything on the audit list is fixed or deliberately left with a reason.** Six fix batches (A-F), two
  owner-approved additions (the first-boot diagnostics and booting without a floppy), one documentation round and one
  round of small fixes from the first real boots, each tested by its helper and again by the manager before merging.
- **Zenix runs on a real Amiga.** On the owner's A1200 (PiStorm32-lite, CM4, Emu68 1.0.7, Kickstart 3.2.3) it boots
  from the SD card with no floppy, runs its own SD driver, keeps its system and the owner's files on the card, and
  passed everything tried by hand on 2026-10-23 (list below).
- **The way of working changed:** one main session plus one helper at a time, a shared number registry, a feature halt
  lifted only by the owner item by item, `make check` as the gate, design notes before new work.

## Fixed, by batch (merged into main)
| Batch | What |
|---|---|
| A | First boot: cache handling for Emu68 and 040/060, the 68060 build check, keyboard timing, the Amiga Swedish keymap, safe image builds, boot tables in allocated memory, quiet Gayle interrupts, floppy motor off |
| A11/A12 | Boot stage colours; the previous boot's log in chip RAM, shown after a warm reset (`dmesg -p`) |
| B | Memory: a real allocator, NULL on failure, size caps, interrupts only queue input, no lost wakeups, no big stack frames, per-program stacks, a checking program loader |
| C | Disks: ext2 refuses or mounts read-only what it does not understand and survives damage, safe `fsck.ext2`, `fdisk` keeps AmigaOS partitions exactly, real `sync`, ZenixFS commit timing and block checks |
| D | Permissions as documented: kill rules, servers answer only the kernel and root, reserved port names, `su`, `tar`, temporary files, set-group-id directories |
| E | Ten Unix-style consoles with a login each (no desktop features in the text console), shell and command fixes, controlling tty, one path limit, the clock per PAL/NTSC, Unix error numbers |
| Docs | Every document checked against the code, `docs-check` counts and links, SPDX licence lines, no private paths, `make check` |
| F | Stack guard, `ls` of any size, the hardware hand-off fixes (keyboard reset warning saves the disks, Gayle ID detection and IDE timing, floppy writes and disk changes, clock, A3000); the floppy test back in the gate |
| zenixboot | Booting from a hard disk, CF or SD card with no floppy (owner-approved) |
| Small | Fixes from the first real boots: login echo, `fdisk` sizes, the SD sector-0 line, `ping` in microseconds, `bench` checks every call, SD writes wait for the card to finish programming, the serial copy no longer slows the screen when nobody listens, `termtest`'s box |

## Verified on the owner's real A1200 (2026-10-23)
Boot from the GoDRIVE (RAM disk) and from the SD card with no floppy; Kickstart 3.2.3 under Emu68 1.0.7; the boot
colours and the previous-boot log after Ctrl-Amiga-Amiga (read back through Wasabi too); the display (PAL, 80x32); the
Swedish keymap including the keypad; Tab completion, Ctrl-R; Amiga+F1-F10 consoles; `passwd` and `login`; files written
to the SD card surviving reboots; `fdisk list` (SD card and GoDRIVE); `free` with 1.9 GB of fast RAM; `bench`; `termtest`
(styles, cursor, scroll region, keys); `crash` containment; job control; zero-copy messages (`ping`); `less` and the editor.
Found there and fixed: the ROM font above 128 MB (the grey screen), the double login name, the small display issues.

## Left on purpose
- A7: a chip-only A1200 booting the RAM disk can overlap the kernel copy (not the owner's machine).
- `user/awk.c` parked (feature halt).
- Performance items to measure on real 68k hardware before changing: the data cache via transparent translation, SD DMA,
  waking only the waiting process, byte-by-byte copies, ext2's synchronous metadata writes. The PiStorm numbers are
  Emu68 numbers; real 68020/030 numbers are still needed.
- A few `-1` results left in the kernel's message calls (limits.md); the shell's listed gaps (man/sh.txt); ext2 directory
  moves between parents, triple-indirect blocks, journal.
- Why the SD driver's mailbox failed with the older Emu68 build / the everyday card (it works with 1.0.7 on the test card).
- Whether the A1200's keyboard sends the reset warning (batch F uses it when it comes).

## Next (approved by the owner)
1. **D10 priorities** (DESIGN.md D10, with QNX priority inheritance) - starts now.
2. **D11 service manager**, once its design note is complete and reviewed.
Then, with the owner's approval item by item: the card as a test channel (logs and a test runner on the A1200), updates
that keep the owner's settings, the serial cable and the network driver, and the kernel best-of list in future-ideas.md.

Last checked: 2026-10-24.
