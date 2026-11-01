# Limits: what is missing and what is not known

Read from the source; last checked against the code 2026-10-25. This is the home of the hard numbers: other documents
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
the kernel and root; the names `fs`, `fs.*`, `disk` and `net` are reserved for root), cannot open another user's console
(`/dev/ttyN`: root, or the console one is on), and `su`, `tar x`, `find -delete`, here-documents and `mktemp` do not hand one user's
files or environment to another (architecture.md section 14). What they cannot cover: a program written to cheat
can still overwrite a message after the kernel stamped it, or any memory at all.

**The shipped images are not locked.** They come with root logged in automatically and no passwords: root, user
and guest have empty passwords and `/etc/autologin` names root, which logs root in on console 1 (the tests rely on
it; the other consoles ask for a name). `login` prints a one-line
warning while this is so. Set the passwords with `passwd` and remove `/etc/autologin` before trusting the system.

A second consequence: **servers are trusted code.** A hung filesystem server blocks every client waiting on it.

**Shared code (DESIGN D13) at this trust level.** One copy of a ZEX2 program's code is run by all its processes, and
nothing but good behaviour keeps a program from writing into it: the kernel refuses to write there for a system call
(`user_ok_wr`), but a program writing through a stray pointer changes the code its other processes run. The checksum
finds that at the next start (a fresh copy is loaded) and when a process running it faults (the log names the
processes still running the damaged copy); those go on running it - nothing can stop them without an MMU. **Raw
messages** a program sends itself (`SYS_call`) carry pointers the servers write through without a check (msgpingd,
diskd's `DISK_INFO`/`DISK_LIST`, root's raw filesystem requests): a program that sends raw messages (bench, badptr,
dd, fsck.ext2, fdisk, msgping, mount, msgpingd, mkfs.ext2, priotest, svctest) must stay ZEXE until the servers check
pointers.

## Not done yet (the plan: ROADMAP.md)

- Real hardware. Everything is verified in FS-UAE only. Emulator timing is not CPU timing, so **no speed claim is
  made**; the TRAP cost, message cost and cache effects are unmeasured on a real 68020+.
- Disk access on the user's own A1200 (a PiStorm): its disk is the Pi's SD card behind Emu68's drivers, which
  die when Zenix kills AmigaOS. Done in code (`kernel/emmc.c`, stage0 device-tree handover; see architecture.md
  section 3) but **never run on a real Pi**: the SD controller is modelled from the SD Host Controller and SD
  Physical Layer specifications, so clocks, the mailbox calls and card-detect are unverified.
  Partition tables: an RDB or a PC MBR (primary entries; verified in FS-UAE with disk images). The Emu68 boot card
  holds Emu68 and the Kickstart ROM in its FAT partition: `mount -t fat` can write there, so never do without a plan. Not done: extended
  partitions, GPT, the 50 MHz high-speed mode, eMMC. Until then: RAM-disk root from the floppy.
