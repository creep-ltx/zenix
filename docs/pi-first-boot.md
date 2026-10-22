# First boot on a PiStorm A1200 (Raspberry Pi 4 / CM4 under Emu68)

Nothing here has run on a real Pi yet; this is the plan for the first test and the list of what to look at. Everything
Zenix prints while it starts is also kept in the kernel log (`dmesg`) and shown on the screen during boot, so no serial
cable is needed.

## Boot it
1. `make build/zenix-ram.adf` (the RAM-disk image: kernel plus a small root filesystem that the boot block loads into
   RAM, so no disk is needed to get a shell). Put `build/zenix-ram.adf` where the Gotek / GoDrive reads it.
2. Boot the Amiga from that floppy image. Zenix needs a 68020 or better (Emu68 counts) and about 4 MB of fast RAM.
3. The screen shows the kernel's boot messages, then the shell prompt. If they scrolled by: `dmesg`, or Shift+Up.

## What the boot messages should say
| Line | Means |
|---|---|
| `cpu: 68040 (attnflags f)` (or similar) | Emu68 reports a 68040-class CPU. |
| `pi: Emu68, BCM2711 (Pi 4/CM4)[, cache range opcodes], peripherals at 0xf2000000` | The device tree was read. If this says "not running under Emu68", stage0 did not find `devicetree.resource`. |
| `pi: emmc ... mailbox ... gpio ... genet ... gic ...` | The addresses the drivers will use. Expect emmc near 0xf234xxxx, mailbox near 0xf200b880, gpio 0xf2200000. |
| `zorro: N: manufacturer 6d73 product 33 ...` | Emu68's own boards (and RAM) as AmigaOS configured them. |
| `emmc: controller at ..., mailbox at ...` then `base clock`, `SD clock`, `card ready`, `card address`, `sectors`, `sector 0 starts ...` | The SD card driver, stage by stage. **The last line that appears shows how far it got.** |
| `ide: no drive found` | Expected: there is no IDE disk. Zenix then tries the SD card, then the A3000's SCSI. |
| `ext2: 1 KB blocks, 768 blocks ...` | The root filesystem came from the RAM disk (the SD card is not the root). |

## Then look at the disk
- `fdisk list` shows `ide0` (the whole SD card: its size in sectors) and its partitions. A normal Emu68 card has a
  FAT boot partition (listed as "other") and perhaps more.
- **Do not run `mkfs.ext2`, `fdisk init/add/del` or any write command on `ide0` or its partitions.** The card holds
  Emu68 and the Kickstart ROM. Zenix itself writes nothing to the card at boot: the root is the RAM disk, and the SD
  init only reads (it power-cycles the card, resets the controller and reads sector 0).
- `dmesg | grep emmc` prints just the SD driver's lines.

## What to report back
A photo or the text of `dmesg`, plus: did the prompt appear, did `fdisk list` show the card, and the last `emmc:` line
if something failed. If the screen stays black or the machine hangs before any text, say at which point (Kickstart
screen, then black).

## Known unverified (could fail on the first run)
The SD controller is written from the SD Host Controller specification and has only run against a model; clocks, the
mailbox calls, card detect and the cache handling around the mailbox message are the likely trouble spots (see
limits.md). Speed is the safe 25 MHz mode (about 12 MB/s at most).
