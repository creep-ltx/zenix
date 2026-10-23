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
**Today:** there is nothing below any stack (limits.md). Kernel stacks are 4 KB and interrupts run on them.

**Idea:**
- Put a guard pattern at the bottom of every kernel stack and user stack.
- Check it in `schedule()`/`switch_to` and on trap entry, and compare the saved stack pointers against the known
  bounds.
- On failure, kill the process with "pid N overflowed its kernel/user stack", or panic if it is pid 0.

**Why:** an overflow becomes a clear message instead of random crashes. The cost is a compare or two per switch.

**Public source:** stack canaries and guard words are textbook practice.

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
- **Zenix as its own ROM: not pursued (owner's decision, 2026-10-23: "not for now, maybe never").** Zenix must run on
  Amigas in general, booted the normal way from a floppy or disk, not only on the owner's PiStorm. A clean-room hand-off
  note about ROM booting (2026-10-23) is kept with the owner's research notes, outside the repository. Two points from
  it still matter without a ROM:
  - Zenix borrows the Topaz 8 font from the running Kickstart at boot. Checked 2026-10-23: no Topaz data is copied into
    Zenix's files, and none may ever be (the font is Commodore's).
  - Emu68 also puts a standard flattened device tree (magic $D00DFEED) on its ROM board, a possible second way for
    stage0 to find the Pi's addresses.
- **The data cache without an MMU** on 68030/040/060 (audit Part 3 item 2): the transparent-translation registers
  (DTT0/DTT1 on the 040/060, TT0/TT1 on the 030, ACRs on the EC parts) can mark chip RAM non-cacheable and cache the
  rest, without paging. Public: the MC68040 and MC68030 user manuals in `Knowledge/manuals`, section "Transparent
  Translation". Needs measuring on hardware.
- **A boot menu on the small boot partition** (owner's idea, 2026-10-23): the Startup-Sequence runs a menu (Kickstart's
  own graphics, keyboard and mouse, a default with a countdown) offering Zenix (runs `zenixboot`) and one or more AmigaOS
  partitions. The reliable way to start another AmigaOS partition is to give it the highest boot priority for one boot
  and reboot (the menu restores the order next time), not to chain into its startup files. Until then, Kickstart's own
  Early Startup menu (hold both mouse buttons at power-on, Kickstart 2.0+) already chooses the boot partition. Pi-side
  choices (several FAT partitions, each with its own Emu68 and ROM, picked by the Pi firmware) are in the knowledge base
  note `amiga/emu68-and-pistorm.md`.
- **`zc`, Zenix Commander** (owner's idea, 2026-10-23): a text-mode two-panel file manager in the spirit of Midnight
  Commander - copy, move, delete, rename, view (with the pager) and edit (with the editor), a command line under the
  panels. Fits the owner's rules: works in 80x32 / 80x25 (no wide-screen layout), every key configurable (as Ahiru and
  CFile do), and loaded parts only when used. Natural after the shared text-interface library (and newlib); the owner's
  Ahiru and CFile (AmigaOS) have file-manager experience to borrow ideas from, but Zenix needs its own code (their
  shared core is built on AmigaOS).

## The kernel: the best idea from each system (owner's goal, 2026-10-23: "an outstanding kernel")
The owner wants the best of Exec, Linux, BSD, Minix, Fuzix, QNX, the Hurd and Darwin in one kernel. Principle 2 ("one way to do
a thing") still rules: for each area **one** idea is taken, the one that fits a 68020+ Amiga without an MMU, not all of
them. Ideas only: code may come only from GPL-2-compatible sources (Linux, Fuzix, BSD, Minix); Exec and QNX are
proprietary, so from them only the documented behaviour (the ROM Kernel Manuals; QNX's published papers).

| Area | Taken from | The idea | Zenix today |
|---|---|---|---|
| Scheduling | **Exec** | Priorities: the highest ready process runs, round robin within a priority. First thing to build after the halt (small, makes the terminal and servers feel quick); with a fairness test | round robin only |
| Servers and priorities | **QNX** | Priority inheritance: a server handling a message runs at its client's priority, so a high-priority program is never stuck behind a low one's request | - |
| Waiting | **Exec** | Signal bits per process (section 1 above); `select`/`poll` built on them | one source at a time |
| Event API for programs | **BSD** | kqueue-style: register many events once, get the ready ones in one call (a thin layer on the signal bits) | - |
| Messages | **QNX / Minix 3** | Synchronous send/receive/reply, pointers not copies | **done** |
| Reliability | **Minix 3** | The reincarnation server: a supervisor restarts a crashed server or driver (fsd, diskd, the network server) instead of the system stopping | a dead server stays dead |
| Devices | **Exec** | Servers loaded on first use (like `OpenDevice` loading from `DEVS:`), unloaded when unused | all start at boot |
| User-level servers | **the Hurd** | Translators: a program can serve a file or directory (an FTP site, an archive, `/proc`) and any user may attach one to their own files | mount is root-only |
| `/proc` and `/dev` | **Linux / Plan 9 via the Hurd idea** | System information as files, served by a translator, not by special system calls | special calls (`procinfo`, `meminfo`) |
| Memory | **Linux/BSD slab + Exec pools** | Slab caches for kernel objects (no fragmentation from many small objects); pools for programs | general allocator; size classes in libz |
| No-MMU discipline | **uClinux / Fuzix** | Relocated flat binaries, `vfork`/spawn, careful fragmentation rules; Fuzix's habit of keeping everything small | **done** (ZEXE, spawn) |
| Shared libraries | **Exec** | Libraries with jump tables, opened by name, one copy in memory (works without an MMU) | static only |
| Timers | **Linux** | One ordered timer list (or a timer wheel); sleeping processes cost nothing until due | scan of every process |
| Port security | **Darwin (Mach)** | Port rights: a process may only send to a port it holds a send right for, handed out by the kernel or by another holder. Replaces batch D's reserved names and kernel flag with one clean rule | reserved names + a kernel-only flag |
| Starting and keeping servers | **Darwin (launchd)** | One service manager that creates a server's port at boot but starts the server only on the first message, and restarts it if it dies (joins the Exec "on first use" and the Minix 3 restart rows) | all start at boot |
| Drivers | **Darwin (IOKit)** | A registry of hardware found (Zorro boards, the Pi's device tree) and drivers matched to it by properties, instead of each driver probing for itself | each driver probes |
| Users and permissions | **Unix/BSD** | The classic model, with the honest no-MMU limit | **done** (batch D) |

What stays deliberately out: `fork` (needs an MMU), Linux's size and its thousands of system calls, Exec's lack of
users and of resource tracking (Zenix keeps per-process ownership: everything a process held is freed when it dies).

Last checked: 2026-10-24.
