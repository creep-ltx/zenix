# Limits: what is missing and what is not known

Written 2026-10-22 from README.md, DESIGN.md, ROADMAP.md and the source.

## The no-MMU limit (permanent unless an MMU mode is added)

Any program can read or write any memory and touch the hardware. Zenix **contains visible crashes** (exceptions
kill only the faulting process; its memory is freed; the kernel validates every pointer passed to a system call;
guard words and block magic catch kernel-side overruns). It does **not** isolate programs from each other. A buggy
program that writes through a valid-looking pointer can corrupt another process, a server or the kernel and
bring the system down. Users, groups and file permissions (DESIGN D8, built) give accounting and
protection against honest mistakes, not against a program written to cheat - the same trust level as AmigaOS.
The documentation must keep saying this.

A second consequence: **servers are trusted code.** A hung filesystem server blocks every client waiting on it.

## Not done yet (see README "Not done" and ROADMAP)

- Real hardware. Everything is verified in FS-UAE only. Emulator timing is not CPU timing, so **no speed claim is
  made**; the TRAP cost, message cost and cache effects are unmeasured on a real 68020+.
- Disk access on the user's own A1200 (a PiStorm): its disk is the Pi's SD card behind Emu68's drivers, which
  die when Zenix kills AmigaOS. Done in code (`kernel/emmc.c`, stage0 device-tree handover; see architecture.md
  section 3) but **never run on a real Pi**: the SD controller is modelled from the SD Host Controller and SD
  Physical Layer specifications, so clocks, the mailbox calls and card-detect are unverified. Not done: partition
  tables other than an RDB at sector 0 (the Emu68 boot card is MBR/FAT: it shows as one disk with no partitions;
  never write to it blindly - it holds Emu68 and the Kickstart ROM), the 50 MHz high-speed mode, eMMC. Until
  then: RAM-disk root from the floppy.
- Signals and handlers, process groups, `/proc`; supplementary groups; a shadow password file; one login session at a
  time (workspaces are not separate sessions); user and group ids are 16 bits.
- Shared libraries; only static programs.
- A graphical programs API, mouse, sound, network, USB, floppy driver, RTG.
- Display server (the terminal and workspaces are inside the kernel), index server for `locate`/`fzf`.
- ZenixFS; multi-filesystem VFS; ext2 journal; unlink of open files; moving directories between parents;
  triple-indirect blocks.
- Scheduler priorities; kernel is single-CPU by design.
- 68000 support (not a target).

## Hard numbers worth knowing

- 16 processes, 16 message ports, 64 open files, 16 fds per process, 8 extra memory regions per process,
  1 KB environment, argv of at most 16 arguments / 512 bytes, paths of at most 63 bytes (system calls copy into
  fixed buffers), pipes of 2 KB, 9 workspaces, 80 columns x 31 text rows, 200 scrollback lines.
- Kernel stack 4 KB per process; user stack 16 KB by default.
- Timer tick 100 Hz (PAL E-clock 709,379 Hz); the high-resolution clock (`SYS_hrtime`) uses the CIA timer.

## Traps found while building it (details in `Knowledge/amiga/zenix-m0-m3-findings.md`)

- Deep recursion in the shell looked like silent garbage because stacks have no guard pages.
- The 68020 allows odd-address word/long access, so a "crash test" must use `illegal`.
- Relocation tables must skip debug sections.
- Masking all interrupts for a PIO sector made the serial receive interrupt lose characters; mask levels 1-2 only.
- FS-UAE crops the top of the screen; use `DIWV=0x38` for screenshot builds only.
