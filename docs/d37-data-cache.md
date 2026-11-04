# D37: the data cache on 68040 and 68060 without an MMU

Approved by the owner on 2026-11-04 ("yes to 060 cache"; `docs/decisions.md`). Built on branch `cache`, 2026-11-04.
**Never run on a real 040 or 060 yet** (section 9 has the steps for the first run on the Apollo 1260).

## 1. What the owner gets, in plain words

On the A1200 with the Apollo 1260, `ls -laR /` took 4.26 s: Zenix ran the 68060 with its data cache off, so every
read of a variable, a buffer or the stack went out to the RAM chips. Now the 68060 (and a 68040) keeps a copy of fast
RAM in its data cache, as AmigaOS does with its 68060.library. Chip RAM, the custom chips, the CIAs, the IDE port, the
PC Card slot and every other piece of hardware are never cached, so the screen, the floppy, the sound and the disks
see exactly what the CPU wrote. If something looks wrong on a real machine, one word at boot turns it off again:
`zenixboot DCACHE=OFF`.

## 2. The problem

Without an MMU, Zenix has no page tables to say "this page is chip RAM, do not cache it". On a 68030 that is the end of
it (section 7). The 68040 and 68060 have something smaller that is enough: four **transparent translation registers**
(TTRs, two for data `DTT0`/`DTT1`, two for instructions `ITT0`/`ITT1`) that each give a cache mode to a block of the
address space, and they work with translation switched off (MC68040UM 3.1.3, MC68060UM 4.1.3: "The TTRs operate
independently of the E-bit in the TCR"). On the 68060, the translation control register's default bits (`TCR` `DCO`,
`DCI`) give the cache mode of every access no TTR matches, while translation is off (MC68060UM 4.1.2). The 68040 has
no such bits: an access no TTR matches is "cachable/write-through" (MC68040UM 3.1.2).

The facts this note relies on, *read* in the manuals (pages confirmed in the text; FS-UAE checks in section 8):
- **TTR layout** (both CPUs): bits 31-24 the base of A31-A24, bits 23-16 a mask of base bits to ignore, bit 15 `E`,
  bits 14-13 `S` (`1x` = user and supervisor), bits 9-8 `U1`/`U0`, bits 6-5 `CM`, bit 2 `W`. One TTR covers whole
  16 MB units; the mask can make a block of 2^n units.
- **Cache modes** (`CM`): 00 write-through, 01 copyback, 10 inhibited and serialized (040) / precise (060), 11
  inhibited (040) / imprecise (060).