- Signals (DESIGN D16) have no SIGSEGV/SIGBUS handlers (a fault ends the program, 139), no sessions, no controlling
  terminals other than the consoles and no SIGHUP on a hang-up (with telnet's pseudo-terminals, later); SIGQUIT,
  SIGTTIN, SIGTTOU and SIGWINCH are never generated; `poll` takes at most 32 items, `select` 32 descriptors; a signal
  frame needs 84 bytes of stack and leaves 512 bytes above the stack guard, or the process is killed.
- `/proc`; more than 8 supplementary groups a process (`NGROUPS`; login, su and sudo say so and keep the first 8);
  a shadow password file (`/etc/passwd` is
  world-readable; the hashes are 1000 rounds of salted SHA-256, a number chosen for a 14 MHz 68020, not measured on
  hardware); user and group ids are 16 bits.
- Shared libraries; only static programs.
- A graphical programs API, a mouse pointer (the mouse is only read, `input`; untested), sound beyond the bell, a
  network card (the network runs over the loopback and SLIP; drivers from D19 step 7), DHCP (step 6), USB, RTG. The floppy driver is experimental: DD disks only (an HD drive or disk is not recognised), no drive-type
  ID, never run on a real drive or a Gotek.
- Display server (the terminal and the consoles are inside the kernel; workspaces and a bar come with the desktop),
  index server for `locate`/`fzf`.
- ext2: a journal (an ext3 volume mounts read-only, ext4 is refused), moving a directory to another parent (`mv`
  copies it; ZenixFS moves it), triple-indirect blocks, files over 4 GB. ZenixFS: what zenixfs.md lists as not done.
- ZenixFS as the root: verified in FS-UAE only (DESIGN D34), not on real hardware, and only with the kernel `kernel-zfs.bin`
  (the shipped one has no ZenixFS server built in). A ZenixFS mount needs `/sbin/zfsd` on the root, and its restart reads that file from the root.
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

- 32 processes, 28 message ports (12 held for services, 16 ordinary), 64 open files, 16 fds per process, 16 extra memory regions per process, 8 mounts
  (the root included), 1 KB environment, argv of at most 256 arguments / 8 KB (-E2BIG beyond; `xargs` splits into runs that fit), paths of
  at most 255 bytes, in a system call and once made absolute (the working directory included when anything but the
  root is mounted; the working directory's own text too; -ENAMETOOLONG beyond, never cut; also where a link's target
  plus the rest of a path would be longer, DESIGN D26; architecture.md section 7, "Paths"), names in a folder of at most
  63 bytes (`udirent`: FFS and PFS3 list a longer one by its start and a number, which opens it), a mount point of at
  most 47,
  40 symbolic links followed in one path once anything but the root is mounted (and always by `chdir`, `mount` and
  `realpath`; -ELOOP beyond; with only the root mounted the server's own rule: 8 nested), link targets of at most 255
  bytes (-ENAMETOOLONG), pipes of 2 KB, 10 virtual consoles, 80 columns x 32 text rows on PAL (25 on
  NTSC), 200 scrollback lines per console (32 KB each: a character and a colour byte per cell, allocated when the
  console first scrolls), a cooked tty line of 511 characters (bytes: a Swedish letter is two, DESIGN D28; a key past that rings the bell, and at its end the whole line is refused with
  "tty: the line was too long": nothing of it is handed on, also when the shell's editor took over halfway).
- UTF-8 in the console (DESIGN D28, architecture.md section 11): the font draws box lines, blocks, braille, CP437's
  symbols, Claude Code's marks and Western letters (Latin Extended letters as their base letter: `ł` as `l`, the
  accents lost); Greek and Cyrillic text, other scripts and colour emoji are the box (a wide one the wide box; an
  add-on font later if wanted). Characters beyond U+FFFF keep their width and are the box. Zero-width characters
  (combining accents, joiners, variation selectors) are not stored: a letter written with a separate accent (NFD
  text, macOS file names) shows without it. Backspace over a Tab moves back one column; over a zero-width character
  it removes it with nothing visible changing; right after a character in the last column (the wrap pending)
  Backspace's echo lands one column left, as on Linux's console (a wide character there: one column too many). Widths are Unicode 15.0's (the C library's tables): a few symbols
  newer than that, and emoji written with a variation selector (`❤️`), may take another width in a Linux terminal.
  The font: at most 4,096 glyphs in 255 half-pages, 98,848 bytes, 8 x 8 only, Latin-1 always Topaz's; one for every
  console. A console that shows a character above U+00FF takes 2,560 bytes more, and 80 bytes for each of its
  scrollback lines with such characters (at most 16,200); with no memory for them such characters show as `?`.
- The screen (DESIGN D14, architecture.md section 11): twice the screen's height in chip RAM, 40 KB at 1 bitplane,
  80 KB at 2, 120 KB at 3, 160 KB at 4, and 7 KB more for the glyphs with more than one plane; by default 4 planes
  only on AGA with fast RAM, 3 on OCS/ECS with fast RAM and on every machine with 2 MB of chip RAM and no fast RAM,
  else 1; `/etc/console` chooses 2, 4, 8 or 16 colours (4 planes only on AGA), at init's start, if the chip RAM is
  there. A console's loaded font (DESIGN D20, `CON_FONT`): one per console, PSF1 of exactly 256 glyphs of 8 x 8 (no
  512-glyph fonts, no Unicode table, no other height), 2 KB plus 8 KB with more than one plane while it is loaded.
