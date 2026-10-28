# D27: the C library, and D13 stage 2 decided (design note, revision 1)

Status: **design, revision 1; nothing built.** Roadmap block 3 ("A terminal you can live in"): "a fuller C library
(newlib); D13 stage 2 (shared libraries) decided". This note chooses the C library, says what Zenix's system calls
must back, how it lives next to libz and next to D13's shared code, decides D13 stage 2, and gives the rules for
porting the owner's AmigaOS programs. It needs **no new system call and no kernel change**; it changes the build, the
two link scripts (additions only) and, in the end, libz. The short version will be DESIGN.md D27.

Facts about today's code are *read* from main at 722d6f6 (2026-10-28). Sizes marked *measured* were taken on
2026-10-28 with the project's cross compiler (gcc 16.1, `m68k-elf`, the 68060 libgcc the Makefile links) by building
newlib 4.5.0 and picolibc 1.8.10 for Zenix's flags (`-m68020-60 -msoft-float -Os -ffunction-sections
-fdata-sections`), linking small test programs with a stand-in for the system-call glue through `link/user.ld` or
`link/pure.ld`, and making real ZEXE / ZEX2 files with `tools/mkzexe.py`; libz was built as the Makefile builds it
(`-O2`). The test programs were built, not run: running them is build-order step 1. Nothing is timed; speed is
judged from the code (*read*) until the stock A1200 times it. The owner's programs were *read* (their sources, not
changed).

## 1. What it is for, in plain words

- Zenix's programs today use libz, a small C library written for Zenix. It is enough for Zenix's own commands, but it
  is not standard C: no `FILE` and `fopen`, no `strftime` or `localtime`, no floating point, no `sscanf`, no
  `setjmp`, no wide characters, and its `fprintf` takes a file number instead of a `FILE`. Every program brought in
  from elsewhere needs its own little stand-in headers (`net/port/shim`, `user/miniz/shim`).
- Block 3 brings real programs: the owner's Sumi, Maki, Markdown, Ahiru and `ctop`, an IRC client, an e-mail client,
  a tracker music player. They need a full, standard C library, small enough for the stock A1200.
- **The choice: picolibc** (section 3), newlib's own descendant: the same newlib code, cleaned of everything
  GPL-incompatible and with a much smaller `printf`. *Measured:* a "hello" with it is a 4,256-byte program, smaller
  than the same with libz (4,384); a small sort tool 6,600 bytes against libz's 7,908 and newlib's 9,472.
- **In the end one C library** (section 6): ported programs use picolibc at once; Zenix's own programs move over
  when the measurements say they lose nothing; libz keeps only what is Zenix's own (paths, colours, error texts).
- **No shared libraries yet** (section 8): one copy of the C library for all programs would save only a few tens of
  KB on a stock A1200 and would freeze the library's whole interface for ever; programs that run several times
  already share their code (D13 stage 1), the C library inside them included.

## 2. Today (read)

- **libz** (`user/libz.c`, `user/libz.h`, about 1,080 lines): string and ctype functions; `malloc` over `SYS_memalloc`
  (size classes, made for `NREG` = 8 regions per process, tags checked by `free`; architecture.md section 12);
  `printf`/`snprintf` and `fprintf(int fd, ...)` with one integer-only formatter; buffered output (`oputs`,
  `oprintf`, `oflush`) and a line reader (`rd_getline`); `qsort`, `strtol`; the environment; the Zenix helpers
  (`path_join`, `dir_list`, `path_find`, `color_*`, `zwhy`, `zstrerror`, `bre_to_ere`); D16's POSIX signal calls,
  `poll`, `select`; D19's sockets. `zerrno` is the error variable, set by every failed system call in `usys.h`.
  Every program links it statically with `--gc-sections`; a pure program (ZEX2) links a pure build, `libz.po`.
- **The start** (`user/crt0.S`, frozen start contract): the kernel jumps to offset 0 with argc and argv on the stack;
  crt0 makes one PC-relative call to libz's `_zstart`, which sets `_zargv`, calls `main` and `_zexit` (flush, exit).
- **How ports carry libc needs today:** lwIP and the network programs include `net/port/shim/{stdio,stdlib,string}.h`,
  three 4-line files that include `libz.h`; miniz has its own `shim/` (`assert` as nothing, `realloc` declared);
  `regex.c` (CUtils') maps `cu_alloc` to `malloc` by `#define`; the crypto library is built with `CRYPTO_DEF`, among
  it `-DBR_USE_UNIX_TIME=0` because newlib's headers would switch on BearSSL's `time()` and a 64-bit division (D21).
  Each port works around the missing library its own way: the thing principle 2 warns about.
- **newlib on this PC:** the cross toolchain carries a newlib 4.5.0 build, but only for the default 68020 with a 68881
  (its `strtod` holds 125 FPU instructions, *measured*): unusable on a 68EC020. Any C library is built by Zenix itself.
