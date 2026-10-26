# Zenix: how to work on this project

Zenix is a small Unix-like OS for 68020+ Amigas without an MMU. It runs on the Amiga itself (no AmigaOS, no Linux
underneath). Read in this order: this file, `docs/ROADMAP.md` (vision and phases), `docs/audit-2026-10-22.md` (the
current work list), then `docs/architecture.md` for the part you touch.

## The owner
- Not a programmer: explain in plain words, make the technical calls yourself, ask only about what Zenix should *be*.
- Often on the phone. Short status lines, no walls of text.
- Owns one Amiga: an A1200 with a PiStorm32-lite (Emu68 on a CM4) and a Gotek. **That machine is the first test machine;
  real Amiga hardware is the ultimate target (owner, 2026-10-24): nothing in the base may depend on Emu68 or the Pi, and
  speed is judged on real 68k CPUs (today a stock 14 MHz 68EC020), not Emu68's JIT. The minimum requirements
  are not decided yet (ROADMAP, open questions).** Order audit fixes by what
  matters on it first. Zenix runs on it since 2026-10-23; real-hardware time is scarce, so every build sent there must be
  ready for it.

## Current focus (2026-10-22): hardening, then the first real boot
**FEATURE HALT (decided by the owner, 2026-10-22): no new features - no new commands, system calls, drivers,
options or programs.** Only fixes from `docs/audit-2026-10-22.md`, tests that check them, and documentation
corrections. Merging work that was already built before the halt (the `drivers`, `zenixfs` and `floppy` branches) is
allowed. **Approved exceptions:** booting without a floppy (`zenixboot`, owner 2026-10-23); DESIGN D10 priorities (owner
2026-10-24), to start only after the audit fixing is done and the closing audit report is written.
The boot menu and the dual-boot SD card (owner 2026-10-24: a small boot partition with a menu, the owner's AmigaOS
on PFS3 partitions, Zenix root and /home); the menu program lives with `zenixboot` in `boot/`.
Rebooting between the systems (owner 2026-10-24): `bootzenix` in AmigaOS, `reboot [amigaos]` / `bootamigaos` in Zenix,
through a one-shot note the boot menu reads. Later (not now): a Wasabi-like remote-access daemon for Zenix (needs the network driver).
DESIGN D11 service manager (owner 2026-10-24: design reviewed twice and approved, docs/d11-service-manager.md).
`awk` (owner 2026-10-24: finish the parked `user/awk.c` now, while D11 is built).
**Colour console approved (owner 2026-10-25):** colours in the text console (ANSI colours, for `ls`, `grep`, the prompt,
`less`); a design note first (a DESIGN number, reviewed), then built. **The shell's memory** (a fix, audit 2026-10-25
'found while fixing'): sh grabs a fixed arena of 64-256 KB per shell and parses a whole script at once; make it grow on
demand, give memory back, and read scripts a piece at a time.
**The order of work is approved (owner 2026-10-25): docs/ROADMAP.md "The order of work" - always the next item when
one is done; and the versioning model there ("Versions and updates").**
**Pace (owner 2026-10-25):** steady progress every day, done properly, never hasty: "an absolute epic operating system
for the Amiga". Every day ends with something finished and merged, and a short status for the owner. Care matches risk:
two independent design reviews for deep core changes (kernel, loader, updates), one for features; every change has tests;
every merge gets the full suite; a focused audit after each big block, its findings fixed at once.
**The week of 2026-10-26 (owner at work until Friday 2026-10-30; the order, not the speed: when a step is done the next starts at once):** Sun D13 merged, shell memory fix started; Mon shell
fix merged, colour console designed; Tue colour console built; Wed self-updates design (two reviews); Thu self-updates
building starts, and the owner's Friday package ready (card update, DoM image, checklist, cable and cu instructions).
**Order after D13 (owner 2026-10-25: "fix things when we find them"):** first the shell's memory fix, then the
self-updates design note (then build), the colour console, then the network driver. Rule from then on: a problem found
during other work is fixed next (or now if small), not parked behind new features; the owner's weekend 2026-10-30/31: card update, serial cable and `cu`,
the DoM on the stock A1200 and the Apollo 060 (D13 measurements).
Next approved, in this order after D11 (owner 2026-10-24): **D13 shared libraries and shared program code** (one copy in
memory of libz and of a program several processes run, as AmigaOS libraries and pure programs; design note first,
reviewed, before code; it comes before the self-updates so the updater ships the final program format), then Zenix
installing updates by itself (no card swaps), then the network driver.
**Standing order (owner 2026-10-24, away from home):** when D11 is merged, first a **focused audit** of everything
changed since the audit closed (D10, the boot menu and reboot, D12/cu, awk, G1/G3/G4, D11): one independent reviewer,
findings fixed, tested and merged; then start D13 without waiting for the owner:
review the design note (an independent review, fixes, a second look), then build stage 1 step by step in FS-UAE. The
A1200 measurements (note section 8) wait until the owner is home; nothing is written to the owner's card meanwhile.
**The owner's own goal (2026-10-24): Claude in a terminal on Zenix on the A1200.** It sets the order of the work, but it
is the owner's goal, not Zenix's purpose (owner, 2026-10-24): never present it as Zenix's main goal in the docs. Approved now: a serial terminal program (Zenix to the PC over the null-modem cable, from
the weekend of 2026-10-31); then the same over the network; then a console good enough for Claude Code's screen
(colours, stand-ins for its Unicode symbols).
Only the owner lifts the halt; a session never decides on its own that it is over. Unfinished work stays
parked. Order of the fixes: batches A and B, then the first PiStorm boot, then C,
D and E.
1. **Batch A** - first-boot blockers (cache flushes, `cpusha`, `-m68020-60`, keyboard timing, Amiga Swedish keymap,
   the RAM-disk image, bootinfo/Gayle/floppy motor). Without these the PiStorm boot fails.
