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
  cache-inhibited and the cache would disagree with DMA. 68040/060 get a `cpusha bc` (0xF4F8: push and invalidate
  both caches) before the cache is enabled - never `cinva`, which on Emu68 and on a copyback 040/060 throws away
  unwritten data. **Every place that writes code the CPU will run flushes the instruction cache first**
  (`cache_flush_code` in `start.S`: CACR bit 3 on 020/030 with the enable bits kept, `cpusha bc` on 040/060 and
  Emu68, which otherwise keeps running its old translation of the bytes): the bootblock after copying the kernel
  and after writing its copier, `start.S` after the self-relocation (followed by a register jump so no stale
  prefetch runs), `proc_load` after a program's image is in place, BSS cleared and relocations applied. The CPU
  type is read from bootinfo (`0x1000`). Verified in FS-UAE (every CPU of the matrix boots); what Emu68 does with
  it is read from its source (knowledge base `amiga/emu68-jit-cache-rules.md`), not yet seen on the PiStorm.
- Code is built with `-m68020-60` and the 68060 multilib's libgcc: no 64-bit `mulu.l`/`divu.l`, which a real 68060
  lacks (`tools/check-68020-60.sh` fails the build on the opcode bytes of kernel, stage0 and every program).
- Interrupts used: level 1 serial transmit, level 2 CIA-A (100 Hz scheduler tick from timer A, and the keyboard),
  level 3 vertical blank (display/scrolling), level 5 serial receive. Gayle IDE shares level 2 with the CIAs, so
  disk transfers mask only levels 1-2. The IDE interrupt itself is never wanted (the driver polls): `ide_init`
  sets nIEN in the device control register (`0xDA101A` on the A1200), clears Gayle's interrupt enable register
  (`0xDAA000`) and its IDE latch (`0xDA9000`, write 0x7C) before probing, so a machine with no drive (a floating
  INTRQ line) cannot storm level 2; a level-2 interrupt with no CIA cause makes `irq2_c` check and clear that latch
  (`ide_stray_irq`, counted and reported once). A port whose status reads 0xFF (no drive, or Emu68's
  `disable_scsi`) is given up at once. ATA's 400 ns settle after a command and after the last data word is two
  CIA reads (E-clock synchronised, so at least 1.4 us on any CPU). All of this is read from the Linux and Fuzix
  Gayle drivers and verified only in FS-UAE, which does not model the latch storm.
- Clock: the 100 Hz tick is CIA-A timer A. `hr_time()` (and `SYS_hrtime`) is **CIA-B timers A and B chained**
  (timer B counts timer A's underflows): a free-running 32-bit E-clock counter (709379 Hz PAL, 715909 NTSC, wraps
  after about 1.7 hours) read from hardware alone, so it is never early or late with respect to the tick
  interrupt (the earlier form, tick count plus timer A's position, read 10 ms low between timer A wrapping and
  the interrupt being taken, and always inside the level-2 handler itself).
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
2. The bootblock reads the kernel (and optionally a RAM-disk image) with `trackdisk.device`, turns the drive's
   motor off (`TD_MOTOR`, length 0), builds a **bootinfo** block (CPU flags, RAM regions with chip/fast
   attributes, kernel address, ramdisk address, the ROM Topaz font pointer) in a 1 KB block it gets from
   `AllocMem` - `0x1000`, where the kernel wants it, belongs to exec while AmigaOS is alive (its chip free list
   starts above it, so it can neither be allocated nor assumed free) - picks the largest fast region as the
   kernel's home, kills the OS, copies the kernel there, copies the block to `0x1000`-`0x13FF`, flushes the
   instruction cache and jumps into the kernel. If the kernel header (ZEXE +28) names a **stage0** routine, the
   bootblock first calls it (while AmigaOS is still alive; skipped on a 68000) with the hardware table's address
   in `a1`: it reads Emu68's `devicetree.resource` and fills the **hardware table** (408 bytes, at `+0x100` in the
   block, so at `0x1100` for the kernel; see "Pi hardware table" below).
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
rewritten `ranges` and stores, in the `struct hwinfo` the bootblock passes in `a1` (at `0x1100` once the kernel
runs): flags (`HW_DT` under Emu68, `HW_BCM2711` on a
Pi 4/CM4, `HW_RANGEOPS` if Emu68 has the cache range opcodes), the peripheral base, mailbox, EMMC2 (SD), GPIO,
GENET Ethernet (registers, two GIC interrupt numbers, PHY address, MAC), GIC-400 distributor and CPU interface,
and the graphics memory Emu68 reserved. It also holds the **Zorro boards** AmigaOS configured (up to 16: manufacturer,
product, type, serial number, base address, size), copied from `expansion.library`'s ConfigDev list with
`FindConfigDev`; that works on every Amiga, with or without Emu68, and is how a future Zorro card driver finds its
card without any autoconfig code of its own. On a machine without Emu68 (or FS-UAE) the Pi part of the table is
empty (`flags == 0`, addresses 0); `kmain` prints the Pi part (`pi: ...`) and the boards (`zorro: ...`).

