# D25: the Zenix demo floppy, network edition (design note, revision 1)

Status: **design, revision 1; nothing built.** Roadmap block 2 ends with **the Zenix demo floppy** (owner 2026-10-25,
in the spirit of the 1999 QNX demo disk): "one 880 KB Amiga floppy that boots into Zenix with the network, `telnet`
and a web-page fetcher, to hand to anyone"; a second edition with the desktop in block 5. The model (owner 2026-10-26)
is KolibriOS: "the programs are packed on the disk and unpacked when they start, which is what makes a desktop
edition fit 880 KB". D19 7.4 and D21 7.1 left the floppy's sizes to this note. The short version is DESIGN.md D25.

Facts about today's code are *read* from main at 7738665 (2026-10-28). Sizes marked *measured* come from building
that tree on the PC (`make`, the programs at -O2 as shipped) and packing its files with the packers named; a
*prototype* number comes from laying out the floppy's filesystem on the PC with those packed files (section 4.3).
Times are *modelled* (the CPU emulator Musashi's 68020 timing tables: no cache misses, no wait states) or *estimates*;
nothing in this note is timed on a real Amiga.

## 1. What the owner gets, in plain words

- **One DD floppy (880 KB, an ADF for emulators and Goteks) that boots any Amiga with a 68020 or better and 2 MB of
  memory**, the stock A1200 included, into Zenix in about 6 seconds of loading (*estimate*; today's RAM floppy reads
  860 KB, about 33 seconds). It asks two questions with defaults (keyboard; network when no card is found), then
  shows a short welcome and a shell. Nothing on the floppy is ever written; the user's files live in memory until the
  machine is switched off.
- **On it:** the shell, 50-odd everyday commands with their manual pages, the network (DHCP on an A2065 or the CM4's
  Ethernet; SLIP over a serial cable to a PC on any Amiga), `ping`, `host`, `telnet` with Zmodem, `ssh`, `wget` with
  HTTPS, the editor, `less`, `grep`, `gzip`/`tar`, `cu`, the PC font for BBSes. About 70 KB stay free (*prototype*).
- **How it fits:** every program is packed on the disk (LZ4, about 70% of its size) and unpacked by the kernel when it
  starts, straight into the program's own memory: no RAM disk holds the programs. The small commands are linked into
  one program (a "box", as BusyBox does) so their shared C library is on the disk once instead of 37 times: that saves
  more than any packer could (section 6).
- **Snappy:** LZ4 was chosen over smaller packers because on a 14 MHz 68020 reading from a floppy it is the only one
  that makes programs start *faster* than unpacked ones (section 5.3); the box stays in memory after its first use, so
  `ls`, `cat`, `ps` answer at once after the first command.

## 2. Today (read and measured)

- **The RAM floppy** `zenix-ram.adf` (Makefile): the bootblock (1 KB; its code 566 bytes of the 1,008 available,
  *measured*), `kernel.bin` from byte 1024, then a 704 KB ext2 RAM-disk image (`RAMDISK_K`) that the bootblock loads
  into memory, where it stays for the whole session (D19 7.4: "a RAM disk stays resident on a RAM-floppy boot, which
  the demo floppy will be"); so the kernel may be at most 179,200 bytes (`KERNEL_MAX`). `kernel.bin` today: **155,944
  bytes** (*measured*). The RAM disk carries a short fixed list (`RAMAPPS`), no manual pages, no network.
- **The bootblock** (`boot/bootblock.S`, `boot/handoff.S`): reads the kernel (and the RAM image) with `trackdisk.device`
  into `AllocMem` buffers while AmigaOS lives, builds bootinfo (the RAM disk's address among it), calls the kernel's
  stage0, kills the OS, copies the kernel home and jumps. The kernel relocates itself (ZEXE `--delta` table).
- **The root rule** (`user/rootpick.h`): a RAM disk (`ram0`) is the root; boot (`user/boot.c`) registers `net` ->
  `/sbin/netd` only when `sys_stat("/sbin/netd")` succeeds at boot, then starts `/bin/init`; init prints
  `/etc/motd`, runs `/bin/mount -a` from `/etc/fstab`, then `/bin/ifup -a` when `/etc/network/interfaces` has an
  `auto` line, then `/bin/login --init` on the consoles (`/etc/autologin`: console 1 without asking).
- **The loader** (`kernel/proc.c`): ZEXE and ZEX2 (both frozen), from a file (`fill_fs`, read through the filesystem)
  or from memory (`fill_mem`, the programs built into the kernel); every header field checked before use (`zexe_bad`,
  `zex2_bad`). ZEX2 code is one segment shared by all processes running it, keyed by (mount slot, mount generation,
  inode, size, time), **freed at once when its last user ends** (D13 7, stage 1; the note already names the later rule:
  keep unused segments and drop them when `mem_alloc` fails, as AmigaOS purges libraries). `sh` and `login` are ZEX2;
  a program that sends raw messages (`mount`, `dd`, `fdisk`, ...) must stay ZEXE (D13 3).
- **The floppy driver** (`kernel/floppy.c`): AmigaDOS tracks by Paula DMA, a whole track per read, one decoded track
  cached, idle motors off after 3 s; `diskd` serves `df0`-`df3` as whole devices (no partitions); **verified in FS-UAE
  only, never run on a real drive or a Gotek** (features.md).
- **fsd** has a read-only mode (`ext2_readonly`: a volume with an unknown feature, the `-fsck` check server); `mount`
  has no option that asks for it (man/mount.txt).
- **The keymap:** `sv` (the default) and `us` are built into the kernel (`kernel/kbd.c`); `kbd_set_keymap(name)`
  exists but nothing calls it (the comment says "/etc/keymap later").
- **sh** reads `/etc/profile`; its `read` has `-p` but no time limit (man/sh.txt).
- **Packers already in Zenix:** miniz (deflate) in `gzip` and `zup`; its decoder `tinfl` is 7,752 bytes of code
  (*measured*, -O2).
- **Memory on the stock A1200** (2 MB chip, no fast RAM): 1,365 KB free with one login on a disk root (D19 step 1,
  *measured* in FS-UAE), 1,159 KB with SLIP up (D21 7.2, *measured*).

## 3. The choices in short

| Question | Choice | Why (section) |
|---|---|---|
| Packer | **LZ4** (block format, packed at level 12 on the PC) everywhere: kernel, RAM image, programs, manual pages | fastest to unpack; the only one that beats a plain floppy read on a 14 MHz 68020 (5) |
| Where programs are unpacked | **by the kernel's loader, into the process's own memory** (a new wrapper `ZPAK` around a ZEXE or ZEX2 file) | no RAM disk for programs: unpacked code costs only while it runs (5.4) |
| Small commands | **one "box"** (a multi-call ZEX2 program) for the light commands, kept in memory after use | one copy of libz on the disk: 3x smaller than separate files (6) |
| Root and writable files | **a 160 KB RAM-disk root** (`/etc`, `/home`, `/tmp`, `/var`, `init`, `mount`), the floppy's filesystem **mounted read-only on `/usr`** | today's root rule unchanged; nothing written to the floppy (7) |
| Floppy layout | one ext2 filesystem over the disk (the Amiga bootblock in ext2's boot area), the packed kernel and RAM image after its end | no partition table, no new device naming (7.1) |
| Format | **DD 880 KB** only for the network edition | every Amiga drive and Gotek; HD only if the desktop edition needs it (10) |
| Keyboard | asked at every boot, a 10-second default | the floppy is for anyone (9) |
| Network | tried in order: the CM4's Ethernet, an A2065, else the user picks SLIP or none | no card needs configuring by hand (9) |

## 4. What goes on the floppy, and the budget

### 4.1 Contents

- **Booted by the bootblock** (outside the filesystem): the kernel (the same `kernel.bin` as every image, with `fsd`,
  `diskd` and `boot` inside, packed by the build) and the RAM-disk root image (packed).
- **The RAM root** (unpacked at boot, 160 KB): `/bin/init`, `/bin/mount` (plain: each runs once at boot, and `mount`
  sends raw messages, so it stays ZEXE); `/etc` (passwd, group, fstab, console, motd, autologin, hosts, profile,
  keymap, `network/` written by the setup, `ssl/anchors` a link); empty `/home/guest`, `/root`, `/tmp`, `/var/lib`;
  `/bin/NAME` links to `/usr/bin/NAME` for every command; `/sbin/netd` and `/sbin/net` links into `/usr/sbin`.
- **The floppy's filesystem, read-only on `/usr`:**
  - `bin/`: `sh` and `login` (ZEX2), **the box** (ZEX2: `ls cat cp mv rm mkdir rmdir ln echo pwd ps free df uname
    uptime date clear kill dmesg sleep true false env hostname id whoami touch umount reboot head tail wc ifconfig
    route ping host netstat`, `ifup`/`ifdown` as names of `ifconfig`, each name a link to `box`), and on their own
    (ZEXE): `less` (`more`), `edit`, `man`, `grep`, `sort`, `gzip` (`gunzip`, `zcat`), `tar`, `cu`, `telnet`, `rz`
    (`sz`), `ssh` (ZEX2), `wget` (when D21's step 7 has built it).
  - `sbin/netd`, `sbin/net/a2065`, `sbin/net/genet` (SLIP is inside netd).
  - `man/`: the pages of every command on the floppy plus `demo` (what this floppy is, the SLIP cable, the PC side),
    each packed.
  - `share/consolefonts/cp437-8x8.psf` (D20, 2,052 bytes), `share/licenses/` (lwIP, BearSSL, Monocypher, miniz,
    Zmodem, the font: the owner's rule, every image carries its notices), `share/ssl/anchors` (D21's anchors file,
    plain: wget reads it by its index).
- **Left out:** `ssh-keygen` (a key made in memory is lost at power-off; `ssh` with passwords works), `zup`/`zpkg`
  (a read-only floppy is never updated: zpkg already refuses system updates on a bootblock boot, D22), the disk tools
  (`fdisk`, `mkfs.*`, `fsck.*`: a demo must never offer to format the visitor's disk), `awk` (100 KB), the test
  programs, `su`/`sudo`/`passwd`/`useradd` (one user session; section 9.4).

### 4.2 The packers measured on today's files

Bytes, *measured* (`lz4 -12`, `gzip -9`, `zstd -19`, `xz -9e`, Shrinkler 4.7 `-d -9`; the programs as built at -O2):

| File | Plain | LZ4 | deflate | zstd | xz | Shrinkler |
|---|---|---|---|---|---|---|
| `kernel.bin` | 155,944 | 103,965 | 88,199 | 82,719 | 75,650 | 77,303 |
| `ssh` | 106,424 | 73,805 | 62,903 | 58,516 | 53,393 | 54,218 |
| `netd` | 93,412 | 66,967 | 54,884 | 50,177 | 45,605 | 46,583 |
| `sh` | 87,124 | 57,687 | 47,927 | 45,267 | 41,480 | 42,034 |
| `telnet` | 31,280 | 23,146 | 18,585 | 17,494 | 15,881 | 15,939 |
| `ls` | 20,676 | 14,551 | 11,494 | 10,743 | 9,761 | 9,686 |
| 40 small commands, each packed alone (sum) | 400,212 | 282,624 | 224,256 | 214,016 | 197,632 | 193,536 |
| the same 40 as one stream ("solid") | 400,212 | 133,769 | 128,657 | 87,933 | 76,512 | 77,505 |
| the 53 manual pages they need, each alone | 96,792 | 66,894 | | | | |

Two findings decide most of the note: **code packs only to about 70% with LZ4 and 50-55% with the strongest packers**
(68k code is dense), and **the same 40 small tools packed together take half of what they take packed one by one**: the
duplicate is libz, 2-4.5 KB in every program (`hello` is 4,652 bytes, `true` 220). A packer cannot see across files;
linking them into one program removes the copies at the source (section 6).

### 4.3 The budget (DD, 901,120 bytes)

The *prototype* (`mke2fs -d` on a tree of the packed files, 1 KB blocks, 144 inodes, `wget` stood in by 73,000
incompressible bytes, D21's anchors file by 52,231): **695 of 763 blocks used, 68 free**, e2fsck clean.

| Part | Bytes | How known |
|---|---|---|
| Bootblock (ext2's boot area) | 1,024 | layout |
| Kernel, LZ4 | 103,969 | *measured* |
| RAM root image (160 KB ext2: init, mount, /etc), LZ4 | 14,423 | *measured* |
| Filesystem: `sh` 57.7 K, `login` 11.0 K, box 43.3 K, `less edit man grep sort gzip tar cu` 117.2 K, `telnet` 23.1 K, `rz` 17.6 K, `ssh` 73.8 K, `wget` ~73 K, `netd` 67.0 K, drivers 19.7 K | ~503 KB | *measured* (wget *estimate*, D21 7.1: 100-110 KB plain) |
| Filesystem: manual pages 65 K, anchors 51 K, licences and font 18 K | ~134 KB | *measured* |
| Filesystem: ext2's own (inode table 18 K, bitmaps, directories, indirect blocks, half a block per file) | ~58 KB | *prototype* |
| **Free** | **~68 KB** (7.5%) | *prototype* |

**The rule for the build** (as `tools/mkadf.py` today): the image is refused when over, and every build prints each
part and the free space. **If a later version is over, drop in this order:** the anchors file's long tail (a list of
the ~25 roots that sign most of the web, about 12 KB instead of 51: frees ~40 KB); the manual pages of the commands
anyone knows (`ls`, `cat`, ...: ~20 KB); `tar`, `cu` (~17 KB); the second box below (6.3).

## 5. The packer

### 5.1 The candidates

- **LZ4** (block format): bytes and copies, no entropy coding. Decoder about 100 bytes of 68000 code, no tables.
- **Deflate** (miniz's `tinfl`, already in Zenix): Huffman plus LZ77. 7.7 KB of code and an 11 KB state: too big for
  the kernel, where every Zenix user would carry it.
- **Shrinkler** (the demoscene's, Blueberry): a range coder with context models; the best ratio here; a ~200-byte
  decoder with 3 KB of tables on the stack (knowledge base `demoscene/research/research_notes/.../shrinkler_cinter.md`).
- **ZX0** (and LZSA2): LZ with Elias-gamma lengths, an 88-byte 68000 decoder (*read*, the same note). **Not measured
  here** (its packer is not on this PC); expected between LZ4 and deflate in size and speed. The candidate for a
  second, stronger method if the desktop edition needs one (section 10).

### 5.2 Unpacking speed (*modelled*)

Each decoder was run in Musashi as a 68EC020 on the files above and its output compared byte for byte; cycles per
**unpacked** byte:

| Decoder | Code | Cycles per byte (68020 model) |
|---|---|---|
| LZ4, plain C at -O2 | 328 bytes | 27-31 (text 35) |
| `tinfl` (deflate), C at -O2 | 7,752 bytes | 120-167 |
| Shrinkler's own 68000 decoder | ~450 bytes with its frame | 560-695 |

The same model on a 68000 gives Shrinkler 2,535 cycles per *packed* byte for `ls`, which matches the earlier
independent model of 2,500 (knowledge base, the same note): the model is consistent. **On the stock A1200** the code
and data are in chip RAM, shared with the display: the model's numbers are taken as **1.5-2x** for LZ4 and Shrinkler
(small loops that fit the 256-byte instruction cache) and **2-3x** for `tinfl` (its code does not fit) - *estimates*,
to be timed on the A1200 in step 1. At 14.18 MHz that is about **250 KB/s** unpacked for LZ4, **35-50 KB/s** for
deflate and **14-16 KB/s** for Shrinkler. A hand-written LZ4 decoder (longword copies) may be faster still.

### 5.3 What the user feels: start times from the floppy (*estimates*)

Assumed: the floppy delivers **20 KB/s** to a program load (one track, 5.5 KB, per revolution of 200 ms, plus steps
and the filesystem's own blocks; 26 KB/s for the bootblock's long sequential reads); the stock A1200; seconds:

| Program | Plain | LZ4 | Deflate | Shrinkler |
|---|---|---|---|---|
| `sh` (at login) | 4.4 | **3.2** | 4.6 | 8.0 |
| `telnet` | 1.6 | **1.3** | 1.7 | 2.9 |
| `ssh` | 5.3 | **4.1** | 5.8 | 9.9 |
| `netd` (in the background at boot) | 4.7 | **3.7** | 5.1 | 8.7 |
| the box, first command | 3.3 | **2.4** | 3.4 | 6.0 |
| the box, every later command (kept in memory, 6.2) | - | **~0.1** | | |
| **The boot**: kernel + RAM image | 33 (today's RAM floppy) | **4.5 read + 1.2 unpack** | (no room in the bootblock) | 3.3 + 21 |

On a 14 MHz 68020 the floppy and the CPU are about equally slow, so **only LZ4 is faster than not packing**; deflate
breaks even and Shrinkler doubles every start. On the 68060 and on the PiStorm every packer unpacks faster than the
floppy reads, so the smallest file wins there; but the floor sets the choice (CLAUDE.md, the stock A1200 rule), and
LZ4 already fits with room (4.3). **One method for everything** (bootblock, kernel, manual pages) keeps one decoder.

### 5.4 The packed file: `ZPAK` (a new format, frozen once shipped)

```
+0  u32 unpacked size      (the whole original file: a ZEXE or ZEX2 image, or any data for `man`)
+4  'ZPAK'                 (at +4 like 'ZEXE' and 'ZEX2', so the loader reads one place)
+8  u8  method             (1 = LZ4 block; others refused; new methods append)
+9  u8  0
+10 u16 header size        (16; a reader skips what it does not know)
+12 u32 packed size        (exactly the rest of the file)
+16 the packed bytes
```

- **The loader** (`proc_load`): a file whose +4 is `ZPAK` is checked (sizes against the file, the unpacked size at
  most `ZEXE_MEM_MAX` and at least a header), read whole into a kernel buffer of `unpacked + margin` with the packed
  bytes at its end, unpacked **in place** forward (the LZ4 rule: the margin is the packed stream's worst case, about
  unpacked/255 + 16 bytes), and then loaded **from memory** by the existing ZEXE/ZEX2 code (`fill_mem`'s path, every
  check unchanged); the buffer is freed after. A ZPAK inside a ZPAK, a method other than 1, a stream that would write
  past the buffer or read past its input, or an unpacked file that is not a valid ZEXE/ZEX2: "exec: NAME refused: ...",
  never a crash (the decoder checks both ends on every copy). Then the instruction cache is flushed as after every
  load.
- **Memory while loading:** the program's own memory plus, for a moment, the unpacked file (`sh`: 87 KB) - about what
  `fill_fs` holds today for the relocation table, larger; freed before the program runs.
- **A ZEX2 program inside ZPAK** is shared as any ZEX2 program: the segment's key is the packed file's identity.
- **`man`** (and later any reader) unpacks a ZPAK page with a libz helper built **from the same decoder source** as
  the kernel's (one source, D13's "no copies" for code that is the same).
- **Not in packages:** zpkg packages and D15 bundles carry plain files; ZPAK is made only by the demo floppy's build.
  The loader's support is general (a kernel change, reviewed as one).
- **The PC side:** `tools/zpak` (C, built by the host compiler as `zenix-slirp` is), using the LZ4 library's own
  compressor (BSD-2-Clause, kept with its licence header in the repository) at level 12, so the result equals what was
  measured here; a host test unpacks every packed file with Zenix's own decoder and compares.

## 6. The box, and keeping shared code in memory

### 6.1 The box (a multi-call program, BusyBox's idea)

**Measured:** the light commands of 4.1 linked into one program (each one's global names renamed with `objcopy`, a
20-line `main` that picks the command by the name it was started as) is **65,732 bytes plain and 43,300 LZ4**, against
293,764 plain and 211,650 LZ4 as 37 separate files. Built from the same sources; only the link differs, and only on
this floppy (hard disks keep separate files). Rules:

- A box command runs exactly as the separate program does: its own process, its own data and stack, `argv[0]` as
  given (`ifup` stays `ifup`).
- **The box is ZEX2 (pure)**, so its code is one copy however many commands run (a pipe `ls | grep x | wc` holds one
  copy of the code and three small data parts). The box's data part is the sum of its members' data and BSS, about
  4.4 KB plus 19 KB *measured* for the ZEXE build; each process carries all of it (a cost of the box, measured at its
  step; members with big static buffers stay outside).
- **Members:** only light commands that send no raw messages (D13 3) and that D13's measured pure-code cost does not
  slow (no CPU-heavy tools: `grep`, `sort`, `gzip` stay outside, as D13 8 keeps them ZEXE).
- **Tests:** every member runs from the box in the demo test (section 11), and the commands test script
  (`rootfs/tests/cmds.sh`) runs once against a box build on the PC's emulator.

### 6.2 Unused shared code stays in memory (D13's later rule, built now)

Without it, every `ls` would read and unpack the 43 KB box again (2.4 s); with it, the second command starts in about
0.1 s. **The rule (D13 7, as the note planned it):** when the last process using a segment ends, the segment stays on
an "unused" list (oldest first) instead of being freed; a new start with the same key takes it again; when `mem_alloc`
fails, the kernel frees unused segments, oldest first, and tries once more. A cached segment never blocks anything:
opening its file for writing, unlinking it, a filesystem server's restart (the mount generation) or a different key
drops it. `free` counts it as reclaimable (a new word at the end of `SYS_meminfo`, main's to claim). This helps every
Zenix machine (a hard-disk `sh` restarts at once too), costs a few dozen bytes of kernel, and is a deep core change:
its own tests (`tools/memtest.sh`: fill memory past the cache, the segments dropped, the allocation succeeds; a
changed file never runs old code).

### 6.3 A second box?

The text tools (`less edit man grep sort`, sharing `regex.c`) as one program are 41,562 LZ4 against 77,664 separate
(*measured*), but they are not pure today, so without 6.2 every start would read the whole box. **Not in the first
version**; taken only if D13's measurements make them pure, or the budget needs 36 KB.

## 7. Layout, boot and the root

### 7.1 The floppy

```
byte 0       the bootblock (1 KB, 'DOS\0', checksum)  = ext2's boot area (ext2 starts its superblock at 1024)
byte 1024    ext2, 1 KB blocks, about 763 blocks: the read-only /usr
after it     the packed kernel (ZPAK), then the packed RAM root image (ZPAK), to the end of the disk
```

The ext2 filesystem simply ends before the two packed images (fsd and e2fsck never look past a volume's last block),
so there is no partition table and `diskd` needs nothing new: `/usr` is the device `df0`. The bootblock carries a small
table at its end (the two images' offsets, packed and unpacked sizes, the method), assembled in a demo build of
`boot/bootblock.S` (`handoff.S` unchanged; the plain bootblock stays byte for byte what it is).

### 7.2 The boot

1. Kickstart loads the bootblock, which reads the two packed images (sequential, near the end of the disk: about 4.5
   s, *estimate*) into `AllocMem` buffers, **unpacks each into a second buffer while AmigaOS still runs** (LZ4 in
   about 100 bytes of the 442 free; on any CPU it runs on), frees the packed copies, and goes on exactly as today
   (stage0, bootinfo with the RAM disk, kill, copy, jump). Peak while AmigaOS runs: about 450 KB.
2. The kernel and boot run as on any RAM floppy: `ram0` is the root. **One change in boot:** `net` is registered
   when the *name* `/sbin/netd` exists (`lstat`), since on this floppy it is a link into `/usr`, mounted only later.
   A dangling link on another system then registers a service whose start fails ("no netd") - the same as a broken
   netd today.
3. init prints `/etc/motd` (the welcome's first half, 9.3) and runs `mount -a`: `/etc/fstab` has `df0 /usr ext2 ro`.
   The image's `/etc/network/interfaces` has no `auto` line, so init starts no `ifup -a`: every boot is a first boot
   (the RAM root starts fresh), and the setup writes the file and brings the network up itself (9.2). Then the logins.

### 7.3 Read-only, for real

- **A new mount option `ro`** (fstab's options word, `mount -o ro`): mount starts fsd on that device in its existing
  read-only mode; every write answers `EROFS`. Without it, fsd could write the superblock's clean mark (D17) on a
  floppy a visitor left writable (a Gotek's ADF usually is).
- **A write-protected floppy** is reported by diskd (the drive's write-protect line, a flag in `diskinfo`) and served
  read-only even without `ro` (as AmigaOS does).
- **The disk must stay in the drive:** taking it out makes every program start and every page fail with an I/O error
  (the floppy driver already notices the change); the welcome says so.

### 7.4 netd's drivers from `/usr` (a D19 6.5 amendment)

D19 6.5: "netd calls only the root's filesystem server `fs` ...; never a `fs.*` mount", so that no filesystem netd
depends on can depend on `net`. Here netd starts `/sbin/net/genet`, a link into `/usr` (the mount `fs.df0`). The
reason of the rule holds (fsd on a floppy never calls `net`), so the amendment is: **"or a mount served by a local
filesystem server (fsd or zfsd on diskd); never a network mount"**. The other way, the drivers copied into the RAM
root, costs 20 KB of memory for the whole session.

## 8. Memory on the stock A1200 (*estimates*, measured in FS-UAE at step 4 and on the A1200 at step 7)

| | KB |
|---|---|
| Free with SLIP up and one login, disk root (D21 7.2, *measured*) | 1,159 |
| The RAM root (160 KB) instead of a disk | -160 |
| A second fsd for `/usr` (its image 28 KB, stack 16 KB, a 24 KB cache: D18) | -70 |
| The floppy driver's track buffers (allocated at first use, D18) | -24 |
| **Free after boot, network up** | **~900** |
| The box in memory after the first command (code; each running command adds its data, ~24 KB) | -45 |
| An `ssh` session (D21 7.2) | -135 |

So a visitor has about 700-900 KB to play with: one ssh session, telnet, a few shells. The 1 MB chip-only machine
(428 KB free with one login, D19 step 1) keeps about 170 KB after the demo's own costs: a shell and the small
commands, SLIP at most, no `ssh` or `wget`; the setup says so.

## 9. First boot: what the visitor sees

Every boot is a first boot (the RAM root starts from the image). Console 1 logs in as root without asking (as every
Zenix image does, `/etc/autologin`; 9.4); root's profile runs `/usr/lib/demo/setup`, a shell script, once per boot.

### 9.1 The keyboard

```
Keyboard:  1 USA   2 Svenska   (Enter, or 10 seconds: USA)
```

The choice goes to `/etc/keymap` and into the kernel through **a new `SYS_console` op `CON_KEYMAP`** (a name: `us`,
`sv`; root or the console's own process; the op number main's to claim) - the call `kbd_set_keymap` was written for.
`loadkeys NAME` (Linux's name) is the command; init applies `/etc/keymap` at every start on every Zenix (the
kernel's "later"). More layouts (German, British, French, ...) are 264 bytes each in the kernel's table, generated from
xkeyboard-config as `sv` was; which ones is an owner question. **`read -t SECONDS`** (bash's meaning) is added to sh
for the time limits.

### 9.2 The network

The script tries, in order, each stopping within 2 seconds when its hardware is absent: **the CM4's Ethernet**
(`genet`), **an A2065** (FS-UAE's and WinUAE's network card); for the first that comes up it writes
`/etc/network/interfaces` (`iface eth0 inet dhcp`, the driver) and runs `ifup eth0` without waiting for the lease. With
no card:

```
No network card found.  1 Serial cable to a PC (SLIP, 115200; see "man demo")
                        2 No network   (Enter, or 10 seconds)
```

This needs one contract from D19's drivers that may not hold today: **`ifup IFACE` ends with a non-zero status within
2 s when the driver finds no hardware** (main checks, and adds it if missing). The clock is set from the internet when
the network comes up (D21); randomness for ssh and HTTPS is ready by then (the floppy's index pulse is credited while
the motor runs, D19 9.3).

### 9.3 The welcome

`/etc/motd` before the setup, and the setup's last lines after it (no host names written in; the phone book is an
owner question):

```
 ZENIX - a Unix-like system for the Amiga, from one floppy.

 Real multitasking with checked memory, users and permissions, ten consoles
 (Amiga+F1 .. F10), a Bourne-style shell with job control, and the network.

   telnet HOST       a BBS, in colour, with Zmodem downloads (rz)
   ssh USER@HOST     your Linux machine
   wget URL          a web page, over HTTPS
   ls /usr/bin       everything on this disk;  man demo  to start

 Nothing is written to this floppy; your files live in memory until you
 switch off.  Network: eth0, 192.168.1.23 (DHCP).  Free memory: 912 KB.
```

### 9.4 Who is logged in

Root, as on every Zenix image (login prints its warning about a root without a password; on this floppy the warning
says instead that the system lives in memory and nothing listens on the network). A visitor can `useradd` nothing
(not on the floppy); the multi-user side is shown by the files' owners and `ls -l`. A `guest` login with `sudo` is
the alternative (owner question).

## 10. DD and HD, the two editions

- **DD 880 KB, the network edition:** 68 KB free (4.3). Works in every Amiga drive, every Gotek, every emulator.
- **HD 1.76 MB** (80 cylinders x 2 x 22 sectors): Kickstart's `trackdisk.device` reads it for the bootblock, but
  Zenix's floppy driver knows only 11 sectors a track, and HD drives are rare on the A1200 (a Gotek can be set to
  HD). **Not for the network edition**; for the desktop edition only if it cannot fit DD, decided in block 5.
- **A 2.06 MB format:** none of the standard Amiga formats has that size (DD 880 KB, HD 1.76 MB; the PC's 1.44 MB);
  an extended format would need its own driver work and work on few drives. Owner question.
- **The desktop edition (block 5):** the same mechanisms plus the display server, the terminal window, the bar and the
  file view, sizes unknown until they exist. On DD it would trade the network's heavy parts (`ssh`, `wget`, the
  anchors: ~200 KB) or need a stronger method 2 for cold files (ZX0, measured then); the `method` byte leaves room.

## 11. Building and testing

- **`make demo`** builds `build/zenix-demo.adf` with `tools/mkdemo.sh`: the box (`tools/mkbox.sh`: the rename and
  dispatcher of 6.1, a pure link), `tools/zpak` on every program and page, the RAM root image (`tools/mkroot.sh` with
  the demo's list and its `/etc`), the `/usr` image (`mke2fs -d`, read-only content, e2fsck clean), the bootblock's
  table, and the size table of 4.3 printed every build; over 880 KB is an error saying by how much.
- **On the PC** (`check.sh`): `tools/zpak-test.sh`: every packed file unpacks to itself with Zenix's decoder (the same
  C built for the host with its checks and with the address sanitizer), and a fuzzer feeds damaged and random
  streams: no read or write outside its buffers, a clean refusal.
- **In FS-UAE** (`tools/demo-test.sh`, part of `make check`, its own emulator slot and copies): boot `zenix-demo.adf`
  on the stock A1200 configuration (2 MB chip, no fast RAM) and on an A1200 with fast RAM and an A2065; check the
  welcome, the setup's defaults after their time, the keymap, every box member and separate command once, `man` on a
  packed page, a packed program refused when damaged (a damaged copy put into `/tmp`), the A2065's DHCP through the
  emulator, `free` against the budget of section 8, the box's second start reading nothing from the floppy (the floppy
  driver's counter), **the ADF unchanged afterwards** (its SHA-256 before and after, FS-UAE's floppy overlays off).
- **The kernel's ZPAK loader and the segment cache** also get tests on the normal disk image (a packed `hello`, a
  packed pure program in `/tests`), so they are tested on every image, not only on the demo.
- **On the real A1200** (the owner, step 7): a real drive and the Gotek; times to the welcome, to `sh`, `telnet`, the
  box's first and second command; `free`. Those are the numbers the docs may then state.

## 12. For the main session (interfaces to claim; not owner questions)

- The `ZPAK` format and method 1 (frozen once shipped, the methods append-only); the loader's refusal messages.
- `SYS_console` op `CON_KEYMAP` and `/etc/keymap` (one word); `loadkeys`; init applying it.
- `SYS_meminfo`'s new last word for reclaimable shared code (6.2).
- mount's `ro` option and how mount tells fsd (as boot passes `-fsck`); diskd's write-protect flag in `diskinfo`.
- boot's `lstat` test for `/sbin/netd` (7.2); D19 6.5's amendment (7.4).
- sh's `read -t`.
- The `ifup` no-hardware contract (9.2).
- The demo bootblock's table (not on the frozen list: only the demo's build reads it).

## 13. Build order (each step small, merged with its tests; the full suite before each merge)

0. **The floppy driver on real hardware** (the owner's A1200 with the Gotek; a real drive if one is at hand): a
   whole-disk read compared with the ADF. Nothing else is worth building before this passes.
1. **ZPAK:** the decoder (one source, kernel and libz), `tools/zpak`, the loader, `man`'s pages; host and FS-UAE
   tests; the LZ4 speed timed on the A1200 (5.2's estimate replaced).
2. **The segment cache** (6.2), with `free`'s word.
3. **The box** (`tools/mkbox.sh`, ZEX2), its tests.
4. **`mount -o ro`, the write-protect flag, boot's `lstat`, the D19 amendment, the demo bootblock, `tools/mkdemo.sh`,
   `make demo`**; `demo-test.sh` boots it in FS-UAE; section 8 measured.
5. **The setup and the welcome:** `CON_KEYMAP`, `loadkeys`, `/etc/keymap`, `read -t`, the `ifup` contract, the script,
   `man demo`.
6. **`wget`** when D21 step 7 has built it; the final budget; the licences checked against every program on the disk.
7. **The owner's A1200:** the times and memory of 11; then the owner's yes to hand it out (a GitHub pre-release of the
   ADF).

## 14. Open questions for the owner (one line each, the recommendation in brackets)

1. The keyboard question at every boot: which layouts? [USA and Svenska now; German and British added, 264 bytes each]
2. Root logged in on console 1, as on every Zenix image, or a `guest` user with `sudo`? [root; nothing listens]
3. A phone book of public BBSes on the floppy (`telnet` by name)? [yes, 5-10 boards you pick]
4. HD floppies: [DD only for the network edition; HD 1.76 MB only if the desktop edition does not fit; no 2.06 MB]
5. Save settings (keyboard, network) back to the floppy? [no: it is never written; a later idea]
6. A bigger `/tmp` on machines with fast RAM (a second RAM image, 1 MB, +15 KB on the floppy)? [yes, if it fits]
7. `edit`, `gzip`/`tar`, `cu` on the floppy while the budget allows? [yes]
8. Hand-out: [a GitHub pre-release ADF after the A1200 test; Aminet later, with your yes]

## 15. Reviews

(Revision 2 follows the two independent reviews.)
