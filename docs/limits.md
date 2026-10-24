# Limits: what is missing and what is not known

Read from the source; last checked against the code 2026-10-23. This is the home of the hard numbers: other documents
point here instead of repeating them.

## The no-MMU limit (permanent unless an MMU mode is added)

Any program can read or write any memory and touch the hardware. Zenix **contains visible crashes** (exceptions
kill only the faulting process; its memory is freed; the kernel validates every pointer passed to a system call;
a magic word in every memory block's header catches overruns of the heap; a guard pattern at the bottom of every
stack, checked at every task switch, system call and tick, kills a process whose stack overflowed - after the fact:
what it wrote below the stack before the check stays written, at most one frame for a program making system calls,
up to one tick (10 ms) of work for one that does not; there is no guard page). It does **not** isolate programs
from each other. A buggy program that writes through a valid-looking pointer can corrupt another process, a server
or the kernel and bring the system down. Users, groups and file permissions (DESIGN D8, built) give accounting and
protection against honest mistakes, not against a program written to cheat - the same trust level as AmigaOS.
The documentation must keep saying this.

What the permissions do cover (audit D): a user cannot kill, stop or take over the terminal of another user's or a
system process, cannot set the clock, cannot talk to the filesystem or disk server directly (the servers answer only
the kernel and root; the names `fs`, `fs.*` and `disk` are reserved for root), cannot open another user's console
(`/dev/ttyN`: root, or the console one is on), and `su`, `tar x`, `find -delete`, here-documents and `mktemp` do not hand one user's
files or environment to another (architecture.md section 14). What they cannot cover: a program written to cheat
can still overwrite a message after the kernel stamped it, or any memory at all.

**The shipped images are not locked.** They come with root logged in automatically and no passwords: root, user
and guest have empty passwords and `/etc/autologin` names root, which logs root in on console 1 (the tests rely on
it; the other consoles ask for a name). `login` prints a one-line
warning while this is so. Set the passwords with `passwd` and remove `/etc/autologin` before trusting the system.

A second consequence: **servers are trusted code.** A hung filesystem server blocks every client waiting on it.

## Not done yet (the plan: ROADMAP.md)

- Real hardware. Everything is verified in FS-UAE only. Emulator timing is not CPU timing, so **no speed claim is
  made**; the TRAP cost, message cost and cache effects are unmeasured on a real 68020+.
- Disk access on the user's own A1200 (a PiStorm): its disk is the Pi's SD card behind Emu68's drivers, which
  die when Zenix kills AmigaOS. Done in code (`kernel/emmc.c`, stage0 device-tree handover; see architecture.md
  section 3) but **never run on a real Pi**: the SD controller is modelled from the SD Host Controller and SD
  Physical Layer specifications, so clocks, the mailbox calls and card-detect are unverified.
  Partition tables: an RDB or a PC MBR (primary entries; verified in FS-UAE with disk images). The Emu68 boot card
  holds Emu68 and the Kickstart ROM in its FAT partition: never write to it without a plan. Not done: extended
  partitions, GPT, the 50 MHz high-speed mode, eMMC. Until then: RAM-disk root from the floppy.
- Signals and handlers, process groups, `/proc`; supplementary groups; a shadow password file (`/etc/passwd` is
  world-readable; the hashes are 1000 rounds of salted SHA-256, a number chosen for a 14 MHz 68020, not measured on
  hardware); user and group ids are 16 bits.
- Shared libraries; only static programs.
- A graphical programs API, a mouse pointer (the mouse is only read, `input`; untested), sound beyond the bell, network,
  USB, RTG. The floppy driver is experimental: DD disks only (an HD drive or disk is not recognised), no drive-type
  ID, never run on a real drive or a Gotek.
- Display server (the terminal and the consoles are inside the kernel; workspaces and a bar come with the desktop),
  index server for `locate`/`fzf`.
- ext2: a journal (an ext3 volume mounts read-only, ext4 is refused), moving a directory to another parent (`mv`
  copies it; ZenixFS moves it), triple-indirect blocks, files over 4 GB. ZenixFS: what zenixfs.md lists as not done.
