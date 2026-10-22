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
| Gayle IDE disk with Amiga RDB partitions (A600/A1200/A4000) | verified | architecture.md section 9 |
| Battery clock (MSM6242, RP5C01) | verified in emulator | `kernel/rtc.c` |
| Real Amiga hardware | planned | limits.md |
| PiStorm SD, network, USB, sound, floppy, mouse, RTG | planned | ROADMAP.md phase 6 |

## Kernel
| Feature | Status | Details |
|---|---|---|
| Relocating kernel and programs (ZEXE format, no MMU) | verified | architecture.md section 4 |
| Memory manager with per-process ownership, freed on exit | verified | architecture.md section 5 |
| Preemptive round-robin scheduler, 100 Hz, 50 ms slice | verified | architecture.md section 6 |
| `spawn` (no `fork`); wait, exit status, zombies | verified | architecture.md section 6 |
| About 55 system calls via `TRAP #0`, all pointers validated | verified | architecture.md section 7 |
| Crash containment: a faulting program is killed, memory freed | verified | how-it-works.md section 3 |
| Named message ports, synchronous zero-copy calls | verified | architecture.md section 8 |
| Servers: disk (`diskd`), filesystem (`fsd`), session manager (`boot`) | verified | architecture.md section 9 |
| Environment variables inherited by children | verified | |
| Priorities, signals with handlers, process groups | planned | |
| Users, groups, permission checks | planned | DESIGN.md D8 |
| Shared libraries | planned | |

## Filesystem and disks
| Feature | Status | Details |
|---|---|---|
| ext2 (revision 0/1, 1-4 KB blocks) read and write, clean under host `e2fsck` | verified | architecture.md section 10 |
| Files up to double-indirect size, growing directories, rename | verified | |
| Hard links, symbolic links (incl. fast symlinks), chmod, utimes | verified | |
| `/dev/null`, `/dev/zero`, `/dev/tty` | verified | |
| Pipes (2 KB) | verified | |
| `fdisk` (RDB), `mkfs.ext2`, `fsck.ext2` (check and repair) | verified | man/fdisk.txt, man/mkfs.ext2.txt, man/fsck.ext2.txt |
| SD card of a Raspberry Pi 4 / CM4 under Emu68 (PiStorm): polling SDHCI driver, 25 MHz 4-bit | built, host-tested, **not run on a Pi** | architecture.md section 3, `kernel/emmc.c` |
| Pi hardware table from Emu68's device tree (stage0, read before AmigaOS is killed) | built; tested on the host and in a CPU emulator against the real CM4 tree, boots in FS-UAE (empty table) | architecture.md section 3 |
| SD card with an MBR partition table (the Emu68 boot card) | not done | limits.md |
| Mounting more than the root filesystem | planned | ROADMAP.md phase 4 |
| ZXFS | planned | zxfs.md |
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
| Amiga keyboard; serial console mirror (115200 baud) | verified | |
| Colours, UTF-8, display server with a drawing API, mouse | planned | ROADMAP.md phase 5 |

## Shell
Line editing, history (`~/.sh_history`), tab completion, variables and the environment, quoting, wildcards,
pipes, redirects (`< > >> 2> 2>&1 &>`), here-documents, `if/while/until/for`, functions, aliases, arithmetic,
command substitution, scripts, `&` and job control, `/etc/profile` and `~/.shrc`. **verified.** See man/sh.txt.

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
