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
    GPL-2 code could be reused too; it was not needed). FAT is only an optional module (not built). Buffer cache
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
against a program written to cheat (the same trust level as AmigaOS). The docs must say so.
- A user id and group id (and supplementary groups later) on every process, inherited by children; calls getuid,
  geteuid, getgid, setuid (root, or back to the real id), setgid, umask.
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
kernel and the base group, the only content of a system bundle) is never installed live: zpkg writes its signed bundle into an update slot file on the
boot partition, zenixboot takes it from there at the restart and D15 runs as built ("Zenix (previous)" the way
back). A package's way back is its previous version in the download cache. The full design:
[d22-zpkg.md](d22-zpkg.md).
## D23. The NFS client (roadmap block 2; design note revision 2 after two reviews; nothing built)
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
## D24. The remote control, `wasabid` (roadmap block 2; design note revision 2 after two reviews; nothing built)
**Why.** Test Zenix on the real A1200 with nobody at it, and let the owner's tools (the `wasabi` command, its Python
API, its MCP server for Claude, the apps) drive Zenix as they drive AmigaOS. **How.** `/sbin/wasabid` speaks the owner's
Wasabi protocol version 1 (frames, `caps`, discovery) and offers what fits Zenix: files (regular files only; network
mounts through a killable helper), `RUN` with two slots and live output, `PS`/`KILL`, the kernel log as `DEBUG`,
`HEALTH`, `REBOOT` (to Zenix or AmigaOS, through `reboot`, guarded), the console as pixels (`GRAB`, `SUM`) and as text
(a new `TEXT` that can wait for a pattern), keys; at most 4 connections within the 16 descriptors a process has; on its
own TCP port 1236 (the A1200's AmigaOS Wasabi is pinned encrypted on 1234); plain with a key of its own on the home
network first, encryption a later step. Five small root-only kernel interfaces (`SYS_console` ops `CON_READ`,
`CON_LINE`, `CON_INPUT` named by a console's descriptor, the kernel keeping the display's layout; `SYS_klog`'s
`KLOG_TOTAL`; `/dev/kmsg`). **A D11 decision:** servers nobody calls (network listeners) are kept alive by init, the
lines of `/etc/daemons` with D11's numbers (amends D11 1 and 4.4, D19 2.3; closes D20 7.2's open point). The full
design: [d24-remote.md](d24-remote.md).
## D25. The demo floppy, network edition (roadmap block 2's end; design note revision 2 after two reviews, nothing built)
**Why.** One 880 KB floppy that boots any 68020+ Amiga with 2 MB (the stock A1200 included) into Zenix with the
network, `telnet`, `ssh` and `wget`, to hand to anyone (owner 2026-10-25; KolibriOS's packed programs the model).
**How.** Every program is packed with LZ4 in a new wrapper, `ZPAK` (the inner header plain, code and data as two
streams), and unpacked by the kernel's loader in place in the program's own memory (no RAM disk holds programs); LZ4
because on a 14 MHz 68020 reading a floppy only it starts programs faster than unpacked ones (*modelled* and
*estimated*; deflate breaks even, Shrinkler doubles the time). The light commands are linked into one pure multi-call
program (a "box": one libz on the disk instead of 36; *measured* 54 KB of shared code against 294 KB of separate
files), kept in memory after use by D13's planned rule (unused segments dropped only when memory runs short). The
floppy is one ext2 filesystem marked read-only and mounted on `/usr`, the Amiga bootblock in its boot area and the
packed kernel and a 160 KB RAM-disk root (`/etc`, `/home`, `/tmp`) after its end, both unpacked by the bootblock;
links from the root into `/usr` need **the kernel to follow symbolic links across mounts** (today a link into another
mount answers ENOENT: a limit fixed for every Zenix). A setup script asks the keyboard (`CON_KEYMAP`, root only) and
finds the network card. *Prototype*: everything fits with about 50 KB free. The owner's approval of the note approves
its features (note section 12). The full design: [d25-demo-floppy.md](d25-demo-floppy.md).
## D26. Symbolic links across mounts (audit 2026-10-26 F12; design note revision 2 after four reviews, nothing built)
**Why.** A symbolic link is followed only inside its own filesystem, so a link into another mount answers ENOENT (and
an absolute target on any mount but the root is resolved from that volume's root); Linux follows it, and D25's links
from the RAM root into `/usr` need it. **How.** When a path request's path is a kernel copy (always while more than the root is mounted, and for
`chdir` and `mount`), the kernel marks it (`UMSG_LINKS`) and a filesystem server stops at the first symbolic link it meets and answers `FS_LINKED` with the text
the path continues at, written into the request's own path buffer; the kernel normalises it, picks the mount again
and sends the request again, at most 40 links per path (Linux's), then -ELOOP. `..` at a mount point is Linux's; the
working directory's text and mount points are kept resolved. A server not asked, and an old server, follow links
inside their volume as today. The bit is set only on a kernel buffer with an absolute path; each path is resolved
once per system call (D13's checks on the resolved mount); `umount` matches the text before asking any server. The full design: [d26-links-across-mounts.md](d26-links-across-mounts.md).
## Where the decisions stand (2026-10-24)
D1 messages: ports + synchronous zero-copy calls done; the filesystems (ext2 `fsd`, ZenixFS `zfsd`) and the disk (`diskd`, the only one with raw-disk system calls) are user-mode servers; fsd and diskd are built into the kernel image, zfsd is the file `/sbin/zfsd` since D19 step 1. Drivers, tty, pipes and the display are still in the kernel. A hung server blocks its clients (a client in a call is only freed by the reply or by the server dying).
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

Open questions: whether to
optionally use an MMU, and how much of the old user programs port unchanged.

Written 2026-10-22; decisions updated and checked against the code 2026-10-23.