- Kernel is single-CPU by design. Priorities have no ageing and no CPU-time share (DESIGN D10, "Not taken").
- 68000 support (not a target).
- Booting without a floppy (`zenixboot`): Kickstart 1.3 is not a target (it refuses; 1.3 also cannot boot an RDB
  partition by itself). Started from a running Workbench it switches AmigaOS off as it is: unwritten files are lost,
  and DMA that the Pi side keeps doing for an Emu68 network or USB driver is not stopped. Whether an RTG card gives the
  monitor back to the chipset is not verified. The boot partition must be FFS/OFS that the Kickstart ROM mounts by
  itself, with no FileSysHeader in the RDB: verified for DOS\3 and DOS\1 (plain FFS, what the images use: the type
  every FFS ROM registers) with Kickstart 3.1, 3.2 and 2.05 in FS-UAE. Emu68's own SD driver (`brcm-emmc.device`)
  mounts the RDB inside a 0x76 partition and looks a partition's type up in the RDB's FileSysHeaders, then in
  `FileSystem.resource` (read from the strings in its binary, not its source); that it boots this partition is not
  yet seen on the PiStorm. The SD
  card image's FAT partition is empty (tools/write-card.sh fills it or keeps the card's).

## Hard numbers worth knowing

- 32 processes, 28 message ports (12 held for services, 16 ordinary), 64 open files, 16 fds per process, 8 extra memory regions per process, 8 mounts
  (the root included), 1 KB environment, argv of at most 256 arguments / 8 KB (-E2BIG beyond; `xargs` splits into runs that fit), paths of
  at most 63 bytes in a system call and 159 bytes once made absolute (the working directory included;
  -ENAMETOOLONG beyond, never cut), pipes of 2 KB, 10 virtual consoles, 80 columns x 32 text rows on PAL (25 on
  NTSC), 200 scrollback lines per console, a cooked tty line of 255 characters (the bell for more).
- Services (DESIGN D11, architecture.md section 9): 12 entries; a program and its arguments of at most 6 words in 80
  bytes; a started server has 30 s to open its port; the start after the 2nd, 3rd and 4th end within 60 s waits 1, 2
  and 4 s; loads nest at most two deep; a server's saved table is at most 512 bytes (diskd's is 426); a restarted filesystem server takes at most 160 open
  inode numbers from the kernel (more than the 64 open files and 32 program loads there can be).
- Priorities (DESIGN D10, architecture.md section 6): 32 levels; nice -20..19 (a value outside is taken as the nearest
  end); a 50 ms slice; the built-in servers at nice -10; priority inheritance passes along a chain of at most 8 servers.
- The shell: no fixed count of arguments, glob matches, words, pipeline stages, `for` items or positional
  parameters (they grow in its arena, which is an eighth of free memory, 64-256 KB: "out of memory" ends a script
  that needs more); a word or an expanded here-document of at most 4095 characters (longer is an error, status 1);
  at most 8 here-documents on one line, 16 nested alias expansions, 64 `local` variables in the running functions,
  128 variables, 48 aliases and 48 functions, 200 history lines (a recalled line is cut to the 478 characters the
  editor holds, with the bell).
- Kernel stack 4 KB per process; user stack 16 KB by default, 24 KB for `sh`, 32 KB for `gzip` and `edit` (the
  Makefile's `STACK_*`, written into each program's header). The lowest 16 bytes of each stack are the guard
  pattern (architecture.md section 6). Measured with `-fstack-usage`
  (2026-10-23): the largest single frames are `mkfs.ext2` main 8.5 KB, `man` 6.1 KB, miniz's Huffman table builder
  5.6 KB (gzip), `fsck.ext2` main 5.4 KB; the directory walkers use 72-170 bytes per directory level (they used
  1.4-2.7 KB: `tar c` 2724, `diff -r` 2580, `chown -R` 2460, `cp -r` 1732, `mv` 1696, `du`/`rm -r`/`grep -r`/`find`
  about 1430).
- A login session (login, the shell and its work area) needs about 200 KB: a machine with 1 MB of chip RAM and no
  fast RAM has room for one; a second console's login says "cannot start /bin/sh: out of memory" (the matrix checks it).
