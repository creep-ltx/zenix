# Zenix

A small, fast, POSIX-like operating system for real Amigas with a 68020 or better and **no MMU needed**.
It runs on the Amiga itself: no Raspberry Pi, no emulation layer, no AmigaOS underneath.

Status (2026-10-22, all *verified in FS-UAE* as a stock A1200 - 68EC020, no MMU - not yet on a real Amiga):
- **M0** boots, serial banner, memory map, timer interrupt.
- **M1** relocating kernel (runs from fast RAM when there is any), chip/fast memory manager with owner tracking,
  user-mode processes with `TRAP #0` system calls, preemptive scheduler, crash containment (an illegal
  instruction kills only the offender), user pointers validated.
- **M2** ext2 root filesystem (read-only so far) found through the RDB, tty line discipline (^C ^D ^U),
  pipes, argv, a shell (`|`, `<`, `&`, quotes, cd) and `ls cat echo pwd ps free wc grep uname`.
- **M3** 640x256 hires planar display, Copper-split bar + 31 text rows, scrolling by Copper pointer with a
  blitter copy at the wrap, nine workspaces (own text grid and tty each; the bar lists the current one and
  the occupied ones, like i3), Amiga keyboard, ROM Topaz 8 font. Amiga+1..9 switch, Amiga+Return opens a
  shell on an empty workspace. (Over serial: 0x1d then 1-9 / Return, for tests.)

Not done: writing to the filesystem (`>`, mkdir, rm), a graphical programs API, mouse, sound, network, real
hardware, 68000 (not a target), MMU use. See DESIGN.md.

Build and test: `make` (needs ~/opt/m68k-elf, mke2fs), `make test` (headless serial log),
`tools/shot.py` (screenshots in a private invisible Wayland session; build with `make DIWV=0x38` because
FS-UAE crops the top display lines), `tools/run-test.sh` with `tools/shell-test.txt`.

Start with [DESIGN.md](DESIGN.md).
