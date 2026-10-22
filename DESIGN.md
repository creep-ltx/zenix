# Zenix design (proposed, 2026-10-22)

## Goal
A Unix-like system (processes, `/` filesystem, pipes, signals, a shell) for the Amiga that is fast and uses
the Amiga hardware: blitter, copper, Paula, chip/fast RAM. Not Linux, not AmigaOS-based.

## Decisions (all proposed until tested; D = decision)
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
    Kernel lives in a raw area or boot partition found through the RDB. Written from the public spec,
    not from Linux's GPL code. FAT is only an optional module. Buffer cache always.
D6. **Scheduler:** preemptive, priority-based, round-robin within a priority, event-driven wake-ups;
    interactive tasks (terminal, display server) high priority; lazy FPU save only if an FPU exists.
D7. **Boot/test:** FS-UAE (68EC020, no MMU) first, then the test Amigas, then the shared A1200 (Emu68
    behaviour unverified; read demoscene/friendly-takeover.md first).

## Basics
- CPU: 68020 or better, 32-bit flat, no MMU required; an MMU may be used later for optional protection.
- Crashes are contained, not prevented: the kernel catches bus error, address error, illegal instruction
  and divide by zero, kills only that process and frees what it owned; guard words around stacks and
  allocations. A wild write can still corrupt another process; that is the cost of having no MMU.
- Toolchain: m68k-elf-gcc (`~/opt/m68k-elf`), newlib later.
- Desktop later: workspaces with an i3-style bar (listed only if current or occupied), one Ahiru-style
  file view.

## Reused from zenix-old
Bootblock, serial console, kmalloc, Gayle IDE, RDB partitions, FAT16 (optional module only), ELF loader, signals, keyboard,
syscall idea, build and FS-UAE setup. **Rewritten:** process switching (shared memory map), loader
(relocatable), exception handling. **Dropped:** 68030 MMU, per-process address spaces, mandatory FPU.

## D8. Multi-user (decided 2026-10-22, to be built right after the terminal command batch)
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
  `whoami`, `chown`, `chgrp`, `umask`, `passwd`; a home directory per user; each workspace may hold a different login.
- The shell prompt shows `$` for a normal user and `#` for root (it already does, from $USER).

## Where the decisions stand (2026-10-22)
D1 messages: ports + synchronous zero-copy calls done; the filesystem is a server (fsd, built into the kernel image, user mode, raw-disk syscalls only for it). tty, pipes and the display are still in the kernel. A hung server blocks its clients (a client in a call is only freed by the reply or by the server dying).
D2 user mode + TRAP: done; the TRAP cost is **not measured** (emulator timing is not CPU timing).
D3 relocating loader, no fork: done (ZEXE format, tools/mkzexe.py); shared libraries not started.
D4 planar display server: the kernel draws text through Copper + blitter; a client API is not designed.
D5 ext2: rev 0/1, 128+ byte inodes, read and write (write-through, e2fsck-clean) done; RDB partitions done; no journal.
D6 scheduler: preemptive round robin on a 100 Hz tick; no priorities yet.
D7 boot/test: FS-UAE done; test Amigas and the shared A1200 not yet.

## Planned servers
- **index server ("find")**: keeps a FuzzyFind-style catalogue of the filesystem in memory, answers search messages
  for `locate`, `fzf` and tab completion, and is told about every create/delete/rename by the filesystem server so
  it is never stale. May also cache directory listings. Decide after the benchmark (tools/bench) shows where directory
  reads really spend their time. Reuses ~/Projects/FuzzyFind's index layout and matcher.
- **display server**: the console/bar/workspaces now inside the kernel, moved behind a port with a drawing API (D4).

## Milestones (all three first ones done, see README)
0. Boots in FS-UAE as an A1200 (68EC020, no MMU) and prints to the serial port.
1. Memory manager, task switcher, system calls, first program from disk.
2. Filesystem, tty, shell: the first "it is a Unix" moment. Tested on a real A1200.
3. Graphics server, bar, terminal. 4. Network, audio, the rest.

Open questions: licence (zenix-old was MIT; copying Fuzix/Linux code would make it GPLv2), whether to
optionally use an MMU, and how much of the old user programs port unchanged.

Written 2026-10-22: proposed. Only the "reused" list was read from zenix-old's source tree, nothing built.
