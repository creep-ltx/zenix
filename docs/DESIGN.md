# Zenix design decisions

The numbered decisions, D1-D13. D1-D9 were proposed on 2026-10-22; D1-D10 are decided and most are built ("Where
the decisions stand" below); D11 is designed, reviewed and approved by the owner (2026-10-24); its core (steps 0-5) is built and merged, steps 6-9 (mounts as services, `/etc/svctab`, the servers' output in the kernel log, the documents) are built on branch d11 (the note's section 15); D12 is built; D13 is approved, its design note awaiting review. Last checked against the code: 2026-10-23.

## Goal
A Unix-like system (processes, `/` filesystem, pipes, signals, a shell) for the Amiga that is fast and uses
the Amiga hardware: blitter, copper, Paula, chip/fast RAM. Not Linux, not AmigaOS-based.

## Decisions (D = decision)
D1. **Messages at the core.** Small kernel: scheduling, memory, interrupts, message ports, buffer cache.
    Filesystems and drivers are separate tasks reached by message ports (pointers, no copies), loaded only
    when used. Hot paths (cached reads) stay inside the kernel.
D2. **Programs run in user mode**, enter the kernel with TRAP. Anything that needs no kernel (memcpy,
    strings, clock from a shared page) stays in the C library. Measure the 68020 TRAP cost early.
D3. **Relocated at load, no MMU.** Loader places programs anywhere in RAM using a relocation table. Static
    linking first; the file format must leave room for position-independent shared libraries later. Fixed
    stack size declared in the program; per-process memory lists freed on exit; big blocks from one end of
    RAM and small ones from the other; slab allocator for kernel objects. No `fork`: `vfork`/`posix_spawn`.
D4. **Graphics: planar is native.** A display server owns the screen and draws (fill, blit, text, scroll)
    with the blitter and copper. Programs draw through it; no raw chunky framebuffer except a slow
    separate mode. Small fixed set of OCS/ECS/AGA modes.
D5. **Filesystem: ext2** behind a VFS layer. Standard little-endian format (swap on read, metadata only),
    128-byte inodes, no extents or 64-bit, old revision flags, readable by Linux. Host `e2fsck` for now.
    ext3 clean volumes may be read; a journal can come later. Timestamps unsigned (good to 2106).
    The kernel is loaded from the boot floppy. Written from the public ext2 specification (since D9 Linux's
    GPL-2 code could be reused too; it was not needed). FAT is a server of its own (D30), not in the kernel. Buffer cache
    always. Later added: ZenixFS, the native filesystem (zenixfs.md), behind the same server protocol.
D6. **Scheduler:** preemptive, priority-based, round-robin within a priority, event-driven wake-ups;
    interactive tasks (terminal, display server) high priority; lazy FPU save only if an FPU exists.
D7. **Boot/test:** FS-UAE (68EC020, no MMU, and the matrix of other setups) first, then the owner's A1200 with a
    PiStorm (Emu68), the target (ROADMAP principle 6).

## Basics
- CPU: 68020 or better, 32-bit flat, no MMU required; an MMU may be used later for optional protection.
- Crashes are contained, not prevented: the kernel catches bus error, address error, illegal instruction
  and divide by zero, kills only that process and frees what it owned; a magic word in every memory block's header
  catches heap overruns (there is no guard below a stack). A wild write can still corrupt another process; that is
  the cost of having no MMU.
- Toolchain: m68k-elf-gcc (`TOOLPREFIX`, README.md); no newlib: Zenix's own small C library, `user/libz.c`.
- Desktop later (workspaces, the bar and the file view are *desktop* features, not terminal features): workspaces with an i3-style bar (listed only if current or occupied), one Ahiru-style
  file view.

## Reused from zenix-old
Bootblock, serial console, Gayle IDE, RDB partitions, keyboard, syscall idea, build and FS-UAE setup.
**Rewritten:** process switching (shared memory map), loader (relocatable ZEXE instead of ELF), memory manager,
exception handling. **Kept in `archive/` but not built:** the old console, ELF loader, FAT16, kmalloc and signals
(`archive/README`). **Dropped:** 68030 MMU, per-process address spaces, mandatory FPU.

## D8. Multi-user (decided 2026-10-22, built 2026-10-22: see architecture.md section 14)
Zenix is multi-user from the start of the terminal phase, as a Unix expects, with one honest limit: **no MMU means no
security boundary** - any program can read or write any memory and touch the hardware. Users, groups and permissions
therefore give accounting, separation of home directories and protection against honest mistakes, not protection
against a program written to cheat (the same trust level as AmigaOS). The docs must say so. (D41, 2026-11-12: with
memory protection switched on, on a CPU with an MMU, an ordinary program can no longer reach other memory or the
hardware; the system's servers and root's powers stay as written here.)
- A user id and group id (and up to 8 supplementary groups since 2026-10-31, set by login, su and sudo from
  /etc/group) on every process, inherited by children; calls getuid, geteuid, getgid, setuid (root, or back to the
  real id), setgid, groups, umask.
- The kernel passes the caller's ids to the filesystem server in every request; the server checks owner/group/other
  r/w/x on lookup, open, create, unlink, rename, chdir and exec; root bypasses. New files get the caller's ids and
  mode & ~umask. ext2 already stores uid, gid and mode.
- /etc/passwd, /etc/group (plain text, no shadow file until there is something to protect), `login`, `su`, `id`,
  `whoami`, `chown`, `chgrp`, `umask`, `passwd`; a home directory per user. A login on each of the ten virtual consoles
  (owner, 2026-10-23: like Linux; switch user with `su`, or log in on another console).
- The shell prompt shows `$` for a normal user and `#` for root (it already does, from $USER).

## D9. Licence: GPL-2.0-only (decided by the owner, 2026-10-22)
"Only" on purpose: nobody may close it, and code from Linux, Fuzix and U-Boot (GPL-2) can be taken. Code from other
projects must be GPL-2 compatible and keeps its own licence header (miniz and `user/regex.c` from CUtils: MIT). Every
Zenix file carries `SPDX-License-Identifier: GPL-2.0-only`. (The "MIT" seen earlier was inherited from zenix-old, never chosen.)

## D10. Priorities (approved by the owner 2026-10-24, to be built after the audit fixing and its closing report; first item of future-ideas "the kernel best-of list")
**What.** Every process has a priority. The highest ready priority always runs; processes of the same priority take
turns (round robin, 50 ms slices) - the Exec rule, which DESIGN D6 always intended.
- **Levels:** 32 levels inside the kernel. Users see the Unix `nice` scale (-20 most favoured ... 19 least), mapped onto
  them; one scale for users, one inside, no third.
- **Who may change what (Unix rule):** anyone may lower their own processes' priority (`nice`, `renice`); only root
  may raise it. A child inherits its parent's priority.
- **Defaults:** the built-in servers (diskd, fsd, zfsd) above normal programs, so a busy program never delays the disk
  and filesystems everyone waits for; login shells and programs at the normal level (nice 0).
- **QNX priority inheritance (the second row of the best-of table, same step):** a server that receives a message
  runs at least at its client's priority until it replies. Simple here because calls are synchronous: the boost starts
  at receive and ends at reply. A low-priority program can then not hold up a high-priority one through a shared server.
- **Waking a higher priority:** when an interrupt or a reply makes a higher-priority process ready, the running one
  gives way at its next return to user mode (a "reschedule" flag checked where `kwork()` already runs). The kernel stays
  non-preemptive; no locks are added.
- **Run queue:** one list per level plus a 32-bit mask of non-empty levels; the 68020's `BFFFO` instruction finds the
  highest in one step. O(1), as today.
**What it replaces:** the single run queue (rq_head/rq_tail). Nothing old stays next to it.
**Not taken:** Linux-style fairness/aging (complex) - Exec shows that strict priorities work when programs wait for
input, and the "only root raises" rule stops a normal user from starving others. Revisit only if a test shows starvation
in normal use.
**Interface:** POSIX `getpriority`/`setpriority` (two system calls, numbers claimed in CLAUDE.md first), commands `nice`
and `renice`, a PRI column in `ps` and `top`-style tools; docs and man pages in the same change.
**Tests (in `make check`):** equal priorities alternate only at slice end (the fairness lesson in future-ideas);
a higher one always wins; nice/renice rules for users and root; priority inheritance: a high-priority client is not
slowed by a low-priority busy program while a server works for it; the existing matrix/jobs/kbd tests unchanged.
**Measured:** task-switch and message round-trip cost before and after (`bench`), in FS-UAE and on the A1200.
**Cost:** a few hundred bytes of kernel; one more word per process.

## D11. Service manager (built: steps 0-5 merged, steps 6-9 on branch d11, the note's section 15; reviewed twice and revised; approved by the owner 2026-10-24, questions answered in the note section 12. Owner 2026-10-24: may be built after D10 once its design is complete and fully thought through - a full design note, reviewed, before any code)
**What.** One mechanism for starting and keeping the servers (Exec "load on first use" + Minix 3 restart + launchd, as
the future-ideas table asks): a registered port name always exists, in a slot range of its own; the first message to
it starts its server, and a server that dies is started again by the next message, with limits (5 ends in 60 s:
given up; for diskd and the root filesystem a panic), every event in the kernel log. The client that needs a stopped
server loads it in its own context, as root (our extension of ramlib's escape from the loader deadlock), so there is
no manager process to deadlock. The kernel keeps the table and the held ports; `boot` fills it (diskd, the root
server with its device's place and volume id, `/etc/svctab`); `mount` registers its servers as the built-in images. A
restarted diskd restores its device slots and claims from a small table it left with the kernel; a restarted
filesystem server rebuilds its open-file counts from the kernel's open files. **Replaces** the hard-wired server list
in `kmain`, the polling for ports in boot, fsd, zfsd and mount, `/bin/mount.ext2` and `/bin/mount.zenixfs`, and "a
dead server stays dead". **Not in it:** port rights, stopping idle servers when memory is low, keep-alive servers,
dependency lists. **Interface (proposed):** one system call (`SYS_svc`: register, remove, self, save, load, opens), one
error number (ECONNRESET), no new message types, no new command. **Cost (estimated):** about 2.7-3.6 KB of the 5.1 KB
left on the RAM floppy, about 1.8 KB of RAM, one test per message call. The full design, its tests, build order, the
owner's open questions and the bugs found in today's code: [d11-service-manager.md](d11-service-manager.md).

## D12. The serial port as a device, and `cu` (approved by the owner 2026-10-24)
**Why.** Zenix needs a way to talk to another computer before there is a network: a terminal session on a PC
through the null-modem cable. Today the serial port belongs to the console: everything shown is
copied there (the mirror), what arrives is typed into the shown console, and `kprintf` writes the kernel log there.
That serves the test rig and a debug terminal, but a program cannot use the port.

**Decision, as Linux and BSD do it.** The port is also a device, `/dev/ttyS0`, handled where the other devices are
(`open_device` in vfs.c, a new file type FT_SERIAL; no server, like `/dev/tty1..10`). One process may have it open at a
time (a second open gets -EBUSY); root only, as for the raw disks (the port's other end is a whole other computer).
While it is open the port is that program's alone: the console's mirror stops, received bytes go to the device and not
to the console, the 0x1D test hook is off, and `kprintf` keeps writing the kernel log to its ring (`dmesg`) but not to
the wire. At the last close everything is as before. Reads return the bytes the receive ring holds (blocking until at
least one, the way a tty in raw mode does; `O_NONBLOCK`-style polling is not needed by `cu`); writes go out through
the existing 4 KB send ring, waiting for room (a tick at a time; a kill or 2 s with no progress ends the write, audit
A5). 115200 baud 8N1, no handshake (the cable's minimal wiring); a later
option may set the speed. Nothing else changes: the receive interrupt still only queues (audit B4), `serial_work`
hands the bytes to the device instead of the console.

**`cu`.** A user program, the classic Unix "call up": `cu [-l /dev/ttyS0]` puts the console in raw mode, copies keys to
the port and the port's bytes to the screen, and ends at `~.` typed at the start of a line (as in BSD/Linux cu; `~~`
sends one `~`). Two directions at once without threads or `select`: `cu` spawns a second copy of itself for the
port-to-screen direction and keeps the keyboard-to-port one, ending both at `~.` (Zenix has no `select` yet; when the
signal-bit waiting of future-ideas item 1 exists, `cu` can become one process). The PC side is a login on the USB
serial adapter (`agetty` on /dev/ttyUSB0 at 115200), set up once on the PC.

**Not in D12:** network (the same over TCP comes with the network driver), the console's colours and Unicode stand-ins
(a later step), handshake lines, file transfer.

**Tests.** FS-UAE's serial port is a PTY on the host: a test opens it, writes bytes and checks `cu`'s screen and what
arrives, the mirror off while `cu` runs and back after `~.`, -EBUSY for a second open, a non-root user refused, and the
kernel log not on the wire while the device is open. On the A1200 with the cable (from 2026-10-31): a terminal session on
the PC, shown in Zenix.

## D13. Shared libraries and shared program code (approved by the owner 2026-10-24 as the step after D11; design note to come, reviewed before any code)
**Why.** Zenix is for machines with little memory. Today every program carries its own copy of the libz functions it
uses (5-7 KB each, measured 2026-10-24), and worse, every running copy of a program loads its whole code again: four
shells on four consoles hold four copies of `sh` (86 KB each). AmigaOS solved both without an MMU: libraries with jump
tables, opened by name, one copy in memory; and "pure" programs whose code is shared while each process has its own
data. D3 left room for this in the program format. D13 builds it: one copy of the code, a data area per process (a base
register, as gcc's `-msep-data`/`-mid-shared-library` for no-MMU systems), libz as the first shared library. It comes
before the self-updates (so they ship the final format), newlib and the network (which make programs much larger).
The full design is in [d13-shared-code.md](d13-shared-code.md) (revision 3, 2026-10-25, after a first review and a second look: stage 1 makes `sh` and `login` pure; ready to build).
## D14. The colour console (approved by the owner 2026-10-25; reviewed; built and merged 2026-10-26)
**Why.** Colours in the text console, as every Unix terminal has: `ls`, `grep`, the prompt, `less`, and programs using
the standard ANSI colour codes. **How.** More bitplanes behind the same text console, chosen by the machine at boot (4
planes and 16 colours on AGA with fast RAM at the 64-bit fetch, which costs the chip bus what one plane did; 3 and 8
colours on OCS/ECS with fast RAM and, since the 2 MB diet (D18), on every machine with 2 MB of chip RAM and no fast
RAM; 1, today's look, on chip-only machines below 2 MB; `/etc/console` chooses 2, 4, 8 or 16), a colour byte per
cell and in the scrollback, the ANSI codes in term.c (256 and true colour mapped to the nearest of 16), region scrolls
by blitter, `ls`/`grep --color` with GNU's bytes. The full design and what the build changed:
[d14-colour-console.md](d14-colour-console.md) (sections 10 and 11).
## D15. Zenix updates itself (approved by the owner 2026-10-24; design note revision 3 after two reviews: ready to build)
**Why.** No more SD card swaps and `dd` on the PC (which also reset the owner's settings). **How.** One update bundle on
BOOT:; zenixboot (still in AmigaOS) installs the new kernel and keeps the old one, and hands the root packages to Zenix
in memory, as it already does for a RAM disk; Zenix's installer applies them transactionally (a journal, rename-over,
the previous version kept, the owner's changed settings never overwritten); the boot menu's "Zenix (previous)" goes
back. The same bundle and installer serve the serial cable and the network later (`zpkg`). The full design:
[d15-self-updates.md](d15-self-updates.md).
## D16. Events and signals (roadmap block 2, the first item; design note revision 3 after two reviews; built on branch d16, 2026-10-26)
**Why.** A process can wait for only one thing at a time, every I/O event wakes every waiting process, and there are no
Unix signals (no handlers for Ctrl-C, no SIGCHLD, SIGALRM or SIGPIPE): the network, telnet and ported programs need
all three. **How.** Exec's signal bits underneath (a pending and a waiting word per process, one AND decides a wake-up,
no lost wake-ups), per-object waiters, `poll`/`select` on one new call, and Unix signals with handlers delivered through
a frame on the user stack and a libz trampoline; process groups, so ^C and ^Z reach a whole pipeline. The full design
and what was built: [d16-events-and-signals.md](d16-events-and-signals.md) (section 15, "As built").
## D17. The ext2 clean mark and the check at boot (audit 2026-10-26 F5, F6; design note revision 2 after two reviews; built on branch fsck)
**Why.** ext2 has no journal: a power cut while it is written leaves leaks (since F5 never worse), but Zenix never
marked a volume "not cleanly unmounted" during use and never repaired one by itself. **How.** fsd marks a writable
ext2 volume before the first change and clean again one second after the last one (AmigaOS FFS's bitmap rule, so an
idle switch-off costs nothing), never over a half-done change; boot checks a marked root with `fsck.ext2 -y` while a
read-only server serves it, then starts the writable one; `mount -a` checks a marked volume of `/etc/fstab`. No new
system call or message. The full design: [d17-ext2-clean-mark.md](d17-ext2-clean-mark.md).
## D18. The 2 MB diet (approved by the owner 2026-10-26; built on branch diet, one commit per step)
**Why.** The stock A1200 (2 MB of chip RAM, no fast RAM) is the floor Zenix stays on as long as possible, and one login
left 1,151 KB free where Workbench 3.1 leaves 1,737 KB (FS-UAE, 2026-10-26). **How**, each step taking nothing the user
feels: (1) without fast RAM the console shows 8 colours (3 planes, 120 KB of screen) instead of 16, and
`/etc/console` chooses 2, 4, 8 or 16 at the next boot (one new call, `SYS_console`; D14 note section 11); (2) fsd's
block cache is one block of memory sized by the machine (as before with fast RAM, 24 KB without; it was grown in
chunks that left a third of it unused); (3) **login steps aside**: Zenix has no `exec`, so a login used to wait for the
whole session to hand init the shell's end. Now login spawns the shell as before (as the user, in the user's
directory, with the user's environment: only the process that has become the user can start the shell as the user)
and then ends with the second argument of `SYS_exit` naming that child: the kernel makes it its parent's child
(init's), as if init had started it, and tells init at once if it has already ended. Linux's `CLONE_PARENT` and the
Unix exec chain getty -> login -> shell keep the same shape: init waits for the session itself, a logout is the
shell's end, init starts a new login on that console. login's exit status says which child it handed over (bit 30 plus
its pid), so init knows the console's session; nothing on the disk (no utmp). Only init's own login does it (`login
--init`): a login typed in a shell still waits for its shell. Process groups (D16) do not change: login's group is
left before, the interactive shell makes its own and the terminal's foreground, and a non-interactive shell stays in
the group login made. (4) Smaller stacks for init (4 KB) and diskd (8 KB), where 16 KB were reserved and under 400
bytes are used (measured), fsd's two block-sized static buffers dropped, the floppy's verify buffer allocated with the
floppy's other buffers. **Measured** (FS-UAE, `free` with one login): 2 MB chip-only 1151 -> 1332 KB, 1 MB
chip-only 254 -> 395 KB (four sessions instead of three; a session 78 KB instead of 110); each step's share in
features.md. **Not taken**, and why: the kernel's fixed tables for 32 processes (about 15 KB: halving them would be
felt with ten consoles in use, and a dynamic table is a kernel change of its own); fewer scrollback lines (felt); the
7 KB glyph table for more than one plane (drawing speed); libz's 4 KB output buffer in every program (output speed,
and every program would need a change); the double-height screen buffer (the owner waits for a real-68020 scroll
test); fsd's 16 KB stack (its path walk recurses up to 9 symlink levels); **the embedded server images** (fsd,
diskd, boot and zfsd: 79 KB of the kernel image stay in memory, and each running server is a copy; zfsd's 37 KB are
never used with an ext2 root): the biggest remaining win, but it needs execute-in-place or zfsd from the disk, a
loader change with its own design note and reviews (zfsd from the disk: D19 step 1, built).
## D19. The network (roadmap block 2; design note revision 2 after two reviews; steps 0-9 built 2026-10-27, step 9 not yet on the A1200)
**Why.** Zenix must reach the world: `ping`, telnet to BBSes, ssh, wget, NFS, a remote control, `zpkg`. **How.** One
TCP/IP server, `netd` (lwIP 2.2, BSD-3, `NO_SYS`, raw API), a service boot registers, loaded from the disk by the
first socket call; small user-mode driver processes as its clients through the kernel (the CM4's GENET; an A2065 for
the emulator), SLIP over `/dev/ttyS0` inside netd for every Amiga; BSD sockets in libz over one multiplexed system
call, a socket a kernel file kind that blocks in the kernel, polls and takes signals like a pipe, with ready bits only
netd changes and an epoch that ends every socket when netd ends; claims, DMA memory and a quiesce so hardware never
writes into freed memory; a kernel entropy pool with `/dev/urandom` and `/dev/random`. **Two rules for the design**:
every driver not needed to reach the root (the network, later USB and sound) is a user-mode process (block drivers
stay in the kernel); a server may supervise its own helper processes, with D11's numbers. **First, on its own (step 1,
built on branch zfout 2026-10-27):** zfsd leaves the kernel image (mount registers `/sbin/zfsd`, D11 6.4; `@zfsd` one
alias to that file, never for the root); the kernel 176,144 -> 139,644 bytes, free memory with one login +36 KB
(*measured* in FS-UAE: 2 MB chip-only 1329 -> 1365 KB, 1 MB 392 -> 428 KB); **a ZenixFS root is test-only** until roadmap
block 4 (the test kernel `kernel-zfs.bin`; zenixboot refuses an update that would take its server away; owner question
5 of the note still open). The full design: [d19-network.md](d19-network.md).
## D20. telnet, the BBS terminal and pseudo-terminals (D19 step 11; design note revision 2 after independent reviews; steps 0-5 built: the console's font and rules (t12), telnet's modes (t3), Zmodem and the phone book (m4), 2026-10-27)
**Why.** The owner's daily use of the Amiga is telnet to BBSes. **How.** One `telnet` process (`poll` on the socket and the tty; BINARY, ECHO, SGA, TERMINAL-TYPE, NAWS 80 x 32) whose PC, Amiga and Unix modes translate keys and filter the stream, while the console draws (a CP437 font and seven board rules per console, loaded on demand by two `SYS_console` ops that name the console by a file descriptor); Zmodem by `rz`/`sz` (CTerm's engine), started by itself; pseudo-terminals, SIGHUP and a telnet server later, as a step of their own. The full design: [d20-telnet.md](d20-telnet.md).
## D21. The crypto library, `ssh` and HTTPS (roadmap block 2; design note revision 2 after two reviews; steps 0-3 built: the library and ssh's core on the PC, `cryptotest`, `ssh` and `ssh-keygen` (branch m4, 2026-10-27); the A1200's time table pending)
**Why.** The owner must reach the Linux PC properly by ssh, `wget` needs HTTPS, and updates and packages need
signatures (D15 step 7). **How.** One static library, `crypto/`: BearSSL (MIT; TLS 1.2, X.509, RSA, ECDSA, AES, its
16-bit-multiply code that suits every 68020-68060) and Monocypher (CC0; Ed25519, ChaCha20-Poly1305; X25519 from whichever of the two is faster on a
68020, measured first), assembler only where measured to pay; Zenix's own small ssh client (`net/ssh/`, OpenSSH's file formats, D20's Unix-mode
keys and filter shared) instead of a Dropbear port; randomness only from `/dev/random`; Ed25519 signatures in detached
`.sig` files over D15's header digest, verified by zenixboot (once per bundle) and `zpkg`; root certificates as a compact
anchors file, the time from the battery clock, SNTP and a floor. No kernel change. The full design:
[d21-crypto-ssh-https.md](d21-crypto-ssh-https.md).
## D22. `zpkg`, the package manager (the owner's wish 2026-10-24; design note revision 2 after two reviews, nothing built)
**Why.** Installing and updating programs and Zenix itself the way Arch's `pacman` does (`-S`, `-Ss`, `-Syu`, `-R`,
`-Rns`, which package owns which file), as **one mechanism with the self-updates** (D15). **How.** A static
repository (plain files: the owner's NAS for testing over HTTP, GitHub Pages for stable over HTTPS): immutable package
files in `pool/` that are D15 bundles without a kernel, a signed package list per channel (`index-SERIAL` with D21's
signature, a serial against replay, an expiry against a frozen mirror) and a one-line `latest`. `zpkg` fetches,
checks and decides; **`/sbin/zup` stays the only installer** (two new modes: a package from a file, a removal; its
journal, prev links, three-way compare and recovery unchanged); D15's manifests stay the database. The system (the
kernel and the base group, the only content of a system bundle) is never installed live: zpkg writes its signed bundle onto the
boot partition as the two files zenixboot already reads (`Zenix/zenix-update.zup` and its `.sig`: new files of the
FFS volume in AmigaOS's safe order, after a check of the whole volume; owner requirement 2026-10-31, d22 amendment
2, which replaced the update slot), zenixboot installs it at the restart and D15 runs as built ("Zenix (previous)"
the way back). A package's way back is its previous version in the download cache. The full design:
[d22-zpkg.md](d22-zpkg.md).
## D23. The NFS client (roadmap block 2; design note revision 2 after two reviews; steps 0-4 built: reading, writing, users and nosuid)
**Why.** Mount the owner's NAS (or a Linux PC) as a directory: `ls`, `cp`, running programs from it. **How.** NFSv3
over TCP (MOUNT v3 and the portmapper to find mountd; no UDP, no NFSv4), AUTH_SYS from a privileged source port; one
server process per mount, `/sbin/nfscd`, a file service registered by `mount -t nfs HOST:/EXPORT DIR` as `fs.nfsN`
(D11 6.4; the host resolved by nfscd at each connect, the export's root handle kept in D11's saved table to check a
restart), which calls `net` (the one change to D11's "filesystem servers call only diskd" rule; NFS mounts are refused
on `/`, `/bin`, `/sbin`, `/etc`, so netd never reaches one) and never blocks: many requests in flight on one connection,
NFS handles mapped to the kernel's 32-bit inode numbers, close-to-open caching, UNSTABLE writes kept until a COMMIT with
an unchanged verifier, `FS_CLOSE` answered at once, `mount -a` never waiting for a NAS, always `nosuid`. "Hard" mounts
by default, made killable by two small kernel changes: **the cancel mark** (`UMSG_CANCEL` on the requests of a killed
client, `POLLPRI` on the server's port) and **the reboot sync** (network mounts synced last, `FS_SYNC` bounded). The
full design: [d23-nfs.md](d23-nfs.md).
## D24. The remote control, `wasabid` (roadmap block 2; design note revision 2 after two reviews; steps 0-2 built on branch wasabi1, 2026-10-28; step 3, the console ops 5-7 and the screen commands, on branch wasabi3, 2026-10-29)
**Why.** Test Zenix on the real A1200 with nobody at it, and let the owner's tools (the `wasabi` command, its Python
API, its MCP server for Claude, the apps) drive Zenix as they drive AmigaOS. **How.** `/sbin/wasabid` speaks the owner's
Wasabi protocol version 1 (frames, `caps`, discovery) and offers what fits Zenix: files (regular files only; network
mounts through a killable helper), `RUN` with two slots and live output, `PS`/`KILL`, the kernel log as `DEBUG`,
`HEALTH`, `REBOOT` (to Zenix or AmigaOS, through `reboot`, guarded), the console as pixels (`GRAB`, `SUM`) and as text
(a new `TEXT` that can wait for a pattern), keys; at most 4 connections within the 16 descriptors a process has; on its
own TCP port 1236 (the A1200's AmigaOS Wasabi is pinned encrypted on 1234); plain with a key of its own on the home
network first, encryption a later step. Five small root-only kernel interfaces (`SYS_console` ops `CON_READ`,
`CON_LINE`, `CON_INPUT` named by a console's descriptor, the kernel keeping the display's layout; `SYS_klog`'s
`KLOG_TOTAL`, replaced by the existing `KLOG_OLDEST`; `/dev/kmsg`). **A D11 decision:** servers nobody calls (network listeners) are kept alive by init, the
lines of `/etc/daemons` with D11's numbers (amends D11 1 and 4.4, D19 2.3; closes D20 7.2's open point). The full
design: [d24-remote.md](d24-remote.md).
## D25. The demo floppy (design note revision 3, 2026-11-04: the desktop edition first; measured, nothing built)
**Why.** The owner (2026-11-04): "We have a desktop. Now can we have a demo disk that boots into the desktop? ONE adf.
With ahiru, Sumi, ctop, preferably." One DD floppy (880 KB ADF) that boots an Amiga with a 68020+ and 4 MB of fast RAM
(the minimum) straight into Zenix Desktop, logged in as `guest`, with Ahiru, Sumi, ctop, 65 small tools in one
multi-call program, the manual pages and, when a card is found, the network by DHCP with `telnet` and Zmodem.
**How.** The KolibriOS model on the minimum's memory: the whole disk is unpacked into memory once, at boot. A stage 2
loader, read by the bootblock and run while AmigaOS still lives, reads the disk track by track with `trackdisk.device`
and unpacks each track while the next is read (deflate, miniz's `tinfl`, already Zenix's) into the kernel and a 1.95 MB
RAM disk that becomes the root; then the ordinary hand-over. The kernel is unchanged. *Measured:* the kernel and image
as one stream pack to 855,705 bytes deflate (994,348 LZ4: does not fit), about 34 KB free; in FS-UAE the desktop with
Ahiru, Sumi and ctop open leaves about 2 MB free with the RAM root (arithmetic from the measurement). *Estimated:*
40-50 s to the desktop, then every program starts from memory. `wget`, `ssh`, Maki and Markdown do not fit DD. A 2 MB
machine is refused with a message; revision 2's network edition for it (`ZPAK` packed programs unpacked by the kernel's
loader, a read-only `/usr`, the segment cache) is kept as the note's Part 2, postponed. The full design and the build
steps: [d25-demo-floppy.md](d25-demo-floppy.md).
## D26. Symbolic links across mounts (audit 2026-10-26 F12; design note revision 2 after four reviews; built on branch d26, 2026-10-28)
**Why.** A symbolic link is followed only inside its own filesystem, so a link into another mount answers ENOENT (and
an absolute target on any mount but the root is resolved from that volume's root); Linux follows it, and D25's links
from the RAM root into `/usr` need it. **How.** When a path request's path is a kernel copy (always while more than the root is mounted, and for
`chdir` and `mount`), the kernel marks it (`UMSG_LINKS`) and a filesystem server stops at the first symbolic link it meets and answers `FS_LINKED` with the text
the path continues at, written into the request's own path buffer; the kernel normalises it, picks the mount again
and sends the request again, at most 40 links per path (Linux's), then -ELOOP. `..` at a mount point is Linux's; the
working directory's text and mount points are kept resolved. A server not asked, and an old server, follow links
inside their volume as today. The bit is set only on a kernel buffer with an absolute path; each path is resolved
once per system call (D13's checks on the resolved mount); `umount` matches the text before asking any server. The full design: [d26-links-across-mounts.md](d26-links-across-mounts.md).
## D28. UTF-8 in the console, and the console's fonts (roadmap block 3; design note revision 2 after two reviews; approved by the owner 2026-10-28; steps 1-6 built on branch utf8, 2026-10-29)
**Why.** The console showed Latin-1 only, so UTF-8 from Linux, the web, `ssh`, Claude Code over `cu` and the
owner's own programs showed as junk. **How.** The kernel's terminal decodes UTF-8 in `put1` (one more parser state:
ASCII pays one test and one read, about 5 instructions of 266); nothing is decoded inside an escape sequence; a byte
that is not part of a character is one cell, its Latin-1 glyph (old Amiga text stays readable) or a box, never a
control. A cell is 16 bits: an `A_HI` mark in its attributes and a high plane allocated on first use (2,560 bytes;
the scrollback keeps high rows only for the lines that need them). Wide characters take two cells with xterm's rules;
zero-width ones are dropped. The widths are the kernel's, from the C library's own tables (`kernel/cwidth.h`, shared
with libz, 1,816 bytes). Glyphs: Topaz stays the look of Latin-1; a supplement font (ZCF1, `zenix-8x8.zcf`, 531
glyphs and 584 stand-ins, 10,424 bytes; box drawing, blocks and braille generated, CP437's symbols, Zenix's own
glyphs) is loaded by init at boot (`/etc/console`'s `font` line; `font none` turns it off) through `SYS_console` op 4
`CON_UFONT`. 8-bit mode (a `CON_FONT`, or the new rule `CONR_8BIT`): no decoding, Latin-1 keys (telnet's Amiga and PC
modes). Keys are UTF-8; cooked Backspace and `sh`, `edit`, `less`, `ls` work by characters and columns; `LANG=C.UTF-8`;
the C library's `setlocale("")` reads the environment. The full design: [d28-console-text.md](d28-console-text.md). Its section 20 (2026-11-04) made the screen a ring of rows shown by the Copper; section 21 (2026-11-07) draws at the end of each write: all its scrolls one move of the Copper's pointers, at most a screenful drawn, glyphs a long word per plane for four cells, no cell copied on a scroll.
## Where the decisions stand (2026-10-24)
D1 messages: ports + synchronous zero-copy calls done; the filesystems (ext2 `fsd`, ZenixFS `zfsd`) and the disk (`diskd`, the only one with raw-disk system calls) are user-mode servers; fsd and diskd are built into the kernel image, zfsd is the file `/sbin/zfsd` since D19 step 1. Drivers, tty, pipes and the display are still in the kernel. A hung server blocks its clients (a client in a call is only freed by the reply or by the server dying); a killed client of a hung server is cancelled (D23: `UMSG_CANCEL`, `POLLPRI` on the port; a server that answers it, nfscd, frees it).
D2 user mode + TRAP: done; the TRAP cost is **not measured** (emulator timing is not CPU timing).
D3 relocating loader, no fork: done (ZEXE format, tools/mkzexe.py); shared libraries not started.
D4 planar display server: not started; the kernel draws ten text consoles through Copper + blitter; a client API is not designed.
D5 ext2: rev 0/1, 128+ byte inodes, read and write (write-through, e2fsck-clean) done; RDB and MBR partitions done; no journal. ZenixFS built and verified in FS-UAE, also as the root.
D6 scheduler: preemptive on a 100 Hz tick, with priorities and round robin within a priority (D10); the kernel itself is not preempted (a switch waits for the return to user mode); no lazy FPU save yet (no FPU code).
D7 boot/test: FS-UAE done (the matrix, `make check`); the owner's A1200 not yet.
D8 multi-user: built (architecture.md section 14).
D9 licence: decided.
D10 priorities: built 2026-10-24 (branch d10): 32 levels, nice -20..19, the run queue per level with BFFFO, the
reschedule flag, QNX priority inheritance, getpriority/setpriority (PRIO_PROCESS and PRIO_USER), `nice`, `renice`, PRI
and NI in `ps`; verified in FS-UAE (`tools/prio-test.sh`); not yet measured on the A1200.

## Planned servers
- **index server ("find")**: keeps a FuzzyFind-style catalogue of the filesystem in memory, answers search messages
  for `locate`, `fzf` and tab completion, and is told about every create/delete/rename by the filesystem server so
  it is never stale. May also cache directory listings. Decide after the benchmark (tools/bench) shows where directory
  reads really spend their time. Reuses the FuzzyFind project's index layout and matcher.
- **display server**: the text consoles now inside the kernel, moved behind a port with a drawing API (D4); the
  desktop's workspaces and bar live there.

## Milestones (0-2 done in FS-UAE; none run on real hardware yet)
0. Boots in FS-UAE as an A1200 (68EC020, no MMU) and prints to the serial port.
1. Memory manager, task switcher, system calls, first program from disk.
2. Filesystem, tty, shell: the first "it is a Unix" moment.
3. Graphics server, the desktop, a terminal program. 4. Network, audio, the rest.
(The phases that replace these milestones are in ROADMAP.md.)

Open questions: how much of the old user programs port unchanged. (Whether to use an MMU optionally: D41.)

Written 2026-10-22; decisions updated and checked against the code 2026-10-23.

## D30. FAT (roadmap block 4; decided in the note, the owner delegated the details 2026-10-31; built on branch fat)
`/sbin/fatd`, a file service per mount: FAT12, FAT16, FAT32 with long names, read and write, through `kernel/fsproto.h`
like zfsd. No owners or modes on disk: mount options. Writes in the order data, FAT, directories, a freed chain zeroed
only after its directory is on the disk; the boot sector never written; the dirty mark in `FAT[1]`; a volume not clean is
read-only unless `-o force`. Details: `docs/d30-fat.md`.

## D31. OFS and FFS (roadmap block 4; decided in the note, the owner delegated the details 2026-10-31; built on branch ffs)
`/sbin/ffsd`, a file service per mount: AmigaDOS volumes `DOS\0`-`DOS\7`, read; `DOS\0`-`DOS\3` also written with `-o rw`, after a
check of the whole volume. The root's bitmap flag is never cleared: the order new blocks, bitmap, changed extension blocks,
changed headers leaves at worst lost blocks after a cut. Details: `docs/d31-ffs.md`.

## D32. PFS3 (roadmap block 4; decided in the note, the owner delegated the details 2026-10-31; built on branch pfs3)
`/sbin/pfs3d`, a file service per mount: PFS3 volumes, read-only, with no write code at all and `-o rw` refused. Written from the
on-disk format's facts, no PFS3 code (its licence does not mix with GPL). What the root block names is the state read; nothing is
replayed or finished. A damaged volume answers errors. Details: `docs/d32-pfs3.md`.

## D34. ZenixFS as the root on a real machine (roadmap block 4; the owner delegated the details 2026-10-31; built on branch zroot)
A machine's own choice, never a default: the kernel `kernel-zfs.bin`, as a second system bundle (`zenixz-VNUM.zup`, the list's `systemz` line), the same tree as an ext2 root. No check after a cut; a root only readable gets boot's choices; `fsck.zenixfs -y`. Repair never frees what it could not read, and rebuilds inner nodes from the leaves. Details, the trial's steps and risks: `docs/d34-zenixfs-root.md`.

## D35. The installer (roadmap block 4; the outline the owner's, the details delegated; built on branch install)
`zinstall`, a text dialogue on Zenix itself: from the install floppies, or an existing Zenix through `zpkg --fetch`. Free space by default on an RDB, or a new type 0x76 on an MBR; an existing partition by typing its name; a whole disk by typing `ERASE ide0`, never with a FAT partition on it. One sector write turns each table old to new, new partitions unbootable and unmounted until everything is written and read back. Details: `docs/d35-installer.md`.

## D36. An NE2000 PC Card in the A1200's slot (roadmap block 4; approved 2026-11-03, the details decided in the note; built on branch ne2000)
`/sbin/net/ne2000`, a D19 driver, polled: the Netgear FA411 (an ASIX AX88x90) and DP8390 cards in the A600/A1200's slot. The kernel's only part is one more base `DRV_CLAIM` takes, `DRV_PCMCIA` (the slot), granted only on an A600/A1200 Gayle with no memory in 0x600000-0xA4FFFF (-EADDRINUSE: refused, said). Gayle's PC Card interrupt stays off; the card is found by its CIS, taken only if it is a network card, configured as Linux's drivers do; taken out and put back while running. Details, the tests and the first steps on the A1200: `docs/d36-pcmcia-ne2000.md`.

## D37. The data cache on 68040 and 68060 without an MMU (approved 2026-11-04, "yes to 060 cache"; built on branch cache)
On a real 68040/68060 with fast RAM above the first 16 MB, the data cache is on for that fast RAM, set up through the transparent translation registers with the MMU off: `DTT0` keeps the first 16 MB (chip RAM, the chips, every I/O, Zorro II) inhibited, `DTT1` caches the smallest aligned block over the fast RAM, copyback by default, the 060's `TCR` default keeps everything else inhibited; the 060 also gets its store buffer, branch cache and superscalar dispatch. A self-test at boot turns it off if chip RAM reads back cached; zenixboot `DCACHE=OFF|WT` (bootinfo `+0x84`) or the left mouse button held at boot turn it off or to write-through. 68020/68030 and Emu68 unchanged. The hand-over to an Amiga program (ZINE) and a software reset push and switch it off first. Details, every push site, the tests and the first steps on the Apollo 1260: `docs/d37-data-cache.md`.

## D39. Ethernet through a plipbox on the parallel port (approved 2026-11-04, "Maybe we can get it on also while we wait?"; built on branch plip)
`/sbin/net/plipbox`, a D19 driver, polled, for plipbox 0.6 (an AVR and an ENC28J60 on the parallel port). The kernel's part: one more base `DRV_CLAIM` takes, `DRV_PARPORT` (CIA-A's port B and CIA-B's bits 0-2; its quiesce steps checked one by one, the serial port's bits never), CIA-A's FLG interrupt on while it is claimed, the level-2 handler counting the ACK line's edges, `DRV_PARACK` to read them. Sends in bursts, receives handshaked (a burst receive is unsafe from a process that can be interrupted); the box's own MAC kept; a box left in a burst freed. Details, the tests and the first steps on the A1200: `docs/d39-plipbox.md`.

## D38. The desktop (roadmap block 5; design note revision 1, proposed 2026-11-04, for the owner to pick a look and answer its questions; nothing built, not reviewed)
Two programs, as X11 keeps them: `displayd` owns the screen, the blitter, the pointer and the keys while the desktop is shown, and draws everything; `desk` (the floating desktop) is the window manager: placement, focus, stacking, the frame style, the top bar. A tiling desktop later is another program in its place. Programs send drawing orders in batches over port `display` (one message per batch, read in place); the server draws into the one screen with the blitter, clipped, and keeps hidden parts of windows in chip RAM (Intuition's smart refresh; simple refresh with expose events past a budget), never composites. Window insides start on 8-pixel columns (byte-aligned text). 4 planes at FMODE 3 on AGA (16 colours), 8 colours on OCS/ECS, one plane for the two-colour look. The kernel keeps the text consoles and hands one console to `displayd`, a switch waiting for its answer; the pointer is sprite 0. Pseudo-terminals (D20 7.2) come first; `term` shares the console's parser. Details, the estimates, the four looks and the questions: [d38-desktop.md](d38-desktop.md).

## D40. Sound: the Paula server, `play`, `modplay` (roadmap block 6; design note revision 2 after one independent review; built on branch sound, 2026-11-12)
`/sbin/sndd`, a D11 service on port `audio` (reserved), owns Paula's four channels through one kernel claim, `DRV_CLAIM` of `DRV_PAULA` (the process of port `audio` only; the bell silent while held; the kernel stops the channels at its end and before ZINE). No interrupt code: sndd reads Paula's request bits at times worked out from the E-clock (Paula's clock / 5), woken by the 100 Hz tick. PCM streams double-buffered in chip RAM on a pair of channels (two at once), or one frames stream on all four for modules (`modplay` computes each tick, sndd applies the queued frames on time). Channels are allocated, not mixed. `play` reads WAV, 8SVX, AIFF and raw bytes; `modplay` ProTracker modules. Details, the protocol, the timing and its limits: [d40-sound.md](d40-sound.md).

## D41. Optional memory protection on CPUs with an MMU (roadmap block 6; design note revision 2 after two independent reviews; the details delegated; built on branch mmu, 2026-11-12; off by default)
On a 68030, 68040 or 68060 whose MMU works (tested at boot: EC parts and Emu68 have none), `zenixboot MMU=ON` switches on memory protection: the MMU only protects, never translates (one shared tree, every page mapped to itself, 4 KB pages), so the single address space, spawn, shared code and ZINE stay as they are. A protected process (every program a user runs) sees only its own memory: its block, its regions, its code read only; a stray read or write anywhere else (the kernel, another program, free memory, the chips, address 0) ends it with a line naming whose memory it was. System processes (the service manager's servers, reserved ports, drivers, a display server, a mount's server) see everything, as before, and serve clients in place (D1). The kernel runs through the transparent translation registers (D37's windows, set for supervisor accesses only), never the tree. A switch to another protected program flips the two views' pages; a switch to or from a system process rewrites two or three registers. A protected server sees its client's memory while it holds the request. Off by default until tried on the 1260 (`MMU=OFF`, the left mouse button). Details, the costs, the tests: [d41-memory-protection.md](d41-memory-protection.md).

## D42. The faster IDE (roadmap block 6; built on branch ide, 2026-11-12, in FS-UAE)
The Gayle IDE driver reads a sector with 128 `move.l` (the drive's register lines are on A2-A4, so A1 selects nothing and a long access reads the data register twice), but only after a second IDENTIFY read that way agrees with the word-wise one; writes stay word by word; a wait reads the clock only after 16 looks at the status. ZenixFS reads runs of whole blocks straight into the caller's buffer (one extent lookup, one request of up to 64 KB, ending before a block cached newer than the disk) and remembers the data block in its read scratch; ext2 reads a file read in order up to a quarter of its cache ahead per request and no longer reads a block it overwrites whole. No interface changes. READ MULTIPLE left out (section 3). Figures (emulator only) and what to measure on the Apollo 1260: `docs/d42-faster-ide.md`.

## D43. RTG: the desktop on a graphics card's screen (roadmap block 6; the details delegated; built on branch rtg, 2026-11-12, in FS-UAE against a pretend card)
`zenixboot RTG` hands the front screen AmigaOS's Picasso96 set up (its memory, size, bytes a row and pixel format: the 16-, 24- and 32-bit ones, not CLUT) to Zenix in the boot's hardware table (`struct hwfb` after `board[]`, written by zenixboot after stage0); the display server finds it there and draws the desktop on the card instead of the chipset: the pens in a one-byte-a-pixel bitmap in fast RAM, drawn by the CPU (no blitter reaches a card), each pass's changed rectangles converted into the card's pixels through a table of the 16 pens, the card's memory only written; the pointer drawn by the server, following the kernel's pointer scaled from the chipset's screen; the chipset shows colour 0. Clients see the card's size and 16 pens; `DPY_MODE` answers `DPYM_RTG`. On a Pi (Emu68) the server writes the HVS's display-list register on console switches, so the one HDMI shows the text consoles (through the C790's capture) or the desktop, as VideoCore.card does. Nothing in the kernel changes (0 bytes); nothing changes without `RTG`. Details, what only the PiStorm can show, the questions: [d43-rtg.md](d43-rtg.md).

## D44. CD-ROMs: an ATAPI CD drive on the IDE port, ISO 9660 discs (roadmap block 6; the details delegated; built on branch cd, 2026-11-12, in FS-UAE)
Three parts, as every disk has: the kernel finds an ATAPI CD drive on the A600/A1200's Gayle port at boot (the slave beside an ATA master, or the master when no ATA drive answered) and moves one packet command's data for the disk server (`SYS_atapi` 98, `struct uatapi`: data in only, bytes `[skip, skip + len)` of the transfer kept, the phases told by the interrupt reason register); diskd gives the packets their meaning and serves the drive as `cd0` in 512-byte sectors over its 2048-byte blocks, read-only (`DISKCAP_RO`, writes `EROFS`), a changed or removed disc raising a generation that makes a mount made before it answer `ESTALE`; `/sbin/iso9660d`, a file service per mount like pfs3d, reads ISO 9660 with Rock Ridge (names, modes without set-id, symbolic links, moved deep trees) when the root says so, else Joliet's names, else the plain ones; inode numbers are half a record's byte position (no table of open files, nothing stale after a restart); volumes up to 4 GB; multi-extent and interleaved files listed but not read; every extent, length and record range-checked. `mount` finds the type by CD001 at block 16. The kernel grew 2.4 KB; the mount tests' RAM disk 692 -> 689 KB for it. Details, the FS-UAE config (`cdrom_drive_0_controller = ide1`), what only a real drive can show: [d44-cd.md](d44-cd.md).

## D45. The boot's memory given back (block 6 audit 2.1-2.2; decided by the main session 2026-11-12; design note revision 2 after two independent reviews; built on branch bootmem, 2026-11-12, in FS-UAE)
The kernel's boot-only code (`KINIT`: `kmain` and the set-up it alone calls), `stage0` and the built-in `boot` program are linked into a span at the end of the kernel's loaded image; the CD drive's packet mover into a second. `mem_init` reserves each as a pool range holding one block in use (before the MMU's tables are built); when `kmain` hands over to the idle loop, the spans are filled with `ILLEGAL`, the caches flushed, and freed (the CD's only when no drive answered): about 26 KB more free RAM on every machine, chip RAM on a stock A1200. `tools/check-init.py` fails any kernel build in which code or data outside a span refers into it. No interface changes. The MMU's runtime code (4.3 KB, dead without `MMU=ON`) is left for a follow-up with the gate pattern written down. Details: [d45-boot-memory.md](d45-boot-memory.md).

## D46. USB (roadmap block 6; design note revision 2 after two independent reviews and two code reviews, the scope from the partner's answers, 2026-11-12; steps U1 and U3 built on branch usb against a model of the controller and a stick, not yet run on real hardware)
Only the PiStorm has USB: the CM4's one USB 2.0 port (one socket), driven by the BCM2711's built-in xHCI (`otg_mode=1`; what AmigaOS's Poseidon drives as `xhci.device` unit 0); a CM4 has no VL805 and its PCIe lane holds the NVMe; the 1260 has no USB card. The partner uses USB for sticks, so the first version serves **one USB stick plugged straight in, read-only**: `/sbin/usbd`, the D11 service of port `usb` (kept running by `lsusb --hold` in `/etc/daemons`), owns the controller through D19's claim, DMA memory and a quiesce program with a bounded wait step (`DRVS_WAIT`), the kernel allowing it the xHCI's claim alone; polled; an xHCI back end in plain C behind the `xc_*` calls; bulk-only transport and SCSI (READ CAPACITY (10) and (16), READ (10)); the stick in diskd as `usb0`, `usb0p1`, ... (`DISK_USBHELLO` once, then posts both ways: diskd waits for usbd at most 5 s, usbd never waits for diskd; the data through usbd's bounce buffer, copied by diskd). stage0 records the controller and halts one AmigaOS left running. Keyboards, mice and hubs dropped for now (revision 1's design in git). kernel.bin +3.4 KB (diskd's 2.3 KB, stage0's 0.7 KB boot-only), accepted. Next: exFAT, its own server (section 10), then writes. Details, sources, the cache and byte-order rules, the tests, the first real run, risks, the partner's answers, the reviews: [d46-usb.md](d46-usb.md).

## D47. exFAT, read-only (roadmap block 6, the USB item's next step; the details delegated; built on branch exfat, 2026-11-13, on the PC and in FS-UAE)
`/sbin/exfatd`, a file service per mount as `fatd`, `pfs3d` and `iso9660d`, reads exFAT from Microsoft's published specification (2019), clean: no other implementation read. Read-only (`-o rw` refused, every change `EROFS`). Every entry set's checksum and every number on the disk checked before use; the boot region's checksum, the backup region when the main one is bad. Nothing loaded whole: 16 sectors of FAT and directories cached, a window of 16 for small reads, whole sectors straight to the caller, a run of contiguous clusters one request; the bitmap read once, for `df`. Names as fatd's (UTF-8 out, UTF-16 on the disk, the same folding, now one copy in `user/utf16name.h`); over 63 bytes listed as start, `~` and number. Programs' 32-bit sizes: a file of 4 GB or more listed with the size capped, `open` `EFBIG` (as pfs3d and NFS). Inode numbers and open slots as fatd's. `mount` learns the type `exfat` from the boot sector's `EXFAT   ` (MBR type 07 is shared with NTFS) and accepts it on a read-only device. About 53 KB of RAM a mount. Details: [d47-exfat.md](d47-exfat.md).

## D48. The file view, `files` (roadmap block 5, "the file view"; the details delegated; built on branch files, 2026-11-13, in FS-UAE)
A client of the display protocol (D38 section 4), like `term` and `deskprefs`: one window that lists a folder in the 8 x 8 font (names, sizes as `ls -h`, dates as `ls -l`; folders first, by name without case), started by zdesk's Files icon and menu line (they started `term -e ahiru`). Ahiru's model and keys (vi keys, prefix keys with their hints, marks, `yy` `dd` `pp` `po`, `dD`, `cw` `a` `A` `I`, `M`, `/` `n` `N`, `o` sorts, `zh`), configurable in `~/.config/files/keys` in Ahiru.keys's format (written with the built-in keys when missing, Ahiru's way); the mouse: a click picks a row, a double click opens it, Shift and a click marks; the menus in the bar. One column at 640 pixels (80 columns first); a window of 72 columns or more shows the parent folder too (Miller columns collapsed to two); no preview column. Lean: a folder is its readdir records (names and types) sorted, only the rows on view `lstat`-ed; a cursor move redraws two rows, a scroll is one copy. Opening runs the program in a terminal window (`term -e`): text in `less`, `E` the editor, a program after a question, anything else what the user types. Copy, move and delete are `cp -r`, `mv`, `rm -r` themselves (no second copy of their code), after asking when a name exists or anything is deleted; a paste into the source's own folder or a folder into itself refused. No new number or message. Details, the partner's questions (programs in a terminal or bare; Files as `files` or Ahiru; hidden names), the tests: [d48-fileview.md](d48-fileview.md).

## D49. Directories in batches, and the device in `stat` (asked by the main session 2026-11-13 after D48 measured a 500-file folder; design note revision 2 after two independent reviews; built on branch readdir, 2026-11-13, in FS-UAE and on the PC)
Measured first: of `ls`'s 11 ms a name on a 500-file ext2 folder, 1 ms was the `readdir` record and 9-10 ms the `lstat` that followed (the server walks the directory to each name; the file view paid it too, revision-0 ext2 giving no types). So one request returns many records and each carries the name's lstat (NFS's READDIRPLUS): `SYS_readdirs` 99 and `FS_READDIRS` 27, records `struct udirx` (`kernel/udirx.h`: size, type, flags, ino, next, a 28-byte `ustat`, the name; readers step by size), `UDX_STAT` as `fstatat` on the open directory (the search right on it checked; none on `.`, `..`, a cut name; a mount point's taken out by libz). Every local server answers it from its own `FS_LSTAT` code (fsd reads the seven numbers in place, `ext2_istat`, inode tables read ahead); nfscd names only; an older kernel or server answers -ENOSYS and libz's `zdir` reads one record a call. libz's `dir_list_some`, `dir_list_stat`, the C library's `readdir` (now with `d_type`), `ls`, `files` and `sh`'s globbing read through `zdir`. `stat` with 32 bytes gives the device (the mount's instance), so `cp a a` and `mv` onto the file itself say "are the same file" (GNU's text) instead of emptying it. Found and fixed on the way: zfsd's record types (the library's 4/8/10, not 1/2/7), `sh`'s glob dropping dangling links. Emulator figures: `ls` 5.66 -> 1.05 s, `ls -l` 6.83 -> 2.19 s, the file view on that folder 6.6 -> 1.7 s; kernel.bin +1,416 bytes (the kernel's own +360, the built-in fsd +1,044). Details, the reviews, the tests: [d49-readdir.md](d49-readdir.md).

## D50. The picture viewer, `show` (roadmap block 5, "viewers"; the details delegated; built on branch view, 2026-11-13, in FS-UAE and on the PC)
A client of the display protocol like `files`: one window showing an IFF ILBM (1-8 planes, EHB, HAM6, HAM8, 24 planes; lores, hires, super-hires, lace from a cleaned CAMG or guessed from the size; ByteRun1 as one stream across rows; a mask plane; a 4-bit CMAP; one nested FORM) or a GIF (87a/89a, the first image, interlaced, local tables), at its shape on the glass (lores twice as wide, lace and square pixels half as tall on 256 lines), shrunk to fit or at its real size (moved with the arrows). In the desktop's 16 pens (8 on OCS/ECS): an ordered 4 x 4 pattern as strong as each colour is far from its pen (a palette picture's pixel is one table look-up; a colour that is a pen stays exact), Floyd-Steinberg or the nearest pen (`d`). Next and previous in the folder (files that are not pictures, devices and FIFOs passed over), a click for the next, the bar's menu; keys in `~/.config/show/keys` through files' parser, moved to `display/lib/keys.c` and shared. `files` opens a picture with `/bin/show` (bare, no terminal). GIF was the one more format (LZW about 1 KB); PNG would cost about 7 KB of inflate in this program, so PNG and JPEG are left for decoders loaded on demand. A screen of its own with the picture's palette and mode is not in protocol version 1: d50 section 4 says what a `DPY_SCREEN` would take, a question for the partner. No new number or message. Details: [d50-viewers.md](d50-viewers.md).

**As built (2026-11-13).** Sizes: `show` 36,444 bytes on the disk (`pic.c` the decoders and renderer 9.0 KB, `show.c` 8.6 KB with its texts, the shared keys parser 3.0 KB, libz and libdisplay linked as every client); `files` 41,736 -> 42,608 bytes (+872: the parser as a module, the open rule). Memory: the program about 54 KB with its tables (the pens' 4 KB, the pattern's 4 KB), the decoded picture (at most 4 MB) and a canvas in pens (a 616 x 208 window 64 KB, a real size at most 1 MB; past it shown smaller, "(smaller)" in the title); chip RAM only for one strip (16 KB, at most 640 x 256) while it is drawn. **The demo floppies:** the HD one carries `show` (322 KB free after it, 343 before); the DD one does not (28 KB free, `files` +872 bytes; `show` would cost 20 KB packed and 40 KB of the RAM disk, leaving 8 KB and no room for pictures), and there `files` asks "Open with:" as before. **Speed** (FS-UAE's stock A1200 with 4 MB fast, emulator figures, not a real machine's; `show -v`): the first build took 6.2 s to render a 616 x 123 window with Floyd-Steinberg: gcc, tuned for the 68060, turned shifts and adds into `muls.l` (28+ cycles on a 68020). Now (`-mtune=68020` for pic.c, constant divisions kept out of line so no 64-bit multiply appears, eight pens shifted into one long for the planes, the pattern's table): a 16-colour lores picture into 616 x 123, 0.73 s with the pattern; HAM6 into 616 x 61 0.74 s; an 800 x 300 real-size hires picture 1.7 s with the pattern or the nearest pen, 12.2 s with Floyd-Steinberg (51 us a pixel), so the pattern is the default and Floyd-Steinberg the choice. The real A1200's figures are for its first run. **Tests:** `tools/show-host-test.sh` (102 pictures pixel by pixel, 26 damaged files, the renderer's three ways, 20,000 mutations each drawn shrunk and enlarged under AddressSanitizer: pass); `tools/show-test.sh` (FS-UAE, STOCK: pass, the screenshots' blocks pixel for pixel, also when opened from `files`). **The review** (one independent, opus) found: a decoded picture leaked (and lost) when a render ran out of memory; strips wider than 640 refused by displayd (an RTG window drew nothing); next and previous opened FIFOs and devices (could hang); the right edge of a panned picture out of reach (an alignment to 16); the docs naming Floyd-Steinberg the default; every picture rendered twice at start; and smaller ones (limits said only with `-v`, `gg` losing the place, a failed palette read giving black pens, a resize after a mode change rendering twice). All fixed; the HAM8 control planes (the file's top two, as ILBM readers take them) are to be checked against a HAM8 file from a real Amiga paint program (d50 section 3).

## D51. TrueType fonts, drawn once a size into Amiga bitmap fonts (roadmap block 5; the direction the partner's, the details delegated; built on branch ttf, 2026-11-13, on the PC and in FS-UAE)
A tool, `ttf2font FONT.ttf SIZE...`, draws the characters 32-255 of a .ttf at SIZE lines into an AmigaOS bitmap font (`NAME/SIZE` and `NAME.font`, the files displayd's reader and AmigaOS's diskfont read), with a stamp in a `HUNK_DEBUG` block naming the .ttf's size and time and the renderer's version. displayd, finding `font NAME SIZE` in no Amiga font but in `/usr/share/fonts/truetype/NAME.ttf`, runs it once into the system-wide cache `/var/cache/fonts` (again when the stamp no longer matches) and then reads the file as any Amiga font: nothing new downstream. Our own rasteriser (display/ttf/ttf.c, 8.4 KB of 68k code; stb_truetype measured 22.5 KB with the soft-float it needs): integer only, no FPU, nonzero rule on pixel centres with dropout control, no hinting; black and white (decided: one mask, one pen, the theme's 16 pens; grey a question); a glyph drawn 1.875 times as wide as its height's scale, the hires pixel's shape. deskprefs lists the .ttf fonts in eight sizes; `ttf2font -c` makes the console's ZCF1 Unicode font (D28 7). Go Regular shipped, cut to 34 KB (BSD-3). No new number or message; the cache file's stamp is frozen. Details: [d51-truetype.md](d51-truetype.md).

## D52. Zenix builds itself, and fun: the plan for roadmap block 7 (proposed 2026-11-13; the first item, Lua, built on branch block7, on the PC and in FS-UAE)
The order of the block: Lua, `make` (pdpmake, public domain), our own linker `zld` (ELF objects and archives straight to ZEXE/ZEX2, checked byte for byte against `ld` + `mkzexe.py`), the assembler, the C compiler, the debugger, the fun, then the full audit and 1.0 with the owner's yes. One ABI: whatever compiles on Zenix follows gcc's m68k-elf ABI, so its objects link with the C library the PC builds; a `devel` package carries the headers and `libc.a` (913 KB without its debug sections). **For the partner:** the assembler, our own `zas` (recommended) or vasm (not GPL-compatible: a separate package only with its author's written consent); the C compiler, tcc (LGPL-2.1) with a 68k back end of our own (recommended: free, small, fast to compile; the largest item) or vbcc (same terms as vasm); a live debugger is a kernel change (stopping a process, the trace bit, breakpoints routed to the debugger, a private copy of shared code), so only a crash-time `zdb` is planned without the partner's yes; which games and demos. **Lua** (built): 5.4.9 pinned unchanged in `ports/lua/upstream`, Zenix's settings by compiler flags and upstream's user-header hook; a plain ZEXE (libgcc's soft float is not pure, so not ZEX2); `LUAI_MAXCCALLS` 100, the pattern matcher's 100 and 256-byte buffers against a fixed 128 KB stack, its use measured exactly in FS-UAE (a painted stack) for 18 kinds of C recursion a script can make from source (binary chunks are not checked by Lua: the man page says so). Found and fixed on the way: the allocator's `realloc` swallowing the whole free block after a grown block (13 MB held for a 5 KB script), picolibc's `fflush(NULL)` crashing. Details: [d52-self-hosting.md](d52-self-hosting.md).

## D53. Scripts started by their `#!` line (the partner's yes 2026-11-13, decisions.md "Block 7's choices" item 4; built on branch shebang, on the PC and in FS-UAE)
In the kernel's spawn, as on Unix (Linux's `binfmt_script`), beside the AmigaOS-program redirection to ZINE (D29 12.1): the one place the shell, make's recipes, `xargs`, `env`, `find -exec`, `posix_spawn`, init's boot lines and the service manager all pass through, one copy of the code (libz would put a copy in every program that starts others and miss the rest; the shell alone would miss every C program). A file with the x bit starting `#!` runs as `INTERPRETER [ARG] PATH ARGS...`: the line at most 127 bytes (longer: -ENOEXEC, never cut), one argument with its inner spaces, the interpreter a Zenix program (one level), the list held to spawn's limits, set-user-id honoured neither on the script nor on its interpreter (the hole of a set-id interpreter opening the path again, and `#!/bin/sudo` looping), the process named after the script, a script of any size. The shell names the interpreter and the reason when a script cannot start; a file with neither header nor `#!` stays refused (`sh FILE` runs it). `xargs` leaves room for a script's three words. Two design reviews before, one code review after. Kernel: proc +707 bytes (kernel.bin 198,780 -> 199,492; the mount tests' RAM disk 684 -> 683 KB). Details: [d53-shebang.md](d53-shebang.md).

## D54. The live debugger, `zdb` (the partner's yes 2026-11-13, decisions.md "Block 7's choices" item 2; design note revision 2 after two independent reviews; built on branch debugger, 2026-11-13, in FS-UAE)
One system call, `debug(op, pid, arg)` (100), ptrace-like: attach (your own processes, root any; never pid 0/1, the kernel's servers or a set-user-id program running as someone else), stop, wait, continue, step (T1; not on Emu68, which never traces), registers, memory, breakpoints the kernel keeps (`ILLEGAL`, 16 a process, written back when the debugger detaches or dies, the instruction cache flushed), and a program started under the debugger stopped at its entry with a private copy of shared code (ZEX2) and no set-user-id. A debugger's stop is `P_STOP` that only the debugger ends; `SIGINT` to a traced process stops it; a fault stops it for a look, then it ends as before (139). The debugger dying detaches what it attached to and kills what it started. Every crash adds two log lines (registers, the frame chain) that `zdb -c` reads after the fact. Only your own processes, never a set-user-id one or one that changed its id (`nodebug`), never the kernel's servers or a system process. No cost on the hot paths: byte tests in the exception path, the stop branch, signals, spawn and exit only. Kernel +4.9 KB (the mount tests' RAM disk 683 -> 678 KB). Details: [d54-debugger.md](d54-debugger.md).

## D55. Screens of their own (`DPY_SCREEN`), and the Zenix intro (block 7's "fun", decisions.md "Block 7's choices" item 3; design note revision 2 after two independent reviews; built on branch intro, 2026-11-14, in FS-UAE)
A program on the desktop may open a screen of its own, as AmigaOS programs open custom screens: its mode (lores or hires, interlaced, 1-8 planes, HAM6, HAM8, EHB; OCS/ECS 6 lores or 4 hires planes), its palette (256 of 24 bits on AGA) and its planes in its own chip RAM, which it draws into directly. Its Copper list is given in Zenix's own instructions (wait, colour, plane, modulos, scroll) and displayd checks and translates it into words in its own chip RAM: every fetch worked out to stay inside the planes (mirrors by negative modulos included, interlaced fields each), pointers and modulos only at a line's start, every line's moves within the Copper's time, no other register reachable. Shown through a trampoline the desktop's one-word jump names; colours change in place, pointers and a list's shape by a flip after the settle. The kernel adds two `SYS_console` ops: `CON_VBLANK` (12: the vertical blank's count and a wait for it, woken by kwork) and `CON_HOLD` (13: displayd holds the planes while shown; their `memfree` is -EBUSY and a dead owner's are handed to displayd with `GEV_GONE`, freed after they leave the glass). Left Amiga+N flips between the desktop and the screen. Refused on a graphics card (D43). `show` (by default, as `show -s`; `show -w` the window) opens a picture on its own screen with its real palette and mode; `zintro` is the intro: a logo, copper bars, stars, a turning wireframe and a scroller, a module of our own through sndd, all made by tools/mkintro.py. Found and fixed on the way (both reviews): `dyn_flip` read the beam before writing the jump word, so a preempted displayd could rewrite a list the Copper still ran. Details: [d55-screens.md](d55-screens.md).

**As built (2026-11-14).** Sizes: kernel.bin 205,136 -> 205,992 bytes (+856: gcon +600 for the holds and the ring's kept slots, gfx +160, the dispatch; ksize-budget gcon 3,600 -> 4,250, gfx 8,900 -> 9,050, TOTAL 205,200 -> 206,000), the mount tests' floppy 1,712 -> 856 bytes left (its 800-byte margin kept); displayd 71,772 -> 81,540 bytes on the disk (screen.c 8.4 KB of code; a screen's record and lists in one chip region of its own while it is open), the demo floppies' displayd built without it (`-DNO_SCREEN`, 66,472 -> 66,620); show 36,512 -> 39,244; dpytest 13,616 -> 18,324; zintro 45,912 (its logo, font, text and module packed with ByteRun1 about 21 KB); the DD demo floppy 22,528 -> 21,504 bytes free, the HD one 320,000 -> 317,440. Chip RAM: zintro's screen (its 311 instructions, the record, both lists) about 25 KB, its two pictures 80 KB. **Fetch** (the open question of `demoscene/aga-specifics.md` 4.3): 320-pixel lores at FMODE 3 is DDF `$38`-`$B0`, measured pixel-exact by the screen test (a pattern that shows every byte a line fetched). **Speed** (FS-UAE's stock A1200 with 4 MB fast, emulator figures, not a real machine's): zintro draws a frame in about 16 ms (stars 2.2, the shape 7.5, the scroller 5.3, colours 0.7) and its two requests take about 10 ms (167 colours in place 8.2, the flip 2.4), so it runs at 25 frames a second (each picture up for two frames; its motion keeps its speed by the frames counted), not 50: the stock 68020 does about a million simple instructions a second here (20,000 loop turns in 30 ms). `CON_VBLANK` counted 100 frames in 1,419,787 E-clock ticks (50 a second). **Tests:** `tools/screen-test.sh` (24 checks), `tools/intro-test.sh`, `tools/show-test.sh` (HAM6 and HAM8 on their own screens), and the desktop's tests (dpy, zdesk, files, show) pass. **The reviews:** two independent design reviews (section 17 of the note: the flip's race, freed planes on show, the wake-up in process context, the cost of a list a frame, mid-line changes, interlace's modulo, all taken), one code review after building (section 18: a 32-bit wrap in the fetch check and late pointer writes, both able to show memory outside the planes, and smaller ones; all fixed, each with a test).

## D56. The tracker music player: `modplay` grown (the partner's yes 2026-11-17, decisions.md "New approvals" item 1; the details delegated; built on branch tracker, 2026-11-17, on the PC and in FS-UAE)
`modplay` plays MOD (ProTracker's kin and FastTracker's 6-32 channels), MED/OctaMED (MMD0-MMD3, sample instruments), Scream Tracker 3 and FastTracker 2 modules, from files or folders as a list, through D40's server unchanged. A module Paula can play as it is (at most 4 channels with Amiga periods: MODs, 4-track MED) goes to Paula by frames as before; every other one is mixed by the CPU (8-bit, no interpolation, a volume table, left/right/middle sums) into one stereo PCM stream on channels 0 and 1, 16 kHz by default (`-r`). One face for every replayer (`user/trk.h`: a tick's registers per channel), samples made 8-bit at load, ping-pong loops laid out once. On a terminal a screen of its own (song, position, meters, the pattern heard, the instruments, the list, the keys), every key in `~/.config/modplay/keys` through the desktop programs' shared parser; `-s` and `-q` keep the old ways. Written from the formats' documents (MMD0/1 by Teijo Kinnunen, the MED and OctaMED SoundStudio manuals, ST3's TECH.DOC, the "Complete" XM specification), no player's source read. Impulse Tracker not in this round. No kernel change, no new number. Details, sizes and the mixer's emulator measurements: [d56-tracker-player.md](d56-tracker-player.md).
## D57. ZINE's guest mode: AmigaOS window programs on the Zenix desktop (a design study for the partner's wish of 2026-11-17, "Kinda how wine does it in Linux"; approved 2026-11-17, to build after 1.0)
An AmigaOS GUI program (Intuition, GadTools, later MUI) runs inside an ordinary Zenix process, the guest, next to other programs, the way Wine runs Windows programs on Linux; take-over mode (D29) stays for demos. The guest holds its own ExecBase and library bases in its own memory; its library calls jump into ZINE's library code built a second time for user mode, without the hardware parts. Exec's tasks are coroutines inside the process; idle guests wait in `evwait`. A window is a desktop window whose inside is a planar canvas in the guest's chip RAM, drawn by the CPU and shown by a new displayd request (`DPY_CANVAS`), so every window behaves as smart refresh; Amiga frames by default; menus in zdesk's bar; custom screens as D55 screens. Files are real Zenix files through a per-user prefix (`~/.zine/default`, assigns, `zine install` of an .lha with its libraries, fonts and catalogs). Disk libraries (reqtools, MUI 3.8 the user owns, OpenMUI as the offered MUI) run as native 68k code on our libraries; built-in or native chosen per library, as Wine's overrides. Kernel changes proposed: the guest's longword at address 4 on its switch (K1), faults reflected to the guest (K2), FPU state for guests (K3). Order: groundwork and a hello window, GadTools, a real Aminet tool (VClock, public domain), installing and ASL, MUI, custom screens; about 160-240 helper hours. Details and the questions for the partner: [d57-zine-guest.md](d57-zine-guest.md).

## D58. The IRC client `irc` (the partner's yes 2026-11-17, decisions.md "New approvals" item 2; the details delegated; built on branch irc, 2026-11-17, on the PC and in FS-UAE)
A terminal IRC client of our own (sic, ii and kirc are MIT but give no screen, windows or keys; catgirl is GPL-3; irssi far too big): a title row, the window's lines, a status row and the input row, 80 columns first; windows for the status, each channel and each private conversation, switched with F1-F10, Ctrl-N/Ctrl-P, Ctrl-X (the next with news); scrollback, nick colours and highlights, nick completion, history. The protocol and the windows are a system-call-free core (`net/irc/`, also built on the PC); every byte from the server is made clean UTF-8 (ISO-8859-1 read where it is not UTF-8, controls, C1 and mIRC codes dropped) before it is parsed, so no escape reaches the console. SASL PLAIN, CTCP ACTION/VERSION/PING, paced sending, a reconnect with the channels joined again. TLS is a helper, `/lib/tls/tlspipe` (the socket on its descriptor 2, the text on two pipes), so the crypto library is loaded only for a TLS server: `irc` is 53 KB, the helper 107 KB; wget and the helper share `user/tlstrust.c` (the clock's rules, the seed, the anchors). Settings in `~/.config/irc/config`, keys in `~/.config/irc/keys` (the shared parser). No kernel change, no new number. Details, sizes and tests: [d58-irc.md](d58-irc.md).
