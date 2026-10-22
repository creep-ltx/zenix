# Zenix: vision, principles and plan

Written 2026-10-22. This is the one place for the long-term picture; DESIGN.md holds the numbered
decisions and README.md the current state. Update all three when something changes.

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
6. **Real A1200 first.** An A1200 with an IDE board and a drive is the target. Other hardware (PiStorm SD,
   network, USB, NVMe, RTG) is added as servers later and never complicates the base.
7. **Write it down.** Findings go to the knowledge base, decisions to DESIGN.md, status to README.md.
8. **Unix experience:** standard names, standard behaviour (POSIX where it makes sense), `/` filesystem,
   text tools that pipe.
9. **AmigaOS speed:** messages pass pointers, no copies; one address space; the blitter and Copper do the
   drawing; chip and fast RAM are separate pools.

## Where we are (2026-10-22)

Done and verified in FS-UAE (not yet on real hardware): kernel with relocation, user mode, messages, memory
manager, scheduler; disk server (RDB partitions), filesystem server (ext2, read and write, e2fsck-clean), tty,
pipes, shell and 20 commands; hires display with a Copper-split bar, workspaces, Amiga keyboard; a setup matrix
of 13 emulated A1200 variants; a benchmark using the CIA's E-clock.

## The plan, in phases (reordered 2026-10-22: a terminal to live in comes before ZenFS and the desktop)

**Phase 1 - Disk tools.** `fdisk` (the RDB partition table), `mkfs.ext2`, `fsck.ext2`, `df`, `sync`; the disk
server refuses raw access to mounted devices. Tested against scratch disks in the emulator and checked with the
host's `e2fsck`.

**Phase 2 - First real hardware.** Boot from the Gotek/GoDrive with the RAM-disk test image; measure the CPU
side (benchmark); fix what real hardware shows. (The user's A1200 is a PiStorm with an SD card and no IDE disk,
so this uses `zenix-ram.adf` until an SD driver exists.)

**Phase 3 - A terminal you can live in.** Before any desktop:
- *Shell:* history, line editing, tab completion, variables and the environment, scripting (`if`, `for`,
  `while`, functions), globbing, aliases, job control, `2>` and here-documents, a profile file. Lessons from
  the csh project (best of bash, zsh and fish).
- *Terminal:* arrow, Home/End and function keys as standard escape codes, scrollback, text styles (bold,
  inverse, underline), colours decided deliberately (the display is one bitplane now), UTF-8 later.
- *C library and kernel:* full printf, stdio, `qsort`, `strtol`, time functions, `getenv`, signals, `kill`,
  process groups, `/dev` (null, zero, tty), `/proc`-like info.
- *Commands:* port CUtils (47 commands: sed, sort, diff, find, tar, gzip, ...) through a Zenix backend for its
  `cu.h` layer; write the rest (`df`, `mount`, `date`, `env`, `test`, `expr`...).
- *Editor and pager:* Sumi and Maki ported (they use tui.library; Zenix gets an equivalent that emits
  standard escape codes), man pages.
- *Network tools later,* when the network server exists.

**Phase 4 - ZenFS.** Our own filesystem (docs/zenfs.md): atomic commit, checksummed metadata, extents, indexed
directories, self-healing; needs the multi-filesystem VFS (mount table, several filesystem servers). ext2 stays
for Linux compatibility.

**Phase 5 - The desktop.** Move the display out of the kernel into a display server with a drawing API
(planar, blitter and Copper); a client library; a terminal program as the first client; the workspace bar;
Ahiru-style file view; mouse. Index server (FuzzyFind) for `locate` / `fzf`.

**Phase 6 - More hardware, as servers.** PiStorm SD, network (TCP/IP server), USB, NVMe, RTG graphics, sound
(Paula) server, floppy driver, serial.

**Phase 7 - Polish and release.** Documentation, licence decisions, public repository, release images.

## Open questions

- ZenFS name: a Western Digital project already uses "ZenFS" (RocksDB on zoned SSDs); decide if that matters.
- Licence (MIT now; code copied from Fuzix or Linux would force GPLv2).
- How much POSIX: `fork` is not possible without an MMU; `vfork`/`spawn` only. Programs that need `fork` are out.
- Journaling: not needed if ZenFS commits atomically; ext2 gets a clean flag and our fsck.
