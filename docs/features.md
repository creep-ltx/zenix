# Zenix feature list

**This is the one list of what Zenix can do.** It is kept in step with the code (rule in `../CLAUDE.md`): when a
feature is added, changed or removed, its line here changes in the same commit. `README.md` only summarises it.

Status words: **verified** = built and run in FS-UAE (an emulator; no feature has yet run on a real Amiga);
**read** = checked in the source only; **planned** = not built. Last full update: 2026-10-22.

## Machine support
| Feature | Status | Details |
|---|---|---|
| 68020, 68030, 68040, 68060 (soft float, no MMU use) | verified | architecture.md section 2; matrix of 13 emulated setups |
| 68000 refused with a message (red screen, serial text) | verified | start.S |
| Chip and fast RAM pools, 1 MB chip up to 8 MB+ fast | verified | architecture.md section 5 |
| Boot from floppy (ADF) bootblock; kernel in fast RAM if present | verified | how-it-works.md section 1 |
| Boot with a RAM-disk root loaded from the floppy (no IDE disk) | verified (dev/test image) | `zenix-ram.adf` |
| Gayle IDE disk with Amiga RDB partitions (A600/A1200/A4000); its interrupt sources switched off (polling only) | verified (the interrupt quieting only in FS-UAE) | architecture.md section 2 |
| Instruction-cache (and Emu68 JIT) flush after every write of code; built for 68020-68060 with an opcode check | verified in FS-UAE, read for Emu68 | architecture.md section 2 |
| Battery clock (MSM6242, RP5C01) | verified in emulator | `kernel/rtc.c` |
| Real Amiga hardware | planned | limits.md |
| PiStorm SD, network, USB, sound, floppy, mouse, RTG | planned | ROADMAP.md phase 6 |