- Services (DESIGN D11, architecture.md section 9): 12 entries; a program and its arguments of at most 6 words in 79
  characters (the text is copied into 80 bytes with its end); at most 10 services beside diskd and the root, but the
  8 mounts include the root, so at most 7 mounted filesystems besides it; a registration
  waits up to 2 s for a slot or a name being given back; a started server has 30 s to open its port; the start after the 2nd, 3rd and 4th end within 60 s waits 1, 2
  and 4 s, and the 5th end within 60 s gives a service up; loads nest at most two deep; a server's saved table is at most 512 bytes (diskd's is 426); a restarted filesystem server takes at most 160 open
  inode numbers from the kernel (more than the 64 open files and 32 program loads there can be).
- FAT (DESIGN D30, `docs/d30-fat.md`): 512-byte sectors only; 64 open files a mount, 16 chains waiting for a commit (more
  forces one), a 24-sector cache; a long name over 63 bytes of UTF-8 is listed by its 8.3 name; a FAT mount's options at
  most about 30 characters (the service text); no `fsck.fat` on Zenix.
- OFS and FFS (DESIGN D31, `docs/d31-ffs.md`): only DOS\0-DOS\3 with 512-byte blocks are written; files under 2 GB; 64 open files a mount, 16 removed entries waiting for a commit (more
  forces one), a cache of 24 blocks at 512 bytes (8 at 2 KB and over); a long name over 63 bytes of UTF-8 is listed by its start and block number; comments are not shown; options at
  most about 30 characters (the service text).
- LhA and LZX (DESIGN D33, `docs/d33-archives.md`): 512 MB unpacked by default, at most 4000 MB (`-L`); archives under 2 GB; names to 255 bytes of UTF-8; 64 extended headers a member; memory at most 73 KB + 2.5 KB; filenotes shown to 79 bytes.
- PFS3 (DESIGN D32, `docs/d32-pfs3.md`): read-only; files under 4 GB (a larger one is listed, and answers `EFBIG` at open); 64 open files a mount; a cache of 16 reserved blocks at 1 KB (8 at 2 KB and over) and one data block; a long name over 63 bytes of UTF-8 is listed by its start and anode; comments and `.deldir` are not shown; options at most about 30 characters (the service text). Real volumes have deep trees, and the system's path limit above counts here: a name deeper than 255 bytes from the root is reached only from a working directory near it, not by `cp -r` or `find`, which say "file name too long" and go on.
- NFS (DESIGN D23, architecture.md section 9 "nfscd"): 8 NFS mounts at most (nfs0 .. nfs7, within the 7 mounts);
  `HOST:/EXPORT` at most 62 characters with no options (the service text); files below 4 GB, offsets below 2 GB;
  names up to 63 bytes listed; the kernel's 8 supplementary groups a request (for a root program's own request: 16 a
  user, 32 users with groups, read at nfscd's start); the
  small profile (chip RAM only, or under 2 MB of fast RAM free): 4 KB reads and writes, 64 files in the handle table,
  a 16 KB data cache, 32 KB of unsaved written data, 8 requests at once plus 8 waiting in nfscd; the large: 32 KB,
  256 files, 32 requests; a heap cap of 160 KB (small) or the cache and 1 MB (large); counts in `df` capped below
  4 TB; 8 unlinked open files kept under `.nfs` names at once (more are removed at once).
