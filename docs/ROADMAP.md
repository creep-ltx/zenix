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

Progress: **batch A (first-boot blockers) is done** (A7, a chip-only A1200 booting the RAM disk, fixed with the installer, 2026-11-01), with the approved first-boot diagnostics (A11 boot colours, A12 previous-boot
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

**The minimum (owner 2026-11-04):** Zenix is made for a stock A1200 with 4 MB of fast RAM; a 2 MB chip-only A1200 is
best effort, not a target (not a hard limit; as lean as possible stays the rule).

## The order of work (approved by the owner 2026-10-25)
The phases below say *what*; this says *in which order*. When one item is done, the next starts at once. Real-hardware findings and problems found along the way are
fixed next, ahead of everything here ("fix things when we find them"). A focused audit closes every block; its findings
are fixed at once. Two independent design reviews for deep core changes, one for features.

**The real-world benchmark (owner 2026-11-04: "having the mounted Amiga partitions give a better real world value"):**
`time ls -laR /` on the A1200 with the 1260, the owner's AmigaOS partitions mounted (/mnt/dh0 PFS3 1 GB, /mnt/dh2
PFS3 8 GB), to /dev/null and on the screen, in the text console and in zdesk's term. First figures (release
2026110403, measured): /dev/null 4.36 s; text console 12.8 s; term 2 min 51.8 s. PiStorm (Emu68, not a real-Amiga speed), the fresh ZenixFS SD root without Amiga partitions, release 2026110404 (owner's photos 2026-11-04): /dev/null 0.08-0.12 s, text console 1.37 s, term 0.32 s (release 2026110403 on the old SD card: console 1.95 s, term 44.8 s).

**Real-hardware findings waiting (the A1200 with the 1260, 2026-11-04), fixed next:** the 040/060 data cache on for fast
RAM through the transparent translation registers, chip RAM uncached (done 2026-11-04, D37: 4.23 s -> 0.95 s on the 1260; boot log was: "data cache off"; `ls -laR /` of 506
lines took 4.26 s, 9.64 s on the screen with the serial cable in: measured; built on branch cache 2026-11-04, DESIGN
D37, docs/d37-data-cache.md: not yet on the 1260); the serial console drops input on long
lines and Zmodem through it stalls at 3 KB; once anything was typed over serial the console's serial copy waits for the 115200-baud port and caps the screen at about 11.5 KB/s (`ls -laR /` on the screen 3.2 s, 0.95 s to /dev/null, the 1260): a cable that stops listening must not slow the screen, and root needs a command to switch the copy off; netd should start a card it finds with DHCP without /etc/network/interfaces;
the installer asks for a package server; `zpkg -Sl` lines within 80 columns, "newer/older installed", zenix-tests and
zpkg-new kept out of a user's list; halt, poweroff and shutdown (done 2026-11-04); ZINE: dos.library Execute (done 2026-11-04: answered as done, the command named; d29 16.7); zdesk: dragging a window tears and flickers on the A1200 (owner 2026-11-05, the demo floppy: the moved window's contents shown at two places mid-move, "drag whole"): move at most once a frame, the copy timed to the beam (after the raster passes, or in the vertical blank) in the blitter's safe direction, so every frame shows the window whole at one place, as Workbench's outline drag never tears (done 2026-11-05 in FS-UAE: drawn off the screen, shown timed to the beam, d38-desktop.md; with it the text-UI programs follow a terminal window's resize); zpkg: when the boot partition is refused and /bin/zpkg-new is newer, say "run zpkg-new -Syu" (owner 2026-11-04: the old zpkg only said BOOT could not be checked); ZINE: a write to any path (T:, ENV:, the program's folder, SYS:) kept in memory for the session like RAM:'s and read back from there, never written to a disk (owner 2026-11-04: "not all demos write to the same location"); Latex's Ltx-Snigelsaft plays with its SID music on the A1200 (owner 2026-11-04, release 2026110407: RAM: files, Assign, playsid.library loaded from RAM:, d29 16.8); done 2026-11-04 in FS-UAE with a library of ZINE's own: RAM: files, Assign, libraries from LIBS:, DeleteFile, audio.device's channels, d29 16.8; the demo itself still to run on the A1200) and Peek's packets to its overlay handle (done 2026-11-04, runs in FS-UAE); Tab completion lists whole paths (`cd /mnt/sh0/Demos/<Tab>` shows /mnt/sh0/Demos/Latex/ ... where bash shows Latex/); zine from a term window on the desktop (owner 2026-11-04; refused for now: ZINE's way back restores the text display, not displayd's, a D38 change; meanwhile a text console, d29 16.7); the installer's manual layout: any existing partition chosen as start-up, root or /home, each kept or formatted (owner 2026-11-04; the PiStorm card with FAT32, a 0x76 holding BOOT and Share, ext2 root, ext2 home: today it is not recognised); the plipbox at 47 KB/s on the 1260 against about 114 KB/s documented (the wait between frames); diskd's note about the RDB chain on the installed card (with the installer helper).