### Kernel log (`kernel/serial.c`, `dmesg`)

`kprintf` writes to the serial port and into an 8 KB ring. When the display comes up (`gfx_init`), `kmain` writes the
ring to the visible workspace and mirrors new `kprintf` output there until the boot is finished (`klog_mirror_off`
just before the scheduler starts), so the boot messages - including every driver's init log - are on the screen of
a machine with no serial cable (scroll back with Shift+Up). `SYS_klog` (73: offset, buffer, length) reads the ring;
`dmesg` prints it.

### Sound (`kernel/audio.c`)

`beep(hz, ms)` plays a 32-byte square wave from chip RAM on Paula channels 0 and 1 (AUDxLC/LEN/PER/VOL, DMACON
enable) and stops it from the 100 Hz tick. The terminal calls it for BEL (0x07). It is the only sound use so far:
no sample playback, no mixing. The first beep logs whether channel 0 raised its end-of-sample flag (it can be
checked without hearing it).

### Mouse and joystick (`kernel/input.c`)

Polled from the 100 Hz tick: JOY0DAT's two 8-bit counters give mouse movement (accumulated until read, so nothing is
lost between reads), CIA-A port A bits 6 and 7 the left button and the joystick fire button, POTGOR the right and
middle buttons; a joystick in port 2 is decoded from JOY1DAT. `SYS_input` (74) copies out
{dx, dy, buttons, joystick} and clears the movement; `input` prints it. No pointer, no events yet.

### Floppy driver (`kernel/floppy.c`, `kernel/mfm.h`) - experimental

The internal and external drives df0-df3: CIA-B port B selects, steps and starts the motor, CIA-A port A reports
ready, track 0, write protect and disk change; Paula reads and writes whole tracks by DMA (sync word 0x4489,
WORDSYNC on read, precompensation from cylinder 40 on write) into a chip-RAM buffer of 6656 words. `mfm.h` codes
and decodes AmigaDOS tracks (11 sectors of 512 bytes, odd/even bit blocks, XOR checksums); one decoded track is
kept as a cache. A write reads the track, replaces the sectors, writes the track and reads it back to verify.
Waits use `hr_time()` (the CIA-B E-clock counter, section 2). The 100 Hz
tick turns idle motors off after 3 s. System calls 76 `SYS_fd_info` and 77 `SYS_fd_rw`, for the disk server only:
`diskd` offers the drives as `df0`-`df3` (df0 probed at start, the others on first use); `dd` copies to and from
them. Tests: `tools/mfm-test.sh` (host), `tools/floppy-test.sh` (FS-UAE, two drives). **Known bug (2026-10-22):**
a whole-disk read of df1 in FS-UAE has once come back with a wrong SHA-256 and no error; not found yet.

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