2. **Batch B** - memory foundations (libz `malloc`/`free`, `mem_alloc` overflow, interrupts only queue input, the
   lost wakeup, stack-hungry tools, the ZEXE loader checks).
3. Then the first boot on the PiStorm with `build/zenix-ram.adf`, following the checklist in the audit (Part 4).
**Text console = Unix/Linux style (owner, 2026-10-23):** ten virtual consoles on Amiga+F1..F10, each with its own login
prompt; no bar, no Amiga+Return, no workspaces outside the future desktop. Never add desktop features to the text console.
Mark each fixed item in the audit file in the same commit as the fix. When a batch is done, say so in ROADMAP.md.
After the first real boot, the next goals are: a terminal the owner can live in on the real machine, then ZenixFS as
root, then the desktop (ROADMAP phases).

## Sessions: one main session, at most one side session
Parallel sessions collided before (two programs claimed system call 75; the Makefile conflicted; the floppy filled up
unseen). So:
- **One main session** works in the main checkout on `main`. Only it edits the shared files: `kernel/kernel.h`
  (system call numbers, limits), `kernel/main.c` (boot order, the 100 Hz tick), `Makefile` (UAPPS, RAMAPPS, image
  sizes), `user/usys.h`, `user/libz.*`, and the counts in `docs/`.
- **At most one side session**, in its own worktree and branch, on a part that touches none of those (good: the
  ZenixFS library and host tools in `zenixfs/`, a host-side driver model test). It merges main into its branch every
  day and is merged back within a few days, not weeks. If it needs a shared-file change, it asks the main session
  (a message to it) or puts the change in one small, separate commit that is merged first.
- Existing branches (`drivers`, `zenixfs`, `floppy`) are merged in this order: main's own staged work, `drivers`,
  `zenixfs`, `floppy`. After every merge: rebuild every image, check the floppy sizes, run the tests.
- Never `git stash`, `git checkout .`, `git reset --hard` or `git add -A` in a shared tree. Commit your own files by
  path (`git commit -- path...`).

