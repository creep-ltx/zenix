# Zenix architecture

Zenix is a small Unix-like operating system for Amigas with a 68020 or better and **no MMU**. It does not sit on
top of AmigaOS: the floppy bootblock borrows Kickstart only to load the kernel and read the memory map, then
switches the OS off and owns the machine. The sources are about 5,000 lines of kernel C/assembler (`kernel/`) and
a set of user-mode programs (`user/`).

## 1. The shape of the system

```
 user mode    sh  ls  cat  grep  sed  edit  less  ...   (ZEXE programs from the ext2 disk, /bin)
                 |     (TRAP #0 system calls)
 ------------------------------------------------------------------------------------------
 kernel       scheduler | memory manager | ports + messages | file table, pipes, tty line
              discipline | terminal + display (Copper/blitter) | keyboard | serial | IDE | clock
                 |      (messages: "fs" port, "disk" port)
 user mode    boot (session manager)   fsd (ext2 filesystem server)   diskd (disk server)
              ^-- these three are linked into the kernel image and started before any disk is readable
```

Principles (from `docs/ROADMAP.md`): small kernel, servers for everything else; one way to do a thing; messages
pass pointers, not copies; the standard build contains only what the standard system uses.

Today the kernel is *not* a pure microkernel: the terminal, display, tty, pipes and the file table are still in
it. Only the filesystem and the disk are servers (DESIGN.md, "Where the decisions stand").

## 2. Hardware assumptions

- CPU: 68020 or better. `start.S` checks `AttnFlags` before any C runs; a 68000 gets a message on the serial port
  and a red screen. No MMU is required or used. No FPU is required (`-msoft-float`).
- Memory: **chip** RAM (reachable by the custom chips: display, audio, disk DMA) and **fast** RAM are separate
  pools. Fast is preferred; chip is used when asked for (`MEM_CHIP`) or when there is nothing else.
- Caches: instruction cache on. The data cache stays off, because without an MMU chip RAM cannot be marked
  cache-inhibited and the cache would disagree with DMA. (68040/060 get a `cinva` before the cache is enabled.)
- Interrupts used: level 1 serial transmit, level 2 CIA-A (100 Hz scheduler tick from timer A, and the keyboard),
  level 3 vertical blank (display/scrolling), level 5 serial receive. Gayle IDE shares level 2 with the CIAs, so
  disk transfers mask only levels 1-2.
- Storage: Gayle IDE (A600/A1200/A4000) with an RDB partition table, or a **RAM disk** that the bootblock reads
  from the floppy into memory (for machines with no IDE disk, e.g. a PiStorm A1200 booted from a Gotek), or the
  **SD card of a Raspberry Pi 4 / CM4 under Emu68** (PiStorm; `kernel/emmc.c`, see below - written and host-tested,
  **not yet run on a real Pi**). `kernel/blk.c` picks the raw disk: IDE first, else the SD card. Either one is
  served by `diskd` under the name `ide0`. On an **Amiga 3000** (no IDE) `kernel/a3000.c` drives the on-board SCSI:
  WD33C93A chip behind the DMAC at `0xDD0000`, one "select with ATN and transfer" command per SCSI command, polling
  (levels 1-2 masked while a command runs), data by DMA through a 64 KB chip-RAM bounce buffer, first direct-access
  device found on targets 0-6, 512-byte blocks only; verified on the host against a model of the chip.

## 3. Boot (summary; the full walk-through is in how-it-works.md)

1. Kickstart loads the 1 KB bootblock (`boot/bootblock.S`) from the floppy and jumps to it.
2. The bootblock reads the kernel (and optionally a RAM-disk image) with `trackdisk.device`, builds a
   **bootinfo** block at `0x1000` (CPU flags, RAM regions with chip/fast attributes, kernel address, ramdisk
   address, the ROM Topaz font pointer), picks the largest fast region as the kernel's home, kills the OS, copies
   the kernel there and jumps into it. If the kernel header (ZEXE +28) names a **stage0** routine, the
   bootblock first calls it (while AmigaOS is still alive; skipped on a 68000): it reads Emu68's
   `devicetree.resource` and fills the **hardware table** at `0x1100` (see "Pi hardware table" below).
3. The kernel image is a **ZEXE** file (see below). It relocates itself, clears its BSS, checks the CPU and calls
   `kmain()`.
