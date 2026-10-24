# First boot on a PiStorm A1200 (Raspberry Pi 4 / CM4 under Emu68)

Nothing here has run on a real Pi yet; this is the plan for the first test and the list of what to look at. Everything
Zenix prints while it starts is also kept in the kernel log (`dmesg`) and shown on the screen during boot, so no serial
cable is needed.

## Boot it
1. `make build/zenix-ram.adf` (the RAM-disk image: kernel plus a small root filesystem that the boot block loads into
   RAM, so no disk is needed to get a shell). Put `build/zenix-ram.adf` where the Gotek / GoDrive reads it.
2. Boot the Amiga from that floppy image. Zenix needs a 68020 or better (Emu68 counts); fast RAM is
   recommended (with 1 MB of chip RAM and nothing else there is room for one login).
3. The screen (all text, no bar: 32 rows on PAL, 25 on NTSC) shows the kernel's boot messages, then on console 1 the
   autologin warning and the shell prompt (the image logs root in on console 1 by itself, `/etc/autologin`). If they
   scrolled by: `dmesg`, or Shift+Up. Amiga+F2 .. Amiga+F10 show consoles 2-10, each with its own `zenix login:`
   prompt (log in as `root`, no password); Amiga+F1 goes back.
   The floppy LED should go off before the kernel's messages (the bootblock stops the motor). Before the text
   appears the whole screen changes colour at every boot step (next section): if it stops on a colour, that colour
   says where.

## Booting from the SD card without a floppy
Instead of the floppy, the SD card can carry Zenix: Kickstart boots a small AmigaDOS partition on the card, whose
start-up script runs `zenixboot` (man/zenixboot.txt), and Zenix uses the ext2 partition on the same card as its root.
The card: p1 FAT32 with Emu68's files, p2 (type 0x76) the 16 MB boot partition, p3 (type 0x83) the Zenix root, 512 MB.
Use the Zenix test card (or another spare card): **everything on the card after p1 is replaced**.
1. On the PC, in the Zenix folder, with the card in the reader (here `/dev/sda`; `lsblk` shows the name):
   `sudo bash tools/write-card.sh /dev/sda`
   This keeps the card's Emu68 partition (the test card has it at the right place) and writes the rest. For a new
   card, give the folder with the Emu68 files (config.txt, start4.elf, the Emu68 file, kick.rom) as well:
   `sudo bash tools/write-card.sh /dev/sda <emu68-dir>` - that erases the whole card first. The script refuses a disk
   that is not removable or not 1-64 GB, shows the card and waits 5 seconds (Ctrl-C stops it), and checks what it
   wrote.
2. Put the card in the Amiga, take any disk out of the Gotek, and switch on. No key is needed.
3. Kickstart's boot screen, then the whole screen blue (zenixboot reads the kernel), cyan, magenta, and from there on
   the same colours and boot log as from the floppy. If zenixboot finds a problem it writes it on the AmigaOS screen
   and stops there (a `1>` prompt).
Not yet tried on the PiStorm: whether Emu68's SD driver boots this partition is the first thing this checks (if
Kickstart shows its insert-disk picture instead, it did not).

## If it freezes: screen colours, then the previous-boot log
No serial cable is needed. Until the display is up, every boot step paints the **whole screen** one colour; a
freeze leaves the colour of the last step reached. (Before the first Zenix colour, Kickstart's own colours come and
go: dark and light grey at power-on, and its insert-disk picture. A Zenix colour only appears once the floppy has
started loading.)

| Screen | Where it stopped | What it likely means |
|---|---|---|
| **blue** | 1. bootblock: loading the kernel from the floppy (or zenixboot reading its files) | The floppy read hangs (Gotek, image) or takes very long. |
| **cyan** (light blue-green) | 2. kernel loaded; stage0 reads the Pi's tables, the memory list is collected | stage0 or Emu68's device tree (a guru here means the OS was still alive: stage0). |
| **magenta** (purple-pink) | 3. AmigaOS switched off; the kernel is copied to its home | The copy or the jump to the kernel (cache flush, A2). |
| **yellow** | 4. the kernel runs, relocating itself | Relocation, or the cache flush after it. |
| **orange** | 5. relocated; about to start the kernel's C code | The start of `kmain`, the kernel stack. |
| **green** | 6. log and serial port, hardware tables, ROM font | Printing the Pi / Zorro tables, the ROM font. |
| **brown** | 7. exception vectors and the memory manager (self-test) | Memory map from AmigaOS, or the memory self-test. |
| **pink** | 8. memory is fine; switching the instruction cache on | The cache (A1: `cpusha`). |
| **white** | 9. starting the display | Copper list, bitplane, chip RAM for the screen. |
| black with grey text | 10. the display is up | From here **the text** says where it is: the boot log is on the screen. |
| **grey** (after one of the above) | no display: no ROM font found or no chip RAM | Zenix carries on, but only on the serial port. |
| **the whole screen red** | the bootblock failed | No memory, or a floppy read error. |
| **a colour on top, red below** | the kernel stopped on an error at that step (a panic, an exception in the kernel) | The step is the top colour. Orange over red is also "this CPU is a 68000". |
| **white text on dark red** | a kernel panic after the display was up | The last lines on the screen say why. |