## Numbers that are claimed (update here before using a new one)
| System calls | Owner |
|---|---|
| 1-77 in use except 39, 40, 68 (removed with the workspaces in batch E; free again) | main, all merged (see `kernel/kernel.h`) |
| 78 `getpriority`, 79 `setpriority` (DESIGN D10, priorities) | main, merged 2026-10-24 |
| 80 `reboot` (save the disks, reset; optionally leave the boot menu a one-shot choice) | main, 2026-10-24 |
| 81 `svc` (DESIGN D11, the service manager: register, remove, self, save, load, opens) | main, merged 2026-10-25 |
| 82 `update` (DESIGN D15: INFO, RELEASE, NOTE) | built on branch d15, 2026-10-26 |
| **next free: 83** | |
Same rule for message types in `kernel/fsproto.h` / `kernel/diskproto.h` and for port names: claim here first.
Message types claimed so far: `FS_*` 1-24 (24 `FS_CLOSE`: fix-c, batch C5), `FS_*` 25 `FS_MARK` (D15 step 2, 2026-10-26:
zup's "needs a check" mark), `DISK_*` 1-8, `DISK_*` 9 `DISK_INFOSZ` and
10 `DISK_LISTSZ` (D15 step 0, fix3, 2026-10-26: `DISK_INFO`/`DISK_LIST` with the caller's buffer size).
Error numbers: `ECONNRESET` 104 (D11, "the server is gone"; main); `ETXTBSY` 26 (D13, "text file busy": a running
shared program opened for writing; claimed 2026-10-25, Linux's number). Update note kinds (D15, frozen, append-only):
1 done, 2 failed, 3 confirmed; 4 "confirmed, but the root half failed" (audit3 B1, 2026-10-26). Port names
registered as services (D11):
`disk`, `fs`, `fs.*`; `ping` (the test service).

## Frozen interfaces (D15 step 0, 2026-10-26; checked in every review)
Updates install a new kernel before the programs (docs/d15-self-updates.md 2 and 4), so an older program always meets
a newer kernel and newer servers. So these never change meaning; they only grow, at the end:
- **Append-only:** system call numbers and their argument meanings; error numbers; message types and their argument
  meanings (`kernel/fsproto.h`, `kernel/diskproto.h`). Never reused, never changed. A new argument goes where every
  older caller passed 0, or into a new number (claimed in the table above first).
- **Structs the kernel or a server writes into a program's memory** (`ustat`, `uproc`, `ustatfs`, `udirent`,
  `umountinfo`, `diskinfo`, the `SYS_meminfo` words) grow only at the end, and the caller passes its buffer size and
  gets at most that many bytes: `SYS_stat`/`lstat`/`fstat`/`readdir`/`procinfo`/`statfs`/`mountinfo` take it in their
  third argument, `SYS_meminfo` in its second; size 0 (every program built before D15) gets the struct's first size
  (`uproc`: 32 bytes), a size below that is -EINVAL; `DISK_INFOSZ`/`DISK_LISTSZ` take it in `a[2]` (the old
  `DISK_INFO`/`DISK_LIST` write the first 80 bytes). The structs servers answer in (`ustat`, `udirent`, `ustatfs`)
  are frozen at their size in the message protocol: a bigger one needs a new message type. libz passes `sizeof`.
- **Frozen:** the program start contract (the stack at entry, `a5`); the ZEXE and ZEX2 formats (the loader keeps both
  for ever); the bootinfo layout; the klog area and boot-note layout and the klog flags (zenixboot and bootmenu
  against the kernel); the `/etc` formats base programs read (svctab, fstab, passwd, group); the service manager's
  `SVC_*` numbers and limits (`SVC_TEXT` 80, `SVC_SAVED` 512, `SVC_MIDREQ`, exit status 86 for a damaged table), the
  port names (`disk`, `fs`, `fs.*`) and the built-in `@name` programs; the tty mode bits (`TTY_RAW`, `TTY_NOSIG`),
  `O_*` flags and mode bits, the spawn limits (argument count and bytes), exit and wait status conventions (139 for a
  fault, `waitcon`'s console status); the boot note's kinds and `SYS_reboot`'s arguments; D15's own formats (the
  bootinfo bundle fields, the update note, the state file, the bundle header and manifest, zup's journal and undo logs).
- Not on the list: the kernel's internal file kinds. Signal numbers join it when signals come. libz is linked into
  each program, so it needs no rule of its own.

## Rules for code (each one was a real bug found on 2026-10-22)
Real hardware and Emu68 (details: knowledge base `amiga/emu68-jit-cache-rules.md`):
- **After writing code that will run** (loading a program, relocating, copying the kernel): flush the instruction
  cache - CACR bit 3 on 020/030, `cpusha bc` on 040/060. Emu68 runs stale translated code otherwise.
- **Never `cinva` the data cache** (it discards unwritten data on Emu68); use `cpusha`.
- **No delays made of empty loops.** Emu68 runs about 1000 MIPS. Time with the CIA (E-clock reads or a timer).
- **Every wait on hardware has a timeout** and says on the serial port what it waited for.
- Build for every CPU: `-m68020-60` (a real 68060 has no 64-bit `mulu.l`). Check new code with objdump if unsure.
- Interrupt handlers only take the byte, put it in a ring and wake someone. No drawing, no line discipline, no
  allocation in an interrupt.
- Check-then-block (a reader testing "is there input?" then sleeping) keeps interrupts masked across both.
- PAL and NTSC both: never hard-code a PAL-only timing or line count.

Memory without an MMU (an overflow corrupts other programs, not only yours):
- No big arrays on the stack (stacks are 16 KB, no guard). Directory lists and paths come from the heap through the
  shared libz helpers; add a helper to libz rather than copying one into a program.
- Check every allocation; memory calls return 0 (NULL) on failure, never -1.
- Everything read from a disk, a message or a program header is untrusted: range-check block numbers, lengths,
  offsets and counts before use. A corrupt disk may fail a request, never crash a server or write outside.
- A limit that is hit says so (an error message and a non-zero exit), never silently drops data.
- Use errno numbers (negative) for errors in new code; do not add new bare `-1` meanings.

Disks and data:
- **Never write to the owner's PiStorm SD card or any disk with AmigaOS partitions** without a plan the owner agreed
  to. The SD card holds Emu68 and the Kickstart. Test disk tools on scratch images only.
- ext2: refuse filesystems with features we do not support; check with the host's `e2fsck -fn` after every
  filesystem test.

## Testing
- Nothing is done until a test *checks* it (the output is compared, not just printed). "verified" in the docs means
  such a test exists and passed in FS-UAE; on hardware say "verified on the A1200" with the date.
- Emulator timing is not hardware timing: no speed claims from FS-UAE.
- Run FS-UAE headless only, never on the owner's screen. Up to 6 FS-UAEs may run at once on this PC (owner,
  2026-10-26), all sessions together: every run takes a slot (`tools/run-test.sh`, `ZENIX_EMU_SLOTS`, lock files
  `/tmp/zenix-emulator.lock` and `.1`-`.5`) and needs its own serial port, image files and output folder:
  `ZENIX_SERIAL=/tmp/<name> ZENIX_BUILD=/tmp/<name>-build ZENIX_OUT=/tmp/<name>-out tools/<test>.sh` (a copy of
  `build/`), or `make BUILD=/tmp/<name>`. `make check` runs its tests that way by itself. No new emulator while the
  machine's 1-minute load is above 16.
- Before committing: `make` (all images, including `build/zenix-ram.adf`), the tests for what you touched, and
  `tools/docs-check.sh --staged` (must say `docs-check: ok`). Before a merge or a hardware build: `make check` (every
  strong test, a table, about 26 minutes with 6 at once; docs/README.md "Testing"). Do not edit the tree while it
  runs (the tests read their scripts and `rootfs/` from it).
- The repository names no path of this PC. On the owner's PC the tests need (in the environment of every test run):
  `ZENIX_KICKSTART_DIR` and `ZENIX_CAGE_RIG` (their values are in the owner's private notes, not here); `TOOLPREFIX` defaults to
  `$(HOME)/opt/m68k-elf/bin/m68k-elf-` when `m68k-elf-gcc` is not on PATH, which is where it is here.
- The real A1200 is shared with other projects: message the other sessions before using it through Wasabi, and say
  when you are done.

## Where files go
- **Documentation lives in `docs/`, nowhere else.** No `.md`/`.txt` notes in `kernel/`, `user/`, `boot/`, `tools/`
  or the project root (only `README.md` and this file). Decisions: `docs/DESIGN.md`. Plan: `docs/ROADMAP.md`. Work
  list: `docs/audit-2026-10-22.md`. Findings that help other projects: the shared knowledge base
  (its `amiga/zenix-*.md` notes), not here.
- Manual pages: `docs/man/NAME.txt` (plain text, at most 78 columns, the format `man` reads). Command index:
  `docs/commands.md`. Feature list: `docs/features.md`.
- Test input scripts: `tools/tests/`. Old code kept for reference: `archive/` (not built). Files copied onto the
  Zenix disk: `rootfs/`.
- No private paths in the repository (home directory, NAS, home-network addresses): it will become public. Use
  relative paths and `TOOLPREFIX`.

## Documentation stays in step with the code (same commit, always)
- **New command** (`user/NAME.c` added to `UAPPS`): write `docs/man/NAME.txt`, add it to `docs/commands.md`, add or
  adjust a line in `docs/features.md`.
- **Changed behaviour of a command** (an option, a default, an output format, an exit status, a limit): update its
  page, including the NOTES section about what it does not do.
- **New or changed kernel feature, system call, message protocol, limit** (`kernel/*.h`): update
  `docs/architecture.md` (and `docs/features.md`, and `docs/limits.md` if a number changed).
- **Removed feature**: remove it from the same places; never leave a page for something that is gone.
- One fact lives in one place. Other documents point to it instead of repeating a number or a status.
- Write what the code *does*, read from the source.

## Licence
GPL-2.0-only. New files carry `SPDX-License-Identifier: GPL-2.0-only`. Code from other projects must be
GPL-2-compatible and keeps its own licence header (miniz MIT, regex.c MIT from CUtils).


**Clean room (AmigaOS sources).** Never open or cite the leaked AmigaOS source. Allowed sources of AmigaOS knowledge:
the public manuals and autodocs (knowledge base `manuals/`), the clean hand-offs in the owner's research folder
(hardware quirks, ROM boot sequence, 2026-10-23) and the knowledge base's `amiga/aos31-study/` notes (owner,
2026-10-24): ideas, facts and numbers in someone else's words, never code or routine-shaped pseudo-code. Cite them as
"AmigaOS 3.1 study, aos31-study/<note>".