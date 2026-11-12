# D45: the boot's memory given back

Decided by the main session 2026-11-12 from the block 6 audit (docs/audit-2026-11-12-block6.md, section 2, items 1 and
2; recommended "yes" for the partner). A deep core change (the kernel's image and its allocator): this note first, two
independent reviews of it before the build and two of the code after (CLAUDE.md "Pace"). Revision 2, 2026-11-12,
branch `bootmem`, after the two design reviews (section 11). No interface changes (section 8).

## 1. In plain words

Part of the kernel runs once, while the machine starts, and never again: the probes of the disks and the SD card, the
set-up of the caches, the MMU, the screen and the clock, `kmain` itself, the `stage0` routine the boot loaders call
inside the kernel image before AmigaOS is gone, and the starter program `boot` that the kernel carries inside itself and
copies into a process of its own. Today all of it stays in memory for good. With this change it lies in one block at the
end of the kernel's loaded image, and when the boot is done (the moment the kernel's idle loop starts) that block is given
to the memory pool like any freed memory: about 24 KB more free RAM on every machine (measured: section 9). The same is
done with the CD drive's packet code on a machine where no CD drive answered at boot (about 0.7 KB).

A tool run at every kernel build fails the build if anything that runs after the boot could reach the given-back code.
The given-back memory is first filled with `ILLEGAL` instructions, so a missed path would stop at once with the
kernel's exception line naming the address, never run stale code.

## 2. What goes in the boot span

The span `.zinit` holds code and read-only data that is used only while `kmain` runs (and, for `stage0`, before the
kernel runs at all). Chosen by reading the callers, confirmed by the build-time check (section 5):

