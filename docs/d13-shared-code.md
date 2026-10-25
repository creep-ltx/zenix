# D13: shared libraries and shared program code (design note, revision 3)

Status: **ready to build (revision 3, 2026-10-25),** after a first review and a second look (both independent). Approved
by the owner on 2026-10-24 as the step after D11 and before the self-updates; the owner's standing order of 2026-10-24
says to build it without waiting for them, the A1200 measurements excepted. The short version is in
[DESIGN.md](DESIGN.md) D13. Facts marked *measured* were taken with the project's own cross compiler (gcc 16.1,
`m68k-elf`) on the real build: all 111 programs and the two filesystem servers built with `-msep-data` through the Makefile (2026-10-25). Clock counts
come from the MC68020 User's Manual's tables (*from the manual*); nothing here is timed in an emulator.

What changed from revision 1 (the review's findings, all accepted): the kernel's pointer checks (section 5, a blocker
the first revision missed); two relocation bases instead of "no relocations in the shared part" (section 4); 16-bit
address-table offsets, which make pure code smaller and nearly as fast (section 3); a new file magic (section 4); a
stronger file identity and the cache rules (section 6); segment ownership and races (section 7); the corrected
numbers (section 2); stage 1 starts with `sh` and `login` only (section 8); the kernel-size plan (section 10).
Revision 3 adds the second look's findings: a changed file is never reused (`ETXTBSY`, mount generations, stale loads;
section 6), which writes are checked where (section 5), `-Os` only on truly cold files (section 10), one relocation
class fewer in stage 1, the segment reference rules, and a safer build order (sections 4, 7, 12).

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
about 81 KB with 425 relocations, and a second shell's start reads about 3.5 KB (table, data and relocations) and applies 422 relocations instead
of 75 KB and 3089.

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
  `mkzexe.py` classifies each by the target symbol's section; **table slots are classified by their symbol**, which every
  slot has through an `R_68K_GOT16O` relocation in `.rela.text` (*measured*: `sh` 326 of 326, `login` 42 of 42), so
  the value-equals-`text_size` ambiguity disappears and absolute symbols are refused. `.got.plt` is placed explicitly
  in `user.ld` (otherwise it lands unplaced between `.data` and `.bss`). It **adds one relocation per `.got` entry itself** (a
  static `-q` link writes the table but lists no `.rela.got`; every entry is an address: *measured*, 695 entries in five
  programs, none zero). It accepts and skips `R_68K_GOT16O`/`GOT32O`/`PLT32` in `.rela.text` (resolved at link time),
  leaves the 12 zero bytes of `.got.plt` alone, and refuses: a value exactly equal to `text_size` (ambiguous), an
  undefined weak symbol (0 in the table), and any reference from the shared part *into data*.
- **References inside the shared part are allowed** when they point into the shared part: they are the same for every
  process, so they are applied once when the segment loads (libgcc's `__clz_tab` and `$_exception_handler` in seven
  programs). **Stage 1 refuses every relocation placed in the shared part:** in `sh` and `login` the only ones are
  `crt0`'s three, which disappear with `_zstart` (section 8), so the "applied once" class waits until a libgcc user
  goes pure. Shared-to-data is refused in any case; in all 111 programs and the servers that is `awk`'s soft-float `_fpCCR` and `crt0`'s
  `_zargv` (below).
- **The kernel checks every relocation**: its place lies in the part it claims, its target class matches, its value
  lies inside that part. Nothing in a header is trusted (audit B7).
- **Old files keep working.** A `ZEXE` (v1) file loads exactly as today, private copy of everything. The kernel and its
  built-in servers stay v1.

## 5. The kernel: user pointers (a blocker in revision 1)

`user_ok()` (kernel/proc.c) accepts only the process's own block and its memalloc regions. With shared code, string
literals live in the shared constants, so `open("/etc/x")`, `write(1, "text", n)` and `spawn("/bin/ls", ...)` would
all fail with -EFAULT. And nothing may write into the shared part: a `read(fd, literal, n)` would corrupt the code every
process runs. So `user_ok_rd` accepts the process's own memory **and its segment**; `user_ok_wr` only its own memory.

Where each is used (21 check sites in `syscall.c`, read from the code):
- **Writes (`user_ok_wr`):** read, statfs, mountinfo, blk_ram, envget, lstat, readlink, readdir, stat, fstat, input,
  klog, getcwd, pipe, procinfo, meminfo; the message of `SYS_call` (the server answers into it); `SYS_svc`'s single
  check is split: SVC_SAVE reads, SVC_LOAD and SVC_OPENS write.
