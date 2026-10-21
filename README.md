# Zenix

A small, fast, POSIX-like operating system for real Amigas with a 68020 or better and **no MMU needed**.
It runs on the Amiga itself: no Raspberry Pi, no emulation layer, no AmigaOS underneath.

Status: restarted 2026-10-22 from `Archive/zenix-old` (the 68030+MMU version). **M0 works** (verified in
FS-UAE as a stock A1200, 68EC020, no MMU): `make test` boots the floppy and prints the banner, CPU,
memory map and a timer-interrupt check on the serial port. The old MMU code is in `old-mmu/`, the pieces
not yet ported (console, IDE, FAT, ELF, processes, signals) in `later/`; neither is built.

Start with [DESIGN.md](DESIGN.md).