**After a freeze or a panic, do this:**
1. Take a photo of the screen (the whole screen: the colour, or the text).
2. Press **Ctrl + Left Amiga + Right Amiga** together (a warm reset). The Gotek still holds the floppy image, so
   Zenix boots again.
3. When the text appears, look for the lines between `=== previous boot (N) ended here ...` and
   `=== end of the previous boot's log ===`: the last 20 lines the frozen boot wrote, each marked `> `. Take a photo
   of them (Shift+Up scrolls back if they went by). The first line also says whether it ended with a kernel panic.
4. At the prompt, `dmesg -p` shows the whole previous log, `dmesg` this boot's.

The log lives in the top 16.5 KB of chip RAM, which a warm reset should not clear. Whether that is true on the
PiStorm is **not yet known**: if the next boot says `klog: no log from a previous boot (power-on, or the memory
was cleared)` right after a warm reset, chip RAM did not survive it there; please report that line. After a
power-off the log is always gone. `klog: ... damaged (checksum)` means part of it was overwritten.

## What the boot messages should say
| Line | Means |
|---|---|
| `cpu: 68040 (attnflags f)` (or similar) | Emu68 reports a 68040-class CPU. |
| `pi: Emu68, BCM2711 (Pi 4/CM4)[, cache range opcodes], peripherals at 0xf2000000` | The device tree was read. If this says "not running under Emu68", stage0 did not find `devicetree.resource`. |
| `pi: emmc ... mailbox ... gpio ... genet ... gic ...` | The addresses the drivers will use. Expect emmc near 0xf234xxxx, mailbox near 0xf200b880, gpio 0xf2200000. |
| `zorro: N: manufacturer 6d73 product 33 ...` | Emu68's own boards (and RAM) as AmigaOS configured them. |
| `emmc: controller at ..., mailbox at ...` then `base clock`, `SD clock`, `card ready`, `card address`, `sectors`, `sector 0: ...` (`MBR partition table`, `Amiga RDB` or `unknown (first bytes ...)`) | The SD card driver, stage by stage. **The last line that appears shows how far it got.** |
| `ide: no drive found on the A1200/A600 port` | Expected: there is no IDE disk. Zenix read the Gayle ID first (0xD on an A1200; `ide: no A600/A1200 Gayle (Gayle id N)` would mean it saw something else, and then no IDE port is touched). Usually the status reads 0xFF and this comes at once; if the empty port reads 0 instead, a line `ide: ... status 0, waiting up to 10 s for a drive to start` comes first and the boot waits those 10 s (a slow disk may need them; please note whether it happens). Zenix then tries the SD card, then the A3000's SCSI. Gayle's interrupt sources were switched off first; `ide: stray IDE interrupt` later would mean they were not. |
| `kbd: reset warning ...` (in the previous boot's lines after Ctrl+Amiga+Amiga) | Your keyboard sends the reset warning: Zenix synced the disks before the reset (`... synced in N ms`). No such line is fine: not every keyboard sends it (Zenix does nothing then). |
| `ext2: 1 KB blocks, 704 blocks, 128 inodes` | The root filesystem came from the RAM disk (the SD card is not the root). |

## Then look at the disk
- `fdisk list` shows `ide0` (the whole SD card: its size in sectors) and its partitions. A normal Emu68 card has a
  FAT boot partition (listed as "other") and perhaps more.
- **Do not run `mkfs.ext2`, `fdisk init/add/del` or any write command on `ide0` or its partitions.** The card holds
  Emu68 and the Kickstart ROM. Zenix itself writes nothing to the card at boot: the root is the RAM disk, and the SD
  init only reads (it power-cycles the card, resets the controller and reads sector 0).
- `dmesg | grep emmc` prints just the SD driver's lines.

## What to report back
A photo or the text of `dmesg`, plus: did the prompt appear, did `fdisk list` show the card, and the last `emmc:` line
if something failed. If the machine hangs before any text: the colour of the screen (table above), and after a
warm reset the photo of the "previous boot" lines and whether the `klog:` lines say the log survived.

## Known unverified (could fail on the first run)
The SD controller is written from the SD Host Controller specification and has only run against a model; clocks, the
mailbox calls, card detect and the cache handling around the mailbox message are the likely trouble spots (see
limits.md). Speed is the safe 25 MHz mode (about 12 MB/s at most).

Last checked against the code: 2026-10-23.