- Updates (DESIGN D15, architecture.md section 3, "Updates"): a bundle of at most 16 MB and 64 parts, 16 packages,
  8 dependencies a package, 1024 changes in one install, paths in a package of at most 55 bytes (with `.zup-new`
  63, the manifest's rule for every zup), a manifest or a journal of at most 1 MB. zenixboot: the trial ends after 3 starts without a sign
  of trouble; its question waits 10 s; boot gives zup 20 minutes.
- Priorities (DESIGN D10, architecture.md section 6): 32 levels; nice -20..19 (a value outside is taken as the nearest
  end); a 50 ms slice; the built-in servers at nice -10; priority inheritance passes along a chain of at most 8 servers.
- The shell: no fixed count of arguments, glob matches, words, pipeline stages, `for` items or positional
  parameters, and no limit on a script's length (it is read and run a command at a time). They grow in its arena,
  which starts at 8 KB and takes more from the kernel while a command needs it (chunks of at least 16 KB, each
  four times the last, so the 16 memory regions a process may have reach several MB), given back when the
  command is done; when the kernel has no more: "sh: out of memory" and that shell ends (status 2); a word of at most 4095 characters (longer is an error, status 1; an expanded here-document has no limit: it is built in the arena);
  at most 8 here-documents on one line, 16 nested alias expansions, 64 `local` variables in the running functions,
  128 variables, 48 aliases and 48 functions, 200 history lines; a typed line of 478 characters (the line editor: a
  key past that rings the bell, and Enter then does not run the line but says "sh: the line was too long", status
  2; a recalled history line cut to 478 the same; longer commands go on over several lines with a final backslash,
  at most 2047 bytes).
- awk: a `printf`/`sprintf` field width or precision of at most 4096 ("field width too large", status 2).
- Kernel stack 4 KB per process; user stack 16 KB by default, 24 KB for `sh`, 32 KB for `gzip`, `edit` and `markdown`, 4 KB for
  `boot` and `init`, 8 KB for `diskd` (the last three live for ever and use under 700 bytes: DESIGN D18; the
  Makefile's `STACK_*`, written into each program's header). The lowest 16 bytes of each stack are the guard
  pattern (architecture.md section 6). Measured with `-fstack-usage`
  (2026-10-23): the largest single frames are `man` 6.1 KB, miniz's Huffman table builder 5.6 KB (gzip), `fsck.ext2`
  main 5.4 KB (`mkfs.ext2` main was 8.5 KB; 0.3 KB since its two 4 KB blocks are static, 2026-10-25); the directory walkers use 72-170 bytes per directory level (they used
  1.4-2.7 KB: `tar c` 2724, `diff -r` 2580, `chown -R` 2460, `cp -r` 1732, `mv` 1696, `du`/`rm -r`/`grep -r`/`find`
  about 1430).
- A login session (the shell and its work area; the login steps aside once the shell runs, DESIGN D18) needs about
  78 KB (measured 2026-10-26 in FS-UAE as the drop in `free` when a second console logs in on a 2 MB chip-only A1200:
  110 KB while the login stayed, 170 KB before malloc's first chunk went from 32 KB to 4 KB and the shell's two 4 KB
  expansion buffers moved to its arena; 293 KB before the shell memory fix of 2026-10-25): a machine with 1 MB of
  chip RAM and no fast RAM has room for four (three before D18); on the next console login says "cannot start
  /bin/sh: out of memory" or init "cannot start /bin/login: out of memory" (the matrix checks both; that message was
  lost before 2026-10-25, init never flushed it).
- Shared code (DESIGN D13, architecture.md section 4): at most 8 segments (`NSEG`) at a time; a ninth program with
  shared code runs as a private copy. A ZEX2 program's address table is at most 32 KB (16-bit offsets: a larger one
  is a link error).
