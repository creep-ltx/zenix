# D13: shared libraries and shared program code (design note, revision 2)

Status: **revised after the first review (2026-10-25), awaiting a second look.** Approved by the owner on 2026-10-24
as the step after D11 and before the self-updates. No code until the second look agrees. The short version is in
[DESIGN.md](DESIGN.md) D13. Facts marked *measured* were taken with the project's own cross compiler (gcc 16.1,
`m68k-elf`) on the real build: all 111 programs and the two filesystem servers built with `-msep-data` through the Makefile (2026-10-25). Clock counts
come from the MC68020 User's Manual's tables (*from the manual*); nothing here is timed in an emulator.

What changed from revision 1 (the review's findings, all accepted): the kernel's pointer checks (section 5, a blocker
the first revision missed); two relocation bases instead of "no relocations in the shared part" (section 4); 16-bit
address-table offsets, which make pure code smaller and nearly as fast (section 3); a new file magic (section 4); a
stronger file identity and the cache rules (section 6); segment ownership and races (section 7); the corrected
numbers (section 2); stage 1 starts with `sh` and `login` only (section 8); the kernel-size plan (section 10).

## 1. The problem

Zenix is for machines with little memory: a stock A1200 has 2 MB of chip RAM. Every process gets its own copy of its
whole program, so the same program running twice is loaded twice. With a login on four consoles, four copies of the
shell's code and four of `login`'s sit in memory.

AmigaOS solved this without an MMU in 1985: a command marked *pure* can be made resident and shared by everyone who runs
it (the `p` protection bit and `RESIDENT`, AmigaDOS Manual 3rd ed., pp. 132-133), and libraries are reentrant code with
a base, reached through a jump table (RKM Libraries & Devices 1990, p. 229). Pure was opt-in per command, and only
programs written for it (the RKM's reentrant start-up: "no unshareable globals") qualified. D13 follows that model.

## 2. What it saves (measured)

| | today | with D13 stage 1 |
|---|---|---|
| `sh` code in memory | 75240 bytes per shell | one copy (72.8 KB with 16-bit offsets) for all shells |
| `login` code | about 12 KB per login (it waits while its shell runs) | one copy |
| per extra login session | | **about 85 KB saved**: 255 KB for four sessions |

The `sh` file today is 87608 bytes, 12 KB of it a table of 3089 relocations freed after loading; pure, it shrinks to
about 81 KB with 425 relocations, and a second shell's start reads about 2 KB and applies 422 relocations instead of
75 KB and 3089.

A session also holds things D13 does not touch: `sh`'s 35 KB of BSS, a 24 KB stack, two 4 KB kernel stacks, libz's
first 32 KB malloc chunk, and the shell's arena (an eighth of free memory, 64-256 KB: about 190 KB for the first shell
on a 2 MB machine). So D13 saves roughly a fifth to a quarter of a session. **The arena rule is a bigger, separate
lever** (a later item, not D13).

## 3. The compiler side

A program has code and constants (`.text`, `.rodata`, never written: shareable) and data (`.data`, the address table
`.got`, `.bss`, stack, heap: private). gcc's `-msep-data` makes the code reach every global through the address table,
whose start the program keeps in register `a5`; the kernel sets `a5` per process. **No new compiler.**

**16-bit table offsets.** For a 68020 the assembler emits each table load as `move.l (d32,a5),An`: 8 bytes and 13
clocks (cache case) against 4 for an absolute address (*from the manual*), so each global access would cost 17 clocks
instead of 4, and gcc's own options do not change it. Rewriting `@GOT(%a5)` to `@GOT.w(%a5)` before assembling gives the
4-byte `(d16,a5)` form, 5 clocks, `R_68K_GOT16O`; it links as long as the table stays under 32 KB (`sh`'s is 1.3 KB; a
larger one is a link error, not a silent fault). It is done by a small wrapper in the build (`gcc -S`, a `sed`, `as`).
*Measured* `.text`: `sh` 71188 normal, 73982 plain `-msep-data`, **69212 with `.w`** (smaller than today); `awk`
87754 / 89358 / 86642; `cat` 6968 / 7182 / 6892.

**Cost.** libz's `memcpy`, `strlen`, `strcmp`, `memset` do not change (no globals). Functions that use globals save and
restore `a5` (about 8 clocks per call) and pay about 1 extra clock per global access with `.w`. Static instruction counts
grow 1.3-4.5% (`sh` +4.5%, `awk` +1.3%, `cat` +2.5%). `a5` stops being a spare register (gcc used it in 124 places in
`sh`): a small loss. The 5% budget is judged from static counts and the manual's clock tables, said openly; Emu68's JIT
hides these costs and emulator timing is not hardware timing. **When the owner's stock A1200 (PiStorm removed) runs
Zenix from the DoM, the same `bench` and text-tool runs are timed there,** the first real 68020 numbers.

## 4. The program file (ZEXE version 2)

- **A new magic, `ZEX2`.** An old kernel then refuses a v2 file cleanly ("not a Zenix program") instead of loading it
  with one base and crashing; this matters for the self-updates. The +28 word is not free (it carries `stage0_blob`
  for the kernel image), so the v2 header is longer and `crt0.S`'s header block grows with it.
- **The layout** (`link/user.ld`): code and constants first; then `. = ALIGN(16)`; then the data part with **`.got`
  first**, then `.data`, then `.bss`. So `a5` = the start of the process's data block, `text_size` = the aligned
  address where data starts. (`a5` must be the start of `.got`, *not* `_GLOBAL_OFFSET_TABLE_`, which lies 0x64 higher:
  *measured* in `cat`, `olen`'s slot at `.got+4` is read as `a5@(4)`.) The aligned split also fixes today's 2-byte
  alignment of `.data`, which would otherwise put the table on an odd word.
- **Two relocation lists** (or a target bit per entry), because the data part holds addresses of both parts (in `sh`,
  96 `.data` words point into `.rodata`, and 245 of 326 table entries point into code or constants):
  - *to code*: add the segment's address;
  - *to data*: add (the process's data address - `text_size`).
  `mkzexe.py` classifies each by the target symbol's section. It **adds one relocation per `.got` entry itself** (a
  static `-q` link writes the table but lists no `.rela.got`; every entry is an address: *measured*, 695 entries in five
  programs, none zero). It accepts and skips `R_68K_GOT16O`/`GOT32O`/`PLT32` in `.rela.text` (resolved at link time),
  leaves the 12 zero bytes of `.got.plt` alone, and refuses: a value exactly equal to `text_size` (ambiguous), an
  undefined weak symbol (0 in the table), and any reference from the shared part *into data*.
- **References inside the shared part are allowed** when they point into the shared part: they are the same for every
  process, so they are applied once when the segment loads (libgcc's `__clz_tab` and `$_exception_handler` in seven
  programs). Only shared-to-data is refused; in all 111 programs and the servers that is `awk`'s soft-float `_fpCCR` and `crt0`'s
  `_zargv` (below).
- **The kernel checks every relocation**: its place lies in the part it claims, its target class matches, its value
  lies inside that part. Nothing in a header is trusted (audit B7).
- **Old files keep working.** A `ZEXE` (v1) file loads exactly as today, private copy of everything. The kernel and its
  built-in servers stay v1.

## 5. The kernel: user pointers (a blocker in revision 1)

`user_ok()` (kernel/proc.c) accepts only the process's own block and its memalloc regions. With shared code, string
literals live in the shared constants, so `open("/etc/x")`, `write(1, "text", n)` and `spawn("/bin/ls", ...)` would
all fail with -EFAULT. And the kernel must never write into the shared part: `read(fd, literal, n)`, or a filesystem
server answering a read into it, would corrupt the code every process runs. So:
- `user_ok_rd` accepts the process's own memory **and its segment**; `user_ok_wr` accepts only its own memory.
- Each of the 22 `UOK` sites in `syscall.c`, `copy_str`, and the message paths (a server copying into a client's
  buffer) uses the right one: anything the kernel or a server *writes* is `wr`.
- Tests: a literal path works for `open`/`spawn`/`write`; a `read` into a literal fails with -EFAULT and the code is
  unchanged.

## 6. Loaded code: identity and the cache rules

- **The key:** (mount slot + that mount's generation, inode, size, modification time). The generation is new on every
  mount, so a slot reused after `umount`/`mount` (the audit A3 class) never matches.
- **Invalidation:** the kernel drops a file's segment from the lookup table (its running users keep it) on every open
  for writing, `O_TRUNC`, rename, unlink and umount: all go through the kernel. This covers an in-place `cp` that keeps
  the inode, the size and the second.
- **Every spawn still opens the file through the filesystem** (`fs_exec_open`): execute permission and set-user-id are
  checked each time. The segment table never bypasses them.

## 7. The kernel: segments, ownership, races, crashes

- **A segment** (`struct seg`): the key, the code's address and size, a reference count, a checksum, and a flag
  "complete". It belongs to no process: its memory carries a **segment owner tag**, so `mem_free_owner(pid)` of the first
  sharer does not free it (today it would).
- **Loading:** a spawn that finds no complete segment loads its own copy. It is entered in the table **only when fully
  loaded, relocated and checksummed**; if another spawn finished first meanwhile, the later one uses the existing segment
  and frees its copy. Nobody waits on a half-loaded segment (so no D10 priority inversion of a nice-19 loader holding up
  a nice -20 one). A failed load frees what it built.
- **The process gets only its data:** one block for table + data + BSS + stack + arguments, as today minus the code;
  the heap (libz's malloc chunks, the shell's arena) stays in separate memalloc regions. The loader copies the data
  image, applies both relocation lists and sets `a5`.
- **`a5`:** set in the first register frame `proc_load` builds (word 9 of the 11 that `user_first_entry` pops; `a5` =
  0 would read table entries from the vector table at address 0). Kept in `struct proc` for anything that may one day
  enter user code (signal handlers, callbacks). Checked: `TRAP #0` saves and restores every register; interrupt
  handlers save d0-d1/a0-a1 and the kernel's C code keeps `a5` by the calling rules (the kernel is not built with
  `-msep-data`); `switch_to` saves a2-a6; a user fault kills the process. There is no `fork`, exec-in-place or
  `setjmp` in libz.
- **The checksum** is a 32-bit add-and-rotate sum (not a table CRC, 4-5 times slower): about 19,400 long words for
  `sh`, roughly 10-25 ms per reuse on a 14 MHz chip-only A1200 (*from the manual*, chip-RAM waits included), much less
  than loading. Checked when a spawn reuses a segment (a mismatch: logged, the segment leaves the table, the spawn loads
  a fresh copy) and when a process running shared code crashes (the crash line then says whether the code was damaged,
  and gives "code+offset").
- **Last user gone:** the segment is freed at once in stage 1. (Later, AmigaOS's rule fits: a library whose openers
  have all closed it is "a candidate for purging", RKM Libraries & Devices p. 229. In Zenix that is a few dozen bytes:
  when `mem_alloc` fails, drop unused segments and try once more.)

## 8. Stage 1: `sh` and `login` pure, everything else unchanged

Nearly all of the saving is in `sh` and `login`, and they carry no speed risk for the CPU-heavy tools (`awk`, `sort`,
`gzip`, `grep`). So stage 1 makes only those two pure, as AmigaOS made pure opt-in per command. The kernel work is the
same; more programs follow after the measurements. `awk` stays v1 in any case until its soft-float `_fpCCR` is solved
(libgcc's float code writes it with absolute addresses: rebuilding gcc 16.1's own `lb1sf68.S` with `-msep-data
-D__uClinux__` makes it table-only, *measured* with the 15.3 source).

`crt0.S`'s two hand-written absolute references go away: it does `bsr.l _zstart`, a C function in libz that sets
`_zargv` and calls `main`, so no hand-written `a5` code remains anywhere. The system-call stubs are C inline assembler
in `usys.h`; `-msep-data` already handles them (`zerrno` goes through the table).

## 9. Stage 2: shared libraries, decided later

Stage 1 already shares each pure program's libz. A shared libz would save only about 6 KB per *distinct* running
program, and gcc's `-mid-shared-library` costs every call in the whole program (*measured* on `sh`: 23,618 instructions
against 22,310; 792 indirect calls; every function reloads `a5`). So stage 2 is not built now. For future big libraries
written for Zenix (the network, the text UI), an AmigaOS-style jump table with a base per opener is simpler for the
kernel (no ID registry, no per-process table) and costs nothing in programs that do not use it. The choice is made when
such a library exists, with its own short design review.

## 10. The kernel size (audit P1)

The kernel is 177064 of the 179200 bytes the RAM floppy allows (2136 left; `make` prints it). D13 adds an estimated
1.5-2 KB (v2 checks, the segment table, two-base relocation, the checksum, `user_ok_rd`/`wr`, the invalidation, the
messages; estimated from the sizes of `proc_load`, `user_ok` and `zexe_bad`). So, **before D13's kernel code**:
1. **`-Os` for the cold kernel files** (syscall, vfs, floppy, emmc, ide, a3000, rtc, main, serial, kbd, tty): about
   15.8 KB smaller (*measured*, text + rodata). The hot ones stay `-O2`: term, gfx, mem, proc, ipc. Every test must pass
   unchanged; `bench` numbers are compared on the A1200 (Emu68, labelled) for the syscall path.
2. Later, if needed: a byte-delta relocation table for the kernel (11,628 bytes today, about 8 KB smaller, about 20
   bytes more in `start.S`), usable for v2 programs and the built-in images too.
3. After stage 1, `RAMDISK_K` can shrink as the v2 files do.
Rejected: moving zfsd off the RAM floppy (the mount tests need it, and the RAM root has too few free blocks for it);
compressing the kernel (a decompressor in the boot path is the most work).

## 11. Tests

- `mkzexe` host test: refuses a shared-to-data reference (named), a value equal to `text_size`, an undefined weak
  symbol; classifies a `.data` pointer into constants and a table entry into BSS correctly.
- Kernel load checks (host-side, like audit B7): a v2 header with overlapping parts, a relocation placed in the wrong
  part or pointing outside it: refused. An old-format (v1) program still loads.
- Emulator (`make check`): two and four shells share one copy (`free` shows it; `ps` shows shared and own sizes); the
  first shell exits and the others keep running; two shells started at once share one segment; a crash in one shell
  leaves the others; the code is freed when the last user ends; a literal passed to a system call works and a `read`
  into a literal is refused; a file overwritten in place in the same second, and a remount in the same slot, are not
  reused; a damaged segment is detected and not reused; every existing test passes.
- On the A1200 (when the owner is home): `free` with one, four and ten logins, before and after.

## 12. Order of the work (each step tested and merged before the next)

0. Room in the kernel: `-Os` for the cold files (section 10), all tests, the size printed.
1. `mkzexe.py`: v2 files (`ZEX2`, the layout, two relocation lists, the checks), the `.w` assembler wrapper, the host
   test; programs still built as v1.
2. The kernel: `user_ok_rd`/`wr`, segments, the v2 loader, `a5`, the checksum, the invalidation; v1 programs unchanged.
3. `crt0`/`_zstart`; `sh` and `login` built pure; the emulator tests.
4. The measurements of section 2 and 3 (memory in FS-UAE and on the A1200; speed by static counts, and timed on the
   stock A1200 from the DoM when the owner is home); then decide which programs follow.

## 13. Open questions (answered by the first review)

1. *Keep a segment after its last user?* Free at once in stage 1; purge-on-demand later (section 7).
2. *Anything entering user code without the right `a5`?* Nothing today; the first frame must set it, and `struct proc`
   keeps it for the future (section 7).
3. *Stage 2's format?* Not decided yet; AmigaOS-style jump tables lean ahead for new libraries (section 9).

Last checked against the code: 2026-10-25.