**Nothing in the header is trusted** (audit B7): before anything is allocated, `zexe_bad` checks that the image lies
inside the file and holds the header, the relocation table lies inside the file (`nreloc` is compared with what is
left after `reloc_off`, so `nreloc * 4` cannot wrap), memory is at least the image and at most `ZEXE_MEM_MAX`
(16 MB), and a stack size, if given, is between 4 KB and 1 MB; then every relocation offset is checked to name a whole
word inside the image at an even address before the first one is applied. A program that fails is refused: the
kernel logs `exec: PATH refused: WHY`, the spawn fails (the shell says "cannot run") and its memory is freed.

The loader (`proc_load` in `kernel/proc.c`) allocates the process's memory, **reads the image straight from the file
system into it** (`fs_exec_open` / `fs_exec_read`: no buffer holding the whole file, so a spawn needs only the
process's own memory plus the few-KB relocation table), zeroes the BSS, and adds the load address to every
word in the relocation table. Reading can block (messages to the file system server), so the process slot is chosen
only after the image is in place and nothing blocks after that; two programs may be loading at the same time (the two
stages of a pipeline). Only `R_68K_32` relocations are needed (PC-relative code stays valid), and
debug-section relocations must be skipped (their offsets are not addresses). The kernel itself uses the same
format and the same loop (in `start.S`). Programs declare their stack size in the header: 16 KB unless the
Makefile's `STACK_<program>` says otherwise (`sh` 24 KB, `gzip` and `edit` 32 KB; the numbers and how they were measured
are in limits.md). There is no guard below a stack. There is no shared-library format yet; programs are linked
statically against `user/libz.c` (with `--gc-sections`, so each takes only the libz functions it calls).

## 5. Memory (`kernel/mem.c`)

Nothing can be moved once handed out (no MMU, no paging), so fragmentation is the enemy. Rules:

- Chip and fast are separate **ranges**; each range is a run of contiguous blocks with a 16-byte header
  (size + used bit, size of the previous block, **owner pid**, a magic value `ZMEM` that catches overruns and
  double frees).
- Blocks of 16 KB or more are cut from the **high** end of a free block, small ones from the **low** end, so the
  two sizes do not interleave and fragment each other.
- Every block records its owner. `mem_free_owner(pid)` hands back everything a process held, which is how a
  crashed or killed process is cleaned up without any cooperation from it.
- Free-byte counters are O(1), so the status bar can read them cheaply.
- `kmalloc`/`kfree` for kernel objects; `SYS_memalloc` gives processes extra regions (at most `NREG` = 8 at a time,
  each at most `MEMALLOC_MAX` = 64 MB). **Every memory call answers 0 (NULL) on failure, never -1.** `mem_alloc`
  refuses 0 and sizes above `MEM_ALLOC_MAX` before rounding, so no size near 4 GB can wrap into a tiny block.
- Every walk over the blocks (`alloc_in`, `mem_free`, `mem_free_owner`) checks each header: the magic value, a size of
  at least one header, a multiple of 16 and inside the range. A header that fails means something overwrote the heap:
  the kernel prints its address on the serial port and stops (`panic`) instead of looping on garbage.

A process's own memory is **one block**: image + BSS + stack + the argument area. The kernel stack (4 KB) is a
second block. Both are owned by the pid and freed together.

## 6. Processes and scheduling (`kernel/proc.c`)

- A fixed table of `NPROC` = 16 `struct proc`. State machine: `FREE, READY, SLEEP, WAIT, IO, ZOMBIE, MSG`.
- **Preemptive round robin**: a 100 Hz tick, a slice of 5 ticks (50 ms), an O(1) run queue (linked list).
  No priorities yet (DESIGN D6 says they are intended).
- Preemption happens only when the timer interrupt hit **user mode**. Kernel code is never preempted, so the
  kernel needs no locks. Where an interrupt can touch the run queue (`ready()`), interrupts are masked briefly.