- Memory per process: at most 16 extra regions at a time (`SYS_memalloc`, `NREG`), each at most 64 MB. `malloc`
  (docs/d27-c-library.md "The allocator") keeps blocks in chunks that grow with the request (4 KB, then 1.41 times
  more for each region it holds, at most 4 MB a chunk) and gives a block of 8 KB or more a region of its own while it
  holds fewer than 10; past that big blocks go into the chunks too, so a program runs out of memory, not of regions,
  first (in the PC's test 16 regions hold over 11 MB of 100-byte blocks); then `malloc` answers NULL. A program file may ask for at most 16 MB (image + BSS) and a
  stack of 4 KB to 1 MB.
- Paths: the directory tools report a joined path longer than a system call takes (255 bytes) instead of cutting it, and
  one the kernel refuses once made absolute. What a path costs (measured 2026-10-31, `tools/ksize-budget`, -fstack-usage;
  all on the 2 MB stock A1200 too): 3,072 bytes of RAM for the working directories (96 in each of the 32 process slots),
  always; kernel.bin 904 bytes smaller (the path calls share one copy-in), so 2,168 bytes more in memory all told; all
  programs together 2,812 bytes more on disk; `wasabid` 1.5 KB more of its own (paths in 4 connections), `nfscd` 96 bytes
  more (the export) and 96 a request held. On the 4 KB kernel stack, only during a call: a rename through a link across
  mounts takes 1,400 bytes up to the server's message (1,408 with the old limits), an open 1,372 (1,384), `umount`
  through a link 1,632 (1,496), `mount` 1,624 (1,320), a read or write 112 less than before; each nested start of a server (at most
  two) 1,064 (1,076). The walks use the buffer the path was copied into, so a path costs one buffer a call, not three.
- Sockets (DESIGN D19, architecture.md section 11): 32 kernel sockets for the whole machine, the last 4 only for root
  (-ENFILE beyond); each is also one of the 64 open files and one of a process's 16 fds; a buffer is checked and
  sent at most 64 KB a message (a blocking stream send loops, a datagram over 64 KB is -EMSGSIZE); an address at most
  28 bytes (the true length is still returned), an option's value at most 64; `SO_RCVTIMEO`/`SO_SNDTIMEO` in ticks
  (10 ms), at most about 248 days (a longer one is cut to that). Without `net` registered (no `/sbin/netd`) every
  socket call is -ENETDOWN.
- The network server, netd (DESIGN D19 step 4, man/netd.txt): IPv4 only, no raw IP sockets, no firewall; its buffers
  are fixed at its start by the machine: below 2 MB of free fast RAM (the stock A1200) 8 TCP connection ends (a
  loopback connection takes two; TIME_WAIT ones are given up first when full), 4 listening sockets, 8 UDP and ICMP
  sockets, a 1,932-byte window, a 4 KB send buffer each, 8 datagrams or 4 KB queued per UDP socket, 24 KB of buffers
  in all; with more, 32 ends, 8 listeners, 16 UDP sockets, 16 KB windows and send buffers, 200 KB in all. A socket
  over a limit is refused at once (-ENOBUFS). The users other than root together hold at most all TCP ends but 2
  and all UDP sockets but 4 (6 and 4 on the stock A1200), so root's servers always find one. At most 8 requests a turn; `/etc/hosts` at most 64 names and 16 KB (so
  `/etc/resolv.conf`), 2 name servers; a lookup answers at most 4 addresses; `netstat` shows at most 64 rows.
  `fcntl(F_SETFL, O_NONBLOCK)` works on sockets only (pipes and terminals: -EINVAL); a pipe is non-blocking only when
  made so (`sys_pipe2(fds, O_NONBLOCK)`, both ends).
