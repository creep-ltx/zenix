# D52. Zenix builds itself, and fun: the plan for roadmap block 7

Roadmap block 7: "**Zenix builds itself, and fun:** assembler, C compiler, `make`, debugger, Lua; games and demos on
Zenix. A full audit of the whole system, then release 1.0 with the owner's yes." The order of work is the owner's
(2026-10-25); the details are delegated as for the blocks before, except where this note says a choice is the
partner's (licences that do not fit GPL-2, a kernel change for the debugger). Written 2026-11-13 on branch block7;
the first item (Lua) is built with it (section 9).

How the facts were found: licences read from each project's own files or pages (named in section 11); sizes
**measured** where the note says so (the source compiled for Zenix with Zenix's compiler flags, `-m68020-60 -Os`,
`m68k-elf-size`), else **estimated** and marked so. Nothing here was run on a real Amiga.

## 1. In short: the order

| # | Item | What | Licence verdict | Size (68k code) | Partner? |
|---|---|---|---|---|---|
| 1 | Lua | Lua 5.4.9, `/bin/lua` | MIT: in the repo | 155 KB Lua + 66 KB C library, maths and soft float = 221 KB (measured) | no: **built** (section 9) |
| 2 | make | pdpmake (POSIX make) | Unlicense (public domain): in the repo | about 25 KB + C library, about 40 KB (part measured, section 4) | no |
| 3 | linker | `zld`, our own: ELF objects and archives straight to `.zx`/`.pzx` | ours, GPL-2.0-only | 22 KB of its own code, 30 KB as a program with the C library (measured) | no: **built** (section 5) |
| 4 | assembler | `zas`, our own, Motorola syntax, ELF objects (or vasm) | ours GPL-2 (vasm: not GPL-compatible) | about 30-50 KB (estimate); vasm 303 KB (measured) | **yes**: vasm only with its author's consent |
| 5 | C compiler | tcc with a new 68k back end (or vbcc) | tcc LGPL-2.1: GPL-2-compatible (vbcc: not) | about 180-220 KB (tcc measured 176 KB for i386) | **yes**: the choice |
| 6 | debugger | stage 1 `zdb` after a crash (no kernel change); stage 2 live | ours | stage 1 about 15 KB (estimate) | **yes** for stage 2 (a kernel change) |
| 7 | fun | Lua's display and sound module, terminal games, a text-adventure player | ours; BSD; GPL-2+ | small each | **yes**: which ones |
| 8 | audit | the whole system | | | release 1.0 with the owner's yes |

Smallest and most useful first: Lua and make need no decision and are useful at once (scripts, games, build files);
the linker is needed by every compiler route and can be checked byte for byte against today's `ld` +
`tools/mkzexe.py`; the assembler and the compiler wait for the partner's licence choice; the live debugger waits for a
kernel decision.

## 2. What "builds itself" means here

