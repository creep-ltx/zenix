<!-- SPDX-License-Identifier: GPL-2.0-only -->
# D35: the installer (design note)

Status: **being built on branch install (2026-11-01), the last feature of block 4 (ROADMAP "The order of work", item 4). The outline is
the owner's ("an installer: partition, format, copy, boot menu"; a boot floppy or an existing Zenix; no PC); the details were delegated and
are decided here. Tested on image files and in FS-UAE only: it has never run on a real disk, and nothing here touches the owner's card.**
What was measured is in section 10.

## 1. What it is
`zinstall` runs on Zenix itself, from the install floppy or from an existing Zenix, and puts Zenix on a hard disk, CF or SD card: pick a
disk (its size and what is on it), pick where (free space by default), pick the root's filesystem, see exactly what will change, confirm,
partitions, format, copy, check, make it bootable, say what to do next. A plain numbered text dialogue of 80 columns, one question at a
time, read as whole lines through the console, so every keymap the console has works and it also runs over a serial line. It asks for digits
and short words only (nothing behind Alt).

**The worst bug this project could ship is an installer that destroys an AmigaOS partition.** Everything below is shaped by that.

## 2. The rules that keep other partitions safe
1. **A write gate.** Table sectors are only written through one function, from a list made in advance (`struct t_plan`): the sector's
   number, its new bytes, and the bytes it must hold now. It re-reads the sector and refuses if it differs; it reads back what it wrote.
   Everything else goes through diskd's partition handles, which confine it to that partition's sectors (diskd refuses the rest).
