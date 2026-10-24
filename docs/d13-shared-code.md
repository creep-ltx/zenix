# D13: shared libraries and shared program code (design note, revision 1)

Status: **proposed, not reviewed.** Approved by the owner on 2026-10-24 as the step after D11 and before the
self-updates. No code until this note is reviewed (as D11 was). The short version is in [DESIGN.md](DESIGN.md) D13.
Facts marked *measured* were taken on 2026-10-24 with the project's own cross compiler (gcc 16.1, `m68k-elf`); the
rest is *proposed*.

## 1. The problem

Zenix is for machines with little memory: a stock A1200 has 2 MB of chip RAM, and a 1 MB machine has room for only one
login session today ([limits.md](limits.md)). Two kinds of waste:

1. **The same program running twice is loaded twice.** Every process gets its own copy of its whole program image.
   Four shells on four consoles hold four copies of `sh`'s code (86 KB each, *measured*). This is the big one.
2. **Every program carries its own copy of the C library.** Each program links the libz functions it calls: 5-7 KB
   per program (`cat` 5.9 KB, `ls` 7.3 KB, `sh` 6.3 KB, *measured*). Small today, but newlib (planned) is far bigger
   than libz, and so are the text-UI and network libraries to come.

AmigaOS solved both without an MMU: "pure" programs whose code is shared by every process that runs them (made
resident), and libraries opened by name with one copy in memory. D13 brings the same two ideas to Zenix in Unix form.

## 2. The idea: one copy of the code, a data area per process

A program has two kinds of memory:
- **Code and constants** (`.text`, `.rodata`): never written. Can be shared.
- **Data** (`.data`, `.bss`, the stack, the heap): different in every process. Must be private.

Without an MMU every process sees the same addresses, so the code cannot simply contain the address of its data: two
processes running the same code need different data. The answer is a **base register**: the code reaches its data
through register `a5`, which the kernel sets to each process's own data area when it starts the process. The code
itself contains no absolute addresses, so it never needs patching and one copy serves every process.

This is not new: gcc's `-msep-data` mode was made for exactly this on no-MMU 68k Linux (uClinux), and
`-mid-shared-library` extends it to shared libraries. **No new compiler is needed.**

### What the compiler does (measured)

`cat` built both ways (`-msep-data` against the normal build):

| | normal | `-msep-data` |
|---|---|---|
| code (`.text`) | 6968 bytes | 7182 bytes (+3%) |
| constants (`.rodata`) | 1024 | 807 |
| data (`.data` + address table `.got`) | 4 | 220 + 112 |
| absolute addresses in the code | 176 | 3 (all in the start-up code, which is rewritten) |
| absolute addresses in the data | 36 (in `.rodata`) | 36 (moved to `.data`, per process) |

So the code becomes shareable: what needed patching moves into the per-process data, where the loader already patches
it. The price is one extra memory read for each access to a global variable (through the address table, the `.got`),
and `a5` is no longer free for the compiler. Section 8 says how this is measured before it is accepted.

## 3. The plan, in two stages