- **What block 3's programs use** (*read*: the owner's TextTools, CFile/Ahiru, CUtils, tui.library):
  - The owner's programs use almost no C library: `string.h`, `malloc`/`free` (or exec's `AllocVec`), `atoi`,
    `strtol`, `qsort`, a few `time()`; **no `FILE` stdio, no `printf`, no floating point, no `setjmp`, no locale**.
    Ahiru formats with its own `fmt()`; CUtils prints through its own `cu_put*`; `ctop` and `top` use `long long`.
  - What they use a lot is **AmigaOS**: dos.library (Open/Read/Write/Seek, Lock/Examine/ExNext/ExAll, MatchFirst,
    Rename, DeleteFile, SetProtection, SetComment, GetVar, SystemTags, DateStamp/DateToStr, IoErr/Fault), exec
    (AllocVec, signals, ports, Forbid), and for Ahiru far more (intuition, graphics, commodities, ARexx, a copy thread
    made with CreateNewProc, an overlay loader, Paula). The terminal goes through tui.library: raw mode by `SetMode`,
    keys by an asynchronous `ACTION_WAIT_CHAR`, ANSI output, the window size by `CSI 0 q`.
  - So for the owner's programs the C library matters less than **a clean POSIX layer** (files, directories, time, the
    terminal, `poll`): that is what section 5 builds and section 9 maps to. The full standard library (stdio, `strftime`,
    floating point, wide characters) is for the ported Unix programs: the IRC and e-mail clients, the music player, and
    later anything else (roadmap phase 3: "porting small Unix and Minix programs").

## 3. Which C library

### 3.1 Measured sizes

Test programs: `t1` `printf("hello %d %s\n")`; `t2` `fopen`, `fgets`, `fputs`, `fprintf(stderr)`; `t3` a small sort
(`fgets`, `realloc`, `strdup`, `qsort`, `strtol`, `getenv`, `printf`); `t4` `time`, `localtime`, `strftime`; `t5`
`strtod`, `printf("%.3f %g")`, `sqrt`; `t6` `setjmp`/`longjmp`, `puts`; `t7` a mail-client-like mix (two files,
`snprintf`, ctype, `strerror`, `gmtime`, `strftime`, `malloc`). Bytes of the ZEXE file (*measured*, 2026-10-28):

| test | libz (`-O2`) | newlib 4.5.0, nano | picolibc 1.8.10, integer `printf` |
|---|---|---|---|
| t1 hello | 4,384 | 6,052 | **4,256** |
| t2 stdio files | (no `FILE`) | 8,480 | **5,512** |
| t3 sort tool | 7,908 (`rd_getline`) | 9,472 | **6,600** |
| t4 time | (none) | 23,040 | **16,824** |
| t5 floating point | (none) | 33,844 (`-u _printf_float`) | **18,232** (float `printf`) |
| t6 setjmp | (none) | 4,252 | **2,944** |
| t7 mail-like mix | (none) | 28,348 | **21,900** |

Per process (*measured*, data + BSS): libz's hello 4,108 bytes (its 4 KB output buffer), newlib-nano's 416, picolibc's
606 (a 512-byte output buffer, section 5.2). The library's stack frames are small: picolibc's deepest, `vfscanf`, 160
bytes; `vfprintf` 116; `strftime` 108 (*measured*, `-fstack-usage`), against libz's formatter's 512-byte buffer on
the stack. Every row passes `tools/check-68020-60.sh`.

Where the bytes go (*measured*, `nm`): `t4`'s 12 KB over hello are `strftime` (3.1 KB), the time-zone code (1.7 KB,
it reads `TZ` with `sscanf`: 1.1 KB), `localtime_r`/`gmtime_r` (1.1 KB) and libgcc's 64-bit division (2.2 KB: picolibc's
`time_t` is 64 bits, good past 2038). `t5`'s 14 KB over hello are the float `printf` (2.9 KB), `strtod` and its engine
(1.8 KB), the conversion tables, `sqrt` and libgcc's soft-float routines; a program that does not use floating point
pays none of it.

