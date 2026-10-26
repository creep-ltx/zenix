# Zenix architecture

Zenix is a small Unix-like operating system for Amigas with a 68020 or better and **no MMU**. It does not sit on
top of AmigaOS: the floppy bootblock borrows Kickstart only to load the kernel and read the memory map, then
switches the OS off and owns the machine. The sources are about 9,000 lines of kernel C, assembler and headers
(`kernel/`) and the user-mode programs and servers (`user/`, about 11,000 lines), plus the ZenixFS library
(`zenixfs/`). Last checked against the code: 2026-10-23.

## 1. The shape of the system

```
 user mode    sh  ls  cat  grep  sed  edit  less  ...   (ZEXE programs from the ext2 disk, /bin)
                 |     (TRAP #0 system calls)
 ------------------------------------------------------------------------------------------
 kernel       scheduler | memory manager | ports + messages | file table, pipes, tty line
              discipline | terminal + display (Copper/blitter) | keyboard | serial | IDE | clock
                 |      (messages: "fs" port, "disk" port)
 user mode    boot (starts init)   fsd (ext2)   zfsd (ZenixFS)   diskd (disk server)
              ^-- these four are linked into the kernel image and started before any disk is readable
```

Principles (from `docs/ROADMAP.md`): small kernel, servers for everything else; one way to do a thing; messages
pass pointers, not copies; the standard build contains only what the standard system uses.

Today the kernel is *not* a pure microkernel: the terminal, display, tty, pipes and the file table are still in
it. Only the filesystems and the disk are servers (DESIGN.md, "Where the decisions stand").

## 2. Hardware assumptions

- CPU: 68020 or better. `start.S` checks `AttnFlags` before any C runs; a 68000 gets a message on the serial port
  and the screen orange over red (a stop at stage 5, section 3). No MMU is required or used. No FPU is required
  (`-msoft-float`).
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
- Interrupts used: level 1 serial transmit, level 2 CIA-A (100 Hz scheduler tick from timer A, and the keyboard's
  serial-port interrupt, ICR bit 3), level 3 vertical blank (display/scrolling), level 5 serial receive. Gayle IDE
  shares level 2 with the CIAs; the IDE interrupt itself is never wanted (the drivers poll), and no disk wait runs
  with level 2 masked (a keyboard that gets no handshake for 143 ms resyncs and, after about ten keys, drops them):
  IDE masks levels 1-2 only around each sector's 256 data words and the acknowledge, the A3000's SCSI not at all.
- **IDE** (`kernel/ide.c`). The machine is decided before any IDE register is touched (the hardware hand-off note,
  2026-10-23, items I1-I4, I7, I9, I10): a Ramsey chip (version 0x0D/0x0F at `0xDE0043`: A3000/A4000) means the A4000
  port at `0xDD2020` only; otherwise the Gayle ID - after a check that `0xDE1000` is not a mirror of the custom chips
  (the real INTENA is toggled with interrupts masked and the mirror would show INTENAR at offset 0x1C), a byte written
  to `0xDE1000` and four reads giving one bit each in bit 7 - must be 0xD (A600/A1200) for the Gayle port at `0xDA0000`;
  anything else probes no port. The Gayle ID and the mirror check are not in the public manuals (hand-off note). On
  the Gayle, `ide_init` sets nIEN in the device control register (`0xDA101A`), clears Gayle's interrupt enable
  register (`0xDAA000`) and its IDE latch (`0xDA9000`, write 0x7C), so a machine with no drive (a floating INTRQ line)
  cannot storm level 2; a level-2 interrupt with no CIA cause makes `irq2_c` read the drive's status and then clear
  that latch (`ide_stray_irq`, counted and reported once). The drive must then show it is there: status 0xFF (no
  drive, or Emu68's `disable_scsi`) is given up at once; BSY and DRDY together (impossible) means no drive; BSY alone
  must read the same from the cylinder-low register (ATA: a busy drive answers its status from every register); DRDY
  must keep 0x55 and 0xAA written to cylinder-low; status 0 is a drive still starting (some show 0 for about 9 s,
  hand-off note) and gets up to 10 s, BSY up to 30 s, with a line on the log saying what it waits for. Every wait is
  timed on the E-clock (`hr_time`); ERR is believed only once BSY is clear (ATA). IDENTIFY word 49 bit 9 must say
  LBA: a CHS-only drive is refused with a message (it would take LBA numbers as cylinder/head/sector). Data goes
  through Gayle's fast window (`0xDA2000`, "16 bit" timing; the registers stay in the "8 bit" `0xDA0000` window,
  Gayle spec p.7-8); on the Gayle port up to 256 sectors per READ/WRITE SECTORS command, one DRQ per sector (the
  A4000 port keeps one sector per command: FS-UAE's A4000 IDE stayed busy for good now and then after a READ SECTORS
  of 2, cause not found). ATA's 400 ns settle after
  a command and after the last data word is two CIA reads (E-clock synchronised, so at least 1.4 us on any CPU).
  Verified in FS-UAE (the matrix: Gayle ID 0xD, the fast data window, the A4000 and A3000 setups); the timings of real
  drives, the latch storm and a CHS-only drive are not modelled there.
- Clock: the 100 Hz tick is CIA-A timer A. `hr_time()` (and `SYS_hrtime`) is **CIA-B timers A and B chained**
  (timer B counts timer A's underflows): a free-running 32-bit E-clock counter (709379 Hz PAL, 715909 NTSC, wraps
  after about 1.7 hours) read from hardware alone, so it is never early or late with respect to the tick
  interrupt (the earlier form, tick count plus timer A's position, read 10 ms low between timer A wrapping and
  the interrupt being taken, and always inside the level-2 handler itself).
