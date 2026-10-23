# Zenix: vision, principles and plan

Written 2026-10-22, last checked against the code 2026-10-23. This is the one place for the long-term picture and
the progress of the plan; DESIGN.md holds the numbered decisions, features.md what is built, README.md the short
state. Update them when something changes.

## Vision

A Unix-like operating system for real Amigas (68020 and up, no MMU needed) with the speed of AmigaOS and
the experience of Unix: a shell, pipes, a real filesystem, small programs that do one thing, and a desktop
built on the Amiga's own hardware (blitter, Copper, planar screens). Not Linux, not AmigaOS underneath.

## Principles (how we keep it clean)

1. **Small kernel, servers for everything else.** The kernel does scheduling, memory, messages and
   interrupts. Filesystems, the disk, the display, the network are servers behind named ports.
2. **One way to do a thing.** No parallel mechanisms. If a second way appears, one of them goes.
3. **The standard build contains only what the standard system uses.** Test and development aids (a RAM-disk
   image, benchmarks, test programs) are separate builds or separate files, never in the default image.
4. **Measure before optimising, and measure on real hardware.** Emulator timing is not CPU timing; numbers
   that matter are taken on the real A1200 and written down with the date.
5. **Nothing is done until it is tested.** Every feature has a test in the setup matrix (tools/matrix.sh) or a
   script next to it. Filesystem code is checked with the host's e2fsck.
6. **Real Amiga hardware is the ultimate target; the owner's PiStorm A1200 is the first test machine (owner,
   2026-10-23/24).** Zenix is tested first on the owner's A1200 with a PiStorm32-lite and a CM4 running Emu68, booting from
   the SD card or a GoDRIVE 1200, because that is the machine at hand. But it is built for real Amigas: a 68020-68060 at
   real clock speeds, Gayle IDE/CF, SCSI, real floppies, chip and fast RAM as the boards have it. So: nothing in the base
   system may depend on Emu68 or the Pi (Pi hardware is reached by optional drivers/servers only); speed is judged on real
   68k CPUs (today: what a stock 14 MHz 68EC020 can do), not by Emu68's ~1000 MIPS, which hides slow code; the stock A1200 setups stay in the
   test matrix; booting works the normal Amiga way (floppy, or a hard disk/CF/SD through `zenixboot`), never a custom ROM.
7. **Write it down.** Findings go to the knowledge base, decisions to DESIGN.md, status to README.md.
8. **Unix experience:** standard names, standard behaviour (POSIX where it makes sense), `/` filesystem,
   text tools that pipe.
9. **AmigaOS speed:** messages pass pointers, no copies; one address space; the blitter and Copper do the
   drawing; chip and fast RAM are separate pools.

## Where we are (2026-10-23)

Phases 1, 3 (most of it) and 4 are built and verified in FS-UAE, not yet on real hardware; the list is features.md.
The hardening round of the audit is done (below); the next step is phase 2, the first real boot.