2. **The default is free space.** An existing partition is touched only if the person chose exactly that one, after seeing its name, size
   and type, and typed its name. Never selectable, whatever is typed: a FAT partition (the Pi's Emu68 and Kickstart live there), a type 0x76
   container as a whole, a mounted partition, an extended or GPT entry, anything in a table that is damaged.
3. **Erasing a whole disk** takes typing `ERASE ide0`, and is refused outright on a disk with a FAT partition or anything mounted.
4. **Show, then re-read.** The plan names every change in words. Right before the first write the tables are read again and compared by
   SHA-256 over every sector that was read to make the plan (the disk's size too); anything different and it stops, having written nothing.
5. **Unknown is refused.** A table that does not add up (bad checksums, loops, overlaps, a partition past the disk's end, more partitions
   or chain blocks than it can hold, an MBR that lies outside the disk, a disk with both an RDB and an MBR, GPT) gets no free-space install.
   Damaged tables are shown with the reason. It reuses `user/rdb.h`, the reader the disk server and fdisk share, and fuzzes all of it.
6. **Bootable last.** New partitions enter the table first as "do not mount" and not bootable: AmigaOS and Kickstart ignore them. Only
   when the root and the boot partition are written and checked does the one last sector write make the boot partition bootable. A cut at
   any point before that leaves the machine starting exactly as before.
7. **Nothing not chosen is written.** Checked by the tests after every install: every partition not chosen is byte-identical.

## 3. The layouts it knows
| The disk | What it offers | What it changes in the table |
|---|---|---|
| RDB (real IDE/CF), free cylinders | new ZB0, ZR0, ZH0 in the free cylinders, in the RDSK block's geometry | new PART blocks in unused blocks of the RDB area; then one write: the last PART's `next` (or RDSK's list if empty). RDSK's HighRDSKBlock first when needed, harmless alone |
| MBR with a free slot and free space (the Emu68 card's kind, or a plain MBR) | a new type 0x76 partition with an RDB of its own inside, holding ZB0, ZR0, ZH0 | that RDB, written in the free space where nothing refers to it; then one write: the MBR sector. No existing RDB is touched |
| MBR whose 0x76 RDB has free cylinders | as for an RDB, inside the container | as for an RDB |
| A chosen RDB partition | ZB0, ZR0, ZH0 inside its sectors | new PART blocks; one write: the chosen partition's PART block becomes the first new one, linked on to the rest and to what followed it |
| A chosen MBR partition (never FAT, never 0x76) | a 0x76 with an RDB inside, in its sectors | one write: the MBR sector, its type byte |
| Whole disk, or nothing on it | an RDB, ZB0, ZR0, ZH0 | PART blocks where the old table does not need them, RDSK last, one write (section 4) |
| GPT, both RDB and MBR, a partition table that does not add up | nothing but the reason (a damaged RDB or MBR: only the erase of the whole disk, with its warning) | - |

**A disk with a PC table (MBR) only boots by itself under Emu68**, whose SD driver reads type 0x76 (knowledge base `amiga/emu68-and-pistorm.md`,
read from the driver's strings; the owner's card boots that way, one 0x76, verified on the A1200 2026-10-24). Kickstart's own `scsi.device` reads an RDB and nothing else. So on an MBR disk the plan says: under Emu68 it
starts by itself; on any other Amiga, start it from the boot floppy (`zenix.adf`, or `zenix-zfs.adf` for a ZenixFS root). A second 0x76
partition next to the first has not been seen working on Emu68: not known, said so.

Sizes: ZB0 (FFS, DOS\1) 16 MB from 256 MB of free space up, 8 MB from 64 MB up, else 4 MB. ZR0: a third of what is left, 128 MB to 1 GB;
under 64 MB of space the rest and no /home. ZH0 (ext2, `/home`): what is left, at most 4 GB. At least 28 MB. The person may use less than
all the free space. New labels avoid names already on the disk (ZB1, ZR1...).

## 4. A cut at any moment
Every table change is arranged so that one sector write turns the old table into the new one; the new blocks before it are in places
nothing refers to. **Where it is not whole:**
- **Erasing a whole disk:** the table swap itself is one sector write, with PART blocks put where neither the old RDB's chains nor the old
  partitions are; if no such blocks exist in the first cylinder, the plan says the old table can be damaged before the new one is whole. The
  data of the old partitions is the thing being erased from the first write of the filesystems on.
- **A chosen partition:** the old table stays whole until its one write, but the contents of the chosen partition are overwritten
  in its first sectors before it (MBR case): that partition is gone from the first write, which is why it was chosen by typing its name.
- **One sector torn by the drive.** A 512-byte write that a power cut leaves half done is outside what software can arrange. The tests
  tear each commit sector at every 4 bytes: an RDB block then fails its checksum, or is the old or the new one; such a block is refused,
  by zinstall, fdisk and AmigaOS; the other partitions' data is not touched. The table copy (section 7) and any RDB tool put it back.
  An MBR has no checksum; a torn entry is checked as any other, and the tests show every tear gives an old, a new, or a refused table.

## 5. Where the files come from
**The install floppy set.** `make install-floppies` makes `zenix-install-1.adf`, `-2`, `-3`... Disk 1 boots: the bootblock, the kernel, a RAM
disk with `zinstall` and what it runs, then the first part of the set. The set is one stream of package bundles (D22's format, verified part by part) laid across
the disks after a small header sector (`ZSET`, set id, disk number). The kernel on disk 1 is the one the set installs, so it is not stored twice. `/etc/zinstall.set`
in the RAM disk lists every item with its place and SHA-256; a floppy's content is trusted because the boot floppy says what it must be. Other
drives are looked at too, so with several drives (or a Gotek) nothing needs swapping; with one drive it asks for the next disk. Core
programs come first, optional ones on the last disk; the person may skip those (then `zpkg -S` later). **A ZenixFS root needs the second
disk for its kernel** (kernel-zfs).

**The network, from an existing Zenix.** If `/etc/zinstall.set` is not there (an existing Zenix) and zpkg has a server configured, `zinstall` asks `zpkg
--fetch DIR` (new, section 8) to bring the signed list's system bundle and packages into the new root's `/var/cache/zpkg/pkg`, checked as
always; then it installs from them the same way. There is no network on the RAM disk: `netd`, its driver and `zpkg` take about 200 KB it
does not have. After a floppy install `zpkg -Syu` brings anything newer.

Both give the same machine: every package's manifest in `/var/lib/zpkg`, so `zpkg` and updates know what is installed.

**The source of the two AmigaOS programs on the boot partition** (`bootmenu`, `zenixboot`): the package `zinstall` carries them in
`/usr/lib/zinstall/`, always installed. So an installed Zenix can also rebuild its boot partition's files, and no separate way exists to get them.

## 6. Dual boot, and the way back
The boot partition gets a boot priority one above the highest on the disk, with `bootmenu` as its only startup. On a disk with AmigaOS the
menu's default is **AmigaOS**, after 5 seconds: the machine goes on starting AmigaOS unless someone presses a key. `bootmenu` writes
nothing to disk. Other ways back, which the program prints at the end:
- At power-on, both mouse buttons: Kickstart's Early Startup menu chooses a partition by hand.
- From AmigaOS's HDToolBox or Zenix's `fdisk del`, remove ZB0, ZR0, ZH0: the disk is as before (the free space then holds old data).
- `/var/lib/zinstall/table-before` in the new root holds every table sector as it was, with its number.

## 7. What it writes where, in order
1. Stage 1 of the table (section 3), each step flushed. Rescan; find the new devices by their exact start and size; refuse if not found.
2. `mkfs.ext2` or `mkfs.zenixfs` on the root, `mkfs.ext2` on `/home`; mount; unpack (each file against its manifest's SHA-256).
3. `/etc/fstab` gets the `/home` line, `/tmp`, `/home`, `/mnt` their places, the table copy; unmount, mount again, every file read back
   against its SHA-256 from the disk; `fsck` read-only.
4. The boot partition: an FFS volume written by the program's own small writer (`user/zinstall/ffsw.c`): `S/Startup-Sequence`, `S/BootMenu.conf`, `bootmenu`,
   `bootzenix`, `zenixboot`, `Zenix/zenix`; read back. A ZenixFS root gets kernel-zfs as `Zenix/zenix`.
5. Stage 2: the root's PART marked bootable (Zenix's rule for which root to take, `user/rootpick.h`), then ZB0 bootable with its priority.

If a step fails, it stops and says what is done; the table has only unbootable, unmounted partitions that fdisk removes.

## 8. Shared things touched
- `tools/mkzup.py`: a family `zinstall`. `user/rdb.h`: `RDB_MAXUSED` may be raised by a program that includes it (a PFS3 handler is 121 LSEG blocks; 128 was the limit: the owner's card fits with no block to spare; diskd keeps 128). `user/zpkg.c`: `zpkg --fetch DIR [--zfs]`, growth only. Makefile: the program, its RAM disk, the floppies. No system call, message, or number claimed.
- The floppy set's format is not frozen: disk 1's own program reads it, always of the same build.

## 9. What it does not do
No resizing of partitions; no GPT; no install from a folder; no network from the floppy; never changes an MBR's active flag; one disk at a time (Zenix serves one IDE disk).