## Kernel
| Feature | Status | Details |
|---|---|---|
| Relocating kernel and programs (ZEXE format, no MMU) | verified | architecture.md section 4 |
| Program loader checks every header field and relocation offset; a corrupt program file is refused with a message | verified (`tools/memtest.sh`, 11 corrupt files) | architecture.md section 4 |
| Memory manager with per-process ownership, freed on exit | verified | architecture.md section 5 |
| Memory calls answer 0 (NULL) on failure; sizes near 4 GB refused; a corrupt heap header stops the kernel with its address | verified (`badptr` in `tools/memtest.sh`) | architecture.md section 5 |
| `malloc` with size classes and free lists (memory reused, big blocks given back), `realloc`; directory tools without stack arrays or entry limits | verified (`tools/memtest.sh`: flat shell memory, tools 25 directories deep) | architecture.md section 12 |
| Preemptive round-robin scheduler, 100 Hz, 50 ms slice | verified | architecture.md section 6 |
| `spawn` (no `fork`); wait, exit status, zombies | verified | architecture.md section 6 |
| About 55 system calls via `TRAP #0`, all pointers validated | verified | architecture.md section 7 |
| Crash containment: a faulting program is killed, memory freed | verified | how-it-works.md section 3 |
| Named message ports, synchronous zero-copy calls | verified | architecture.md section 8 |
| Servers: disk (`diskd`), filesystem (`fsd`), session manager (`boot`) | verified | architecture.md section 9 |
| Environment variables inherited by children | verified | |
| Interrupt handlers only queue input; tty, echo, Ctrl-C/Ctrl-Z, workspaces and the bar run in process context (no lost wakeups, lost input counted in `dmesg`) | verified in FS-UAE (`tools/kbd-test.sh`, `tools/jobs-test.sh`, the matrix) | architecture.md section 11 |
| Job control: Ctrl-Z stop, `jobs`, `fg`, `bg`, stopped state in `ps` (`SYS_jobctl`, `SYS_wait` untraced) | verified (`tools/jobs-test.sh`) | architecture.md section 7 |
| Priorities, signals with handlers, process groups | planned | |
| Users and groups: uid/gid on every process (real and effective), `setuid`/`setgid`, `umask`, `chown` | verified | architecture.md section 14 |
| Permission checks (r/w/x for owner, group, other; directory search; sticky bit; set-user-id programs) in the filesystem server | verified | architecture.md section 14 |
| `/etc/passwd`, `/etc/group`, `login` (also autologin, with a warning while root has no password), `su` (gives the target's environment), `passwd` (salted SHA-256, 1000 rounds), `id`, `chown`/`chgrp` | verified | man pages |
| Process and server protection (audit D): kill/stop/foreground only of one's own processes, never the built-in servers; clock and serial copy root only; the filesystem and disk servers answer only the kernel and root; reserved port names; no shell from Amiga+Return without a login | verified in FS-UAE (users-test, zroot-test) and on the host (ext2-host-test, disk-host-test) | architecture.md section 14 |
| Set-group-id directories, non-owner writes clear set-user-id, `utimes` rules, exclusive create (`O_EXCL`) in both filesystem servers | verified (host test for ext2; FS-UAE for ZenixFS through badptr ipc, here-documents and mktemp) | kernel/fsperm.h, architecture.md section 14 |
| Shared libraries | planned | |

## Filesystem and disks
| Feature | Status | Details |
|---|---|---|
| ext2 (revision 0/1, 1-4 KB blocks) read and write, clean under host `e2fsck`; modern `mke2fs -t ext2` volumes too; ext3 read-only, ext4 refused | verified (FS-UAE; host test `tools/ext2-host-test.sh`) | architecture.md section 10 |
| A damaged ext2 disk fails requests, never crashes the server (fuzzed) | verified on the host (`tools/ext2-host-test.sh`) | architecture.md section 10 |
| Files up to double-indirect size, growing directories, rename | verified | |
| Hard links, symbolic links (incl. fast symlinks), chmod, utimes | verified | |
| `/dev/null`, `/dev/zero`, `/dev/tty` | verified | |
| Pipes (2 KB) | verified | |
| `fdisk` (RDB), `mkfs.ext2`, `fsck.ext2` (check and repair) | verified | man/fdisk.txt, man/mkfs.ext2.txt, man/fsck.ext2.txt |
| `fdisk` keeps an RDB written by HDToolBox exactly (other geometries, FileSysHeader/LSEG chain); refuses a table it cannot keep | verified (host `tools/disk-host-test.sh`, FS-UAE `tools/disktools.sh`) | man/fdisk.txt |
| Disk server: stable handles, overlapping devices protected, a dead server's mount released | verified on the host (`tools/disk-host-test.sh`) | man/diskd.txt |
| SD card of a Raspberry Pi 4 / CM4 under Emu68 (PiStorm): polling SDHCI driver, 25 MHz 4-bit | built, host-tested, **not run on a Pi** | architecture.md section 3, `kernel/emmc.c` |
| Floppy drives df0-df3 (AmigaDOS 880 KB tracks, MFM by Paula DMA), read and write with verify; `dd` | built, FS-UAE-tested, one open bug (a whole-disk read once wrong), **not run on a real drive** | architecture.md "Floppy driver", `kernel/floppy.c` |
| Amiga 3000 on-board SCSI (WD33C93A + DMAC): a disk on targets 0-6, polled, DMA via a bounce buffer | built; host model test; see status in limits.md | architecture.md section 2 |
| Mouse (port 1) and joystick (port 2): polled at 100 Hz, `input` shows them, system call `SYS_input` | joystick verified in FS-UAE (`tools/joystick-test.py`, keyboard-as-joystick); mouse movement and buttons built but not tested | man/input.txt |
| Terminal bell: a short Paula tone (`beep`, any program printing BEL); the first beep logs whether the sound DMA ran | verified in FS-UAE (the DMA flag; not heard) | man/beep.txt |
| Kernel message log: boot messages on the screen, `dmesg` afterwards (8 KB ring) | verified in FS-UAE | architecture.md section 3, man/dmesg.txt |
| List of the Zorro expansion boards AmigaOS configured (manufacturer, product, address, size), read before the OS is killed | verified in FS-UAE (its fast-RAM board shows up) and in a 68020 emulator | architecture.md section 3 |
| Pi hardware table from Emu68's device tree (stage0, read before AmigaOS is killed) | built; tested on the host and in a CPU emulator against the real CM4 tree, boots in FS-UAE (empty table) | architecture.md section 3 |
| Disks with a PC partition table (MBR; primary entries): ext2 type 0x83, RDB inside type 0x76, FAT etc. listed and never mounted | verified in FS-UAE (`tools/mbr-test.sh`) | man/diskd.txt |
| Extended partitions, GPT | not done | limits.md |
| Mounting more than the root filesystem (`mount`, `umount`, nested mounts, ext2 servers per device) | verified in FS-UAE | man/mount.txt, architecture.md section 9 |
| ZenixFS on Zenix: `mkfs.zenixfs`, `mount` (type from the partition table), `fsck.zenixfs`; atomic commits, checksums | verified in FS-UAE | man/mount.zenixfs.txt, man/mkfs.zenixfs.txt |
| ZenixFS as the root filesystem (a second root server in the kernel, chosen by the partition type) | built; test tools/zroot-test.sh written, not yet run | man/mount.zenixfs.txt |
| `/etc/fstab` and `mount -a` at boot | built; test tools/zroot-test.sh written, not yet run | man/mount.txt, man/init.txt |
| Disk flush request (ATA FLUSH CACHE) and device capabilities in the disk protocol | verified (IDE in FS-UAE; not on real drives) | man/diskd.txt |
| Unlink of an open file, directory moves between parents, triple-indirect | not done | limits.md |

## Terminal and display
| Feature | Status | Details |
|---|---|---|
| 640x256 hires planar display, Copper-split bar, blitter-assisted scrolling | verified | architecture.md section 11 |
| ROM Topaz 8 font from the Kickstart in use at boot | verified | |
| 80x31 text, ANSI/VT100 (cursor, erase, insert/delete, scroll regions, bold/underline/inverse) | verified | man/termtest.txt |
| Scrollback 200 lines per workspace (Shift/Ctrl+Up/Down) | verified | |
| Nine workspaces, i3-style bar, Amiga+1..9, Amiga+Return | verified | scaffolding for the desktop |
| Raw tty mode, arrow and function keys as escape codes | verified | |
| Amiga keyboard, handshake timed by the E-clock; serial console mirror (115200 baud) | verified | architecture.md section 11 |
| Keymaps with AmigaOS's four layers (plain, Shift, Alt, Shift+Alt): Swedish (`sv`, the AmigaOS "s" layout) and US; Caps Lock; key repeat | verified in FS-UAE (`tools/kbd-test.sh`) | architecture.md section 11 |
| Colours, UTF-8, display server with a drawing API, mouse | planned | ROADMAP.md phase 5 |

## Shell
Line editing, history (`~/.sh_history`) with Ctrl-R search, tab completion, variables and the environment, quoting,
wildcards, brace expansion, `${v#pat} ${v%pat} ${v/pat/new} ${v:off:len}`, pipes, redirects (`< > >> 2> 2>&1 &>`),
here-documents, `if/while/until/for/case`, functions, aliases, arithmetic, command substitution, `trap … EXIT`, scripts,
`&` and job control (Ctrl-Z, `fg`, `bg`), `/etc/profile` and `~/.shrc`. **verified** (tools/cmds-test.sh compares with bash;
tools/jobs-test.sh). See man/sh.txt.

## Commands
Every command has a manual page. The full list with one-line summaries is in [commands.md](commands.md); the
pages are in [man/](man/). **verified** against GNU for the text tools (`tools/cmds-test.sh`).

## Tooling
| Feature | Status |
|---|---|
| `make` builds the floppy, the disk image and the test images | verified |
| Headless FS-UAE test runs from serial scripts (`tools/run-test.sh`, `tools/tests/`) | verified |
| 13-machine matrix (`tools/matrix.sh`), benchmark with the CIA E-clock (`bench`) | verified |
| Commands compared against GNU (`tools/cmds-test.sh`) | verified |
| Documentation check (`tools/docs-check.sh`) | verified |
