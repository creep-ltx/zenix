# Zenix: how to work on this project

Zenix is a small Unix-like OS for 68020+ Amigas without an MMU. It runs on the Amiga itself (no AmigaOS, no Linux
underneath). Read in this order: this file, `docs/ROADMAP.md` (vision, phases, the order of work), then for the part
you touch: `docs/architecture.md`, its design note (`docs/dNN-*.md`), and before changing an interface
`docs/frozen-interfaces.md` and `docs/claims.md`. What the owner approved: `docs/decisions.md`. Open audit items: the
newest `docs/audit-*.md`.

## The owner
- Not a programmer: explain in plain words, make the technical calls yourself, ask only about what Zenix should *be*.
- Often on the phone. Short status lines, no walls of text.
- Owns one Amiga: an A1200 with a PiStorm32-lite (Emu68 on a CM4) and a Gotek. **That machine is the first test machine;
  real Amiga hardware is the ultimate target (owner, 2026-10-24): nothing in the base may depend on Emu68 or the Pi, and
  speed is judged on real 68k CPUs (today a stock 14 MHz 68EC020), not Emu68's JIT. The minimum requirements
  are not decided yet (ROADMAP, open questions).** Order audit fixes by what
  matters on it first. Zenix runs on it since 2026-10-23; real-hardware time is scarce, so every build sent there must be
  ready for it.

## Speed and size: the golden path (owner, 2026-10-26, for the whole system, not only the kernel)
Zenix must **feel snappy**, like AmigaOS on low-powered machines: a key, a command, a window answers at once. And it
must **take little**: disk, RAM, floppy. Where the two pull apart, take the middle road:
- **Fast where the user feels it:** input to screen, the console, task switches and messages, starting a program,
  reading files, the shell's prompt. These get the careful code (asm in hot loops, -O2) and a measured budget.
- **Small everywhere else:** cold code at -Os, no copies (shared code, D13), no feature that costs every user for a
  few, load on demand, packed files where it pays (the demo floppy).
- **Safety stays:** the checks (pointers, stack guards, owners of memory, the clean mark) are Zenix's edge over AmigaOS;
  make them cheap, never drop them for speed.
- **The three test machines (owner, 2026-10-26):** the stock A1200 (68EC020 14 MHz, 2 MB chip RAM only, no fast RAM:
  the floor for memory and speed; 1,132 KB free with one login, measured), the A1200 with the Apollo 1260 (68060
  80 MHz, 64 MB fast), and the PiStorm (Emu68). No other memory expansion exists.
  **The stock A1200 stays supported as long as possible (owner 2026-10-26):** raising the minimum (perhaps 4 MB fast for
  the desktop) is the owner's call, brought with measurements, never done quietly.
- **Measure, do not guess:** sizes in every build (the RAM-floppy count, `usize-test`); speed on a real 68020 (the stock
  A1200, the 060), never Emu68's JIT or an emulator alone; a change that costs speed or size says how much.

## What is approved (the feature halt and the order of work)
- **Feature halt (owner, 2026-10-22):** only the owner approves features. Built may be only what is approved:
  the record of every approval, with its conditions, is `docs/decisions.md`; the order is `docs/ROADMAP.md` "The order
  of work" (always the next item when one is done). Only the owner lifts the halt. New approvals go into
  `docs/decisions.md` in the same commit that records them.
- **Fix things when found (owner 2026-10-25):** a problem found during other work is fixed next (or now if small),
  never parked behind new features. Mark each fixed audit item in its audit file in the same commit as the fix.
- **Pace (owner 2026-10-25):** steady progress every day, done properly, never hasty; every change has tests; every
  merge gets the full suite; a focused audit after each big block. Reviews by risk: two independent design reviews for
  deep core changes (kernel, loader, updates), one for features and for kernel code (owner 2026-10-28: the quota is
  tight, so none for small changes the tests cover).
- **Text console = Unix/Linux style (owner 2026-10-23):** ten virtual consoles on Amiga+F1..F10, each with its own login
  prompt; never desktop features in the text console.
- **The owner's own goal** (Claude in a terminal on Zenix on the A1200) sets the order of work but is not Zenix's
  purpose: never present it as Zenix's main goal in the docs.
- **The demo floppy (D25) is postponed** (owner 2026-10-28): nothing of it is built until the owner says.

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
- **Coherence first (owner 2026-10-27):** parallel work is allowed only while the system stays one coherent design.
  The first Zenix run had many sessions each inventing their own solution; never again. A side helper builds only what
  an approved design note assigns to it (D19 section 10: the host-only parts), with the note's names, formats and
  interfaces; a question the note does not answer goes to the main session, never a local invention. The main session
  reviews every side branch against the note before merging, as it does its own.
- Existing branches (`drivers`, `zenixfs`, `floppy`) are merged in this order: main's own staged work, `drivers`,
  `zenixfs`, `floppy`. After every merge: rebuild every image, check the floppy sizes, run the tests.
- Never `git stash`, `git checkout .`, `git reset --hard` or `git add -A` in a shared tree. Commit your own files by
  path (`git commit -- path...`).

## Numbers that are claimed
Every system call number, message type, port name, error number and flag in use or reserved is listed in
`docs/claims.md`. **Claim a new one there before using it**, in the same commit; never reuse one. The next free system
call number is written at the end of its table.

## Frozen interfaces
Updates install a new kernel before the programs, so an older program always meets a newer kernel and servers:
system calls, error numbers, messages, the structs written into programs, file formats and the `/etc` formats only grow,
at the end, never change meaning. The full list is `docs/frozen-interfaces.md`: **read it before changing any system
call, struct, message, file format or config file**, and add new frozen items there in the same commit. Every review
checks changes against it.

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
2026-10-24): ideas, facts and numbers in someone else's words, never code or routine-shaped pseudo-code. Also the free NDK 3.2's interface files (includes, `.fd` files) for names and numbers only, never code (owner 2026-10-30, for ZINE). Cite them as
"AmigaOS 3.1 study, aos31-study/<note>".