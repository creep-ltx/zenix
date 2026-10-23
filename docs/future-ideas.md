# Future ideas (after the feature halt)

Ideas the owner wants kept for later (decided 2026-10-23). **None of them is built**, and none may be built while the
feature halt holds (CLAUDE.md), except the stack guard, which the owner moved into the hardening work as a safety fix.
Each idea is grounded in public documentation: the ROM Kernel Reference Manuals and the Hardware Reference Manual, as
searchable text in the knowledge base (`Knowledge/manuals/`, read its INDEX.md, then grep), or textbook practice. The
first list came from another session's study of a classic microkernel's design, passed on as ideas only (clean room).
Zenix code is never written from anything but these descriptions, the public manuals and Zenix's own needs.

Facts about Zenix below were read from main on 2026-10-23; check them again before acting.

## 1. Signal bits per process: wait for many things at once
**Today:** a process waits on one thing at a time: one port, a timer, or one child. The only combination is
receive-with-timeout. There is no `select`/`poll`, and the shell polls the tty's interrupt flag.

**Idea:** each process gets two 32-bit words, *pending* and *waiting for*. Every source it cares about gets one bit,
allocated per process: each port it owns, tty input, a timer, child exit, Ctrl-C.
- An event ORs its bit into *pending*. If *pending AND waiting-for* is non-zero and the process is blocked, it becomes
  ready.
- A wait sets *waiting for*. If the AND is already non-zero it returns at once, so no wake-up is lost; otherwise the
  process blocks. On return it hands back the bits that fired and clears them.

**Why:** one AND decides every wake-up, `select`/`poll` become a thin layer on top, a terminal can wait for the
keyboard and the network together, and the shell stops polling.

**Care:** allocating and freeing bits per process; reserved bits for Ctrl-C, child exit and the timer; the
check-then-block must stay atomic against interrupts (audit B5).

**Public source:** this is how AmigaOS Exec works. See *ROM Kernel Reference Manual: Exec*, chapter "Signals" (page 19 of
that volume): `AllocSignal`, `Signal`, `Wait` and the mask that `Wait` returns. Exact calls are in *Includes and
Autodocs* (`exec.library/Wait`, `exec.library/Signal`).

## 2. Wake only the process that waits
**Today:** `wake_io()` (proc.c) walks every process slot and readies every process waiting for I/O, on any pipe or tty
event. Each one wakes up, checks, and sleeps again.

**Idea:** each object (a pipe end, a tty) records which process waits on it, and which bit to raise if idea 1 is built.
An event then wakes exactly that process. Without idea 1, a small list of waiters per object does the same.

