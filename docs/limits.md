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
  Physical Layer specifications, so clocks, the mailbox calls and card-detect are unverified.
  Partition tables: an RDB or a PC MBR (primary entries; verified in FS-UAE with disk images). The Emu68 boot card
  holds Emu68 and the Kickstart ROM in its FAT partition: never write to it without a plan. Not done: extended
  partitions, GPT, the 50 MHz high-speed mode, eMMC. Until then: RAM-disk root from the floppy.
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
- Kernel stack 4 KB per process; user stack 16 KB by default, 24 KB for `sh`, 32 KB for `gzip` and `edit` (the
  Makefile's `STACK_*`, written into each program's header). No guard below a stack. Measured with `-fstack-usage`
  (2026-10-23): the largest single frames are `mkfs.ext2` main 8.5 KB, `man` 6.1 KB, miniz's Huffman table builder
  5.6 KB (gzip), `fsck.ext2` main 5.4 KB; the directory walkers use 72-170 bytes per directory level (they used
  1.4-2.7 KB: `tar c` 2724, `diff -r` 2580, `chown -R` 2460, `cp -r` 1732, `mv` 1696, `du`/`rm -r`/`grep -r`/`find`
  about 1430).
- Memory per process: at most 8 extra regions at a time (`SYS_memalloc`), each at most 64 MB. `malloc` keeps blocks of
  up to 32 KB in a few growing chunks (32 KB, 64 KB, ... 512 KB each) and gives every bigger block a region of its
  own, so a program holds at most 8 minus its chunks blocks bigger than 32 KB at once (`badptr`, with two chunks
  in use, gets 6); then `malloc` answers NULL. A program file may ask for at most 16 MB (image + BSS) and a
  stack of 4 KB to 1 MB.
- Paths: at most 63 bytes in a system call; the directory tools report a longer joined path instead of cutting it.
- Timer tick 100 Hz (PAL E-clock 709,379 Hz); the high-resolution clock (`SYS_hrtime`) is CIA-B's two timers
  chained, an E-clock count that wraps after about 1.7 hours.
- Input rings between the interrupts and `kwork`: 128 keyboard codes, 1024 serial bytes; a tty queue holds 1024
  bytes of typed input that no program has read yet. Anything dropped is counted on the kernel log.
- Keyboard: 500 ms repeat delay, 33 repeats per second; keymaps `sv` and `us` only; the accent keys that AmigaOS
  treats as dead keys (acute, grave, circumflex, tilde, diaeresis) type the spacing character, there is no
  composition with the next letter.

## Traps found while building it (details in `Knowledge/amiga/zenix-m0-m3-findings.md`)

- Deep recursion in the shell looked like silent garbage because stacks have no guard pages.
- The 68020 allows odd-address word/long access, so a "crash test" must use `illegal`.
- Relocation tables must skip debug sections.
- Masking all interrupts for a PIO sector made the serial receive interrupt lose characters; mask levels 1-2 only.
- The boot tables at `0x1000` are in exec's private low chip RAM: the bootblock may only put them there after the
  OS is dead (a 408-byte hwinfo written into a 512-byte block while AmigaOS lived gave a random guru).
- FS-UAE crops the top of the screen; use `DIWV=0x38` for screenshot builds only.
