# Zenix feature list

**This is the one list of what Zenix can do.** It is kept in step with the code (rule in `../CLAUDE.md`): when a
feature is added, changed or removed, its line here changes in the same commit. `README.md` only summarises it.

Status words: **verified** = built and run in FS-UAE (an emulator); "verified on the A1200" with a date when it ran on the owner's real PiStorm A1200 (the list of
what was checked there: audit-closing-2026-10-24.md);
**read** = checked in the source only; **built** = written, but no test runs it where it matters; **planned** = not
built. Last checked against the code: 2026-10-23.

## Machine support
| Feature | Status | Details |
|---|---|---|
| 68020, 68030, 68040, 68060 (soft float, no MMU use) | verified | architecture.md section 2; the emulated machines of the matrix (architecture.md section 13) |
| 68000 refused with a message (orange over red screen, serial text) | verified (`tools/diag-test.sh`) | start.S |
| Boot stage colours: the whole screen changes colour at each boot step until the display is up; error stops show the step's colour over red, a panic after that a red bar with its text | verified in FS-UAE (`tools/diag-test.sh`: each stage's colour from a screenshot); verified on the A1200 (2026-10-23) | pi-first-boot.md (the table), architecture.md section 3 |
| Chip and fast RAM pools, 1 MB chip up to 8 MB+ fast | verified | architecture.md section 5 |
| Boot from floppy (ADF) bootblock; kernel in fast RAM if present | verified | how-it-works.md section 1 |
| Boot with a RAM-disk root loaded from the floppy (no IDE disk) | verified (dev/test image) | `zenix-ram.adf` |
| Boot without a floppy: Kickstart boots a small FFS partition whose Startup-Sequence runs `zenixboot` (an AmigaOS program, Kickstart 2.04+, 68020+), which loads the kernel file (and optionally a RAM disk) and starts Zenix; errors return to AmigaOS. Images: `build/zenix-boot.hdf` (RDB disk for IDE/CF), `build/zenix-card.img` (Emu68 SD card: MBR, FAT, 0x76 RDB, ext2 root), written by `tools/write-card.sh` | verified in FS-UAE (`tools/zenixboot-test.sh`: Kickstart 3.1, 3.2 and 2.05; 68EC020, 68020 chip-only, 68040; the 68000 refused; the error paths); verified on the A1200 (2026-10-23: from the SD card under Emu68, Kickstart 3.2.3; the RDB disk image for IDE/CF not yet on real hardware) | man/zenixboot.txt, how-it-works.md section 1 |
| Gayle IDE disk with Amiga RDB partitions (A600/A1200/A4000); the machine decided first (Ramsey, Gayle ID 0xD) and only its port probed; a drive must prove it is there (register read-back), slow starters get 10 s, waits timed on the E-clock; LBA drives only; up to 256 sectors per command on the Gayle port; its interrupt sources switched off (polling only) | verified in FS-UAE (the matrix); real drives' timing, CHS-only drives and the interrupt storm read only | architecture.md section 2 |
| Instruction-cache (and Emu68 JIT) flush after every write of code; built for 68020-68060 with an opcode check | verified in FS-UAE, read for Emu68 | architecture.md section 2 |
| Battery clock (MSM6242, RP5C01); no write before a check that the area is not a mirror of the custom chips; the RP5C01 read with its timer stopped | read (FS-UAE shows the time in the boot log; no test compares it) | architecture.md section 2 |
| Real Amiga hardware: the owner's A1200 with a PiStorm32-lite (Emu68), booted from its SD card through `zenixboot` and the boot menu | verified on the A1200 (2026-10-23/24) | audit-closing-2026-10-24.md (what was checked); a stock 68020 without the PiStorm is not tested yet |
| Network, USB, RTG graphics, sound beyond the bell | planned | ROADMAP.md phase 6 |