4. `kmain` starts the serial port, vector table, memory manager, caches, display, clock and disk, then creates
   three built-in processes: `diskd`, `fsd`, `boot`. `boot` waits for the `fs` port and starts `/bin/init`, which
   prints `/etc/motd` and runs `/bin/sh`.

### Pi hardware table (stage0, `boot/stage0.c`, `kernel/hwinfo.h`)

Under Emu68 the Pi's peripherals appear in the 68k address space, but not at fixed addresses: Emu68 rewrites
the `ranges` of the device-tree nodes `/soc` and `/scb` so each window gets the next 2 MB-aligned slot from
`0xF2000000`. The only place that knows the addresses is the device tree, which is AmigaOS memory
(`devicetree.resource`), so the bootblock calls stage0 before the OS is killed. stage0 is position-independent
68020 code (built with `-mpcrel`, no data, no absolute relocations) embedded in the kernel image; it keeps `a6`
(the compiler's frame pointer) intact around every OS call. It translates each node's `reg` through the
rewritten `ranges` and stores, in `struct hwinfo` at `0x1100`: flags (`HW_DT` under Emu68, `HW_BCM2711` on a
Pi 4/CM4, `HW_RANGEOPS` if Emu68 has the cache range opcodes), the peripheral base, mailbox, EMMC2 (SD), GPIO,
GENET Ethernet (registers, two GIC interrupt numbers, PHY address, MAC), GIC-400 distributor and CPU interface,
and the graphics memory Emu68 reserved. On a machine without Emu68 (or FS-UAE) the table is present with
`flags == 0` and all addresses 0; `kmain` prints it (`pi: ...`).

### SD card driver (`kernel/emmc.c`)

The Pi 4/CM4 SD slot is a standard SDHCI controller (EMMC2) at `hwinfo.emmc`. Registers are little-endian
32-bit and always accessed whole; the data port is read and written raw (no byte swap). The driver polls (no
interrupts, no DMA): power-cycles the card and sets the controller clock through the VideoCore mailbox, resets
the controller, runs the SD initialisation (CMD0, 8, ACMD41, 2, 3, 9, 7, ACMD6 for the 4-bit bus), reads the
card size from the CSD, and transfers 512-byte blocks by PIO (CMD17/18/24/25, automatic CMD12; 25 MHz).
Every stage logs with `kprintf`. Only SD cards (CM4 Lite) are handled, not eMMC. The mailbox message is the one
DMA-like access (the VideoCore reads it from RAM), so its cache line is cleaned and dropped with the 68040
`cpushl`/`cinvl` that Emu68 translates.

## 4. Executables: ZEXE and relocation

No MMU means no per-process virtual addresses, so a program must run wherever it happens to be loaded.
`tools/mkzexe.py` takes an ELF linked at address 0 (`-Wl,-q` keeps relocations) and produces:

```
+0  bra.w entry     +4 'ZEXE'     +8 image_size   +12 mem_size   +16 nreloc
+20 reloc_off       +24 stack_size                +28 reserved
...image...   then nreloc big-endian offsets of 32-bit words that hold link-time addresses
```

The loader (`proc_load` in `kernel/proc.c`) allocates the process's memory, **reads the image straight from the file
system into it** (`fs_exec_open` / `fs_exec_read`: no buffer holding the whole file, so a spawn needs only the
process's own memory plus the few-KB relocation table), zeroes the BSS, and adds the load address to every
word in the relocation table. Reading can block (messages to the file system server), so the process slot is chosen
only after the image is in place and nothing blocks after that; two programs may be loading at the same time (the two
stages of a pipeline). Only `R_68K_32` relocations are needed (PC-relative code stays valid), and
debug-section relocations must be skipped (their offsets are not addresses). The kernel itself uses the same
format and the same loop (in `start.S`). Programs declare their stack size in the header (default 16 KB; `sh`
asks for 24 KB, but the `STACK_*` Makefile values are not applied yet - audit B6 - so every program gets 16 KB). There is no shared-library format yet; programs are linked statically against `user/libz.c`.

## 5. Memory (`kernel/mem.c`)

Nothing can be moved once handed out (no MMU, no paging), so fragmentation is the enemy. Rules:

- Chip and fast are separate **ranges**; each range is a run of contiguous blocks with a 16-byte header
  (size + used bit, size of the previous block, **owner pid**, a magic value `ZMEM` that catches overruns and
  double frees).
- Blocks of 16 KB or more are cut from the **high** end of a free block, small ones from the **low** end, so the
  two sizes do not interleave and fragment each other.
- Every block records its owner. `mem_free_owner(pid)` hands back everything a process held, which is how a
  crashed or killed process is cleaned up without any cooperation from it.
- Free-byte counters are O(1) so the status bar can read them from an interrupt.
- `kmalloc`/`kfree` for kernel objects; `SYS_memalloc` gives processes extra regions (at most `NREG` = 8).

A process's own memory is **one block**: image + BSS + stack + the argument area. The kernel stack (4 KB) is a
second block. Both are owned by the pid and freed together.

## 6. Processes and scheduling (`kernel/proc.c`)

- A fixed table of `NPROC` = 16 `struct proc`. State machine: `FREE, READY, SLEEP, WAIT, IO, ZOMBIE, MSG`.
- **Preemptive round robin**: a 100 Hz tick, a slice of 5 ticks (50 ms), an O(1) run queue (linked list).
  No priorities yet (DESIGN D6 says they are intended).
- Preemption happens only when the timer interrupt hit **user mode**. Kernel code is never preempted, so the
  kernel needs no locks. Where an interrupt can touch the run queue (`ready()`), interrupts are masked briefly.
- pid 0 is the idle process: it runs on the boot stack and executes `cpu_idle()` when nothing is ready.
- **No `fork`.** Without an MMU a copy of a process cannot be given the same addresses. The only way to make a
  process is `spawn(path, argv, stdio)`: load a ZEXE, give it the parent's cwd and environment, and pass only file
  descriptors 0-2 (so a pipe's write end can't leak into a reader and prevent end-of-file).
- Process exit closes fds, releases the tty, closes the process's ports (clients get an error), frees all
  memory owned by the pid, and wakes a waiting parent. Orphaned zombies are reaped by the idle loop.
- Each process has 16 file descriptors; the system has 64 open-file slots and 16 message ports.

## 7. System calls (`kernel/syscall.c`)

`TRAP #0`, call number in `d0`, arguments in `d1-d3`, result in `d0`. About 55 calls: files (`open read write
lseek readdir stat fstat lstat chdir getcwd mkdir unlink rmdir rename link symlink readlink chmod utimes dup dup2
pipe`), processes (`spawn wait exit yield sleep kill procinfo`), memory (`memalloc memfree meminfo`), time
(`ticks hrtime time settime`), terminal (`ttymode ttyinfo ttyintr isatty setfg`), environment (`envset envget`),
messages (`port_create port_find call recv reply replyrecv`), mounts (`mount umount mountinfo`; `statfs` takes a
mount index), and a few privileged ones for the servers (`blk_read blk_write blk_flush blk_size blk_ram`).

**Every user pointer is validated** (`user_ok`) against the process's own span and its `memalloc` regions. A bad
pointer makes the call return -1; the process lives. Strings are copied into kernel buffers with bounds.

Identity: `getuid getgid geteuid setuid setgid umask chown session` (see section 14).

`kill` today means "terminate" (`proc->killed`); Unix-style signal handlers do not exist yet. There is one more
thing a process can be told: **stop and continue** (job control). Ctrl-Z on a terminal (`kernel/tty.c`) or `SYS_jobctl`
(pid, 0 = stop, 1 = continue) calls `proc_stop`, which sets `stop_req` and wakes the process if it is blocked; at its
next return from `schedule()` it turns itself into `P_STOP`, tells a waiting parent, and is not scheduled again until
`proc_cont`. `SYS_wait` with flag 2 (`WAIT_UNTRACED`) reports a stopped child once, as `ST_STOPPED` (0x114). A client
inside a call to a server is never stopped until the reply arrives (the server holds pointers into its memory).
`sleep` loops until its time, so a stop and continue does not cut it short.

## 8. Ports and messages (`kernel/ipc.c`)

The central mechanism (DESIGN D1). A **port** is a named queue owned by one process. A client calls

```
struct umsg { next, from, server, state, result, type, a[4], uid, gid, umask, client }
sys_call(port, &msg)   -> blocks until the server replies, returns msg.result
```

- A message lives in the **sender's memory** and only its address moves. With no MMU the server simply reads and
  writes the client's buffers in place: a `read()` of 4 KB is a message holding a pointer to the client's buffer
  and the filesystem server fills it directly. **No copies.**
- Calls are **synchronous** (send-and-wait-for-reply), like QNX and Minix 3, not asynchronous like AmigaOS
  `PutMsg/GetMsg`. A client in a call (`P_MSG`) is woken only by the reply or by the server dying, never by a
  `kill` - the server may still be using the client's buffers.
- A server loops on `replyrecv(msg, result, port)`: reply to the last message and wait for the next in a single
  trap. (Measured in the emulator: this and an O(1) run queue cut a message round trip from 407 to 248 us; only the
  ratio means anything.)
- If a server dies, its ports are closed and its pending and in-service requests fail with an I/O error (-5); the
  kernel then forgets the cached port number and looks it up again next time.
- Drawback of the design, noted in DESIGN.md: a **hung** server blocks its clients.

## 9. The servers

### diskd - the disk server (`user/diskd.c`, port `disk`)
Owns the hardware through privileged raw-sector syscalls (`blk_read/blk_write/blk_size/blk_ram`, only for
processes flagged `priv`). Reads the Amiga **RDB** partition table (`RDSK`/`PART` blocks, DosEnvec geometry) and
serves named devices: `ide0` (the whole disk - IDE or the Pi SD card), `ide0p1`, `ide0p2`... (the partitions of an RDB, else of a PC MBR: type 0x83 = ext2, type 0x76 = an RDB inside, others listed and never mounted), and `ram0` for the boot RAM disk. Handles are
confined to their partition (LBAs are partition-relative). A device opened `DISK_MOUNT` has exactly one writer -
the filesystem server; tools (`fdisk`, `mkfs.ext2`, `fsck.ext2`) open `DISK_RAW`, which is refused while mounted.
`DISK_FLUSH` makes earlier writes durable (ATA FLUSH CACHE through `blk_flush`; the SD driver has no write cache of its own);
`struct diskinfo` carries capability bits and the largest transfer. Protocol: `kernel/diskproto.h`.

### fsd - the filesystem server (`user/fsd.c` + `kernel/ext2.c`, port `fs`)
The ext2 code compiled to run in user mode (`user/fsenv.h` supplies its environment), speaking the protocol in
`kernel/fsproto.h` (`FS_OPEN, FS_READ, FS_WRITE, FS_STAT, FS_READDIR, FS_MKDIR, FS_RENAME, ...`). Files are named
by **inode number**; the kernel's open-file table stores the inode, offset and flags, the server is stateless
about open files. Chooses its root: a RAM-disk image from the boot floppy if present, else the first RDB
partition that probes as ext2, else the bare disk. Started with `fsd DEVICE PORTNAME` (as `/bin/mount.ext2`, by `mount`)
it serves that device on its own port instead; it answers `FS_ROOT` (root inode, device and type names) and `FS_QUIT`
(umount: sync, release the device, exit).

### mount.zenixfs - the ZenixFS server (`user/zenixfsd.c` + `zenixfs/zenixfs.c`, port chosen by `mount`)
The same library that the host tests run (`zenixfs/`, compiled with `-DZENIXFS_FREESTANDING`; `user/zenixfs_env.h`), over the
disk server (`user/zenixfs_disk.c`: filesystem blocks of 1-4 KB as 2-8 sector calls plus `DISK_FLUSH`). Same protocol and the same
permission rules as the ext2 server. Changes commit three seconds after the first one, at `FS_SYNC` and at `FS_QUIT`. The library
allocates only at mount (the Zenix `malloc` is a bump allocator that cannot return small blocks): block cache, bitmaps, three scratch blocks.

### The mount table (`kernel/vfs.c`)
Entry 0 is the root (port `fs`); `SYS_mount` puts a running server's port on a directory (up to 8 mounts). With only the
root mounted, paths go to its server untouched. Otherwise the kernel makes every path absolute and normalised as text
(working-directory text + path, `.` and `..` removed), picks the longest mount point that prefixes it and hands the server
the rest as a path inside that filesystem together with that filesystem's root inode as the working directory. Each
open file remembers its mount. `link` and `rename` across two mounts fail. `umount` is refused while a process has its
working directory or an open file inside, or another mount lies below. Caveat: because `..` is resolved on the text, a
working directory reached through a symbolic link behaves as its text says once a second filesystem is mounted.

### boot - the session manager (`user/boot.c`)
Waits for the `fs` port, spawns `/bin/init`, and then sleeps in `SYS_wsreq`, spawning a shell on a new
workspace whenever the user presses Amiga+Return. (It exists because the idle process cannot block on a message.)

## 10. The filesystem

- **ext2**, revision 0/1, 128-byte inodes, 1 KB or 4 KB blocks, read **and write** (bitmaps, group descriptors,
  direct/indirect/double-indirect blocks, growing directories, rename, hard and symbolic links, fast symlinks).
  Little-endian on disk, swapped on read/write of metadata. Write-through block cache with an MRU/hash lookup.
  Directory entries are parsed in place in the cache.
- Verified against the host's `e2fsck -fn` after scripted write sessions, and byte-for-byte file checksums.
- Zenix ships its own `mkfs.ext2` and `fsck.ext2` (five-pass check and repair) and `fdisk` (RDB).
- Not done: freeing a file that is still open (it frees at once), moving directories between parents,
  triple-indirect files, a journal.
- Several filesystems can be mounted (`mount`, see section 9). Planned: **ZenixFS** (atomic commit, checksummed metadata, extents),
  see `zenixfs.md`.

## 11. Pipes, tty, terminal, display

- **Pipes** (`vfs.c`): 2 KB ring buffer in the kernel; EOF when all writers close; a write to a pipe with no
  readers ends the writer with status 141 (like SIGPIPE). Special files `/dev/tty`, `/dev/null`, `/dev/zero`.
- **tty** (`tty.c`): one per workspace, line discipline in the kernel: echo, backspace, `^U`, `^C` (kills the
  foreground process; if nobody is foreground the shell polls `ttyintr`), `^D`. `TTY_RAW` / `TTY_NOSIG` modes for
  editors and pagers.
- **Terminal** (`term.c`): 80x31 text grid per workspace plus 200 lines of scrollback, ANSI/VT100: cursor motion,
  erase, insert/delete, scroll regions, bold (pixel smear), underline, inverse; `ESC[6n` answers with the cursor
  position. Arrows and function keys arrive as escape codes.
- **Display** (`gfx.c`): 640x256 hires, one bitplane. A **Copper list** splits the screen: palette and bitplane
  pointers for a bar line, then the text area. Scrolling just moves the Copper's bitplane pointers at vertical
  blank; when the 2-screen buffer wraps, the **blitter** copies the visible rows back to the top (identical pixels,
  so there is no visible switch). Glyphs come from the ROM Topaz 8 font, grabbed from `GfxBase` by the bootblock
  before the OS is killed.
- **Workspaces**: nine, each with its own text grid and tty; the top bar (i3-style) lists the current one and the
  occupied ones. Amiga+1..9 switch, Amiga+Return opens a shell. These are scaffolding for a future desktop and are
  not part of what a terminal program should depend on.
- **Keyboard** (`kbd.c`): CIA-A serial data register with handshake; Amiga raw codes to ASCII/escape codes.
- **Serial** (`serial.c`): interrupt-driven, 115200 baud, mirrors the focused screen; the test rig depends on it.

## 12. User space

`user/libz.c` is a tiny freestanding C library (`printf`, string/ctype, `malloc` over `memalloc`, `qsort`, `strtol`,
environment, buffered line reader, a regex engine from the CUtils project). `printf`, `snprintf`, `sprintf`, `fprintf`
and `oprintf` share one formatter with no length limit: `%d %i %u %x %X %o %c %s %p %%`, flags `- 0 + space #`, width and
precision as numbers or `*`; no floating point, no 64-bit numbers. About 77 programs: a full shell (`sh`:
line editor, history, completion, variables, quoting, wildcards, pipes, redirects incl. here-documents,
`if/while/for`, functions, aliases, arithmetic, command substitution, job control), file tools, text tools
(`grep sed sort diff cut tr ...`), `gzip`/`tar`, a pager (`less`), an editor (`edit`), `man`, disk tools (`fdisk
mkfs.ext2 fsck.ext2 df sync`) and test programs (`crash spin badptr hello bench termtest`). The commands are
tested by running one script (`rootfs/tests/cmds.sh`) under Zenix and under GNU bash on the host and comparing the
outputs (`tools/cmds-test.sh`).

## 13. Build and test

- Toolchain: `m68k-elf-gcc` in `~/opt/m68k-elf`; `make` produces `build/zenix.adf` (bootblock + kernel only),
  `build/zenix.hdf` (ext2 root disk) and, as a separate dev/test build, `build/zenix-ram.adf` (the same plus a RAM
  root image).
- Pi driver tests (no hardware needed): `tools/stage0-test.sh` (stage0's logic on the host against the real CM4
  device tree with Emu68's range rewriting), `tools/stage0-68k-test.py` (the real 68020 stage0 code in a CPU
  emulator, `pip install unicorn`), `tools/emmc-test.sh` (the SD driver against a strict SDHCI model,
  `tools/emmc-fake.h`) and `tools/a3000-test.sh` (the A3000 SCSI driver against a WD33C93A + DMAC model). They show the logic matches the specifications; they cannot show the real chip behaves the same.
- Test rig: FS-UAE (headless, serial script) via `tools/run-test.sh`, `tools/serial-tap.py`, `tools/shot.py`.
  `tools/matrix.sh` boots 13 emulated machine variants (A1200 stock and with fast RAM, chip-only, NTSC, 030/040/060,
  A4000, and an A600 that must refuse cleanly). Filesystem work is checked with the host's `e2fsck`.
- Source layout: `boot/` bootblock, `kernel/` the kernel and its protocol headers, `user/` programs and libc,
  `link/` linker script, `tools/` build and test scripts, `rootfs/` files copied to the root disk, `fsuae/` emulator
  configs, `archive/later/` and `archive/old-mmu/` code kept from the earlier MMU-based Zenix (reference, not built).

## 14. Users and permissions (D8)

- Every process has an **effective uid** (`uid`, what permission checks use), a **real uid** (`ruid`, who started
  it), a gid and a umask; all are inherited by `spawn`. `setuid(n)`: root becomes anyone (real and effective); others
  can only return to their real id. `setgid`: root, or the same gid. `chown`: root only.
- A program file with the **set-user-id bit** (mode 04000) runs with its owner's effective uid (`proc_spawn`): this
  is how `su` and `passwd` work for ordinary users.
- `ipc_call` stamps every message with the caller's uid, gid and umask (`struct umsg`), so the **filesystem server
  decides**. The rules are in one dependency-free header, `kernel/fsperm.h`, that every filesystem server
  includes (ext2 today, ZenixFS next); `ext2_access` applies them to ext2 inodes. In words: `ext2_access` checks r/w/x (owner, then group, then other; root passes, except that executing needs
  at least one x bit); `ext2_namei` demands x on every directory searched; creating, removing, renaming and linking
  need w+x on the directory; opening needs r and/or w (and x with `O_EXEC`, which the kernel adds when it loads
  a program); `chmod` is for the owner and root; `chown` for root; a **sticky** directory (`/tmp`, 1777) only lets
  the owner of the file or of the directory remove or rename entries. New files belong to the caller and get
  `0666 & ~umask` (directories `0777 & ~umask`). A refusal returns -13 to the caller.
- `login` (run by `init`) authenticates against `/etc/passwd`, drops root and starts the shell; it calls `SYS_session`
  first, so the kernel remembers the ids and environment of the session and gives them to workspace shells that
  the session manager starts (`SYS_spawn_tty`). `init` clears the session when it ends. One session at a time.
- `mkroot.sh` builds the image under `fakeroot` so files belong to root; `/tmp` is 1777; `su` and `passwd` are 4755.
- The kernel's own file loading and the servers run as root. Kernel-side calls such as `fs_read_file` use the
  identity of the process that asked for the spawn.
- Programs report a refusal as "permission denied": `sys3` stores the code of the last failed call in `zerrno`, and
  `zwhy("cannot open")` (libz) returns that text unless the code was -13.
- What this is not: with no MMU a program can still read any memory and call the disk server directly, so this is
  accounting and protection against honest mistakes (limits.md).