- **When both data TTRs match, `DTT0` wins** (MC68040UM 3.4 and MC68060UM 4.4: "If both registers match, the TT0
  status bits are used").
- **060 `TCR`**: bit 15 `E`, bit 14 `P`, bits 9-8 `DCO` (data default cache mode), 7-6 `DUO`, 5 `DWO`, 4-3 `DCI`
  (instruction default), 2-1 `DUI`; all 0 at reset (write-through). The EC060 has the TTRs and the default bits too,
  without the paged MMU (MC68060UM 1.1.2.1); the EC040's access control registers sit where the TTRs are.
- **060 `CACR`**: 31 `EDC`, 30 `NAD`, 29 `ESB` (store buffer, used by write-through and imprecise pages), 28 `DPI`
  (CPUSH keeps lines valid), 27 `FOC`, 23 `EBC` (branch cache), 22 `CABC` (clear it), 21 `CUBC`, 15 `EIC`, 14 `NAI`,
  13 `FIC`. **040 `CACR`**: 31 `DE`, 15 `IE`.
- **060 `PCR`**: bits 31-16 the part's identity, 15-8 its revision, 7 `EDEBUG`, 1 `DFP` (FPU off), 0 `ESS`
  (superscalar dispatch); `ESS` is 0 after a reset.
- The branch cache is cleared by any `CINV`/`CPUSH` of the instruction cache (MC68060UM 5.11); it is virtually mapped
  and must be cleared on a context switch only when the address space changes, which never happens in Zenix.
- **Reset does not invalidate the caches** (both manuals, the cache chapters); a software `RESET` resets only the
  devices, not the CPU's own registers.

How Linux does it (*read*, `arch/m68k/kernel/head.S`): with its MMU on, and on a 060 `CACR` = `EDC` `EIC` `ESB` `DPI`
`EBC` `CABC` and `PCR` `ESS` = 1. Zenix takes the same `CACR` bits except `DPI` (section 5) and the same `PCR` bit.

## 3. The decision: which addresses get which mode

The memory facts come from the kernel's memory list (the regions exec called `MEMF_FAST`) and the board list in the
hardware table (`hwinfo`: configured boards without the RAM bit). Units are 16 MB (A31-A24). The choice is
`kernel/cache_plan.h`, pure code the PC's test runs too.

- **`DTT0` = the first 16 MB (0x00000000-0x00FFFFFF), cache-inhibited, serialized (040) / precise (060).** That unit
  holds all of chip RAM, the CIAs, the custom chips, Gayle's IDE (0xDA0000), the PC Card window (0x600000-0xA4FFFF),
  Zorro II memory and I/O (0x200000-0x9FFFFF, 0xE80000 autoconfig, 0xE90000-0xEFFFFF), the RTC (0xDC0000), the A3000's
  SCSI (0xDD0000) and the ROM. Precise on the 060: no store buffer, every write in program order, as before.
  A Zorro II fast RAM board in this unit (FS-UAE's `fast_memory`, an A2000's card) stays uncached: it shares its 16 MB
  with the chips.
- **`DTT1` = the smallest aligned block of 2^n units that holds every unit with fast RAM above 16 MB**, in the chosen
  mode (section 4). Where it also covers the first unit, `DTT0` wins. On the 1260 (fast RAM 0x01000000-0x04FFFFFF)
  that is 0x00000000-0x07FFFFFF: units 1-7 are cached, of which 5-7 hold nothing. A block that would also hold a board
  that is not RAM (a Zorro III card's registers) is refused: the data cache stays off and the boot log says which board.
- **Everything else**: on a **68060**, the `TCR` default `DCO` = inhibited, precise (`TCR` = 0x0200): Zorro III I/O,
  its autoconfig space at 0xFF000000 and anything an accelerator puts high stay uncached. `DCI` stays 00, so
  instruction fetches are cached everywhere as before (the instruction side has no TTRs set, as before).
  On a **68040** the rest is write-through cacheable by the CPU's fixed default; only unmapped space is there unless a
  board is, so where the board list shows a board that is not RAM above 16 MB (an A3000 or A4000 with a Zorro III I/O
  card), a 68040 keeps its data cache off (the log says why). A finer answer (`DTT1` over those boards, fast RAM
  write-through by the default) is possible but cost kernel bytes for machines Zenix has never met; not built.
- **The instruction cache** is on, as before, on every CPU.
- **Emu68** (the PiStorm; `hwinfo` says so): nothing changes. Emu68 stores `CACR`'s data bit and the TTRs without using
  them (*read* in its source, `M68k_LINE4.c`: the MOVEC cases store the values; its memory is coherent with the
  chips through the ARM's own caches): the registers are not written, `CACR` = `IE` only, exactly as before.
- **68020 and 68030**: nothing changes (section 7).

## 4. Copyback, not write-through (the default), and why

Copyback keeps writes in the cache until a line is replaced or pushed; write-through writes every store to memory
(through the 060's four-entry store buffer). The kernel and the programs write mostly to the stack, to their own
variables and to buffers they copy in and out of (the block cache, messages), which copyback keeps on the chip.
Copyback needs a push wherever something other than the CPU's data side reads memory the CPU wrote. Each such place in
Zenix, checked (section 6), is either uncached memory or already pushes. So the default is **copyback**; the boot
option `DCACHE=WT` gives write-through and `DCACHE=OFF` the old state, so the owner can compare both on the 1260 in
minutes (section 9). The emulator cannot decide this (FS-UAE has no 68040/68060 data cache at all, section 8); its
figures there are labelled as such. If the 1260 shows write-through as fast or copyback misbehaving, the default changes to
write-through in one line (`cache.c`).

## 5. The CPU registers Zenix writes (`kernel/cache.c`, `cache_setup`, after `mem_init`, interrupts masked)

In this order, with `cpusha bc` first (push and invalidate both caches; never `cinva`, CLAUDE.md). The manuals say
"the CINV instruction must clear the caches before enabling them" (MC68040UM 4.5, MC68060UM 5.3): `cpusha` does the
same only when the tags are sane, which holds because the ROM's boot (and 68040/68060.library) invalidated them after
power-on and everything since pushed; the old `cache_enable` relied on the same:
0. Not at all, and the data cache stays off as before, with `DCACHE=OFF` (bootinfo `kopts`) or **the left mouse
   button held** while Zenix starts (CIA-A's port bit 6, read once here): the way out that needs no new zenixboot and
   works from the floppy too.
1. 060 only, probed by reading `PCR` with the illegal-instruction vector caught (exec's AttnFlags are not enough: FS-UAE
   and a 1260 without 68060.library report a 68040): `PCR` = `ESS` on, `DFP` kept, `EDEBUG` off.
2. `TCR` = 0x0200 on the 060, 0 on the 040 (translation off).
3. `ITT0` = `ITT1` = 0; `DTT0`, `DTT1` as section 3.
4. `CACR`: 040 `DE` `IE` (0x80008000); 060 `EDC` `ESB` `EBC` `CABC` `EIC` (0xA0C08000). `DPI` stays 0: every push also
   invalidates, which the ZINE hand-over needs (its program changes memory while Zenix's lines would stay valid).
   With `DCACHE=WT` only `DTT1`'s mode changes. Off (step 0, or no safe block): nothing of this is written, `CACR` =
   `IE`, as before. A missing register (an EC/LC part without one; a trap caught) also leaves `CACR` as before.
5. **A self-test**, in assembly with no memory access between the steps: a long in chip RAM and a long in the cached
   block are written and read with the data cache switched off and on in between, which tells whether a read is served
   from the cache and whether a write stayed in it. Chip RAM must come out uncached: if it does not, the data cache is
   switched off at once and the log says so (the TTRs did not do what this note says). Fast RAM (a line in the kernel's
   own memory, the largest fast region) is reported as seen: copyback, write-through, or not cached (FS-UAE).
6. The log lines, as the kernel prints them (`%p` without leading zeros):
   `cache: 68060 (superscalar, branch cache, store buffer), data cache copyback for 0x1000000-0x7ffffff, the rest uncached`
   `cache: 68040, data cache write-through for 0x8000000-0xbffffff, the first 16 MB uncached`
   `cache: self-test: chip RAM uncached, fast RAM copyback`
   `cache: instruction cache on, data cache off: DCACHE=OFF` (or `mouse button held`, `68020/68030`, `Emu68 (its own)`,
   `no fast RAM above 16 MB`, `a board in fast RAM's block`, `68040, a board above 16 MB`, `a register missing`,
   `the self-test found chip RAM cached`, `no hardware table` (an old loader: Emu68 and the boards unknown))
   The "for" range starts at 0x1000000 when the block also covers the first 16 MB (which `DTT0` keeps uncached).

## 6. Every place a push matters, checked

- **Programs loaded and relocated** (`proc.c`, ZEXE and ZEX2 with shared code): `cache_flush_code` = `cpusha bc`
  after the image is written and before it runs: it pushes the loader's writes and drops stale instruction lines.
  Unchanged; it was already the one path for every program, the boot blob included.
- **The kernel relocating itself** (`start.S`): runs before `cache_setup`, with the data cache off (zenixboot switches
  it off before the hand-over; the floppy path comes from a ROM boot without 68040/68060.library), and pushes anyway.
- **ZINE's blob** (`zine.c`, `zine_enter`): written by `/bin/zine` in user mode, run by the kernel. It was not flushed
  before (D29 says the kernel flushes; the code did not: a stale instruction line or Emu68 translation could have run);
  now `zine_enter` switches the data cache off and `cpusha bc` (020/030: `CACR` `CI`) before it calls the blob. The
  program then runs with the data cache off and Zenix's TTRs in place, as D29 8.1 says; the blob saves and restores
  the TTRs, `TCR` and `PCR` (`zb_mmu_save`, `zb_mmu_back`) and `zine_resume` puts `CACR` back and `cpusha bc`, so no
  line from before the session survives into it (`DPI` is 0).
- **A software reset** (`machine_reset`, used by `reboot` and the reset warning): interrupts masked, `CACR` 0 and at once
  `cpusha bc` (no line can be dirtied in between: the review found an interrupt could have done so between a push
  first and `CACR` after), then the TTRs and `TCR` cleared as a hardware reset leaves them (the 060's `PCR` keeps
  `ESS`), before `RESET` (which leaves the CPU's own registers as they are, MC68060UM 8): the ROM must not start with Zenix's cache set-up, and
  no dirty line may be lost. A hardware reset (Ctrl-Amiga-Amiga without a warning) loses unwritten lines; nothing Zenix
  keeps in fast RAM is used after a reset (the disks are written by the reset warning's sync; the klog and the boot
  note are in chip RAM, uncached).
- **DMA**: every DMA on these machines reads or writes chip RAM (display, blitter, Copper, audio, floppy) or a board's
  own memory (the A2065's buffer at its Zorro II address), all in the first 16 MB: uncached. The A3000's SCSI moves
  through a bounce buffer in chip RAM. IDE, the PC Card NE2000 and the SD card are programmed I/O. `genet` runs only
  under Emu68. `DRV_CACHE` (drivers with a DMA region) now pushes the region's lines with `cpushl` on a real 040/060
  with the data cache on, as its contract says; no driver needs it on a real 68k today.
- **The bootinfo, hwinfo, klog area, boot note**: chip RAM, uncached.
- **Not covered**: a program that writes code and runs it without a system call (GCC's nested-function trampolines);
  none in Zenix's tree. Such a program was already unsafe with the instruction cache on.

## 7. The 68030 stays as it is

The 030's own way to keep a region out of its caches is its MMU: the page descriptors' `CI` bit, or the `CI` bit of
its two transparent translation registers `TT0`/`TT1` (MC68030UM 9.3, 5.7.2), which the EC030 does not have. Without
the MMU, what keeps chip RAM out of the 030's data cache is the board asserting the `CIIN` signal for those cycles
(MC68030UM 5.7.1), and that is the board's design, which Zenix cannot ask. Also the gain is small: the 030's data cache
is 256 bytes, write-through only. So the 030's data cache stays off. A later option, not built: `TT0` with `CI` over
the first 16 MB on a full 68030 (whether the TTs act with translation off is to be checked on a real 030 first).

## 8. Tests (FS-UAE, labelled emulator results) and what they cannot show

- `tools/cache-host-test.sh` (host, in `make check`): the block chooser (`cache_plan.h`) on the PC for the 1260's
  layout, FS-UAE's two kinds of 32-bit RAM, A4000 layouts, Zorro II fast only, no fast RAM, and boards that are not RAM
  inside and outside the block (060 and 040 rules); for every chosen block it checks that no board's unit is in it.
- The matrix's A1200 040 and 060 (`fsuae/matrix/`) have 64 MB of 32-bit fast RAM at 0x08000000 besides the Zorro II
  8 MB, so every test that boots them runs with the data cache set up: `matrix` checks the cache lines
  (`tools/matrix.sh`), `zine-emu-test`'s 040 part hands the machine over and back with it, `crypto68k-test` runs there.
  `tools/zenixboot-test.sh` setups `dcache-off` and `dcache-wt` (a 68060 with 32-bit RAM, from AmigaOS) check that the
  option reaches the kernel.
- **What FS-UAE cannot show.** FS-UAE 3.2.35 has no data cache for the 68040/68060 (its core knows only the 030's, in
  cycle-exact mode; the WinUAE option `cpu_data_cache` is not in it: tried, `uae_cpu_data_cache` changes nothing). The
  self-test says so on every emulated boot ("fast RAM not cached"). So no emulator test can fail for a missing push:
  every push site (section 6) was checked by reading, and the real check is the self-test and section 9 on the 1260.
  The registers themselves are emulated (the TTRs, `TCR`, `PCR` and `CACR` are written and the boot goes on, on an
  emulated 040 and 060), and the chip-RAM half of the self-test runs.
- **FS-UAE needs its MMU model for this** (found 2026-11-04, measured): without `uae_mmu_model`, with `CACR`'s data
  bit set, the tick (CIA-A timer A, level 2) was lost now and then: `sleep` never returned, `uptime` stood still, in
  about half of the runs on the 060 and once in two on the 040 (`hello`/`sleep` loops, 4-6 runs each). It happened
  with the data bit alone and no TTRs, and with every TTR inhibited too; never with the data bit off (the old kernel,
  4 of 4, also at `uae_cpu_speed = max`; this kernel with only the data bit cleared, 4 of 4); never with
  `uae_mmu_model = 68060` (4 of 4). So, inferred, FS-UAE without its MMU model does not apply the TTRs' cache modes and
  treats the CIA's registers as cacheable once the data cache is on, which is just what a real CPU would do with no
  TTRs: the reason chip RAM and I/O must be inhibited. The matrix's 040 and 060 and zenixboot-test's dcache setups
  therefore set `uae_mmu_model` (Zenix leaves the MMU's translation off; only the TTRs act).

**Emulator figures** (FS-UAE 3.2.35, A1200 with a 68060 and 64 MB at 0x08000000, the matrix's setup, 2026-11-04; not
hardware speeds: FS-UAE has no data cache, so the change it shows comes from the rest of the set-up, likely the
superscalar bit and the branch cache): `time ls -laR / > /dev/null` 1.885 / 1.887 / 1.885 s before, 1.250 / 1.237 /
1.234 s after; `cp` of a 246 KB program 4.15 -> 2.71 s (`cmp` the same both times); `sha256sum` of three programs
(528 KB) 4.98 -> 3.84 s; `seq 1 20000 > /dev/null` 1.29 -> 0.78 s.

## 9. The first run on the Apollo 1260

0. The option needs the new zenixboot on the card (zenixboot is never updated by a bundle: copy it by hand, as
   zenixboot(8) says). Without it, holding the left mouse button while Zenix starts does the same as `DCACHE=OFF`.
1. With today's kernel (before this change): `time ls -laR / > /dev/null` three times; note the figures.
2. Install the new kernel; boot as usual. The boot log (`dmesg | grep cache`) must show the 060 line and
   `cache: self-test: chip RAM uncached, fast RAM copyback`.
3. `time ls -laR / > /dev/null` three times; then a copy of a big file and `cmp`; a few programs; the network
   (FA411) with a `wget` of a file and its checksum; the screen scrolling a long listing.
4. Boot with write-through: in the boot menu's `S:BootMenu.conf` a line
   `Zenix (write-through) = run SYS:zenixboot DCACHE=WT`, or from an AmigaOS shell `zenixboot DCACHE=WT`; step 3 again.
5. Anything odd: boot with `DCACHE=OFF` (a menu line `Zenix (no data cache) = run SYS:zenixboot DCACHE=OFF`); if it
   goes away, the cache is the cause. The floppy's bootblock has no options: it always uses the default.

## 10. Numbers and interfaces

- **bootinfo +0x84 `kopts`** (frozen, grows at the end; an older loader leaves it 0 = the defaults):
  `KOPT_DCACHE_OFF` 1, `KOPT_DCACHE_WT` 2. zenixboot's `DCACHE=OFF|WT|CB` keyword sets it; an unknown word is an
  error before anything is loaded.
- No system call, message or file format changes. The kernel grew 2,000 bytes (`tools/ksize-budget`); the mount
  tests' RAM disk gave 2 KB for it (712 -> 710 KB, 41 blocks still free).
