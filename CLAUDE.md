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
  matters on it first. Nothing has run on it yet; real-hardware time is scarce, so every build sent there must be
  ready for it.

## Current focus (2026-10-22): hardening, then the first real boot
**FEATURE HALT (decided by the owner, 2026-10-22): no new features - no new commands, system calls, drivers,
options or programs.** Only fixes from `docs/audit-2026-10-22.md`, tests that check them, and documentation
corrections. Merging work that was already built before the halt (the `drivers`, `zenixfs` and `floppy` branches) is
allowed. **Approved exception (owner, 2026-10-23): booting from the SD card / a hard disk without a floppy (`zenixboot`).**
Only the owner lifts the halt; a session never decides on its own that it is over. Unfinished work stays
parked (e.g. the uncommitted `user/awk.c`). Order of the fixes: batches A and B, then the first PiStorm boot, then C,
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
- **One main session** works in `~/Projects/zenix` on `main`. Only it edits the shared files: `kernel/kernel.h`
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
| **next free: 78** | |
Same rule for message types in `kernel/fsproto.h` / `kernel/diskproto.h` and for port names: claim here first.
Message types claimed so far: `FS_*` 1-24 (24 `FS_CLOSE`: fix-c, batch C5), `DISK_*` 1-8.

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
- Run FS-UAE headless only, never on the owner's screen. Give every run its own serial port and build directory:
  `ZENIX_SERIAL=/tmp/<name> tools/run-test.sh ...`, `make BUILD=/tmp/<name>`. One emulator run of yours at a time;
  stop if the machine's load is above 16.
- Before committing: `make` (all images, including `build/zenix-ram.adf`), the tests for what you touched, and
  `tools/docs-check.sh --staged` (must say `docs-check: ok`). Before a merge or a hardware build: `make check` (every
  strong test, a table, about 1.5 hours; docs/README.md "Testing").
- The repository names no path of this PC. On the owner's PC the tests need (in the environment of every test run):
  `export ZENIX_KICKSTART_DIR=~/UAE/ROM ZENIX_CAGE_RIG=~/Projects/DemoScene/Creator`; `TOOLPREFIX` defaults to
  `$(HOME)/opt/m68k-elf/bin/m68k-elf-` when `m68k-elf-gcc` is not on PATH, which is where it is here.
- The real A1200 is shared with other projects: message the other sessions before using it through Wasabi, and say
  when you are done.

## Where files go
- **Documentation lives in `docs/`, nowhere else.** No `.md`/`.txt` notes in `kernel/`, `user/`, `boot/`, `tools/`
  or the project root (only `README.md` and this file). Decisions: `docs/DESIGN.md`. Plan: `docs/ROADMAP.md`. Work
  list: `docs/audit-2026-10-22.md`. Findings that help other projects: the shared knowledge base
  (`~/Projects/Knowledge/amiga/zenix-*.md`), not here.
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
