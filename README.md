# Zenix

A small, fast, POSIX-like operating system for real Amigas with a 68020 or better and **no MMU needed**.
It runs on the Amiga itself: no Raspberry Pi, no emulation layer, no AmigaOS underneath.

Status (2026-10-22, all *verified in FS-UAE* as a stock A1200 - 68EC020, no MMU - not yet on a real Amiga):
- **M0** boots, serial banner, memory map, timer interrupt.
- **M1** relocating kernel (runs from fast RAM when there is any), chip/fast memory manager with owner tracking,
  user-mode processes with `TRAP #0` system calls, preemptive scheduler, crash containment (an illegal
  instruction kills only the offender), user pointers validated.
- **M2** ext2 root filesystem (now writable, see M4) found through the RDB, tty line discipline (^C ^D ^U),
  pipes, argv, a shell (`|`, `<`, `&`, quotes, cd) and `ls cat echo pwd ps free wc grep uname`.
- **M2b (write support)** `>` `>>`, `mkdir rmdir rm cp mv touch`: block and inode allocation, files with
  direct/indirect/double-indirect blocks, directories that grow, rename. Write-through cache. Verified:
  after a scripted session (`tools/tests/write-test.txt`) the host's `e2fsck -fn` reports a clean disk and a 15.5 KB
  file written by Zenix has the same SHA-256 as the host concatenation.
- **M3** 640x256 hires planar display, Copper-split bar + 31 text rows, scrolling by Copper pointer with a
  blitter copy at the wrap, nine workspaces (own text grid and tty each; the bar lists the current one and
  the occupied ones, like i3), Amiga keyboard, ROM Topaz 8 font. Amiga+1..9 switch, Amiga+Return opens a
  shell on an empty workspace. (Over serial: 0x1d then 1-9 / Return, for tests.)

Not done: unlinking a file that is still open (frees at once), moving directories between parents, triple-indirect files, a graphical programs API, mouse, sound, network, real
hardware, 68000 (not a target), MMU use. See docs/DESIGN.md.

Images: `build/zenix.adf` (the standard floppy: bootblock + kernel only; the root filesystem is on the IDE disk,
`build/zenix.hdf` or `zenix-rdb.hdf`) and `build/zenix-ram.adf` (development/test only: also carries a small root
filesystem that is loaded into RAM, for machines without an IDE disk, e.g. a Gotek boot).
Long-term plan and principles: docs/ROADMAP.md. Native filesystem idea: docs/zenfs.md.

Build and test: `make` (needs ~/opt/m68k-elf, mke2fs), `make test` (headless serial log),
`tools/shot.py` (screenshots in a private invisible Wayland session; build with `make DIWV=0x38` because
FS-UAE crops the top display lines), `tools/run-test.sh` with `tools/tests/shell-test.txt`.

Documentation: [docs/README.md](docs/README.md) is the index (architecture, comparisons, command manuals,
the feature list [docs/features.md](docs/features.md)). Design decisions: [docs/DESIGN.md](docs/DESIGN.md).

Licence: GPL-2.0 only (see LICENSE). Decided 2026-10-22; the earlier "MIT" was inherited from zenix-old, never chosen for this project.
