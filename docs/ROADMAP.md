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

## Where we are (2026-10-24)

Phases 1, 3 (most of it) and 4 are built and verified in FS-UAE; phase 2 is done: Zenix runs on the owner's A1200
(PiStorm32-lite, Emu68 1.1), booted from the SD card through a boot menu next to AmigaOS. The audit round is closed;
D10 (priorities) is built, D11 (service manager) is built (its last steps on branch d11, to be merged).

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
**Batch F (stack guard, `ls`, the hardware hand-off note) is done** - 2026-10-23 on branch `fix-f`: the stack guard,
`ls` without a cap, and every item of the hardware hand-off note that applies (keyboard reset warning and handshakes,
IDE machine detection and timing, floppy write tail, gap, settle times, retries and disk change, clock, A3000 SCSI
timeouts); the floppy test is back in `make check`; verified in FS-UAE and on the host as the audit says per item; not
yet on the A1200.

## The order of work (approved by the owner 2026-10-25)
The phases below say *what*; this says *in which order*. When one item is done, the next starts at once. Real-hardware findings and problems found along the way are
fixed next, ahead of everything here ("fix things when we find them"). A focused audit closes every block; its findings
are fixed at once. Two independent design reviews for deep core changes, one for features.

1. **Foundations finished (the week of 2026-10-26):** D13 shared code; the shell's memory fix; the colour console;
   the self-updates. Audit.
2. **Zenix reaches the world** (done in FS-UAE 2026-10-29, closing audit docs/audit-2026-10-29-block2.md; the real A1200 checks are the owner's weekend): signals and `select`/`poll`; the network (the CM4's Ethernet driver, TCP/IP as a
   server, DHCP, DNS); `ping`, `telnet` (made for BBSes, the owner's daily use: ANSI colours, the PC character set and Amiga
   fonts, the BBS window size, downloads), `wget`, an NFS client; a Wasabi-like remote control (early: it lets the A1200
   be tested remotely); **one small crypto library** (owner 2026-10-26: "I need to be able to SSH into my Linux machine,
   properly"): modern, small and constant-time, e.g. BearSSL (MIT) for TLS and Dropbear's client (MIT) for SSH, with
   Ed25519/X25519/ChaCha20 (fast without an FPU or AES hardware) and a real random source; it also signs `zpkg`'s
   packages; then `ssh` (a proper client, keys, known hosts) and HTTPS for `wget`; `zpkg` with its repository on the
   owner's NAS. A security audit. Then (postponed by the owner on 2026-10-28, design D25 ready) **the Zenix demo floppy**
   (owner 2026-10-25, in the spirit of the 1999 QNX demo disk): one 880 KB Amiga floppy that boots into Zenix with the
   network, `telnet` and a web-page fetcher, to hand to anyone; a second edition with the desktop in block 5. The model (owner 2026-10-26): KolibriOS, a
   desktop, network and programs on one 1.44 MB floppy; as there, the programs are packed on the disk and unpacked
   when they start, which is what makes a desktop edition fit 880 KB.
3. **A terminal you can live in:** a fuller C library (picolibc, newlib's descendant: D27; steps 0-4 built 2026-10-29); D13 stage 2 (shared libraries) decided; the text-UI
   library (ported 2026-10-29: ports/tui), then Sumi (ported 2026-10-29: ports/sumi), Maki, Markdown and `zc`; UTF-8 in the console; PC (CP437) and other bitmap fonts for the console
   (the PC set comes earlier, with telnet); a text-only e-mail client (IMAP/SMTP over TLS; owner 2026-10-26); and
   (owner 2026-10-26) the owner's Ahiru file manager and `ctop` ported, a tracker music player (MOD and friends, on
   Paula), an IRC client. Audit; then a first public pre-release (0.1),
   with the owner's yes.
3½. **Amiga demos on Zenix** (approved by the owner 2026-10-29: "I would like to be able to watch an Amiga demo in
   Zenix. That would be amazing. So yes, please move it up"): a design note first (reviewed twice: it hands the whole
   machine over and takes it back), then built: choose a demo (an ADF or a program file) in Zenix, Zenix saves its
   state and steps aside, the demo has the hardware and chip RAM, and Zenix comes back when it ends, as demos return
   to Workbench. Demos on the bare hardware first; demos that need AmigaOS's libraries later. Starts when block 3's
   audit is done. The idea's notes: docs/future-ideas.md "Amiga demos on Zenix".
4. **The Amiga world:** FAT, FFS/OFS, then PFS3; LhA/LZX unpacking; ZenixFS as the root on real hardware; the
   installer. Audit, with the filesystems fuzzed against damaged disks.
5. **The desktop:** the display server; a terminal window, workspaces with the i3-style bar, the mouse; the file view,
   viewers, preferences; TrueType fonts (owner 2026-10-26; rendered once per size into bitmap fonts and cached, so a
   68020 without an FPU pays only once; also usable by the console); the demo floppy's desktop edition. Audit.
6. **Sound and hardware:** a Paula server, `play`, a MOD player; optional memory protection on CPUs with an MMU; USB,
   RTG, faster IDE, CD. Audit.
7. **Zenix builds itself, and fun:** assembler, C compiler, `make`, debugger, Lua; games and demos on Zenix. A full
   audit of the whole system, then release 1.0 with the owner's yes.

## Versions and updates (approved by the owner 2026-10-25)
- **Every part is a package with its own version** (the kernel, `sh`, `awk`, a driver ...); `zpkg -Syu` fetches what
  is new, a fix to one program waits for nothing else.
- **"base" moves as one:** the kernel, the built-in servers, `init` and the C library share system-call and message
  numbers, so they are one package group with one version, always updated together.
- **Rolling, in two channels:** *testing* (every version that passes `make check`; the owner's A1200) and *stable* (a
  version that has been in daily use a while without trouble: "beta until daily driven").
- **Dated snapshots** for installing and for release notes ("Zenix 2026.11"), not "version 2" that everyone waits for.
- **Always a way back:** an update keeps the previous version, and the boot menu can start it.
- Everything is 0.x with beta numbers until the owner calls a release 1.0.
The self-updates design (DESIGN, the week of 2026-10-26) and `zpkg` are built on this.

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
  inverse, underline), colours (DESIGN D14: 16 on AGA, 8 on OCS/ECS with fast RAM), UTF-8 later.
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
  matrix on a stock 68EC020 with 2 MB chip RAM and no fast RAM, and on 1 MB chip-only with two login sessions (one before the shell memory fix of 2026-10-25). Until the
  owner decides, keep it running on those, and say in a change's notes when it costs memory or speed on them.
  **The owner, 2026-10-26:** the stock A1200 (2 MB chip, no fast RAM) stays a supported machine for as long as it is
  possible without sacrificing Zenix's goals; no sloppiness that pushes it out early. A desktop and all features may
  realistically need 4 MB fast RAM; raising the minimum is the owner's decision, brought to them with measurements
  when a feature cannot fit, never done quietly. Big features may need more while the base system keeps fitting.

- ZenixFS name: a Western Digital project already uses "ZenixFS" (RocksDB on zoned SSDs); decide if that matters.
- How much POSIX: `fork` is not possible without an MMU; `vfork`/`spawn` only. Programs that need `fork` are out.
- Journaling: not needed if ZenixFS commits atomically; ext2 gets a clean flag and our fsck.