- **Reads (`user_ok_rd`):** write, envset, mount, `copy_str`, and `do_spawn`'s three direct checks.
- `SYS_blk_read` and `SYS_fd_rw` stay unchecked by design (privileged, as today).

**Messages.** Requests the kernel sends to a server on a process's behalf carry only pointers the system call already
checked (or kernel buffers), so the server's zero-copy write into them is covered by the system call's `wr` check.
**Raw messages** a program sends itself (`SYS_call`) are not checked by the servers today: pingd, diskd's `DISK_INFO`/
`DISK_LIST` and root's raw filesystem requests write through the pointers they are given. That stays as it is (the
no-MMU trust level), with one rule: **a program that sends raw messages stays v1** until servers check pointers
(bench, badptr, dd, fsck_ext2, fdisk, ping, mount, pingd, mkfs_ext2, priotest, svctest; `sh` and `login` send none).

Tests: a literal path works for `open`/`spawn`/`write`; a `read` into a literal fails with -EFAULT and the code is
unchanged.

## 6. Loaded code: never reuse a changed file

The rule: a segment is reused only if the file cannot have changed since it was loaded. The kernel sees every change
that goes through it, and refuses the rest:
- **`ETXTBSY`, as Unix:** opening a file for writing (or with `O_TRUNC`) fails with `-ETXTBSY` ("text file busy") while
  a segment of it exists or a load of it is in progress. Updating a program is then done the safe Unix way: write a new
  file and rename it over the old name (a new inode, so a new key); running shells keep the old code. This is also what
  the self-updates will do.
- **The other direction:** a spawn of a file that is open for writing loads a private copy (v1 style, never entered in
  the table).
- **Names that change:** unlink, rename, link and symlink are path-only in the kernel (vfs.c), so it cannot tell which
  inode they touch; each drops every unused-for-lookup segment of that mount from the table (running users keep theirs).
  This also covers an unlinked inode number being reused for a new file.
- **The key:** (mount slot, the mount's generation, inode, size, modification time). The **mount generation is new**
  (a field in `struct mount`): it changes on every mount *and* whenever the mount's filesystem server is restarted by
  the service manager (D11), because a restarted ZenixFS server drops uncommitted changes and could bring back older
  contents under the same key.
- **Loads in progress:** the kernel's list of loads (`loading`, vfs.c) carries mount and inode; an invalidation marks
  a matching load stale, and a stale load is not entered in the table (it runs as a private copy).
- **Every spawn still opens the file through the filesystem** (`fs_exec_open`): execute permission and set-user-id are
  checked each time; the table never bypasses them.
- **Known gaps, said openly:** raw `FS_WRITE` messages from root bypass the kernel (nothing in the tree sends them);
  a raw `dd` to a mounted device or a second mount of it are refused by diskd already (checked).
- **The checksum is not a backstop for a changed file:** it detects damage to the copy in memory, not a stale but intact
  copy. The rules above are what prevent stale reuse.

## 7. The kernel: segments, ownership, races, crashes

- **A segment** (`struct seg`): the key, the code's address and size, a reference count, a checksum, and a flag
  "complete". It belongs to no process: its memory carries a **segment owner tag**, so `mem_free_owner(pid)` of the first
  sharer does not free it (today it would).
- **References:** a spawn that reuses a segment takes its reference **at lookup, before any read that can block**
  (otherwise the last user could exit and free it meanwhile), and every failure exit of `proc_load` after that drops it.
  A process drops its reference in `proc_exit` (after the crash checksum below), not at reap.
- **Loading:** a spawn that finds no complete segment loads its own copy. It is entered in the table **only when fully
  loaded, relocated and checksummed**; if another spawn finished first meanwhile, the later one uses the existing segment
  and frees its copy. Nobody waits on a half-loaded segment (so no D10 priority inversion of a nice-19 loader holding up
  a nice -20 one). A failed load frees what it built.
- **The process gets only its data:** one block for table + data + BSS + stack + arguments, as today minus the code;
  the heap (libz's malloc chunks, the shell's arena) stays in separate memalloc regions. The loader copies the data
  image, applies both relocation lists and sets `a5`.
- **`a5`:** set in the first register frame `proc_load` builds: the 11 words `switch_to` pops with `movem.l
  (sp)+,d2-d7/a2-a6` (start.S), `a5` the 10th from the lowest address (index 9); `user_first_entry` then builds only SR
  and PC. `a5` = 0 would read table entries from the vector table at address 0. Kept in `struct proc` for anything that may one day
  enter user code (signal handlers, callbacks). Checked: `TRAP #0` saves and restores every register; interrupt
  handlers save d0-d1/a0-a1 and the kernel's C code keeps `a5` by the calling rules (the kernel is not built with
  `-msep-data`); `switch_to` saves a2-a6; a user fault kills the process. There is no `fork`, exec-in-place or
  `setjmp` in libz.