| Part | Where | Why only at boot |
|---|---|---|
| `kmain`, `vectors_init`, `eclock_init`, `timer_init`, `mem_selftest`, `cpu_name` | `kernel/main.c` | called once, from `start.S` / `kmain` |
| `mem_init`, `add_minus`, `add_range`, `upd_reserve` | `kernel/mem.c` | the pools' set-up |
| `ide_init`, `cd_probe`, `ide_ident_cmd`, `id_model` | `kernel/ide.c` | the port's probe (ZINE's way back calls `ide_zine`, which is not boot-only) |
| `emmc_init` and its card commands (`acmd`, `mbox_call`, `delay_us`...) | `kernel/emmc.c` | the SD card's start-up |
| `a3000_init`, `blk_init`, `proc_init`, `tty_init`, `serial_init` | their files | set-up, once |
| `cache_setup`, `cache_block`, `mmu_setup` and its helpers | `kernel/cache.c`, `kernel/mmu.c` | the CPU's set-up, once (D37, D41) |
| `gfx_init`, `gfx_font_init`, `font_extract` and the depth's choice | `kernel/gfx.c` | the screen's start (ZINE gets the screen back through `gfx_zine`, not `gfx_init`) |
| `klog_init`, `klog_to_screen`, `klog_show_prev`, `slot_valid` | `kernel/serial.c` | the previous boot's log, read once |
| `rnd_init`, `rtc_read` and its checks (`wait_lines`...) | `kernel/random.c`, `kernel/rtc.c` | the pool's first samples, the battery clock read once |
| `boot_blob` (`boot.zx`, 7.7 KB) | `kernel/embed.S` | copied into pid 1 by `proc_spawn_image` (`fill_mem`, a `memcpy`) before the release; never spawned again (svc.c's images are `fsd` and `diskd` only) |
| `stage0_blob` (3 KB) | `kernel/start.S` | called by `handoff.S` on the loaded file while AmigaOS lives, found through the ZEXE header's +28 offset; never by the kernel |

The final list is what the build-time check accepts; the design commits to the mechanism and the rules, not to every
name above (a function that turns out to be called later stays resident; the check says which).

Not in it: string literals (gcc puts each file's in one `.rodata.str` section, shared by boot and runtime code: they
stay resident), static data the boot fills (`vector_table`, the log's buffers, `id`: live after the boot), and
`idle_loop` (it never returns, but runs forever).

## 3. The mechanism

**Link.** `link/kernel.ld` gets output sections between `.data` and `.bss`, inside the loaded image (so `kernel.bin`
carries them, nothing is loaded twice, the relocation pass covers them as before). Names of our own (`.zinit`, not
ELF's reserved `.init`, which a stray crt or libgcc section would join):

```
.zinit ALIGN(16) : { __zinit_start = .; . += 32; *(.zinit.text) KEEP(*(.zinit.stage0)) }
.zinitd : { *(.zinit.rodata) . = ALIGN(16); __zinit_end = .; }       /* the blob: data, not code */
.zhwcd ALIGN(16) : { __zhwcd_start = .; . += 32; *(.zhwcd.text) . = ALIGN(16); __zhwcd_end = .; }
```

`stage0`'s input section is renamed `.zinit.stage0` in `start.S` (ld puts an input section under the first rule that
matches it: left as `.text.stage0`, `.text`'s `*(.text*)` would take it, and with no KEEP there `--gc-sections` would
drop it; `mkzexe.py` now refuses a kernel without `stage0_blob`). The blob is a separate non-code output section so
`check-68020-60.sh`'s disassembly never reads `boot.zx`'s bytes as kernel code.
The first 32 bytes are room for the pool's block header (16 bytes at the first 16-byte boundary: every loader puts
the kernel's home on a 256 KB boundary - the bootblock's `handoff_tables`, zenixboot and the demo floppy's stage 2 by
`boot/bootmem.h` `pick_home`, the chip fallback 0x40000 - so it is the section's start; `mem_init` still rounds up at
run time, and 32 covers any 2-byte alignment). Functions are put there with `KINIT`
(`__attribute__((section(".zinit.text"), optimize("Os")))`, in `kernel/kernel.h`; section 12 says why not
`noinline`); the blob with `.section .zinit.rodata,"a"`. `KHWCD` for `.zhwcd.text` adds `noinline` (a static function
with one caller is otherwise inlined into its resident caller whatever its section, and the span would be empty).

**Reserved at `mem_init`, as the update bundle is (D15).** At the end of `mem_init` (before `mmu_setup`, which builds
the MMU's tables over the ranges that exist then: both spans must be reserved here, never later), each span `[lo, hi)`
(lo the first 16-byte boundary at or after its start, hi its end rounded down to 16) becomes a range of its own
holding **one block in use**, owner 0 (the kernel), its header in the span's pad: `add_range` (whether it added the
range is seen in `nranges`), then the used bit and the free counter, the way `upd_reserve` does it. Its chip flag is the one of
the bootinfo region that holds the kernel (`kernel_pa`): chip RAM on a chip-only A1200 (home 0x40000), fast elsewhere;
a fast span on a chip-only machine would make ZINE's planner refuse every start (`zp_plan`'s chip check) and let fast
requests get chip RAM. The range is **appended** at the end of the table (the table follows exec's memory list, not
addresses, and its order is the allocator's preference: the spans are taken last and never take a slot before real
memory). A pointer per span (`span_blk[0]`, `span_blk[1]`, as `upd_blk`) is set only when the reservation succeeded; if the
range table is full (16 ranges; a machine uses about 4-8) the span is not reserved, stays resident, and the release
does nothing for it (a `mem_free` of an unreserved block would panic). From then on, everything that looks at the
pools sees it: the MMU's tables, ZINE's walk and plan (one used block), `mem_owner_of` ("the kernel's memory").
`mem_totals` counts the spans: `free`'s total and ZINE's totals are larger by them from the boot on.

**Given back when the idle loop starts.** `kmain` ends by calling `idle_loop`, which never returns. Its first act
(before its loop) is `mem_boot_release()` (`kernel/mem.c`, resident): for the boot span, and for the CD span when
`ide_cd_present()` says no drive answered: fill the block's data with `0x4AFC` (`ILLEGAL`), push the caches and
invalidate the instruction cache (`cache_flush_code`, the loader's routine), then `mem_free` the block (D41: with
protection on, `free_blk` makes its whole pages invalid, as for any freed block; they already were). One line on the
log: `mem: boot code given back: N bytes of fast RAM (free A -> B)` (or chip RAM, by the span's range), and for the CD
span its own. At that moment the
boot process exists but has not run (no `schedule` yet); interrupts are on, but no interrupt handler allocates
(CLAUDE.md), so nothing can be given the memory before the fill ends.

Nothing executes in the span after the release: `kmain`'s own code ends with the call into `idle_loop` (a resident
function, which never returns into it); `start.S` calls `kmain` once and has nothing after it but a loop that is never
reached.

## 4. Code kept only when its hardware is there

**The CD drive (built).** `ide_atapi` is split: the resident `ide_atapi` checks `cd_sel` (0: `-ENODEV`, as today)
and calls `atapi_run`, the packet mover, in `.zhwcd`. `cd_sel` is set only by `cd_probe` (boot only) and never changes
after the boot, so with no drive `atapi_run` can never be called: its span is given back. With a drive it stays (its
block stays in use, never freed). The check allows exactly this one entry into `.zhwcd` (`ide_atapi` -> `atapi_run`).