- Memory per process: at most 8 extra regions at a time (`SYS_memalloc`), each at most 64 MB. `malloc` keeps blocks of
  up to 32 KB in a few growing chunks (32 KB, 64 KB, ... 512 KB each) and gives every bigger block a region of its
  own, so a program holds at most 8 minus its chunks blocks bigger than 32 KB at once (`badptr`, with two chunks
  in use, gets 6); then `malloc` answers NULL. A program file may ask for at most 16 MB (image + BSS) and a
  stack of 4 KB to 1 MB.
- Paths: the directory tools report a joined path longer than a system call takes (63 bytes) instead of cutting it.
- Timer tick 100 Hz to within the E-clock's resolution (latch 7093 on PAL, 709,379 Hz: 99.997 Hz; 7158 on NTSC,
  715,909 Hz: 100.001 Hz); the high-resolution clock (`SYS_hrtime`) is CIA-B's two timers chained, an E-clock count
  that wraps after about 1.7 hours (`bench` converts it with the PAL rate).
- Error numbers: the kernel and the servers return negative errno values (architecture.md section 7). Still a bare
  -1: `SYS_mountinfo` past the end, `ipc_call`/`ipc_recv` on a bad port, `port_create`/`port_find`, the internal
  block allocators of ext2.c. `zwhy` shows no text for -1 (EPERM, 1) for that reason: the caller's own words appear.
- Input rings between the interrupts and `kwork`: 128 keyboard codes, 1024 serial bytes; a tty queue holds 1024
  bytes of typed input that no program has read yet. Anything dropped is counted on the kernel log.
- Serial send ring: 4 KB, drained at about 11.5 KB/s (115200 baud). Until the port has received a byte since boot,
  console output that does not fit is dropped (counted on the kernel log); after that the console waits for it.
- Keyboard: 500 ms repeat delay, 33 repeats per second; keymaps `sv` and `us` only; the accent keys that AmigaOS
  treats as dead keys (acute, grave, circumflex, tilde, diaeresis) type the spacing character, there is no
  composition with the next letter.
- Disks: IDE only on an A600/A1200 Gayle (Gayle ID 0xD) and the A4000's port; LBA drives only (a CHS-only drive, as in
  some early A600/A1200s, is refused with a message); PIO polling, up to 256 sectors per command on the Gayle port (one on the A4000's). A drive is given up to
  10 s to start (status 0) and 30 s for any one wait: a drive that never answers holds the boot that long. The A3000's
  SCSI waits once per target ID (250 ms selection timeout) and does not wait for slow drives to spin up.
- The reset warning (Ctrl+Amiga+Amiga) gives init at most 8 s to sync the filesystems; only keyboards that send the
  warning give that time at all (the HRM: not every model). After it, disk writes are refused until the reset.
- Kernel log: 8 KB, kept in the top 16.5 KB of chip RAM (two 8 KB slots: this boot's and the previous one's). The
  previous-boot log needs chip RAM to survive the reset: it is lost after a power-off, and on the PiStorm it is not
  yet known whether a warm reset keeps it. It holds the last 8 KB of the previous boot; the next boot shows its last
  20 lines. A boot that stops before the display only shows its colour (no text) until the next boot. With little chip
  RAM (the kernel or the RAM disk in the top 16.5 KB) the log is not kept there at all.

## Traps found while building it (details in `Knowledge/amiga/zenix-m0-m3-findings.md`)

- Deep recursion in the shell looked like silent garbage because stacks had no guard (now a guard pattern, checked
  at task switches and system calls).
- The 68020 allows odd-address word/long access, so a "crash test" must use `illegal`.
- Relocation tables must skip debug sections.
- Masking all interrupts for a PIO sector made the serial receive interrupt lose characters; mask levels 1-2 only.
- The boot tables at `0x1000` are in exec's private low chip RAM: the bootblock may only put them there after the
  OS is dead (a 408-byte hwinfo written into a 512-byte block while AmigaOS lived gave a random guru).
- FS-UAE crops the top of the screen; use `DIWV=0x38` for screenshot builds only.