1. **Foundations finished (the week of 2026-10-26):** D13 shared code; the shell's memory fix; the colour console;
   the self-updates. Audit.
2. **Zenix reaches the world** (done in FS-UAE 2026-10-29, closing audit docs/audit-2026-10-29-block2.md; the real A1200 checks are the owner's weekend; reported 2026-11-03: telnet to a BBS with downloads works on the real A1200): signals and `select`/`poll`; the network (the CM4's Ethernet driver, TCP/IP as a
   server, DHCP, DNS); `ping`, `telnet` (made for BBSes, the owner's daily use: ANSI colours, the PC character set and Amiga
   fonts, the BBS window size, downloads), `wget`, an NFS client; a Wasabi-like remote control (early: it lets the A1200
   be tested remotely); **one small crypto library** (owner 2026-10-26: "I need to be able to SSH into my Linux machine,
   properly"): modern, small and constant-time, e.g. BearSSL (MIT) for TLS and Dropbear's client (MIT) for SSH, with
   Ed25519/X25519/ChaCha20 (fast without an FPU or AES hardware) and a real random source; it also signs `zpkg`'s
   packages; then `ssh` (a proper client, keys, known hosts) and HTTPS for `wget`; `zpkg` with its repository on the
   owner's NAS. A security audit. Then (postponed by the owner on 2026-10-28; its desktop edition approved 2026-11-04, block 5) **the Zenix demo floppy**
   (owner 2026-10-25, in the spirit of the 1999 QNX demo disk): one 880 KB Amiga floppy that boots into Zenix with the
   network, `telnet` and a web-page fetcher, to hand to anyone; a second edition with the desktop in block 5. The model (owner 2026-10-26): KolibriOS, a
   desktop, network and programs on one 1.44 MB floppy; as there, the programs are packed on the disk and unpacked
   when they start, which is what makes a desktop edition fit 880 KB.
3. **A terminal you can live in** (done in FS-UAE 2026-10-30, closing audit docs/audit-2026-10-30-block3.md; the public pre-release waits for the owner; the LHA/LZX rewrite is built, docs/d33-archives.md): a fuller C library (picolibc, newlib's descendant: D27; steps 0-4 built 2026-10-29); D13 stage 2 (shared libraries) decided; the text-UI
   library (ported 2026-10-29: ports/tui), then Sumi, Maki and Markdown (ported 2026-10-29: ports/sumi, ports/maki, ports/markdown) and `zc`; UTF-8 in the console (D28 steps 1-6 built 2026-10-29, branch utf8); PC (CP437) and other bitmap fonts for the console
   (the PC set comes earlier, with telnet); (owner 2026-10-26) the owner's Ahiru file manager and `ctop` ported (ported 2026-10-30: ports/ahiru, ports/ctop). (Postponed by the owner 2026-10-29, "can wait":
   the tracker music player, the IRC client and the e-mail client; later, unscheduled.) Audit; then a first public pre-release (0.1) (owner 2026-11-03: not before a desktop, see block 5),
   with the owner's yes.
3½. **Amiga demos on Zenix: ZINE** (steps 1-3 merged 2026-10-31: four demos run in FS-UAE, d29 16.3; on the real PiStorm A1200 demos run with their sound, reported 2026-11-03) (`zine PROGRAM`, or the program's name) (approved by the owner 2026-10-29: "I would like to be able to watch an Amiga demo in
   Zenix. That would be amazing. So yes, please move it up"): a design note first (reviewed twice: it hands the whole
   machine over and takes it back), then built: run an AmigaOS executable with `zine PROGRAM` or by its name (no disk images for now: owner 2026-10-30), Zenix saves its
   state and steps aside, the demo has the hardware and chip RAM, and Zenix comes back when it ends, as demos return
   to Workbench. Demos on the bare hardware first; demos that need AmigaOS's libraries later. Starts when block 3's
   audit is done. The idea's notes: docs/future-ideas.md "Amiga demos on Zenix".
4. **The Amiga world:** FAT (built 2026-10-31 in FS-UAE: docs/d30-fat.md, `/sbin/fatd`), FFS/OFS (built 2026-10-31 on branch ffs, FS-UAE: docs/d31-ffs.md, `/sbin/ffsd`), PFS3 read-only (built 2026-10-31 on branch pfs3, FS-UAE and against AmigaOS's own volumes: docs/d32-pfs3.md, `/sbin/pfs3d`); LhA/LZX unpacking (built 2026-11-01 on branch lha, FS-UAE: docs/d33-archives.md, `unlha`, `unlzx`, the clean rewrite that settles the public-release blocker); ZenixFS as the root on real hardware (built 2026-11-01 on branch zroot, FS-UAE and the host: docs/d34-zenixfs-root.md; the real A1200's trial is the owner's decision, its steps in the note); the
   installer (built 2026-11-03 on branch install, FS-UAE and the host: docs/d35-installer.md; never run on real hardware yet); (owner 2026-11-03: "do the FA411 driver after the installer") a PCMCIA NE2000 network driver for the
   owner's Netgear FA411 in the A1200's side slot (`/sbin/net/ne2000`, D19's driver protocol), so the A1200 with the 1260
   reaches the network (built 2026-11-03 on branch ne2000, the PC against a model and FS-UAE's refusals: docs/d36-pcmcia-ne2000.md; never run on a real card yet); (owner 2026-11-04: "Maybe we can get it on also while we wait?") the plipbox on the parallel port (`/sbin/net/plipbox`, built 2026-11-04 on branch plip, the PC against a model of the box: docs/d39-plipbox.md; never run on a real box yet). Audit, with the filesystems fuzzed against damaged disks.
5. **The desktop** (owner 2026-11-03: "I want a desktop before a public release ... starting with a small minimal desktop
   with floating windows. Something classic but modern."): first a design note with a few looks to choose from (pictures,
   the owner picks; written 2026-11-04: docs/d38-desktop.md, four looks in docs/desktop-looks/), then **a minimal desktop with floating windows** as the first step, and the first public pre-release
   after it. (Owner 2026-11-03: "it's a Unix-like system. Several desktop choices are expected.") So the display
   server and the desktop are separate programs, as X11 and Wayland's compositors keep them: a documented protocol, and the
   window manager / desktop a program of its own that can be swapped; the floating desktop is the first, an i3-style
   tiling one a second choice, others possible. Then the rest: the display server; a terminal window, workspaces with the i3-style bar, the mouse; the file view,
   viewers, preferences; TrueType fonts (owner 2026-10-26; rendered once per size into bitmap fonts and cached, so a
   68020 without an FPU pays only once; also usable by the console). **The demo floppy's desktop edition** (owner
   2026-11-04: "We have a desktop. Now can we have a demo disk that boots into the desktop? ONE adf. With ahiru, Sumi,
   ctop, preferably."; docs/d25-demo-floppy.md revision 3, Part 1: one DD ADF, the whole disk unpacked into memory at
   boot, for the minimum's 4 MB of fast RAM) comes **next in block 5, after the desktop's real-hardware findings**,
   ahead of the file view, viewers, preferences and TrueType, in its section 10's order (the box, the stage 2 loader,
   `make demo`, the session, the keyboard, the network found by itself, `demo-test.sh`, the owner's A1200). Audit.
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
  inverse, underline), colours (DESIGN D14: 16 on AGA, 8 on OCS/ECS with fast RAM), UTF-8 (DESIGN D28).
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