- **The checksum** is a 32-bit add-and-rotate sum (not a table CRC, 4-5 times slower): about 19,400 long words for
  `sh`, roughly 10-25 ms per reuse on a 14 MHz chip-only A1200 (*from the manual*, chip-RAM waits included), much less
  than loading. Checked when a spawn reuses a segment (a mismatch: logged, the segment leaves the table, the spawn loads
  a fresh copy) and when a process running shared code crashes (the crash line then says whether the code was damaged,
  and gives "code+offset"). After a crash that damaged the code, the other processes keep running the damaged copy (nothing
can stop that without an MMU): the log names their pids.
- **`ps` and `free`:** `free` reports shared code through the two spare words `SYS_meminfo` already returns; `ps`
  showing shared and own sizes needs a new field in `struct uproc` (an interface change, documented with it).
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
messages). So, **before D13's kernel code**, `-Os` for the files that are truly cold: **floppy, emmc, a3000, rtc and
kbd**, about 6.3 KB smaller (*measured*), three times what D13 needs, and none of them on a hot path (every wait in them
is timed by the E-clock or a timer, checked). The floppy's track decoding (`mfm_decode_track`, inlined from `mfm.h`)
keeps `-O2` through a function attribute. **Not** `-Os`: `main.c` (the kernel's `memcpy`/`memset`, the tick, the serial
interrupt, `kwork`), `syscall.c` (every system call), `tty.c` (every console write), `vfs.c` (pipes, read/write), `ide.c`
(its unrolled read loop is the DoM's disk path), `serial.c` (the serial terminal work from 2026-10-31), and the hot term,
gfx, mem, proc and ipc. So no speed check is needed for this step.

Later, if needed: a byte-delta relocation table for the kernel (11,628 bytes today, about 8 KB smaller, about 20 bytes
more in `start.S`), usable for v2 programs and the built-in images too; and a smaller `RAMDISK_K` as v2 files shrink.
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
  reused; a write-open of a running program gives `ETXTBSY` and a rename-over update is picked up by the next spawn
  while running shells keep the old code; a filesystem server restart bumps the mount generation; a damaged segment is
  detected and not reused; every existing test passes.
- On the A1200 (when the owner is home): `free` with one, four and ten logins, before and after.

## 12. Order of the work (each step tested and merged before the next)

0. Room in the kernel: `-Os` for floppy, emmc, a3000, rtc, kbd (section 10); all tests; the size printed.
1. `mkzexe.py`: v2 files (`ZEX2`, the layout, the relocation lists, the checks), the `.w` assembler wrapper, the host
   test; programs still built as v1, and **every v1 `.zx` and `kernel.bin` byte-identical before and after**.
2. The kernel: `user_ok_rd`/`wr` at the 21 sites, segments and references, the v2 loader, `a5`, the checksum, mount
   generations, `ETXTBSY` and the invalidation; v1 programs unchanged. Tested with a pure build of `hello` placed in
   the test image only (not a new command).
3. a. `crt0` with `_zstart` for all 111 programs; `make check`.
   b. `sh` and `login` pure: separate pure builds of crt0, libz, `pw.uo` and `sha.uo` (the header lengths differ);
      a `-Wwrite-strings` review of both (a pure program writing into its own literal would corrupt every shell; the 8
      warnings found so far only read); the emulator tests of section 11.
4. The measurements of sections 2 and 3 (memory in FS-UAE and on the A1200; speed by static counts, and timed on the
   stock A1200 from the DoM when the owner is home); then decide which programs follow.

## 13. Open questions (answered by the first review)

1. *Keep a segment after its last user?* Free at once in stage 1; purge-on-demand later (section 7).
2. *Anything entering user code without the right `a5`?* Nothing today; the first frame must set it, and `struct proc`
   keeps it for the future (section 7).
3. *Stage 2's format?* Not decided yet; AmigaOS-style jump tables lean ahead for new libraries (section 9).

## 14. As built (stage 1)

What the build did differently from sections 1-13, and why; each item names the step.

- **Step 0.** The floppy's MFM code keeps `-O2` through `#pragma GCC optimize("O2")` around `#include "mfm.h"` in
  `floppy.c`, so the whole of `mfm.h` (encoding too, not only `mfm_decode_track`) stays `-O2`: the decode and encode
  functions compile to the same instructions as before (compared with objdump). The kernel went from 177064 to 170212
  bytes (8988 left of 179200).
- **Step 1.** The pure layout lives in a new linker script, `link/pure.ld`, not in `link/user.ld`: changing
  `user.ld` would have moved every v1 program's `.data` (the 16-byte alignment) and broken "every v1 file
  byte-identical". `pure.ld` also discards `.eh_frame` (nothing unwinds). `_zstart` (libz) and crt0's `-DZEX2` variant
  came with step 1, because the host test needs a working pure link; v1 programs do not reach `_zstart`, so
  `--gc-sections` drops it from them (step 3a moves every program to it). Pure objects are `NAME.po`, a pure program
  `NAME.pzx` (`make build/NAME.pzx`). The relocation lists hold link addresses (not offsets in the data part), so a
  later "applied once" class in the shared part needs no format change. mkzexe also refuses a writable section in the
  shared part, a table slot that holds something other than its symbol's address, and a `.got.plt` that is not zero;
  it verified on every slot of `hello`, `cat` and `sh` that the 16-bit offsets count from the start of `.got`.
- **Step 2.** The segments live in a new file, `kernel/seg.c` (`-Os` except the checksum loop), with a fixed table
  of `NSEG` = 8; a program that finds it full runs as a private copy. A private copy (open for writing, stale, table
  full) is simply owned by the process's pid, so `mem_free_owner` frees it at the reap; only a listed segment's block
  gets `SEG_OWNER` (`mem_set_owner`, new in mem.c, when it is listed). A loader's own copy checks that its first 40
  bytes are the header it read first (a file changed between the two reads is refused, -EIO). **`ETXTBSY`:** the kernel
  learns a file's inode only from the server, so a write-open with `O_TRUNC` (the server truncates inside `FS_OPEN`)
  costs one `FS_STAT` before the open - only while something of that mount is loaded, which with `sh` pure (step 3b) is
  always on the root - and every other write-open is checked on the open's answer and closed again if busy. A
  segment is "busy" when its whole key matches the file's status (inode, size, time, generation), so an unlinked
  program's inode number given to a new file does not make that file busy. Gap, said openly: a load or a listing that
  happens between that `FS_STAT` and the truncating `FS_OPEN` cannot be refused any more; it is unlisted or marked stale
  (never reused), but the file is already truncated. `O_TRUNC` without write access truncates nothing in either server
  and is not checked. **Generations** come from one counter (`gen_last`), set on every mount and on every
  `svc_handover` of a port that a mount uses (also the first start: harmless). `umount` needs no rule of its own: the
  slot's next mount has a new generation. **`ps`** got a SHR(K) column after MEM(K) (the tests that read `ps` lines
  were adjusted); MEM is the process's own memory, a private copy's code included. **`free`** prints a third line,
  "shared code: N KB, sharing saves M KB" (SYS_meminfo words 2 and 3: the bytes of shared code in memory and, for each
  segment, its size times its users beyond the first). **Tests:** a shell cannot be made to fault on purpose, so the
  crash, damage and literal checks use a ZEX2 build of `crash` with four new test modes (`sleep N`, `poke`, `damage`,
  `literal`; `crash` is a test program, man page updated), placed with the ZEX2 `hello` only in the test images'
  `/tests` (not commands; the mount tests' small floppy leaves them out: no room). The remount check mounts a floppy
  holding `hellopure` twice in the same slot. `tools/mkbadzexe.py` also writes 14 corrupt ZEX2 files from `hellopure`.
  The kernel went from 170212 to 174368 bytes (4832 left): about 4.2 KB, more than the 1.5-2 KB estimated (seg.c 1.4 KB,
  the loader 1.5 KB, vfs 0.7 KB, the rest and the kernel's relocation table); `-Os` on seg.c saved 240 bytes.
- **Step 3a.** As planned: crt0 is `bsr.l _zstart` for every program (the stack at entry already holds argc and
  argv as `_zstart`'s arguments, so crt0 pushes nothing). Each ZEXE program loses crt0's three relocations; the kernel
  grew 24 bytes (its built-in servers link libz's `_zstart`): 174392 of 179200.

Last checked against the code: 2026-10-25.
