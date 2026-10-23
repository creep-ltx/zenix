# Zenix design decisions

The numbered decisions, D1-D9. They were proposed on 2026-10-22; all are decided now, and most are built ("Where
the decisions stand" below). Last checked against the code: 2026-10-23.

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

## D11. Service manager (outline, 2026-10-24. Owner 2026-10-24: may be built after D10 once its design here is complete and fully thought through - a full design note, reviewed, before any code)
`boot` grows into one service manager (Exec "load on first use" + Minix 3 restart + launchd, as one mechanism): it starts
the servers listed in a small configuration file, restarts a server that dies (clients of a dead server already get an
error and retry), and later starts servers on first use. It replaces "start everything at boot" and the hard-wired
server list in kmain. Port rights (Darwin) are a separate, later decision.

## Where the decisions stand (2026-10-23)
D1 messages: ports + synchronous zero-copy calls done; the filesystems (ext2 `fsd`, ZenixFS `zfsd`/`mount.zenixfs`) and the disk (`diskd`, the only one with raw-disk system calls) are user-mode servers, built into the kernel image. Drivers, tty, pipes and the display are still in the kernel. A hung server blocks its clients (a client in a call is only freed by the reply or by the server dying).
D2 user mode + TRAP: done; the TRAP cost is **not measured** (emulator timing is not CPU timing).
D3 relocating loader, no fork: done (ZEXE format, tools/mkzexe.py); shared libraries not started.
D4 planar display server: not started; the kernel draws ten text consoles through Copper + blitter; a client API is not designed.
D5 ext2: rev 0/1, 128+ byte inodes, read and write (write-through, e2fsck-clean) done; RDB and MBR partitions done; no journal. ZenixFS built and verified in FS-UAE, also as the root.
D6 scheduler: preemptive round robin on a 100 Hz tick; no priorities yet.
D7 boot/test: FS-UAE done (the matrix, `make check`); the owner's A1200 not yet.
D8 multi-user: built (architecture.md section 14).
D9 licence: decided.

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