**Stage 1: pure programs (shared program code).** Programs are built with `-msep-data`. The first process to run a
program loads its code; later processes that run the same file reuse that copy and get only their own data. libz is
still linked into each program (it becomes part of the shared code, so `sh`'s libz is shared between shells too).
This gives the big saving (problem 1) and changes nothing for anyone writing a program.

**Stage 2: shared libraries.** libz (later newlib, the text-UI library, the network library) becomes a library file
loaded once and used by every program, with `-mid-shared-library`: each library has a fixed number (its ID), and each
process has a small table, reached through `a5`, holding the address of its own data for each library. A call into a
library loads the library's data address from that table. The library's code is one copy; its data (libz's buffers,
`malloc`'s lists, `environ`) is per process, as it must be.

Stage 2 is designed here so stage 1 does not block it, but it is built only after stage 1 is measured on the A1200.

## 4. The program file (ZEXE version 2)

Today's header ([architecture.md](architecture.md) section 4) has a reserved word at +28. Version 2 uses it:

```
+0  bra.w entry   +4 'ZEXE'   +8 image_size   +12 mem_size   +16 nreloc   +20 reloc_off   +24 stack_size
+28 flags: bit 0 = pure (shared code); bits 8-15 = format version (2)
+32 text_size     the shared part: code and constants, from the start of the image
+36 got_off       where the address table starts inside the data part
+40 got_count     how many address-table entries (every one is an address: relocated per process)
+44 nlibs, +48 libs_off   (stage 2: the libraries this program needs, by name and ID)
```

- **The shared part must contain no relocations.** `mkzexe.py` refuses to build a pure program with an absolute
  address in its code or constants (the build fails and names the symbol), and the kernel checks it again at load
  (nothing in a header is trusted, audit B7).
- **The address table is relocated per process.** In a statically linked `-msep-data` program every `.got` entry is
  an address; the linker writes them but `-q` does not always list them as relocations, so `mkzexe.py` adds one
  relocation per entry itself (checked against the symbol table).
- **Old programs keep working.** A file without the pure flag loads exactly as today (private copy of everything).
  Programs move to the new format one by one; the kernel and its built-in servers are not changed.

## 5. The loader and the kernel

- **A table of loaded code** (`struct seg`): file identity (device, inode, size, change time), the code's address and
  size, a reference count, and a checksum of the code. `spawn` of a pure program looks the file up: found and
  unchanged means reuse; otherwise it loads the code into a new segment.
- **The process gets only its data**: one block for data + address table + BSS + stack (+ the heap as today). The
  loader copies the data image, relocates it and the address table, and sets `a5` to the address table in the new
  process's first register frame.
- **Ownership:** a segment belongs to no process. Each process holds a reference; `proc_exit` drops it, and the last
  one frees the code. (A crash frees the process's data as today; the code stays for the others.)
- **A file replaced while it runs** (the self-updates): the identity no longer matches, so new processes load the
  new code; running ones keep the old copy until they end. This is why D13 comes before the self-updates.
- **System calls and `a5`:** the kernel saves and restores every user register already; it must never change a
  process's `a5`. Signals or callbacks that run user code (none today) would have to set it.
- **Memory pressure** (later, not stage 1): keeping a segment loaded after its last user ends (AmigaOS's `Resident`)
  makes the next start instant; it would be given back when memory runs low.

## 6. Safety without an MMU

Sharing code makes one thing worse: today a wild write into a program's own code harms only that process; with shared
code it harms every process running that program. Zenix cannot prevent the write, but it can notice it:
- the segment's **checksum is verified when a new process reuses it**; a mismatch logs the damage, and the new process
  gets a fresh copy (the damaged one stays with its current users, who are already affected);
- the checksum is also checked when a process running the code crashes, so the log says whether the shared code was
  damaged (a hint for finding the cause).
This is the same trust level as AmigaOS's resident programs, said openly in limits.md.

## 7. What changes for a program's author

Nothing in stage 1, apart from the build flags (the Makefile does it). Rules that become checked:
- no writing to code or constants (always true in C; the build refuses an absolute address there);
- assembler files must reach data through `a5` (only `crt0.S` and the system-call stubs; reviewed by hand).

## 8. Measured before it is accepted

The note's claims must hold on the real machine, so stage 1 is accepted only with these numbers (on the A1200, labelled
as Emu68 numbers, plus an FS-UAE 68020 run for the instruction-count view):
- **memory:** `free` with one, four and ten shells logged in, before and after;
- **speed:** `bench` and a text-tool run (`sort`, `grep`, `awk` on a large file) before and after: the extra memory
  read per global access must cost less than 5% (if it costs more, the hot globals of libz move to locals or the
  pure flag stays off for that program);
- **size:** every program's file size before and after (the address table adds a little per program).

## 9. Tests

- `mkzexe` host test: a pure program with an absolute address in its code is refused, with the symbol named.
- Kernel load checks (host-side, like the ZEXE checks of audit B7): a v2 header with the shared part overlapping the
  data, a relocation into the shared part, an address table outside the image: all refused.
- Emulator (`make check`): four shells share one copy (the free memory shows it; `ps` shows shared and own sizes);
  a crash in one shell leaves the others running; the code is freed when the last one exits; a program file replaced
  while it runs (old processes keep working, new ones run the new file); a damaged segment is detected and not reused;
  old-format programs still run; `cmds-test`, `cmds2-test`, `memtest` and every other test still pass with every
  program pure.

## 10. Order of the work (each step tested and merged before the next)

1. `mkzexe.py`: version 2 files, the checks, its host test (programs still built the old way).
2. The kernel: segments, the loader for v2, `a5`, the checksum; old programs unchanged.
3. `crt0.S` and the system-call stubs for `a5`; one program (`cat`) built pure; the emulator tests.
4. All programs pure; the measurements of section 8 on the A1200; the owner sees `free`.
5. Stage 2 (libraries): its own short design review once stage 1's numbers are in.

## 11. Open questions for the review

1. Should a segment stay loaded after its last user ends (instant restart, like `Resident`), or be freed at once?
   Proposed: freed at once in stage 1; `Resident`-style keeping comes with a memory-pressure rule later.
2. `a5` is reserved in every program. Is anything in the kernel's user-mode start-up, or in a planned feature (signal
   handlers, D-bus-like callbacks), going to run user code without the right `a5`? Today nothing does.
3. Stage 2's library format: the uClinux "ID" scheme (fixed library numbers, a table per process) is proposed for its
   simplicity. The alternative, AmigaOS-style jump tables with a library base in `a6`, would need every library
   function rewritten to take its data from the base; rejected unless the review finds a reason.

Last checked against the code: 2026-10-24.