- The C library (DESIGN D27, block 3 steps 1-4; programs built on it so far: `/tests/libctest`, `libctestpure`,
  `/tests/tuitest`): paths at most 255 bytes
  (`PATH_MAX` 256), names 63, 16 descriptors (`OPEN_MAX`); `open`'s and `mkdir`'s mode is set by a `chmod` after the
  call (the system calls take none: not atomic); `stat` gives one time (the modification's) for all three; the wall
  clock has whole seconds (`gettimeofday`'s microseconds 0), `CLOCK_MONOTONIC` and the sleeps 10 ms ticks, `clock()`
  is wall time since the start; `lseek` past 4 GB `EOVERFLOW`; `readdir`'s `d_type` always `DT_UNKNOWN`; the
  environment at most 63 variables and 1 KB; `printf` without a float build prints `%f` as `*float*`; no `fork`,
  `exec`, `mmap`, `sbrk`, threads; `O_CLOEXEC` is 0 (a spawned program gets only descriptors 0-2 anyway), `O_SYNC`
  and `O_NOCTTY` are not defined. The terminal (D27 5.4): `ICANON`, `ECHO` and `ISIG` all on is the kernel's cooked
  mode, any of them off its raw mode (with `ICANON` the C library edits the line: erase, kill, ^D, at most 511
  characters; ^Z and ^\ are bytes there, only ^C signals in raw mode); `VMIN` 1 and `VTIME` 0 only (else `EINVAL`);
  the other bits (`ICRNL`, `IXON`, `OPOST`, the speeds ...) are kept and answered back but change nothing: in raw mode
  Return is `\r`; `tcgetattr` answers what the process set last (cooked at its start); no `SIGWINCH`; `TIOCGWINSZ`
  is the console's size. Processes: `posix_spawn`'s file actions only for the child's descriptors 0-2 (others
  `EINVAL`), at most 8 opens; `envp` set around the spawn; only `POSIX_SPAWN_SETPGROUP` (done right after the spawn);
  `waitpid` for one pid or any (-1; 0 and other groups `EINVAL`), a status as the shell's `$?` (`WIFSIGNALED` never
  true: a program a signal ended shows 128 + the signal as its exit status); `system` ignores `SIGINT` and `SIGQUIT`
  while it waits from the moment the caller runs again after the spawn (the child inherits ignored signals and
  `SYS_spawn` cannot give it the defaults): a signal sent before that still reaches the caller. `glob`: `*`, `?`, `[...]`, a backslash, the flags but `GLOB_BRACE`, `GLOB_TILDE`,
  `GLOB_ALTDIRFUNC`, `GLOB_LIMIT` (`GLOB_ABEND`). `regcomp`: `user/regex.c`'s syntax, 9 groups reported, `REG_NEWLINE`
  only for `^` and `$`, `REG_NOTEOL` `REG_INVARG`. `getgrnam`'s members at most 127 bytes. `fcntl`, `ioctl` and the
  sockets as libz's (below and above: `O_NONBLOCK` on sockets and `pipe2` only).
- The text-UI library (roadmap block 3, `ports/tui/README`; the owner's tui.library linked in, used by
  `/tests/tuitest`, `sumi`, `maki`, `markdown` and `ahiru`): UTF-8 output and the C library's widths since the console's UTF-8
  (DESIGN D28; `make TUI_UTF8=0` the Latin-1 build), no composing of a separate accent onto its letter; no mouse, no pastes, no resize events, no second screen (closing clears it); `TuiSignal` bit 8 (the
  terminal's input, which the AmigaOS layer's `Wait` polls), `TuiWait`'s extra signals at most 8 descriptors; a grid of at most 400 x 200 cells; 64 events queued (upstream drops the one
  past that); about 80 KB of memory while a terminal is open on 80 x 32 (two grids of 6-byte cells, 31 KB, which the
  allocator keeps for reuse after closing until the program ends).
- Sumi (`ports/sumi/README`, man/sumi.txt): no mouse and no clipboard shared with other programs; settings only with
  `HOME` set; a path of at most 255 bytes; saving through a link replaces the link; about 116 KB of its own and 87 KB of
  shared code with one file open on 80 x 32.