- Battery clock (`kernel/rtc.c`, read once at boot): an Oki MSM6242 or a Ricoh RP5C01 at `0xDC0000`. Before any
  clock register is written the area is checked not to be a mirror of the custom chips (as for the Gayle ID above;
  a write there would land in a chip register: hand-off note C1); the clock then counts only if its seconds are seen
  ticking. The RP5C01 is read with its timer stopped (bank 0, MODE 0) and restarted after, so a read at a rollover
  cannot mix old and new digits (hand-off note C4). Seen in FS-UAE (the boot log's `clock:` line); no test compares
  it.
- Storage: Gayle IDE (A600/A1200/A4000) with an RDB partition table, or a **RAM disk** that the bootblock reads
  from the floppy into memory (for machines with no IDE disk, e.g. a PiStorm A1200 booted from a Gotek), or the
  **SD card of a Raspberry Pi 4 / CM4 under Emu68** (PiStorm; `kernel/emmc.c`, see below - host-tested, and
  verified on the A1200 since 2026-10-23). `kernel/blk.c` picks the raw disk: IDE first, else the SD card. Either one is
  served by `diskd` under the name `ide0`. On an **Amiga 3000** (no IDE) `kernel/a3000.c` drives the on-board SCSI:
  WD33C93A chip behind the DMAC at `0xDD0000`, one "select with ATN and transfer" command per SCSI command, polling
  (interrupts on, the DMAC's interrupt enable off), data by DMA through a 64 KB chip-RAM bounce buffer, first direct-access
  device found on targets 0-6, 512-byte blocks only; verified on the host against a model of the chip. Its waits are
  timed on the E-clock (they were counted in polls, a thousand times too short on Emu68) with interrupts on, and the
  selection timeout register is 45 (250 ms at 14.3 MHz: ms x MHz / 80, WD33C93A data sheet; it was 20, about
  110 ms).

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
   **Or, with no floppy: `zenixboot`** (`boot/zenixboot.c`, `boot/zenixboot-start.S`), an AmigaOS program on a small
   AmigaDOS FFS boot partition (on an IDE/CF disk's RDB, or under Emu68 in the RDB inside an MBR type-0x76 partition
   of the SD card). Kickstart boots that partition with its ROM filesystem and runs its one-line
   `S:Startup-Sequence` (`SYS:zenixboot`) in its ROM shell; zenixboot reads the kernel *file* (`zenix`, the same
   `kernel.bin` as on the floppy) and an optional RAM-disk image with `dos.library`, checks them, and then runs
   **the same hand-over code as the bootblock**: `boot/handoff.S` holds the stage colours, the klog-area size and
   four macros (`handoff_tables`: bootinfo, GfxBase, stage0, the memory list, the kernel's home; `handoff_kill`:
   SuperState, interrupts/DMA/CIA interrupts off; `handoff_jump`: the copier to `0x500` and the jump; `handoff_subs`:
   the cache flush and the copier). The bootblock built from them is byte-for-byte the one before the split. Unlike the
   bootblock, zenixboot fails safely: everything is loaded and checked while AmigaOS runs, an error is printed (on the
   console and the serial port) and returns to AmigaOS. It keeps its buffers out of the kernel's future place, low
   memory and the klog area (allocating again when a block lands there), and before the jump it also stops every
   floppy motor, shows no screen (`LoadView(NULL)`), resets FMODE/BPLCON0/BPLCON3, waits for the blitter, switches a
   68040/060's data cache (`CacheControl`) and MMU off (TC and the transparent-translation registers, probed with
   the illegal and line-F vectors caught), all of which a running AmigaOS may have changed and a floppy boot never
   has. It is built with the same m68k-elf compiler as the kernel (no Amiga toolchain): linked like a Zenix program
   and wrapped into an AmigaOS hunk executable by `tools/mkhunk.py`. Details and limits: man/zenixboot.txt.
   **Updates, the boot half** (D15, "Updates" below): with `Zenix/` next to zenixboot it takes the kernel from there by
   the start rule (`zenix`, a `zenix.new` with the state's SHA-256, `zenix.old`), installs a bundle's kernel in the
   careful FFS order (each step followed by `ACTION_FLUSH`, DoPkt to `GetDeviceProc("PROGDIR:")`'s port, which FFS
   answers once its buffers are written), keeps `Zenix/state`, puts the bundle (loaded fast RAM first, 16-aligned with
   16 bytes free before it) into bootinfo `+0x78`/`+0x7C` and the rollback flag into `+0x80`, and follows the trial
   through the update note and the previous boot's klog header (evidence: reason PANIC, or RUNNING without
   confirmation, in a boot after its last handover; a cold start is none).
3. The kernel image is a **ZEXE** file (see below). It relocates itself, clears its BSS, checks the CPU and calls
   `kmain()`.
4. `kmain` reads the previous boot's log (below), then starts the serial port, vector table, memory manager,
   caches, display, clock and disk, then creates one process from its image: `boot` (pid 1). `boot` registers the
   disk server and the root filesystem server as services (section 9, "The service manager"): its first call to
   `disk` starts `diskd` (pid 2), it picks the root device and its server from the superblock, and its first call to
   `fs` starts `fsd` or `zfsd` (pid 3); it runs `/sbin/zup` (D15: an update's root half, a journal a power cut left, or the way back; boot waits, says so after a
minute and stops it after 20 - every process named zup, also the second one a new zup runs, audit3 B4 -, then, when
none runs any more, gives the bundle's memory back with `SYS_update RELEASE`); it registers the services of `/etc/svctab` (started when first called); then it starts `/bin/init`, which prints `/etc/motd`, runs `mount -a` and
   starts `login` on console 1 (section 11).

### Boot stage colours (`kernel/diag.h`, audit A11)

The bootblock's first instructions (zenixboot: once its files are open, so that an error message can still be shown)
switch Copper and bitplane DMA off and set `BPLCON0` to no bitplanes (and
`BPLCON3` to 0 on ECS/AGA), so `COLOR00` fills the whole screen. Each boot step then writes its own colour: stages
1-3 in the bootblock (blue, cyan, magenta), 4-5 in `start.S` (yellow, orange), 6-9 in `kmain` through
`diag_stage()` (green, brown, pink, white); from stage 10 the Copper owns `COLOR00` and the boot log is on the
screen as text; grey means the display could not start. A stop on an error before the display is up
(`panic`, an exception in the kernel, a 68000 in `start.S`) calls `diag_halt()` (`start.S`, plain 68000 code): it
polls the beam counter and paints the stage colour above line 150 and red below, forever. After the display is up a
panic turns the screen dark red with white text (`gfx_panic`) and leaves its text there. The whole screen red is the bootblock's
own failure. The table for the owner (colour, where, likely cause) is in pi-first-boot.md. Test builds:
`make DIAG=-DDIAG_STOP=N` hangs at stage N, `DIAG=-DDIAG_PANIC=N` panics there (`tools/diag-test.sh`).

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
ring to console 1 (the console on the screen at boot) and mirrors new `kprintf` output there until the root
filesystem's server has opened its port (`klog_mirror_off` in `svc_handover`), so the boot messages - including every
driver's init log and the start-up lines of diskd and the root server - are on the screen of a machine with no
serial cable (scroll back with Amiga+Up). **The servers' output** (D11 step 8): a process the service manager starts
gets, as its standard input, output and error, a file of type `FT_KLOG`: what it writes goes through `kputc` like
`kprintf` (the serial port, the ring, the screen while mirrored); reading it gives end of file. So after the boot no
server message appears on a console; `mount` says "see dmesg" when a server did not start. `boot` itself (and so
`init`) keeps console 1. `SYS_klog` (73: offset, buffer, length) reads the ring;
`dmesg` prints it. A panic or an exception in the kernel switches the mirror on again, so its text reaches the
screen, and records the reason in the log before the machine stops.

**Previous-boot log (audit A12).** The log lives in the top 16.5 KB of chip RAM (`KLOG_AREA_SIZE` = `0x4200` below the
highest chip address in bootinfo; `0x1FBE00` on a 2 MB machine), so that after a warm reset (Ctrl-Amiga-Amiga) the
next boot can show how this one ended. Why there: Kickstart keeps its own data low in chip RAM (exec's free list
starts at `0x4020` on 3.1) and its free-memory headers at the start of a free block, and allocations go first-fit from
the bottom; the bootblock `AllocAbs()`es the area (exec's `MaxLocMem` - `0x4200`) before it asks for any buffer, so
neither the boot tables nor the kernel/RAM-disk load buffer can land on it, and `mem_init` keeps it out of the chip
pool. Layout: two slots, each a 256-byte header (magic `ZKLG`, ring size, boot number, characters logged, a sum over
the 8 KB ring, the reason the boot ended - running or panic -, and a sum over the header) and the 8 KB ring. Both sums
are updated with every character (interrupts masked, O(1)), so a slot is consistent between any two characters.
`klog_init()`, the first call in `kmain`, checks both slots before anything is printed. A slot counts by its header
alone (magic, size, header sum: D15 step 0); the valid one with the higher boot number is the previous boot's, its
boot number and reason are used, and its text is shown only if the ring's sum matches too (else "its log text is
damaged (checksum): not shown"). The boot menu (`boot/bootmenu.c`) `AllocAbs()`es the whole area to read and clear
the boot note, never only the note's 16 bytes: exec writes an 8-byte free-memory header at the start of the free
remainder an allocation leaves, which was slot 0's first text bytes; a slot rejected for that showed an older boot as
the previous one (`tools/zenixboot-test.sh bootmenu`). Its `AllocAbs()` and `FreeMem()` run under `Forbid()` (audit3
B8): `FreeMem()` writes such a header at the area's start (slot 0's magic and size) unless the free memory below ends
there, and with no other task taking that memory in between it merges back; it writes one there only when a free
block already began at the area, whose header was in those bytes before. (Restoring the 8 bytes afterwards, as the
audit proposed, would overwrite that header and break AmigaOS's memory list.) zenixboot rounds the chip top as the kernel does
(`MaxLocMem & ~0xFFF`). **The update note** (D15) lies at `+0xE0`-`+0xEF`, in slot 0's header padding (outside every
sum, before the boot note at `+0xF0`): magic `ZUPN`, kind (1 done, 2 failed - written by zup, or by `boot` when
`/sbin/zup` is missing or did not end well -, 3 confirmed - written by `login` when its prompt is up on console 1 -,
4 confirmed but the root half failed: the kernel makes a confirmation over a failed note of the same version that, so
login never erases zup's failure, audit3 B1), a version, and `~(magic + kind + version)`. zenixboot reads it at the
next start and clears it; `klog_init` clears it at every start, before every write of it, so it says what this boot
did. This boot writes the other slot (boot number + 1), so the previous log stays where it is
and costs the kernel no memory. Once the display is up, `klog_show_prev()` prints `klog: boot N, log kept
at ...` and "no log from a previous boot", "damaged (checksum)" or the previous boot's last 20 lines, each starting
with a `>`, between `=== previous boot (N) ended here ...` (with or without a kernel panic) and `=== end of the
previous boot's log ===`. `SYS_klog` with bit 31 set in the offset (`KLOG_PREV`) reads the previous slot (`dmesg -p`).
If the area overlaps the kernel or the RAM disk (little chip RAM), a 1 KB ring in the BSS is used and the log does not
survive a reset (the kernel says so). Whether chip RAM survives a warm reset on the PiStorm is not yet known; in
FS-UAE (Kickstart 3.1, keyboard reset) it does (`tools/diag-test.sh`).

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
WORDSYNC on read, precompensation from cylinder 40 on write) into a chip-RAM buffer. `mfm.h` codes and decodes
AmigaDOS tracks (11 sectors of 512 bytes, odd/even bit blocks, XOR checksums); one decoded track is kept as a cache,
with a mask of which of its sectors are valid. Waits use `hr_time()` (the CIA-B E-clock counter, section 2). The
100 Hz tick turns idle motors off after 3 s. System calls 76 `SYS_fd_info` and 77 `SYS_fd_rw`, for the disk server
only: `diskd` offers the drives as `df0`-`df3` (df0 probed at start, the others on first use); `dd` copies to and
from them. The hardware rules it follows (HRM 3rd ed. p.244-248 and Appendix H; the numbers marked "hand-off" come
from the hardware hand-off note, 2026-10-23, and are not in the public manuals):
- **A read** is 6656 words (a bit more than one revolution) after the sync word; the buffer is cleared first, so a
  DMA that did not run cannot leave an older track to be decoded again. A sector whose header begins the buffer with
  no sync in front (the DMA started inside the first sync word, about 1 read in 1000: hand-off) is still found. Up to
  10 tries, a full recalibration before every 4th (hand-off); good sectors are kept from try to try, so a weak sector
  fails only the requests that need it. The disk-block flag must come within 300 ms for the sync (hand-off) plus the
  transfer time, not 2 s.
- **A write** reads the track (all 11 sectors must be good, unless the request covers the whole track), replaces the
  sectors, writes the track and reads it back to compare. The written track is 836 words of MFM zeros, the 11
  sectors and one filler word, 6821 words: longer than one revolution at the slowest drive speed long-proven drivers
  allow for (6000-6812 words, hand-off), so no strip of the old track survives; the filler word ($AAA8, or $2AA8 after
  a 1 bit) is there because Paula loses the last three bits of every write (HRM p.247; the filler word is hand-off).
  4 ms after the write before the DMA stops (the HRM's 1.3 ms; some drives need 4, hand-off).
- **Heads**: 3 ms a step, 18 ms when the direction changes, 15 ms to settle (HRM p.244); 1.5 ms after any side or
  select change before a DMA (hand-off; the HRM's 100 us gave errors), and the side line is never flipped just before
  a write. Recalibration (hand-off): if track 0 is already signalled, step inward until it goes off, then outward at
  4 ms a step (the signal needs the extra millisecond) for at most 100 steps.
- **Disk change**: the change line is read with the drive selected, 20 us after the select (one drive make needs about
  12, hand-off), at the start of every request and after every DMA. When it is active the cached track and the head
  position are forgotten and the request fails with a log line; every later request on that drive fails until the
  disk server opens it again (`fd_info`, called on every open of `dfN`), so a swapped disk never receives the old
  disk's sectors. No disk at all fails at once instead of after the read retries.
- After the reset warning's flush (section 11, keyboard) no new track write starts.
Tests: `tools/mfm-test.sh` (host: the filler word, losing its last 3 bits costs no sector, a read with no sync in
front), `tools/floppy-test.sh` (FS-UAE, two drives, part of `make check`: whole-disk reads, a write, its read-back, a
whole-disk read after it and the host's image file). FS-UAE does not model the lost bits, the drive timings or the
change line's delays: those are read, not verified. The "whole-disk read of df1 came back wrong" of 2026-10-22 was
FS-UAE, not the driver: it had kept an earlier run's writes in an overlay file (`Save States/test/df1.sdf`) and laid
them over the fresh image; the test now gives FS-UAE a save-state directory of its own.

### SD card driver (`kernel/emmc.c`)

The Pi 4/CM4 SD slot is a standard SDHCI controller (EMMC2) at `hwinfo.emmc`. Registers are little-endian
32-bit and always accessed whole; the data port is read and written raw (no byte swap). The driver polls (no
interrupts, no DMA): power-cycles the card and sets the controller clock through the VideoCore mailbox, resets
the controller, runs the SD initialisation (CMD0, 8, ACMD41, 2, 3, 9, 7, ACMD6 for the 4-bit bus), reads the
card size from the CSD, and transfers 512-byte blocks by PIO (CMD17/18/24/25, automatic CMD12; 25 MHz).
Every stage logs with `kprintf`. Only SD cards (CM4 Lite) are handled, not eMMC. The mailbox message is the one
DMA-like access (the VideoCore reads it from RAM), so its cache line is cleaned and dropped with the 68040
`cpushl`/`cinvl` that Emu68 translates.

### Updates (DESIGN D15, docs/d15-self-updates.md)

Zenix updates itself from a **bundle** (`.zup`) that reaches the boot partition. The version lives in one place,
`kernel/version.h`: a number that orders versions (`YYYYMMDDNN`, the date and a count within the day) and a string
for people; the kernel image carries both in a 24-byte record (`"ZVER"`, the number, the string, in
`kernel/syscall.c`). All formats below are frozen (CLAUDE.md): they only grow, at the end.

- **Packages** (`make packages`, `tools/mkzup.py`): `build/pkg/NAME.manifest` and `NAME.tgz`. The manifest is text,
  one item a line: `zpkg 1`, `name NAME`, `version NUMBER STRING`, `group base` (the packages that move as one),
  `depends NAME NUMBER` (at least that version), then the entries: `f MODE UID GID SHA256 SIZE /PATH` (a file),
  `c ...` (the same for a config file the owner may edit), `l TARGET /PATH` (a symbolic link), `d MODE UID GID /PATH`
  (a directory). Words a later format adds are ignored by older readers. A path is at most 55 bytes (with
  `.zup-new` it fits a system call's 63). The `.tgz` is a gzip'd ustar archive of the `f` and `c` files in manifest
  order. The standard packages: **zup** (`/sbin/zup`), **base** (`init`, `login`, `sh`, `mount`, the config files
  `/etc/svctab`, `passwd`, `group`, `fstab`, `motd`; `/etc/autologin` belongs to the test images and is never
  shipped), **utils** (every other command, the links such as `more`), **man** (the manual pages). The full root
  images (`zenix.hdf`, the SD card's root) carry the manifests of what they hold in `/var/lib/zpkg/`.
- **The bundle** (`make bundle`: `build/zenix-update.zup`), big-endian: a 96-byte header (`ZUP1`, format 1, header
  size 96, version number, version string[16], date `YYYYMMDD`, the lowest installed base version it applies to
  (0: any), channel[8], table-of-contents offset 96 and entry count (1-64), total size (at most 16 MB), flags 0, and
  at +64 the SHA-256 over header bytes 0-63 followed by the table), the table (80 bytes an entry: name[40]
  NUL-terminated, `boot/...` or `pkg/...` of `[A-Za-z0-9._-]`; offset; size; SHA-256), then the parts, each at a
  16-byte boundary: `boot/zenix` (the kernel; `mkzup.py` refuses one that does not carry the bundle's version) and
  `pkg/NAME.manifest` + `pkg/NAME.tgz` for each package. Every count, size and offset is range-checked by every
  reader (`tools/zup-host-test.sh` feeds each kind of damage).
- **The state file** `BOOT:Zenix/state` (zenixboot's, text): `zenixboot-state 1`, `version V`, `kernelsha HEX`, `phase
  installing|trial|confirmed|rejected`, `handovers N`, `lastboot N` (the newest klog boot number at the last
  handover), `end`; unknown lines ignored, one without `end` is not used. Written as `state.new`, flushed, `state`
  deleted, `state.new` renamed, flushed; a start that finds only `state.new` reads it. The files beside it:
  `zenix`, `zenix.new`, `zenix.old`, `zenix.rejected`, `zenix-update.zup` (`.part` while copied, `.bad`, `.done`,
  `.rejected`). The bootinfo fields: `+0x78` the bundle's address (16-aligned), `+0x7C` its size (0: none), `+0x80`
  flags (bit 0: going back, "Zenix (previous)").
- **zup** (`user/zup.c`, `/sbin/zup`, man/zup.txt) installs the packages of a bundle onto the root, as one
  transaction: `/var/lib/zpkg/journal` (text: `zupj 1`, `target VERSION T`, one line an operation - `OP LINK MODE UID
  GID SHA TARGET OLD PATH`, `-` where nothing applies; OP is `R` replace, `A` add, `D` delete, `l` new link, `L`
  changed link, `u` removed link; LINK `T.N` names the hard link of the previous file in
  `/var/cache/zpkg/prev-VERSION/f/` - then `end`; `commit` or `rollback` appended later) written and synced first,
  the root marked "needs a check" (`FS_MARK`), the previous files hard-linked, the new ones written as
  `PATH.zup-new`, read back against their SHA-256, owner then mode set, renamed over; then
  `prev-VERSION/log.T` (the same lines: the undo record), `commit`, the older prev sets removed, the journal removed,
  the root clean again. At the next start a journal is finished (every new file complete and checked) or rolled back
  (the hard links put back; a name already on the old inode is left alone); one cut while it was written (no `end`)
  had changed nothing. A journal `undo VERSION` is a rollback to the previous version (from the logs of
  prev-VERSION, last change first) and is repeated to its end after a cut. A newer `zup` package is installed in its
  own transaction first and the new `/sbin/zup -2` does the rest (transaction 2 of the same version). Config files
  (`c`) are replaced only when their SHA-256 is the installed manifest's (the owner never changed them), else written
  as `PATH.new`; one the owner deleted is not brought back; a file the new version no longer lists is removed (a
  config file never). The database is the installed manifests, `/var/lib/zpkg/NAME.manifest`; the one-line result
  goes to `/var/lib/zpkg/result`. Built for the PC with `tools/host/zup-host.c` (the same system calls on a directory,
  a power cut before any chosen change) for `tools/zup-host-test.sh`.

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
header, with one difference in its table (`tools/mkzexe.py --delta`, kernel.bin only; docs/d13-shared-code.md section
10): the relocations are a byte stream of distances from one relocated word to the next in 2-byte units (a byte 1-127;
bit 7 set: the top 7 bits of a 23-bit distance whose low 16 follow; 0 ends it), padded to 4 bytes, and `nreloc` is the
stream's length in longwords, so zenixboot's check of `nreloc * 4` against the file holds. `start.S` decodes it; it is
about a quarter of the plain table (2932 bytes instead of 11576 on 2026-10-26). `tools/mkzexe-test.sh` checks that
both tables give the same image and the same offsets. Programs declare their stack size in the header: 16 KB unless the
Makefile's `STACK_<program>` says otherwise (`sh` 24 KB, `gzip` and `edit` 32 KB; the numbers and how they were measured
are in limits.md). A guard pattern lies at the bottom of every stack (section 6). There is no shared-library format yet; programs are linked
statically against `user/libz.c` (with `--gc-sections`, so each takes only the libz functions it calls).

**ZEX2: programs with shared code** (DESIGN D13, docs/d13-shared-code.md). A program built for it has one copy of its
code and constants in memory for every process running it, as an AmigaOS pure program. Every object is compiled with
gcc's `-msep-data` through `tools/purecc.sh`, which rewrites `sym@GOT(%a5)` to `sym@GOT.w(%a5)` before assembling,
so each global is reached through the program's address table with a 16-bit offset from `a5` (`move.l (d16,a5)`; a
table over 32 KB is a link error). Such a program has its own builds of crt0 (`-DZEX2`) and of every part it links
(`NAME.po`), is linked with `link/pure.ld` and made by `make build/NAME.pzx`. The layout: code and constants first
(the shared part), then from `text_size` (16-aligned, `__text_end`) the data part every process has its own copy of:
the table `.got` first (`a5` points at its start: the offsets count from there, not from `_GLOBAL_OFFSET_TABLE_`),
`.got.plt` (12 zero bytes), `.data`, `.bss`. The header:

```
+0  bra.w entry   +4 'ZEX2'   +8 text_size   +12 data_size   +16 mem_size (data + bss)
+20 nrel_code     +24 nrel_data              +28 reloc_off   +32 stack_size   +36 reserved
code and constants (text_size)   data image (data_size)   nrel_code, then nrel_data link addresses of 32-bit words
```

The words to relocate all lie in the data image (stage 1 allows none in the shared part); those in the first list
point into the shared part (the loader adds the code's address), those in the second into the data part (it adds the
data's address minus `text_size`). `tools/mkzexe.py` classifies every relocation by the section of its target symbol,
never by its value: the table's slots through the `R_68K_GOT16O` relocations in the code that read them (a static
link lists no relocations for the table, so it adds one per slot). It refuses, naming the symbol: a relocation placed
in the shared part, a PC-relative reference between the two parts (shared code reaching data), a value equal to
`text_size` (it could be either part), an undefined weak or an absolute symbol, a table slot no relocation names, a
writable section in the shared part. `tools/mkzexe-test.sh` (host) checks this. The new magic means a kernel that
does not know ZEX2 refuses such a file as "not a Zenix program" instead of starting it wrongly. The programs built
into the kernel (diskd, fsd, zfsd, boot) stay ZEXE; a ZEX2 file is loaded only from a filesystem.

**Loading a ZEX2 program** (`proc_load`, `fill_v2` in `kernel/proc.c`; the segments in `kernel/seg.c`). The header is
checked as a ZEXE one is (`zex2_bad`: the shared part 16-aligned, holding the header and inside the file; the data
image and the two lists inside the file and in that order, without overlap and with no count that can wrap; memory at
least the data image and at most 16 MB; the stack size), and every relocation before the first is applied (`rel2_ok`:
its place a whole word of the data image at an even address, its value inside the part its list says). The process's
block holds only the data part (table + `.data` + BSS), the stack and the arguments; its code is a **segment**: one
copy for every process that starts the same unchanged file. A spawn looks for a listed segment of the file first and
takes a reference to it at once, before anything can block, after checking its checksum; else it reads a copy of its
own (and checks that its header is the one read first), and only when that copy is complete, checked and
checksummed is it listed (if another spawn listed the same file meanwhile, that one is used and this copy freed:
nobody waits on a half-loaded segment). Then both lists are applied (to-code words get the code's address, to-data
words the data address minus `text_size`) and the first register frame gets `a5` = the start of the data part (the
10th of the 11 words `switch_to` pops; `struct proc` keeps it in `a5`). A segment is listed under a key, (mount slot,
the mount's generation, inode, size, modification time), and is found only while nothing the kernel saw can have
changed the file (section 10, "Loaded code"). A copy that may not be listed - the file was open for writing at the
spawn, it may have changed during the load, the table of `NSEG` = 8 segments is full - is the process's own (a
private copy, freed with it). The process drops its reference in `proc_exit`; the last one frees the code at once.
The checksum (a 32-bit add-and-rotate over every long word of the code) is checked at every reuse (a mismatch: "exec:
PATH: its shared code in memory is damaged (checksum): loading a fresh copy", the segment leaves the list) and when a
process running shared code faults: the crash line gives `code+OFFSET`, and the next line says the code is intact, or
that it is DAMAGED, is not used again, and which processes still run it (nothing can stop them without an MMU).
`tools/pure-test.sh` (part of `make check`) checks all of this in FS-UAE with ZEX2 builds of `hello` and `crash`
(`/tests/hellopure`, `/tests/crashpure`) and 14 corrupt ZEX2 files. **Stage 1 installs `sh` and `login` as ZEX2**
(`PUREAPPS` in the Makefile: `/bin/sh` and `/bin/login` are `sh.pzx`, `login.pzx`, linked with pure builds of
crt0, libz, `pw.po` and `sha.po`); every other program and the built-in servers stay ZEXE. A program that sends raw
messages must stay ZEXE (limits.md); `sh` and `login` send none.

## 5. Memory (`kernel/mem.c`)

Nothing can be moved once handed out (no MMU, no paging), so fragmentation is the enemy. Rules:

- Chip and fast are separate **ranges**; each range is a run of contiguous blocks with a 16-byte header
  (size + used bit, size of the previous block, **owner pid**, a magic value `ZMEM` that catches overruns and
  double frees).
- Blocks of 16 KB or more are cut from the **high** end of a free block, small ones from the **low** end, so the
  two sizes do not interleave and fragment each other. `MEM_LOW1G` (the SD driver's mailbox message, which the Pi's
  VideoCore reads only below 1 GB) takes a block that ends below 1 GB, from the low end, also from a fast range that
  crosses 1 GB (audit A15; `tools/mem-test.sh`).
- Every block records its owner. `mem_free_owner(pid)` hands back everything a process held, which is how a
  crashed or killed process is cleaned up without any cooperation from it.
- Free-byte counters are O(1), so `free` and `SYS_meminfo` read them cheaply.
- `kmalloc`/`kfree` for kernel objects; `SYS_memalloc` gives processes extra regions (at most `NREG` = 8 at a time,
  each at most `MEMALLOC_MAX` = 64 MB). **Every memory call answers 0 (NULL) on failure, never -1.** `mem_alloc`
  refuses 0 and sizes above `MEM_ALLOC_MAX` before rounding, so no size near 4 GB can wrap into a tiny block.
- Every walk over the blocks (`alloc_in`, `mem_free`, `mem_free_owner`) checks each header: the magic value, a size of
  at least one header, a multiple of 16 and inside the range. A header that fails means something overwrote the heap:
  the kernel prints its address on the serial port and stops (`panic`) instead of looping on garbage.

The pools leave out low chip RAM (`0`-`0x1FFF`: vectors, boot tables), the kernel, the RAM disk and the kernel
log's area at the top of chip RAM (section 3, "Previous-boot log").
**An update bundle** zenixboot left in memory (bootinfo `+0x78` address, `+0x7C` size, `+0x80` flags; D15) is checked
at `mem_init`: 16-byte aligned, it and the 16 bytes before it inside one bootinfo region, clear of `0`-`0x2000`, the
kernel, the klog area and the RAM disk, and in free memory; else it is ignored with a message ("update: the bundle at
... is ignored: ..."). It is then **a block in use inside the pool** (`upd_reserve`: its header is the 16 bytes before
the bundle; a free remainder smaller than two headers is taken into the block), so `SYS_update RELEASE` gives it
back with the normal `mem_free` and it merges with its neighbours (no range slot: `add_range` keeps 16).

A process's own memory is **one block**: image + BSS + stack + the argument area (a ZEX2 program's: its data part,
section 4). The kernel stack (4 KB) is a second block. Both are owned by the pid and freed together. The code of a
ZEX2 program shared by several processes (a segment, section 4) is a block owned by no process: its owner is
`SEG_OWNER` (0x80000000, never a pid), so `mem_free_owner` of one of its users never frees it; the segment's
references do (`mem_set_owner` hands a loader's copy over to the segment when it is listed). The lowest 16 bytes of each stack hold the stack guard's
pattern (section 6).

## 6. Processes and scheduling (`kernel/proc.c`)

- A fixed table of `NPROC` = 32 `struct proc` (room for a login and a shell on each of the ten consoles, the servers,
  init and the commands they run). State machine: `FREE, READY, SLEEP, ZOMBIE, MSG, STOP, EV` (`WAIT` and `IO` keep
  their numbers for `SYS_procinfo`, which reports a process in `EV` as `WAIT` when it waits for a child and as `IO`
  otherwise, but are no longer entered: DESIGN D16).
- **Priorities** (DESIGN D10): the highest ready priority always runs; processes of the same priority take turns, a
  slice of 5 ticks (50 ms) of the 100 Hz tick each. There is no ageing: a busy program keeps every worse one off the
  processor until it waits (the Exec rule).
  - **Levels and the nice scale.** 32 levels inside the kernel (`NLEVEL`), 0 the highest. Users see the Unix nice
    scale, -20 (most favoured) .. 19, and nothing else; the kernel maps it once, `level = (nice + 20) * 4 / 5`
    (`NICE_LEVEL`): nice -20 and -19 are level 0, nice -10 is 8, nice 0 is 16, nice 10 is 24, nice 19 is 31 (five
    nice values share four levels). The idle process (pid 0) runs below every level (`PRI_IDLE`, 32).
  - **Base and effective.** Each process keeps its base (`nice`, with its level `blevel`) and its effective level
    (`prio`), which is the base or, while it serves a better client, that client's level (priority inheritance,
    section 8). `ps` shows both: PRI the effective level, NI the nice value.
  - **Rules.** A child inherits its parent's nice (not a server's boost). Anyone may lower the priority of their own
    processes (`nice`, `renice`, `setpriority`); only root may raise one, or change another user's; the built-in
    servers are out of reach of non-root (the kill rule, section 14). diskd, fsd and zfsd start at nice -10
    (`NICE_SERVER`, kmain), so a busy program never delays the disk and the filesystems everyone waits for; boot,
    init, login, the shells and their programs run at nice 0.
  - **Run queue.** One list per level and a 32-bit mask of the non-empty levels (level L is bit 31 - L), so the
    68020's `BFFFO` gives the best level in one instruction (implemented in hardware on every CPU up to the 68060; it
    is not on the 68060's list of unimplemented integer instructions). O(1). A process joins its level at the back; a
    queued process whose effective level changes moves to its new level's list (a short walk of the old one).
  - **Switching.** The kernel is still never preempted, so it needs no locks; where an interrupt path can touch the run
    queue (`ready()`), interrupts are masked briefly. Every tick counts the running process's slice, whether it cut
    into user code or into a system call (before D10 only user-mode ticks counted, so a program that spent most of its
    time in system calls kept the processor for many slices; the fairness test found it). The end of a slice, a
    process of a strictly better level becoming ready (`ready()`: a reply, a wake-up, input in `kwork`, a child's
    exit) and a sleeper falling due (the tick compares `wake_min`) all only set `need_resched`; `resched()` acts on it
    at the next return to user mode - the end of every system call, and the level-2 interrupt when it cut into user
    code, the places where `kwork()` already runs. A better level switches at once; at the end of a slice the next
    process of the same level gets its turn (the running one goes to the back), and with nobody of its level waiting
    it starts a new slice. A process of the same level that becomes ready waits for the end of the running one's
    slice, so equal priorities alternate only there.
  - **Sleepers.** `schedule()` still scans the process table for due sleepers (the ordered timer list is a later
    item of future-ideas.md), but only once one is due: `sleep_until()` keeps `wake_min`, the earliest wake-up, and the
    scan readies every due sleeper onto its own level and finds the next one.
  - Verified in FS-UAE by `tools/prio-test.sh` (in `make check`): equal priorities take turns in whole 50 ms slices,
    a higher priority keeps the processor, a woken higher priority runs within a tick, inheritance (section 8), and
    the nice/renice rules for a user and for root.
- **Events** (DESIGN D16, `kernel/event.c`, docs/d16-events-and-signals.md). Exec's model underneath: every process has
  fixed event bits pending and bits it waits for (`EV_IO` an object it registered on changed, `EV_SIG` a signal that
  runs a handler, `EV_CHILD` a child ended or stopped, `EV_CON` init's console requests); each object a process can
  wait on - a tty, a pipe's two ends, `/dev/ttyS0`, a port - carries a 32-bit mask of the waiting process slots
  (`NPROC` is 32), so registering is one OR. A waker sets the bit and readies a process only if it is in the
  event-wait state (`P_EV`, or `P_SLEEP` with bits set: a timed wait) and waits for that bit (rule 1); a stale bit in
  a mask (a process that left or died) does nothing, because every wait checks again in a loop. A wait goes in
  Exec's order: register on the objects and clear the bit, check readiness with interrupts on, then with interrupts
  masked only test the bits (and that no kill, stop or handler is due) and block (`ev_block`). Every producer -
  `tty_input`, `tty_setmode`, `tty_inject`, `serial_work`, the pipes and `file_close`, a child's exit or stop (to its
  parent), `con_switch`, `ipc_enqueue`, `sig_send`, the alarm - runs in process context and sets the bit under the
  same mask, so no wake-up is lost (`tools/signal-host-test.sh` tries every interleaving). Blocking `read`, `write`
  and `wait` wait this way, so the old `wake_io` (which woke every process waiting for any I/O) is gone.
  `SYS_evwait` waits for several at once: up to 32 items { fd, events, revents } (Linux's `struct pollfd` and bits)
  and a timeout in ticks (-1: none, 0: only look); an item with `POLLPORT` names a port the caller owns (`POLLIN`: a
  message is queued). It returns how many items are ready, 0 at the timeout, -EINTR when a handler is due (never
  restarted). ttys and `/dev/ttyS0` are always writable (the tick empties their send rings); files and `/dev/null`
  never wait; a pipe's read end gives `POLLHUP` when no writer is left, its write end `POLLERR` when no reader is.
  libz's `poll` (milliseconds) and `select` (`fd_set` of 32) are built on it.
- **Signals** (DESIGN D16, `kernel/signal.c`). Linux's numbers and default actions (frozen): HUP 1, INT 2, QUIT 3,
  KILL 9, USR1 10, USR2 12, PIPE 13, ALRM 14, TERM 15, CHLD 17, CONT 18, STOP 19, TSTP 20, TTIN 21, TTOU 22, WINCH 28;
  the default ends the process (wait status 128 + the number: 143 TERM, 137 KILL, 141 PIPE, 142 ALRM), stops it
  (STOP, TSTP, TTIN, TTOU), continues it (CONT) or does nothing (CHLD, WINCH). KILL and STOP are never caught,
  blocked or ignored; KILL acts at once (as a second kill). Process 1 (`boot`) is never ended or stopped by a default
  action (the old form of `kill` still ends it for root). Per process the kernel keeps the pending, blocked, caught,
  ignored and restart masks, one `sa_mask` per signal and **one entry point** in the program (Exec's exception
  code): libz keeps the table of handlers, the kernel only knows "caught". A spawned program keeps the ignored and
  blocked signals of its parent and starts with the default for the caught ones (as fork and exec). A caught,
  unblocked signal is pending until the process next returns to user mode; there, as the very last step of a system
  call and of the level-2 interrupt when it cut into user code (whose entry now saves all 15 registers, like the
  system call's), `sig_deliver` first acts on a kill and a stop, then writes a frame on the user stack (84 bytes:
  size, d0-d7, a0-a7, PC, condition codes, signal, blocked mask; refused - "stack overflow in a signal: killed", 139 -
  if it would leave less than 512 bytes above the stack guard) and sends the process to its entry (d0 the signal, a0
  the frame, a5 its D13 data base); the signal and its `sa_mask` are blocked while the handler runs. `SYS_sigreturn`
  takes the frame back after checking it lies in the process's memory before reading it, that the PC is even (else
  the process is killed, 139) and taking only the condition codes of the status register, never its upper byte (user
  mode, interrupts on). Only a signal that will run a handler interrupts a blocking call (-EINTR): tty, pipe and
  serial reads and writes, `sleep`, `wait`, `evwait` and a server's untimed receive; a write that moved bytes returns
  the count; a client in a server call gets it after the reply; the service manager's waits are never cut.
  `SA_RESTART` restarts the call after the handler (the trap again: PC - 2, d0 the call's number), never `sleep`,
  `evwait` or a timed receive. `SYS_alarm` is a kernel deadline counted with the sleepers (`ntimer`, `wake_min`), so
  SIGALRM reaches a program that computes without system calls. SIGCHLD goes to the parent when a child ends or
  stops; a parent that ignores SIGCHLD has its children reaped by themselves and `wait` then gives -ECHILD. A write to
  a pipe with no reader sends SIGPIPE (ignored: -EPIPE). A process that exits sends its stopped children SIGCONT and
  SIGHUP (rule 12). Calls: `sigaction(sig, struct usigaction *)` (size, handler: 0 default, 1 ignore, else caught;
  mask, flags, entry; returns the old disposition), `sigprocmask(how, set, old *)`, `sigreturn(frame)`,
  `alarm(ticks)`; `kill`'s new form and the groups below.
- **Process groups** (DESIGN D16). `pgid` is inherited at spawn; a process with no parent (boot, the servers) is its
  own group; `login` and every interactive `sh` make their own, and the shell puts each job in one (its first
  process's pid). `SYS_setpgid(pid, pgid)` only on oneself or one's own children (a zombie child too), into its own
  pid or a group that exists. A terminal's foreground is a process group (`SYS_setfg(pgid, 1)`) or, in the old form,
  one process; ^C sends SIGINT and ^Z SIGTSTP to every member, so every stage of a pipeline gets them; when nobody is
  in the foreground ^C still sets the tty's `intr` (`SYS_ttyintr`). `SYS_kill(pid, sig, 1)` is the new form (pid 0:
  the caller's group, -g: group g, sig 0 only asks): 0 if at least one process got the signal, -EPERM if none was
  allowed, -ESRCH for nobody, -EINVAL for pid -1; the old form `SYS_kill(pid, 0, 0)` ends a process with status 130
  as before. `SYS_jobctl` (stop, continue) sends SIGSTOP and SIGCONT. `uproc` ends with the `pgid` (libz's
  `getpgid`).
- **Interrupts only queue** (audit B4). The keyboard and serial interrupts put raw codes and bytes into rings and
  set `kwork_pending`; everything else input causes (keymap, line discipline, echo, Ctrl-C/Ctrl-Z, console
  switches, scrollback) is done by `kwork()` in process context - see section 11. The same rule closes the
  lost wakeup (B5): a reader's "is there input? no: block" is kernel code, and `kwork`, the only thing that delivers
  input, runs only at a few chosen points, never between such a check and its block.
- pid 0 is the idle process: it runs on the boot stack and executes `cpu_idle()` when nothing is ready.
- **Stack guard** (future-ideas.md section 3, done as a fix 2026-10-23). Nothing stops a stack from growing past its
  bottom without an MMU, so four guard words (16 bytes, "ZSTK"...) lie at the bottom of every kernel stack, every
  user stack (just above the program's BSS) and the boot stack (pid 0). At every task switch the process leaving
  the CPU has both guards and both stack pointers checked (`stack_bad`), and the saved stack pointers of the one
  coming in are checked against its bounds; every system call and every tick that interrupts user code makes the
  cheap check (the user stack pointer in bounds, the guard's first word intact) and the full one if that fails; a
  program that faults is told whether its stack had overflowed. A failure prints "[kernel] pid N (name) overflowed
  its user stack: killed" (or kernel stack) and kills the process (exit 139, as for a fault; a blocked one is woken
  so that it dies at once); pid 0's kernel stack, or a saved kernel stack pointer outside its stack, is a panic, so
  the text is in the previous-boot log. It finds an overflow, it cannot prevent one: what was written below the stack
  before the check stays written (one frame for a program that makes system calls, up to one tick for one that does
  not). Cost, counted in the 68020 code: about 70 instructions per task switch, about 18 per system call and per
  tick. Verified in FS-UAE: `crash stack` (the matrix, every machine) recurses with a system call per level until the
  guard kills it and the system goes on; a `STACK_TEST` build (`tools/diag-test.sh stack`) damages a process's own
  kernel-stack guard (`crash kstack`: killed with the message, the system goes on) and pid 0's (`crash idle`: the
  panic, and its text in the next boot's previous-boot lines).
- **Shared code** (DESIGN D13, section 4): a ZEX2 program's processes run one copy of its code (a segment); each has
  its own data part, and `a5` set in its first register frame (`TRAP #0` and the interrupt handlers keep every user
  register; the kernel is not built with `-msep-data` and keeps `a5` by the C calling rules; `switch_to` saves it).
- **No `fork`.** Without an MMU a copy of a process cannot be given the same addresses. The only way to make a
  process is `spawn(path, argv, stdio)`: load a ZEXE, give it the parent's cwd and environment, and pass only file
  descriptors 0-2 (so a pipe's write end can't leak into a reader and prevent end-of-file). At most 256 arguments
  and 8 KB of argument text (`SPAWN_ARGC`, `SPAWN_ARGBYTES`, syscall.c; copied through the kernel heap, not the
  4 KB kernel stack); more is refused with -E2BIG.
- **Controlling tty** (`struct proc` `ctty`, audit E): a process's terminal is the console of its fd 0 at spawn
  when that is a console (init starting `login` on `/dev/ttyN`), else its parent's. `/dev/tty`, `SYS_ttymode`,
  `SYS_setfg` and `SYS_ttyintr` use it, so the second stage of `ls | less` on console 2 reads console 2's keys
  (before, a process whose fd 0 was a pipe fell back to console 1).
- Process exit closes fds, releases the tty, closes the process's ports (clients get an error), frees all
  memory owned by the pid, and wakes a waiting parent. A zombie nobody will wait for (its parent is gone or is
  itself a zombie) is freed by the idle loop, by every process exit and before a spawn looks for a free slot, so
  a busy machine that never idles does not fill its table with them.
- A process's environment (at most 1 KB, `ENVMAX`) is a block of its own memory, copied from its parent's at spawn
  and replaced by `SYS_envset` (since 2026-10-26; it was a 1 KB array in each of the 32 slots, 32 KB of kernel BSS on
  every machine). The handlers' `sa_mask` table (D16, 128 bytes) is taken from the kernel heap only by a program that
  sets a mask.
- Each process has 16 file descriptors; the system has 64 open-file slots (a slot's reference count is 16 bits)
  and 16 message ports.

## 7. System calls (`kernel/syscall.c`)

`TRAP #0`, call number in `d0`, arguments in `d1-d3`, result in `d0`. There are 86 system calls (the numbers and
a line on each are in `kernel/kernel.h`; numbers 1-89, of which 39, 40 and 68 are unused; the next free number is
claimed in CLAUDE.md first): files (`open close read write lseek readdir stat fstat lstat chdir getcwd mkdir unlink
rmdir rename link symlink readlink chmod chown utimes dup dup2 pipe sync isatty`, and `statfs`, which takes a mount
index), processes (`spawn wait exit yield sleep getpid kill jobctl procinfo getpriority setpriority setpgid`),
events and signals (`evwait sigaction sigprocmask sigreturn alarm`, section 6), the machine (`reboot`,
`update`), services (`svc`), memory (`memalloc memfree meminfo`),
time (`ticks hrtime time settime`), terminal (`ttymode ttyinfo ttyintr setfg ttymirror`, and `console`, the
display's colours: D14 note section 11), environment (`envset
envget`), identity (`getuid getgid geteuid setuid setgid umask`, section 14), messages (`port_create port_find call
recv reply replyrecv`), mounts (`mount umount mountinfo`), the kernel log and input (`klog input`), and privileged
ones, refused (-EPERM) to all but privileged processes, i.e. the disk server (`blk_read blk_write blk_flush blk_size blk_ram fd_info fd_rw`).

**Every user pointer is validated** against the process's own span and its `memalloc` regions. A bad
pointer makes the call return -EFAULT; the process lives. Since D13 there are two checks: `user_ok_wr` (its own memory
only) where the kernel, or a server answering for it, writes into the caller's memory - `read`, `statfs`, `mountinfo`,
`blk_ram`, `envget`, `lstat`, `readlink`, `readdir`, `stat`, `fstat`, `input`, `klog`, `getcwd`, `pipe`, `procinfo`,
`meminfo`, the message of `call` (the server answers into it), `svc`'s `SVC_LOAD` and `SVC_OPENS` - and `user_ok_rd`
(its own memory or its code: a ZEX2 program's string literals lie in its shared code) where it only reads - `write`,
`envset`, `mount`'s request, `svc`'s `SVC_SAVE`, every string copied in (`copy_str`) and `spawn`'s argument vector and
stdio array. So a literal path works, and a `read` into a literal fails with -EFAULT instead of changing the code every
process running the program runs. `blk_read` and `fd_rw` stay unchecked (privileged, as before). Raw messages a
program sends itself are not checked by the servers (limits.md): a program that sends them stays ZEXE.
**A struct written into a program never overruns its buffer** (D15 step 0, the frozen-interface rule in CLAUDE.md):
`stat`, `lstat`, `fstat`, `readdir`, `procinfo`, `statfs` and `mountinfo` take the caller's buffer size in their third
argument, `meminfo` in its second (libz passes `sizeof`). Every program built before this passed 0 there, and 0 means
the struct's first size - a frozen constant in kernel.h (`USTAT_FIRST` 28, `UDIRENT_FIRST` 72, `UMOUNTINFO_FIRST` 88,
`UUPDATE_FIRST` 32, `UPROC_FIRST` 32, `USTATFS_FIRST` 20, `UMEMINFO_FIRST` 16), checked against the structs' first
fields by `_Static_assert` and by the host test `tools/usize-test.sh` (audit3 B3: four calls passed `sizeof`, which
would have grown with the struct); a size below it is -EINVAL; a bigger one gets at most the kernel's own struct (`usize` in
syscall.c). The disk server does the same for `DISK_INFOSZ`/`DISK_LISTSZ` (size in `a[2]`; the old `DISK_INFO` and
`DISK_LIST` write the first 80 bytes of `struct diskinfo`, their callers never set `a[2]`). Tested by `badptr` (memtest):
an old 40-byte `uproc` buffer with a guard after it gets 32 bytes; the old kernel wrote 44.
`SYS_procinfo` fills `struct uproc` (32 bytes at first: pid to name; D10 appended `nice` and `pri`, D13 `shared`; a
caller passing size 0 gets the first 32): `mem` is the memory the
process owns (since D13 its `memalloc` regions and a private code copy too; before, only its load block - image,
data, stack: an accepted one-time change of meaning, audit3 B7 and CLAUDE.md "Frozen interfaces"), `shared` the bytes of shared code it runs (`ps` shows them as
SHR). `SYS_meminfo` fills four words: chip free, fast free, the bytes of shared code in memory, and what sharing saves
(for each segment, its size times its users beyond the first); `free` prints the last two. Strings are copied into kernel buffers with bounds; a path
argument longer than 63 bytes is -ENAMETOOLONG.

**Errors are negative errno numbers** (audit E), the Linux/POSIX values in one header, `kernel/errno.h`, shared by
the kernel, the servers, libz and the host tests: -ENOENT, -EACCES, -EEXIST, -ENOTDIR, -EISDIR, -EINVAL, -ENOSPC,
-EROFS, -ENAMETOOLONG, -ENOTEMPTY, -EXDEV, -EBUSY, -EBADF, -EFAULT, -ENOMEM, -E2BIG ... The filesystem servers'
answers pass through the kernel unchanged, and both servers return the same number for the same case (section 9).
libz keeps the last one in `zerrno`; `zwhy(dflt)` and `zstrerror(e)` give its text. A few places still answer a
bare -1; limits.md ("Error numbers") lists them. A permission refusal is always -EACCES (13), also for chmod/chown by a non-owner (Linux says
EPERM there).

**Reboot**: `SYS_reboot(note)`, root only, does what the reset warning does (every filesystem synced, disk writes
stopped; each server has 5 s to answer its `FS_SYNC`, then the tick resets the machine anyway and logs the server's
name, audit A7), notes "rebooted" in the log for the next boot (`dmesg -p`), and resets the machine as exec's ColdReboot does
(RESET and JMP to the ROM's start in one long word). A note of 1 (AmigaOS) or 2 (Zenix) is first left in the spare
bytes of the log area's first header (`BOOTNOTE_*`, `kernel/kernel.h`) for the boot menu (`man/bootmenu.txt`).

**Updates** (D15, section 3 "Updates"): `SYS_update(op, a, b)` (82). `UPD_INFO` fills `struct uupdate` (32 bytes, frozen,
at most the size given in `b`: the bundle's address and size - 0 when there is none -, its flags, the running kernel's
version number and string from the `ZVER` record; anyone); `UPD_RELEASE` gives the bundle's memory back to the pool
(root; `boot` calls it after zup, whatever zup did); `UPD_NOTE` (root) writes the update note (kind 1 done, 2 failed,
3 confirmed, 4 confirmed but the root half failed - what a 3 over a 2 or 4 of the same version becomes -, and a
version) at klog area +0xE0 for zenixboot (section 3, "Kernel log"). `uname` prints the version
from it. The bundle is read in place and never passed to a system call: it is no process's memory (-EFAULT). Test
builds only: `DIAG=-DDIAG_TRIAL=1` panics instead of confirming (and the panic resets the machine), `=2` resets
without confirming, `=3` never confirms (tools/update-test.sh); `UDIAG=-DDIAG_SLOWZUP` (the programs' flags) gives
boot a 4-minute limit for zup and makes a zup started as the second stage hang (update-test's `slowzup`).

**Priorities** (section 6): `SYS_getpriority(which, who)` and `SYS_setpriority(which, who, nice)`, POSIX's calls.
`PRIO_PROCESS`: who is a pid, 0 the caller; `PRIO_USER`: every process whose real uid is who (0: the caller's);
`PRIO_PGRP` is -EINVAL (there are no process groups). getpriority answers 20 - nice (1..40, the best of the matching
processes, as Linux's system call does), so no nice value looks like an error number; libz's `getpriority()` turns
it back into the nice value (-1 with `zerrno` set on failure, as POSIX). setpriority clamps to -20..19; a value below
the process's present nice needs root (-EACCES), another process needs the kill rule (section 14); with `PRIO_USER`
every process is checked before any is changed.

`kill` sends a signal (section 6, DESIGN D16); its old form, and a signal's default "end it", go through
`proc_kill` (`proc->killed`, D11's rules for servers). **Stop and continue** (job control): SIGTSTP (Ctrl-Z on a
terminal, `kernel/tty.c`), SIGSTOP or `SYS_jobctl` (pid, 0 = stop, 1 = continue: SIGSTOP and SIGCONT) call
`proc_stop`, which sets `stop_req` and wakes the process if it is blocked; at its
next return from `schedule()` it turns itself into `P_STOP`, tells a waiting parent, and is not scheduled again until
`proc_cont`: no other wake-up runs a stopped process (`ready()` leaves it stopped; only `proc_cont` and `proc_kill`
end a stop, DESIGN D16 rule 2), so a server stopped while it waited in receive does not answer a message that arrives
meanwhile. `SYS_wait` with flag 2 (`WAIT_UNTRACED`) reports a stopped child once, as `ST_STOPPED` (0x114); flag 4
(`WAIT_CONSOLE`, root only: init) also returns `ST_CONSOLE` + n (0x200 + n) when console n was shown with nobody on it,
so init starts a login there (section 11). A client
inside a call to a server is never stopped until the reply arrives (the server holds pointers into its memory). A stop
the process asks for itself (a SIGTSTP handler raising SIGTSTP) is acted on at the end of that system call.
`sleep` loops until its time, so a stop and continue does not cut it short (a handler does: -EINTR).

## 8. Ports and messages (`kernel/ipc.c`)

The central mechanism (DESIGN D1). A **port** is a named queue owned by one process. A client calls

```
struct umsg { next, from, server, state, result, type, a[4], uid, gid, umask, kflags, client }
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
- `recv(port, wait)`: wait 0 returns at once (0 if nothing is queued), 1 waits for a message, N > 1 waits at most N-1
  ticks and then returns 0 - a server with a deadline (ZenixFS's commit three seconds after the first change) sleeps
  until a message or the deadline, whichever comes first, instead of polling. A server that installs a signal
  handler (DESIGN D16) gets -EINTR from an untimed `recv` when a handler is due (restarted with `SA_RESTART`); a timed
  one is never cut. A port's owner may also wait for it among other things with `SYS_evwait` (`POLLPORT`).
- If a server dies, the request it was serving fails with -ECONNRESET ("the server is gone", errno 104; the kernel's
  file calls turn it into -EIO for programs). A service's port (section 9, "The service manager") goes back to
  *held*: its queued requests wait for the next server, which the first of them starts. Any other port is closed, its
  queued requests fail with -ECONNRESET, and so does a later call to it (a closed port, or a port number whose slot
  was reused); the kernel forgets a cached port number and looks it up again next time. A call to one's own port is
  -EINVAL. `ipc_reply` looks the message up
  among the requests really in flight (the clients' `wait_msg`) before it reads anything in it, so a stray or forged
  pointer from a server is refused (-EINVAL) instead of being followed.
- The kernel stamps every message with the caller's uid, gid and umask, and `kflags`: `UMSG_KERNEL` when the kernel
  itself sends it for a system call (`ipc_kcall`, used by vfs.c), 0 for a program's own `SYS_call` (`ipc_call`
  always clears it). The filesystem servers answer only messages with `UMSG_KERNEL`, or from root; the disk server
  only root (and `DISK_LIST`/`DISK_INFO` for anyone). A registered service name (`disk`, `fs`) is created only by the
  process the kernel started for it: anyone else, root too, gets -EBUSY, also after a crash. The names `fs.*`
  (mounts) not registered as services are reserved: only root may create them (`port_create` answers -EACCES), so a
  dead server's name cannot be taken over by a normal user. Audit D, D11.
- 28 port slots: the first 12 are the held ports of the services (one per entry, never given to another port), the
  other 16 ordinary ones, so creating ports until the ordinary ones run out cannot stop a service.
- **Priority inheritance** (DESIGN D10, after QNX). A server runs at least at the level of every client it holds:
  `call()` raises the port owner to the caller's effective level when the message is queued (so a server with several
  queued clients serves at the best waiting one's level), and `ipc_reply()` (also the reply half of `replyrecv`)
  drops it back to its base, or to the best client still queued for it or still being served by it
  (`ipc_best_client`). Only a boosted server recomputes, so a reply costs a compare when nothing was raised. The
  boost passes along a chain: a raised process that is itself a client in a call (fsd calling diskd for a client)
  raises the server of its request, up to a depth of 8. So a low-priority program cannot hold up a high-priority one
  through a shared server: a nice 0 client of a nice 10 server is not delayed by a busy nice 5 program (`priotest
  inherit`; a test-only kernel built with `-DNO_PRIO_INHERIT` fails that check: the calls wait until the busy program
  ends).
- Drawback of the design, noted in DESIGN.md: a **hung** server blocks its clients (a kill ends it: below).
- **Kills and servers** (D11). A kill (not a fault) waits for the end of a message call; a process killed while it
  serves a request ends only between requests: `ipc_recv` refuses to take the next message, and the end of a system
  call ends it only when it serves nothing. So a kill never cuts a filesystem change in half. A second kill, or a kill
  still pending 5 s later, ends it at once (a server hung inside a request) - but not while the server is itself a
  client waiting in a message call it made (fsd waiting for diskd): no kill ends a process in a call, so such a
  server ends when its own call returns (a hung diskd keeps the fsd waiting for it too).

## 9. The servers

### diskd - the disk server (`user/diskd.c`, port `disk`)
Owns the hardware through privileged raw-sector syscalls (`blk_read/blk_write/blk_size/blk_ram`, only for
processes flagged `priv`). Reads the Amiga **RDB** partition table (`RDSK`/`PART` blocks, DosEnvec geometry) and
serves named devices: `ide0` (the whole disk - IDE or the Pi SD card), `ide0p1`, `ide0p2`... (the partitions of an RDB, else of a PC MBR: type 0x83 = ext2, type 0x76 = an RDB inside, others listed and never mounted), and `ram0` for the boot RAM disk. Handles are
confined to their partition (LBAs are partition-relative). RDB partitions are placed by their own DosEnvec geometry
(`user/rdb.h`). A handle is a device's slot, kept across rescans (same sectors, same medium), so a mounted server's handle
never moves. Devices that share sectors overlap (the whole disk and its partitions, a 0x76 container and the RDB inside,
overlapping partitions of a damaged table): `DISK_MOUNT` and `DISK_RAW` (tools: `fdisk`, `mkfs.ext2`, `fsck.ext2`) are
refused while an overlapping device is mounted, and a write touching a device another process has mounted is refused; a
plain open reads. A mount whose server has died is released when it is in the way (checked with `SYS_procinfo`).
`DISK_FLUSH` makes earlier writes durable (`blk_flush`: ATA FLUSH CACHE on IDE, SYNCHRONIZE CACHE (10) on the A3000; the SD
driver waits after every write until the card has programmed it: CMD13 until READY_FOR_DATA and the card state is out of
"prg", as Linux checks - some cards report ready for data while still programming). `struct diskinfo` carries capability bits and the largest
transfer; `DISKCAP_FLUSH` is set only if a flush tried at start worked, and `DISK_FLUSH` answers -95 on a device without it.
`FS_SYNC` and `FS_QUIT` (umount) end with a `DISK_FLUSH` in both filesystem servers. Protocol: `kernel/diskproto.h`.
**Across a restart** (D11): whenever its table changes (a claim by `DISK_MOUNT`, `DISK_UNMOUNT`, a rescan, a floppy
probed, a dead server's claim released) diskd leaves the kernel a small record (`SVC_SAVE`: per slot the medium, start,
size, name and claiming pid; the floppy drives looked at; a checksum; 426 bytes), before it replies. A restarted diskd
reads it back (`SVC_LOAD`) before it opens its port, checks it like disk data, puts every device back into its old slot
with its claim (if the claiming process still lives) and then rescans as usual: old handles keep naming the same
devices and mounted devices stay protected. A damaged record is never guessed at: diskd stops (exit status 86) and the
kernel panics. The holder of a claim may send its `DISK_OPEN` with `DISK_MOUNT` again and gets its handle back. The
filesystem servers send a request that failed with -ECONNRESET (diskd ended while serving it) once more
(`user/diskcall.h`): repeating a sector read or write is safe.
Only root's messages (`m->uid` 0, stamped by the kernel) are served, except `DISK_LIST` and `DISK_INFO`: raw sectors
would bypass every file permission. The filesystem servers, `fdisk`, `mkfs.*`, `fsck.*` and `dd` on a device run as
root; for anyone else they answer "permission denied" or cannot open the device (audit D).

### fsd - the filesystem server (`user/fsd.c` + `kernel/ext2.c`, port `fs`)
The ext2 code compiled to run in user mode (`user/fsenv.h` supplies its environment), speaking the protocol in
`kernel/fsproto.h` (`FS_OPEN, FS_READ, FS_WRITE, FS_STAT, FS_READDIR, FS_MKDIR, FS_RENAME, ...`). Files are named
by **inode number**; the kernel's open-file table stores the inode, offset and flags. The server counts opens: every
successful `FS_OPEN` (also the kernel's own, to load a program) is matched by one `FS_CLOSE` when the kernel lets go of
the inode (the last close of the file, or the program is loaded). An inode whose last name goes while it is open keeps
its blocks until its last `FS_CLOSE` (so an old handle never reads or writes a file that reused the number); after a
crash such an inode is left with no links and `fsck.ext2`/`e2fsck` free it. ZenixFS makes it an orphan
(`zenixfs_orphan_add`), which the next mount frees after a crash. Started as `fsd DEVICE PORTNAME [-root] [-id ID]`: it
serves DEVICE on port PORTNAME; with `-root` (the root's server, registered by boot) the volume must be one Zenix can
**write** (see "ext2: what is mounted" in section 10; an ext3, ext4 or damaged volume is never the root), and it
prints as `fsd` (a mount's server prints as its port name, `fs.ide0p2:`); `-id START,SIZE,UUID` is the volume id boot or `mount` read (`user/volid.h`): at every
start the server looks for the device at that place on the same medium (a partition renamed by `fdisk` is found
under its new name) and serves it if its superblock's uuid matches or the recorded one is all zero; only if nothing is
at that place, the one device carrying that uuid (two carriers are refused); else it ends ("the volume is on no
device"). A floppy without a uuid is not served again after a restart (it may have been swapped).
**A restarted fsd** (D11 step 3) learns the inodes the kernel holds open on its mount (`SVC_OPENS`) before its
first request and rebuilds its open counts, so a deleted file held open keeps its data and frees its space at its last
close; if its predecessor ended in the middle of a request it clears the superblock's "cleanly unmounted" state bit
(Linux and e2fsck then check the volume; Zenix keeps writing) and says "DEV was being changed when its server
ended: check it with fsck.ext2".
**The clean mark (DESIGN D17, docs/d17-ext2-clean-mark.md).** On a writable ext2 volume fsd clears that bit (written
and flushed) before the first request that may write after a clean moment (`FS_WRITE CHMOD CHOWN UTIMES LINK SYMLINK
MKDIR UNLINK RMDIR RENAME`, `FS_OPEN` with `O_CREAT`/`O_TRUNC`, `FS_CLOSE` while an orphan is open; the request
fails with -EIO, changing nothing, if the mark cannot be written), and sets it again between requests 1 s after the
last such request (a timed `SYS_recv`), at `FS_SYNC` and at `FS_QUIT`, after a flush - but never while an open file's
last name has gone (an orphan), while zup holds the mark, or when the volume needs a check: it was not clean when this
server mounted it, a crash in a request, a failed block write, or damage met (`ext2_ndamage`). So the bit never says
clean while the volume holds a half-done change. `FS_MARK` (root only, D15): 1 holds the mark (zup, while it
installs), 0 lets it go; both answer 1 if the volume needed no check before (fsd's own short mark does not count),
and 0 never clears a real need. With `-fsck` (boot's check of the root) fsd applies the root's rules but serves the
volume read-only on a plain device handle, so `fsck.ext2` may open it raw. `zfsd` does not know `FS_MARK` (-ENOSYS):
ZenixFS commits atomically.
A mounted filesystem's server is the same built-in image, registered by `mount` (the mount table, below). It answers
`FS_ROOT` (root inode, device and type names) and `FS_QUIT` (umount: sync, release the device, exit). Two filesystem
servers are built into the kernel image, `fsd` (ext2) and `zfsd` (ZenixFS); there is no other copy of them. **Which one serves the root** is boot's choice
(`user/rootpick.h`): a RAM disk is the root; else the bootable ext2/zenixfs partition; else the first one of those;
else the bare disk; boot reads the device's superblock (ext2 magic at byte 1080, ZenixFS `ZNFS` at byte 32) and
registers `fs` with that server and device, so only one root server is ever started. `init` runs `mount -a` (from
`/etc/fstab`) before login.

### zfsd - the ZenixFS server (`user/zenixfsd.c` + `zenixfs/zenixfs.c`, port `fs` or `fs.DEVICE`)
The same library that the host tests run (`zenixfs/`, compiled with `-DZENIXFS_FREESTANDING`; `user/zenixfs_env.h`), over the
disk server (`user/zenixfs_disk.c`: filesystem blocks of 1-4 KB as 2-8 sector calls plus `DISK_FLUSH`). Same protocol and the same
permission rules as the ext2 server. Changes commit three seconds after the first one, at `FS_SYNC` and at `FS_QUIT`.
It takes the same `-root -id` arguments as fsd and prints as `zfsd` (the root's) or as its port name (a mount's). **A restarted zfsd** mounts the
last commit keeping the orphans (`ZENIXFS_MNT_READONLYISH`), rebuilds its open counts from `SVC_OPENS`, frees only
the orphans nobody holds (`zenixfs_orphans_keep`) and sets the next inode number to at least the committed one +
65536 and above every number the kernel holds (`zenixfs_ino_floor`, committed before the first request): a file
created in the seconds before the crash (not committed) is gone, its open file reads an error, and its number is
never handed to another file. The library
allocates only at mount (block cache, bitmaps, three scratch blocks), so a running server needs no more memory.

### The mount table (`kernel/vfs.c`)
Entry 0 is the root (port `fs`); `SYS_mount` puts a server's port on a directory (8 entries, the root
included). **Mounts are services** (D11 6.4): `mount DEVICE DIR` reads the device's volume id (as boot does),
registers `fs.DEVICE` -> `@fsd DEVICE fs.DEVICE -id ID` or `@zfsd ...` at nice -10 and calls `SYS_mount`, whose first
call to the port (`FS_ROOT`) starts the server in `mount`'s context; a mount whose server died is started again by
its next use, from the kernel image (no disk read). A failed mount removes its entry again. `umount` (`vfs_umount`)
removes the entry *before* it sends `FS_QUIT` (a no-start call), so the server's end is final; the held slot is free
again when the server's port closes, and a registration that finds its name or every slot still being given back
waits up to 2 s for it (an `umount` followed at once by a `mount`). A mounted filesystem is not essential: given up,
it stays mounted and answers -EIO until `umount`. A future filesystem type with no built-in image would be a file
service (`/sbin/fs.TYPE`), not built. With only the
root mounted, paths go to its server untouched. Otherwise the kernel makes every path absolute and normalised as text
(working-directory text + path, `.` and `..` removed; `path_abs`, the one normaliser, also used for the
working-directory text itself and for mount points; at most 159 bytes, `FS_PATHMAX` in fsproto.h, longer is
-ENAMETOOLONG and never cut: before, the working directory text was cut at 95 characters, so relative paths could
act on the wrong directory once something was mounted), picks the longest mount point that prefixes it and hands the server
the rest as a path inside that filesystem together with that filesystem's root inode as the working directory. Each
open file remembers its mount. `link` and `rename` across two mounts fail. `umount` is refused while a process has its
working directory or an open file inside, or another mount lies below. Caveat: because `..` is resolved on the text, a
working directory reached through a symbolic link behaves as its text says once a second filesystem is mounted.

### boot - the starter (`user/boot.c`)
pid 1, the only process the kernel creates itself. It registers `disk` -> `@diskd` (nice -10), picks the root (above),
registers `fs` -> `@fsd DEVICE fs -root -id ID` or `@zfsd ...` (nice -10), asks for the root inode (`SYS_mountinfo`
0: the first call to `fs` starts the root server), checks an ext2 root that is not marked clean (DESIGN D17: `fs` first
registered with `-fsck`, a read-only server that leaves the device to `/bin/fsck.ext2 -y DEVICE`, which boot runs
and waits for at most 20 minutes; then `FS_QUIT` to it and `fs` registered with `-root` again, so the next call starts
the writable server; the kernel forgets that a service served when its program text changes, so a writable root
server that fails to start is still D11's panic), runs `/sbin/zup`, registers the services of `/etc/svctab` (one `PORT NICE PROGRAM
[ARGUMENTS]` a line, man/boot.txt; a bad line is reported on console 1 and skipped), then spawns `/bin/init` on console 1 and waits for it; should init
end, it says so and starts it again. (It exists because the idle process cannot block on a message.) The logins on the
consoles are init's work (section 11, man/init.txt).

### The service manager (`kernel/svc.c`, DESIGN D11)
The full design is docs/d11-service-manager.md; what is built (steps 0-8 of its build order: the core, the mounts,
`/etc/svctab` and the servers' output in the kernel log):
- **A service** is a port name with a program: a built-in image (`@diskd`, `@fsd`, `@zfsd`, each with its own `priv`
  bit) or an absolute path, at most 6 words in 80 bytes, and a nice. `SYS_svc` (81): `SVC_REGISTER` and `SVC_REMOVE`
  (root), `SVC_SELF` (how many times the caller's entry was started). 12 entries (`NSVC`); `disk` and `fs` are
  *essential* (set by the kernel for these names).
- **Held ports.** A registered name always has its port, in a slot of its own (slots 0-11), with the same number
  (generation included) across restarts, so every cached port number stays right. Only the process the kernel started
  for the entry can take the port over (`port_create`); anyone else gets -EBUSY.
- **On demand.** A call to a held port with no server makes the *caller* the loader, in its own context (no loader
  process to deadlock with): an image is copied from the kernel (no disk), a file is read as root (the calls of a
  load carry uid 0). The new process has no parent (uid 0, `/`, empty environment, ppid 0: only root may kill it), the
  entry's nice, and its output in the kernel log (section 3, "Kernel log"). Callers that come meanwhile queue and wait. The loader's own message stays out
  of the queue while it loads and goes first once the server took its port.
- **The deadline.** A started server has 30 s to open its port, kept by a *watcher* (the loader, else the first
  queued client) with a timed sleep in its own context, never in the tick; one that misses it is killed and counted,
  and the queued calls fail with -ECONNRESET. A loader or watcher that is killed passes its role on.
- **A death.** The request in service fails with -ECONNRESET; the queued ones stay and the first of them starts the
  next server. An end with status 0 while nothing was in service is a *stop* (not counted); anything else is an *end*,
  logged and counted. The start after the 2nd, 3rd and 4th end within 60 s waits 1, 2 and 4 s; the 5th end within
  60 s gives the service up ("svc: NAME: given up after 5 ends in 60 s"): its port stays held and every call fails
  at once with -ECONNRESET until root registers it again (for diskd and the root a panic, "svc: an essential server
  ended 5 times in 60 s"). A server's start-up that calls its own port fails at once (it would wait for itself). A server that ends
  before it opened its port has failed to start: its queued calls fail. An essential server that ends before it ever
  served (or cannot be loaded) is a panic at once ("svc: an essential server ended before it served").
- **A saved table.** `SVC_SAVE` / `SVC_LOAD` (the process the kernel started for an entry only): up to 512 bytes a
  server leaves for its next start, kept in the kernel while the entry exists (diskd's slots and claims, above).
  `SVC_SELF` tells a server it is a restart (more than 1 start). An essential server that ends with status 86 (its
  saved table is damaged) is a panic, not counted.
- **Open files across a restart.** `SVC_OPENS` (the started server, after it took its port, before its first
  request): one inode number per open the kernel holds on that mount - every file entry of the mount (also one
  whose close is in flight), every `FS_OPEN` the kernel sent that was answered but not yet used by its client (a root
  program's own `FS_OPEN` makes no file entry and is not counted), every program load in progress (the loads are
  linked through the spawners' kernel stacks until their `FS_CLOSE`). `SVC_SELF` also says (bit 16) whether the last
  end came in the middle of a request. `umount` uses the same list (`mount_opens`), and also any kernel request still
  in flight on the mount's port: while any of them exists the mount is busy (audit A3: a load that went on after an
  `umount` read the inode of whatever volume was mounted next in the same slot).
- **No-start calls.** `FS_CLOSE` (from `file_close` and the end of a program load) and every call made by a dying
  process never start a server: dropped while the entry is stopped, queued while it is loading or starting.
- **The reset warning and `reboot`** (step 5): `FS_SYNC` and `FS_QUIT` are no-start calls that skip a server that
  is not running (it has nothing to write); a live one may still start diskd to flush, with no back-off: loaders
  asleep in a back-off are woken (`svc_hurry`). Once `disk_frozen` is set nothing is started any more.
- **Loads nest at most two deep** (a file service read while the root server is stopped); a third level fails with
  -ECONNRESET. Kernel stack: the deepest such chain is about 2.1 KB of the 4 KB (`-fstack-usage` sums, 2026-10-24).
- **The log** (`dmesg`): `svc: NAME: started PROGRAM (pid N, for pid M NAME)`, `svc: NAME: PROG (pid N) ended, status
  S[, during request T for pid C]`, `... stopped`, `svc: NAME: start K when next used`, `... did not open its port in
  30 s`, `... cannot start PROGRAM (error E)`.

### What the two filesystem servers answer (audit E)
The same errno for the same case: a missing name -ENOENT, an existing one -EEXIST (also `mkdir` of an existing
directory, and of `.`/`..`), a path through a non-directory -ENOTDIR, reading, writing or opening for writing a
directory -EISDIR (`FS_READ`/`FS_WRITE` on a directory inode too), `rmdir` of a non-empty directory -ENOTEMPTY,
`unlink` of a directory -EISDIR, a hard link to a directory -EPERM, `readlink` of a non-link -EINVAL, a name
longer than 63 bytes -ENAMETOOLONG, too many links followed -ELOOP, a full volume -ENOSPC, a read-only one -EROFS,
the disk or a damaged volume -EIO, any permission refusal -EACCES, an unknown request -ENOSYS. `O_CREAT` on a
symbolic link that points nowhere is -ENOENT in both (no second entry of that name; Linux would create the
target). What they still do differently, by capability: ext2 cannot move a directory to another parent (-EXDEV, so
`mv` copies it) nor rename over an existing directory (-EISDIR); ZenixFS does both.

## 10. The filesystem

- **ext2**, revision 0/1, inodes of 128 bytes or more, 1, 2 or 4 KB blocks, read **and write** (bitmaps, group
  descriptors, direct/indirect/double-indirect blocks, growing directories, rename, hard and symbolic links, fast
  symlinks). Little-endian on disk, swapped on read/write of metadata. Write-through block cache with an MRU/hash
  lookup and least-recently-used replacement; directory entries are parsed in place in the cache. **The cache's size
  by the machine** (the 2 MB diet, DESIGN D18): one region of memory, allocated at the first block read after a
  mount: with fast RAM 48 blocks (as before), without fast RAM 24 KB (24 blocks of 1 KB), never fewer than 12 blocks.
  Measured on the host (a session's requests: logins, `ls -l /bin` twice, ten program loads, 20 files written and
  removed; sectors read): 1 KB blocks 751 with 48 blocks, 943 with 24; 4 KB blocks 723 with 48, 1307 with 12 (3315
  with 8, hence the floor); without the program loads 65 and 85. The extra reads are programs read again that 48
  blocks still held. Before, each block was a separate `malloc`, whose growing chunks held 84 KB for 48 KB of
  blocks; fsd on the 2 MB A1200 without fast RAM went from 148 KB to 87 KB.
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
- **Rename survives a power cut** (no journal, so the order of the block writes does it, as Linux's ext2): the moving
  inode's link count is raised first, the existing target's entry is pointed at it in place (inode number and
  file-type byte, one block write) or the new entry added, the old entry removed, the count lowered, and only then the
  old target's inode dropped. A cut between any two writes leaves at worst a count one too high or a nameless inode
  (`fsck` fixes both), never a missing name. Tested on the host by cutting the disk after every block write
  (`tools/ext2-host-test.sh`, then `e2fsck -fn`). **The same rule for every request (audit 2026-10-26 F5): a power
  cut leaves only leaks**, never a block or inode in use marked free, a count below its names or a `..` naming a free
  inode: freeing a file's blocks (delete, truncate) writes the inode without them first and frees them in the bitmaps
  after, its number last; `mkdir` raises the parent's count before adding the entry; `rmdir` refuses (-ENOTEMPTY) a
  directory whose count says a subdirectory still points at it (one a cut left unnamed). What a cut can leave is a
  count one too high, an unnamed inode or directory, blocks or inodes marked in use that nothing holds, free counts a
  little off: space lost until `fsck.ext2` or `e2fsck` gives it back. Tested on the host by cutting the disk after
  each of an update's 2200 block writes (`tools/zup-host-test.sh` section 3, `tools/e2safe.py` sorts e2fsck's findings
  into leaks and the rest). Such a cut leaves the volume marked "not cleanly unmounted" (the clean mark, section 9,
  DESIGN D17): boot checks a marked root with `fsck.ext2 -y` at the next start, `mount -a` a marked volume of
  `/etc/fstab`, and `fsck.ext2` repairs every leak a cut leaves in one run (`tools/zup-host-test.sh`,
  `tools/ext2-host-test.sh`, `tools/rootcheck-test.sh`). ZenixFS's rename is one commit, and an old target that loses its
  last link is freed only after the new entry is in place (freeing a big file may commit on the way).
- Not done: moving directories between parents (rename answers -EXDEV, so `mv` copies), triple-indirect files, a
  journal.
- **Loaded code: a changed file is never reused** (DESIGN D13, `kernel/vfs.c` and `kernel/seg.c`). A shared code
  segment (section 4) is found only while the kernel has seen nothing that could have changed its file. Opening a
  file for writing (`O_WRONLY` or `O_RDWR`) fails with -ETXTBSY (26, "text file busy") while a segment of it is in
  memory or a load of it is in progress (with `O_TRUNC` the kernel asks the server for the file's status before the
  open, since the server truncates in it - only while something of that mount is loaded - and that check is the
  answer: a load that began between it and the open is marked stale and a segment listed meanwhile unlisted, never
  refused after the file is already emptied, audit3 B5; without `O_TRUNC` it checks the open's answer and closes it
  again); so a program is updated the Unix way, a new file renamed over the old name, and
  running processes keep the old code. A spawn of a file that is open for writing loads a private copy. unlink,
  rename, link and symlink are path-only in the kernel, so while something of that mount is loaded the kernel first
  asks the server which inode the affected name leads to (`FS_LSTAT`: the name removed, the one renamed over, a
  link's new name) and afterwards unlists that inode's segments and marks its loads stale (its number may be given
  to a new file); the rest of the mount keeps sharing (audit3 B2: every rm unlisted the whole mount, so the next
  shell loaded a private copy). The renamed file keeps its inode and contents, so its segments stay right; running
  processes keep theirs. Every mount, and every start of a
  mount's filesystem server by the service manager (`svc_handover`, `vfs_server_started`: a restarted ZenixFS server
  drops what it had not committed and could bring back older contents under the same inode, size and time), gives the
  mount a new generation, part of the key. Every spawn still opens the file through the server (execute permission,
  set-user-id). Not seen by the kernel: raw `FS_WRITE` messages from root (nothing sends them), and a raw write to a
  mounted device (diskd refuses it). The checksum finds damage to the copy in memory, not a stale but intact copy.
- Several filesystems can be mounted (`mount`, see section 9). **ZenixFS** (copy-on-write commits, checksummed
  metadata, extents) is the native one, served by `zfsd` and possible as the root: `zenixfs.md`.

## 11. Pipes, tty, terminal, display

- **Pipes** (`vfs.c`): 2 KB ring buffer in the kernel; EOF when all writers close; a write to a pipe with no
  readers sends the writer SIGPIPE, which by default ends it with status 141 (ignored: the write gives -EPIPE; DESIGN
  D16). A blocked reader or writer waits on the pipe's slot mask (section 6). Special files `/dev/tty` (the process's controlling
  tty), `/dev/tty1` .. `/dev/tty10` (the consoles; another console than one's own: root only, -EACCES), `/dev/console`
  (console 1, the same rule), `/dev/ttyS0` (the serial port, DESIGN D12: root, one owner at a time (-EBUSY); while it
  is open the console's copy, the kernel log's copy and the 0x1D hook stay off the wire and received bytes wait in the
  receive ring for its reader; a write waits for room in the 4 KB send ring a tick at a time, so the machine runs on,
  and ends on a kill or after 2 s in which the port took nothing (logged: what was written, or -EIO), audit A5),
  `/dev/null`, `/dev/zero`. `fstat` of a console gives its number in `st_ino` (the
  `ustat` has no device field; `login` uses it to apply `/etc/autologin` to console 1 only).
- **Virtual consoles** (owner's decision 2026-10-23: the text console behaves like Unix/Linux): ten, `/dev/tty1` ..
  `/dev/tty10`, each with its own text grid, scrollback and tty, allocated when first used. **Amiga+F1 .. Amiga+F10**
  shows console 1-10 (Linux: Alt+F1..); plain F-keys stay with the programs and send their escape codes; other
  Amiga+key combinations do nothing. The whole screen is text: no bar, no status line, no workspaces (those are
  desktop features, ROADMAP phase 5). `init` keeps a `login` on every console that has been shown, like a getty:
  console 1 at boot, console N the first time it is shown (`con_switch` sees nobody whose controlling tty it is,
  sets a bit in `con_want` and readies init, which waits in `SYS_wait` with `WAIT_CONSOLE` - Linux's autovt does the
  same), and a session that ends is followed by a new login. So a console costs memory only once it is used. **A
  login steps aside** (the 2 MB diet, DESIGN D18): Zenix has no `exec`, so the shell is a new process, and only a
  process that has become the user can start it as the user; init's login (`login --init`) spawns the shell and then
  ends through `SYS_exit` with its second argument naming the shell, which `proc_handover` makes init's child first
  (Linux's `CLONE_PARENT`, after the fact; a child that has already ended or stopped is posted to init at once). Its
  status, `ST_HANDED` (bit 30) plus the shell's pid, tells init the console's session goes on in that child; a
  logout is the shell's end. A session holds no login process (30 KB, and login's 13 KB of shared code once no
  console waits at a prompt); nothing is written to the disk (no utmp). `init`'s `waitcon` takes one ended child per
  call and answers its status: the one console whose process is no longer there (wait says -1) gets that status, the
  others' are asked for one by one.
- **tty** (`tty.c`): one per console, line discipline in the kernel: echo, backspace, `^U`, `^C` (SIGINT to the
  foreground: a process group or, in the old form of `SYS_setfg`, one process; nobody, or a group with no living
  member: `intr` is set for a shell that polls `ttyintr`), `^Z` (SIGTSTP the same way), `^D`; Tab is kept in the line (Backspace
  over it moves back one column on the screen; the line itself is right). A cooked line holds 511 characters
  (more than the shell's line editor, 478, so a line typed ahead for the shell is never cut here): a key past that
  rings the bell and marks the line too long, and at its end the whole line is refused with "tty: the line was too
  long ... not taken", never handed on cut; the mark outlives a switch to raw mode, so the rest of such a line typed
  after the shell's editor took over is refused too (audit 2026-10-26 F2: a 260-character line typed ahead lost its
  middle, `break` arrived as `ak`). `TTY_RAW` / `TTY_NOSIG` modes for editors and pagers. **Type-ahead is
  kept**: when a program switches the tty to raw mode (the shell's line editor after a command), what was typed
  meanwhile as a half line goes into the queue as it is (before, `tty_setmode` dropped it: `rm /etc/motd` typed
  during a slow command arrived as `c/motd`).
- **Terminal** (`term.c`): an 80-column text grid per console, as many rows as the display has (32 on PAL, 25 on
  NTSC: `trows`), plus 200 lines of scrollback, ANSI/VT100/xterm: cursor motion, erase, insert/delete, scroll regions,
  bold (pixel smear), underline, inverse; `ESC[6n` answers with the cursor position, as input on the console of the
  program that asked (it went to the console on screen, so a background program could type into another console's
  input: audit3 B9). Arrows and function keys
  arrive as escape codes. `SYS_ttyinfo` gives the size; the shell sets `LINES` from it and `TERM=xterm-16color`
  (xterm's keys and 16 colours; it was `vt100`; over a serial line the other side's getty picks `TERM`). Programs
  colour their output by one libz rule (`color_auto`: the output is a terminal, `TERM` set and not `dumb`,
  `NO_COLOR` not set): `ls` and `grep` (`--color=auto|always|never`, GNU's bytes), the shell's default prompt,
  `edit`'s status line; `less` passes colour codes through. **Colours** (DESIGN D14):
  each cell has a character, an attribute byte and a colour byte (foreground in the low nibble, background in the
  high one; grey on black is `0x07`). SGR `30-37`/`40-47`, `90-97`/`100-107`, `39`/`49`; `0` and a bare `ESC[m` reset
  everything (so does `ESC c`); `1` is bold and never changes the colour; `38;5;n`/`48;5;n` (xterm's 256: the 16, a
  6x6x6 cube, 24 greys) and `38;2;r;g;b`/`48;2;...` (also with `:`, with or without a colour space) are shown as the
  nearest of the 16 (by distance in RGB to the palette); `2 3 5 8 9` are ignored. How a colour looks depends on the
  depth (Display, below). **Erase takes the background colour** (as Linux and xterm): erased cells and the rows a
  scroll brings in get the default foreground on the current background. **Region scrolls are blitter work:** `CSI
  S/T/L/M`, `ESC M` and line feeds inside a scroll region move the region's rows with one blitter copy and fill the new
  rows with one blitter fill (before, the CPU redrew every row of the region); whole erased rows (`ESC[2J`, `ESC[J`,
  `ESC[2K`) are blitter fills too. `ESC 7`/`ESC 8` save and restore the colours and attributes with the cursor (as
  xterm). **The scrollback** keeps each line's characters and one colour byte per cell, inverse folded in (foreground
  and background swapped when the line is pushed): 200 lines x 80 x 2 = 32 KB per console, allocated when the console
  first scrolls; bold and underline are not kept there. **Cursor-key mode** (DECCKM, `ESC[?1h`/`ESC[?1l`, per console,
  reset by `ESC c`): `kbd.c` sends the unmodified arrows as `ESC O A`..`ESC O D` while it is set (programs on another
  computer over `cu` switch it on). The cursor is the cell drawn inverse; where foreground and background are the same
  it uses the default colours so it stays visible. `term.c` is built `-Os`, its per-character path `-O2`. Verified on
  the host (`tools/term-test.sh`: each escape's colours, the mappings, BCE, region scrolls as blitter calls, the
  scrollback's colour byte, DECCKM, and a fuzz after which the screen always equals the grid, PAL and NTSC). The escape parser follows
  ECMA-48 (DESIGN D14): a CSI sequence is parameter bytes 0x30-0x3F (up to 16 parameters kept, each clamped at 9999;
  `:` sub-parameters as in `38:5:196`), intermediate bytes 0x20-0x2F and a final byte 0x40-0x7E; a sequence it does
  not know is swallowed whole, never printed; a private marker (`?`, `>`, `=`, `<`) makes it a private sequence, of
  which only `?25` (the cursor) and `?1` (DECCKM) with `h`/`l` are acted on; OSC and the other control strings (window titles) are swallowed up
  to BEL or `ESC \`, `ESC ( B` and the like up to their final byte; CAN and SUB cancel a sequence; a control character
  inside one acts and the sequence goes on (tools/term-test.sh, on the host).
- **Display** (`gfx.c`, DESIGN D14): 640 pixels wide hires, the whole screen text. `gfx_init`
  measures the beam's last line for about 60 ms (E-clock timed): above 280 (312 on PAL) the display is 256 lines,
  32 rows; else (262 on NTSC) 200 lines, 25 rows and `DIWSTOP` to match, so no row is hidden below the picture.
  **How many bitplanes** the machine decides at boot, because the display takes chip-bus time from the CPU and chip
  RAM (the 2 MB diet, DESIGN D18; D14 note section 11): **AGA** (Lisa's id register `DENISEID` reads `$F8`, 16 reads
  alike) **with fast RAM: 4 planes, 16 colours, fetched 64 bits at a time (FMODE 3)**, which costs the 40 bus slots
  per line that one plane costs at the 16-bit fetch (never 4 planes at FMODE 0: that takes every slot of the visible
  lines); **OCS/ECS with fast RAM, and any machine with 2 MB of chip RAM and no fast RAM: 3 planes**, 8 colours (a
  bright foreground is the colour plus bold, bright black is grey); **otherwise 1 plane**, grey on black (the colours
  are not shown, inverse is the glyph inverted). **2 planes** (only by the setting) show 4 colours: black, red (red,
  yellow, magenta), cyan (green, blue, cyan) and grey, bright as bold. On AGA every depth above one plane is fetched
  at FMODE 3 (3 planes 30 slots a line, 2 planes 20); one plane keeps the 16-bit fetch. `make GFXDEPTH=1|2|3|4`
  builds a kernel that uses that depth (the screenshot tests; 4 only on AGA). If chip RAM is short the next smaller
  depth is used. The boot log says which: `display: AGA chips, 4 bitplanes at 64-bit fetch (FMODE 3)` and `display:
  640x256, 4 bitplanes, 80x32 text`. **The setting:** `/etc/console`'s line `colours N` (2, 4, 8 or 16), read by
  `init` at its start (man/init.txt), switches the display with `SYS_console` (`CON_COLOURS`, root only): the new
  buffer and Copper list are allocated first (no chip RAM: -ENOMEM, nothing changes), the new list is pointed at with
  interrupts masked, the Copper takes it at the next vertical blank, a frame later (100 ms timeout) the old ones are
  freed and the console shown is drawn again from its grid; the log says the new depth. At FMODE 3 the fetch window
  is `DDFSTRT` `$38`, `DDFSTOP` `$C0`, `BPLCON1` 0 at every depth (one plane at the old fetch uses `$3C`/`$D4`): found
  by the screenshot test of `termtest columns` (all 80 columns, the edges exactly where one plane puts them, nothing
  repeated; knowledge base `amiga/zenix-colour-console.md`), not yet seen on the A1200.
  The planes are **interleaved** (the planes of a line follow each other, 80 bytes each; modulo (N-1) x 80), so one
  pointer serves all planes and whole lines are cleared or copied with one blit per plane or one blit. The
  **Copper list** writes FMODE, `BPLCON3` = `$0C00` and `BPLCON4` = `$0011` (AGA only: a previous program's
  colour-index XOR survives a warm reset), `BPLCON0`-`2`, the modulos, the fetch window and the 2/4/8/16 colours (12
  bits) every frame, then WAITs for line `$20` (before the picture on the `$2C` and `$38` builds, PAL and NTSC) and
  moves the plane pointers. Scrolling moves those pointers at vertical blank, written only while the beam is before
  line `$1C` (else at the next vertical blank), so all planes move together; when the 2-screen buffer wraps, the
  **blitter** copies the visible rows back to the top (identical pixels, so there is no visible switch; every wait
  for the blitter gives up after 500 ms and says so once on the log). A glyph line is drawn by the CPU, one byte per
  plane: `(glyph & m1) ^ m2` (m1 set where the foreground and background bits differ, m2 where the background bit is
  set); with more than one plane the glyphs are kept a second time with each line repeated in all four bytes of a
  long (7 KB), so a line is one read. Glyphs come from the ROM Topaz 8 font, grabbed from `GfxBase` by the bootblock
  before the OS is killed. A kernel panic turns the background dark red and every other colour white.
- **Input path: interrupts queue, `kwork` works** (`kernel/main.c`, audit B4/B5). The interrupt handlers do
  only what cannot wait: the keyboard handler (`kbd_isr`) reads the byte, answers the keyboard's handshake and puts
  the raw code into a 128-slot ring; the serial handler (`serial_rx_isr`, level 5) moves each byte from Paula into a
  1024-byte ring; the 100 Hz tick asks for work while a key is held (repeat).
  Each ring has one producer and one consumer, so no lock: the producer fills the slot, then moves `head`; the
  consumer reads it, then moves `tail`. A full ring drops the new input and counts it; `kwork` reports the count on
  the kernel log (`dmesg`: "kbd: N key codes lost", "serial: N bytes lost"; after lost key codes the held modifiers
  and the repeating key are forgotten, since a lost "key up" would leave a key repeating), and a full tty queue
  (nobody reading) says so too. `kwork()` empties the rings in process context with interrupts enabled - `serial_work` (bytes to
  the tty, the 0x1d test hook), `kbd_work` (modifiers, Amiga+F1..F10 console switches, keymap, key repeat, the tty),
  the log line for an unexpected interrupt vector - and is called from exactly four places: the idle loop; the end of every system call; the level-2
  interrupt when it cut into **user** mode (then no kernel code is running at all, because the kernel is never
  preempted; the mask is lowered to 0 first, and if Ctrl-C or Ctrl-Z hit the running program it is rescheduled
  at once so it ends or stops); and between the 256-byte pieces of a long terminal write (`tty_write`: drawing
  4 KB can take seconds on a 68020 with chip RAM only, and the rings would overflow; the terminal is consistent
  there, and a writer killed by Ctrl-C stops at once). An interrupt that cuts into kernel code (a system call, or `kwork` itself) never
  calls it, and a `busy` flag keeps it from being re-entered, so the tty queue and line, the terminal grids, the
  blitter and the heap (the first use of a console allocates its tty and terminal) are only ever changed by one
  piece of code at a time. Lost wakeups (B5) cannot happen: `tty_read` and init's `proc_wait` (`WAIT_CONSOLE`) test
  "is there something?" and then block in kernel code with none of those points in between, so `kwork` - the only producer -
  cannot run between the two; input that
  arrives meanwhile waits in its ring, and the wake-up that delivers it (`ev_wake`, section 6) comes after the reader
  is blocked - and one that came between the check and the block is caught by the event bit. The
  idle loop checks `kwork_pending` with interrupts masked and then `STOP`s (which enables them in the same
  instruction), so input queued in between ends the wait at once. Latency: at once when the machine is idle or a
  system call ends; at the next tick (10 ms) while a program computes without system calls; every 256 bytes of
  terminal output; while another long system call runs (a disk transfer), input waits in the ring until it ends.
- **Keyboard** (`kbd.c`): CIA-A serial data register; after each byte the handshake pulls SP low for at least
  75 E-clock ticks (105 us; the hardware manual asks for 85) timed with `hr_time()`, never with a counting loop
  (Emu68 runs about 1000 MIPS). The CIA's serial-port interrupt (ICR bit 3) is enabled, so the byte is taken and
  answered at once, not at the next tick (the keyboard's own limit is 143 ms, and it buffers about 10 keys); the
  handler queues it for `kbd_work` (above). One handshake is given when the tick starts (`kbd_init`): the keyboard may
  still wait for an answer to a byte nobody took (hand-off note K2). The keyboard's own codes are not keys: $F9 (lost
  sync) and $FA (its buffer overflowed) make `kbd_work` forget held modifiers and the repeating key, since key-up
  codes may be lost, and say so on the log; $FC (self test failed) is logged; $FD/$FE are ignored (HRM p.359-360).
  **Reset warning** (HRM p.361): Ctrl+Amiga+Amiga makes keyboards that have the feature send $78 twice; only two in a
  row count. The first is answered normally; on the second `kbd_isr` keeps KDAT low, `kbd_work` logs "kbd: reset
  warning" and wakes init, and init, in its own context in `proc_wait` (`kbd_reset_flush`), syncs every filesystem
  (the servers write what they hold and flush the drive's write cache, as `sync` does; floppy writes are synchronous,
  so no track is half written), then all disk writes are refused (`disk_frozen`), the log gets "kbd: reset warning:
  filesystems synced in N ms" (kept in chip RAM for the next boot) and KDAT is released: the keyboard resets the
  machine. The tick lets go after 8 s whatever happened (the keyboard waits 10 s). A keyboard without the feature
  resets at once, as before. Verified in FS-UAE, whose keyboard sends the warning (`tools/diag-test.sh`: "filesystems
  synced in 10 ms" in the next boot's previous-boot lines); whether the A1200's keyboard sends it is not known. Raw codes 0x00-0x3F go through a keymap with four layers like AmigaOS's (plain, Shift,
  Alt, Shift+Alt; the Alt layer is AmigaOS's, the same on every national keyboard, and is where `[ ] { } \ | @`
  live on a Swedish Amiga); `sv` is the AmigaOS keymap "s" (generated from xkeyboard-config by the Fuzix Amiga
  port, checked against Workbench 3.1's `DEVS:Keymaps/s`), `us` the US one. Caps Lock (raw 0x62, a toggle the
  keyboard reports as down/up) shifts the letters only. Accents that AmigaOS treats as dead keys are typed as
  the spacing character (no composition). Software key repeat: 500 ms, then 33 per second (timed by the tick, done by `kbd_work`), until
  the key is released or another goes down; modifiers and Amiga combinations do not repeat. Codes 0x40-0x67 are
  layout-neutral; arrows, F-keys, Help and Del become escape sequences in raw mode. Verified in FS-UAE
  (`tools/kbd-test.sh`); the handshake length and the reset warning are read from the manual and the hand-off note,
  not measured on hardware.
- **Serial** (`serial.c`): interrupt-driven receive (queued for `serial_work`, above), 115200 baud, mirrors the
  console on the screen (its output from then on; a switch does not resend the screen); input goes to that
  console's tty. The test rig depends on it, including the hook 0x1D + 1-9 / 0 (show console 1-9 / 10, as
  Amiga+F1..F10) and u d U D (scroll back a line or a page); 0x1D with any other byte does nothing.
  Sending is a 4 KB ring drained by polling Paula (about 11.5 KB/s), far slower than the screen. **The kernel's
  own output** (`kprintf`: the boot log, panics) always waits for room, so it is complete. **The console's copy**
  waits only once the port has received a byte since boot (someone listens: a terminal, or the test harness, which
  sends 0x1D '.' half a second after each boot's `tick latch` line); until then a byte that does not fit the ring
  is dropped and counted, so with no cable the screen is never held to the serial speed. The kernel log says
  `serial: nothing received on the serial port yet: ...` at the first drop, the running count at each doubling,
  and the total when input is first seen. Root can also switch the copy off (`SYS_ttymirror`; bench measures its
  screen line both ways). Verified on the host (`tools/serial-test.sh`, a model of Paula's port); FS-UAE sends
  serial bytes without the baud-rate delay, so its ring never fills and it cannot show the drops. Drawing text is
  slow too: a 79-character line takes about 11 ms on an A1200's chip RAM (FS-UAE 68020, and bench on the PiStorm
  with the copy off), longer than its 7 ms on the wire - the copy limits output that is cheap to draw (escape
  sequences, a fast CPU with fast drawing), not plain text on a stock A1200.
- **Unexpected interrupts** (audit E): vectors 24-31 without a handler of their own (spurious, levels 1, 4 and 6,
  the NMI; nothing enables their sources) go to `irq_other_entry`: a Paula level is switched off in `INTENA` (its
  `INTREQ` bits stay: the floppy and sound drivers poll them), level 6 also reads CIA-B's ICR, `kwork` logs each
  vector once (`irq: unexpected ...`), and the interrupted code goes on untouched. Before, they killed whatever
  process was running (or panicked in kernel code).
- **The clock** (`main.c`, audit E): the E-clock is the crystal / 40, 709379 Hz on PAL machines and 715909 Hz on
  NTSC ones; the kernel reads which from the Agnus/Alice id's NTSC bit (VPOSR bit 12) at boot (`eclock_hz`) and sets
  the CIA-A tick latch to the nearest of E / 100 - 1 (a timer period is latch + 1): 7093 on PAL (99.997 Hz, about
  0.25 s a day), 7158 on NTSC (100.001 Hz). The old fixed 7094 gave 99.98 Hz on PAL (15 s a day) and 100.9 Hz on
  NTSC (13 minutes a day). `hr_time()` counts the same E-clock; the floppy driver's waits use `eclock_hz`; the boot
  log says `clock: E-clock N Hz (PAL|NTSC), tick latch N`. What the display shows (the beam count above) and the
  crystal are read separately, so a machine switched to the other display standard still keeps time.

## 12. User space

Every program starts in `user/crt0.S`: the kernel jumps to offset 0 with argc and argv on the stack, where the C
rules put a function's arguments, and crt0 makes one PC-relative call to libz's `_zstart`, which sets `_zargv` (for
libz's messages), calls `main` and ends with `_zexit` (buffered output flushed, then `SYS_exit`). So crt0 holds no
address that needs relocating and no hand-written code reaches a global, which a program with shared code requires
(DESIGN D13; crt0 built with `-DZEX2` has the ZEX2 header).

`user/libz.c` is a tiny freestanding C library (`printf`, string/ctype, `malloc` over `memalloc`, `qsort`, `strtol`,
environment, buffered line reader, a regex engine from the CUtils project), and the POSIX signal and wait calls on
top of DESIGN D16's system calls (section 6): `sigaction`, `signal` (BSD style, as glibc: with `SA_RESTART`),
`sigprocmask` and the `sigset_t` macros, `kill`, `raise`, `alarm` (seconds), `pause`, `setpgid`, `getpgid`,
`getpgrp`, `tcsetpgrp`, `poll` (milliseconds; also ports), `select`, `zsig_num` / `zsig_name` (the names `kill`,
`pkill`, `timeout` and the shell take). The handler table and the entry the kernel sends handlers to (`_zsigentry`:
pc-relative only, a5-relative data, so it works in a program with shared code) are libz's; the entry keeps `zerrno`.

**`malloc`** (audit B2) has to live with `NREG` = 8 regions per process, so small blocks never take a region each.
Every block has an 8-byte header: its capacity and a tag (small in use, big in use, free). Blocks of up to 32 KB
come in size classes of 16, 32, 64 ... 32768 bytes (the header on top, so the usual power-of-two buffer fills its
class), cut from chunks the kernel gives: the first
chunk is 4 KB (so a program that needs little holds little: `login` waiting for its shell, the servers), each next
one four times the last up to 512 KB (4, 16, 64, 256, 512 KB; smaller when memory is short), so even a program that
needs megabytes uses few regions. A freed small block goes on its class's free list and is used again (O(1), no
coalescing); an empty class carves a free block of a larger class before taking new chunk space; small blocks are never given
back to the kernel (that happens when the program ends). A bigger block gets a region of its own, and `free` gives
exactly those back with `SYS_memfree`. `free` and `realloc` check the tag: a pointer `malloc` did not give out, or
one freed twice, is reported on standard error and ignored. `realloc` grows in place when the block's capacity is
enough, else copies only the old block's bytes. `xmalloc` ends the program with "NAME: out of memory".

**The shell's arena** (`user/sh.c`, fixed 2026-10-25: it was a fixed region of an eighth of free memory, 64-256 KB per
shell, and a script was read and parsed whole). Tokens, the parsed tree and expansions come from a stack of chunks
taken with `SYS_memalloc`: an 8 KB first chunk that stays, and more while a command needs them (at least 16 KB, each
four times the last, so few regions; a command substitution's output is gathered in a `malloc` buffer and copied
in once; an expanded here-document and the joined value of an assignment are built in the arena directly, with no
fixed buffer and no length limit). The word being expanded stays in two fixed 4 KB buffers (its characters and their
quote marks: a word is at most 4095 characters), with a third for a case pattern's marks: the expander's results go to
the arena while a word is built, so a buffer taken there would stay until the command ends, one per word. Every
command, loop round and pipeline takes a mark and releases it when done;
releasing gives back the chunks taken after the mark except one kept as a spare, and between the commands of a script
and at the prompt the spare goes back too, so an idle shell holds only the first chunk. A file of commands (`sh file`,
`. file`, `/etc/profile`, `~/.shrc`) is read 512 bytes at a time on a descriptor above 9 (out of the way of `N>&M`)
and run one complete command at a time (so are commands from standard input when it is not a terminal: there fd 0 is
read in place, one byte at a time from a pipe, or a block at a time from a file with the position moved back to the
end of the command before it runs, so the command reads the rest; `sh -i` forces the prompt): lines are added until
the lexer and the parser no longer ask for more (an
open quote, a here-document without its end, an `if` without `fi`, a line ending in `\`, `|` or `&&`), the command
runs, and its memory is released before the next is read. The interactive prompt asks the same question of what was
typed (`cmd_incomplete`: lexed and parsed in the arena, then released): while the command is unfinished it prints PS2
and adds the next line (at most 2047 bytes in all); a syntax error is not unfinished and is reported at once.
Functions keep their text on the heap (they are parsed again
at each call), so they outlive the command that defined them. When the kernel gives no more memory the shell says
"out of memory" and ends (status 2). Variables, aliases, history and functions live on the heap (`malloc`).

**Directory walkers** (audit B6: `rm -r`, `cp -r`, `mv`, `du`, `find`, `grep -r`, `chown -R`, `tar c`, `diff -r`,
`tree`, `ls -R`) use two libz helpers instead of arrays on the stack: `dir_list` reads a directory into a
heap array that grows as needed (no limit on entries; `dir_free` gives it back), and `path_join` makes "dir/name" in
a new heap string, or refuses (NULL, `zerrno` ENAMETOOLONG, "file name too long") when it would be longer than the
63 bytes a system call takes. Programs that start another program by name (`find -exec`, `xargs`, `timeout`) find it
with `path_find`, the shell's `$PATH` search; `sed` and `expr :` share `bre_to_ere`. libz and the programs are built
with `-ffunction-sections -fdata-sections`, so the linker's `--gc-sections` leaves out the libz parts a program does
not use. A level of recursion costs 72-170 bytes of stack instead of 1.4-2.7 KB. `printf`, `snprintf`, `sprintf`, `fprintf`
and `oprintf` share one formatter with no length limit: `%d %i %u %x %X %o %c %s %p %%`, flags `- 0 + space #`, width and
precision as numbers or `*`; no floating point, no 64-bit numbers. The programs (the count and the list: commands.md): a full shell (`sh`:
line editor, history, completion, variables, quoting, wildcards, pipes, redirects incl. here-documents,
`if/while/for`, functions, aliases, arithmetic, command substitution, job control), file tools, text tools
(`grep sed sort diff cut tr ...`), `gzip`/`tar`, a pager (`less`), an editor (`edit`), `man`, disk tools (`fdisk
mkfs.ext2 fsck.ext2 df sync`) and test programs (`crash spin badptr hello bench termtest`). The commands are
tested by running one script (`rootfs/tests/cmds.sh`) under Zenix and under GNU bash on the host and comparing the
outputs (`tools/cmds-test.sh`).

## 13. Build and test

- Toolchain: `m68k-elf-gcc` and binutils, prefix `TOOLPREFIX` (README.md); `make` produces `build/zenix.adf` (bootblock + kernel only),
  `build/zenix.hdf` (ext2 root disk) and, as a separate dev/test build, `build/zenix-ram.adf` (the same plus a RAM
  root image). That floppy holds the bootblock, the kernel and the 704 KB RAM disk (`RAMDISK_K` in the Makefile), so
  the kernel may be at most 179200 bytes (`KERNEL_MAX`): every build of `kernel.bin` prints how much is left, and
  `tools/mkadf.py` refuses an image that is over and says by how much. The kernel is built with `-O2`, except the five
  truly cold drivers - `floppy`, `emmc`, `a3000`, `rtc` and `kbd` (every wait in them is timed by the E-clock or a timer)
  - which are built with `-Os` (`KOPT_*` in the Makefile, DESIGN D13 step 0: about 6.8 KB smaller); the floppy's MFM
  track coding (`mfm.h`) keeps `-O2` through a pragma in `floppy.c`. `gfx` is built with `-Os` too (DESIGN D14: its
  set-up code), its glyph drawing with `-O2` (a function attribute).
- Pi driver tests (no hardware needed): `tools/stage0-test.sh` (stage0's logic on the host against the real CM4
  device tree with Emu68's range rewriting), `tools/stage0-68k-test.py` (the real 68020 stage0 code in a CPU
  emulator, `pip install unicorn`), `tools/emmc-test.sh` (the SD driver against a strict SDHCI model,
  `tools/emmc-fake.h`) and `tools/a3000-test.sh` (the A3000 SCSI driver against a WD33C93A + DMAC model). They show the logic matches the specifications; they cannot show the real chip behaves the same.
- Test rig: FS-UAE (headless, serial script) via `tools/run-test.sh`, `tools/serial-tap.py`, `tools/shot.py`;
  `tools/kbd-test.sh` presses keys on the emulated keyboard (wtype in the cage rig) and checks the keymap layers,
  Caps Lock and key repeat. `tools/diag-test.sh` checks the boot stage colours (test builds that stop at each
  stage; the screenshot's pixel colour), the split and red-bar panic screens, and the previous-boot log across a
  warm reset (Ctrl+Amiga+Amiga pressed through wtype; `shot.py` `RESET_AT=`). `tools/colour-test.sh` checks the
  colour console by screenshots at 1, 2, 3 and 4 bitplanes (`make GFXDEPTH=n`): `termtest columns` decoded cell by
  cell (all 80 columns, the edges where one plane has them), the 16 colours against the palette, a screen after
  `termtest scroll` pixel-identical to a redraw from the grid, the scrollback's colours; and `/etc/console`'s setting
  on the A1200 without fast RAM (4 and 16 colours switched to and checked the same way, a wrong value refused); a
  serial script's `shot NAME` line
  takes a screenshot when the script gets there (`shot.py`, `serial-tap.py`). `make BUILD=/tmp/x` builds everything
  in another directory (the kernel finds `stage0.bin` and the built-in servers through `-I$(BUILD)`).
  `tools/zenixboot-test.sh` boots with no floppy: Kickstart 3.1 on an A1200 (68EC020, and a 68040), Kickstart 3.2
  and 2.05 (an A600 with a 68020 and chip RAM only) when those ROMs are given, a 68000 that zenixboot must refuse, and
  a run through its error paths; the disks are made by `tools/mkffs.py` (an FFS volume written from Linux),
  `tools/mkrdb-multi.py` and `tools/mkcard.py`.
  `tools/matrix.sh` boots 14 emulated setups, every config in `fsuae/` and `fsuae/matrix/` but the mount and
  ZenixFS-root ones: A1200 stock (68EC020, 2 MB chip), with 4 or 8 MB fast RAM, 1 MB chip only, NTSC, with a
  68030, 68040 or 68060, an RDB disk, a RAM-disk boot with no disk, an A3000 (SCSI), two 68030 development setups
  (A1200- and A4000-class), and a 68000 that must be refused cleanly. Each boots, runs a short session and logs in
  on consoles 2, 3 and 4 (the 1 MB chip-only one goes on to 5 and 6, where memory runs out: init or login must say
  so). Filesystem work is checked with the host's `e2fsck`. `make check` runs all the strong tests in one go
  (docs/README.md, "Testing"); the Kickstart files come from `ZENIX_KICKSTART_DIR` (`tools/fsuae_conf.py`).
- Source layout: `boot/` bootblock, zenixboot and the hand-over code they share, `kernel/` the kernel and its protocol headers, `user/` programs and libc,
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
  `0666 & ~umask` (directories `0777 & ~umask`). A refusal returns -EACCES (13) to the caller.
- `login` (run by `init` on each console, section 11) authenticates against `/etc/passwd`, drops root and starts
  the shell, then hands it to init and ends (DESIGN D18). Every console has its own login, so several users (or one
  user several times) can be logged in at once; nothing in the kernel records a "session".
- **Processes** (one rule, `may_signal` in syscall.c, for `SYS_kill` - each member of a group, D16 -, `SYS_jobctl`,
  `SYS_setfg` and `SYS_setpriority` on another process): root may signal
  any process but the idle one; others only a process whose real or effective uid equals their real or effective
  uid, or their own child, and never a built-in server (started from the kernel image: diskd, fsd, zfsd, boot) or a
  privileged process. A refusal is -EACCES (-EPERM in `kill`'s new form, as POSIX), no such process -ESRCH. `SYS_setfg` works on the caller's own terminal, 0 (nobody)
  is always allowed. `SYS_settime` and `SYS_ttymirror` are root only.
- **Raw messages** (section 8): the filesystem servers do nothing for a message that did not come from the kernel
  unless it is root's, so a user cannot read or write by inode number, open files without the kernel's open-file
  table, or send `FS_QUIT`; the disk server serves root only; `fs`, `fs.*` and `disk` are names only root can take.
  `proc_spawn` takes the set-user-id bit only from the server of the mount table (the port it routes the path to;
  a port of that name can only be root's).
- **The filesystem rules** added by audit D (kernel/fsperm.h rules 5, 7, 9, 10, in both servers): a set-group-id
  directory gives new entries its group and new directories the bit; a write or truncate by anyone but the owner and
  root clears set-user-id (and set-group-id when the group has x); `utimes` with a given time is for the owner and
  root, "now" (`FS_UTIME_NOW`) also for anyone who may write; a non-root owner's `chmod` drops set-group-id unless
  the file's group is theirs; moving a directory to another parent needs w on it (ZenixFS; ext2 does not move
  directories between parents). `O_EXCL` with `O_CREAT` fails with -EEXIST if the name exists, also as a dangling
  symbolic link, in the same request as the creation (here-documents and `mktemp` use it, with mode 0600).
- **The tools**: `su` gives the target's USER, LOGNAME, HOME, SHELL and PATH=/bin and drops IFS, ENV, CDPATH; `sh`
  reads `~/.shrc` only if it belongs to the effective user or root; `/etc/passwd` lines with a non-numeric uid or
  gid are ignored (never read as 0); `passwd` and the lookups read the whole file (up to 64 KB); new password hashes
  are `1000$salt$hex` (old `salt$hex`, 100 rounds, still accepted); `tar x` drops leading `/`, refuses `..` parts and
  names through a link the same archive made; `find` never follows links below its starting points.
- **The shipped images** have root logged in automatically and no passwords (limits.md); `login` warns.
- `mkroot.sh` builds the image under `fakeroot` so files belong to root; `/tmp` is 1777; `su` and `passwd` are 4755.
- The kernel's own file loading and the servers run as root. The kernel's reads of a program (`fs_exec_open`,
  `fs_exec_read`) use the identity of the process that asked for the spawn.
- Programs report a refusal as "permission denied": `sys3` stores the error number of the last failed call in
  `zerrno`, and `zwhy("cannot open")` (libz) returns its text ("permission denied", "no such file or directory",
  ...; section 7), or the given text when there is none.
- What this is not: with no MMU a program can still read and write any memory - for example change a message after
  the kernel stamped it, or the kernel's own tables - so this is accounting and protection against honest mistakes
  and against users using the system's own calls, not against a program written to cheat (limits.md).