## Kernel
| Feature | Status | Details |
|---|---|---|
| Relocating kernel and programs (ZEXE format, no MMU) | verified | architecture.md section 4 |
| Program loader checks every header field and relocation offset; a corrupt program file is refused with a message | verified (`tools/memtest.sh`, 11 corrupt files) | architecture.md section 4 |
| Memory manager with per-process ownership, freed on exit | verified | architecture.md section 5 |
| Memory calls answer 0 (NULL) on failure; sizes near 4 GB refused; a corrupt heap header stops the kernel with its address | verified (`badptr` in `tools/memtest.sh`) | architecture.md section 5 |
| `malloc` with size classes and free lists (memory reused, big blocks given back), `realloc`; directory tools without stack arrays or entry limits | verified (`tools/memtest.sh`: flat shell memory, tools 25 directories deep) | architecture.md section 12 |
| Preemptive scheduler with priorities (DESIGN D10): the highest ready level runs, round robin in 50 ms slices within a level; nice -20..19 on 32 levels; built-in servers at nice -10; `nice`, `renice`, `getpriority`/`setpriority`; PRI and NI in `ps` | verified in FS-UAE (`tools/prio-test.sh`: fairness, a higher one wins, wake-up, the user/root rules) | architecture.md section 6 |
| Priority inheritance: a server runs at least at its best client's level until it replies | verified in FS-UAE (`priotest inherit`; a kernel without it fails the check) | architecture.md section 8 |
| `spawn` (no `fork`); wait, exit status, zombies | verified | architecture.md section 6 |
| System calls via `TRAP #0` (the count and the list: architecture.md section 7), all pointers validated; errors as negative errno numbers (`kernel/errno.h`, Linux values), the same from both filesystem servers, shown as text by `zwhy`/`zstrerror` | verified (users-test permission texts, cmds2-test batch E script, host ext2 test) | architecture.md section 7 |
| Crash containment: a faulting program is killed, memory freed | verified | how-it-works.md section 3 |
| Stack guard: a pattern below every kernel and user stack, checked with the stack pointers at every task switch, system call and tick; an overflow kills the process with "overflowed its user/kernel stack" (pid 0: a panic) | verified in FS-UAE (`crash stack` in the matrix; `tools/diag-test.sh stack` for the kernel-stack checks) | architecture.md section 6 |
| Named message ports, synchronous zero-copy calls | verified | architecture.md section 8 |
| Servers: disk (`diskd`), filesystems (`fsd` for ext2, `zfsd` for ZenixFS; the root's and the mounts'), starter (`boot`) | verified | architecture.md section 9 |
| Service manager (DESIGN D11): the disk and root filesystem servers are services boot registers; the first call starts a server, a crashed or killed one is started again by the next call (its queued calls wait, the one in service fails), with a 30 s start deadline and a back-off; registered names are taken by nobody else; a kill acts between requests; the bootable partition is the root; a restarted diskd keeps its device slots and claims (a table it leaves with the kernel), the filesystem servers send a failed disk request once more; a restarted filesystem server finds its volume by place and uuid, rebuilds its open files from the kernel (a deleted open file keeps its data), marks an ext2 volume "needs a check" after a crash mid-request, and on ZenixFS keeps open orphans and never reuses an inode number; 5 ends in 60 s give a service up (diskd or the root: a panic) | verified in FS-UAE (`tools/svc-test.sh`) and on the host (`tools/disk-host-test.sh`) | architecture.md section 9 |
| Environment variables inherited by children | verified | |
| Interrupt handlers only queue input; tty, echo, Ctrl-C/Ctrl-Z and console switches run in process context (no lost wakeups, lost input counted in `dmesg`); unexpected interrupt vectors are logged once and ignored | verified in FS-UAE (`tools/kbd-test.sh`, `tools/jobs-test.sh`, the matrix); the unexpected vectors read only | architecture.md section 11 |
| Job control: Ctrl-Z stop, `jobs`, `fg`, `bg`, stopped state in `ps` (`SYS_jobctl`, `SYS_wait` untraced) | verified (`tools/jobs-test.sh`) | architecture.md section 7 |
| Signals with handlers, process groups | planned | |
| Users and groups: uid/gid on every process (real and effective), `setuid`/`setgid`, `umask`, `chown` | verified | architecture.md section 14 |
| Permission checks (r/w/x for owner, group, other; directory search; sticky bit; set-user-id programs) in the filesystem server | verified | architecture.md section 14 |
| `/etc/passwd`, `/etc/group`, `login` (also autologin, with a warning while root has no password), `su` (gives the target's environment), `passwd` (salted SHA-256, 1000 rounds), `id`, `chown`/`chgrp` | verified | man pages |
| Process and server protection (audit D): kill/stop/foreground only of one's own processes, never the built-in servers; clock and serial copy root only; the filesystem and disk servers answer only the kernel and root; reserved port names; another console's `/dev/ttyN` root only | verified in FS-UAE (users-test, zroot-test) and on the host (ext2-host-test, disk-host-test) | architecture.md section 14 |
| Set-group-id directories, non-owner writes clear set-user-id, `utimes` rules, exclusive create (`O_EXCL`) in both filesystem servers | verified (host test for ext2; FS-UAE for ZenixFS through badptr ipc, here-documents and mktemp) | kernel/fsperm.h, architecture.md section 14 |
| Shared libraries | planned | |

## Filesystem and disks
| Feature | Status | Details |
|---|---|---|
| ext2 (revision 0/1, 1-4 KB blocks) read and write, clean under host `e2fsck`; modern `mke2fs -t ext2` volumes too; ext3 read-only, ext4 refused | verified (FS-UAE; host test `tools/ext2-host-test.sh`) | architecture.md section 10 |
| A damaged ext2 disk fails requests, never crashes the server (fuzzed) | verified on the host (`tools/ext2-host-test.sh`) | architecture.md section 10 |
| Files up to double-indirect size, growing directories, rename | verified (FS-UAE write tests; the double-indirect size on the host, `tools/ext2-host-test.sh`: a 300 KB file on 1 KB blocks, `e2fsck` clean) | |
| Hard links, symbolic links (incl. fast symlinks), chmod, utimes | verified | |
| `/dev/null`, `/dev/zero`, `/dev/tty` (the controlling tty), `/dev/tty1`..`/dev/tty10`, `/dev/console` | verified: `/dev/null` (the test scripts), the consoles and their root-only rule (users-test); the rest read | architecture.md section 11 |
| Pipes (2 KB) | verified | |
| `fdisk` (RDB), `mkfs.ext2`, `fsck.ext2` (check and repair) | verified | man/fdisk.txt, man/mkfs.ext2.txt, man/fsck.ext2.txt |
| `fdisk` keeps an RDB written by HDToolBox exactly (other geometries, FileSysHeader/LSEG chain); refuses a table it cannot keep | verified (host `tools/disk-host-test.sh`, FS-UAE `tools/disktools.sh`) | man/fdisk.txt |
| Disk server: stable handles, overlapping devices protected, a dead server's mount released | verified on the host (`tools/disk-host-test.sh`) | man/diskd.txt |
| SD card of a Raspberry Pi 4 / CM4 under Emu68 (PiStorm): polling SDHCI driver, 25 MHz 4-bit | built, host-tested, **not run on a Pi** | architecture.md section 3, `kernel/emmc.c` |
| Floppy drives df0-df3 (AmigaDOS 880 KB tracks, MFM by Paula DMA), read and write with verify; disk changes detected; `dd` | verified in FS-UAE (`tools/floppy-test.sh`); the drive timings read only, **not run on a real drive or a Gotek** | architecture.md "Floppy driver", `kernel/floppy.c` |
| Amiga 3000 on-board SCSI (WD33C93A + DMAC): a disk on targets 0-6, polled, DMA via a bounce buffer | built; host model test; see status in limits.md | architecture.md section 2 |
| Mouse (port 1) and joystick (port 2): polled at 100 Hz, `input` shows them, system call `SYS_input` | joystick verified in FS-UAE (`tools/joystick-test.py`, keyboard-as-joystick); mouse movement and buttons built but not tested | man/input.txt |
| Terminal bell: a short Paula tone (`beep`, any program printing BEL); the first beep logs whether the sound DMA ran | verified in FS-UAE (the DMA flag; not heard) | man/beep.txt |
| Kernel message log: boot messages on the screen, `dmesg` afterwards (8 KB ring) | verified in FS-UAE | architecture.md section 3, man/dmesg.txt |
| Previous-boot log: the log is kept at the top of chip RAM; after a warm reset the next boot shows the last 20 lines and whether it ended in a panic; `dmesg -p` prints all of it | verified in FS-UAE (`tools/diag-test.sh`, keyboard reset, Kickstart 3.1); whether chip RAM survives a reset on the PiStorm is unknown | architecture.md section 3, man/dmesg.txt |
| List of the Zorro expansion boards AmigaOS configured (manufacturer, product, address, size), read before the OS is killed | verified in FS-UAE (its fast-RAM board shows up) and in a 68020 emulator | architecture.md section 3 |
| Pi hardware table from Emu68's device tree (stage0, read before AmigaOS is killed) | built; tested on the host and in a CPU emulator against the real CM4 tree, boots in FS-UAE (empty table) | architecture.md section 3 |
| Disks with a PC partition table (MBR; primary entries): ext2 type 0x83, RDB inside type 0x76, FAT etc. listed and never mounted | verified in FS-UAE (`tools/mbr-test.sh`) | man/diskd.txt |
| Extended partitions, GPT | not done | limits.md |
| Mounting more than the root filesystem (`mount`, `umount`, nested mounts, a server per device); mounts are services (D11): the server at nice -10 comes back after a crash or kill, ends for good at `umount`, a failed mount leaves no service, a floppy without a volume id is not served again after a restart | verified in FS-UAE (`tools/mounttest.sh`) | man/mount.txt, architecture.md section 9 |
| ZenixFS on Zenix: `mkfs.zenixfs`, `mount` (type from the partition table), `fsck.zenixfs`; atomic commits, checksums | verified in FS-UAE | man/mount.txt, man/mkfs.zenixfs.txt |
| ZenixFS as the root filesystem (a second root server in the kernel, chosen by the partition type) | verified in FS-UAE (`tools/zroot-test.sh`) | man/boot.txt, zenixfs.md |
| `/etc/fstab` and `mount -a` at boot | verified in FS-UAE (`tools/zroot-test.sh` mounts an ext2 volume from it) | man/mount.txt, man/init.txt |
| Disk flush request (ATA FLUSH CACHE) and device capabilities in the disk protocol | verified (IDE in FS-UAE; not on real drives) | man/diskd.txt |
| An open file that is unlinked keeps its data until the last close (both filesystems) | verified (host ext2 test, cmds-test `unlinkopen`) | architecture.md section 9 |
| ext2: moving a directory to another parent (ZenixFS does it; `mv` copies), triple-indirect files | not done | limits.md |

## Terminal and display
| Feature | Status | Details |
|---|---|---|
| 640x256 (PAL) or 640x200 (NTSC, measured at boot) hires planar display, all text, blitter-assisted scrolling | verified (the matrix, NTSC included) | architecture.md section 11 |
| ROM Topaz 8 font from the Kickstart in use at boot | read (every test boots with it; no test compares the glyphs) | |
| 80x32 text (80x25 on NTSC), ANSI/VT100 (cursor, erase, insert/delete, scroll regions, bold/underline/inverse) | the size verified (the matrix); the escape codes read (`termtest` draws them, no test compares the screen) | man/termtest.txt |
| Scrollback 200 lines per console: Amiga+Up/Down a screenful, Amiga+Shift+Up/Down a line, in every mode, also at the shell's prompt (a console key like Amiga+F1..F10, as Linux's Shift+PageUp: programs never see it; every Amiga keyboard has these keys) | verified in FS-UAE (`tools/kbd-test.sh`: Amiga+Up at the prompt shows older lines, Amiga+Down returns) | |
| Ten virtual consoles, Amiga+F1..F10, each with its own `login` (started when the console is first shown, again after a logout); the controlling tty of a process is its console (`ls \| less` on console 2 reads console 2's keys) | verified in FS-UAE (`tools/kbd-test.sh`, `tools/users-test.sh`, the matrix) | architecture.md section 11, man/init.txt |
| Type-ahead kept when a program switches the tty to raw mode | verified (`tools/users-test.sh`) | architecture.md section 11 |
| The clock: tick latch from the machine's E-clock (PAL or NTSC crystal), 100 Hz to within the E-clock's resolution | built; the boot log shows the choice (the matrix's NTSC machine); the rate itself not measured | architecture.md section 11 |
| Raw tty mode, arrow and function keys as escape codes | verified for the arrows (`tools/edit-regress.sh`); the function keys read | |
| Amiga keyboard, handshake timed by the E-clock, taken by its own CIA interrupt, one handshake at start, lost-sync/overflow codes forget held keys; serial console mirror (115200 baud; waits for the port only once something was received, else drops and counts) | verified (the keyboard tests); the keyboard's own codes read | architecture.md section 11 |
| Reset warning: Ctrl+Amiga+Amiga on a keyboard that sends it holds the reset while init syncs the disks (at most 8 s) | verified in FS-UAE (`tools/diag-test.sh`: its keyboard sends the warning; the sync's line is in the next boot's previous-boot lines); whether the A1200's keyboard sends it is unknown | architecture.md section 11 |
| Keymaps with AmigaOS's four layers (plain, Shift, Alt, Shift+Alt): Swedish (`sv`, the AmigaOS "s" layout) and US; Caps Lock; key repeat | verified in FS-UAE (`tools/kbd-test.sh`) | architecture.md section 11 |
| Colours, UTF-8, display server with a drawing API, a mouse pointer | planned | ROADMAP.md phase 5 |

## Shell
Line editing, history (`~/.sh_history`) with Ctrl-R search, tab completion, variables and the environment, quoting,
wildcards, brace expansion, `${v#pat} ${v%pat} ${v/pat/new} ${v:off:len}`, pipes, redirects (`< > >> 2> 2>&1 &>`),
here-documents, `if/while/until/for/case`, functions, aliases, arithmetic, command substitution, `trap … EXIT`, scripts,
`&` and job control (Ctrl-Z, `fg`, `bg`), `local` (dynamic scope), `${10}`, `/etc/profile` and `~/.shrc`; no fixed caps on
arguments, glob matches, words, pipeline stages or `for` items (they grow; a limit that is reached is an error), loops
in flat memory, type-ahead kept. **verified** (tools/cmds-test.sh compares with bash; tools/jobs-test.sh;
tools/cmds2-test.sh runs rootfs/tests/batch-e.sh). See man/sh.txt.

## Commands
Every command has a manual page. The full list with one-line summaries is in [commands.md](commands.md); the
pages are in [man/](man/). **verified** against GNU for the text tools (`tools/cmds-test.sh`).

`awk` (owner-approved 2026-10-24): a POSIX awk (patterns and actions, arrays, user functions, the built-in functions,
`printf`, `getline`, output to files and pipes), on a fixed 64 KB stack with a depth guard. **verified** in FS-UAE:
`tools/cmds-test.sh` compares 17 groups of awk programs with GNU awk; `tools/memtest.sh` checks that endless recursion
and deep nesting stop with a message and that a long loop keeps memory flat. See man/awk.txt.

## Tooling
| Feature | Status |
|---|---|
| `make` builds the floppy, the disk image and the test images | verified |
| Boot menu (owner-approved 2026-10-24): `bootmenu` on the boot partition of an SD card with AmigaOS and Zenix (`tools/mkdualcard.sh`: Emu68 FAT, an RDB with BOOT and the AmigaOS partitions, Zenix root and /home); a countdown and a default, AmigaOS partitions booted through a one-shot resident at the next reset (nothing written to the disk), Zenix through zenixboot | verified in FS-UAE (Kickstart 3.2: countdown, reboot into the chosen partition; a test card's root and /home mounted by Zenix); **verified on the A1200 2026-10-24** (Emu68 1.1, Kickstart 3.2.3: the menu and the reboot into DH0) | man/bootmenu.txt |
| Rebooting between the systems (owner-approved 2026-10-24): `reboot [amigaos|zenix]` / `bootamigaos` in Zenix (system call 80: sync, stop disk writes, reset) and `bootzenix` in AmigaOS leave a one-shot note at the top of chip RAM that `bootmenu` follows without showing the menu | verified in FS-UAE (both ways, Kickstart 3.2.3); verified on the A1200 2026-10-24, both ways (`bootzenix`, `bootamigaos`) | man/reboot.txt, man/bootmenu.txt |
| The serial port as a device, `/dev/ttyS0`, and `cu` (DESIGN D12, owner-approved 2026-10-24): root, one owner at a time; while open the console copy, the log copy and the 0x1D hook are off the wire | verified in FS-UAE (`tools/cu-test.sh`: a line from the PC reaches cu, keys reach the port alone, `~.` ends it, the console copy comes back; `tools/serial-test.sh` on the host: one owner, -EBUSY, nothing else on the wire, bytes unchanged); **not yet on the A1200** (the cable arrives 2026-10-31) | man/cu.txt |
| AmigaOS-side tools from Linux without root or amitools: an FFS volume (`tools/mkffs.py`), an AmigaOS executable from a ZEXE (`tools/mkhunk.py`), the zenixboot disks (`tools/mkcard.py`) | verified (Kickstart 3.1/3.2/2.05 boot the FFS volume in FS-UAE; amitools' `xdftool` reads it back when installed) |
| Headless FS-UAE test runs from serial scripts (`tools/run-test.sh`, `tools/tests/`) | verified |
| `make check`: every strong test in one run, a table, fails on any failure (`tools/check.sh`, docs/README.md "Testing") | verified |
| The machine matrix (`tools/matrix.sh`, architecture.md section 13), benchmark with the CIA E-clock (`bench`) | verified |
| Commands compared against GNU (`tools/cmds-test.sh`) | verified |
| Documentation check (`tools/docs-check.sh`) | verified |