- **Interrupts only queue** (audit B4). The keyboard and serial interrupts put raw codes and bytes into rings and
  set `kwork_pending`; everything else input causes (keymap, line discipline, echo, Ctrl-C/Ctrl-Z, workspace
  switches, scrollback, the bar) is done by `kwork()` in process context - see section 11. The same rule closes the
  lost wakeup (B5): a reader's "is there input? no: block" is kernel code, and `kwork`, the only thing that delivers
  input, runs only at a few chosen points, never between such a check and its block.
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
about open files. Chooses its root: a RAM-disk image from the boot floppy if present, else the first partition of
type ext2 (RDB `LNX\0`, MBR 0x83) that holds an ext2 volume Zenix can **write** (see "ext2: what is mounted" in
section 10; an ext3, ext4 or damaged volume is never the root), else the bare disk. Started with `fsd DEVICE PORTNAME` (as `/bin/mount.ext2`, by `mount`)
it serves that device on its own port instead; it answers `FS_ROOT` (root inode, device and type names) and `FS_QUIT`
(umount: sync, release the device, exit). Two root servers are built into the kernel image, `fsd` (ext2) and `mount.zenixfs`
started without arguments (`zfsd`); both run at boot and apply `user/rootpick.h`: a RAM disk is the root; else the bootable
ext2/zenixfs partition; else the first one of those; else the bare disk. The one whose type it is serves port `fs`, the other
exits. `init` runs `mount -a` (from `/etc/fstab`) before login.

### mount.zenixfs - the ZenixFS server (`user/zenixfsd.c` + `zenixfs/zenixfs.c`, port chosen by `mount`)
The same library that the host tests run (`zenixfs/`, compiled with `-DZENIXFS_FREESTANDING`; `user/zenixfs_env.h`), over the
disk server (`user/zenixfs_disk.c`: filesystem blocks of 1-4 KB as 2-8 sector calls plus `DISK_FLUSH`). Same protocol and the same
permission rules as the ext2 server. Changes commit three seconds after the first one, at `FS_SYNC` and at `FS_QUIT`. The library
allocates only at mount (block cache, bitmaps, three scratch blocks), so a running server needs no more memory.

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

- **ext2**, revision 0/1, inodes of 128 bytes or more, 1, 2 or 4 KB blocks, read **and write** (bitmaps, group
  descriptors, direct/indirect/double-indirect blocks, growing directories, rename, hard and symbolic links, fast
  symlinks). Little-endian on disk, swapped on read/write of metadata. Write-through block cache with an MRU/hash
  lookup. Directory entries are parsed in place in the cache.
- **ext2: what is mounted** (`ext2_mount_as`, kernel/ext2.c). Refused: revision above 1, blocks above 4 KB, a
  superblock or group table that does not add up (sizes, inode count, every group's bitmaps and inode table inside
  its group), and every INCOMPAT feature except `filetype` - extents, 64bit, flex_bg, meta_bg, needs_recovery,
  journal_dev, inline_data, ... (so ext4 is refused, with the feature names). **Read-only**: unknown RO_COMPAT
  features (known: sparse_super, large_file), an ext3 journal (`has_journal`: Zenix does not write journals), and a
  volume whose state says "has errors"; every write then fails with -30 (EROFS) and the server writes no block. The
  root is only taken from a volume that would mount read-write (`mount` may mount the others read-only). A volume
  made by a modern `mke2fs -t ext2` (ext_attr, resize_inode, dir_index, filetype, sparse_super, large_file, 256-byte
  inodes) is written so that `e2fsck -fn` stays clean: the filetype byte of directory entries is filled in, a
  directory Zenix changes loses its htree flag (`EXT2_INDEX_FL`; its index blocks read as empty entries), a new
  inode is cleared whole (with `i_extra_isize` as the superblock asks), extended-attribute blocks are released
  (reference count) with their inode, the reserved inodes below `s_first_ino` are never handed out.