**`printf` is chosen per program at link time** (picolibc's `vfprintf` variants, `--defsym=vfprintf=__X_vfprintf`):
integer 3,858 bytes of text for t1; with `long long` 4,134; float-only 11,988; double 13,312 (*measured*). Zenix's
default: **integer with `long long`** (`ctop`'s counters; 276 bytes more); a program that prints floating point says
so in the Makefile (`LIBC_FLOAT_name`).

### 3.2 Licence (GPL-2.0-only)

- **picolibc:** every file used to build the library is under BSD-like licences (2- and 3-clause BSD and older
  permissive ones), listed file by file in its machine-readable `COPYING.picolibc`; its README says no GPL-related
  newlib parts are left. The only GPL/AGPL files in its tree are two `printf` test files and a cross-file helper
  script, none of them built or vendored. GPL-2.0-compatible; the notice ships as `/usr/share/licenses/picolibc` in
  every full image and the base package, as lwIP's does (D19).
- **newlib:** 57 licence sections in `COPYING.NEWLIB`, permissive for the m68k-elf build (the LGPL parts are for Linux
  targets only); compatible too, but the bookkeeping is newlib's own, not file by file.
- Our port's files (`libc/zenix/`, section 12) are new, `GPL-2.0-only`.

### 3.3 The decision: picolibc 1.8.10

- **Smaller in every row,** 25-45% below newlib-nano and below libz where libz can do the same (3.1).
- **The same code base the roadmap meant:** picolibc is newlib's code with AVR-libc's small stdio (`tinystdio`) in
  place of newlib's; the roadmap's "newlib" was chosen for its completeness, and
  picolibc keeps that. **Owner question 1** (section 14), because the roadmap names newlib.
- **No reentrancy structure:** newlib keeps per-thread state in `struct _reent` reached through `_impure_ptr`;
  picolibc uses plain globals (`errno` is an `int`), which is what a single-threaded Zenix process and `-msep-data`
  want (each global one table slot).
- **Built for small machines and m68k:** its m68k `memcpy`/`memset` are hand-written (16 bytes per loop), it has m68k
  `setjmp`, a "C" plus optional UTF-8 locale, and build options for everything Zenix does not want.
- **Pure-ready** (section 7): all of its objects compile through `tools/purecc.sh` (*measured*: 982, none failed),
  and t1, t2, t3 and t6 link as ZEX2 programs `mkzexe` accepts.
- **Rejected:**
  - *newlib itself:* bigger in every row, `_reent`, and nano's float `printf` silently prints nothing unless a
    special link flag (`-u _printf_float`) pulls it in.
  - *libz grown into a full library:* `strftime`, time zones, `strtod` and float formatting, `sscanf`, wide characters
    and locales written and tested by us, for a result picolibc already is, smaller (3.1).
  - *musl, uClibc-ng, dietlibc:* built around Linux's system calls and process model (`fork`, `mmap`, threads);
    bigger; uClibc-ng LGPL. *klibc, PDCLib:* incomplete. *Fuzix's libc:* small but written for 8/16-bit Fuzix, not
    for porting today's Unix programs. *libnix* (the owner's AmigaOS programs use it): built on AmigaOS.

## 4. How it is built

- **Vendored, only the files used** (as BearSSL in `crypto/`): `libc/picolibc/` holds picolibc 1.8.10's sources that
  the build compiles, its `COPYING.picolibc` and a README naming the version and how to update; a generated
  `picolibc.h` (the configuration header meson would write) is checked in. **The Makefile compiles the list
  directly** (`LIBC_SRCS`, as `CRYPTO_SRCS`): no meson in Zenix's build.
- **The configuration** (meson's names, for the record): `tinystdio`, single thread, no thread-local storage,
  `newlib-nano-malloc` off (Zenix's own `malloc`, 5.1), `picocrt` off (Zenix's crt0), `semihost` off,
  `posix-console` off (Zenix's own three streams, 5.2), `io-long-long` on, `mb-capable` on with only UTF-8
  (`mb-extended-charsets` off: *measured*, a program using `mbrtowc`/`wcwidth` grows from 5,152 to 9,664 bytes, every
  other program by nothing), `fortify-source` none, `assert-verbose` on.
- **Two builds,** as libz: `build/libc.a` (ZEXE programs) and `build/libc-pure.a` (every object through
  `tools/purecc.sh`, for ZEX2 programs). `-msoft-float` everywhere: one build runs on every 68020-68060; an FPU build
  of float-heavy programs for the 060 is a later choice, not now.
- **The golden path:** `-Os` for the library, `-O2` for what programs spend their time in: the string and memory
  functions (picolibc's m68k assembler `memcpy`/`memset`; `strlen`, `strcmp`, `strchr`, `memchr`, `memcmp` at `-O2`),
  the integer `printf` engine, the stdio buffer code (`getc`/`putc`/`fgets`/`fwrite`) and `malloc`.
- `tools/check-68020-60.sh` runs over the library's objects as over programs.

## 5. What Zenix's system calls back (the port, `libc/zenix/`)

Picolibc calls POSIX names (`read`, `open`, ...); the port gives them over `usys.h`'s system calls. **Every one is
there today; nothing new is needed in the kernel.** The port's parts:

| Area | C library names | Zenix's call (read from `usys.h`) |
|---|---|---|
| Files | `open` (Zenix's `O_*`), `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `pipe`, `pipe2`, `fcntl` (`F_GETFL`/`F_SETFL` `O_NONBLOCK`, `F_GETFD`/`F_SETFD` accepted) | 8, 9, 10, 2, 11, 49, 50, 17, the existing libz `fcntl` |
| Names | `stat`, `lstat`, `fstat` (`struct stat` filled from `ustat`), `unlink`, `rename`, `mkdir`, `rmdir`, `link`, `symlink`, `readlink`, `chmod`, `chown`, `utime(s)`, `chdir`, `getcwd`, `umask`, `access` (from `stat` and the ids) | 13, 57, 14, 26, 28, 25, 27, 53, 54, 55, 52, 66, 56, 15, 16, 65 |
| Directories | `opendir`, `readdir`, `closedir`, `rewinddir`, `scandir`; `glob` (the port's; picolibc has none) over them and picolibc's `fnmatch` | `open` + 12 (`udirent`, with its size) |
| Memory | `malloc`, `free`, `calloc`, `realloc`, `posix_memalign`, `malloc_usable_size`; `sbrk` answers `ENOMEM` | 18, 19 (5.1) |
| Time | `time`, `gettimeofday`, `clock_gettime` (`CLOCK_REALTIME` from 58; `CLOCK_MONOTONIC` from the E-clock, 36), `nanosleep`/`usleep`/`sleep` (10 ms ticks), `alarm`, `clock` (ticks since the process began, said to be wall time: there is no CPU accounting) | 58, 36, 20, 7, 87 |
| Processes | `getpid`, `getppid` (`procinfo`), `getuid`... `setgid`, `posix_spawn`/`posix_spawnp`, `system`, `popen`/`pclose`, `waitpid`, `wait`, `_exit`, `kill`, `raise` | 4, 21, 61-64, 67, 5, 6, 1, 24 |
| Signals, waiting | `sigaction`, `signal`, `sigprocmask`, `pause`, `poll`, `select`, process groups | libz's D16 code (moved, section 6) |
| Terminal | `isatty`, `tcgetattr`/`tcsetattr` (the subset in 5.4), `ioctl(TIOCGWINSZ)`, `tcsetpgrp` | 60, 45, 46, 23 |
| Environment | `getenv`, `setenv`, `unsetenv`, `environ` (at most 1 KB, `ENVMAX`) | 47, 48 |
| Network | sockets, `inet_*`, `getaddrinfo`/`freeaddrinfo` (libz has them today, D19) | 90 (libz's code, moved) |
| Regular expressions | `regcomp`/`regexec`/`regfree`: a thin POSIX wrapper over `user/regex.c`, so Zenix keeps one regex engine (picolibc's own is not vendored) | none |
| Users | `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid` over `/etc/passwd` and `/etc/group` (frozen formats) | `user/pw.c`'s parser, one copy |

**The constants are Zenix's, from one source.** picolibc's headers carry newlib's numbers, which are not Zenix's: its
`ENOTSOCK` is 108 (Zenix 88, Linux's), its `O_CREAT` 0x200 (Zenix 0x40), its `SIGUSR1` 30 (Zenix 10, Linux's). The
port puts its own `errno.h`, `fcntl.h` (`sys/_default_fcntl.h`), `signal.h` (`sys/signal.h`), `limits.h` additions
(`PATH_MAX` 64: system calls take paths of at most 63 bytes) and `sys/stat.h` bits ahead of picolibc's in the include
path, each generated from or including `kernel/errno.h` and `kernel/kernel.h` (the frozen numbers), never typed twice.
`strerror` gives libz's texts (one table, section 6). A host test checks every constant against the kernel's headers.

### 5.1 Memory: libz's allocator, moved

`malloc` is libz's: size classes cut from chunks that grow 4 KB, 16, 64, 256, 512 KB, so a program uses few of its
eight regions; big blocks get a region and go back on `free`; a pointer `malloc` did not give out is reported, never
followed ("safety stays"). picolibc's own `malloc` needs `sbrk`, one growing heap, which a no-MMU Zenix cannot give.
The source moves to one file both libraries build (section 6), so there is one allocator. It gains what the standard
asks and libz lacks: `posix_memalign`/`aligned_alloc`, `malloc_usable_size`, `reallocarray`.

### 5.2 The three streams

`stdin`, `stdout`, `stderr` are the port's: `stdin` buffered, `stdout` line-buffered on a terminal and fully buffered
otherwise, `stderr` unbuffered, as C says. **The buffer is 4 KB, taken by `malloc` on first use** (libz's output
buffer is 4 KB of BSS in every program today; picolibc's own console is 512 bytes, eight times the system calls for
`cat`-like output): the console path is a hot path, and a program that never prints holds nothing. `exit` flushes them;
a write that fails with `EPIPE` ends the program with status 141, as libz's `oflush` does (SIGPIPE's status).

### 5.3 The start and the end

- crt0 is unchanged: one `bsr.l _zstart`. The port's `_zstart` sets `environ` (from `SYS_envget`, as libz's
  `env_load`), `_zargv`, runs the `init_array` (empty for C programs; C++ is not a goal), calls `main`, then `exit`:
  `atexit` functions, the `fini_array`, the streams flushed, `SYS_exit`. **The start contract (the stack at entry,
  `a5`) does not change.**
- The link scripts gain `PROVIDE`d `__init_array_start/end` and `__fini_array_start/end` and `KEEP` of the two
  sections (picolibc's `exit` refers to them; without them a pure link fails on undefined weak symbols, *measured*).
  `PROVIDE` defines a symbol only when something refers to it, so **every program built today must stay
  byte-identical** (checked as D13 step 1 checked it).

### 5.4 The terminal

- `tcgetattr`/`tcsetattr` give a **termios subset** over `SYS_ttymode`: `ICANON` and `ECHO` off together are
  `TTY_RAW`, `ISIG` off is `TTY_NOSIG`; other bits are kept in the structure and answered back but do nothing;
  `VMIN`/`VTIME` other than 1/0 are refused with `EINVAL` (a timed read is `poll` with a timeout, as every Zenix
  program does it). `cfmakeraw` sets both bits.
- `ioctl(fd, TIOCGWINSZ)` answers rows and columns from `SYS_ttyinfo` (80 x 32 PAL, 80 x 25 NTSC; telnet's and
  pseudo-terminals' sizes as D20 gives them). No `SIGWINCH` (the text console never changes size).
- Keys arrive as the standard escape codes the console sends (architecture.md section 11); the console understands
  ANSI/ECMA-48 output and D14's colours. `TERM` names it (`/etc/profile`).

### 5.5 What is not there, and says so

`fork`, `vfork`, the `exec` family, `mmap`, `sbrk`, threads (`pthread_create`), `getrusage`, `ftruncate`/`truncate`
(no system call; `O_TRUNC` is), `fsync` (it is `SYS_sync`, the whole cache, documented), `dlopen`. Each is either
absent (a link error, the honest answer when porting) or, where programs commonly test for it, present and failing
with `ENOSYS`; the list goes in limits.md. `posix_spawn`'s file actions are done in the parent around the spawn, so
they can only set the child's descriptors 0-2 (Zenix passes only those; architecture.md section 6): other actions are
refused with `EINVAL`. Whether Zenix ever gets an `exec` that replaces a process in place is a kernel design of its
own, opened only if a port needs it.

## 6. libz and the C library: one or the other per program, then one

**A program links one of them, never both:** their prototypes collide (`fprintf(int fd, ...)` against
`fprintf(FILE *, ...)`, `fputs(s, fd)` in `pw.h`, `strlen` returning `u32`).

1. **Now (block 3):** every program that exists keeps libz; every ported or new block-3 program uses picolibc. The
   Zenix helpers a ported program wants (`path_join`, `dir_list`, `color_auto`, `zwhy`, `zsig_name`...) come as
   **libzenix** (`zenix.h`): the same source files libz uses, built against the C library's headers. The code the two
   share (the allocator, signals, `poll`/`select`, sockets, the error texts, the passwd parser) lives in **one file
   each**, compiled into both, so nothing is written twice. `zerrno` and `errno` are one variable (the port defines
   `errno`; `usys.h` writes it under the C library, `#define zerrno errno`).
2. **The bridge (a small mechanical step):** libz's non-standard names get standard ones: `fprintf(int fd, ...)` and
   `vfprintf(int fd, ...)` become POSIX's `dprintf` and `vdprintf` (the same meaning, the standard name), `pw.h`'s
   `fputs(s, fd)` a private name, sizes `size_t`. Every program's behaviour is unchanged (its output compared by the
   existing tests); after it, a Zenix program's source compiles against either library.
3. **The switch, measured:** every program built both ways, a table of sizes (`tools/libc-size.sh`); the base system
   moves to picolibc + libzenix when the table shows **no program grows by more than 2% (or 256 bytes) and the hot
   programs (`sh`, `ls`, `cat`, `grep`, `less`) lose no speed** (static instruction counts, then the stock A1200's
   `bench` and text-tool runs, D13 section 3's rule). Then libz's C-library half is removed, libz is libzenix, and
   principle 2 holds again: one C library. The built-in servers (diskd, fsd, boot) move with the rest; the kernel's
   size line (`KERNEL_MAX`) is watched in the same table.

Until step 3, the C library is not part of "base" in D15's sense beyond what every program carries: it is linked
into each program, so (as libz) it needs no frozen-interface rule of its own (CLAUDE.md). A fix to it reaches users as
new builds of the packages that contain the fixed code; builds are reproducible (D21 8.3's build date), so D22's
`zpkg` sees which packages really changed.

## 7. Shared code (D13 stage 1) with the C library (measured)

The pure build (`libc-pure.a`, every object through `purecc.sh`) and Zenix's `crt0 -DZEX2`, `link/pure.ld`:

| test | ZEX2 file | shared part | data part | refused because |
|---|---|---|---|---|
| t1 hello | 4,280 | 3,904 | 220 | - |
| t2 stdio files | 5,492 | 4,896 | 344 | - |
| t3 sort tool | 6,384 | 5,968 | 240 | - |
| t6 setjmp | 2,912 | 2,560 | 208 | - |
| t4 time, t7 mix | refused | | | libgcc's 64-bit division reaches `__clz_tab` by an absolute address in the shared part |

What made it work, and what the port must do (*measured*):
- the link scripts' init/fini symbols (5.3);
- weak references picolibc leaves (its `exit` flushes `stdin` weakly; `strerror` calls a weak `_user_strerror`): the
  port defines the streams and its own `strerror`, so none are left undefined (mkzexe refuses an undefined weak
  symbol, D13 section 4);
- **the 64-bit division:** libgcc's `__udivdi3` family (from the 68060 multilib) is not pure. The port carries its
  own `__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3` (new code, C over the 68020's 32-bit `divu.l`, which every CPU up
  to the 060 has; no table), linked before libgcc in both builds. This also serves the pure `long long` of `ctop`.
- **Floating point stays out of pure programs** (D13 section 8: libgcc's soft-float writes `_fpCCR` by address): a
  program that uses floating point is a ZEXE program, as `awk` is, until a pure soft-float exists. Few block-3 programs
  need it.
- A pure program's own data part grows by the library's globals: about 200-350 bytes measured.

## 8. D13 stage 2, shared libraries: decided - not now, and when

**The numbers.** The C library inside a program is 2.5-21 KB (section 3.1: the t-programs minus their own code); in a
typical block-3 program (a text-UI program with stdio and time) about 6-17 KB. In a lively session on the stock A1200
- shells, Sumi, Maki, an IRC client, the e-mail client - four distinct programs carry it, so one shared copy would save
about **20-50 KB** (*estimate*) of the about 1,130 KB free with one login: 2-4%. The text-UI library in the same four
would add a similar amount (its AmigaOS original is about 1,800 lines). Two of the same program already share
everything (D13 stage 1, section 7).

**The costs.**
- **A frozen interface the size of the C library.** Today the C library is linked into each program and freezes nothing;
  a shared one makes every exported function, structure layout (`FILE`, `struct stat`, `struct tm`) and global a
  frozen interface for ever, because an older program meets a newer library after every update (D15's rule), and
  the library could never again be fixed by changing a layout.
- **Per-process state everywhere:** `errno`, the three streams, the allocator's lists, `environ`, `atexit`, the time
  zone. A shared C library needs a second data base per opener on every call (AmigaOS passes it in `a6`; gcc's
  `-mid-shared-library` reloads `a5` in every function: D13 section 9 *measured* `sh` at 23,618 instructions against
  22,310, 5.9% more, and 792 indirect calls).
- **Kernel and format work:** a library segment kind, opener data blocks, a resolver: an *estimated* 2-4 KB of kernel
  (its RAM-floppy budget is watched) and a new file kind next to ZEXE/ZEX2.

**The decision: stage 2 is not built in block 3; the C library stays static** (and shared per program by stage 1). It
is reopened, with its own design note and two reviews, when either holds:
- the same library code, duplicated across programs that run at once in the owner's daily session, measured on the
  stock A1200 (`free`'s figures with those programs running), reaches **100 KB**; or
- the desktop (block 5) has its client library, which every graphical program will carry: the likely first real
  shared library.

**Its shape then** (D13 section 9 already leans this way): an AmigaOS-style library for libraries written for
Zenix, mostly stateless (the display client, the text UI, perhaps the crypto library): a jump table, opened by name, a
data base per opener in a register, a **new magic** of its own; ZEXE and ZEX2 stay exactly as they are, and a program
that opens no library pays nothing. Not the C library.

## 9. Porting rules for block 3's programs

**General.**
1. **One source for both systems where it is the owner's own program** (owner question 3): the program keeps its
   repository; its AmigaOS calls are gathered behind the program's own small OS layer (CUtils has one, `cu.h`; Sumi,
   Maki and Markdown share tui.library), and that layer gets a Zenix implementation in C over the C library. Zenix's
   tree carries a pinned copy (as lwIP and BearSSL) under `ports/NAME/`, updated by a script, its licence header kept.
2. **A program from elsewhere** (an IRC client, a MOD player) is chosen first for its licence (GPL-2-compatible), its
   size and its needs: POSIX, `poll`, no threads, no `fork`, no `mmap`. Candidates are judged at their own step.
3. **No threads, no `fork`:** a second task becomes a second process (`posix_spawn` and a pipe), or work done in
   slices in the program's `poll` loop (Ahiru's copy job).
4. **Pure when several may run at once** (an editor, a pager): ZEX2, so a second Sumi costs only its data; ZEXE for
   programs that use floating point (section 7) or send raw messages (D13 section 5).
5. **Stacks:** the program's measured need in its Makefile line (`STACK_name`, `-fstack-usage`), never AmigaOS's
   `StackSwap`. Paths at most 63 bytes (`PATH_MAX` 64); 16 descriptors per process.
6. **Sizes on the stock A1200 said in each port's commit** (file, shared/data parts, memory with it running).

**AmigaOS to Zenix** (what replaces what):

| AmigaOS | Zenix |
|---|---|
| `AllocVec`/`FreeVec`, `AllocMem` (any memory) | `malloc`/`free`. `MEMF_CHIP` has no place in a program: what needs chip RAM (sound, pictures) goes through a server |
| `Open`/`Read`/`Write`/`Seek`/`Close`, `FGets`, `FPuts`, `PutStr` | `open`/`read`/`write`/`lseek`/`close`, or stdio |
| `Lock`/`Examine`/`ExNext`/`ExAll`, `MatchFirst` (`#?`) | `stat`/`lstat`, `opendir`/`readdir`, `fnmatch`/`glob` (Unix wildcards) |
| `Rename`, `DeleteFile`, `CreateDir`, `SetProtection` | `rename`, `unlink`/`rmdir`, `mkdir`, `chmod` (Unix modes: `rwxrwxrwx`, not `hsparwed`) |
| `SetComment`, `.info` icons, assigns, volumes, the DosList | none: Unix paths and the mount table (`/`, `/mnt/...`) |
| `GetVar`/`SetVar` | `getenv`/`setenv` (the environment, at most 1 KB) |
| `SystemTags`, `Execute`, `CreateNewProc` | `posix_spawn`, `system`, `popen` |
| `Wait`, `SetSignal`, Ctrl-C as `SIGBREAKF_CTRL_C` | `poll` (descriptors and ports), signals (`SIGINT` ...: D16) |
| `DateStamp`, `DateToStr`, `StrToDate`, timer.device | `time`, `localtime`/`strftime`, `clock_gettime`, `poll`'s timeout |
| `SetMode(fh, 1)`, `WaitForChar`, `ACTION_WAIT_CHAR` | `tcsetattr` raw (5.4), `poll` on descriptor 0 |
| `CSI 0 q` (the window's size) | `ioctl(TIOCGWINSZ)` |
| `CSI` as 0x9B in input | the console sends `ESC [` |
| `IoErr`, `Fault` | `errno`, `strerror` |
| utf8.library | `mbrtowc`, `wcwidth` in `C.UTF-8` (section 4); the console's UTF-8 is a block-3 item of its own |
| clipboard.device, commodities, ARexx, intuition/graphics | none in the text console (the desktop, block 5, or later); the program works without them |
| tui.library | Zenix's text-UI library (the next roadmap item), over 5.4 and ANSI output |
| exec's task lists (`ps`, `top`, `ctop`) | `SYS_procinfo`, `SYS_meminfo` (`ps` and `free` show the way) |
| Paula and `audio.device` (a MOD player) | not the C library's: the player needs a sound interface the kernel does not have yet (only `beep`; architecture.md section 3, "Sound"): the main session decides whether block 6's Paula server comes forward |

## 10. Size and speed budgets

- **Per program** (*measured* figures as the limits, checked by `tools/libc-size.sh` in `make check`): hello at most
  4.5 KB; a stdio + `malloc` + `qsort` tool at most 7 KB; the time functions at most 13 KB more; floating point at
  most 15 KB more; UTF-8 functions at most 5 KB more; the library's data + BSS in a process at most 1 KB (the 4 KB
  output buffer is heap, taken on first use).
- **Per process on the stock A1200:** no more than libz costs today (its first `malloc` chunk, 4 KB, is the same).
- **The base system's switch** (section 6 step 3): no program more than 2% (or 256 bytes) bigger, the RAM floppy's and
  the demo floppy's (D25) budgets kept.
- **Speed** (said, not yet timed): the string functions are as fast or faster (picolibc's `memcpy` moves 16 bytes per
  loop, libz's 4, *read*). The stock A1200 times `cat`,
  `grep`, `sort` and `ls` before and after the switch (D13 section 3's rule); a loss over 5% on a hot program blocks it.

## 11. Tests

- **Host** (`tools/libc-host-test.sh`): every constant the port's headers give (`E*`, `O_*`, `S_*`, `SIG*`, `PATH_MAX`)
  equals the kernel's; `libc.a` and `libc-pure.a` hold no FPU instruction and pass `check-68020-60`; the port's
  64-bit division against the host's for a table of values and random ones; the size table against the budgets.
- **FS-UAE** (`tools/libc-test.sh`, in `make check`): `libctest`, a test program in `/tests` (not a command), compares
  its output with expected text: `printf`/`snprintf` formats (widths, flags, `%lld`, `%n` refused), `sscanf`, `strtol`
  edges, `qsort`, `fopen` modes `r w a r+ w+` (create, truncate, append checked with `stat`), `fseek`/`ftell`, `ungetc`,
  line buffering on a console and full buffering into a pipe, `EPIPE` ends with 141, `malloc` stress (thousands of
  blocks, every class, `realloc` growth, a bad `free` reported), `setjmp`, `time`/`gmtime`/`strftime` of fixed times,
  `TZ=CET-1CEST,M3.5.0,M10.5.0/3`, `mbrtowc`/`wcwidth` of Swedish and wide characters, `strerror` texts equal libz's,
  `opendir` over a directory of 200 names, `posix_spawn` and `popen` with a pipe, `tcsetattr` raw and back, the window
  size; a float build checks `%f`/`%g` and `strtod`. Its ZEX2 build runs on consoles 2 and 3 at once (one segment,
  `free` shows it). The bridge (section 6 step 2) is checked by the existing command tests' outputs.
- **On the A1200:** with the base system's switch, the timed runs of section 10.

## 12. For the main session (claims; not owner questions)

- **No system call, message type, error number or kernel change.** The constants the library bakes into programs are
  the frozen ones; if the port finds it needs a call (none known), it is claimed in CLAUDE.md's table first.
- **New directories:** `libc/picolibc/` (vendored subset, its licence file) and `libc/zenix/` (the port: system layer,
  headers, the three streams, `_zstart`/`exit`, `strerror`, the 64-bit division, termios, `TIOCGWINSZ`); `ports/` for
  ported programs (9). The shared files of section 6 step 1 (allocator, signals, `poll`, sockets, error texts) move out
  of `libz.c` into `user/` files both builds compile: a libz change, the main session's.
- **Makefile:** `LIBC_SRCS`, `LIBC_O2`, `build/libc.a`, `build/libc-pure.a`, `LIBC_FLOAT_name`, a link rule for C
  library programs (`$(BUILD)/libc.a` before `$(LIBGCC)`), `tools/libc-size.sh` and `tools/libc-test.sh` in `make check`.
- **Link scripts:** `link/user.ld` and `link/pure.ld` gain the `PROVIDE`d init/fini array symbols and their `KEEP`;
  every existing `.zx`/`.pzx` checked byte-identical.
- **`usys.h`:** `#define zerrno errno` when built under the C library (one error variable).
- **libz:** the bridge renames (`dprintf`, `vdprintf`; `pw.h`'s `fputs`) across the tree in one commit; the end of
  libz's C half at the switch.
- **Docs:** DESIGN.md D27 and its line 40 ("no newlib: Zenix's own small C library") corrected at the switch;
  ROADMAP block 3's "(newlib)" to "(picolibc, newlib's descendant)" after the owner's answer; architecture.md sections 4
  ("There is no shared-library format yet") and 12; limits.md (5.5's list, `PATH_MAX`, 16 descriptors, termios
  subset); source-tree.md (it says "own small library, not newlib"); vs-unix-linux.md; features.md; the licence file
  in `rootfs/` and the base package.
- **Review depth:** one review for the library (a feature: no kernel change); two for the base system's switch
  (section 6 step 3: it touches every program and the built-in servers).

## 13. Build order (each step small, merged with its tests; the full suite before each merge)

0. **The library builds** (no program uses it): `libc/picolibc/` vendored with `picolibc.h`, both archives from the
   Makefile, `check-68020-60` over them, the licence file in the images. Host: the archives' size and FPU check.
1. **The port and its first test:** `libc/zenix/` with the system layer, the headers from the kernel's constants (host
   test), the allocator and the other shared files moved out of `libz.c` (libz's programs byte-identical or their
   tests unchanged), the three streams, `_zstart`/`exit`, `strerror`; `libctest` (ZEXE) in `/tests`;
   `tools/libc-test.sh`.
2. **Pure:** `libc-pure.a`, the port's 64-bit division, the link scripts' init/fini symbols (every existing file
   byte-identical), `libctest` as ZEX2 on two consoles.
3. **Terminal and processes:** termios subset, `TIOCGWINSZ`, `posix_spawn`, `system`, `popen`, `getpw*`, `getaddrinfo`;
   their `libctest` parts. *The C library is ready for the text-UI library and the first port (the next roadmap items).*
4. **The bridge:** `dprintf`/`vdprintf` and the other renames across the tree; every test's output unchanged.
5. **The switch, measured:** the size table for every program both ways; static counts for the hot programs; with the
   owner home, the stock A1200's timed runs; then the base system on picolibc + libzenix, libz's C half removed, the docs
   of section 12. Two reviews.

## 14. Open questions for the owner (one line each, the recommendation in brackets)

1. The roadmap says newlib; picolibc is newlib's own smaller descendant (the same code, a smaller `printf`, cleaner licences), measured 25-45% smaller per program: use picolibc? [Yes]
2. Ahiru and the CFile core it uses are in a private repository; porting Ahiru puts its code into Zenix's tree, which will be public: may it be published, under MIT like Sumi, Maki and CUtils? [Yes, MIT]
3. Your own programs (Sumi, Maki, Markdown, Ahiru, `ctop`) keep one source for AmigaOS and Zenix, each in its own repository, and Zenix carries a copy, rather than Zenix getting its own versions? [Yes, one source]

## 15. Reviews

(To be filled in after the two independent reviews.)

Last checked against the code: 2026-10-28 (main at 722d6f6).
