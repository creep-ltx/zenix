# Zenix design (proposed, 2026-10-22)

## Goal
A Unix-like system (processes, `/` filesystem, pipes, signals, a shell) for the Amiga that is fast and uses
the Amiga hardware: blitter, copper, Paula, chip/fast RAM. Not Linux, not AmigaOS-based.

## Decisions
- **CPU: 68020 or better, 32-bit flat.** No 68000. No MMU required (the target machines have none).
  If an MMU is present it may be used later for optional protection.
- **No MMU, so no `fork`.** `vfork` and `posix_spawn` only. Programs are relocatable and the loader places
  them anywhere in RAM, so there is no fixed-address fragmentation.
- **Crashes are contained, not prevented.** The kernel catches bus error, address error, illegal
  instruction and divide by zero, kills only that process and frees everything it owned (per-process memory
  tracking). Guard words around stacks and allocations; `CHK2` stack checks on 68020+. A wild write through
  a bad pointer can still corrupt another process. This is the cost of having no MMU.
- **Message passing at the core.** Pipes, sockets and signals are built on fast message ports.
- **Chip RAM and fast RAM are separate pools** in the allocator.
- **Boot:** bootblock takes over the machine from Kickstart and never returns to AmigaOS.
- **Toolchain:** m68k-elf-gcc (`~/opt/m68k-elf`), newlib later, FS-UAE for tests, then the test Amigas.
- **Graphics (later):** a display server on the blitter and copper; workspaces with an i3-style bar
  (a workspace is listed only if it is current or has a program running); one file view (Ahiru-style).

## Reused from zenix-old
Bootblock, serial console, kmalloc, Gayle IDE, RDB partitions, FAT16, ELF loader, signals, keyboard,
syscall idea, build and FS-UAE setup. **Rewritten:** process switching (shared memory map), loader
(relocatable), exception handling. **Dropped:** 68030 MMU, per-process address spaces, mandatory FPU.

## Milestones
0. Boots in FS-UAE as an A1200 (68EC020, no MMU) and prints to the serial port.
1. Memory manager, task switcher, system calls, first program from disk.
2. Filesystem, tty, shell: the first "it is a Unix" moment. Tested on a real A1200.
3. Graphics server, bar, terminal. 4. Network, audio, the rest.

Open questions: licence (zenix-old was MIT; copying Fuzix/Linux code would make it GPLv2), whether to
optionally use an MMU, and how much of the old user programs port unchanged.

Written 2026-10-22: proposed. Only the "reused" list was read from zenix-old's source tree, nothing built.