- **A damaged disk** fails requests, it never crashes the server or makes it write outside the volume: every block
  number from the disk is checked against the volume in one place (the block cache), directory entries are checked
  (length, name length, inode number) wherever they are parsed, freeing a block or inode that is out of range or
  already free is refused (reported as `ext2: damaged: ...`, at most 20 lines per mount) instead of corrupting the
  counters, and freeing a file visits at most as many blocks as the inode says it owns. Fast symbolic links and
  device inodes keep text and device numbers in the block array: they are never freed as blocks.
- Verified against the host's `e2fsck -fn` after scripted write sessions, and byte-for-byte file checksums; on the
  host by `tools/ext2-host-test.sh` (the server, `fsck.ext2` and `mkfs.ext2` built for the PC against image files:
  modern ext2/ext3/ext4 images, a workload then `e2fsck -fn`, and a bounded fuzz loop of damaged images under
  AddressSanitizer).
- Zenix ships its own `mkfs.ext2` and `fsck.ext2` (five-pass check and repair; it repairs nothing when it finds
  damage it cannot repair) and `fdisk` (RDB).
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
- **Input path: interrupts queue, `kwork` works** (`kernel/main.c`, audit B4/B5). The interrupt handlers do
  only what cannot wait: the keyboard handler (`kbd_isr`) reads the byte, answers the keyboard's handshake and puts
  the raw code into a 128-slot ring; the serial handler (`serial_rx_isr`, level 5) moves each byte from Paula into a
  1024-byte ring; the 100 Hz tick asks for work while a key is held (repeat) and once a second (the bar's clock).
  Each ring has one producer and one consumer, so no lock: the producer fills the slot, then moves `head`; the
  consumer reads it, then moves `tail`. A full ring drops the new input and counts it; `kwork` reports the count on
  the kernel log (`dmesg`: "kbd: N key codes lost", "serial: N bytes lost"; after lost key codes the held modifiers
  and the repeating key are forgotten, since a lost "key up" would leave a key repeating), and a full tty queue
  (nobody reading) says so too. `kwork()` empties the rings in process context with interrupts enabled - `serial_work` (bytes to
  the tty, the 0x1d test hook), `kbd_work` (modifiers, Amiga+key workspace commands, keymap, key repeat, the tty),
  the bar - and is called from exactly four places: the idle loop; the end of every system call; the level-2
  interrupt when it cut into **user** mode (then no kernel code is running at all, because the kernel is never
  preempted; the mask is lowered to 0 first, and if Ctrl-C or Ctrl-Z hit the running program it is rescheduled
  at once so it ends or stops); and between the 256-byte pieces of a long terminal write (`tty_write`: drawing
  4 KB can take seconds on a 68020 with chip RAM only, and the rings would overflow; the terminal is consistent
  there, and a writer killed by Ctrl-C stops at once). An interrupt that cuts into kernel code (a system call, or `kwork` itself) never
  calls it, and a `busy` flag keeps it from being re-entered, so the tty queue and line, the terminal grids, the
  blitter and the heap (the first use of a workspace allocates its tty and terminal) are only ever changed by one
  piece of code at a time. Lost wakeups (B5) cannot happen: `tty_read` and `SYS_wsreq` test "is there something?"
  and then `block_io()` in kernel code with none of those points in between, so `kwork` - the only producer -
  cannot run between the two; input that
  arrives meanwhile waits in its ring, and the `wake_io()` that delivers it comes after the reader is blocked. The
  idle loop checks `kwork_pending` with interrupts masked and then `STOP`s (which enables them in the same
  instruction), so input queued in between ends the wait at once. Latency: at once when the machine is idle or a
  system call ends; at the next tick (10 ms) while a program computes without system calls; every 256 bytes of
  terminal output; while another long system call runs (a disk transfer), input waits in the ring until it ends.
- **Keyboard** (`kbd.c`): CIA-A serial data register; after each byte the handshake pulls SP low for at least
  75 E-clock ticks (105 us; the hardware manual asks for 85) timed with `hr_time()`, never with a counting loop
  (Emu68 runs about 1000 MIPS). The keyboard interrupt is not enabled in CIA-A's mask: the byte is picked up by
  the 100 Hz tick handler, which reads the ICR, so a key waits at most 10 ms (the keyboard's own limit is 143 ms
  and it buffers 10 keys); the handler queues it for `kbd_work` (above). Raw codes 0x00-0x3F go through a keymap with four layers like AmigaOS's (plain, Shift,
  Alt, Shift+Alt; the Alt layer is AmigaOS's, the same on every national keyboard, and is where `[ ] { } \ | @`
  live on a Swedish Amiga); `sv` is the AmigaOS keymap "s" (generated from xkeyboard-config by the Fuzix Amiga
  port, checked against Workbench 3.1's `DEVS:Keymaps/s`), `us` the US one. Caps Lock (raw 0x62, a toggle the
  keyboard reports as down/up) shifts the letters only. Accents that AmigaOS treats as dead keys are typed as
  the spacing character (no composition). Software key repeat: 500 ms, then 33 per second (timed by the tick, done by `kbd_work`), until
  the key is released or another goes down; modifiers and Amiga combinations do not repeat. Codes 0x40-0x67 are
  layout-neutral; arrows, F-keys, Help and Del become escape sequences in raw mode. Verified in FS-UAE
  (`tools/kbd-test.sh`); the handshake length is read from the manual, not measured on hardware.