## The text console behaves like Unix/Linux (owner's decision, 2026-10-23)
The text console has ten plain virtual consoles, switched with Amiga+F1..F10 (plain F-keys stay free for programs), each
with its own `login` prompt, like Linux's Alt+F1..F6. No bar, no Amiga+Return, no workspaces in the text console: those
are desktop features and come back in phase 5 with the display server. Done as a cleanup in audit batch E (it removes a
wrong turn; it also removes the root shell that Amiga+Return gave at the login prompt). **Done** (2026-10-23, branch
`fix-e`, verified in FS-UAE): `/dev/tty1`..`/dev/tty10`, a login on each (started when a console is first shown, as
Linux's autovt), the whole screen text (32 rows PAL, 25 NTSC), the serial hook 0x1d + digit; architecture.md section 11.

## Feature halt (2026-10-22)
The owner halted all new features on 2026-10-22. Until the owner lifts it, work is only the fixes listed in
`audit-2026-10-22.md` (batches A-E), the tests that check them, documentation corrections and merging the branches
built before the halt. The phases below resume after that.
**Exception approved by the owner on 2026-10-23:** booting without a floppy - a small AmigaDOS boot partition whose
Startup-Sequence runs `zenixboot`, an Amiga program that loads the kernel from a file and takes over like the bootblock.
It works on any Amiga that boots from a hard disk, CF or SD card (the PiStorm's SD card through Emu68's driver).

Progress: **batch A (first-boot blockers) is done** except A7 (a chip-only A1200 booting the RAM disk, left open:
the owner's machine has fast RAM), with the approved first-boot diagnostics (A11 boot colours, A12 previous-boot
log); verified in FS-UAE, not yet on the A1200.
**Batch B (memory foundations) is done** - all seven items (B1-B7), 2026-10-23 on branch `fix-b`,
verified in FS-UAE (`tools/memtest.sh`, keyboard, job-control and matrix tests); not yet on the A1200.
**Batch C (filesystem and disk safety) is done** - C1-C10, 2026-10-23 on branch `fix-c`: verified on the host (`tools/ext2-host-test.sh`,
`tools/disk-host-test.sh`, `make -C zenixfs test`) and in FS-UAE (disk, mount, ZenixFS, ZenixFS-root, command, user and matrix tests);
not yet on the A1200.
**Batch D (permissions as documented) is done** - 2026-10-23 on branch `fix-d`: verified in FS-UAE (users and
ZenixFS-root tests) and on the host (ext2 and disk tests); not yet on the A1200.
**Batch E (correctness in daily use) is done** - 2026-10-23 on branch `fix-e`: the Linux-style consoles, the shell's
leaks and silent caps, the command fixes, the kernel items and errno numbers; verified in FS-UAE (users, keyboard,
matrix, command, job, log, diagnostics, memory, mount, ZenixFS and ZenixFS-root tests) and on the host; what is left
is listed at the end of the audit; not yet on the A1200.

## The plan, in phases (reordered 2026-10-22: a terminal to live in comes before ZenixFS and the desktop)

**Phase 1 - Disk tools** (done, verified in FS-UAE). `fdisk` (the RDB partition table), `mkfs.ext2`, `fsck.ext2`, `df`, `sync`; the disk
server refuses raw access to mounted devices. Tested against scratch disks in the emulator and checked with the
host's `e2fsck`.

**Phase 2 - First real hardware.** Boot from the Gotek/GoDrive with the RAM-disk test image; measure the CPU
side (benchmark); fix what real hardware shows. (The user's A1200 is a PiStorm with an SD card and no IDE disk,
so this uses `zenix-ram.adf` until an SD driver exists.)

**Phase 3 - A terminal you can live in** (mostly built; what is left is in features.md and limits.md). Before
any desktop:
- *Shell:* history, line editing, tab completion, variables and the environment, scripting (`if`, `for`,
  `while`, functions), globbing, aliases, job control, `2>` and here-documents, a profile file. Lessons from
  the csh project (best of bash, zsh and fish).
- *Terminal:* arrow, Home/End and function keys as standard escape codes, scrollback, text styles (bold,
  inverse, underline), colours decided deliberately (the display is one bitplane now), UTF-8 later.
- *Users:* multi-user with the honest no-MMU limit (DESIGN.md D8): uids, permission checks in the filesystem server,
  `login`, `su`, `chown`, `/etc/passwd`.
- *C library and kernel:* full printf, stdio, `qsort`, `strtol`, time functions, `getenv`, signals, `kill`,
  process groups, `/dev` (null, zero, tty), `/proc`-like info.
- *Commands:* written natively for Zenix, with ideas and the regex engine from CUtils (sed, sort, diff, find, tar,
  gzip, ...; the list is commands.md).
- *Editor and pager:* Sumi and Maki ported (they use tui.library; Zenix gets an equivalent that emits
  standard escape codes), man pages.
- *Network tools later,* when the network server exists.

**Phase 4 - ZenixFS** (built: the library, the server, the tools, the mount table; works as the root in FS-UAE;
what is left is in zenixfs.md). Our own filesystem (docs/zenixfs.md): atomic commit, checksummed metadata, extents,
indexed directories, self-healing. ext2 stays for Linux compatibility.

**Phase 5 - The desktop.** (The workspaces and the i3-style bar are desktop features: they were removed from the kernel
console in audit batch E and come back here, in the display server. The terminal itself never depends on them.) Move the display out of the kernel into a display server with a drawing API
(planar, blitter and Copper); a client library; a terminal program as the first client; the workspace bar;
Ahiru-style file view; mouse. Index server (FuzzyFind) for `locate` / `fzf`.

**Phase 6 - More hardware, as servers.** Network (TCP/IP server), USB, NVMe, RTG graphics, sound (Paula) server,
serial. (Already built into the kernel and to be moved out later: the PiStorm SD card, never run on a Pi; the floppy
driver, experimental; the A3000 SCSI.)

**Phase 7 - Polish and release.** Documentation, public repository, release images (the licence is decided:
DESIGN.md D9).

## Ideas parked for later

## Open questions

- **Minimum requirements: not decided (owner, 2026-10-24).** The owner sets them later, depending on how much Zenix
  grows. What is true today: the kernel refuses a 68000/68010 (the code needs a 68020+); everything boots and passes the
  matrix on a stock 68EC020 with 2 MB chip RAM and no fast RAM, and on 1 MB chip-only with one login session. Until the
  owner decides, keep it running on those, and say in a change's notes when it costs memory or speed on them.

- ZenixFS name: a Western Digital project already uses "ZenixFS" (RocksDB on zoned SSDs); decide if that matters.
- How much POSIX: `fork` is not possible without an MMU; `vfork`/`spawn` only. Programs that need `fork` are out.
- Journaling: not needed if ZenixFS commits atomically; ext2 gets a clean flag and our fsck.