- Maki (`ports/maki/README`, man/maki.txt): no mouse; a long search not stopped by Ctrl+C (a key); settings only with
  `HOME` set; a path of at most 255 bytes; about 202 KB of its own and 63 KB of shared code with one file open on
  80 x 32.
- Markdown (`ports/markdown/README`, man/markdown.txt): no mouse, no pictures, no browser, no clipboard shared with
  other programs; containers inside containers to 24 levels (a 32 KB stack); a link going up a folder (`../x`) opens
  the wrong file; settings only with `HOME` set; a path of at most 255 bytes; about 242 KB of its own and 136 KB of
  shared code with one file open on 80 x 32.
- Ahiru (`ports/ahiru/README`, man/ahiru.txt): inside the terminal only (no screen or window of its own); no mouse,
  picture previews, sound, ARexx, shared clipboard, background copies or drives for disk images; lha, lzx and zip
  archives read only; AmigaOS's protection letters on the status row; g/ and dt (an AmigaOS path's root and trashcan)
  do nothing; names compared without case; settings only with `HOME` set; a path of at most 255 bytes; folder walks to
  20 levels (the core's MAXDEPTH); about 392 KB of its own and 223 KB of shared code with a folder open on 80 x 32.
- SLIP (DESIGN D19 step 5): 115,200 baud only (Zenix's serial port, D12), static addresses only (no DHCP over SLIP),
  MTU at most 1006, one serial port; while it is up the port is the network's (no `cu`, the kernel log not on the
  wire). The kernel's receive ring is 1 KB (89 ms of SLIP): when netd cannot read for longer (a busy program at its
  level) bytes are lost and counted in `dmesg`, and TCP sends them again. At most 4 interfaces with the loopback;
  `/etc/network/interfaces` holds at most 4 stanzas (the rest reported); only the default route can be set
  (`route`); `ping` sends at most 1,400 data bytes, at most 5 a second for users.
- The network's drivers (DESIGN D19 step 7, architecture.md section 11): 4 claims on the whole machine, one DMA region
  a claim of at most 4 MB, a quiesce program of at most 8 steps (no reads, no tests, no jumps); a driver has at most
  32 receive buffers of 1,536 bytes (frames of at most 1,514 bytes: an MTU of 576 to 1,500), at most 16 entries of
  each kind in one transfer, a frame to send in at most 4 pieces; netd keeps at most 32 frames queued or out with a
  driver (16 for a DMA one) and drops the next (TCP sends it again). Drivers are polled (no interrupts yet, D19 5.4).
  A driver that ends 5 times in 60 s is given up until `ifup`; `ifup IFACE` waits at most 10 s for its driver to
  attach. One search domain, used only for a name without a dot.
- Random numbers (DESIGN D19 9.3, architecture.md section 3): a read or write of `/dev/random` or `/dev/urandom` moves
  at most 4,096 bytes (a short count, as Linux); `/dev/random` waits for 256 credited bits (on a fresh machine that
  is 256 key presses and releases, or the seed file at the next start); a sample ring of 32 entries (a full one drops
  samples, which then earn nothing); the generator takes a new key from the pool at most once a minute once seeded.
  The seed file is written at every start, at the pool's first seeding and by `reboot`, not at the reset warning
  (the kernel syncs there, it does not write files). The floppy's index pulse is credited only while the driver waits
  for a track (it polls). The sources' real behaviour on the A1200 and the RNG200 are not yet verified there.
- Timer tick 100 Hz to within the E-clock's resolution (latch 7093 on PAL, 709,379 Hz: 99.997 Hz; 7158 on NTSC,
  715,909 Hz: 100.001 Hz); the high-resolution clock (`SYS_hrtime`) is CIA-B's two timers chained, an E-clock count
  that wraps after about 1.7 hours (`bench` converts it with the PAL rate).
- Error numbers: the kernel and the servers return negative errno values (architecture.md section 7). Still a bare
  -1: `SYS_mountinfo` past the end, `ipc_recv` on a bad port, `port_create`/`port_find`, the internal
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