**Public source:** per-object wait queues are textbook design (any Unix kernel's sleep/wakeup on a wait channel).

## 3. Stack overflow check at every task switch (approved as a fix, 2026-10-23)
**Before batch F:** there was nothing below any stack. Kernel stacks are 4 KB and interrupts run on them.

**Idea:**
- Put a guard pattern at the bottom of every kernel stack and user stack.
- Check it in `schedule()`/`switch_to` and on trap entry, and compare the saved stack pointers against the known
  bounds.
- On failure, kill the process with "pid N overflowed its kernel/user stack", or panic if it is pid 0.

**Why:** an overflow becomes a clear message instead of random crashes. The cost is a compare or two per switch.

**Public source:** stack canaries and guard words are textbook practice.

**Done** (audit batch F, 2026-10-23): architecture.md section 6 ("Stack guard").

## 4. CPU time per process and an idle counter
**Today:** there is no per-process CPU time, no idle counter and no profiling hook.

**Idea:**
- At each switch, read `hr_time()` (the CIA-B E-clock counter) and add the difference to the outgoing process's CPU
  time.
- Count pid 0's time as idle, and count the switches.
- Show both through `ps`/`top` as CPU% and idle%.
- Later: one hook pointer at the switch for profilers.

**Care:** the counter wraps after about 1.7 hours, so add up differences rather than storing absolute values.

**Public source:** the CIA timers are in the *Hardware Reference Manual* (appendix on the 8520 CIA) and the CIA data
sheets listed in `manuals/INDEX.md`.

## 5. Shorter stretches with interrupts off
**Today:** `schedule()` scans all process slots for sleepers with interrupts masked, and all of `proc_exit` runs with
interrupts off.

**Idea:**
- (a) Keep sleepers in a list sorted by wake-up tick, so the tick only looks at the head.
- (b) Separate "no task switch" from "no interrupts". The kernel is not preemptive, so most of `proc_exit` only needs
  not to be switched away from. Mask interrupts only around the fields that interrupt handlers also touch.

**Why:** long masked stretches cost serial and keyboard latency.

## 6. Deferred work through the software interrupt (needs care)
**Today:** `kwork()` runs from four places: the idle loop, the end of every system call, the level-2 interrupt when
it hit user mode, and between pieces of a long terminal write.

**Idea:** when an interrupt queues work, it sets SOFTINT in INTREQ. The hardware delivers it at level 1 as soon as the
mask allows, and `kwork` runs or is scheduled from there. The same trick gives "reschedule as soon as allowed" from
places that cannot switch right now.

**Care:** `kwork`'s re-entry guard; whether `kwork` may run at level 1 in the middle of a system call, since the kernel
is not preemptive.

**Public source:** *Hardware Reference Manual*, the interrupt chapter: INTREQ bit 2, SOFTINT, level 1.

## Lessons to test for
- **Freed memory really comes back.** Allocate, free everything, and check that the free total is back where it
  started. Do this for the kernel's `mem.c` and for the large-block regions of the user `malloc`. (A bug class that
  goes unseen for years when nothing tests it.)
- **Fairness.** If priorities are added, test that processes of equal priority switch only at the end of a time slice.

## Other ideas noted on 2026-10-23
- **Shared libraries:** one copy in memory used by many programs (AmigaOS-style jump tables work without an MMU).
  First candidates: libz, the regex engine, the line editor, a text-user-interface library for porting Sumi and Maki.
  The ZEXE format left room for it (DESIGN D3).
- **newlib** as a fuller C library, to make porting small Unix and Minix programs easy.
- **PFS3 support** written from the on-disk format (facts, not code), or as a separate filesystem server under its own
  licence. PFS3's code is 4-clause BSD, which cannot be mixed into GPL code; its authors could be asked to drop the
  advertising clause.
- **SD card log area:** once the SD driver is proven on the owner's card, save the kernel log to a small area of that
  card, so it can be read on a PC.
- **Zenix as its own ROM** (hand-off note "rom-boot-sequence", 2026-10-23, kept outside the repository with the
  owner's research notes): Emu68 loads a Zenix `kick.rom` and AmigaOS never runs. Dual boot stays possible through
  separate Pi FAT partitions, each with its own kick.rom, chosen with `autoboot.txt` or a one-shot reboot into the other
  partition (knowledge base `amiga/emu68-and-pistorm.md`). The note covers:
  - the ROM image layout;
  - the power-on sequence (overlay, silencing the chips, per-CPU setup, chip RAM sizing, warm vs cold reset);
  - Zorro autoconfig as a ROM must do it;
  - Emu68's flattened device tree on its ROM board (magic $D00DFEED), which a ROM-booted Zenix can scan for directly,
    without `devicetree.resource`;
  - an 11-step checklist for a Zenix ROM.

  Two things must happen first:
  - **its own font.** Zenix borrows the Topaz 8 bitmap from the running Kickstart at boot, and that font is Commodore's.
    Checked 2026-10-23: no Topaz data is copied into Zenix's files. A ROM build needs a free font, for example a
    public-domain 8x8 font.
  - **a copy to RAM before running**, because the kernel patches itself.
- **The data cache without an MMU** on 68030/040/060 (audit Part 3 item 2): the transparent-translation registers
  (DTT0/DTT1 on the 040/060, TT0/TT1 on the 030, ACRs on the EC parts) can mark chip RAM non-cacheable and cache the
  rest, without paging. Public: the MC68040 and MC68030 user manuals in `Knowledge/manuals`, section "Transparent
  Translation". Needs measuring on hardware.

Last checked: 2026-10-23.