**The MMU's runtime code (not built; a follow-up).** The audit's 4.3 KB (`mmu.o`'s flips, lends, fault decoding,
switch) is called from 17 places in 9 files, and today each function tests `mmu_on` itself (`mmu_fault` is called
unconditionally from `exc_c`). Freeing it when protection is off needs that test moved to every call site, in hot
paths (`schedule`, `mem_alloc`, `free_blk`, `ipc_recv`), and the build-time check cannot prove a call site's test - a
reviewer would have to, for every future hook. Not "simple and safe" in the sense of the task; left as a follow-up
with this note's mechanism ready for it. The pattern for it (review A): the runtime functions renamed with a trailing
`_` in a `.zhwmmu` span, each called only through a `static inline` gate in `mmu.h` (`if (mmu_on) f_(...)`); a
source check that no file but `mmu.h` and `mmu.c` names an `_` function; `mmu_on` set only in `mmu_setup`. It matters
most on the floor machine (a 68EC020 has no MMU: all of it is dead there), so it is the next step.

## 5. The build-time check (`tools/check-init.py`, run on every kernel ELF the Makefile links)

The kernel is linked with `-q` (every relocation kept in the ELF, as `mkzexe.py` already reads). The tool reads the
ELF itself (no extra Python package) and runs inside the recipe every kernel image goes through: `kernel.bin` and its
variants (DIAG, GFXDEPTH, the test builds), and `kernel-zfs.bin` (its own objects). It fails the build, naming the
place, when:

1. **A relocation outside a span points into it.** For every relocation in a loaded section (`R_68K_NONE` skipped),
   the target is in a span if its symbol is defined in a span section, or (for section symbols and others) if the
   address symbol value + addend falls inside it; the place outside that span is an error unless the pair (symbol
   containing the place, target symbol) is on the tool's short list and the place is a call or jump: `start.S`'s call
   of `kmain` (a label `enter_kmain` names that place) and `ide_atapi` -> `atapi_run` (the bounds are used only by
   `mem_init`, itself in the span). This covers
   calls, jumps, tail calls, function pointers stored in data (a table in `.data` or `.rodata` pointing at boot code),
   and address-taking in resident code. The `.zhwcd` span may not point into `.zinit` either (it runs after the boot).
2. **Boot code takes the address of boot code.** Inside a span, a relocation that targets a span's *code* must sit at
   a call or jump (`jsr`/`jmp` absolute `4EB9`/`4EF9`, `bsr.l`/`bra.l` `61FF`/`60FF`, `bsr.w`/`bra.w` `6100`/`6000`,
   `jsr`/`jmp` `(d16,pc)` `4EBA`/`4EFA`, `Bcc.w`/`Bcc.l`); a `lea fn,%aN` or `move.l #fn,%dN` is also accepted when
   what follows in its function (read linearly in the span's disassembly) only calls through the register, copies it
   into an address register (followed in turn) or overwrites it with a plain load (`move`, `moveq`, `lea`, `clr`, a
   `movem` load): gcc -Os's "function cse" of repeated calls. Any other use (arithmetic, a store, a push, a `movem`
   store, a return or a jump out of the function while it is held) is an error. The disassembly also catches a
   PC-relative `lea`/`pea` inside one input section (no relocation is written for those). The boot span's data (the
   `boot` program) may be addressed only by the uses listed (`kmain`'s two, handed to `proc_spawn_image`, which copies
   it). Anything else could put a pointer into the span somewhere that outlives the boot, and is an error.
3. **The spans' layout.** `__zinit_start`, `__zhwcd_start` 16-aligned, their first 32 bytes zero; `stage0_blob` and
   `boot_blob` inside the boot span; `atapi_run` inside `.zhwcd`; no writable section inside a span.

