# D25: the Zenix demo floppy, network edition (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 15); nothing built.** Roadmap block 2 ends with
**the Zenix demo floppy** (owner 2026-10-25, in the spirit of the 1999 QNX demo disk): "one 880 KB Amiga floppy that
boots into Zenix with the network, `telnet` and a web-page fetcher, to hand to anyone"; a second edition with the
desktop in block 5. The model (owner 2026-10-26) is KolibriOS: "the programs are packed on the disk and unpacked when
they start, which is what makes a desktop edition fit 880 KB". D19 7.4 and D21 7.1 left the floppy's sizes to this
note. The short version is DESIGN.md D25.

Facts about today's code are *read* from main at 7738665 (2026-10-28). Sizes marked *measured* come from building
that tree on the PC (`make`, the programs at -O2 as shipped) and packing its files with the packers named; a
*prototype* number comes from laying out the floppy's filesystem on the PC with those packed files (section 4.3).
Times are *modelled* (the CPU emulator Musashi's 68020 timing tables: no cache misses, no wait states) or *estimates*;
nothing in this note is timed on a real Amiga.

**Approval and the feature halt.** This note proposes new features (section 12 lists them: a packed program format in
the loader, keeping unused shared code, symbolic links across mounts, the box, `CON_KEYMAP` and `loadkeys`, sh's
`read -t`, a driver "no hardware" status, the demo's build). As with D21, **the owner's approval of this note is the
approval of those features**; none is built before it. **Prerequisites:** the security audit that the roadmap puts
before the demo floppy; D21 step 7 (`wget`, `sntp`); D13 step 4's decision on which programs become pure (the box
makes 36 small programs pure); two design reviews of the code of each kernel step (5.4, 6.2, 7.3) as for every deep
core change.

## 1. What the owner gets, in plain words

- **One DD floppy (880 KB, an ADF for emulators and Goteks) that boots any Amiga with a 68020 or better and 2 MB of
  memory**, the stock A1200 included. *Estimates* for the stock A1200: about **12 seconds from power-on to the
  keyboard question** and about **20 seconds to the welcome with the network up** (today's RAM floppy alone reads
  860 KB, about 33 s). Two questions with defaults (keyboard; network when no card is found), a short welcome, a
  shell. Nothing on the floppy is ever written; the visitor's files live in memory (about 90 KB of room) until the
  machine is switched off.
- **On it:** the shell, 50-odd everyday commands with their manual pages, the network (DHCP on an A2065 or the CM4's
  Ethernet; SLIP over a serial cable to a PC on any Amiga), `ping`, `host`, `telnet` with Zmodem, `ssh`, `wget` with
  HTTPS, the editor, `less`, `grep`, `gzip`/`tar`, `cu`, the PC font for BBSes. About 50 KB stay free (*prototype*,
  less the additions of revision 2).
- **How it fits:** every program is packed on the disk (LZ4, about 70% of its size) and unpacked by the kernel when it
  starts, in place in the memory the program gets anyway: no RAM disk holds the programs. The small commands are
  linked into one program (a "box", as BusyBox does) so their shared C library is on the disk once instead of 36
  times: that saves more than any packer could (section 6).
- **Snappy:** LZ4 was chosen because on a 14 MHz 68020 reading from a floppy it is the only packer that makes programs
  start *faster* than unpacked ones (section 5.3); the box stays in memory after its first use, so `ls`, `cat`, `ps`
  start in a fraction of a second after the first command.

## 2. Today (read and measured)

- **The RAM floppy** `zenix-ram.adf` (Makefile): the bootblock (1 KB; header and code 566 bytes, 458 free,
  *measured*), `kernel.bin` from byte 1024, then a 704 KB ext2 RAM-disk image (`RAMDISK_K`) that the bootblock loads
  into memory, where it stays for the whole session (D19 7.4); so the kernel may be at most 179,200 bytes
  (`KERNEL_MAX`). `kernel.bin` today: **155,944 bytes** (*measured*). The RAM disk carries a short fixed list
  (`RAMAPPS`), no manual pages, no network.
- **The bootblock** (`boot/bootblock.S`, `boot/handoff.S`): reads the kernel and the RAM image with
  `trackdisk.device` into one `AllocMem` buffer while AmigaOS lives, builds bootinfo, calls the kernel's stage0, kills
  the OS, copies the kernel home (chip `0x40000` on a chip-only machine, the copier at `0x500`) and jumps.
- **The root rule** (`user/rootpick.h`): a RAM disk (`ram0`) is the root. boot (`user/boot.c`) registers `net` ->
  `/sbin/netd` when `sys_stat("/sbin/netd")` succeeds, registers the lines of `/etc/svctab`, and starts `/bin/init`;
  with a RAM root it runs no fsck and never credits or writes a persistent seed (`boot.c` 246, 276). init prints
  `/etc/motd`, runs `/bin/mount -a` from `/etc/fstab`, then `/bin/ifup -a` when `/etc/network/interfaces` has an
  `auto` line, then `/bin/login --init` on the consoles (`/etc/autologin`: console 1 without asking).
- **Absolute paths in the programs** (*read*): init spawns `/bin/mount`, `/bin/login`, `/bin/ifup`; sh spawns
  `/bin/sh` and its default `PATH` is `/bin`; `man` spawns `/bin/less`; `telnet` spawns `/bin/rz`; `cu` spawns
  `/bin/cu`; `ifconfig` checks `/sbin/netd`; netd starts `/sbin/net/NAME` (frozen, D19) and reads `/etc/network/*`,
  `/etc/hosts`, `/etc/resolv.conf`; wget will read `/etc/ssl/anchors` (D21).
- **Symbolic links do not cross mounts** (*read*, a limit found by the first review): the kernel picks the mount from
  the path's text (architecture.md section 9) and the filesystem server follows a link inside its own volume
  (`ext2_namei_ex`, `kernel/ext2.c` 599-607: an absolute target restarts at that volume's root inode). So a link
  `/bin/ls -> /usr/bin/ls` on the root finds the root's own empty `/usr` and answers ENOENT.
- **The loader** (`kernel/proc.c`): ZEXE and ZEX2 (both frozen). ZEXE from a file (`fill_fs`) or from memory
  (`fill_mem`, the programs built into the kernel); **ZEX2 only from a file** (`v2 = magic == ZEX2 && fill ==
  fill_fs`, 478): `fill_v2` reads the code, the data image and the relocations through `fs_exec_read`, and **reads the
  data and relocations from the file at every start**, even when the code segment is shared. Every header field is
  checked before use (`zexe_bad`, `zex2_bad`); a spawn needs only the process's own memory plus a small relocation
  buffer. A segment is keyed by (mount slot, mount generation, inode, size, time) and **freed at once when its last
  user ends** (D13 7, stage 1; the note names the later rule: keep unused segments, drop them when memory runs short).
  `sh` and `login` are ZEX2; a program that sends raw messages (`mount`, `dd`, `fdisk`, ...) stays ZEXE (D13 3).
- **The floppy driver** (`kernel/floppy.c`): AmigaDOS tracks by Paula DMA, a whole track per read (about 213 ms of DMA
  that the driver polls, the kernel not preempted meanwhile), one decoded track cached, the motor off after 3 s idle
  and up to 800 ms to spin up again; `diskd` serves `df0`-`df3` as whole devices (no partitions); after a disk change
  every request fails until diskd opens the drive again. **Verified in FS-UAE only, never on a real drive or a Gotek**.
- **fsd** serves a volume with an unknown RO_COMPAT feature read-only when it is mounted (`kernel/ext2.c` 338-350,
  `EXT2_ANY`); its block cache is 24 blocks on a 2 MB machine and holds file data and metadata alike.
- **The keymap:** `sv` (the default) and `us` are built into the kernel (`kernel/kbd.c`), one setting for every
  console; `kbd_set_keymap(name)` exists but nothing calls it (the comment says "/etc/keymap later").
- **`ifup IFACE`** (`user/ifconfig.c`, `wait_attached`): after netd starts the driver it polls netd's interface list
  every 100 ms and gives up after **10 s** (status 1). The drivers end at once when the hardware table has no board
  (`net/drv/a2065.c` "no A2065 board in the hardware table", `net/drv/genet.c` "no GENET ..."), and **netd restarts a
  driver that ends** (D19 2: 1, 2, 4 s back-off, given up after 5 ends in 60 s).
- **Randomness:** the floppy's index pulse is credited 1 bit per track read by Zenix (`floppy.c`, `RND_CR_FLOPPY`),
  keys 1 bit each, Ethernet nothing; `/dev/random` waits for 256 bits.
- **sh** reads `/etc/profile`; its `read` has `-p` but no time limit (man/sh.txt).
- **Memory on the stock A1200** (2 MB chip, no fast RAM; FS-UAE, D19 *measured*): 1,347 KB free with one login on a
  disk root, 1,159 KB with SLIP up (netd 116 KB), 1,120 KB with the A2065 and DHCP; the 1 MB chip-only machine 410 KB.

## 3. The choices in short

| Question | Choice | Why (section) |
|---|---|---|
| Packer | **LZ4** (block format, packed at level 12 on the PC) everywhere: kernel, RAM image, programs, manual pages | the only one that beats a plain floppy read on a 14 MHz 68020 (5) |
| Where programs are unpacked | **by the kernel's loader, in place in the program's own memory** (a new wrapper `ZPAK`: the inner header plain, the code and the data each one LZ4 stream) | no RAM disk and no extra buffer for programs (5.4) |
| Small commands | **one "box"** (a multi-call pure program) for the light commands, kept in memory after use | one copy of libz on the disk: 4.9x smaller packed than separate files (6) |
| Root and writable files | **a 160 KB RAM-disk root** (`/etc`, `/home`, `/tmp`, `/var`, `init`, `mount`), the floppy's filesystem **read-only on `/usr`**, links from the root into it | today's root rule and every frozen path unchanged; nothing written to the floppy (7) |
| Links into `/usr` | **the kernel follows a symbolic link across mounts** (a limit fixed for every Zenix) | every absolute path in the programs keeps working (7.3) |
| Floppy layout | one ext2 filesystem over the disk (the Amiga bootblock in ext2's boot area), marked read-only, the packed kernel and RAM image after its end | no partition table, no new device naming, no new mount option (7.1) |
| Format | **DD 880 KB** only for the network edition | every Amiga drive and Gotek; HD only if the desktop edition needs it (10) |
| Keyboard | asked at every boot, a 10-second default | the floppy is for anyone (9) |
| Network | tried in order: the CM4's Ethernet, an A2065, else the visitor picks SLIP or none | no card needs configuring by hand (9) |

## 4. What goes on the floppy, and the budget

### 4.1 Contents

- **Booted by the bootblock** (outside the filesystem): the kernel (the same `kernel.bin` as every image, with `fsd`,
  `diskd` and `boot` inside, packed by the build) and the RAM-disk root image (packed).
- **The RAM root** (unpacked at boot, 160 KB, about 100 inodes): `/bin/init` and `/bin/mount` (plain: each runs once
  at boot, and `mount` sends raw messages, so it stays ZEXE); `/etc` (passwd, group, fstab, console, motd, autologin,
  hosts, profile, `svctab` with the line `net 0 /sbin/netd`, `sntp`, `network/` written by the setup); an empty
  `/usr` (the mount point), `/home/guest`, `/root`, `/tmp`, `/var/lib`, `/sbin/net` (the setup copies the one driver
  it uses there, 9.2); **links** `/bin/NAME -> /usr/bin/NAME` for every other command, `/sbin/netd ->
  /usr/sbin/netd`, `/etc/ssl/anchors -> /usr/share/ssl/anchors`, `/usr/lib` reached the same way. Free for the
  visitor's files: about 90 KB.
- **The floppy's filesystem, read-only on `/usr`:**
  - `bin/`: `sh` and `login` (ZEX2), **the box** (ZEX2: `ls cat cp mv rm mkdir rmdir ln echo pwd ps free uname uptime
    date clear kill dmesg sleep true false env hostname id whoami touch umount reboot head tail wc ifconfig route ping
    host netstat`, with `ifup`, `ifdown` and `bootamigaos`; each name a hard link to `box`), and on their own (ZEXE):
    `df` (6.1), `less` (`more`), `edit`, `man`, `grep`, `sort`, `gzip` (`gunzip`, `zcat`), `tar`, `cu`, `telnet`,
    `rz` (`sz`), `ssh` (ZEX2), `wget` and `sntp` (when D21 step 7 has built them).
  - `sbin/netd`, `sbin/net/a2065`, `sbin/net/genet` (SLIP is inside netd).
  - `lib/demo/setup` (the first-boot script, section 9).
  - `man/`: the pages of every command on the floppy plus `demo` (what this floppy is, the SLIP cable, where the PC
    side `zenix-slirp` and the source code are), each packed.
  - `share/consolefonts/cp437-8x8.psf` (D20, 2,052 bytes); `share/licenses/`: Zenix's own GPL-2 text and the notices
    of lwIP, BearSSL, Monocypher, miniz, regex.c, the Zmodem code and the font (the owner's rule, every image carries
    its notices; handing out the floppy is distribution under the GPL, so `man demo` names the exact source);
    `share/ssl/anchors` (D21's anchors file, plain: wget reads it by its index).
- **Left out:** `ssh-keygen` (a key made in memory is lost at power-off; `ssh` with passwords works), `zup`/`zpkg`
  (a read-only floppy is never updated: zpkg refuses system updates on a bootblock boot, D22), the disk tools
  (`fdisk`, `mkfs.*`, `fsck.*`: a demo never offers to format the visitor's disk), `awk` (100 KB), the test programs,
  `su`/`sudo`/`passwd`/`useradd` (one user session; 9.4).

### 4.2 The packers measured on today's files

Bytes, *measured* (`lz4 -12`, `gzip -9`, `zstd -19`, `xz -9e`, Shrinkler 4.7 `-d -9`; the programs as built at -O2; the
LZ4 figures include its 15-19-byte frame, a ZPAK file carries 16 bytes instead):

| File | Plain | LZ4 | deflate | zstd | xz | Shrinkler |
|---|---|---|---|---|---|---|
| `kernel.bin` | 155,944 | 103,965 | 88,199 | 82,719 | 75,650 | 77,303 |
| `ssh` | 106,424 | 73,805 | 62,903 | 58,516 | 53,393 | 54,218 |
| `netd` | 93,412 | 66,967 | 54,884 | 50,177 | 45,605 | 46,583 |
| `sh` | 87,124 | 57,687 | 47,927 | 45,267 | 41,480 | 42,034 |
| `telnet` | 31,280 | 23,146 | 18,585 | 17,494 | 15,881 | 15,939 |
| `ls` | 20,676 | 14,551 | 11,494 | 10,743 | 9,761 | 9,686 |
| 40 small tools, each packed alone (sum, in KB) | 391 | 276 | 219 | 209 | 193 | 189 |
| the same 40 as one stream ("solid"), bytes | 400,212 | 133,769 | 128,657 | 87,933 | 76,512 | 77,505 |
| the 53 manual pages they need, each alone | 96,792 | 66,894 | | | | |

Two findings decide most of the note: **code packs only to about 70% with LZ4 and 50-55% with the strongest packers**
(68k code is dense), and **the same 40 small tools packed together take half of what they take packed one by one**:
the duplicate is libz, 2-4.5 KB in every program (`hello` is 4,652 bytes, `true` 220). A packer cannot see across
files; linking them into one program removes the copies at the source (section 6).

### 4.3 The budget (DD, 901,120 bytes)

The *prototype* (`mke2fs -t ext2 -E revision=0 -b 1024 -N 144 -d` on a tree of the packed files, `wget` stood in by
73,000 incompressible bytes, D21's anchors file by 52,231): **695 of 763 blocks used, 68 free**, e2fsck clean; 3 of
its 144 inodes were left, so the build sizes `-N` from the file count and makes the command names hard links.

| Part | Bytes | How known |
|---|---|---|
| Bootblock (ext2's boot area) | 1,024 | layout |
| Kernel, LZ4 | 103,966 | *measured* (103,950 packed + 16) |
| RAM root image (160 KB ext2: init, mount, /etc), LZ4 | ~15,000 | *measured* 14,423 for a smaller tree; the full one packs about the same |
| Filesystem: `sh` 57.7 K, `login` 11.0 K, box 40.7 K, `df` 4.7 K, `less edit man grep sort gzip tar cu` 117.2 K, `telnet` 23.1 K, `rz` 17.6 K, `ssh` 73.8 K, `wget` ~73 K, `sntp` ~5 K, `netd` 67.0 K, drivers 19.7 K | ~512 KB | *measured* (wget and sntp *estimates*, D21 7.1) |
| Filesystem: manual pages ~67 K, anchors 51 K, the GPL text and notices ~22 K, font 2 K, setup ~3 K | ~145 KB | *measured* (setup *estimate*) |
| Filesystem: ext2's own (inode table, bitmaps, directories, indirect blocks, half a block per file) | ~55 KB | *prototype* |
| **Free** | **~50 KB** (5.5%) | *prototype* less revision 2's additions |

**The rule for the build** (as `tools/mkadf.py` today): the image is refused when over, saying by how much, and every
build prints each part and the free space. **If a later version is over, drop in this order:** the anchors file's long
tail (the ~25 roots that sign most of the web, about 12 KB instead of 51: frees ~40 KB); the manual pages of the
commands anyone knows (`ls`, `cat`, ...: ~20 KB); `tar`, `cu` (~17 KB); then the second box (6.3).

## 5. The packer

### 5.1 The candidates

- **LZ4** (block format): bytes and copies, no entropy coding; a small decoder, no tables.
- **Deflate** (miniz's `tinfl`, already in Zenix): Huffman plus LZ77; 7,752 bytes of code and an 11 KB state: too big
  for the kernel, where every Zenix user would carry it.
- **Shrinkler** (the demoscene's, Blueberry): a range coder with context models; the best ratio here; a ~200-byte
  decoder with 3 KB of tables on the stack (knowledge base `demoscene/research/research_notes/Blueberry Loonies
  source/shrinkler_cinter.md`).
- **ZX0** (and LZSA2): LZ with Elias-gamma lengths, an 88-byte 68000 decoder (*read*, the same note). **Not measured
  here** (its packer is not on this PC); expected between LZ4 and deflate in size and speed. The candidate for a
  second, stronger method if the desktop edition needs one (section 10).

### 5.2 Unpacking speed (*modelled*)

Each decoder was run in Musashi as a 68EC020 on the files above, its output compared byte for byte; cycles per
**unpacked** byte:

| Decoder | Code | Cycles per byte (68020 model) |
|---|---|---|
| LZ4, plain C at -O2, no checks | 328 bytes | 27-31 (text 35) |
| LZ4, plain C at -O2, input, output and offset checked (the second review's) | 394 bytes | 33-39 (text 44) |
| `tinfl` (deflate), C at -O2 | 7,752 bytes | 120-167 |
| Shrinkler's own 68000 decoder | ~450 bytes with its frame | 560-695 |

The same model on a 68000 gives Shrinkler 2,535 cycles per *packed* byte for `ls`, which matches the earlier
independent model of 2,500 (the same knowledge-base note): the model is consistent. **On the stock A1200** the code and
data are in chip RAM, shared with the display: the model's numbers are taken as **1.5-2x** for a decoder whose loop
fits the 68020's 256-byte instruction cache (Shrinkler's; LZ4 only when hand-written in assembler: the C one is 394
bytes) and **2-3x** otherwise - *estimates*, timed on the A1200 in step 1. At 14.18 MHz the checked LZ4 is then about
**200 KB/s** unpacked, deflate **35-50 KB/s**, Shrinkler **14-16 KB/s**.

### 5.3 What the visitor feels: start times from the floppy (*estimates*)

Assumed: the floppy delivers **20 KB/s** to a program load (one 5.5 KB track per about 250 ms: 9 ms to the sync word,
6,656 words of DMA at 32 us, the decode, a step every second track: about 22 KB/s at best, less with the filesystem's
own blocks); **plus 0.3-1.3 s for every cold start** (the motor's spin-up after 3 s idle, up to 0.8 s; the inode table
and directory blocks read again, since fsd's 24-block cache also carries the previous program's data); the stock A1200;
seconds, the fixed cost not included:

| Program | Plain | LZ4 | Deflate | Shrinkler |
|---|---|---|---|---|
| `sh` (at login) | 4.4 | **3.3** | 4.6 | 8.0 |
| `telnet` | 1.6 | **1.3** | 1.7 | 2.9 |
| `ssh` | 5.3 | **4.2** | 5.8 | 9.9 |
| `netd` (in the background) | 4.7 | **3.8** | 5.1 | 8.7 |
| the box, first command | 3.3 | **2.4** | 3.4 | 6.0 |
| the box, a later command (code kept in memory, 6.2; its 5 KB data stream read) | - | **~0.3** | | |
| **The boot**: kernel + RAM image (bootblock, 26 KB/s sequential) | 33 (today's RAM floppy) | **4.5 read + 1.6 unpack** | (no room in the bootblock) | 3.3 + 21 |

**The visitor's timeline** (*estimate*): Kickstart to the bootblock 1-2 s, the bootblock 6 s, mounting `/usr` 0.5 s,
`login` 0.6 s, `sh` 3.3 s: **about 12 s to the keyboard question**; then the box's first command, netd, a driver and
DHCP: **about 20 s to the welcome with the network**.

On a 14 MHz 68020 the floppy and the CPU are about equally slow: from the figures above, **LZ4 beats an unpacked read
while the floppy is slower than about 70 KB/s and beats deflate while it is faster than about 6 KB/s**; deflate breaks
even and Shrinkler doubles every start. On the 68060 and on the PiStorm every packer unpacks faster than the floppy
reads, so the smallest file would win there; but the floor sets the choice (CLAUDE.md, the stock A1200), and LZ4 fits
with room (4.3). **One method for everything** (bootblock, kernel, manual pages) keeps one decoder.

### 5.4 The packed program: `ZPAK` (a new format, frozen once shipped)

```
+0  u32 inner size         bytes of the original file's header that follow, plain (40 or 48)
+4  'ZPAK'                 (at +4 like 'ZEXE' and 'ZEX2', so the loader reads one place)
+8  u8  method             (1 = LZ4 block; others refused; new methods append)
+9  u8  0
+10 u16 header size        (32 + inner size; a reader skips what it does not know)
+12 u32 stream 1 packed    the shared part (ZEX2: text_size bytes) or the image (ZEXE: image_size bytes)
+16 u32 stream 2 packed    the rest of the original file after stream 1 (ZEX2: data image and relocations;
                           ZEXE: relocations), 0 if none
+20 u32 stream 2 offset    file offset of stream 2
+24 u32 original size      the size of the unpacked file (every check of zexe_bad/zex2_bad uses it as fsize)
+28 u32 0
+32 the original ZEXE/ZEX2 header, plain; then stream 1; then stream 2
```

- **The inner header is plain**, so the loader checks it with the unchanged `zexe_bad`/`zex2_bad` (with the original
  size as the file size) **before allocating anything**, and for a ZEX2 program looks up the segment (`seg_find`)
  **before unpacking anything**: a cached box start unpacks only stream 2 (the box's: 2,720 bytes of data and 640
  relocations, about 5 KB).
- **In place, no extra buffer:** stream 1 is read into the top of the memory it unpacks into (a ZEXE's own block,
  which is image + BSS + stack + arguments; a ZEX2's code block, allocated as text + the margin) and unpacked forward
  from the bottom; stream 2 the same into the data block and into the relocation buffer `fill_fs` already uses. The
  margin is LZ4's own rule, `(packed >> 8) + 32` bytes (`LZ4_DECOMPRESS_INPLACE_MARGIN`; 1-6 bytes were needed for
  the files measured). A spawn then needs what a plain spawn needs, plus at most the margin.
- **A memory source for `fill_v2`:** the ZEX2 path reads its parts through `exec_src`; ZPAK adds a source that unpacks
  instead of reading, and keeps the packed file's identity as the segment's key. This and the in-place rule are new
  loader code: a loader change, reviewed twice.
- **Untrusted input** (CLAUDE.md): the header size at least 32 and inside the file; each stream inside the file; each
  unpacked size at most 255 x its packed size and at most `ZEXE_MEM_MAX`; every literal and copy bounded by the output
  end, every match offset by the bytes already produced, every read by the input end; **the produced length exactly
  the expected one** (else the tail would be old memory read as a program). A ZPAK inside a ZPAK, an unknown method or
  any failed check: "exec: NAME refused: ...", never a crash. The memory being filled belongs to the new pid from the
  start, so a kill during loading frees it. The instruction cache is flushed as after every load.
- **Not one long stop:** the kernel is not preempted (architecture.md, D6), and unpacking `sh` takes about 0.4 s,
  `ssh` 0.55 s (*modelled* with the chip factor): longer than the 4 KB serial ring lasts at 115,200 baud (about 0.36
  s), so SLIP would lose frames while a program starts. The decoder runs in slices of 16 KB with the scheduler's
  check between them. (The floppy driver's own 0.25 s of polled DMA per track is the same kind of stop, today's, a
  matter for the driver: listed for main, not changed here.)
- **`zpak` on the PC** (`tools/zpak.c`, built by the host compiler as `zenix-slirp` is) uses the LZ4 library's own
  compressor (BSD-2-Clause, kept with its licence header) at level 12 and stores a file plain when packing would not
  shrink it; a host test unpacks every packed file with Zenix's own decoder (the same C) and compares.
- **Data files** (`man`'s pages) use the same header with no inner header and one stream; `man` unpacks them with a
  libz helper built **from the same decoder source** as the kernel's (one source for the same code, D13).
- **Not in packages:** zpkg packages and D15 bundles carry plain files; ZPAK is made only by the demo floppy's build.

## 6. The box, and keeping shared code in memory

### 6.1 The box (a multi-call program, BusyBox's idea)

**Measured:** the light commands of 4.1 linked into one program (each one's global names renamed with `objcopy`, a
short `main` that picks the command by the name it was started as): as a ZEXE 65,732 bytes plain and 43,300 LZ4,
against 293,764 plain and 211,650 LZ4 as 37 separate files; **as the pure ZEX2 it will be** (built by the second
review with `tools/purecc.sh`, without `df`): **54,368 bytes of shared code, 15,124 bytes of table, data and BSS per
process, 40,716 LZ4**. Built from the same sources; only the link differs, and only on this floppy (hard disks keep
separate files). Rules:

- A box command runs exactly as the separate program does: its own process, its own data and stack, `argv[0]` as
  given (`reboot` answers to `bootamigaos`, `ifconfig` to `ifup` and `ifdown`: the box's table lists every name; an
  unknown name prints the list and exits 2).
- **Members:** light commands that send no raw messages (D13 3; checked: none of them calls `sys_call`), pass D13's
  `-Wwrite-strings` review (checked: clean), need no stack above the default 16 KB (checked: none has a `STACK_`
  line) and link no absolute relocation into the shared part. **`df` is not a member:** it needs libgcc's 64-bit
  division, whose table `__clz_tab` is reached by an absolute address, which mkzexe refuses in a pure program (verified
  by the second review); it stays a separate ZEXE until a pure libgcc exists (D13 8, as for awk).
- **Its cost:** each running member carries the whole box's data and BSS, about 31 KB with its stack. A pipe of two
  box commands while the box is not yet in memory takes about 116 KB (two data parts and the code), against about 67 KB
  for `ls` and `wc` as separate programs; once the box's code is in memory, each further member costs 31 KB.
- **Tests:** every member runs from the box in the demo test (section 11), and the commands test script
  (`rootfs/tests/cmds.sh`) runs once against a box build.

### 6.2 Unused shared code stays in memory (D13's later rule, built now)

Without it every `ls` would read and unpack the whole box again (2.4 s); with it, a later command reads only the box's
data stream. **The rule (D13 7, as that note planned it):** when the last process using a segment ends, the segment
stays on an "unused" list (oldest first); a new start with the same key takes it again (after D13's reuse checksum,
10-25 ms for sh, counted in 5.3's 0.3 s); **when any allocation fails** (a program's memory, a kernel buffer such as
the relocation table), the kernel frees **all** unused segments (one old one may not leave a big enough block) and
tries once more. A cached segment never blocks anything: opening its file for writing, unlinking it, `umount` of its
volume (never EBUSY because of it), a disk change, a filesystem server's restart (the mount generation) or a different
key drops it; D13's `ETXTBSY` then means "a segment with users, or a load in progress". `free` counts it as
reclaimable (a new word at the end of `SYS_meminfo`, main's to claim). This helps every Zenix machine (a hard-disk
`sh` starts again at once too) and is a deep core change with its own tests (`tools/memtest.sh`: fill memory past
the cache, the segments dropped, the allocation succeeds; a changed file never runs old code; `umount` succeeds).

### 6.3 A second box?

The text tools (`less edit man grep sort`, sharing `regex.c`) as one program are 41,562 LZ4 against 77,664 separate
(*measured*), but they are not pure today, so every start would read the whole box. **Not in the first version**;
taken only if D13 step 4 makes them pure, or the budget needs 36 KB.

## 7. Layout, boot and the root

### 7.1 The floppy

```
byte 0       the bootblock (1 KB, 'DOS\0', checksum)  = ext2's boot area (ext2 starts its superblock at 1024)
byte 1024    ext2 revision 0, 1 KB blocks, about 760 blocks, the RO_COMPAT bit "read-only": the /usr
after it     the packed kernel (ZPAK), then the packed RAM root image (ZPAK), to the end of the disk
```

The ext2 filesystem ends before the two packed images (fsd never compares a volume's size with the device's; e2fsck
accepts a longer device), so there is no partition table and `diskd` needs nothing new: `/usr` is the device `df0`
(served whole, `user/diskd.c` 147-160). With 1 KB blocks ext2's first data block is 1 (fsd requires it), so block 0
is free for the bootblock; the build writes the bootblock after `mke2fs`. **Read-only without a new option:** the
build sets ext2's `read-only` RO_COMPAT feature (`tune2fs -O read-only`; e2fsck stays clean, checked by the first
review), so fsd serves the volume read-only whatever fstab says (and Linux refuses to mount it writable); fsd writes
nothing to it, not even the clean mark. `/etc/fstab` has `df0 /usr ext2 defaults`. The bootblock carries a small table
at its end (the two images' offsets, packed and unpacked sizes, the method), assembled in a demo build of
`boot/bootblock.S` (`handoff.S` unchanged; the plain bootblock stays byte for byte what it is).

### 7.2 The boot

1. Kickstart loads the bootblock from **DF0**. A bootblock started from another drive (its I/O request names the
   unit) refuses and returns to Kickstart, which asks for a disk: `/usr` is always `df0`. It reads the two packed
   images (sequential, near the end of the disk) into `AllocMem` buffers, **unpacks each into a second buffer while
   AmigaOS still runs** (an LZ4 decoder of about 100 bytes, with the output-end check, in the 458 free bytes), frees
   the packed copies, and goes on exactly as today (stage0, bootinfo with the RAM disk, kill, copy, jump). **Placement
   (zenixboot's rule, architecture.md section 3):** an unpacked buffer that lands on the kernel's future home, the
   klog area, low memory or the copier at `0x500` is allocated again elsewhere. Peak while AmigaOS runs: about 450 KB;
   tested on 1 MB and 2 MB chip-only machines.
2. The kernel and boot run as on any RAM floppy: `ram0` is the root (no fsck, no seed written). boot's own `net`
   registration finds no `/sbin/netd` yet (`/usr` is not mounted), so **the demo's `/etc/svctab` carries `net 0
   /sbin/netd`**: boot registers it from there; the path is resolved when netd first starts, after `/usr` is mounted
   (D19 6.3 kept `net` out of svctab only because of D15's `.new` merges, which a never-updated RAM image does not
   have). boot's code is unchanged.
3. init prints `/etc/motd` (the welcome's first half, 9.3) and runs `mount -a`. The image's
   `/etc/network/interfaces` has no `auto` line, so init starts no `ifup -a`: every boot is a first boot (the RAM root
   starts fresh), and the setup writes the file and brings the network up itself (9.2). Then the logins.

### 7.3 Links across mounts (a limit fixed for every Zenix)

Every absolute path the programs use (section 2) must keep working, and the RAM root cannot hold the programs. So
**the kernel follows a symbolic link whose target leaves the volume**: when a filesystem server meets, during a walk,
a link with an absolute target (or a `..` above its volume's root), it answers with a new reply that carries the link's
target and the rest of the path; the kernel joins them, normalises the text again (`path_abs`), picks the mount again
and goes on, counting the links followed against the existing limit (ELOOP). A new reply kind in `fsproto.h`
(append-only, main's to claim), implemented in fsd and zfsd, a kernel change reviewed twice; it is Unix's behaviour,
and today's limit (a link from one mount into another answers ENOENT) is a finding for main in any case (CLAUDE.md:
fix what is found). The other ways were weighed: copying every program the paths name into the RAM root (`sh`,
`login`, `less`, `rz`, `cu`, netd: over 200 KB of memory for the session), or rewriting the paths in the programs (a
copy of every program for the demo, against D13's one-copy rule).

### 7.4 netd's driver

netd starts `/sbin/net/NAME`, and D19 6.5 keeps netd off every mount but the root's. **The setup copies the one driver
it found** (a2065 8.7 KB, genet 17.9 KB plain) **into the RAM root's `/sbin/net`**, so no rule changes, only a machine
with a card pays, and the network keeps running if the floppy is taken out.

### 7.5 Why a RAM root (and not the floppy as the root)

A root that is not a RAM disk is a persistent disk root to boot: `fsck.ext2 -y` when it is not clean and the random
seed rewritten at every start (`boot.c` 246, 276-277) would write to the floppy, and a volume marked read-only is never
taken as the root at all (architecture.md section 10). A RAM root keeps every rule as it is; `/etc` is writable for
the setup; the visitor's files have a place.

### 7.6 When the floppy is taken out

Every request on `df0` fails after a disk change until diskd opens the drive again; fsd keeps it open, and `umount` is
on the floppy itself. **diskd opens the drive again by itself when a disk with the same volume id is put back**
(`volid.h`, the id mount already records), and the box's cached code is dropped (6.2). The welcome and `man demo` say
"leave the floppy in DF0". If `mount -a` fails at boot (no `/usr`), mount reports the line and init's retries print
"init: cannot start /bin/login ..." every 5 s; the demo's `/etc/motd`, printed just before, says what that means ("if
login cannot start, the Zenix floppy is not in DF0"), so init stays unchanged.

## 8. Memory on the stock A1200 (*estimates*, measured in FS-UAE at step 4 and on the A1200 at step 7)

| | KB |
|---|---|
| Free with SLIP up and one login, disk root (D19, *measured*) | 1,159 |
| The RAM root (160 KB) instead of a disk | -160 |
| A second fsd for `/usr` (40 KB image and BSS, 16 KB stack, 24 KB cache, a kernel stack) | -82 |
| The floppy driver's track buffers (allocated at first use, D18) | -24 |
| The driver copied into the RAM root (an Ethernet machine only; SLIP needs none) | 0 to -18 |
| **Free after boot, network up** | **~890** |
| The box's code in memory after the first command | -55 |
| Each running box command (data and stack) | -31 |
| An `ssh` session (D21 7.2) | -135 |

So a visitor has about 650-850 KB to play with: an ssh session, telnet, a few shells. Loading a program needs nothing
beyond its own memory (5.4). **The 1 MB chip-only machine** (410 KB with one login) keeps about 145 KB after the
demo's own costs: a shell and the box's commands, **no network** (netd alone takes 116 KB); the setup says so and
skips it.

## 9. First boot: what the visitor sees

Every boot is a first boot (the RAM root starts from the image). Console 1 logs in as root without asking (as every
Zenix image does, `/etc/autologin`; 9.4); root's profile runs `/usr/lib/demo/setup`, a shell script, once per boot.

### 9.1 The keyboard

```
Keyboard:  1 USA   2 Svenska   (Enter, or 10 seconds: USA)
```

The choice goes into the kernel through **a new `SYS_console` op `CON_KEYMAP`** (a name: `us`, `sv`; **root only**,
with no descriptor in the op word as `CON_COLOURS`, since the keymap is one setting for every console; the op number
main's to claim, after the ops D24 proposes) - the call `kbd_set_keymap` was written for. The command is `loadkeys
NAME` (Linux's name), and the setting is **a `keymap NAME` line in the existing `/etc/console`** (init reads that file
and already reports unknown words), which init then applies at every start on every Zenix. More layouts (German,
British, French, ...) are 264 bytes each in the kernel's table for every Zenix user, generated from xkeyboard-config as
`sv` was. **`read -t SECONDS`** (bash's meaning) is added to sh for the time limits.

### 9.2 The network

The script tries, in order, **the CM4's Ethernet** (`genet`), then **an A2065** (FS-UAE's and WinUAE's network card).
For the first one that comes up it copies the driver into `/sbin/net` (7.4), writes `/etc/network/interfaces`
(`iface eth0 inet dhcp`, the driver), runs `ifup eth0` and starts `sntp` (D21: a stock A1200 has no battery clock,
and HTTPS checks certificate dates). With no card:

```
No network card found.  1 Serial cable to a PC (SLIP, 115200; see "man demo")
                        2 No network   (Enter, or 10 seconds)
```

**Today an absent card costs 10 s** (ifup's wait, while netd restarts the ending driver five times, section 2), 20 s
for both. **The change (an append to the frozen driver contract, main's to claim):** a driver that finds no hardware
ends with a status that means "no such hardware" (sysexits' 69, `EX_UNAVAILABLE`); netd does not restart it, marks
the interface, and answers `ifup` at once with -ENODEV and the driver's reason; then an absent card costs well under
a second (*estimate*).

**Randomness is not ready by then:** Zenix's own floppy reads give about 40 bits and the pool needs 256 (section 2),
so the first `ssh` or HTTPS fetch may say "waiting for the random pool (type on the keyboard)". The setup's questions
ask for keys anyway, and the welcome says so; whether the floppy should credit more (each sector's timing) is a
question for D19's main session, not decided here.

### 9.3 The welcome

`/etc/motd` before the setup, and the setup's last lines after it (no host names or addresses written in; ADDRESS is
filled in from `ifconfig`, or "waiting for DHCP"; the phone book is an owner question):

```
 ZENIX - a Unix-like system for the Amiga, from one floppy.

 Real multitasking with checked memory, users and permissions, ten consoles
 (Amiga+F1 .. F10), a Bourne-style shell with job control, and the network.

   telnet HOST       a BBS, in colour, with Zmodem downloads (rz)
   ssh USER@HOST     your Linux machine (the first time, type a few keys)
   wget URL          a web page, over HTTPS
   ls /usr/bin       everything on this disk;  man demo  to start

 Nothing is written to this floppy: leave it in DF0.  Your files live in
 memory (about 90 KB) until you switch off.  Network: eth0, ADDRESS (DHCP).
```

### 9.4 Who is logged in

Root, as on every Zenix image; login stays as it is (its warning about a root without a password shows), and the
welcome adds that the system lives in memory and nothing listens on the network (no telnet server, no `/etc/daemons`).
The multi-user side is shown by the files' owners and `ls -l`. A `guest` login with `sudo` is the alternative (owner
question).

## 10. DD and HD, the two editions

- **DD 880 KB, the network edition:** about 50 KB free (4.3). Works in every Amiga drive, every Gotek, every emulator.
- **HD 1.76 MB** (80 cylinders x 2 x 22 sectors): Kickstart's `trackdisk.device` reads it for the bootblock, but
  Zenix's floppy driver knows only 11 sectors a track, and HD drives are rare on the A1200 (a Gotek can be set to HD).
  **Not for the network edition**; for the desktop edition only if it cannot fit DD, decided in block 5.
- **A 2.06 MB format:** none of the standard Amiga formats has that size (DD 880 KB, HD 1.76 MB; the PC's 1.44 MB); an
  extended format would need its own driver work and would work on few drives.
- **The desktop edition (block 5):** the same mechanisms plus the display server, the terminal window, the bar and the
  file view, sizes unknown until they exist. On DD it would trade the network's heavy parts (`ssh`, `wget`, the
  anchors: ~200 KB) or need a stronger method 2 for cold files (ZX0, measured then); the `method` byte leaves room.

## 11. Building and testing

- **`make demo`** builds `build/zenix-demo.adf` with `tools/mkdemo.sh`: the box (`tools/mkbox.sh`: the rename and the
  dispatcher of 6.1, a pure link), `tools/zpak` on every program and page, the RAM root image (`tools/mkroot.sh` with
  the demo's list and its `/etc`), the `/usr` image (`mke2fs -d` with `-N` from the file count, hard links for the
  names, `tune2fs -O read-only`, e2fsck clean), the bootblock's table, and the size table of 4.3 printed every build;
  over 880 KB is an error saying by how much.
- **On the PC** (`check.sh`): `tools/zpak-test.sh`: every packed file unpacks to itself with Zenix's decoder (the same
  C built for the host, also with the address sanitizer), and a fuzzer feeds damaged and random streams and headers:
  no read or write outside the buffers, an exact length or a clean refusal.
- **In FS-UAE** (`tools/demo-test.sh`, part of `make check`, its own emulator slot and copies): boot
  `zenix-demo.adf` on the stock A1200 configuration (2 MB chip, no fast RAM), on 1 MB chip-only, and on an A1200 with
  fast RAM and an A2065; check the welcome, the setup's defaults after their time, the keymap, every box member and
  separate command once, `man` on a packed page, a damaged packed program refused (a damaged copy in `/tmp`), the
  A2065's DHCP, `free` against section 8, a box command's second start reading only its data stream (the floppy
  driver's counter), the disk taken out and put back, **`netstat -l` empty, no write to any device but `ram0`, the ADF
  unchanged afterwards** (its SHA-256 before and after, FS-UAE's floppy overlays off).
- **Links across mounts, the ZPAK loader and the segment cache** also get tests on the normal disk image (a link into
  a mounted volume, a packed `hello`, a packed pure program in `/tests`, `umount` with a cached segment), so they are
  tested on every image, not only on the demo.
- **On the real A1200** (the owner, step 7): a real drive and the Gotek; the times of 5.3, `free`; those are the numbers
  the docs may then state.

## 12. For the main session (interfaces to claim; the owner approves them with the note)

- The `ZPAK` format and method 1 (frozen once shipped; methods append-only); the loader's refusal messages.
- The reply kind for a link that leaves its volume (`fsproto.h`), in the kernel, fsd and zfsd.
- `SYS_meminfo`'s new last word for reclaimable shared code (6.2).
- `SYS_console` op `CON_KEYMAP` (root only); `/etc/console`'s `keymap` word; `loadkeys`; init applying it.
- The drivers' "no such hardware" exit status (69), netd's handling and `ifup`'s -ENODEV (9.2).
- sh's `read -t`.
- diskd opening a drive again when the same volume comes back (7.6).
- For the floppy driver, not changed here: the 0.25 s of polled DMA per track that stops the system (5.4).
- For D19: whether the floppy should credit more randomness (9.2).
- The demo bootblock's table and the demo's own `/etc/svctab` line (not on the frozen list: only the demo reads them).

## 13. Build order (each step small, merged with its tests; the full suite before each merge)

0. **The floppy driver on real hardware** (the owner's A1200 with the Gotek; a real drive if one is at hand): a
   whole-disk read compared with the ADF. Nothing else is worth building before this passes.
1. **Links across mounts** (7.3): the reply, fsd, zfsd, the kernel; tests on the disk image.
2. **ZPAK** (5.4): the decoder (one source, kernel and libz), `tools/zpak`, the loader for ZEXE and ZEX2, `man`'s
   pages; host, fuzz and FS-UAE tests; the LZ4 speed timed on the A1200 (5.2's estimate replaced).
3. **The segment cache** (6.2), with `free`'s word.
4. **The box** (`tools/mkbox.sh`, ZEX2), its tests; after D13 step 4's decision.
5. **The demo bootblock, `tools/mkdemo.sh`, `make demo`, `demo-test.sh`** in FS-UAE; diskd's re-open; section 8
   measured.
6. **The setup and the welcome:** `CON_KEYMAP`, `loadkeys`, `/etc/console`'s word, `read -t`, the drivers' status,
   the script, `man demo`.
7. **`wget` and `sntp`** when D21 step 7 has built them; the final budget; the licences checked against every program
   on the disk.
8. **The owner's A1200:** the times and memory; then the owner's yes to hand it out.

## 14. Open questions for the owner (one line each, the recommendation in brackets)

1. The default keyboard, USA (for anyone) or Svenska (yours)? [USA, Svenska one key away]
2. More layouts than USA and Svenska (each 264 bytes in every Zenix)? [German and British]
3. Root logged in on console 1, as on every Zenix image, or a `guest` user with `sudo`? [root; nothing listens]
4. A phone book of public BBSes on the floppy (`telnet` by name)? [yes, 5-10 boards you pick]
5. HD floppies for the desktop edition if it does not fit DD? [DD for the network edition; decide in block 5]
6. Save settings (keyboard, network) back to the floppy? [no: it is never written]
7. `edit`, `gzip`/`tar`, `cu` on the floppy while the budget allows? [yes]
8. The source: the floppy is GPL software handed out, and the repository is private today. [make it public, with a
   source link in `man demo`, before the first hand-out]
9. Hand-out: [a GitHub pre-release ADF after the A1200 test, with `zenix-slirp` for the PC; Aminet later]

## 15. Reviews

Two independent reviewers read revision 1 (2026-10-28), told not to edit: one on correctness, size and speed (it
rebuilt, re-measured, built the box pure and benchmarked a checked decoder), one on architecture fit, frozen
interfaces and the stock A1200. What they found and what revision 2 did:

**Correctness, size and speed:**
1. *Blocker:* links from the RAM root into `/usr` cannot work: a link is followed inside its own volume. -> 7.3:
   the kernel follows links across mounts (a limit found for main too); the other ways weighed.
2. *Blocker:* the ZPAK plan ("from memory, every check unchanged") does not fit ZEX2, which loads only from a file. ->
   5.4: a memory source for `fill_v2`; a loader change reviewed twice.
3. *Blocker:* a later box command would read and unpack the whole box again (`fill_v2` reads data and relocations
   from the file each time). -> ZPAK keeps the inner header plain and packs code and data as two streams; a later
   start unpacks only the 5 KB data stream (~0.3 s with the fixed costs).
4. The in-place margin was not LZ4's rule; corrupt-input checks missing. -> LZ4's macro, store plain when it does not
   shrink, exact length, 255x cap, all bounds listed.
5. The load peak was understated (an extra 87-106 KB buffer). -> unpacked in place into the program's own memory.
6. Long stops: an unpack in the non-preempted kernel overruns the 4 KB serial ring. -> slices of 16 KB; the floppy's
   own polled DMA listed for main.
7. Start times left out the motor's spin-up and the metadata re-reads. -> 0.3-1.3 s per cold start in 5.3.
8. The benchmark's decoder had no checks; the C decoder does not fit the instruction cache. -> the checked figures
   (33-39 cycles, about 200 KB/s), the cache premise only for an assembler decoder.
9. The boot time hid the visitor's wait. -> the timeline: ~12 s to the question, ~20 s to the welcome.
10. Randomness is not ready after boot. -> said in 9.2 and the welcome; a question for D19's main.
11. `df` cannot be pure (libgcc's `__clz_tab`); the box's real sizes; the pipeline cost; the names. -> `df` separate;
    the pure box's measured sizes (54,368 code, 31 KB per member); the cost said; an unknown name lists and exits 2.
12. Inodes run out (3 of 144 free). -> hard links for the names, `-N` from the count, revision 0 recorded.
13. Zenix's GPL text, Monocypher's and regex.c's notices and the source pointer missing. -> in 4.1 and the budget.
14. The RAM-root prototype was incomplete; the visitor's room unsaid. -> ~100 inodes, ~90 KB of room, said.
15. No way back after the disk is taken out. -> 7.6.
16. Section 8's numbers. -> fsd 82 KB, the box 55 + 31 KB, no load peak, the 1 MB paragraph corrected (no network).
17. Byte figures that were KB sums; kernel 103,965 vs 103,969; "3x". -> the row in KB; 103,966; 4.9x.
18. Layout checks confirmed; the bootblock's decoder needs its output check; the bootblock written after mke2fs. ->
    7.1, 7.2.
19. Segment-cache traps (umount, freeing all on failure, ETXTBSY, the checksum time). -> 6.2.
20. ZPAK header checks. -> 5.4.

**Architecture fit, frozen interfaces, the stock A1200:**
1. *Blocker:* the ZPAK loader promise (as correctness 2). -> the same fix.
2. `mount -o ro` and a write-protect flag are not needed: ext2's `read-only` RO_COMPAT bit makes fsd serve it
   read-only (checked with `tune2fs`, e2fsck clean). -> taken; both new options dropped.
3. boot's `lstat` change is not needed: the demo's own `/etc/svctab` line registers `net`. -> taken; boot unchanged.
4. The D19 6.5 amendment had no enforcement (an NFS `/usr` would deadlock netd). -> dropped; the setup copies the one
   driver into the RAM root.
5. `df0` hard-coded: a boot from DF1 leaves no `/usr` and no shell; `/usr` missing as a mount point. -> the bootblock
   refuses another drive; `/usr` listed; init's message and the motd hint (7.6).
6. `CON_KEYMAP`: a global setting must be root only; D24's proposed ops; a new `/etc/keymap` format not needed. ->
   root only, numbered after D24's, a word in `/etc/console`; init applying it is part of the approval.
7. The feature halt and the prerequisites were not stated. -> the approval paragraph at the top.
8. The `ifup` contract changes D19's driver contract (netd restarts ending drivers). -> the "no hardware" status 69,
   netd's and ifup's handling, claimed by main.
9. `sntp` was missing (the stock A1200 has no battery clock). -> on the floppy, started by the setup.
10. The bootblock's buffers must avoid the kernel's home, the klog area and the copier. -> zenixboot's rule in 7.2.
11. Section 8 should use the latest measurements; the 1 MB machine cannot run SLIP and the box. -> 1,347 / 410 KB; no
    network on 1 MB.
12. A demo mode inside login would be a side path. -> login unchanged; the welcome says it.
13. The example address; the welcome cannot show a lease ifup does not wait for. -> ADDRESS or "waiting for DHCP".
14. Visitor safety confirmed by reading (rootpick and diskd only read, no fsck or seed on a RAM root, the battery clock
    never written); add checks. -> `netstat -l` empty, no write but to `ram0`, the disk change, in 11.
15. Say why a RAM root. -> 7.5.
16. D13 details (members checked, ETXTBSY, the 255x cap, the buffer owned by the new pid). -> 5.4, 6.1, 6.2.
17. Missing owner questions: the source for a GPL hand-out, the default keyboard; each keymap costs every user. ->
    questions 1, 2 and 8; two questions that were technical (2.06 MB, a bigger `/tmp`) decided instead (10; not taken).
