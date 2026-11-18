<!-- SPDX-License-Identifier: GPL-2.0-only -->
# D35: the installer (design note)

Status: **built on branch install (2026-11-01 to 11-03), the last feature of block 4 (ROADMAP "The order of work", item 4). The outline is
the owner's ("an installer: partition, format, copy, boot menu"; a boot floppy or an existing Zenix; no PC); the details were delegated and
are decided here. Tested on image files and in FS-UAE, and run by the partner on the real A1200 (2026-11-03/04: the live tests below, a split install,
a ZenixFS root on the 1260's CF card); from the install floppies on a real drive: not yet. Nothing here touches a card by itself.**
What was measured is in section 11. Section 3b (set it up yourself, the owner 2026-11-04) was built on branch inst2, 2026-11-05.

## 1. What it is
`zinstall` runs on Zenix itself, from the install floppy or from an existing Zenix, and puts Zenix on a hard disk, CF or SD card: pick a
disk (its size and what is on it), pick where (free space by default), pick the root's filesystem, see exactly what will change, confirm,
partitions, format, copy, check, make it bootable, say what to do next. A plain numbered text dialogue of 80 columns, one question at a
time, read as whole lines through the console, so every keymap the console has works and it also runs over a serial line. It asks for digits
and short words only (nothing behind Alt).

**The worst bug this project could ship is an installer that destroys an AmigaOS partition.** Everything below is shaped by that.

**Words (the owner's live test, 2026-11-03):** every prompt and message in short, plain words a newcomer reads at once: "the system part
(root)", "your files (/home)", "the start-up partition" for ZB0, "A password for root, the administrator", "Type it again to check". Nothing
it prints names a person or a machine of the project's.

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
| MBR whose 0x76 RDB has free cylinders (offered first) | as for an RDB, inside the container | as for an RDB |
| MBR with a free slot and free space (the Emu68 card's kind, or a plain MBR) | a new type 0x76 partition, in the last free slot, with an RDB of its own inside, holding ZB0, ZR0, ZH0 | that RDB, written in the free space where nothing refers to it; then one write: the MBR sector. No existing RDB is touched |
| A chosen RDB partition | ZB0, ZR0, ZH0 inside its sectors | new PART blocks; one write: the chosen partition's PART block becomes the first new one, linked on to the rest and to what followed it |
| A chosen RDB partition, split (asked: how much of it Zenix gets; `--zenix-size MB`) | ZB0, ZR0, ZH0 first in its range (ZB0 as low as it can be; the 4 GB rule stands), then one more partition to its old end: **the old name** (assigns still point at it), the old DosType and DosEnvec (buffers, MaxTransfer, mask; its filesystem's FSHD/LSEG chain in the RDB untouched), its own LowCyl, mountable, not bootable, boot priority as it was. Its contents are not touched: AmigaOS shows it unformatted; the plan says so | the same one write as a chosen partition; the rest's PART block is one more new block in the chain before what followed |
| A chosen MBR partition (type 0x83 only: never FAT, 0x76 or a type it does not know) | a 0x76 with an RDB inside, in its sectors | one write: the MBR sector, its type byte |
| Whole disk, or nothing on it | an RDB, ZB0, ZR0, ZH0 | PART blocks where the old table does not need them, RDSK last, one write (section 4) |
| GPT, both RDB and MBR, a partition table that does not add up (also an RDB inside a 0x76: what it claims may reach past its container), a bad-block list (its spare blocks look free) | nothing but the reason (a damaged RDB or MBR: only the erase of the whole disk, with its warning, never with a FAT partition) | - |

**A disk with a PC table (MBR) only boots by itself under Emu68**, whose SD driver reads type 0x76 (knowledge base `amiga/emu68-and-pistorm.md`,
read from the driver's strings; the owner's card boots that way, one 0x76, verified on the A1200 2026-10-24). Kickstart's own `scsi.device` reads an RDB and nothing else. So on an MBR disk the plan says: under Emu68 it
starts by itself; on any other Amiga, start it from the boot floppy (`zenix.adf`, or `zenix-zfs.adf` for a ZenixFS root). **A second 0x76 next to the first has not been seen working on Emu68: not known, and the plan says so**; that is why free cylinders in the first 0x76 are offered before it, and why it takes the last free slot (a driver that counts slots keeps its count for the others).

**An RDB area kept tight** (RDBBlocksHi just past the last PART, as tools/mkrdb-multi.py writes): it may grow into the rest of the cylinders before the first partition, which no table refers to and no partition holds.

**Free space past 4 GB on a top-level RDB is refused**: with the highest boot priority a boot partition there could be read at wrapped places by Kickstart 3.1's disk code (read, not tried).

**Another root marked bootable is refused:** Zenix takes the first (`user/rootpick.h`), so it would stay the machine's root. A disk with more than 14 partitions after the install is refused: boot and mount look at the first 16 devices.

Sizes (the owner's live test on the real A1200, 2026-11-03: /home's filesystem asked too, layouts to choose, full control). First, for free
space or a chosen Amiga partition, how many MB Zenix gets (the rest stays free, or a partition of the old name: the split). Then the
layout of that share, each shown in MB before it is chosen:
- **recommended:** ZB0, then a root of 1 GB from 3 GB of space up, else a third of it (at least 128 MB; at most two thirds of a space under
  192 MB), /home the rest (none under 4 MB); under 64 MB of space the root only.
- **one partition:** ZB0 and a root of all the rest; /home a folder on it.
- **custom:** the person types the root (at least 32 MB) and /home (Return: all that is left; 0: none; at least 4 MB), the MB left shown after
  each; what they leave stays free, or stays the split partition for AmigaOS.
ZB0 is always the rule's: 16 MB from 256 MB of space up, 8 MB from 64 MB, else 4 MB. The root's and /home's filesystems are asked apart
(ext2 or ZenixFS, the same notes for both). A /home of ZenixFS is served by `/sbin/zfsd` like any ZenixFS mount, whichever the root is.
**Memory note:** `fsck.ext2` needs about 12 bytes an inode (one inode per 16 KB here): a 4 GB ext2 /home needs about 3 MB to be checked, more
than a 2 MB machine has; such a machine is better served by a smaller /home or one partition. New labels avoid names already on the disk
(ZB1, ZR1...). Unattended: `--zenix-size MB`, `--layout recommended|one|custom`, `--root-size MB`, `--home-size MB` (0: none),
`--root-fs` and `--home-fs` `ext2|zenixfs`.

**The partition step (the owner's live test, 2026-11-03: "as any OS installer does it").** On "Where should Zenix go?" the choice `d`
lists every partition (name, size, type, dostype, and whether it holds Zenix or AmigaOS) and lets the person mark partitions to delete,
one at a time, each by typing its name (with a plain warning when it is not Zenix's), the list shown again each time, Return when done.
Never deletable: a FAT entry of an MBR, a 0x76 container as a whole, a mounted partition, anything in a damaged table, and partitions
of two tables at once. The space of the marked ones joins the free space next to it; then the usual layouts, filesystems, plan and one
confirmation. **The write:** every kept PART block of that RDB is copied as it is (only `next` changes) and the new ones written, into
blocks of the RDB area nothing uses; one write of the RDSK block (its list, HighRDSKBlock, RDBBlocksHi) names the new chain; the old
blocks stay as they were and unused. On an MBR the deleted entry and the new 0x76 are the same one MBR write. `--undo` restores the old
table (and says the deleted partitions' contents are gone where Zenix was written). Unattended: `--delete NAME`, repeatable.
A torn RDSK write here loses the whole RDB, where the plain install risks one PART block: the same "refused, an RDB tool puts it back".

**A disk with Zenix already** (a ZBn, ZRn and perhaps ZHn of the same n in one Amiga table): the first question, with the advice that
`zpkg -Syu` is the way to update and reinstalling is for a broken or unwanted system:
1. **Reinstall in the same partitions, keep /home** (the default): the users, groups, passwords and the computer's name read from the old
   root first (or new ones asked; the old root not readable: new ones), the root formatted again (its filesystem asked), Zenix copied,
   checked, the start-up partition's files written anew, /home and its fstab line kept, the old table copy kept (so `--undo` still means
   "the table before the first install"). For the time it takes, the start-up and root partitions are unbootable and unmounted (their own
   PART blocks only), as in an install.
2. **Reinstall and erase /home** (its filesystem asked); the users kept or not, their home folders made again.
3. **Remove Zenix and start over:** the partition step with Zenix's partitions already marked.
4. **Install somewhere else:** the usual choice; at the very end the old root's and old start-up partition's bootable marks are cleared
   (their own blocks), so the new one is taken.
Unattended: `--reinstall [--erase-home] [--keep-users|--new-users]`, `--remove-zenix`, `--elsewhere`.

## 3b. Set it up yourself (the owner, 2026-11-04: "use this as boot (keep/format), use this as root (keep/format), use this as home (keep/format), and every option in between")
Besides the layouts above, "Where should Zenix go?" offers `m  set it up yourself`, also on a disk where nothing else is offered (the owner's
card: four slots used, no free cylinders). For each of three roles the person picks any existing partition, by its number in a list, and
says keep or format:

| Role | What may be chosen | Kept | Formatted |
|---|---|---|---|
| **start-up** (zenixboot, the kernel; with a new one also the boot menu) | a partition of an Amiga table (an RDB, or one inside a 0x76); or none: Zenix then starts from its boot floppy | an AmigaDOS FFS volume (DOS\1, DOS\3; 512-byte blocks, 2 reserved, at most about 550 MB), walked whole and checked first by zpkg's own writer (`user/zpkg/bootfs.c`). Added: `zenixboot` at its top (another zenixboot there is replaced), the folder `Zenix/` if missing, `Zenix/zenix` (replaced if there). zenixboot's state reset: its old `state`, `state.new`, `zenix.new`, `zenix.old`, `zenix.rejected` and `zenix-update.*` deleted, so nothing of the old system is started, rolled back to or installed. Nothing else on it changes: its boot menu, its Startup-Sequence, its other files, its table entry | a new FFS volume as ZB0 is (the boot menu with what this disk starts by itself, zenixboot, `Zenix/zenix`); its PART block DOS\1 with Reserved 2; made bootable last with the priority above the highest. Up to about 550 MB (bitmap extension blocks) |
| **root** | a partition of an Amiga table, or an MBR entry of type 0x83, used as it is (never turned into a 0x76) | an ext2 or ZenixFS filesystem found on it: its files stay, Zenix's own files are written anew over them, the users, groups, passwords and the computer's name on it are kept (read before the copy, as a reinstall reads them). If it has too little room the copy stops (said) | ext2 or ZenixFS (asked); at least 32 MB |
| **/home** | as the root; or none (a folder on the root) | an ext2 or ZenixFS filesystem found on it: its files stay | ext2 or ZenixFS (asked); at least 4 MB |

**Every combination**, three different partitions: e.g. the start-up kept with the kernel added, the root formatted, /home kept with its files.
**Never selectable** (section 2, rule 2): FAT, a 0x76 as a whole, an extended or unknown MBR entry, a mounted partition, anything in a damaged
table; for the start-up nothing but an Amiga table's partitions. **Formatting takes typing the partition's name**, when it is chosen, after
a plain warning (as the partition step's). **Kept means what the role needs is found there**: an FFS volume that passes bootfs's whole walk;
an ext2 superblock (0xEF53 at byte 1080) or ZenixFS's (`ZNFS` at byte 32); else refused with the reason.

**The root mark.** Zenix takes the first root that is marked (`user/rootpick.h`): the chosen root is marked as the last but one write (an
RDB partition: bootable; an MBR 0x83 entry: its active flag, the only active flags zinstall ever changes); a partition chosen as /home loses a
mark it had (else it would be taken as the root). Any other root-type partition that is marked is refused, as in section 3. The root must
be one of the first 14 partitions.

**What is written to the tables:** nothing is added, moved or resized; the device names stay. Stage 1: the chosen partitions' own PART
blocks (a formatted one's dostype; flags "do not mount" and not bootable for the time it takes), and the MBR sector when an active flag of a
chosen 0x83 entry changes. Stage 2: the root's mark; then a formatted start-up partition bootable, with its priority; an earlier Zenix's
marks off (as "install somewhere else"). A kept start-up partition's table entry is not written: whether it is the one the disk starts is
said, not changed.

**A kept start-up partition** is written by bootfs's rules: each new file in free blocks and in the safe order (data, bitmap, then the one
write that links it); a file replaced by writing the new data into free blocks, then its header block anew in its own place (one write: the
old file or the new one, never neither), the old blocks given back after; a file deleted by the one write that unlinks it, its blocks given
back after; a cut leaves at most blocks in use that nothing names, which the next write gives back. The summary says whether the partition
is the one the disk starts (bootable, the highest priority) and whether its `S/Startup-Sequence` or `S/BootMenu.conf` runs zenixboot; if
not, the line to add from AmigaOS (`Zenix = run SYS:zenixboot` in `S/BootMenu.conf`). zinstall never writes those files of a kept partition.

**The rest as in any install:** the kernel for the root (kernel-zfs for ZenixFS), `/etc/fstab`'s `/home` line with its device and
filesystem, the accounts (unless kept with the root), the check. `--undo` is not offered (what was chosen is formatted or written over), as
after a replaced partition. The summary before the one confirmation (`yes`) names every role, what is erased, what is kept, every table
block written, and what is not touched.

**Unattended:** `--startup keep:NAME|format:NAME|none`, `--root keep:NAME|format:NAME`, `--home keep:NAME|format:NAME|none` (NAME: the
partition's Amiga name or its device, `ide0p5`), with `--root-fs`, `--home-fs`.

**The owner's card** (MBR: p1 FAT32 for Emu68, p2 0x76 with BOOT, FFS 128 MB with the owner's boot menu, and SH0, PFS3; p3 0x83 1 GB;
p4 0x83 4 GB) installs with: start-up BOOT kept (zenixboot and `Zenix/zenix` added, the menu untouched), root p3 formatted ZenixFS
(kernel-zfs), /home p4 formatted or kept. Written to the table: at most the MBR sector, twice (p3's active flag off, then on). Tested on
an image laid out like it (`zinstall-test` u, e; d an RDB disk), with the host tests; in u Kickstart also starts a copy of the kept BOOT
(as the IDE slave), whose own menu runs zenixboot, which starts the kernel zinstall put there. Not run on the real card.

**The package server** (asked in every install, before the summary; `--server URL|none`): which server zpkg uses for updates. The default
is the first `server` line of the running system's `/etc/zpkg.conf` (an existing Zenix, or none on the floppy); `none` leaves the new
`/etc/zpkg.conf` without one. Written after the check, the `server` lines of the new `/etc/zpkg.conf` replaced; the rest of it as shipped.

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
  by zinstall and fdisk (and, as its manuals say, by AmigaOS); the other partitions' data is not touched. If it happens, only an RDB tool or a PC can put that block back: the copy of section 7 does not exist yet at that moment.
  An MBR has no checksum; a torn entry is checked as any other, and the tests show every tear gives an old, a new, or a refused table.

## 5. Where the files come from
**The install floppy set.** `make install-floppies` makes `zenix-install-1.adf`, `-2`, `-3`... Disk 1 boots: the bootblock, the kernel, a RAM
disk with `zinstall` and what it runs, then the first part of the set. The set is one stream of package bundles (D22's format, verified part by part) laid across
the disks after a small header sector (`ZSET`, set id, disk number). The kernel on disk 1 is the one the set installs, so it is not stored twice. `/etc/zinstall.set`
in the RAM disk lists every item with its place and SHA-256; a floppy's content is trusted because the boot floppy says what it must be. Other
drives are looked at too, so with several drives (or a Gotek) nothing needs swapping; with one drive it asks for the next disk. Core
programs come first, the rest of the set on the last disk; the person may skip that disk (then `zpkg -S` later).

**What the install set holds (2026-11-18, the main session's decision for the full audit's FA-3):** the base system
that boots, logs in, uses the network and runs zpkg, nothing more. `tools/mkinstall.py`'s `CORE` (on disks 1-3: the
base group zup, base and zpkg; the everyday commands' families; shadow, disktools with the filesystems' servers,
archive, edit, nettools, zinstall) and `SETX` (alone on the last disk, which may be skipped: awk, base-man, telnet,
ssh, wget, sntp, zmodem, cu, usb, the desktop with its servers). Every other package is optional and on no disk:
devel (tcc, zld, zas, zdb and the C library), games, lua, make, sumi, maki, markdown, ahiru, ctop, irc, mail and their
tls, sound (the server and the players), zine, wasabid; `zpkg -S NAME` brings each from the package server after
installing, and the start from AmigaOS says so at its prompt. Why: block 7 grew the packages to 4.0 MB (devel 0.9 MB,
games 0.4 MB), and the start from AmigaOS carried "the whole set" in a RAM disk of 3,400 KB that no longer held it
(the build failed) and would not load beside AmigaOS on the 4 MB minimum anyway. The desktop stays whole: `files` and
`show` are in its package, and splitting a package is a change of the package set (D22's takes), not of the
installer. The build checks the size: `mkinstall.py` adds up the set (each file's blocks with its indirect blocks)
against the RAM disk's free blocks and inodes, keeps 64 KB free, and stops with both numbers and what to do if the set
outgrows it. **A ZenixFS root needs the second
disk for its kernel** (kernel-zfs).

**No floppy at all: from AmigaOS** (for a machine with fast RAM, such as the A1200 with the Apollo 1260). `make install-floppies` also makes
`install/from-amigaos/`: `zenixboot`, the kernel `zenix`, and `zinstall-ram.img` (2,900 KB, `RAMDISK_INSTALL_HD_K`: the installer's RAM
disk with the install set inside as files, each checked against `/etc/zinstall.set`; 2,018 KB of it the set, 291 KB free, 2026-11-18). Steps, from AmigaOS:
1. Copy the three files to a folder, e.g. `Work:Zenix-Install/` (they are written nowhere else).
2. In a Shell: `Work:Zenix-Install/zenixboot Work:Zenix-Install/zenix RAM=Work:Zenix-Install/zinstall-ram.img`
   (zenixboot's own `RAM=`, docs/man/zenixboot.txt). AmigaOS stops; Zenix starts with the installer's RAM disk as its root.
3. At the prompt: `zinstall --list` first (below), then `zinstall`.
Verified in FS-UAE on an A1200 with a 68060 and 8 MB of fast RAM (`zinstall-test` h, the files on an FFS DH0 started by Kickstart's
own shell). The partition the files are on is only read.

**The first time on a new machine** (real hardware Zenix has not met): Zenix's Gayle IDE driver with a real CF card and Zenix on a real
68060 have run since 2026-11-04 (the A1200 with the Apollo 1260, d37), but a disk Zenix has not read before is still new. So: start the installer,
`zinstall --list` (it only reads: the table as Zenix reads it, each partition's name, size, type, dostype, start and end cylinder, the
cylinder size), compare it with HDToolBox, `reboot` back to AmigaOS; only if it agrees, start again and install.

**The network, from an existing Zenix.** If `/etc/zinstall.set` is not there (an existing Zenix) and zpkg has a server configured, `zinstall` asks `zpkg
--fetch DIR` (new, section 8) to bring the signed list's system bundle and packages into the new root's `/var/cache/zpkg/pkg`, checked as
always; then it installs from them the same way. There is no network on the RAM disk: `netd`, its driver and `zpkg` take about 200 KB it
does not have. After a floppy install `zpkg -Syu` brings anything newer.

Both give the same machine: every package's manifest in `/var/lib/zpkg`, so `zpkg` and updates know what is installed.

**The source of the two AmigaOS programs on the boot partition** (`bootmenu`, `zenixboot`): the package `zinstall` carries them in
`/lib/zinstall/`, always installed. So an installed Zenix can also rebuild its boot partition's files, and no separate way exists to get them.

## 6. Dual boot, and the way back
The boot partition gets a boot priority one above the highest on the disk, with `bootmenu` as its only startup. **The menu's default is whatever this disk
started by itself until now** (the highest bootable, mounted partition, whatever its name; it may be a menu of its own), after 5 seconds: the machine goes on starting it unless someone presses a key. `bootmenu` writes
nothing to disk. A partition whose name the menu cannot use (a space at its end, a colon) is refused: the menu would reboot for ever looking for it. With no such partition (a system on another disk is not seen) the plan says so. A boot priority above 5 also outranks the floppy; Early Startup still chooses. Ways back, which the program prints at the end:
- **`zinstall --undo`**: every table sector as it was, the one that made the new table real first; refused if any of them is no longer what zinstall left, and after a replaced partition or an erased disk (what was there is gone). After a split it puts the old table back too (the partition whole, its old name), and says that its contents are gone and it needs formatting. Tested from the new Zenix itself; the copy is `/var/lib/zinstall/table-before` in the new root (`zinstall --undo FILE` from elsewhere).
- At power-on, both mouse buttons: Kickstart's Early Startup menu chooses a partition by hand.
- HDToolBox or `fdisk del` on an Amiga table.

## 7. What it writes where, in order
1. Stage 1 of the table (section 3), each step flushed. Rescan; find the new devices by their exact start and size; refuse if not found.
2. `mkfs.ext2` or `mkfs.zenixfs` on the root, `mkfs.ext2` on `/home`; mount; unpack (each file against its manifest's SHA-256).
3. Unmount, mount again, every file read back against its SHA-256 from the disk; then `/etc/fstab` gets the `/home` line, `/tmp`, `/home`, `/mnt` their places, the table copy (a failure to write it stops the install), `/home` its folders; `mkfs.ext2 -i 16384`: `fsck.ext2` needs about 12 bytes an inode, and at the default of 4 KB a 128 MB root ran it out of memory on 2 MB of chip RAM (measured).
4. The boot partition: an FFS volume written by the program's own small writer (`user/zinstall/ffsw.c`): `S/Startup-Sequence`, `S/BootMenu.conf`, `bootmenu`,
   `bootzenix`, `zenixboot`, `Zenix/zenix`; read back as a reader finds them, by the hash tables. A ZenixFS root gets kernel-zfs as `Zenix/zenix`. Then the root is unmounted and `fsck` runs read-only.
4b. **The machine and its accounts** (the owner, 2026-11-03: set up, not left for later), after the check, with `/home` mounted: the machine's name
   (default `zenix`, 1-63 of `a-z 0-9 -`) into `/etc/hostname`, which login shows in its prompt and gives the shell as `$HOSTNAME`
   (nothing read a name from a file before; `hostname` printed `$HOSTNAME` or `zenix`); root's password, typed twice with no echo
   (`read_secret`, as passwd); a first user (skippable): the login name checked as useradd checks it, a full name, a password typed twice,
   the groups `users`, `wheel` (sudo) and `zine` (demos), home `/home/NAME` mode 700 its own. The shipped accounts `user` and `guest`, which
   have no password, are locked (`*`) unless chosen as the first user's name: an installed machine has no account open to anyone at the
   console. **One way of writing `/etc/passwd`:** `pw_set_hash_at` and `pw_add_at` in `user/pw.c`, which passwd and useradd now call too, on
   a file named by path (the new root's). As options, for a run with no questions: `--hostname`, `--root-password-file`, `--user`,
   `--user-name`, `--user-password-file`, `--no-user`.
5. Stage 2: the root's PART marked bootable (Zenix's rule for which root to take, `user/rootpick.h`), then ZB0 bootable with its priority.

If a step fails, it stops and says what is done; the table has only unbootable, unmounted partitions. Run again, they are offered by name. `/etc/fstab`'s names (`ide0pN`) follow the table's order: a later change to the table before them shifts them (known, as for any fstab of `ide0pN`).

## 8. Shared things touched
- `tools/mkzup.py`: a family `zinstall`. `user/rdb.h`: `RDB_MAXUSED` may be raised by a program that includes it, and is 512 by default (it was 128: with a PFS3 handler of 121 LSEG blocks, RDSK, FSHD and 6 PART blocks passed it, and after a split install the disk server said at every boot "a block chain leaves the RDB area or loops" about a table that was right; found on the real A1200 2026-11-04, reproduced on the host by `tools/host/rdbread-host.c`, which reads with rdb.h's own default; AmigaOS was not affected: every chain block lay within RDBBlocksHi and HighRDSKBlock). The message for a full table is now its own. `user/zpkg.c`: `zpkg --fetch DIR [--zfs]`, growth only. Makefile: the program, its RAM disk, the floppies. No system call, message, or number claimed.
  Section 3b (branch inst2, 2026-11-05): zpkg's boot partition writer `user/zpkg/bootfs.c` is linked into zinstall too, and grew by
  `zp_boot_cd` (the root or a folder in it), `zp_boot_mkdir`, `zp_boot_replace` (a file anew in its own header block) and `zp_boot_delete`,
  each in the safe order and tested as the rest of it (`tools/host/zpkg-test.sh boot`: AmigaOS's rules, xdfscan, a cut before every
  write); `user/zinstall/bootkeep.c` is the kept start-up partition's code on top of it; `user/zinstall/ffsw.c` writes bitmap extension
  blocks (a formatted start-up partition up to about 550 MB). The install floppy's RAM disk grew from 600 to 632 KB for zinstall's
  31 KB more (`RAMDISK_INSTALL_K`).
- The floppy set's format is not frozen: disk 1's own program reads it, always of the same build.

## 9. The review
One independent read aimed only at damaging a partition (2026-11-01). Found, and fixed with a test each: the skip of partitions named `BOOT...` made the menu lose the system the machine really starts; a new 0x76 was offered beside an inner RDB with a partition past its container, and landed in it; a partition inside an MBR with overlapping entries could be chosen, on the FAT; new PART blocks could land on a bad-block list's spare blocks; a boot partition past 4 GB had only a note; a menu name it cannot find would reboot for ever; the way back printed `fdisk del` where fdisk refuses (hence `--undo`); "the machine starts as before" after a replaced partition; `hasfat` missed behind a full table and on a bad signature; exFAT and unknown types could be chosen; busy flags not compared in the re-read. Known and left: a second console mounting a partition after the re-read can still see the table change before diskd stops the data writes; `fstab` names shift as above; an AmigaOS on another drive is not seen, only named in the plan.

**What it shows.** `--list` and the first screen show the RDB's own fields (RDBBlocksLo/Hi, HighRDSKBlock, how many blocks its chains own
and their range, the filesystems' range) and each partition's PART block, to compare with HDToolBox. On a disk whose Zenix is running,
`--list` and `zinstall` say so and offer nothing: it cannot install over itself (zpkg -Syu updates it). Every printed line fits 79
columns: a longer one is broken at a space (a line of exactly 80 wrapped on the console, found on the real A1200).

## 10. What it does not do
No shrinking of an existing filesystem in place (the owner's decision, 2026-11-03: free space, or a partition the person chooses to replace, which may be split into Zenix and a new empty partition of its old name and type); no GPT; no install from a folder; no network from the floppy; never changes an MBR's active flag but that of a 0x83 entry chosen as the root or /home in section 3b; never writes a kept start-up partition's boot menu or Startup-Sequence; one disk at a time (Zenix serves one IDE disk).

## 11. Measured (FS-UAE and the PC, 2026-11-01 to 11-04; no speed figures: emulator time is not hardware time)
The machine Zenix is made for (CLAUDE.md, 2026-11-04) is a stock A1200 (68EC020) with 2 MB of chip and 4 MB of fast RAM: the
install must pass there (`zinstall-test` m, z). 2 MB of chip RAM alone is best effort (part b reports, never fails).
| | |
|---|---|
| `zinstall` | 145,268 bytes (ZEXE), -Os, with the password code it shares with passwd and useradd, and zpkg's boot partition writer (2026-11-05, section 3b; it was 113,900) |
| The install floppies | 4 disks: disk 1 the bootblock, the kernel (172 KB), a RAM disk of 632 KB (600 before section 3b) and part of the set, core on disks 1-3, the rest of the set (SETX) alone on disk 4 (2026-11-18: 2,375 KB of bundles with the padding) |
| No floppy (`install/from-amigaos/`) | zenixboot 53 KB, the kernel 172 KB, `zinstall-ram.img` 2,900 KB (needs fast RAM; 3,400 KB with the whole set until 2026-11-18, which no longer fitted: FA-3) |
| Free at the prompt, booted from disk 1 | 2 + 4 MB: 1,255 KB chip and 3,505 KB fast; 2 MB chip alone: 729 KB |
| Lowest free memory seen while installing (sampled at each file and program end; a child's peak in between is not seen) | 2 + 4 MB: 4,198 KB with ext2, 4,075 KB with ZenixFS; 2 MB chip alone: 220 KB with ext2 (one partition), ZenixFS refused (2026-11-05, after section 3b: 87 KB, 87 KB and 57 KB less than on 11-04, the bigger RAM disk and program) |
| What zinstall wants free before it writes anything | 350 KB (ext2), 500 KB (ZenixFS): measured use plus a margin; less, and it says so and writes nothing |
| The check after the copy | fsck.ext2 -n needs about 12 bytes per inode, two block bitmaps and 96 KB (zinstall gives the copy's 32 KB window and track buffer back first); where that is more than is free it is skipped and said (every file was already checked against its SHA-256), and the line to run later is printed |
| The installed machine, one login | 2 + 4 MB: 1,855 KB chip free and 3,464 KB fast (ext2) or 3,196 KB fast (ZenixFS); 2 MB chip alone: 1,287 KB on ext2 |
| Files checked against their SHA-256 after the copy | 307 (core) to 312 (everything) |

**2 MB of chip RAM alone (best effort), measured 2026-11-04.** With ext2 on one partition the install finishes (277 KB lowest)
but fsck.ext2 does not fit beside zinstall for a 283 MB root (it needs about 379 KB, 317 KB were free): it is left for later,
as said above. ZenixFS does not start: 460 KB free against the 500 KB it wants. What it would take: a smaller RAM disk on
disk 1 (each KB is a KB free; the shared code there is 87 KB), zinstall itself smaller or run as two programs (the copy, then
the check after zinstall has exited), and fsck.ext2 reading the inode table in groups instead of keeping 12 bytes per inode.
None of it is done: the floor that must pass is 2 + 4 MB.

Tests: `tools/zinstall-host-test.sh` (314 checks, the fuzzer 300,000 rounds, ASAN and UBSAN), `tools/zinstall-test.sh` f r a m z b c n o h k (m z b: 71 checks, 2026-11-04), `useradd-test` (63) and `users-test` (61) after the shared password code.
Section 3b (2026-11-05, fast mode): `zinstall-host-test` 349 checks (the owner's card's layout in every role combination that matters, the
refusals, a cut at nine points, the fuzzer with manual plans: only chosen PART blocks and the MBR sector written), `zpkg-test boot` 113
(bootfs's new calls, a cut before each write), `zinstall-test` 303 with the new parts u (25), e (20), d (16), `sysupdate-test` 27.