- **Programs, not the kernel, at first.** The goal is that a C program (a new one, or one of Zenix's own commands)
  is compiled, assembled and linked on Zenix into a `.zx` that runs. The kernel and the C library use gcc's own
  extensions and its exact code (the kernel's size budget, `-fstack-usage` figures, `tools/check-init.py`), so they
  stay built by the PC's gcc; building them on Zenix is a stretch goal, never a promise.
- **One ABI: gcc's m68k-elf.** Whatever compiler runs on Zenix must call and be called as gcc's code does (arguments
  on the stack, the result in d0, d0-d1/a0-a1 scratch, a struct's result through a1, `int` and `double` aligned to
  2 bytes in structs, soft-float through libgcc's names such as `__adddf3`), so its objects link with the C library
  that the PC builds (`build/libc.a`, D27) and with libgcc. Then nothing of the C library is built twice.
- **A development package** (`zpkg -S devel`, a new family): picolibc's and the port's headers (930 KB as source;
  smaller with the comments left out), `libc.a` with its debug sections stripped (913 KB measured, from 4.1 MB),
  libgcc's soft-float members, `crt0.o`, `crt0-pure.o`, the two link scripts. It is not on the demo floppies and not in
  the base: only a machine that builds needs it (the golden path: nobody else pays).
- **Memory:** the minimum is a stock A1200 with 4 MB of fast RAM. A compile of one small file must fit there;
  building something the size of `sh` may need the 1260's 64 MB, and the note says so where it applies.

## 3. Lua (item 1)

Lua 5.4.9 (2026-08-10), MIT, the reference interpreter. 5.5.1 is newer (its arrays take less memory), but the scripts
and books in use are 5.4's; moving to 5.5 is a later update of the pinned copy. Built unchanged: the Zenix settings
are compiler flags and upstream's user-header hook, no patch (section 9). Measured: 221 KB of 68k code with the C library, the maths library and the
soft float it needs; a plain program (ZEXE: libgcc's soft float cannot be shared code today). Section 9 has what
was built.

## 4. make (item 2)

**pdpmake** (Ron Yorston, the make in BusyBox for Windows), "released into the public domain" under the Unlicense:
it may go into the GPL-2 repository like any public-domain code. POSIX 2024 make, with a few extensions off unless
asked for. 3,964 lines of C in 9 files. Measured: 7 of the 9 files compile for Zenix unchanged, 13.8 KB of code before
linking; the other two need what picolibc and Zenix lack today (`utimensat` with `UTIME_NOW`/`AT_FDCWD` for `touch`
rules, `confstr(_CS_PATH)`), a small Zenix patch or two C-library functions. Expected about 40 KB on disk. It runs
commands through `/bin/sh -c` (the C library's `posix_spawn`, D27), which Zenix has. Others looked at: GNU make
(GPL-3+: may not be combined with GPL-2-only code; as a separate program it could ship, but at about 250 KB it is
six times the size), BSD make (BSD, larger, needs its own library code). Tests: the same Makefiles run under pdpmake
on the PC and on Zenix, the outputs compared, as `tools/lua-test.sh` does for Lua.

## 5. The linker, `zld` (item 3)

Today the PC links with GNU ld (`link/user.ld`, `link/pure.ld`, `--gc-sections`, `-q`) and `tools/mkzexe.py` turns the
ELF into a ZEXE or ZEX2 file. On Zenix one small program does both: read ELF relocatable objects and `ar` archives
(the C library, libgcc), take only the sections that are reached (as `--gc-sections`), lay them out as the two link
scripts do, resolve the symbols, apply the `R_68K_*` relocations and write the ZEXE or ZEX2 header and relocation
table directly (architecture.md section 4; the ZEX2 rules of mkzexe.py, such as no relocation in the shared part).
Our own code, GPL-2.0-only.

- **Why our own:** GNU ld is GPL-3+ and over 1 MB; vlink (Frank Wille's, distributed beside vasm and vbcc; its
  terms not read here, but of the same non-commercial kind as far as known) has no Zenix format. A linker for one CPU, one input format and one output format is small (about 1,500 lines
  of C, 20-30 KB of code: an estimate).
- **Checked on the PC first:** `zld` built for the PC links the objects of every program in `UAPPS` and must give
  byte for byte the `.zx`/`.pzx` that `ld` + `mkzexe.py` give (or a difference that is explained and allowed: the
  order of sections). Then the same in FS-UAE on Zenix itself, the result run.
- Needed by every compiler route below; that is why it comes before the assembler and the compiler.

### As built (2026-11-13)

`user/zld.c` (2,100 lines of standard C and POSIX: the same file builds for the PC and, as a C library program with
shared code, for Zenix: 22.2 KB of its own code, `/bin/zld` 30.4 KB on the disk with the C library's parts, 5 KB of bss).
`zld [-v] [-s STACK] [--defsym=SYM=VALUE] [--wrap=SYM] [-L DIR] -o PROGRAM FILE... [-lNAME]`; man/zld.txt.
**Every program the build links (192: UAPPS, the servers, drivers, test programs, `zenixboot`, `bootmenu`) comes out
byte for byte as `ld` + `tools/mkzexe.py` make it** (`tools/zld-host-test.sh`), so no difference had to be allowed.
That needed GNU ld 2.45's own rules, each coded where it applies (read from binutils' source, `bfd/elflink.c`,
`bfd/merge.c`, `bfd/elf32-m68k.c`, `ld/ldlang.c`, `ld/ldexp.c`, libiberty's `hashtab.c`):

- **Archives:** the index in order, a member pulled for a symbol undefined (not weakly) at that point; another pass
  only when a member added new undefined symbols. Sections are placed in the order the members were pulled, not the
  archive's order.
- **--gc-sections:** kept is what `_start`, `_init`/`_fini`, the `KEEP()` sections (`.text.start`, the constructor
  tables), notes and `SHF_GNU_RETAIN` sections reach through their relocations.
- **--defsym:** read where it stands among the inputs (gcc passes it before the objects): the value's symbol becomes
  an undefined reference (it pulls archive members), the name is defined at once (it pulls nothing); a value whose
  section was dropped gives an absolute symbol (`vfscanf` is `A 0` in a program that scans nothing).
- **SHF_MERGE:** sections alike (flags, entry size, alignment, output section) merge into the first kept one in load
  order; strings in the order first seen, a string that ends a longer one placed inside it (ld's reverse-sorted
  suffix pass, its alignment rules), the size padded when every input's was.
- **The address table of a ZEX2 program (.got):** ld's slot order is the order of libiberty's open-addressed hash
  tables (per object, then merged), keyed by ld's numbers for its input files and a global symbol's first GOT use.
  Those numbers were found by fitting all 14 ZEX2 links of the build: the output file 0, each object two (itself and
  the LTO plugin's stand-in, as gcc always passes the plugin), each archive two when opened (itself and the first
  member, opened to check the format), each member it gives two more. One more trap: libiberty grows a table on every
  lookup made with INSERT, even when the entry is found, and ld looks up with INSERT for every relocation.

What it reads and refuses: ELF32 big-endian 68k relocatable objects, GNU `ar` archives with a symbol index; refused
with a message naming the file (and the symbol): COMDAT groups, common symbols, REL relocations, sections the layout
has no place for, undefined references, duplicate definitions, a relocation out of reach or not supported (the ZEXE
and ZEX2 rules of `tools/mkzexe.py`, with its checks), a damaged file. Nothing is written until the program is
complete. One difference on purpose, which no program of the build meets: an `R_68K_32` to an absolute symbol (a
number by `--defsym`, a `.set`) is written as the number; `tools/mkzexe.py` lists it for relocation, so the loader
would add the load address to it. A reference to a symbol in a section the layout leaves out (`.comment`, `.note`) is
refused, as ld refuses it. The review's cases the build does not use (merged constants, string tails, empty aligned
sections, `--wrap`, an archive with two members of one name, GOT8O/16O/32O mixed in both orders) are in
`tools/tests/zld`, compared with ld + mkzexe.py on every run.

**Memory** (it must link mid-sized programs on the 4 MB minimum): the objects' symbol tables and the relocations of
their loaded sections, the archive's index, the merged strings and the program's image; archive members not needed
are never read, debug sections never. Measured in FS-UAE (an A1200 with 4 MB of fast RAM, 3,469 KB free; emulator
figures, not hardware speed): linking sh (a 96 KB ZEX2 program) zld owned at most 477 KB (`ps` once a second);
libctest (92 KB, the C library's 913 KB archive searched) took 11.1 s, hello 1.0 s.

**The development package** (`zpkg -S devel`, the family devel in `tools/mkzup.py`, staged by `tools/mkdevel.sh`):
`/bin/zld`, `/usr/lib/libc.a`, `libc-pure.a`, `libgcc.a` (debug sections stripped), `crt0.o`, `crt0-pure.o`,
`/usr/lib/zenix/user.ld` and `pure.ld`, the headers (the port's in `/usr/include`, picolibc's in the Makefile's order
under `/usr/include/pico`); 685 KB packed. Why two levels: the port's headers (`stdio.h`, `stdlib.h`, `regex.h`,
`sys/stat.h`, `sys/syslimits.h`) add to picolibc's by `#include_next`, which finds the next header of that name further
down the `-I` list, and picolibc's own directories hold headers of the same names; one flat `/usr/include` would keep
only one of each. So the port's are `/usr/include`, the first on the list, and picolibc's directories stay apart under
`/usr/include/pico` in the Makefile's `LIBC_INC` order (man/zld.txt lists it for a compiler's `-I`). Accepted by the
main session 2026-11-13.

**Tests:** `tools/zld-host-test.sh` (the PC, in `make check`'s host list): the 192 links compared byte for byte;
links that must fail (an undefined reference, a duplicate definition, a common symbol, a `bsr.w` out of reach, an
`R_68K_16`, no crt0, not an object, a missing library) with their messages and no output file; 400 damaged objects and
archives (3,000 in a longer run) under AddressSanitizer and UBSan, each linked or refused, never a crash.
`tools/zld-test.sh` (FS-UAE, an A1200 with 4 MB of fast RAM): zld on Zenix links libctest from libc.a and libgcc.a
(found with `-L` and `-l`), hello and sh (ZEX2), each the PC's output byte for byte, and they run (hello's lines, `sh -c`,
libctest's 184 checks); a link with an undefined symbol is refused there with its message. **Verified in FS-UAE
2026-11-13; not yet on a real Amiga.**

## 6. The assembler (item 4)

Candidates (68k, Motorola syntax, an output Zenix's linker can read, i.e. ELF relocatable objects):

- **vasm** (Volker Barthelmann; Frank Wille's 68k module): the best 68k assembler there is, Motorola syntax, ELF
  and hunk output. Licence (vasm.texi, 2002-2025): "This archive may be redistributed without modifications and used
  for non-commercial purposes. An exception for commercial usage is granted, provided that the target CPU is M68k
  and the target OS is AmigaOS. ... In all other cases you need my written consent." **Not GPL-compatible** (the
  non-commercial clause is a restriction GPL-2 forbids), so never in the repository. As a separate package the
  terms allow passing on the archive unmodified for non-commercial use on any OS; but a Zenix build needs changes
  (a modified version, which the terms do not allow), and Zenix copies may be sold (the AmigaOS exception does not
  cover Zenix), so a Zenix package should have the author's **written consent**.
  Size measured: the 68k CPU module, the Motorola syntax and the ELF output compiled for Zenix are 153 KB of code and
  150 KB of tables (the opcode tables), about 320 KB with the C library.
- **GNU as** (binutils): GPL-3+ since binutils 2.18; 2.17 (2006) was the last GPL-2+. Through BFD it is about 1 MB
  (an estimate from Linux builds), its syntax is gas's, not Motorola's. Too big for the minimum; no.
- **ASMotor** (Carsten Sørensen): Motorola-style syntax, hunk and ELF output, C99, but **GPL-3**: a separate package
  only, never linked with Zenix's GPL-2-only code; its 68k support is aimed at games consoles. A fallback, not a
  first choice.
- **A68k** (Aminet, public domain): 68000/68010 only, hunk objects only. No.
- **Our own, `zas`** (recommended): GPL-2.0-only, Motorola syntax as vasm and Devpac write it, the 68020 integer
  instructions and addressing modes plus what the compiler of item 5 needs, sections, `dc`, `ds`, `equ`, `xdef`/`xref`,
  simple macros later; ELF relocatable output with `R_68K_32`/`PC16`/`PC32`/`PC8`. Also gas's m68k syntax ("MIT
  syntax", `%`-less registers as the repository writes them) the repository's own `.S` files use (`crt0.S` is three instructions), so Zenix can assemble its start-up code. An
  estimate: 2,500-3,500 lines, 30-50 KB. **Checked on the PC** the way the linker is: every instruction form
  assembled by `zas` and by `m68k-elf-as`, the object bytes and relocations compared.

**For the partner:** our own `zas` (free, small, takes longer) or vasm (the best one, ready, but only as a separate
package with Volker Barthelmann's written consent, which the partner would ask for). Recommended: `zas`; it is what
the free compiler route (item 5, tcc) needs anyway, since tcc writes machine code itself and needs an assembler only
for `.S` files.

## 7. The C compiler (item 5)

What it must do: C99 at least (the C library's headers are picolibc's, C99 and newer), gcc's m68k-elf ABI (section 2),
run in 4 MB for a small file, be honest about its code. Candidates:

| Compiler | Licence | 68k back end | Size / memory | Verdict |
|---|---|---|---|---|
| **tcc** (Tiny C Compiler, mob branch 2026-11-09) | LGPL-2.1: GPL-2-compatible, may go in the repo | **none**: back ends for i386, x86-64, ARM, ARM64, RISC-V, C67 | i386 build for Zenix measured: 176 KB of code (preprocessor, compiler, ELF linker, i386 assembler) + 80 KB of tables; one pass, a function at a time: modest memory | **recommended**: write the m68k back end (`m68k-gen.c`, `m68k-link.c`; ARM's are 2,375 + 455 lines) |
| **vbcc** (Volker Barthelmann) | "redistributed without modifications ... non-commercial ... AmigaOS exception ... in all other cases you need my written consent": **not GPL-compatible** | excellent, the Amiga's own | the PC build is 1.4 MB (measured, x86-64); a 68k build maybe 0.6-0.9 MB (estimate); a function at a time, runs in 1-2 MB on AmigaOS | the best code with the least work, but a separate package only, with written consent; needs vasm and vlink too (same terms) |
| gcc 2.95.3 + binutils 2.17 | GPL-2+: compatible | excellent, gcc's own ABI | cc1 about 2 MB (Amiga builds of the time), needs several MB to compile; 25-year-old code that today's compilers build only with work | for the 1260 (64 MB) at most; not for the minimum; no |
| ACK (Amsterdam Compiler Kit) | BSD-3 since 2003: compatible | yes (m68020, `linux68k`) | many small passes (made for small machines) | C89 only (picolibc's headers need C99), its own object format and libc, its own ABI: much glue; no |
| pcc (Portable C Compiler) | BSD, older files under Caldera's 4-clause licence (with the advertising clause: GPL-incompatible inside one program) | an m68k back end exists in the tree, its state unknown (last release 1.1.0, 2014) | small | writes gas syntax, so it needs gas or a gas-syntax `zas`; uncertain; no |
| cproc + QBE | ISC + MIT: compatible | none, and QBE is built for 64-bit targets (pointers are 64-bit "l") | small | a 32-bit back end changes QBE itself; no |
| chibicc (and EiGen's ecc) | MIT (EiGen GPL-3, C++) | none (EiGen has an unfinished m68k part, C++) | chibicc never frees memory: a big file does not fit 4 MB | no |
| lcc | its own licence ("may not be sold"): not open source | none in 4.2 | | no |
| SDCC | GPL | no 68k | | no |

**Recommendation: tcc with a new m68k back end, ours.** It is the only route that is free, fits the GPL, is small
enough for the minimum and compiles fast on a 68020 (tcc's whole point). It also gives `tcc -run file.c` (compile into
memory and run: C as a script language) and its own linker, which can write `.zx` through `zld`'s code. Its code is
plainer than gcc's or vbcc's (a value stack with a few registers): fine for tools, not for the hot paths, which stay
built by the PC's gcc. The work: the code generator (about 2,500 lines, the size of ARM's), the ELF relocations
(about 400), soft-float calls to libgcc's names, the ABI of section 2; tcc's own test suite (`tests/tests2`, some 200
programs with their expected output) run on Zenix in FS-UAE is the check, and every test also compiled by gcc and
run, the two outputs compared. An estimate: one to two weeks of work, the largest item of the block. Offering it
upstream to tcc keeps it alive.

**For the partner:** tcc with our own back end (free, recommended, takes longest) or vbcc + vasm + vlink as a
separate package with its authors' written consent (Volker Barthelmann for vbcc and vasm, Frank Wille for vlink;
best code, least work, not free software in the
partner's sense: "I do not like proprietary software"). Both could exist later: vbcc as an optional package if
consent is given.

## 8. The debugger (item 6) and the fun (item 7)

**Debugger, stage 1, no kernel change:** a crash already prints the program's name, the address and `code+OFFSET`
(architecture.md section 4). `zdb` reads the program's symbol table (a `.sym` file the build writes beside each
program from its ELF, function names and line numbers, in the development package) and turns the crash line into
"function:line" and the frame chain the crash line can show; it also disassembles around the address (a 68020
disassembler, about 1,500 lines). Small, useful at once, nothing frozen changes.

**Stage 2, a live debugger (breakpoints, single steps, looking at a running program): a kernel change, the
partner's call.** Without an MMU every process's memory can be read by the kernel, so reading is easy; what is new is
stopping and resuming a process, the trace bit (SR T1, single step) and the breakpoint instruction (`bkpt`/`illegal`)
routed to the debugger instead of killing the program, a new system call and a stopped state that `ps`, signals and
`wait` must know (a frozen-interface growth). And a breakpoint written into shared code (ZEX2, D13) would stop every
process running that program: a debugged program must get a private copy of its code (the loader can do this today
for a file open for writing). D41's memory protection, when on, must let the debugger read the child. A design note
with two reviews first (deep core).

**Fun on Zenix (item 7, the partner picks):** ZINE already runs AmigaOS demos; "fun on Zenix" means things made for
Zenix itself:
- **Lua as the game language:** a `zenix` module for Lua giving a window from the display server (D38: draw pens,
  blit a picture, read keys and the mouse) and sound through sndd (D40: play a sample, a module): small games written
  in Lua, like PICO-8's, shipped as scripts. The module is ours (GPL-2), Lua MIT.
- **Terminal games:** BSD games (BSD licence: robots, worm, snake, tetris, hangman, backgammon, adventure, ...) in the
  text console, each a few KB.
- **A text-adventure player:** Frotz (GPL-2+, compatible) for Infocom-style story files (the free ones, as the
  Interactive Fiction Archive's); the classic games themselves are not free to ship.
- **A Zenix intro:** our own demo through the display server, once `DPY_SCREEN` (D50 section 4, a question already
  with the partner) gives a program a screen of its own.

## 9. Lua as built (item 1, 2026-11-13)

**Where:** `ports/lua/upstream/` holds Lua 5.4.9's `src/*.c`, `src/*.h` (not `luac.c`) and its licence, pinned and
unchanged (lua.org's `lua-5.4.9.tar.gz`; its size and sha256 in `ports/lua/README`). There are no Zenix patches:
everything Zenix needs is set by compiler flags in the Makefile (`LUA_CFLAGS`) and by upstream's own hook for a user
header (`LUA_USER_H`, included by `lua.h` after `luaconf.h`: `ports/lua/zenix/luauser.h`):

- `LUA_USE_POSIX`: `io.popen` through the C library's `popen`, `os.tmpname` through `mkstemp`, `fseeko`.
- `_setjmp`/`_longjmp` named as `setjmp`/`longjmp` (picolibc has only those; its `setjmp` saves no signal mask, so
  they are the same thing).
- `LUAI_MAXCCALLS` 100 (upstream 200) and `LUAL_BUFFERSIZE` 256 (upstream 512 on the 68k), with the stack: see
  "The C stack" below.
- No dynamic libraries (`require` of a C module says "dynamic libraries not enabled; check your Lua installation",
  upstream's own message); `require` of Lua modules searches upstream's paths (`/usr/local/share/lua/5.4/`,
  `/usr/local/lib/lua/5.4/`, `./?.lua`, ...).
- Numbers as everywhere: 64-bit integers and double floats, the doubles in software (no FPU is assumed; 68881/2 and
  the 040/060 FPUs are not used). printf's `%g` through the C library's floating-point formatter (`__d_vfprintf`).

**What is shipped:** `/bin/lua` only, a plain program (ZEXE). It was meant to be a program with shared code (ZEX2:
one copy of the code for every Lua running), but libgcc's soft float for the 68k (`lb1sf68.S`) keeps its state
(`_fpCCR`, `__exception_bits`, the exception handler's pointer) in data it reaches by absolute addresses, which
`tools/mkzexe.py` rightly refuses in a shared part (33 such places). A pure soft float (libgcc's C soft-fp compiled
through `tools/purecc.sh`, or the asm with its data reached through `a5`) would make every float-using program
shareable; it is noted for the compiler item, which needs soft float too. `luac` is left out (`string.dump` and
`load` do its job in scripts; it would be a second 200 KB program).
The package `lua` (tools/mkzup.py) with its notice in `/usr/share/licenses/lua`. Not on the demo floppies and not on
the RAM-disk test floppy.

**Sizes (measured, 2026-11-13):** `/bin/lua` 243,948 bytes on the disk; 220.8 KB of code, of which Lua's own is
155 KB and the C library, the maths library and the soft float 66 KB. The test root image has 270 KB less free
(5,241 of its 16,384 1-KB blocks free, 5,511 before): the program, its notice, the test scripts and libctest's check.
**Memory** (FS-UAE, an A1200 with 4 MB of fast RAM, emulator figures from `tools/lua-test.sh`):
a Lua that has just started owns 396 KB (its code and data 244 KB, its stack 128 KB, its first heap with the
standard libraries about 24 KB); with 100 KB of strings in its heap 542 KB. A second Lua running at once costs the
same again (no shared code, above).

**The C stack.** The stack is fixed (no MMU, nothing grows it, and an overflow writes into the memory below it until
the kernel's guard notices). Lua itself stops a script that nests too deep in C: at `LUAI_MAXCCALLS` levels of C
calls (a C function calling Lua again: `string.gsub` with a function, `pcall`, `table.sort`'s comparison, a
metamethod, `require`, `load` with a reader, a coroutine resumed from a coroutine, `tostring` through
`__tostring`) or of the parser (brackets, tables, functions, blocks inside each other), with "C stack overflow", an
error the script can catch; while it handles that error it allows a tenth more, and a Lua message handler (`xpcall`)
runs on top of the deepest chain. Lua calling Lua costs no C stack in 5.4 (those calls nest until memory runs out).
The pattern matcher has a recursion counter of its own (lstrlib's `MAXCCALLS`, 200 upstream, 100 here through
`luauser.h`: "pattern too complex" past it), which adds to the deepest chain.

`rootfs/tests/lua/cstack.lua` drives 18 kinds of recursion to Lua's limit (the 16 plain kinds, then the pattern
matcher at every level of the gsub chain, then a message handler that goes down the chain again). Two ways of
measuring, both in FS-UAE (the A1200 with 4 MB fast, 2026-11-13):

- **First, by bisection:** builds whose header named smaller stacks, the kernel's guard killing the ones whose stack
  was too small. With upstream's 200 levels and 512-byte buffers gsub needed 192-256 KB. **This reads low:** the
  guard is 16 bytes, checked at system calls and ticks, and a big frame that is mostly unwritten (str_gsub's buffer
  and capture array) can dip past it unseen (the review's point): gsub "fitted" in 96 KB at 100 levels, but needs 99 KB.
- **Then exactly, by a painted stack:** a test build, `/tests/lua/luaprobe` (`ports/lua/zenix/stackprobe.c`, through
  upstream's `luai_userstateopen`/`luai_userstateclose` hooks in `luauser.h`), fills 240 KB of its stack with a
  pattern when the Lua state opens and finds the deepest word written when it closes. With 100 levels and 256-byte
  buffers, from the program's arguments down, in bytes:

| Kind (cstack.lua, run one at a time) | Deepest stack |
|---|---|
| handler (a Lua message handler down the gsub chain again, the pattern at each level) | 111,996 |
| pattern (the gsub chain, a 99-level pattern at every level) | 103,264 |
| traceback (the gsub chain, a traceback made at the bottom) | 99,876 |
| gsub (string.gsub's function calling gsub) | 99,288 |
| load (a reader that loads) | 83,380 |
| format (`%s` through `__tostring`) | 76,396 |
| pcall | 59,324 |
| resume (a coroutine resumed from a coroutine) | 57,116 |
| sort (the comparison sorting again) | 53,116 |
| arith, tostring, require (metamethods, `__tostring`, `package.preload`) | 40,740 / 39,924 / 35,204 |
| functions, index, ifs, blocks, tables, parens (the parser; `__index`) | 30,796 / 29,100 / 21,748 / 19,104 / 17,496 / 15,976 |
| zenix.lua (the whole functional test) | 29,100 |

A gsub level is about 0.9 KB (str_gsub's frame 676 bytes by `-fstack-usage` with the 256-byte buffer, 932 with
upstream's 512). Upstream's 200 levels would need about 250 KB for every Lua. **`STACK_lua` is 128 KB:** the deepest
kind measured, 112 KB, plus 19 KB. `tools/lua-test.sh` runs the probe build over all 18 kinds on every run and fails
unless the deepest leaves 8 KB of `STACK_lua` free, and runs them under `/bin/lua` itself, failing if the guard ever
fires. 100 nested C calls or 100 levels of brackets is far beyond hand-written code; a script that goes past gets
Lua's error. **Except binary chunks:** Lua does not check them (upstream: "binary chunks are not checked"), and
lundump's loading of nested functions has no depth limit, so a crafted chunk given to `load` (whose default mode
"bt" takes binary) can run past the stack, which without an MMU writes into memory that is not Lua's. Load binary
chunks only from `string.dump` of code you trust (docs/man/lua.txt says so).

**Found on the way, fixed: the allocator's `realloc`** (`user/zmalloc.c`, D27, shared by libz and the C library)
grew a block in place by taking the whole free block after it, not only what it needed. A block lying just before a
new chunk's untouched rest swallowed the rest (megabytes, once the heap had grown), the next request found nothing
free and took a new chunk, and so on: Lua parsing `cstack.lua` (5 KB) held 13 MB in 16 regions and failed with
"not enough memory". Now the rest stays free (`take`'s rule); `zmalloc-host grow` (in `tools/zmalloc-test.sh`)
fails on the old code (out of regions at the 17th block) and passes on the new; the recorded traces of the other
programs replay within their budgets as before. Every program using `realloc` was exposed to it.
**Also fixed:** picolibc's `fflush` took `NULL` as a stream and called through the low memory's
contents (Lua's `io.popen` calls `fflush(NULL)` first, as C allows: "every output stream"): every `io.popen`
crashed. The port has its own `fflush` now (`libc/zenix/stdio.c`; picolibc's `fflush.c` left out in
`libc/picolibc/sources.mk`, its file removed): every stream with output waiting is flushed, a stream last read (an
"r+" one too) left alone, as glibc does (flushing it would seek back over its buffered input, which a pipe or a
terminal cannot do); checked by `/tests/libctest` (a written stream's bytes in the file after `fflush(NULL)`, a pipe
being read keeping its buffered input). **Found, a limit (docs/limits.md):** picolibc's printf (its Ryu engine, D27)
gives a number's shortest exact digits when more are asked: `%.17g` of 0.1 is `0.1` (glibc `0.10000000000000001`)
and the subnormal 2^-1074 is `5e-324` at Lua's `%.14g`; round-trip safe, not C's exact expansion. Checked by
`tools/lua-test.sh` so a change shows.

**Tests:** `tools/lua-host-test.sh` (the PC, in `make check`'s host list): the pinned source built with the same
settings under AddressSanitizer and UBSan; `rootfs/tests/lua/zenix.lua` (112 lines of results: integers and their
wrap-around, floats and their printing, the maths library to 12 digits, strings and patterns, utf8,
`string.pack`, tables and sort, closures, varargs, deep Lua recursion, metatables and every metamethod kind,
`<close>`, errors and their messages, coroutines, goto, load and `string.dump`, `require`, files read and written
in every mode, `seek`, `io.lines`, rename and remove, `os.tmpname`, `io.popen` both ways, `os.execute`'s statuses,
`os.time` and `os.date` in UTC, the garbage collector's modes) must end with status 0 and leave no file;
`cstack.lua`'s 18 kinds must each end in Lua's own error. `tools/lua-test.sh` (FS-UAE, `make check`): the same
script's output on Zenix compared with the PC's line for line, the 18 kinds stopped by Lua and the guard never
firing, the probe build's deepest stack against `STACK_lua`, `lua -v`, `-e`, a script from standard input with arguments, a script file with arguments, an error's
message, traceback and status 1, a missing file, `os.exit(7)`, `-i` with a line typed at the prompt, the memory.
**Verified in FS-UAE 2026-11-13; not yet on a real Amiga** (the first run there should time `zenix.lua` on the stock
A1200 and the 1260, and run `cstack.lua`).

**Speed:** not measured on a real machine (emulator timing is not hardware timing). Expect a 14 MHz 68020 to run Lua
integer code at roughly a tenth of a 1990s PC's pace and floating-point code far slower (every double operation a
libgcc call); `lvm.c` at `-O2` instead of `-Os` is the first thing to try once the A1200 has numbers.

## 10. Questions for the partner

1. **The assembler** (section 6): our own `zas` (recommended) or vasm as a separate package with Volker
   Barthelmann's written consent?
2. **The C compiler** (section 7): tcc with a 68k back end of our own (recommended; one to two weeks) or vbcc +
   vasm + vlink as a separate package with their authors' written consent (Volker Barthelmann; vlink's Frank
   Wille)? Or both, vbcc later as an option?
3. **A live debugger** (section 8): may the kernel grow a way to stop a process and route its trace and breakpoint
   traps to a debugger (a design note with two reviews first)? Without it, `zdb` works on crashes only.
4. **The fun** (section 8): which first: Lua's display-and-sound module for games, the BSD terminal games, Frotz,
   or a Zenix intro (which waits for `DPY_SCREEN`)?
5. **Running a script by its `#!` line** (`./count.lua` instead of `lua count.lua`): Zenix's spawn did not read
   `#!`; Lua and later `make` and `tcc -run` would profit. A kernel or shell change, so the partner's. (Answered
   "Yes" 2026-11-13; built as DESIGN D53, docs/d53-shebang.md.)
6. **Lua on the demo floppies:** not planned (243 KB, 128 KB with gzip -9); the HD one has room. Wanted?

## 11. Sources

- vasm's licence: `doc/vasm.texi` in vasm 2.0f's source ("This archive may be redistributed without
  modifications ..."); vbcc's: Aminet `dev/c/vbcc_bin_amigaos68k.readme` (0.9h patch 2, "Legal").
- pdpmake: github.com/rmyorston/pdpmake, `LICENSE` (the Unlicense), commit 02f03e2 (2026-08-28).
- tcc: repo.or.cz/tinycc.git, mob, commit aade7ba (2026-11-09), `COPYING` (LGPL 2.1); `RELICENSING` lists the
  files whose authors also allow a BSD-style licence.
- Lua: lua.org/ftp, `lua-5.4.9.tar.gz` (2026-08-10) and its sha256 from that page; 5.5.1 (2026-07-24) is the
  newest 5.5.
- ACK: github.com/davidgiven/ack (BSD since April 2003, per its history; the `linux68k` platform, `mach/m68020`).
- pcc: pcc.ludd.ltu.se (BSD; older files under Caldera's licence; 1.1.0 of 2014 the last release).
- ASMotor: github.com/csoren/asmotor (GPL-3). EiGen: github.com/rochus-keller/EiGen (GPL-3, C++).
- binutils 2.18 (2007) moved to GPL-3; 2.17 was the last GPL-2+.
- The measurements: Zenix's `m68k-elf-gcc` 16.1 with `-m68020-60 -msoft-float -Os`, `m68k-elf-size` (sections
  1-7); FS-UAE 2026-11-13 (section 9).