- **Serial** (`serial.c`): interrupt-driven receive (queued for `serial_work`, above), 115200 baud, mirrors the
  focused screen; the test rig depends on it, including the hook 0x1D + 1-9 (switch workspace) / Return (open a
  shell) / u d U D (scroll back).

## 12. User space

`user/libz.c` is a tiny freestanding C library (`printf`, string/ctype, `malloc` over `memalloc`, `qsort`, `strtol`,
environment, buffered line reader, a regex engine from the CUtils project).

**`malloc`** (audit B2) has to live with `NREG` = 8 regions per process, so small blocks never take a region each.
Every block has an 8-byte header: its capacity and a tag (small in use, big in use, free). Blocks of up to 32 KB
come in size classes of 16, 32, 64 ... 32768 bytes (the header on top, so the usual power-of-two buffer fills its
class), cut from chunks the kernel gives: the first
chunk is 32 KB, each next one twice the last up to 512 KB (smaller when memory is short), so even a program that
needs megabytes uses few regions. A freed small block goes on its class's free list and is used again (O(1), no
coalescing); an empty class carves a free block of a larger class before taking new chunk space; small blocks are never given
back to the kernel (that happens when the program ends). A bigger block gets a region of its own, and `free` gives
exactly those back with `SYS_memfree`. `free` and `realloc` check the tag: a pointer `malloc` did not give out, or
one freed twice, is reported on standard error and ignored. `realloc` grows in place when the block's capacity is
enough, else copies only the old block's bytes. `xmalloc` ends the program with "NAME: out of memory".

**Directory walkers** (audit B6: `rm -r`, `cp -r`, `mv`, `du`, `find`, `grep -r`, `chown -R`, `tar c`, `diff -r`,
`tree`, `ls -R`) use two libz helpers instead of arrays on the stack: `dir_list` reads a directory into a
heap array that grows as needed (no limit on entries; `dir_free` gives it back), and `path_join` makes "dir/name" in
a new heap string, or refuses (NULL, `zerrno` 36, "path too long") when it would be longer than the 63 bytes a
system call takes. A level of recursion costs about 100 bytes of stack instead of 1.4-2.7 KB. `printf`, `snprintf`, `sprintf`, `fprintf`
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
- Test rig: FS-UAE (headless, serial script) via `tools/run-test.sh`, `tools/serial-tap.py`, `tools/shot.py`;
  `tools/kbd-test.sh` presses keys on the emulated keyboard (wtype in the cage rig) and checks the keymap layers,
  Caps Lock and key repeat.
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