What it cannot see, and the rules that cover it: boot code storing the address of boot-only *data* somewhere lasting
(the only boot data is `boot_blob`, passed to `proc_spawn_image`, whose `fill_mem` copies it before returning) -
reviewed, and the `ILLEGAL` fill makes any mistake loud; inline asm jumping by a computed address (none in the
kernel's C).

## 6. Every CPU, the caches, Emu68, the MMU

- **68020/030:** the fill is data writes; `cache_flush_code` sets CACR's clear-instruction-cache bit, so no stale
  instruction cache line of boot code survives. Nothing runs there afterwards anyway; the flush makes a missed path
  meet `ILLEGAL` instead of an old cache line.
- **68040/060:** `cache_flush_code` is `cpusha bc` (D37's rule: never `cinva`), pushing the fill out of a copyback
  data cache and dropping instruction lines; on the 68060 a CPUSH that includes the instruction cache also clears the
  branch cache (MC68060UM 5.11), so no prediction into the span survives either.
- **Emu68:** its translated blocks are found by address until the next instruction-cache flush, then re-checked
  against the 68k bytes (knowledge base `amiga/emu68-jit-cache-rules.md`). The flush after the fill makes every old
  translation of the span invalid. Later code put there (a program) is flushed by the loader as always.
- **Memory protection (D41) on or off:** the span's range exists before `mmu_setup` builds the tables, so its pages are
  in the tree; until the release they are kernel memory (invalid for protected programs; the kernel reaches everything
  through the transparent translation registers). A `MEM_USER` block later cut from the range is whole pages inside
  the range (alloc_in's page rule), so no protected program ever sees the kernel's bytes in the span's partial first
  or last page. Off, nothing of D41 runs.
- **A 68030 with AmigaOS's translation left on** (A6-14): unchanged; `mmu_setup` still switches it off, now from
  `.zinit`, before the release.

## 7. ZINE (D29)

ZINE's blob gets the range table and walks the blocks (`user/zine/blob.c`); the span's range is one more range with
blocks that tile it, inside the region that holds the kernel, overlapping no other range: `zp_plan`'s checks hold. Its
free blocks after the release are free fast (or chip) memory the AmigaOS program may get, like any other. `zine_args`'
`kernel_pa`/`kernel_mem` (the kernel's whole span, today unused by the blob) now include the given-back span: the
planned checksums (d29 section 4.4, not built) must sum the kernel image outside the spans; said in `kernel/zine.h`.
ZINE's way back (`zine_resume`, `ide_zine`, `kbd_zine`, `fd_zine`, `input_init`, `gfx_zine`) uses no boot-only code
(the check proves it).

## 8. Interfaces

Nothing frozen changes. `kernel.bin` stays a ZEXE kernel image with the same header (the `stage0` offset at +28 now
points further in; every loader takes it from the header). No system call, message, struct, error number or file
format. `free`, `SYS_meminfo` and ZINE's totals report the larger pool (the spans count in the total from the boot on;
with a CD drive its 0.7 KB stays a used block of the kernel's). No claims.

## 9. Costs and the measured gain

- `kernel.bin`: a few hundred bytes more (the 2 x 32-byte pads, the reserve, the release, the split of
  `ide_atapi`): measured at the build and said in `tools/ksize-budget`.
- RAM: the measured span sizes go here and into `docs/limits.md` after the build.
- Time: the fill of about 24 KB once per boot, under 10 ms on a 14 MHz 68020 in chip RAM.

## 10. Tests

- `tools/check-init.py` at every build (section 5), plus a self-test of the tool: a copy of `kernel.elf` with one
  relocation retargeted into the span must fail (`tools/check-init-test.sh`, host).
- `tools/bootmem-test.sh` (FS-UAE): boots the test floppy on the 68020 config and, with `MMU=ON`, on the 68030 and
  68060; reads the release line and checks that the bytes given back equal the span sizes read from `kernel.elf`, that
  the free count grew by exactly that, that the CD span is given back without a drive, and that the system then runs
  programs (memory taken from the span among them).
- The core suite: mounttest, svc-test, zenixboot-test, mmu-test (030 and 060), cd-test (a drive: the CD span kept),
  ide-speed-test, zroot-test, the matrix (every CPU 68020-060) and update-test (the bundle reserve beside the spans).
- On real hardware (not possible from the build machine; for the partner's next hardware session): one boot on the
  PiStorm A1200 (Emu68's JIT with the fill and the flush) and one on the Apollo 1260's 68060, each showing the
  release line and a working desktop.

## 11. Reviews of the design (2026-11-12, two independent reviewers)

No high finding about the release itself: both found every listed function reached only from the boot, the release's
moment safe (the kernel is never preempted; the boot process has not run), the D41 tables and page rule, ZINE's plan
and the update bundle sound. What revision 2 changed for them: stage0's section renamed (ld's first-match rule would
have dropped it silently: A1, B3); the span's chip flag from the kernel's region (B2); ranges appended, a pointer per
reserved span and no release without it (A5, B5, B6); `ALIGN(16)` on the sections and names of our own (A9, A11, B7,
B10); the blob in a non-code section (A7, B8); `noinline` on the split CD function (A4, B9); the check classifies by
symbol and address, skips `R_68K_NONE`, runs on every kernel variant and `kernel-zfs`, and accepts gcc's `lea` +
`jsr (aN)` while scanning the span's disassembly for PC-relative address-taking (A2, A3, B1, B4); both reservations
inside `mem_init` (A8); the 060 branch cache's reason corrected (A10); the pool totals said (A6); real-hardware boots
added to the tests (A13). The MMU part stays a follow-up (both agree), with A's gate pattern in section 4.

## 12. As built (2026-11-12)

- **Measured** (`make`, `tools/bootmem-test.sh`): the boot span 25,296 bytes (its code 17.5 KB with stage0's 3 KB, the
  `boot` program 7.8 KB), the CD span 752 bytes; on the stock A1200 the chip RAM free count went 1,707,120 ->
  1,732,416 at the release, and the CD's 752 bytes after it: **26,048 bytes more free RAM** on a machine without a CD
  drive. `kernel.bin` 193,112 -> 193,716 bytes (+604: the reservation and the release in `mem.c`, the 64 bytes of
  pads, the CD split); 844 bytes stay on the mount tests' floppy (budget 800). The DD demo floppy 60,928 -> 59,904
  bytes free.
- **What went in** (`KINIT`, the build's check accepting each): `kmain`, `vectors_init`, `eclock_init`, `timer_init`,
  `mem_selftest`, `cpu_name`; `mem_init`, `add_range`, `add_minus`, `upd_reserve`; `ide_init`, `ide_present`,
  `ide_ident_cmd`, `ide_identify`, `gayle_id`, `custom_mirror`, `id_model`, `cd_probe`; `emmc_init`, `mbox_call`,
  `delay_us`, `acmd`, `set_clock`; `a3000_init`; `blk_init`; `proc_init`; `serial_init`, `klog_init`, `slot_valid`,
  `text_valid`, `klog_to_screen`, `klog_show_prev`; `gfx_font_init`, `plausible`, `font_extract`, `gfx_init`;
  `rnd_init`; `rtc_read`, `wait_lines`, `two`, `date_ok`; `cache_setup`, `cache_block`, `mark`; `mmu_setup`,
  `off_why`, `table`. Left resident though boot-only: `cache_plan.h`'s inline helpers (shared with the boot loaders),
  `tty_init` (empty), a clone of `diag_stage`.
- **`KINIT` is `section(".zinit.text"), optimize("Os")`**, without `noinline`: a boot helper inlined into a boot
  function is in the span anyway, and one inlined into a resident function would only lose its saving (no reference
  is left). `noinline` and `optimize("no-function-cse")` together cost about 600 bytes more of `kernel.bin` (measured); instead the
  check follows gcc's calls through a register (section 5). `KHWCD` keeps `noinline` (the split must stay split).
- **The check** (`tools/check-init.py`): 4,819 relocations of `kernel.elf` (4,820 of `kernel-zfs.elf`), none into a
  span from outside; `tools/check-init-test.py` makes four damaged copies (a resident call retargeted at `kmain`, a
  `jsr` of boot code made a `pea`, a register holding boot code's address pushed, the pad not zero) and each is
  refused.
- **Reviews of the code** (2026-11-12, two independent reviewers, one of them building deliberately bad kernels): no
  finding in the kernel change itself (every caller of every `KINIT` function, the reservation's arithmetic, the
  release's order, ZINE's walk and plan, the MMU's tables, the linker script, `kernel-zfs`). In the check: two false
  passes shown with real builds - a boot-code address returned in `%d0` by a boot function (gcc's `move.l #fn,%d0;
  rts`), and arithmetic on a held register taken for an overwrite - fixed by the stricter rule above (B1, B2, A3);
  span data not checked (A1, B3: the `ALLOW_DATA` list); the allowed pairs accepted any instruction (A2: now calls
  only); stage0's window a fixed 4 KB (A4, B4: the label `stage0_blob_end`); the doc behind the code (A6, B5). Each
  case is now in `tools/check-init-test.py` (eight damaged copies, each refused). The host allocator test
  (`tools/mem-test.sh`) had not linked since D41 (A5: the MMU hooks undefined): fixed, with the D45 cases (the
  reservation, the release, the CD span kept with a drive, a full range table). Not done: one range for both spans
  (B6, 16 bytes and a range slot; optional).
- **Tests:** see section 10; the results are in the commit message.
