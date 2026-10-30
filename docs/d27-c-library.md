# D27: the C library, and D13 stage 2 decided (design note, revision 2)

Status: **design, revision 2 after two independent reviews (section 15); build steps 0-4 built 2026-10-29 (section 13,
"As built"); the text-UI library ported (the last section).** Roadmap block 3 ("A terminal
you can live in"): "a fuller C library (newlib); D13 stage 2 (shared libraries) decided". This note chooses the C
library, says what Zenix's system calls must back, how it replaces libz's C half without making Zenix's own programs
bigger, how it works with D13's shared code, decides D13 stage 2 (for the C library and for the text-UI library), and
gives the rules for porting the owner's AmigaOS programs. It needs **no new system call and no kernel code change**:
three error numbers are added to `kernel/errno.h` (section 5), the build and the two link scripts grow (additions
only), and libz is reshaped in steps. The short version will be DESIGN.md D27.

Facts about today's code are *read* from main at 722d6f6 (2026-10-28). Sizes marked *measured* were taken on
2026-10-28 with the project's cross compiler (gcc 16.1, `m68k-elf`, the 68060 libgcc the Makefile links) by building
newlib 4.5.0 and picolibc 1.8.10 for Zenix's flags (`-m68020-60 -msoft-float -ffunction-sections -fdata-sections`,
`-Os` unless said), linking small test programs through `link/user.ld` or `link/pure.ld` and making real ZEXE / ZEX2
files with `tools/mkzexe.py`. **The test programs were linked with a stand-in for the port** (a few lines of system
calls, an `sbrk` for the libraries' own `malloc`, no environment loading, no `strerror` table, picolibc's own console
streams): the real port (libz's allocator, its error texts, `_zstart`'s environment) will differ by some hundreds of
bytes, measured again at build step 1. The programs were built, not run (running them is step 1). Nothing is timed:
speed is judged from the code (*read*) until the stock A1200 times it. The owner's programs were *read*, not changed.

## 1. What it is for, in plain words

- Zenix's programs use libz, a small C library written for Zenix. It is enough for Zenix's own commands but it is not
  standard C: no `FILE`/`fopen`, no `strftime`/`localtime`, no floating point, no `sscanf`, no `setjmp`, no wide
  characters, and its `fprintf` takes a file number. Every program brought in from elsewhere needs its own stand-in
  headers (`net/port/shim`, `user/miniz/shim`).
- Block 3 brings real programs: the owner's Sumi, Maki, Markdown, Ahiru and `ctop`, an IRC client, an e-mail client,
  a tracker music player. Ported programs need a full standard C library, small enough for the stock A1200.
- **The choice: picolibc** (section 3), newlib's own descendant: the same code, a smaller `printf` and stdio, licences
  listed file by file. It is 25-45% smaller than newlib per program (*measured*).
- **Standard C costs a little; Zenix's own programs do not pay it** (section 6). *Measured* at the same compiler
  setting: a hello program is 2,192 bytes with libz, 2,312 with picolibc's formatter writing straight to the file
  (as libz's `printf` does today), and 4,532 with C's `printf` and its `FILE` streams. So Zenix's own programs keep
  writing straight to file descriptors (the standard `dprintf` and Zenix's buffered `oputs`) and pay about 120 bytes;
  a ported program that uses `FILE` streams pays about 2.2 KB more for them.
- **In the end one C library** (section 6): picolibc, Zenix's port of it and Zenix's own helpers; libz's own copies of
  the string, format and memory code go, and every program uses one allocator, one formatter, one regex engine.
- **No shared libraries yet** (section 8): one shared copy of the C library would save about 20-70 KB in a busy session
  on the stock A1200 and would freeze the library's whole interface for ever. Programs that run several times already
  share their code (D13 stage 1), the C library inside them included.

## 2. Today (read)

- **libz** (`user/libz.c`, `user/libz.h`, about 1,080 lines, built `-O2`): string and ctype functions; `malloc` over
  `SYS_memalloc` (size classes, made for `NREG` = 8 regions per process, tags checked by `free`; replaced 2026-10-30:
  "The allocator"; architecture.md
  section 12); `printf` (straight to descriptor 1), `snprintf` and `fprintf(int fd, ...)` with one integer-only
  formatter; buffered output to descriptor 1 (`oputs`, `oprintf`, `oflush`; 4 KB of BSS) and a line reader
  (`rd_getline`); `qsort`, `strtol`; the environment (at most 63 variables, 1 KB); the Zenix helpers (`path_join`,
  `dir_list`, `path_find`, `color_*`, `zwhy`, `zstrerror`, `bre_to_ere`); D16's signal calls, `poll`, `select`; D19's
  sockets and `getaddrinfo`; `fcntl` (`O_NONBLOCK` on sockets only). `zerrno` (defined in libz.c) is set by every
  failed system call in `usys.h`. Every program links it statically with `--gc-sections`; pure programs link
  `libz.po`. Of the programs in `UAPPS` with a source file, 71 of 132 call `printf`, and 97 files under `user/` and
  `net/` call `fprintf(fd, ...)` or `vfprintf(fd, ...)`.
- **The start** (`user/crt0.S`, frozen start contract): the kernel jumps to offset 0 with argc and argv on the stack;
  crt0 makes one PC-relative call to libz's `_zstart`, which sets `_zargv`, calls `main` and `_zexit` (flush, exit).
- **How ports carry libc needs today:** lwIP and the network programs include `net/port/shim/{stdio,stdlib,string}.h`
  (three 4-line files that include `libz.h`); miniz has its own `shim/`; `regex.c` (CUtils') maps `cu_alloc` to
  `malloc` by `#define`; the crypto library is built with `CRYPTO_DEF`, among it `-DBR_USE_UNIX_TIME=0` because
  newlib's headers would switch on BearSSL's `time()` and a 64-bit division (D21). Each port works around the missing
  library its own way: the thing principle 2 warns about.
- **newlib on this PC:** the cross toolchain carries a newlib 4.5.0 build, but only for the default 68020 with a 68881
  (its `strtod` holds 125 FPU instructions, *measured*): unusable on a 68EC020. Zenix builds its C library itself.
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
    terminal, `poll`): what section 5 builds and section 9 maps to. The full standard library (stdio, `strftime`,
    floating point, wide characters) is for the ported Unix programs (the IRC and e-mail clients, the music player)
    and later ones (roadmap phase 3: "porting small Unix and Minix programs").

## 3. Which C library

### 3.1 Measured sizes

Test programs: `t1` `printf("hello %d %s\n")`; `t9` the same through `snprintf` and `write` (no `FILE`: the way libz's
`printf` works); `t2` `fopen`, `fgets`, `fputs`, `fprintf(stderr)`; `t3` a small sort (`fgets`, `realloc`, `strdup`,
`qsort`, `strtol`, `getenv`, `printf`); `t4` `time`, `localtime`, `strftime`; `t5` `strtod`, `printf("%.3f %g")`,
`sqrt`; `t6` `setjmp`/`longjmp`, `puts`; `t7` a mail-client-like mix (two files, `snprintf`, ctype, `strerror`,
`gmtime`, `strftime`, `malloc`); `t10` a raw `read`/`write` copy. Bytes of the ZEXE file (*measured*, 2026-10-28):

| test | libz `-Os` | libz `-O2` (as built today) | newlib 4.5.0 nano | picolibc 1.8.10 (`long long` `printf`) |
|---|---|---|---|---|
| t1 hello, `printf` | 2,192 | 4,384 | 6,052 | 4,532 |
| t9 hello, no `FILE` | (= t1) | (= t1) | - | **2,312** |
| t10 raw copy | - | - | - | 308 |
| t2 stdio files | (no `FILE`) | | 8,480 | 5,788 |
| t3 sort tool | 4,896 (`rd_getline`) | 7,908 | 9,472 | 6,876 |
| t4 time | (none) | | 23,040 | 17,100 |
| t5 floating point | (none) | | 33,844 (`-u _printf_float`) | 18,232 (float `printf`) |
| t6 setjmp | (none) | | 4,252 | 2,944 |
| t7 mail-like mix | (none) | | 28,348 | 22,176 |

What the rows say: **at the same setting picolibc's formatter costs about what libz's does** (t9 against libz's
hello, +120 bytes); **C's `FILE` streams cost about 2.2 KB** (t1 against t9: the buffer code, `stdout`, the flush at
exit); against newlib-nano picolibc is 25-45% smaller in every row. (Revision 1 compared libz at `-O2` with picolibc
at `-Os`, which made picolibc look smaller than libz; the second review found it.)

Per process (*measured*, data + BSS): libz's hello 4,108 bytes (its 4 KB output buffer), newlib-nano's 416, picolibc's
606. The library's stack frames are small: picolibc's deepest, `vfscanf`, 160 bytes; `vfprintf` 116; `strftime` 108
(*measured*, `-fstack-usage`). Every row passes `tools/check-68020-60.sh`.

Where the bytes go (*measured*, `nm`): `t4`'s 12.5 KB over hello are `strftime` (3.1 KB), the time-zone code (1.7 KB;
it reads `TZ` with `sscanf`, 1.1 KB), `localtime_r`/`gmtime_r` (1.1 KB) and libgcc's 64-bit division (2.2 KB:
picolibc's `time_t` is 64 bits, good past 2038). `t5`'s 14 KB are the float `printf`, `strtod` and its engine, the
conversion tables, `sqrt` and libgcc's soft-float routines; a program without floating point pays none of it.

**`printf` variants** (picolibc's, chosen at link time by `--defsym=vfprintf=__X_vfprintf`): t1's text is 3,858 bytes
with the integer one, 4,134 with `long long`, 11,988 float-only, 13,312 double (picolibc's own default). **Zenix's
Makefile rule links the `long long` one for every program** (`ctop`'s counters; 276 bytes over integer); a program
that prints floating point names itself (`LIBC_FLOAT_name`), and `libctest` checks what a `%f` gives without it (it must be
visible, never nothing: checked at step 1).

**`-O2` costs** (*measured*): 13 hot objects (the string functions, the stdio buffer code, `fgets`, `fputs`, `fwrite`,
`getc`, `putc`) rebuilt at `-O2` add 260-310 bytes per program; the integer formatter at `-O2` would add about 1.9 KB
(the second review: `vfiprintf.c` 1,403 bytes of text at `-Os`, 3,305 at `-O2`), so **the formatter stays `-Os`**:
it is cold next to the system call it feeds.

### 3.2 Licence (GPL-2.0-only)

- **picolibc:** every file used to build the library is under BSD-like licences (2- and 3-clause BSD and older
  permissive ones), listed file by file in its machine-readable `COPYING.picolibc`; its README says no GPL-related
  newlib parts are left. The only GPL/AGPL files in its tree are two `printf` test files and a cross-file helper
  script, none of them built or vendored. GPL-2.0-compatible; the notice ships as `/usr/share/licenses/picolibc` in
  every full image and the base package, as lwIP's does (D19).
- **newlib:** 57 licence sections in `COPYING.NEWLIB`, permissive for the m68k-elf build (the LGPL parts are for Linux
  targets only); compatible too, but the bookkeeping is newlib's own, not file by file.
- The port's files (`libc/zenix/`, section 12) are new, `GPL-2.0-only`.

### 3.3 The decision: picolibc 1.8.10 (the main session's, section 14)

- **Smaller than newlib-nano in every row** (25-45%), and as small as libz where both do the same job (3.1).
- **The code base the roadmap meant:** picolibc is newlib's code with AVR-libc's small stdio (`tinystdio`) in place of
  newlib's; the roadmap's "newlib" was named for its completeness, and picolibc keeps it.
- **No reentrancy structure:** newlib keeps per-thread state in `struct _reent` through `_impure_ptr`; picolibc uses
  plain globals (`errno` is an `int`), which suits a single-threaded Zenix process and `-msep-data` (each global one
  table slot).
- **Built for small machines and m68k:** hand-written m68k `memcpy`/`memset` (16 bytes per loop), m68k `setjmp`, a "C"
  and an optional UTF-8 locale, build options for everything Zenix does not want.
- **Pure-ready** (section 7): all of its objects compile through `tools/purecc.sh` (*measured*: 982, none failed).
- **Rejected:**
  - *newlib itself:* bigger in every row, `_reent`, and nano's float `printf` prints nothing unless a special link flag
    (`-u _printf_float`) pulls it in.
  - *libz grown into a full library:* `strftime`, time zones, `strtod` and float formatting, `sscanf`, wide characters
    and locales written and tested by us, to reach what picolibc is at the same size.
  - *musl, uClibc-ng, dietlibc:* built around Linux's system calls and process model (`fork`, `mmap`, threads);
    bigger; uClibc-ng LGPL. *klibc, PDCLib:* incomplete. *Fuzix's libc:* written for 8/16-bit Fuzix, not for porting
    today's Unix programs. *libnix* (the owner's AmigaOS programs use it): built on AmigaOS.

## 4. How it is built

- **Vendored, only the files used** (as BearSSL in `crypto/`): `libc/picolibc/` holds picolibc 1.8.10's sources that
  the build compiles, its `COPYING.picolibc` and a README naming the version and how to update; a generated
  `picolibc.h` (the configuration header meson would write) is checked in. **The Makefile compiles the list
  directly** (`LIBC_SRCS`, as `CRYPTO_SRCS`): no meson in Zenix's build.
- **The configuration** (meson's names, for the record): `tinystdio`, single thread, no thread-local storage,
  `newlib-nano-malloc` off (Zenix's allocator, 5.1), `picocrt` off (Zenix's crt0), `semihost` off, `posix-console`
  off (the port's own streams, 5.2), `io-long-long` on, `mb-capable` on with only UTF-8 (`mb-extended-charsets` off:
  *measured*, a program using `mbrtowc`/`wcwidth` grows from 5,152 to 9,664 bytes, every other program by nothing),
  `fortify-source` none, `assert-verbose` on. picolibc's `regex` and `malloc` sources are not vendored (one engine and
  one allocator, section 5).
- **Two builds,** as libz: `build/libc.a` (ZEXE programs) and `build/libc-pure.a` (every object through
  `tools/purecc.sh`, for ZEX2 programs). `-msoft-float` everywhere: one build runs on every 68020-68060; an FPU build
  of float-heavy programs for the 060 is a later choice.
- **The golden path:** `-Os` for the library, the formatter included (3.1); `-O2` for the string functions (and
  picolibc's m68k assembler `memcpy`/`memset`), the stdio buffer path (`getc`/`putc`/`fgets`/`fputs`/`fwrite`) and the
  allocator: about 300 bytes per program (*measured*), the console and file paths programs spend their time in.
- `tools/check-68020-60.sh` runs over the library's objects as over programs.

## 5. What Zenix's system calls back (the port, `libc/zenix/`)

picolibc calls POSIX names (`read`, `open`, ...); the port gives them over `usys.h`'s system calls. **Every call needed
is there today.** The port's parts (numbers checked against `usys.h` by the first review):

| Area | C library names | Zenix's call |
|---|---|---|
| Files | `open` (Zenix's `O_*`), `close`, `read`, `write`, `lseek`, `dup`, `dup2`, `pipe`, `pipe2`, `fcntl` | 8, 9, 10, 2, 11, 49, 50, 17, libz's `fcntl` |
| Names | `stat`, `lstat`, `fstat` (`struct stat` filled from `ustat`), `unlink`, `rename`, `mkdir`, `rmdir`, `link`, `symlink`, `readlink`, `chmod`, `chown`, `utime(s)`, `chdir`, `getcwd`, `umask`, `access` (from `stat` and the ids) | 13, 57, 14, 26, 28, 25, 27, 53, 54, 55, 52, 66, 56, 15, 16, 65 |
| Directories | `opendir`, `readdir`, `closedir`, `rewinddir`, `scandir`; `glob` (the port's; picolibc has none) over them and picolibc's `fnmatch` | `open` + 12 (`udirent`, with its size) |
| Memory | `malloc`, `free`, `calloc`, `realloc`, `posix_memalign`, `malloc_usable_size`; `sbrk` answers `ENOMEM` | 18, 19 (5.1) |
| Time | `time`, `gettimeofday`, `clock_gettime` (`CLOCK_REALTIME` from 58; `CLOCK_MONOTONIC` from the 100 Hz tick, 20 op 0, which wraps after 497 days, never from the 32-bit E-clock count of 36, which wraps every 101 minutes), `nanosleep`/`usleep`/`sleep` (10 ms ticks), `alarm`, `clock` (ticks since the process began: wall time, there is no CPU accounting) | 58, 20, 7, 87 |
| Processes | `getpid`, `getppid` (`procinfo`), `getuid`, `getgid`, `geteuid`, `setuid`, `setgid` (`getegid` answers the gid: there is no effective gid), `posix_spawn`/`posix_spawnp`, `system`, `popen`/`pclose`, `waitpid`, `wait`, `_exit`, `kill`, `raise` | 4, 21, 61-64, 67, 5, 6, 1, 24 |
| Signals, waiting | `sigaction`, `signal`, `sigprocmask`, `pause`, `poll`, `select`, process groups | libz's D16 code (shared, section 6) |
| Terminal | `isatty`, `tcgetattr`/`tcsetattr` (5.4), `ioctl(TIOCGWINSZ)`, `tcsetpgrp`, `getpass` (over `pw.c`'s `read_secret`) | 60, 45, 46, 23 |
| Environment | `getenv`, `setenv`, `unsetenv`, `environ` (at most 63 variables and 1 KB, `ENVMAX`) | 47, 48 |
| Network | sockets, `inet_*`, `getaddrinfo`/`freeaddrinfo` (libz's, D19) | 90 |
| Regular expressions | `regcomp`/`regexec`/`regfree`: a thin POSIX wrapper over `user/regex.c`, so Zenix keeps one engine | none |
| Users | `getpwnam`, `getpwuid`, `getgrnam`, `getgrgid` over `/etc/passwd` and `/etc/group` (frozen formats) | `user/pw.c`'s parser, one copy |

**Limits the port states** (limits.md, and `libctest` checks each): `fcntl`'s `O_NONBLOCK` works on sockets only
(`pipe2(fds, O_NONBLOCK)` for pipes; files and terminals answer `EINVAL`, as libz's does), `F_GETFL` reports only the
access mode on other files; `mkdir`'s mode is applied by a `chmod` after `SYS_mkdir` (which takes none), so it is
not atomic; `utimes` sets one time (no separate access time); 16 descriptors per process; paths at most 63 bytes.

**The constants are Zenix's, from one source.** picolibc's headers carry newlib's numbers, and some differ from
Zenix's frozen ones: `ENOTSOCK` is 108 there (Zenix 88, Linux's), `SIGUSR1` 30 (Zenix 10), and in `fcntl.h`
**`O_EXCL` is 0x800, which is Zenix's `O_NONBLOCK`** (Zenix's `O_EXCL` is 0x80; picolibc's `O_NONBLOCK` 0x4000): a
silent wrong meaning if picolibc's header were used (`O_CREAT` 0x40, `O_TRUNC` and `O_APPEND` agree). The port puts its
own `errno.h` (`sys/errno.h`), `sys/_default_fcntl.h`, `signal.h`, `limits.h` additions (`PATH_MAX` 64) and the
`sys/stat.h` bits ahead of picolibc's in the include path, each generated from or including `kernel/errno.h` and
`kernel/kernel.h`, never typed twice; the port's `errno.h` keeps picolibc's `extern int errno` declaration. A host test
checks every constant against the kernel's headers.

**Three error numbers are missing** (the first review): picolibc's sources use `EOVERFLOW` (27 places), `EILSEQ` (11:
the UTF-8 code) and `ENOTSUP` (5), and C's `math.h` needs `EDOM`; `kernel/errno.h` has none of them. They are added
there with Linux's values, **`EDOM` 33, `EOVERFLOW` 75, `EILSEQ` 84**, and `ENOTSUP` defined as `EOPNOTSUPP` (95, as
Linux does), with their texts in the one error table; claimed in CLAUDE.md's table first (append-only; the kernel's
code does not change).

### 5.1 Memory: libz's allocator, shared

`malloc` is libz's, one source for both (`user/zmalloc.c`; its policy since 2026-10-30, measured, is "The allocator"
at the end of this note): a pointer `malloc` did not give out is reported, never followed ("safety stays").
picolibc's own `malloc` needs `sbrk`, one growing heap, which a no-MMU Zenix cannot give. It gains what the standard
asks and libz lacks: `posix_memalign`/`aligned_alloc`, `malloc_usable_size`, `reallocarray`.

### 5.2 The streams

- `stdin`, `stdout`, `stderr` (for programs that use `FILE`): `stdout` line-buffered on a terminal and fully buffered
  otherwise, `stderr` unbuffered, `stdin` buffered with `stdout` flushed before it reads, as C says. **Buffers sized to
  the allocator** (the second review): `stdout` 2,048 bytes and `stdin` 1,024, taken by `malloc` on first use; with
  their 8-byte headers both fit the first 4 KB chunk (2,056 + 1,032 bytes), so stdio takes no region of its own. (A
  4,096-byte buffer would be a 4,104-byte block that does not fit the first chunk and would pull an 8 KB one.)
- **Nothing of stdio reaches a program that does not use it** (the second review): `exit` reaches the stream flush only
  through a pointer the stream code sets on first use, so `_zstart`/`exit` name no stream. Checked in the size table:
  `true` and a raw `cat` stay under 600 bytes (t10 is 308 with the stand-in).
- **Zenix's own output, kept:** `oputs`/`oputn`/`oputc`/`oprintf`/`oflush` (Zenix helpers, section 6) keep libz's
  behaviour: a 4 KB buffer for descriptor 1, written when full or flushed (and at exit), the same on a terminal, so
  `ls`, `grep`, `ps` make one system call per screen, not one per line. A program uses either these or `stdout`'s
  `FILE` functions, never both (two buffers would mix their order); the size tool warns when one program links both.
- A write that fails with `EPIPE` ends the program with status 141, in both (as libz's `oflush` does today).

### 5.3 The start and the end

- crt0 is unchanged: one `bsr.l _zstart`. The port's `_zstart` sets `environ` (from `SYS_envget`, as libz's
  `env_load`), `_zargv`, runs the `init_array` (empty for C programs; C++ is not a goal), calls `main`, then `exit`:
  `atexit` functions, the `fini_array`, the stream flush if streams were used (5.2), the Zenix output buffer,
  `SYS_exit`. **The start contract (the stack at entry, `a5`) does not change.**
- The link scripts gain `PROVIDE`d `__init_array_start/end` and `__fini_array_start/end` and `KEEP` of the two
  sections (picolibc's `exit` refers to them; without them a pure link fails on undefined weak symbols, *measured*).
  `PROVIDE` defines a symbol only when something refers to it, and no object of today's build has an `.init_array`,
  `.fini_array`, `.ctors` or `.dtors` section, so **every program built today stays byte-identical**: *measured*
  2026-10-28, the whole tree at 722d6f6 built twice with `make BUILD=/tmp/...`, once with today's scripts and once
  with the addition below at the start of `.data` in both `link/user.ld` and `link/pure.ld`; **all 146 outputs
  compared, 0 differ** (every `.zx` and `.pzx`, `kernel.bin`, `zenixboot` and `bootmenu`, which `link/user.ld` also
  links, and `zenix.adf`). The addition, as tested:
  `PROVIDE(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE(__init_array_end = .);`
  and the same for `fini`. (`KEEP` takes one input description: `KEEP(*(A) *(B))` is a link-script syntax error.)

### 5.4 The terminal

The kernel's line discipline (`kernel/tty.c`, *read*) has two modes: cooked (line editing, echo, and ^C always a
signal) and `TTY_RAW` (bytes as they come; ^C a signal unless `TTY_NOSIG`, which only counts in raw mode). termios is
mapped onto that so nothing a program asks for is silently ignored (the first review: revision 1 would have echoed a
password typed with `ECHO` off):
- `ICANON`, `ECHO`, `ISIG` all on: cooked mode, the kernel's.
- **Any of them off:** `TTY_RAW`, plus `TTY_NOSIG` when `ISIG` is off; if `ICANON` is still on, the port edits the
  line itself in `read` (erase, kill, ^D at the start of a line, the newline ends it) and echoes only when `ECHO` is
  on. So a password prompt (`ICANON` on, `ECHO` off) reads a line with no echo; `getpass` uses `pw.c`'s
  `read_secret`, which already reads raw.
- `VMIN` 1 / `VTIME` 0 only; other values answer `EINVAL` (a timed read is `poll` with a timeout, as every Zenix
  program does it). Other bits (`IXON`, `OPOST`, ...) are kept and answered back but change nothing, listed in
  limits.md. `cfmakeraw` sets raw without signals.
- `ioctl(fd, TIOCGWINSZ)` answers `ENOTTY` when the descriptor is not a terminal, else the console's size from
  `SYS_ttyinfo` (80 x 32 PAL, 80 x 25 NTSC). `SYS_ttyinfo` takes no descriptor; when D20's pseudo-terminals are built
  (they are not yet), their own sizes need an argument to it (0 for every older caller: a frozen-interface addition
  made then). No `SIGWINCH` (the text console never changes size).
- Keys arrive as the standard escape codes the console sends (architecture.md section 11; the keymap, Swedish on the
  owner's A1200, is the kernel's); the console understands ANSI/ECMA-48 output and D14's colours. `TERM` names it.

### 5.5 What is not there, and says so

`fork`, `vfork`, the `exec` family, `mmap`, `sbrk`, threads (`pthread_create`), `getrusage`, `ftruncate`/`truncate`
(no system call; `O_TRUNC` is), `fsync` (it is `SYS_sync`, the whole cache, documented), `dlopen`. Each is either
absent (a link error, the honest answer when porting) or, where programs commonly test for it, present and failing
with `ENOSYS`; the list goes in limits.md. `posix_spawn` (over `SYS_spawn`, which gives the child the parent's
environment and descriptors 0-2 only):
- file actions are done in the parent around the spawn and can only set the child's descriptors 0-2; others answer
  `EINVAL`;
- an `envp` other than `environ` is set with `SYS_envset` around the spawn and restored;
- `POSIX_SPAWN_SETPGROUP` is done by the parent right after the spawn (`setpgid`); the other attributes answer `EINVAL`.

Whether Zenix ever gets an `exec` that replaces a process in place is a kernel design of its own, opened only if a
port needs it.

## 6. libz and the C library: one library in the end

A program links one of them, never both: their prototypes collide (`fprintf(int fd, ...)` against
`fprintf(FILE *, ...)`, `pw.h`'s inline `fputs(s, fd)`, `strlen` returning `u32`). The way to one library, in steps:

1. **Now (block 3):** every existing program keeps libz; every ported or new block-3 program uses the C library. The
   Zenix helpers a ported program wants (`path_join`, `dir_list`, `color_auto`, `zwhy`, `zsig_name`, the `oputs`
   family) come as **libzenix** (`zenix.h`). The code libz and libzenix share - the allocator, signals, `poll`/`select`,
   sockets, the error texts, the passwd parser, the helpers - lives in **one source file each**, compiled into both, so
   nothing is written twice. One error variable: under the C library `usys.h` has `#define zerrno errno` and the shared
   files define `zerrno` only in the libz build (else it would be a second definition of `errno`).
2. **The bridge (mechanical, one commit):** libz's non-standard names get standard ones with the same meaning:
   `fprintf(int fd, ...)` and `vfprintf(int fd, ...)` become POSIX's `dprintf` and `vdprintf`; libz's `printf` (which
   writes straight to descriptor 1, unbuffered) becomes `dprintf(1, ...)` in the programs, not C's buffered `printf`
   (that would pull in the streams, 2.2 KB, into 71 of them); `pw.h`'s inline `fputs(s, fd)` gets a private name;
   sizes become `size_t`. Every program's behaviour is unchanged (the existing tests' outputs compared); after it, a
   Zenix program's source compiles against either library.
3. **The switch, measured:** every program built both ways, a table (`tools/libc-size.sh`): its file size today (libz
   `-O2`), with libz at `-Os` (the fair comparison, 3.1) and with the C library. The base system moves to the C library
   + libzenix, and libz's own string, format and memory code is deleted, when **no program is bigger than today's file
   by more than the larger of 256 bytes or 2%, the sum of all program files is not bigger than today's, `kernel.bin`
   (with the built-in servers diskd, fsd and boot) is not bigger, and the hot programs (`sh`, `ls`, `cat`, `grep`,
   `less`) lose no speed** (static instruction counts, then the stock A1200's `bench` and text-tool runs, D13 section
   3's rule). Expected (*estimate* from 3.1): about 120 bytes more than libz `-Os` per program, well under today's
   `-O2` files; the built-in servers use no streams. Then libz is libzenix, and principle 2 holds: one C library.
   If the table says otherwise, the main session decides with it; a cost that pushes the stock A1200 (the RAM floppy:
   4,988 bytes left on the mount tests' floppy, *measured* 2026-10-28) or a floppy goes to the owner with the numbers.

**"base" in the roadmap's sense** (`Versions and updates`: the kernel, the built-in servers, init and the C library):
the C library is in base because it bakes the system-call, error and message numbers into every program; since those
are frozen and append-only, a program built with an older C library keeps working on a newer kernel, so no lockstep
beyond that is needed. It is linked into each program, so (as libz) it has no frozen-interface rule of its own
(CLAUDE.md). A fix to it reaches users as new builds of the packages that contain the fixed code: builds are
reproducible (D21 8.3's build date), so only packages whose bytes change are republished, but a fix to a function in
nearly every program (the formatter) costs what D22 7.3 estimates for a libz change: **about 6-9 minutes of
`zpkg -Syu` on the stock A1200** for all 40 packages.

## 7. Shared code (D13 stage 1) with the C library (measured)

The pure build (`libc-pure.a`, every object through `purecc.sh`), Zenix's `crt0 -DZEX2` and `link/pure.ld` (integer
`printf`, the stand-in glue):

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
  own `__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3` (new code, C over the 32-bit `divu.l`, which every CPU up to the
  060 has; only the 64-bit forms are missing on the 060; no table), linked before libgcc in both builds. It also
  serves the pure `long long` of `ctop`. libgcc's `__muldi3` is checked the same way when a pure program first links it.
- **Floating point stays out of pure programs** (D13 section 8: libgcc's soft-float writes `_fpCCR` by address): a
  program that uses floating point is a ZEXE program, as `awk` is, until a pure soft-float exists.
- A pure program's data part grows by the library's globals: about 200-350 bytes measured.

## 8. D13 stage 2, shared libraries: decided - not in block 3, and when

**The numbers** (*estimates*, from 3.1). The C library inside a program is 2.3 KB (no streams) to 22 KB (t7); a
typical ported block-3 program (a text-UI program with stdio and time: t4-t7) carries about 6-17 KB of it. In a busy
session on the stock A1200 - shells, Sumi, Maki, an IRC client, the e-mail client - four distinct programs carry it,
so one shared copy would save about **16-70 KB** of the about 1,130 KB free with one login: 1.5-6%. Two of the same
program already share everything (D13 stage 1, section 7).

**The costs.**
- **A frozen interface the size of the library.** Linked into each program, the C library freezes nothing. A shared
  one makes every exported function, structure layout (`FILE`, `struct stat`, `struct tm`) and global a frozen
  interface for ever, because an older program meets a newer library after every update (D15's rule). This is true of
  any shared library, an AmigaOS-style jump table included: only a library whose interface has settled can be shared.
- **Per-process state everywhere:** `errno`, the streams, the allocator's lists, `environ`, `atexit`, the time zone. A
  shared C library needs a second data base per opener on every call (AmigaOS passes it in `a6`; gcc's
  `-mid-shared-library` reloads `a5` in every function: D13 section 9 *measured* `sh` at 23,618 instructions against
  22,310, 5.9% more, and 792 indirect calls).
- **Kernel and format work:** a library segment kind, opener data blocks, a resolver: an *estimated* 2-4 KB of kernel
  and a new file kind next to ZEXE/ZEX2.

**The decision.**
- **The C library stays static** (and shared per program by stage 1): never a shared library, for the frozen interface
  and the per-process state.
- **The text-UI library (block 3's next item) is static too** (D13 section 9 named it a candidate "when such a library
  exists"; this is that decision): it is new, its interface will change through block 3 (Sumi, Maki, Markdown, `zc`
  and Ahiru each shape it), and every program links only the parts it calls (*estimate*: 5-12 KB per program; its
  AmigaOS original is about 1,800 lines). Freezing it now would freeze its first draft.
- **Reopened** (a design note of its own, two reviews) when either holds:
  - **at block 3's closing audit the main session measures it:** in FS-UAE (memory, not time) on the stock A1200's
    setup, with the shells, Sumi, Maki, Markdown, the IRC and the e-mail clients running, the size table's library
    parts summed over the distinct programs; if the duplicated library code (the C library, the text UI, the crypto
    library) reaches **100 KB**, stage 2 comes next; or
  - the desktop (block 5) has its client library, which every graphical program will carry.
- **Its shape then** (D13 section 9 leans this way): an AmigaOS-style library for libraries written for Zenix with a
  settled interface, mostly stateless (the display client, the text UI once settled, perhaps the crypto library): a
  jump table, opened by name, a data base per opener in a register, a **new file magic** of its own; ZEXE and ZEX2 stay
  exactly as they are, and a program that opens no library pays nothing.

## 9. Porting rules for block 3's programs

**General.**
1. **One source for both systems where it is the owner's own program** (owner question 2): the program keeps its
   repository; its AmigaOS calls are gathered behind the program's own small OS layer (CUtils has one, `cu.h`; Sumi,
   Maki and Markdown share tui.library), and that layer gets a Zenix implementation in C over the C library. Zenix's
   tree carries a pinned copy (as lwIP and BearSSL) under `ports/NAME/`, updated by a script, its licence header kept.
2. **A program from elsewhere** (an IRC client, a MOD player) is chosen first for its licence (GPL-2-compatible), its
   size and its needs: POSIX, `poll`, no threads, no `fork`, no `mmap`; a music player that mixes in fixed point, not
   floating point (section 7). Candidates are judged at their own step.
3. **No threads, no `fork`:** a second task becomes a second process (`posix_spawn` and a pipe), or work done in
   slices in the program's `poll` loop (Ahiru's copy job).
4. **Pure when several may run at once** (an editor, a pager): ZEX2, so a second Sumi costs only its data. ZEXE for a
   program that uses floating point (section 7) or sends raw messages (D13 section 5: such programs stay ZEXE until
   servers check pointers).
5. **Stacks:** the program's measured need in its Makefile line (`STACK_name`, `-fstack-usage`), never AmigaOS's
   `StackSwap`. Paths at most 63 bytes (`PATH_MAX` 64); 16 descriptors per process.
6. **Sizes on the stock A1200 said in each port's commit** (file, shared/data parts, memory with it running).

**AmigaOS to Zenix** (what replaces what):

| AmigaOS | Zenix |
|---|---|
| `AllocVec`/`FreeVec`, `AllocMem` (any memory) | `malloc`/`free`. `MEMF_CHIP` has no place in a program: what needs chip RAM (sound, pictures) goes through a server |
| `Open`/`Read`/`Write`/`Seek`/`Close`, `FGets`, `FPuts`, `PutStr` | `open`/`read`/`write`/`lseek`/`close`, `oputs` or stdio |
| `Lock`/`Examine`/`ExNext`/`ExAll` | `stat`/`lstat`, `opendir`/`readdir` |
| `MatchFirst`, `ParsePattern` (`#?`, `~(...)`, `(a\|b)`) | `fnmatch`/`glob` with Unix wildcards (`*`, `?`, `[...]`): the program's pattern text is translated, and AmigaOS-only forms (negation, alternatives) are dropped or done by the program |
| `Rename`, `DeleteFile`, `CreateDir`, `SetProtection` | `rename`, `unlink`/`rmdir`, `mkdir`, `chmod` (Unix modes `rwxrwxrwx`, not `hsparwed`) |
| `SetComment`, `.info` icons, assigns, volumes, the DosList | none: Unix paths and the mount table (`/`, `/mnt/...`) |
| `GetVar`/`SetVar` | `getenv`/`setenv` (the environment: 63 variables, 1 KB) |
| `SystemTags`, `Execute`, `CreateNewProc` | `posix_spawn`, `system`, `popen` |
| `Wait`, `SetSignal`, Ctrl-C as `SIGBREAKF_CTRL_C` | `poll` (descriptors and ports), signals (`SIGINT` ...: D16) |
| `DateStamp`, `DateToStr`, `StrToDate`, timer.device | `time`, `localtime`/`strftime`/`strptime`, `clock_gettime`, `poll`'s timeout |
| `SetMode(fh, 1)`, `WaitForChar`, `ACTION_WAIT_CHAR` | `tcsetattr` raw (5.4), `poll` on descriptor 0 |
| `CSI 0 q` (the window's size) | `ioctl(TIOCGWINSZ)` |
| `CSI` as 0x9B in input | the console sends `ESC [`; the program's (or the text-UI library's) input parser accepts it, as tui.library's already does |
| `IoErr`, `Fault` | `errno`, `strerror` |
| utf8.library | `mbrtowc`, `wcwidth` in `C.UTF-8` (section 4); the console's UTF-8 is a block-3 item of its own |
| clipboard.device, commodities, ARexx, intuition/graphics | none in the text console (the desktop, block 5, or later); the program works without them |
| tui.library | Zenix's text-UI library (the next roadmap item), over 5.4, `poll` and ANSI output |
| exec's task lists (`ps`, `top`, `ctop`) | `SYS_procinfo`, `SYS_meminfo` (`ps` and `free` show the way) |
| Paula and `audio.device` (a MOD player) | not the C library's: the player needs a sound interface the kernel does not have yet (only `beep`; architecture.md section 3, "Sound"): the main session decides whether block 6's Paula server comes forward |

## 10. Size and speed budgets (checked by `tools/libc-size.sh` in `make check`)

Set from 3.1's *measured* figures plus the planned `-O2` path (about 300 bytes) and a margin for the real port (step 1
measures it and the budgets are confirmed then):
- **Per program, file size:** a raw program (`read`/`write` only) at most 600 bytes; hello without streams (`dprintf`,
  `oputs`) at most 2.8 KB; hello with `printf` at most 5.2 KB; a stdio + `malloc` + `qsort` tool at most 7.6 KB; the
  time functions at most 13 KB more; floating point at most 15 KB more; UTF-8 functions at most 5 KB more.
- **As checked** (`tools/libc-size.sh`, the measured port with room): hello with `printf` at most 7.6 KB and the stdio +
  `malloc` + `qsort` tool 11.4 KB since the allocator of 2026-10-30 ("The allocator": about 0.7 KB more of its code).
- **Per process (memory):** the library's data + BSS at most 1 KB; stdio's buffers inside the first 4 KB `malloc`
  chunk (no region of their own, 5.2); `libctest` reports its regions and bytes (`SYS_procinfo`'s `mem`) and checks them.
- **The switch** (section 6 step 3): the per-program, total and `kernel.bin` rules there.
- **Speed** (said, not yet timed): the string functions are as fast or faster (picolibc's `memcpy` moves 16 bytes per
  loop, libz's 4, *read*); Zenix's own programs keep one write per 4 KB on the console (5.2). The stock A1200 times
  `cat`, `grep`, `sort` and `ls` before and after the switch (D13 section 3's rule); a loss over 5% on a hot program
  blocks it.

## 11. Tests

- **Host** (`tools/libc-host-test.sh`): every constant the port's headers give (`E*`, `O_*`, `S_*`, `SIG*`, `PATH_MAX`)
  equals the kernel's; `libc.a` and `libc-pure.a` hold no FPU instruction and pass `check-68020-60`; the port's
  64-bit division against the host's for a table of values and random ones; the size table against the budgets,
  including the warning for a program that links both `oputs` and `stdout`'s functions.
- **FS-UAE** (`tools/libc-test.sh`, in `make check`): `libctest`, a test program in `/tests` (not a command), compares
  its output with expected text: `printf`/`snprintf`/`dprintf` formats (widths, flags, `%lld`, `%n` not supported, `%f`
  without a float build visibly marked), `sscanf`, `strtol` edges, `qsort`, `fopen` modes `r w a r+ w+` (create,
  truncate, append checked with `stat`), `O_EXCL` and `O_NONBLOCK` kept apart, `fseek`/`ftell`, `ungetc`, line
  buffering on a console and full buffering into a pipe, `stdout` flushed before `stdin` reads, `EPIPE` ends with 141,
  `malloc` stress (thousands of blocks, every class, `realloc` growth, a bad `free` reported) and the stream buffers
  inside the first chunk, `setjmp`, `time`/`gmtime`/`strftime` of fixed times, `TZ=CET-1CEST,M3.5.0,M10.5.0/3`,
  `CLOCK_MONOTONIC` never going back, `mbrtowc`/`wcwidth` of Swedish and wide characters, `EILSEQ` on bad UTF-8,
  `strerror` texts equal libz's, `opendir` over a directory of 200 names, `posix_spawn` (file actions, `envp`, a
  process group) and `popen` with a pipe, termios: raw and back, `ICANON` on with `ECHO` off reads a line with nothing
  echoed (the console's grid checked), `ISIG` off makes ^C a byte; `TIOCGWINSZ` on a console and `ENOTTY` on a pipe; a
  float build checks `%f`/`%g` and `strtod`. Its ZEX2 build runs on consoles 2 and 3 at once (one segment, `free`
  shows it). The bridge (section 6 step 2) is checked by the existing command tests' outputs.
- **On the A1200:** with the switch, the timed runs of section 10.

## 12. For the main session (claims; not owner questions)

- **Error numbers** (append-only, claimed in CLAUDE.md's table first): `EDOM` 33, `EOVERFLOW` 75, `EILSEQ` 84 in
  `kernel/errno.h`, `ENOTSUP` = `EOPNOTSUPP` (95), their texts in the shared error table. No system call, message type
  or kernel code change; if the port finds it needs a call (none known), it is claimed first.
- **New directories:** `libc/picolibc/` (vendored subset, its licence file) and `libc/zenix/` (the port: system layer,
  headers, the streams, `_zstart`/`exit`, `strerror`, the 64-bit division, termios, `TIOCGWINSZ`, `glob`, the `regcomp`
  wrapper); `ports/` for ported programs (9). The shared files of section 6 step 1 move out of `libz.c` into `user/`
  files both builds compile: a libz change, the main session's.
- **Makefile:** `LIBC_SRCS`, `LIBC_O2`, `build/libc.a`, `build/libc-pure.a`, the `long long` `printf` for every C
  library program, `LIBC_FLOAT_name`, a link rule for C library programs (`$(BUILD)/libc.a` before `$(LIBGCC)`),
  `tools/libc-size.sh`, `tools/libc-host-test.sh` and `tools/libc-test.sh` in `make check`.
- **Link scripts:** `link/user.ld` and `link/pure.ld` gain the init/fini lines of 5.3; every existing output stays
  byte-identical (*measured*, 5.3; checked again in the commit that makes the change).
- **`usys.h`:** `#define zerrno errno` under the C library.
- **libz:** the bridge renames of section 6 step 2 across the tree (97 files call `fprintf(fd, ...)`; `printf` in 71 of the
  programs), in one commit; libz's own string, format and memory code deleted at the switch.
- **Docs:** DESIGN.md D27, and its line 40 ("no newlib: Zenix's own small C library") at the switch; ROADMAP block 3's
  "(newlib)" to "(picolibc, newlib's descendant)"; architecture.md sections 4 ("There is no shared-library format yet":
  D27 section 8's decision) and 12; limits.md (5's limits, 5.4's termios bits, 5.5's list); source-tree.md (it says
  "own small library, not newlib"); vs-unix-linux.md; features.md; the licence file in `rootfs/` and the base package.
- **Review depth:** one review for the library (no kernel code); two for the switch (section 6 step 3: it touches every
  program and the built-in servers).

## 13. Build order (each step small, merged with its tests; the full suite before each merge)

0. **The library builds** (no program uses it): `libc/picolibc/` vendored with `picolibc.h`, both archives from the
   Makefile, `check-68020-60` over them, the licence file in the images. Host: the archives' size and FPU check.
1. **The port and its first test:** the three error numbers; `libc/zenix/` with the system layer, the headers from the
   kernel's constants (host test), the allocator and the other shared files moved out of `libz.c` (every libz
   program's output unchanged), the streams, `_zstart`/`exit`, `strerror`; `libctest` (ZEXE) in `/tests`;
   `tools/libc-test.sh`; the size table with the real port, the budgets of section 10 confirmed.
   **As built (steps 0 and 1, 2026-10-29):**
   - *Step 0.* picolibc 1.8.10 from its official release (checksum in `libc/picolibc/README`), 875 files: the 694
     sources of `libc/picolibc/sources.mk` (from the release's CMakeLists.txt lists for ctype, errno, locale, misc,
     posix, search, stdlib, string, time, tinystdio, uchar, machine/m68k and libm's common, math, fenv; left out: the
     allocator, regex, the environment, `system`, `strerror`, `clock`, `exit`, `fdopen`, arc4random, the Berkeley
     hash/ndbm, pathconf, fdevopen, the POSIX console), every file they include (the cross compiler's `-MM`,
     `tools/libc-vendor.sh`) and the public headers; none changed. `build/libc.a` (703 objects, 239,206 bytes of code)
     and `build/libc-pure.a` from the Makefile, both through `check-68020-60`; `COPYING.picolibc` as
     `/usr/share/licenses/picolibc` on the full images and in the base package.
   - *Step 1.* `EDOM` 33, `EOVERFLOW` 75, `EILSEQ` 84, `ENOTSUP` in `kernel/errno.h` (claimed), their texts in the one
     table. The port, `libc/zenix/`: the system layer (`sys.c`: files, names, ids, time), directories (`dirent.c`),
     the streams (`stdio.c`), `_zstart`/`exit` (`start.c`), `strerror` (`strerror.c`) and `dprintf`/`vdprintf`
     (`dprintf.c`: tinystdio has none). Headers ahead of picolibc's: `sys/errno.h` (includes `kernel/errno.h`),
     `sys/_default_fcntl.h`, `signal.h`, `sys/syslimits.h` (over picolibc's by `#include_next`), `sys/dirent.h`,
     `stdio.h` (adds `dprintf`), `zenix.h` (libzenix's `oputs` family); the numbers from `zenix-consts.h`, which
     `tools/libc-consts.sh` generates from the kernel's headers. The shared files out of `libz.c` (`user/zshare.h`):
     `zmalloc.c` (+ `memalign`, `aligned_alloc`, `posix_memalign`, `malloc_usable_size`, in the C library only),
     `zenv.c`, `zout.c`, `zerrtab.c`; `libz.uo` is `libz.c` and those joined by `ld -r`. Every libz program builds
     from them with its output unchanged (the whole suite); the sum of the program files 7.4 KB smaller (cross-file
     inlining lost and gained, largest growth 124 bytes, `host`). Tests: `tools/libc-host-test.sh` (106 constants
     against the kernel's, no FPU instruction, check-68020-60), `tools/libc-test.sh` (`/tests/libctest`, 73 checks
     and the shell-side ones), `tools/libc-size.sh`; all in `make check`.
   - *Found while building, and decided here:* the link scripts' init/fini lines (planned for step 2) came now,
     because `exit` needs them for `atexit` (picolibc keeps those in `.fini_array_onexit`, which the lines' last
     `KEEP` takes; `__bothinit_array_*` too, which picolibc's `__libc_init_array` reads): every existing output
     byte-identical (149 files compared). `fopen`'s files are kept in a list by the port's own `fdopen` so `exit`
     flushes them (picolibc's leaves that to `fclose`, against C). The environment is loaded at the start only when
     the program links it (a weak reference), or every program would carry its 1.3 KB of BSS. `O_CLOEXEC` is 0
     (every spawned program gets only descriptors 0-2). The signal calls, `poll`, `select`, sockets and the path
     helpers stay in `libz.c` until the C library first offers them (step 3), so step 1 moves only what it uses.
   - *The size table with the real port (`tools/libc-size.sh`, ZEXE bytes):* raw 536 (budget 600), hello with
     `dprintf` 2,732 (2,800), with `oputs` 888, with `printf` 6,312 (was 5,200), the sort tool 9,960 (was 7,600),
     time +13,632 over hello (was 13 KB), floating point +15,560 (was 15 KB), UTF-8 +5,500 (was 5 KB); the library's
     data + BSS with `printf` 226 bytes (1 KB). The programs without streams keep their budgets; those with a stream
     or `malloc` cost about 1.1-2.4 KB more than the stand-in measured: libz's allocator (`malloc`, `free`,
     `realloc`: about 1.4 KB at `-O2`, against the stand-in's `sbrk` one) and the lazily taken stream buffers
     (section 5.2's design). Section 10's budgets for those rows are raised to the measured sizes with about 5% room
     (6,600; 10,400; +14,300; +16,300; +5,800), for the main session to confirm. Zenix's own programs are not
     affected (they keep libz, then `dprintf`/`oputs`).
2. **Pure:** `libc-pure.a`, the port's 64-bit division, the link scripts' init/fini lines (every existing output
   byte-identical, as 5.3 measured), `libctest` as ZEX2 on two consoles.
   **As built (step 2, 2026-10-29):**
   - `libctest.pzx` (`/tests/libctestpure`): `user/libctest.c` through `tools/purecc.sh`, the pure crt0, `link/pure.ld`
     and `libc-pure.a` (Makefile: `.cpo`, `.cpuelf`, `.pzx` rules for `LIBCAPPS`). ZEX2, 78,368 bytes: 71,920 shared,
     3,360 data, 14,024 bytes of memory per process (with its 16 KB stack counted apart); the ZEXE build 81,836.
   - The 64-bit division: `libc/zenix/divdi3.c` (`__udivdi3`, `__umoddi3`, `__divdi3`, `__moddi3`; 570 bytes at
     `-Os`): one `divu.l` for 32-bit operands, four for a 16-bit divisor (16-bit digits), else shift and subtract
     (at most 64 steps); in both archives, so ahead of libgcc. It is 2 KB smaller than libgcc's: the time row of the
     size table went from +13,632 to +11,604 bytes. `tools/libc-host-test.sh` checks it against the PC's division
     (968 edge cases and 200,000 random ones, every divisor size, signed and unsigned).
   - libgcc's `__muldi3` (the time code's 64-bit multiply) is linked into `libctestpure` and `mkzexe` accepts it: no
     absolute reference into the shared part (section 7's check, done).
   - The init/fini lines: checked again at 98f88cc, the tree built with and without them (the same build date and
     test key): 151 outputs compared, only `libctest.zx` (which needs them) and `zenix.hdf` (which holds it) differ.
   - `tools/libc-test.sh`'s second run (FS-UAE with fast RAM): `libctestpure hold 40` on consoles 2 and 3 at once,
     every check passing in both; `free` shows one copy of its code and the second one saved, `ps` both running it,
     and the copy freed when both end. The test's temporary files are named by the process (two run at once).
3. **Terminal and processes:** termios (5.4), `TIOCGWINSZ`, `getpass`, `posix_spawn`, `system`, `popen`, `getpw*`,
   `glob`, `regcomp`; their `libctest` parts. *The C library is ready for the text-UI library and the first port (the
   next roadmap items).*
   **As built (step 3, 2026-10-29):**
   - New port files: `termios.c` (+ `include/sys/termios.h`, Linux's names and numbers), `spawn.c` (`posix_spawn`,
     `posix_spawnp`, the file actions and attributes, `waitpid`, `wait`, `system`, `popen`/`pclose`, `getppid`),
     `pwd.c` (`getpwnam`, `getpwuid`, `getgrnam`, `getgrgid`), `glob.c`, `regcomp.c` (over `user/regex.c`, which now
     also compiles in the C library); `sys.c` gained `pipe2` and the read hook of the line editing. Headers:
     `sys/socket.h`, `netinet/in.h`, `netinet/tcp.h`, `arpa/inet.h`, `netdb.h` (all `user/socket.h`, now one header for
     both libraries), `sys/ioctl.h` (`struct winsize`, `TIOCGWINSZ` 0x5413), `poll.h`, `sys/poll.h`, `regex.h`
     (picolibc's uses `size_t` without its header), `stdio.h` (+ `popen`, `pclose`); `signal.h`'s set operations
     became inline functions (as macros they warned when used as statements).
   - Moved out of `libz.c` into shared files (`user/zshare.h`), now in both libraries: `zsig.c` (the signals, the
     process groups, the signals' names), `zpoll.c` (`poll`, `select`), `zsock.c` (the sockets, `fcntl`, `ioctl`, the
     address texts, `getaddrinfo`), `zhelp.c` (`path_join`, `dir_list`, `path_find`, `zrealpath`, the colour rule and
     the BRE translation, `zs_bre_to_ere`, which libz's `bre_to_ere` wraps) and, out of `pw.c`, `zpw.c` (the passwd
     and group parser, `pw_slurp`, `read_secret`); `zenix.h` (libzenix) declares the helpers. The socket and poll
     numbers come from the kernel's headers through `zenix-consts.h` (37 more names; the host test checks 137).
     Every libz program's output is unchanged (the whole suite); the sum of the program files is 7.7 KB smaller than
     step 1's (2 KB of it `libctest`), the largest growth 328 bytes (`nettest`: inlining across files lost);
     `kernel.bin` is byte-identical.
   - `libctest` grew to 143 checks (signals, `poll`/`select`, a UDP datagram to itself on the loopback, the helpers,
     the users, `glob`, `regcomp`, `posix_spawn` with file actions, `envp` and a process group, `wait`, `system`,
     `popen` both ways) and a `tty` part (15 checks, keys typed by the test: the console's size and `ENOTTY` on a
     pipe, raw and back, ^C a byte without `ISIG`, `ICANON` without `ECHO` with nothing on the console's grid
     (`CON_READ`), the line editing's erase and a line read in pieces, `getpass`, cooked again).
   - Sizes: a raw program 568 bytes (536 at step 1: `read`'s hook), hello with `printf` 6,348, the sort tool 9,996;
     `termios.c` is 1,288 bytes of code and 714 of BSS (its line), only in programs that set the terminal.
   - *Decided here where the note is silent (for the main session to confirm):* `waitpid` gives SYS_wait's status
     (the shell's `$?`: 128 + the signal for a program a signal ended) as an exit status, so `WIFSIGNALED` is never
     true; a stopped child (`WUNTRACED`) is stopped by `SIGTSTP`; `waitpid(-1)` and `wait` find which child ended from
     the process table before and after (SYS_wait does not say); `pid` 0 or below -1 answers `EINVAL`. `tcgetattr`
     answers the settings this process set last (the kernel cannot be asked), cooked at the start. In the port's line
     editing ^Z and ^\ are bytes (the kernel's raw mode signals only ^C); a line is at most 511 characters, refused
     with the kernel's words. `VMIN`/`VTIME` are checked whatever `ICANON` is. `getpass` puts back the mode set last.
     `posix_spawnattr_setsigmask` and `setsigdefault` answer `EINVAL` (the scheduling ones are absent); at most 8
     opens per spawn. `system` does not ignore `SIGINT`/`SIGQUIT` while it waits (POSIX asks it). `glob`'s
     unsupported flags answer `GLOB_ABEND`; `regexec`'s `REG_NOTEOL` answers `REG_INVARG`; `REG_NEWLINE` covers `^`
     and `$` only; 9 groups at most are reported.
4. **The bridge:** `dprintf`/`vdprintf`, `printf` to `dprintf(1, ...)` and the other renames across the tree; every
   test's output unchanged.
   **As built (step 4, 2026-10-29):**
   - libz's `fprintf(int fd, ...)` and `vfprintf(int fd, ...)` are `dprintf` and `vdprintf` (the same code); libz's
     `printf` is gone and its calls are `dprintf(1, ...)`: 137 files under `user/` and `net/` (a script's
     mechanical rename; `fsenv.h`'s `fs_log` with them; lwIP's own files untouched, the C library's programs keep
     C's `printf`); `pw.h`'s inline `fputs(s, fd)` is `pw_fputs`; libz's sizes are `size_t` (`__SIZE_TYPE__`,
     `unsigned long` on m68k-elf as before: `strlen`, the `mem*` and `strn*` functions, `malloc`, `calloc`,
     `realloc`, `qsort`, `snprintf`, `vsnprintf`, `path_find`, `zrealpath`). The PC builds of Zenix files
     (`tools/host/zhost.h`, `zup-host.h`) map `dprintf` onto the host's streams, so a test's lines keep their order.
   - Sizes (the whole tree against 5d892bd): the program files 1,380 bytes more in all (each `dprintf(1, ...)` pushes
     one argument more); the largest growth `fsck.ext2` 444, `termtest` 340, `mount` 196; `kernel.bin` 64 more (fsd
     inside it): the mount tests' floppy keeps 9,308 bytes (9,372 before). Every test's output unchanged (`make
     check`).
   - `system` ignores `SIGINT` and `SIGQUIT` while it waits and puts the caller's handlers back (the main session's
     decision of 2026-10-29). Set **after** the spawn: a child inherits ignored and blocked signals (as across
     `exec`) and `SYS_spawn` cannot reset them as POSIX's `fork` does in between, so ignoring first would make the
     command deaf to ^C. A signal sent before the caller runs again after the spawn still reaches it (limits.md).
     `libctest` checks it (3 checks: a command that signals its caller, the handlers back, the command's own
     defaults through `libctest sigdfl`).
   - *Found and fixed:* a C library program that does not use the environment crashed at its start: `_zstart` called
     `zenv_load` through a weak reference, which is address 0 when `zenv.o` is not linked, and `mkzexe` relocated it
     like any address, to the program's own start, which then called itself until the stack guard stopped it
     (`libctest` uses the environment, so no test had met it; `tuitest` did). The environment is now loaded by a
     pointer `zenv.c` puts in `.init_array` (C library build only; gcc's `constructor` attribute uses `.ctors` here,
     which the link scripts drop), and `mkzexe` refuses an undefined weak symbol in a ZEXE as it does in a ZEX2
     (`tools/mkzexe-test.sh` checks both).
5. **The switch, measured:** the size table for every program three ways; static counts for the hot programs; with the
   owner home, the stock A1200's timed runs; then the base system on the C library + libzenix, libz's own C code
   deleted, the docs of section 12. Two reviews.

## 14. Decisions and open questions

**Decided by the main session (2026-10-28), not owner questions:** picolibc instead of the roadmap's newlib, for 3.3's
reasons (the same code base, 25-45% smaller than newlib per program, licences listed file by file); the roadmap's block
3 line is corrected with it. D13 stage 2 as section 8 decides it.

**For the owner** (one line each, the recommendation in brackets):
1. Ahiru and the CFile core it uses are in a private repository; porting Ahiru puts its code into Zenix's tree, which will be public: may it be published, under MIT like Sumi, Maki and CUtils? [Yes, MIT]
2. Your own programs (Sumi, Maki, Markdown, Ahiru, `ctop`) keep one source for AmigaOS and Zenix, each in its own repository, and Zenix carries a copy, rather than Zenix getting its own versions? [Yes, one source]

## 15. Reviews

Two independent read-only reviews of revision 1 (2026-10-28); every finding is worked in above.

**Correctness against the code and the frozen interfaces** (read against `usys.h`, `libz.c`, `crt0.S`, both link
scripts, `purecc.sh`, `mkzexe.py`, `kernel/tty.c`, `kernel/term.c`, `kernel/errno.h`, `kernel/kernel.h`, `proc.c` and
picolibc's headers). *No blocker.* *Should fix* - "no error number" was wrong: `EDOM`, `EILSEQ`, `EOVERFLOW` and
`ENOTSUP` are missing and picolibc uses them (5, 12: claimed with Linux's values); the constants example was wrong
(`O_CREAT` agrees; `O_EXCL` 0x800 is Zenix's `O_NONBLOCK`, the real trap; picolibc's signals are in `signal.h`) (5); the
termios mapping did not fit `tty.c` (`TTY_NOSIG` only counts in raw mode; `ECHO` off alone would have echoed passwords):
any bit off now means raw plus the port's own line editing, and `getpass` (5.4); `CLOCK_MONOTONIC` from the E-clock
would jump every 101 minutes: the 100 Hz tick instead (5); `TIOCGWINSZ` claimed pseudo-terminal sizes that do not
exist: the console's size, `ENOTTY`, and the future argument named (5.4); the size comparison put libz at `-O2` against
picolibc at `-Os` (3.1, redone); `zerrno`'s definition would clash with `errno` (6); `fcntl`'s `O_NONBLOCK` is sockets
only (5). *Minor* - `mkdir`'s mode, `utimes`' single time (5); `posix_spawn`'s `envp`, process group, `getegid` (5, 5.5);
the 63-variable cap (5); `zenixboot` and `bootmenu` in the byte-identical check (5.3: included); the 97 files of the
bridge and `pw.h`'s inline `fputs` (6); `__muldi3` in pure programs (7). *Found sound:* every system-call number in
section 5's table; `NREG`, `ENVMAX`, `NFD`, the path limit; the allocator, `EPIPE` 141, crt0 and `_zstart`; spawn's
descriptors 0-2; `TTY_RAW`/`TTY_NOSIG` and `SYS_ttyinfo`; the 64-bit division plan; the stage 2 reasoning.

**Size, speed and coherence** (with its own builds: libz at `-Os`, picolibc's formatter and buffer code at `-O2`, a raw
program). *Blockers* - with libz at the same setting the "smaller than libz" claim flipped (libz hello 2,192 bytes,
picolibc's 4,256): section 3.1 redone, and the cause measured (picolibc's formatter is libz's size, t9 2,312; the
`FILE` streams are the 2.2 KB); the planned `-O2` formatter would add 1.9 KB to every printing program: it stays
`-Os` (3.1, 4, 10); with both, the switch rule could not be met, so "one library in the end" was not credible: Zenix's
own programs now keep writing straight to descriptors (`dprintf`, the `oputs` family) and only ported programs pay for
streams; the switch rule gained a total and a kernel budget and the fallback is named (6). *Should fix* - a 4 KB
`malloc`'d buffer would pull an 8 KB chunk and more: buffers sized to the first chunk (5.2); line buffering on a
terminal would make the base programs write once per line: they keep libz's 4 KB buffer (5.2); stdio must not reach
programs that do not use it (5.2, 10: under 600 bytes); the stage 2 trigger skipped the text-UI library D13 section 9
named: decided static here, and the 100 KB check given to block 3's closing audit (8); the 20-50 KB estimate's basis
(8: 16-70 KB, *estimate*) and the frozen-interface argument applies to jump-table libraries too (8); "not base" against
the roadmap's "Versions and updates", and D22 7.3's 6-9 minutes for a widely used fix (6); `KERNEL_MAX` only "watched"
(6: a budget); "2% (or 256 bytes)" ambiguous (6: the larger, plus a total). *Minor* - the stand-in glue said (status
line); the `long long` `printf` the default in the Makefile rule, a missed float build visible (3.1, 11); fixed point for
a music player (9); rule 4 repeats D13 section 5 (9); `MatchFirst`'s patterns and `CSI` 0x9B (9); owner question 1 was
a technical choice (14: the main session's). *Found sound:* picolibc over newlib-nano; no new system calls; the
constants from the kernel's headers; one allocator, one regex engine, one passwd parser; the bridge; the pure build and
the 64-bit division; floating point staying ZEXE; 5.5's list; deferring a shared C library; the build order.

Revision 2 keeps revision 1's choice (picolibc, static, a port over today's system calls, no kernel code) and changes
how Zenix's own programs reach it: they keep libz's lighter way of writing output, so the switch to one library costs
them about 120 bytes each instead of 2 KB; and it decides the text-UI library's stage 2 question now.

Last checked against the code: 2026-10-28 (main at 722d6f6).

## Owner decisions (2026-10-28)

The owner answered **yes to every question above** (2026-10-28, "I think yes for all questions"): Ahiru and the CFile core it uses may be published in Zenix's tree under MIT; the owner's programs keep one source for AmigaOS and Zenix, each in its own repository, and Zenix carries a copy. This is the yes to the note: built in block 3's order (after block 2's audit).

## Main session, 2026-10-29: steps 0-1 merged

- **The raised size budgets** of `tools/libc-size.sh` (hello with printf 6,600, the sort tool 10,400, time +14,300,
  floating point +16,300, UTF-8 +5,800) are accepted: only ported programs pay them; the base system keeps libz's
  direct path (section 7), and its own rows are within the design's budgets. Cutting libz's allocator for the C
  library build is looked at in step 5's size table, not before.
- **The documentation split** is right: DESIGN.md D27 and architecture.md sections 4 and 12 change with the switch.
- The helper's other decisions (the init/fini lines in step 1, the port's own `fdopen` list, the environment by weak
  reference, `O_CLOEXEC` 0, `memalign` in the C library only, what stays in libz until step 3) are accepted as built.

## Main session, 2026-10-29: steps 2-3 merged

- **waitpid's status** stays as built (SYS_wait's 128 + n read as an exit status, `WIFSIGNALED` never true): the wait
  convention is frozen; a wait form that names the signal is the additive fix, weighed together with the audit's
  exit-status trap (D24 notes, block 2's audit), not a remapping of 129-159.
- **`system()`** ignores SIGINT and SIGQUIT while it waits, as POSIX asks: done in step 4 (the bridge), with its test.
- The other calls (wait's table compare, termios's remembered settings and raw-mode bytes, `REG_NOTEOL`,
  `REG_NEWLINE`, the unsupported spawn and glob flags) are accepted as built and stay listed in the step-3 "As built".

## For the main session: step 4 (the builder helper, 2026-10-29)

(The helper's notes for the main session's review; nothing here is decided by an owner question.)

- The bridge and `system`'s signals as step 4's "As built" says; the window before the caller runs again is the
  conservative choice (the command keeps ^C); a spawn that gives a child the default dispositions would close it
  (a `SYS_spawn` flag, a kernel change: not proposed).

## The text-UI library on Zenix (roadmap block 3, the first port; built 2026-10-29)

What section 8 decided (static, no shared library) and section 9 asked (one source, a pinned copy, its OS layer given
a Zenix implementation), as built; the files and the way to update them are in `ports/tui/README`.

- **The copy:** `ports/tui/upstream/` holds the owner's tui.library (AmigaLibs, MIT, commit 3b74efd, 0.5b1) as it
  is: the toolkit, the glue of the old `tui_*` names, the headers, the licence (`tools/tui-vendor.sh` copies them from
  a commit). Upstream has no platform layer of its own (its terminal code is AmigaOS calls in the same file), so
  Zenix's layer is separate files in `ports/tui/zenix/` that compile the upstream files unchanged: `core.c` renames
  upstream's terminal functions by `#define` and declares them `static` first, so they are compiled and never
  emitted, and gives its own; `include/amiga.h` maps the few AmigaOS calls the drawing half keeps (`Write`,
  `AllocVec`, `FreeVec`, `CopyMem`, `CurrentTime`) onto the C library and only declares the rest (a link error if a
  kept part ever needs one). A platform split in upstream itself (its terminal half in a file of its own) would make
  the renaming unnecessary: for the owner's repository, not done here.
- **Section 9's table, as built:** `SetMode` raw is `cfmakeraw` (no echo, no signals: ^C and ^Z arrive as keys,
  as upstream's Ctrl-C..F signals become keys); `ACTION_WAIT_CHAR` and `Wait()` are `poll` on the input descriptor,
  with upstream's timeouts (40 ms for the rest of an escape sequence); `CSI 0 q` is `TIOCGWINSZ` (80 x 24 if it
  fails, as upstream); a `BPTR` is a descriptor, 0 meaning descriptor 0 for the input and 1 for the output. No
  `SIGWINCH` and no resize events (the console never changes size).
- **Decided here (conservatively, for the main session to confirm):**
  - `CTerm` is 1 on Zenix: upstream's CTerm path is the ANSI one (the colours as they are, `ESC [ ? 25 h/l` for the
    cursor, the last cell drawn: the console's wrap is deferred, it does not scroll). Upstream's plain-console path
    would map the colours onto Workbench's four pens and hide the cursor with `CSI 0 SP p`, which the console does
    not know. `UTF8` 0, `CellW`/`CellH` 0; no mouse, no pastes. A program reading `CTerm` as "CTerm's mouse" gets
    no mouse events, nothing worse.
  - The console has no second screen: closing and suspending clear it (upstream does so in a plain console);
    `ESC [ ? 1049 h/l` and `ESC [ ? 7 l` are still sent, for a terminal that has them.
  - `TuiSignal` answers 0 and `TuiWait`'s extra signals are none: a program with a loop of its own polls
    descriptor 0 and calls `TuiPoll` (Ahiru's port decides whether it needs more, e.g. a call giving the descriptor:
    an upstream interface addition, not made here). A signal that interrupts a wait (its handler ran) ends it with
    `TE_NONE`, as upstream's extra signals do.
  - Widths: the console is Latin-1, so the toolkit writes Latin-1 and upstream's stand-ins for the rest, with
    upstream's own width table. `make TUI_UTF8=1` switches on UTF-8 output and the C library's `wcwidth` through
    upstream's own way to utf8.library (`TUI_LIB`); composing accents onto letters is not there (picolibc has no
    normalisation; the accent is left out, as upstream does without utf8.library). Off until D28 lands; compiled in
    every build so it keeps building.
  - Only a ZEXE build of the library for now; the pure build (`libtui-pure.a`, D27 9 rule 4) comes with Sumi, the
    first program that runs several times at once.
  - The test program injects the escape sequences itself (`CON_INPUT`, all at once, as the keyboard gives them):
    the serial tap types a byte every 20 ms, and more than 40 ms between ESC and the rest is a lone Esc by
    upstream's rule.
- **Sizes (measured, FS-UAE's stock A1200):** the library inside a program that uses all of it (`tuitest`) is 10,570
  bytes of code (section 8 estimated 5-12 KB); `tuitest` is 29 KB with the C library's streams. Memory while a
  terminal is open on 80 x 32: **98,336 bytes**, of which 81,920 are upstream's two grids (2,560 cells of 16 bytes
  each, what should be shown and what is shown) and about 7.6 KB its state (the 4 KB output buffer, 64 queued
  events); after closing the grids' 80 KB stays in the allocator's chunks for reuse until the program ends. That is
  about 9% of the stock A1200's 1,132 KB free per program with a terminal open (Sumi and Maki at once: about 190 KB).
  Smaller cells are upstream's design to change (the owner's repository), not the port's.
- **Found in upstream (for the owner's repository; the copy is not changed):** a 24-bit colour whose green value
  has bit 1 set (`TC_RGB(r, g, b)` with g = 2, 3, 6, 7, ... 255) is taken for a pen (`TC_PEN`, bit 0x200): `sgrcol`
  and `to16any` test 0x200 before 0x1000000, so it is drawn as pen `g & 7`. Testing 0x1000000 first fixes it.
- **Tests:** `tools/tui-test.sh` (in `make check`): `/tests/tuitest`, 46 checks against the console's grid read back
  with `CON_READ` (the title, a box, Latin-1 text, UTF-8 text as its stand-ins, colours and attributes, a wide
  character, the full block, the last cell, the cursor; only changed cells sent, `TuiRedraw` sending all; nine keys
  as events, among them ^C as a key and a lone Esc; a timed wait, `TuiPoll`; suspend, `TuiKeyAfter`, resume; the
  terminal given back cooked, with the cursor, cleared) on FS-UAE's stock A1200. The MIT notice travels with it
  (`/usr/share/licenses/tui` on the images that carry `/tests/tuitest`); no shipped program uses the library yet.

## Sumi on Zenix (roadmap block 3, the first of the owner's programs; built 2026-10-29)

Section 9's rule 1 for the first time on a whole program; the files, the choices and the way to update are in
`ports/sumi/README`, the user's page is `docs/man/sumi.txt`.

- **The copy:** `ports/sumi/upstream/` holds Sumi 0.2b6 (the owner's TextTools, MIT, commit 5334392) as it is
  (`tools/sumi-vendor.sh`, since Maki `tools/texttools-vendor.sh`). Upstream has no OS layer of its own (it calls
  AmigaOS directly), so Zenix's layer is AmigaOS's names over the C library (`ports/sumi/zenix/`, since Maki
  `ports/common/`: `include/amigados.h` and `dos.c`, about 580 lines): files,
  locks, `Examine`/`ExNext`, dates, protection bits as Unix modes, `IoErr`/`Fault`, `SystemTags` as `system()`.
  Upstream compiles unchanged, warning-free under `-Wwrite-strings` (no write into a literal, which a ZEX2 needs).
- **Built as shared code (rule 4):** `/bin/sumi` is ZEX2, over `libc-pure.a` and the text-UI library's pure build
  (`libtui-pure.a`, new here, as the library's "as built" planned). 93,752 bytes: 83,392 shared, 5,488 data,
  31,564 bytes of data and BSS per process (the 16 KB stack apart); the library's part of the code 11.4 KB.
  Stack: `STACK_DEFAULT`, measured (`ports/sumi/README`).
- **Settings:** upstream's `ENVARC:Sumi` is `$HOME/.config/Sumi` (the keys, the settings, where each file was left);
  `ENV:` has no copy on Zenix. Decided here with the others the README lists (no shared clipboard, `run` through
  `sh`, the mode and root's owner kept on save), for the main session to confirm.
- **Memory (FS-UAE, the stock A1200, 80 x 32, one file open):** Sumi owns 194 KB and shares 81 KB of code (`ps`):
  about a quarter of the 1,132 KB free with one login for the first Sumi, 194 KB for each one more. Most of it is
  upstream's design: the text-UI library's two 80 KB grids and a 26 KB places table (the README's findings, for the
  owner's repository).
- **Test:** `tools/sumi-test.sh` (in `make check`): the screen read back with `termtest screen` (`CON_READ`, then the
  keys typed with `CON_INPUT`, so every wait is for an event), the saved file byte for byte, its mode, the settings
  folder, the cursor put back at a second start.

## Main session, 2026-10-29: Sumi merged

- The port's choices are accepted as built (settings in `$HOME/.config/Sumi`, `ENV:Sumi` no file, `T:` as `/tmp`, the
  Unix mode kept on save, no clipboard between programs, `run` through sh).
- **Shift+Tab** sends `ESC [ Z` in raw mode, as xterm and Linux's console do: a small keyboard change, next job.
- **Memory:** 194 KB per Sumi on the stock A1200 is too much; the causes are upstream's (the text-UI library's 16-byte
  cells, 80 KB for two grids; Sumi's 26 KB places table allocated whole). A change to the owner's own libraries and
  programs (they run on AmigaOS too): proposed to the owner, not made; the copies are refreshed once upstream changes.

## Maki and Markdown on Zenix (roadmap block 3, the owner's pager and Markdown viewer; built 2026-10-29)

Sumi's way again (section 9's rule 1), with one layer for the three; the files, the choices and the way to update are
in `ports/common/README`, `ports/maki/README` and `ports/markdown/README`; the user's pages `docs/man/maki.txt` and
`docs/man/markdown.txt`. Both are in UAPPS under the owner's yes of 2026-10-28 (the block-3 programs); `less` and
`more` stay.

- **The copies:** `ports/maki/upstream/` (Maki 0.1b6) and `ports/markdown/upstream/` (Markdown 0.1b5), the owner's
  TextTools at commit 5334392, Sumi's commit too, unchanged (`tools/texttools-vendor.sh PROG`, which replaces
  `tools/sumi-vendor.sh`).
- **One layer, no copies:** `ports/sumi/zenix/` moved to `ports/common/` and grew what Maki and Markdown call
  (DateToStr, GetVar, Input/Output/IsInteractive, Delay, the FileHandle and DOS packets of Maki's pipe reader, the
  AmigaOS r w e bits from the Unix mode, SystemTags' SYS_Input/SYS_Output, OpenLibrary as none, and two headers whose
  calls are never reached: `proto/utf8.h`, `inline/macros.h`). `dos.c` is compiled once per program with the names it
  maps (`TT_NAMES_PROG` in the Makefile: "Sumi"; "Maki" and "FileColours"; "Markdown"), and the Makefile's rules for
  the three are one template (`TEXTTOOL_RULES`). Sumi grew 288 bytes of shared code by it.
- **Decided here (for the main session to confirm):**
  - **The text-UI library takes handles:** `TuiOpen`, `TuiOpenIO` and `TuiPossible` take the descriptor + 1, as the
    layer's handles are (0 still the default: descriptor 0 for the input, 1 for the output); before, a descriptor.
    Maki hands the library the terminal it opened (`tui_console`), which needs the two to agree. Only `tuitest`
    called them with a descriptor (changed with it).
  - **`TuiWait`'s extra signals are descriptors:** `tui_zenix_sigfd(bit)` names the one behind a signal bit
    (`ports/tui/zenix/sigfd.c`: none, a library member of its own; `ports/common/dos.c` gives the program's own: the
    reply port of Maki's pipe read). The wait polls it with the terminal and ends with `TE_NONE` and the bit when it
    has data, as upstream's extra signals do: a pipe's late lines show without a key (`tools/maki-test.sh` checks
    it). This is the addition the library's "as built" left for a program with a loop of its own.
  - **"*" is `/dev/tty`** (was `dup` of 0 or 1): the keys of a Maki reading a pipe come from the terminal.
  - **The licences:** packages `maki` and `markdown` (a family each, as `sumi`), each with its own notice and the
    text-UI library's under its own name (`maki-tui`, `markdown-tui`: two packages may not own one file, zpkg 4.3;
    `sumi` keeps `tui`).
  - **Markdown's stack is 32 KB** (`STACK_markdown`), upstream's own: its renderer recurses about 1 KB per container
    in a container, to 24. Upstream's `MAXDEPTH 10` build (its datatype's) would take 16 KB less per Markdown.
- **Memory (FS-UAE, the stock A1200, 80 x 32, one file open, `ps`):** Maki owns **650 KB** and shares 58 KB of code;
  Markdown owns **241 KB** and shares 131 KB; Sumi 195 KB and 82 KB. Maki's 650 KB is upstream's design (the cells of
  one 40,000-character line, 320 KB; its bytes, 72 KB; a 128 KB file cache; the library's two grids, 80 KB): more
  than half of the stock A1200's 1,132 KB free with one login. A change for the owner's repository (it runs on AmigaOS
  too): proposed, not made here, as Sumi's memory was.
- **Found in upstream** (the READMEs list them): Maki's memory above; a long search in Maki stops only on the Ctrl-C
  signal (a key on Zenix); Maki's pipe reader imitates AmigaOS's FileHandle; Markdown makes a link's target an
  AmigaDOS path ("../x" as "/x": on Zenix the wrong file); Markdown carries its own copy of Sumi's editor files.
- **Shift+Tab** (the main session's next job above): the keyboard sends `ESC [ Z` in raw mode (`kernel/kbd.c`);
  Sumi's unindent is checked through the keyboard's codes in `tools/sumi-test.sh` (`termtest screen`'s `\kHH`), the
  bytes in `tools/con-test.sh`; Zenix's own `edit` had Shift+Tab in its help but never read it: it does now.
- **Tests:** `tools/maki-test.sh` and `tools/markdown-test.sh` (in `make check`), Sumi's pattern (`termtest screen`,
  `CON_READ`, then `CON_INPUT`), their logs read by `tools/screenlog.py` (shared with `tools/sumi-test.sh`).

## The owner's leaner versions (builder helper, 2026-10-30)

The copies refreshed to the owner's new versions (`tools/tui-vendor.sh`, `tools/texttools-vendor.sh`, unchanged):
tui.library **0.5b3** (AmigaLibs cd59201: a cell of 6 bytes, not 16; 0.5b2 fixed the green-bit colour found here),
Sumi **0.2b7**, Maki **0.1b7**, Markdown **0.1b6** (TextTools ff34b48: buffers that grow when needed). Nothing in
Zenix's layers changed. Memory on FS-UAE's stock A1200, 80 x 32, one file open (`ps`, emulator figures):

| | before | after | its file |
|---|---|---|---|
| tuitest, a terminal open | 98,336 bytes | 81,920 bytes | 34,200 -> 35,332 bytes |
| Sumi | 195 KB (86 KB shared code) | **116 KB** (87 KB) | 98,756 -> 100,100 bytes |
| Maki | 650 KB (62 KB) | **202 KB** (63 KB) | 67,864 -> 69,488 bytes |
| Markdown | 241 KB (134 KB) | 242 KB (136 KB) | 152,412 -> 154,060 bytes |

Chip RAM free with the program, the test's shell and `ps` (1,335 KB before it starts): Sumi 918 -> 996 KB, Maki 487
-> 933 KB, Markdown 821 -> 818 KB.

- **For the main session (the allocator, not upstream):** the 15,360-byte grids are now small blocks (the 16 KB size
  class) instead of a region each, so they stay with the program after closing (tuitest's old check, "less after
  closing than while open", could no longer hold; it now checks that a second open and close takes nothing more), and
  Markdown's figure did not move: its heap probably grew into the allocator's next chunk, four times the last
  (read from `user/zmalloc.c`, not measured block by block). A 15 KB block taking a 16 KB class and chunks growing
  4, 16, 64, 256 KB are the allocator's rounding; a closer fit for blocks of 8-32 KB would let such savings show.

## Ahiru on Zenix (roadmap block 3, the owner's file manager; built 2026-10-30)

Section 9's rule 1 a fourth time, on the biggest of the owner's programs: Ahiru and the CFile core under it (the
owner's yes of 2026-10-28: published in Zenix's tree under MIT). The files, the choices and the way to update are in
`ports/ahiru/README`, the user's page is `docs/man/ahiru.txt`; `/bin/ahiru` is in UAPPS (approved with the block-3
programs), its package `ahiru` (its notice and the text-UI library's as `ahiru-tui`).

- **The copy:** `ports/ahiru/upstream/` holds Ahiru 0.3b6 and the core's files it builds (the owner's CFile
  repository, commit 78e80fd; `tools/ahiru-vendor.sh`). The CFile repository's files have no licence notice, so the
  script adds a two-line MIT notice naming the owner to each copied source file and the owner's MIT licence as
  `LICENSE` (the wording of his TextTools); otherwise unchanged.
- **One layer, no copies:** `ports/common` grew what Ahiru and the core call (the README's list: more places, locks
  with absolute paths, `NameFromLock` resolving links, `ParentDir`, `CurrentDir`, `Info`, `DateStamp` and
  `SetFileDate`, `AvailMem`, `Wait`, `Execute`, `SystemTags` in the background), and `gui.c`, the screen calls, which
  never open. Ahiru's own Zenix files are `ports/ahiru/zenix/`: `os.c`, the core's OS layer (`fs/os.h`, the seam the
  core already has for its Kickstarts) instead of upstream's AmigaOS one; `none.c`, the parts with no place on the
  console yet (sound, pictures, ARexx, the commodity, trackfile drives, module loading, the background paste); and
  `term.c` and `list.c`, upstream's compiled with one function each of Zenix's (every terminal taken for CTerm's, so
  MODE auto opens inside it; the parent of a Unix path).
- **Changed in the shared layer (for the main session to confirm):**
  - A `DateStamp` counts days from 1978, as AmigaOS's (it counted from 1970: only the layer read it before; the CFile
    core reads it itself and writes it into archives and disk images). Sumi, Maki and Markdown pass their tests
    unchanged.
  - `T:` is `/tmp` for any name (it was `T:PROG-` only), with `RAM:` (`/tmp`), `SYS:` (`/`), `NIL:` (`/dev/null`)
    and `PROGDIR:NAME` (`$HOME/.config/NAME/`): Ahiru's literals and default keys name them.
  - The text-UI library's `TuiSignal` answers bit 8 (it answered 0) and names the terminal's descriptor, which the
    layer's `Wait` polls: Ahiru's main loop waits in `Wait(ui_sig() | ...)`, as on AmigaOS. `tuitest` checks the
    bit.
  - `OpenLibrary("tui.library")` opens (the library linked in, its version 0.5): Ahiru's terminal half asks for it.
- **Decided here (conservatively; the README's list):** inside the terminal only; the background paste becomes a
  paste in the front (no threads, rule 3: slicing it is upstream's design to change); lha, lzx and zip read only
  (no outside packers); the volume list is the mount table; Unix wildcards for patterns; `C:Ed` (the default editor)
  is sumi; the stack `STACK_DEFAULT` (measured, about 11 KB at the deepest).
- **Sizes and memory (FS-UAE, the stock A1200, 80 x 32):** `/bin/ahiru` 242,876 bytes: 228,632 of shared code,
  8,026 of data, 41,518 of BSS. With a folder open Ahiru owns **392 KB** and shares 223 KB (`ps`): a third of the
  stock A1200's 1,132 KB free with one login.
- **For the main session (the allocator):** much of the 392 KB is `user/zmalloc.c`'s rounding, as Markdown's 242 KB
  above: a block is rounded up to a power of two (Ahiru's 16,400-byte preview buffer takes a 32 KB block), and the
  chunks grow four times over (4, 16, 64, 256 KB), so a heap a little over 84 KB holds 340 KB (read from the code and
  the sizes, not measured block by block). Chunks after 64 KB growing by 64 or 128 KB (at most 8 regions a process,
  D11) would let the ports' memory shrink without touching them; measured before it is changed. (Done 2026-10-30,
  measured: "The allocator"; Ahiru now owns 178 KB.)
- **Found in upstream** (the README lists them, for the owner's repository): AmigaDOS path shapes (`g/`, `dt`, the
  title's "//" at the root), names compared without case, a picture in an archive unpacked for a preview that cannot
  be shown, `O` and `launch` starting terminal programs without the terminal, keys typed during an operation dropped
  (`ui_abort` takes every waiting key; only Esc counts), AmigaOS words in messages, and the licence notes of
  `core/lha.c` ("follows LHa for UNIX"), `core/lzx.c` (unlzx) and `core/dms.c` (xDMS, public domain) for the owner to
  confirm under the notice added here.
- **Tests:** `tools/ahiru-test.sh` (in `make check`): the three columns read back (`termtest screen`, `CON_READ`), keys
  put in (`CON_INPUT`): a copy, a move, a delete at its question, E opening Sumi and back, an lha archive entered and
  its member (packed -lh5-) copied out; the files on the disk (the copy with its mode, the move, the delete, the link,
  the member with the archive's date); the settings and `FileColours`; the memory.

## The allocator (built 2026-10-30: one policy for every program, measured)

**What it was.** `user/zmalloc.c` rounded every block up to a power of two (16 bytes to 32 KB, the 8-byte header on
top), cut them from chunks of 4, 16, 64, 256 and 512 KB, never gave a small block back to the kernel, and gave each
block over 32 KB a region of its own; the kernel allows `NREG` = 8 regions a process. Ahiru's 16,400-byte buffer took
32 KB, and a program a little over a chunk held the next one, four times bigger and mostly empty.

**How it was measured.** A diagnostic build (`make UDIAG=-DZMALLOC_TRACE`: never shipped) makes the programs named in
`user/zmalloc.c` append every call to `/var/mt/NAME.PID`; their FS-UAE tests ran with it (ahiru twice, sumi, maki,
markdown, sh, netd over the loopback and with the A2065 and an NFS mount, nfscd, wasabid), and the files are
`tools/testdata/mtrace`. `tools/zmalloc-host.c` replays them against the allocator built for a 32-bit PC with the
kernel's regions imitated (16-byte headers and rounding, a region limit), counting what `ps` counts: the bytes held at
the peak. Policies tried on the nine traces (the sum of their peaks; the old allocator 1,457 KB): chunks doubling,
growing by 1.41, by half or all of the heap so far, by the regions held; own regions from 4, 8, 15, 16 or 32 KB, for
6 to 14 regions. **With 8 regions none was both small and roomy:** the smallest without a failed call held about
710 KB, but then only 250-430 KB of 100-byte blocks fit into the regions (the old one: 1,755 KB), because small
chunks use the regions up; growing late chunks fast enough for megabytes gave back most of the gain. **With 16
regions** the policy below holds 693 KB for the same traces and over 11 MB of 100-byte blocks (5 MB of 20 KB ones).
So `NREG` is 16 (kernel/kernel.h): 2 KB more of the kernel's BSS (eight more region slots in each of 32 process
slots); the kernel's region loops (a pointer check in a region, `SYS_memalloc`, `ps`) run over 16 slots.

**The design.** One allocator for libz and the C library (its head comment in `user/zmalloc.c` is the reference):
- An 8-byte header (capacity, tag) on every block, 8-aligned; a request is rounded up to 8 bytes (up to 256: to 16).
- Chunks hold blocks end to end with an end mark. A free block is in a bin of its size (powers of two, a bit map),
  its capacity repeated in its last word and a flag in the next header, so `free` joins both neighbours at once;
  `malloc` takes the best of the first eight that fit in its bin, else the first of the next bin that holds one.
- Quick lists, the hot path: a freed small block (at most 256 bytes) goes on the list of its size (16-byte steps;
  `malloc` takes from it or from the list 16 bytes up), a medium one (up to 8 KB) on its bin's list (`malloc` takes
  the head if it is big enough, else the next bin's). They go back into their chunks, joined, before the heap grows,
  when the medium lists hold more than 32 KB, or when the heap has a free block of 64 KB or more (memory a program
  gave back: quick blocks would pin its chunks; not the newest chunk's untouched rest, which a heap that has just
  grown always has: flushing for it at every free kept the lists empty, audit B3-11).
- A new chunk: the request with its headers, else 4 KB times 1.41 for each region the allocator holds (4, 6, 8, 12,
  16 ... KB, plus 64 bytes so two 4 KB blocks fit 8 KB's), from 10 regions on at least the heap so far, at most 4 MB;
  halved while memory is short. A chunk wholly free again (after its quick blocks went back) goes back to the kernel,
  except the first one.
- A request of 8 KB or more gets a region of its own while the allocator holds fewer than 10 regions (6 stay for the
  heap and the program's own `SYS_memalloc`); `free` gives it back at once. Past that, big blocks go into the chunks,
  so a program runs out of memory before it runs out of regions.
- `realloc` grows in place when the block, or the free block after it, is big enough.
- Safety stays: a pointer `malloc` did not give out, or one freed twice, is reported on standard error and ignored
  (as before); a damaged header found on the way (a free block's tag, a neighbour's when joining: a program wrote
  past its block) stops the program with status 134 and the header's address, since the lists cannot be trusted.
- The state is one structure reached through one address register (shorter code, fewer relocations in every
  program); the chunks' code is `-Os`, the quick lists' `-O2`.

**Memory, before and after** (FS-UAE, the stock A1200 configuration, 80 x 32; `ps`'s own memory with the program
open, from each test's printed figures; emulator figures, the same build otherwise):

| program (KB) | before | after |
|---|---|---|
| ahiru (a folder open) | 392 | 178 |
| markdown (a file open) | 242 | 135 |
| maki (a 300-line file) | 202 | 115 |
| sumi (a file open) | 116 | 73 |
| ctop (new) | | 102 |
| sh (the login shell: its own arena, malloc's first chunk) | 61 | 61 |
| netd (its own heap, kept: below), the loopback | 116 | 116 |
| netd, the A2065 and one NFS mount | 122 | 122 |
| nfscd (the region heap, kept), one mount | 120 | 120 |
| wasabid (the region heap, kept), one connection | 88 (D24) | 87 |

Chip RAM free at the prompt with one login: 1,304 KB before, 1,300 after (the kernel's 2 KB for 16 regions, the
programs' code).

The traces' peaks on the PC (KB held at the peak, the old allocator / the new one): ahiru 393 / 179 and 329 / 196
(an lha archive), markdown 169 / 61, maki 169 / 97, sumi 85 / 50, netd over the loopback 85 / 19 and with NFS
21 / 11, nfscd 121 / 61, wasabid 41 / 19, sh 5 / 5.

**Speed.** On the PC (`tools/zmalloc-test.sh`, the same code for 32-bit x86, -O2; it compares builds, nothing
more): a small free and malloc of mixed sizes 5.6 ns (old 9.6), a 300-3,800-byte malloc and free 4.5 ns (old 4.4),
the ahiru trace's calls 10.0 ns each (old 8.5), netd's 5.3 (old 5.4). In FS-UAE, `bench`'s two new lines run back to
back on both builds (emulator figures, not hardware): small blocks 49 us a pair (old 54), 300-3,800 bytes 67 (old
57). The emulated 68020 rewards few memory accesses and a hot path that fits its 256-byte instruction cache: a byte
count and a list bit map on the small path cost 15-25%, a free that also handled medium blocks in line slowed the
small path, so the small free is a push and one look at the bins' bit map, and the medium path sits in a function of
its own. The stock A1200's own figures are still to be taken (`bench`, the two malloc lines).

**Size.** The allocator's code is about 1.25 KB more in every program that frees and reallocates (`ls` 23,488 to
24,756 bytes, `sh` 90,596 to 91,840; the per-process BSS 212 bytes, was 56); all the programs' files together 99 KB
more (ctop's 62 KB aside). The C library's budgets grew with it (hello with `printf` 7.6 KB, section 10). The mount
tests' floppy (`zenix-mount.adf`) was full: passwd, whoami, uname and touch, which no mount test runs, left it
(28 KB free, 25 before). The RAM floppy (`zenix-ram.adf`) keeps 101 KB.

**The daemons' own heaps stay** (measured; the trace replays give KB at the peak / at rest):
- **netd's** (`net/netd/main.c`: first fit in regions a quarter of the profile's limit): folded into malloc it held
  120 KB in `ps` over the loopback and 126 with NFS on the stock A1200 (116 and 122 with its own; FS-UAE, netd-test and
  nfs-emu-test), though 120 against 152 with 4 MB of fast RAM (the large profile, whose 50 KB regions are mostly
  empty). The replays: its heap 17 / 17 over the loopback, 9 / 9 with NFS, 25 / 25 in wasabid's test, malloc 19 / 11,
  11 / 11, 31 / 19. The stock A1200 is the floor, so it stays; regions of a fixed 8 KB instead of a quarter of the
  limit would give the large profile most of the 32 KB (for the main session).
- **The region heap of wasabid and nfscd** (`rheap_*` in libz): wasabid 17 / 0 against malloc's 19 / 19 (its 4 KB
  pieces sit on the medium quick lists while idle), nfscd 54 against 61 per mount: their buffers are fixed 4 KB pieces
  its 16 KB regions hold exactly, and it gives everything back when idle.

**Also found and changed:** `badptr` asked for sixteen 64 KB blocks and expected NULL before the sixteenth (regions
ran out at 8); it now asks until memory or regions run out (at most 4,096). bench gained two lines (small blocks and
300-3,800-byte blocks).

## ctop on Zenix (roadmap block 3, the owner's monitor; built 2026-10-30)

Built as the other ports (D27 section 9; `ports/ctop/README` has the details and the choices): upstream's `ctop.c`
(CUtils, 0.1b3) pinned unchanged in `ports/ctop/upstream` (`tools/ctop-vendor.sh`), compiled over `ports/common` and
ctop's own Zenix files (`ports/ctop/zenix`: the part of CUtils' library ctop calls, `cu.h` and `cu.c`, and
bsdsocket.library's base, `sockbase.S`), a program with shared code (ZEX2) in UAPPS as `/bin/ctop`, the package
`ctop`.
- **What it shows on Zenix:** the processor (the kernel's log's CPU line), memory in use (the size: the kernel's boot
  log, the memory it had to give; free from `SYS_meminfo`), every mount's space (`SYS_mountinfo`, `statfs`), the
  network's bytes in and out a second and their totals while netd runs (`SOCK_IFLIST`'s counters, the loopback left
  out; ctop never starts netd), every process (`SYS_procinfo`: its name, its nice value as the priority, its state),
  sorting, the filter, signals (C, D, E, F as SIGINT, SIGTERM, SIGUSR1, SIGUSR2) and the priority (`setpriority`).
- **What it cannot show** (its page, NOTES): how busy the processor is and each process's CPU% and time (the kernel
  keeps no CPU time a process: docs/future-ideas.md item 4, not approved), a process's stack and its free part, CLI
  numbers, the Pi; the largest free block is the layer's guess.
- **Changed in the shared layer** (`ports/common`, for the main session to confirm): AvailMem's `MEMF_TOTAL`,
  `AttnFlags`, timer.device's `ReadEClock`, "/path:" as a path, BADDR of an address, `ENVARC:` writes making
  `$HOME/.config`, and `zenix_openlib` (a program's own libraries) - `ports/common/README`; `tools/purecc.sh` hands
  `-Wa,` options to the assembler.
- **Found and fixed while porting:** upstream sets a6 to the library base in inline assembler; this GCC frames with
  a6, so an inlined call wrote ctop's locals into its own shared code (the kernel: "shared code is DAMAGED"). ctop is
  built with `-fno-inline` (the calls stay in small functions that save a6).
- **Sizes and memory (FS-UAE, the stock A1200, 80 x 32):** `/bin/ctop` 62,412 bytes; with ctop open it owns 102
  KB and shares 57 KB (`ps`).
- **Found in upstream** (for the owner's repository; `ports/ctop/README`): the help says 0.1b1 while the version is
  0.1b3; the a6 inline calls depend on AmigaOS's GCC framing with a5; "CPU% is sampled" is the only wording for an
  unmeasured CPU; 256-task arrays fixed in BSS; AmigaOS words in its messages.
- **Tests:** `tools/ctop-test.sh` (in `make check`): the boxes read back (`CON_READ`), keys put in (`CON_INPUT`), the
  net box with netd running, the errors and the usage, the keys file, the memory; part n the net box with traffic over
  the A2065 (a download's rate, then 0B/s with nothing left of the longer rate after it).

## Step 5: the measurement (builder helper, 2026-10-30; no switch, as decided 2026-10-29)

**How it was built** (in `/tmp`, nothing of it in the tree): three builds of the whole tree at main 63d65e4. (1) Today:
libz at `-O2`. (2) libz at `-Os` (its own file and the shared files `user/z*.c` only; the programs' own code unchanged),
section 6's fair comparison. (3) The C library + a slim libzenix: every libz program's own code compiled exactly as
today (`-O2`, the same flags) with `-DZENIX_LIBC` and the C library's headers, a stand-in `libz.h` that includes the
standard headers and `zenix.h` and declares the Zenix-only helpers, and linked with `libzenix.a` + `libc.a` instead of
`libz.uo`. libzenix is libz.c without its own string, memory, ctype, `strtol`/`qsort`/`atoi` and formatter code (the
note's "libz's own C code deleted"): the line reader, `_zexit` (now `exit`), the region heap, `xmalloc`, `svc_line`,
`bre_to_ere`, the priorities, the `zu8*` functions and `oprintf` (C's formatter into the 4 KB output buffer through 128
bytes on the stack), one archive member each (one object pulled the environment's start-up hook into every program:
about 1 KB more each, measured). The tree needed three mechanical changes: `pw.h`'s private `memchr` and `sh.c`'s
`utoa` collide with the C library's names, and lwIP needs `SSIZE_MAX` from the C library's headers (netd built without
its string shim). Every image built; nothing run (a measurement of sizes only).

**The table** (ZEXE file bytes; the five ports and `libctest` are C library programs already and the same in all
three, so not listed; `over` = more than the larger of 256 bytes or 2% above today's file, section 6's rule):

```
program        libz -O2 libz -Os C library  change       %
diskd              9280     9444      9684    +404   +4.4
init              13620    11312     12636    -984   -7.2
sh                91840    88984     91472    -368   -0.4
ls                24756    21272     24220    -536   -2.2
cat               11304     8800     10152   -1152  -10.2
echo                340      560       528    +188  +55.3
pwd                6148     4288      4760   -1388  -22.6
ps                 4272     2384      3044   -1228  -28.7
free               4120     2232      2896   -1224  -29.7
wc                 7300     5440      5912   -1388  -19.0
grep              28568    25604     29824   +1256   +4.4
uname              4012     2124      2784   -1228  -30.6
hello              4140     2220      3140   -1000  -24.2
spin               4052     2132      3052   -1000  -24.7
crash              6000     4044      5056    -944  -15.7
badptr            14616    12464     13632    -984   -6.7
boot               7688     7640      8320    +632   +8.2
mkdir              6696     4836      5308   -1388  -20.7
rmdir              6244     4388      4856   -1388  -22.2
rm                11168     8848     10172    -996   -8.9
cp                12452    10124     11816    -636   -5.1
mv                12328    10000     11700    -628   -5.1
touch              6432     4568      5088   -1344  -20.9
msgpingd           5748     3804      4664   -1084  -18.9
msgping            6500     4580      4428   -2072  -31.9
socktestd          8204     6260      7208    -996  -12.1
socktest          26504    24244     25732    -772   -2.9
bench             17564    15428     15164   -2400  -13.7
fdisk             16468    14524     14576   -1892  -11.5
mkfs_ext2         12932    10692     10920   -2012  -15.6
fsck_ext2         41576    39408     40588    -988   -2.4
df                 6032     4144      3736   -2296  -38.1
sync                192      412       380    +188  +97.9
termtest          38740    36436     39672    +932   +2.4
head              11436     8900     10876    -560   -4.9
tail              12120     9520     11612    -508   -4.2
sort              12960    10468     11676   -1284   -9.9
uniq              11364     8836     10272   -1092   -9.6
cut               11804     9392     11144    -660   -5.6
tr                 6112     4264      7104    +992  +16.2
tee                6528     4664      5188   -1340  -20.5
seq                4884     2788      3884   -1000  -20.5
basename           6400     4216      5612    -788  -12.3
dirname            6264     4088      5436    -828  -13.2
yes                 284      504       472    +188  +66.2
sleep              4412     2568      3832    -580  -13.1
true                140      360       328    +188 +134.3
false               140      360       328    +188 +134.3
env                7608     5176      6688    -920  -12.1
printenv           4444     2472      3264   -1180  -26.6
which              4896     2900      3912    -984  -20.1
clear              3848     1960      2624   -1224  -31.8
whoami             8500     6248      7876    -624   -7.3
hostname           4372     2400      3196   -1176  -26.9
id                 9680     7348      8976    -704   -7.3
uptime             3984     2096      2760   -1224  -30.7
cmp                4764     2852      3584   -1180  -24.8
hexdump            7716     5520      6240   -1476  -19.1
stat               4384     2496      3160   -1224  -27.9
expr              20720    18340     21732   +1012   +4.9
find              14600    11800     13652    -948   -6.5
xargs             13236    10772     12924    -312   -2.4
du                11556     9092     10576    -980   -8.5
sed               29108    26456     30364   +1256   +4.3
diff              18300    15536     17484    -816   -4.5
chmod              4992     3152      4280    -712  -14.3
ln                10164     7892      9376    -788   -7.8
readlink           5976     3868      4896   -1080  -18.1
date               9984     7816      8448   -1536  -15.4
less              28668    25920     29568    +900   +3.1
edit              45960    42916     47664   +1704   +3.7
man                7996     5852      8912    +916  +11.5
gzip              34196    32064     37784   +3588  +10.5
tar               16780    14468     16384    -396   -2.4
input              4584     2660      3628    -956  -20.9
beep                192      412       380    +188  +97.9
dmesg              4268     2372      3084   -1184  -27.7
dd                 5860     3936      5172    -688  -11.7
login             15584    13120     14736    -848   -5.4
su                12880    10156     11820   -1060   -8.2
passwd            15336    12892     14476    -860   -5.6
chown             13228    10684     12648    -580   -4.4
sha256sum          8636     6644      7028   -1608  -18.6
printf             8124     5956      7120   -1004  -12.4
tac               10692     8248      9560   -1132  -10.6
rev               10072     7872      9112    -960   -9.5
nl                11432     8900     10504    -928   -8.1
fold              11136     8688     10656    -480   -4.3
paste             11304     8860     10240   -1064   -9.4
comm              11140     8724      9964   -1176  -10.6
expand             7324     5404      6332    -992  -13.5
mktemp             8044     6164      6844   -1200  -14.9
realpath           5328     3292      4572    -756  -14.2
base64             8272     6344      7528    -744   -9.0
md5sum             7680     5796      6212   -1468  -19.1
cal                6464     4220      5356   -1108  -17.1
time               8400     6524      6168   -2232  -26.6
timeout            8996     6832      7808   -1188  -13.2
kill               8232     6232      6924   -1308  -15.9
pgrep              7240     5328      6712    -528   -7.3
watch              6768     4620      6132    -636   -9.4
tree              11992     9640     11812    -180   -1.5
mount             16584    14416     17428    +844   +5.1
umount             6372     4516      4984   -1388  -21.8
mkfs_zenixfs      22960    20760     22568    -392   -1.7
fsck_zenixfs      24140    21988     23420    -720   -3.0
nice               9032     6880      7796   -1236  -13.7
renice            11856     9624     10980    -876   -7.4
priotest          12764    10864     11796    -968   -7.6
reboot             8648     6676      7112   -1536  -17.8
cu                 7804     5860      6380   -1424  -18.2
awk              102840   100464    103136    +296   +0.3
svctest           11020     9156     10264    -756   -6.9
useradd           16508    14272     15956    -552   -3.3
userdel           13240    11056     12620    -620   -4.7
gpasswd           13976    11708     13276    -700   -5.0
sudo              16832    14044     15556   -1276   -7.6
zup               57616    55404     61428   +3812   +6.6
sigtest           30820    28144     29724   -1096   -3.6
netstat           13716    11252     12828    -888   -6.5
nettest           48048    43980     46012   -2036   -4.2
ifconfig          14056    11784     12900   -1156   -8.2
route              9416     7236      7900   -1516  -16.1
ping              15204    12576     14068   -1136   -7.5
host              13004    10612     11784   -1220   -9.4
telnet            33344    30508     32800    -544   -1.6
rz                25544    23312     24564    -980   -3.8
cryptotest        80908    78908     78832   -2076   -2.6
ssh              108468   105756    108404     -64   -0.1
ssh-keygen        57532    55280     56828    -704   -1.2
wget             121652   118856    122568    +916   +0.8
sntp              17812    15164     16256   -1556   -8.7
zpkg              90624    87832     92176   +1552   +1.7
zfsd              38184    36024     37688    -496   -1.3
netd              93896    91992     93536    -360   -0.4
wasabid           64748    61856     63580   -1168   -1.8
nfscd             60816    58444     60204    -612   -1.0
drv-a2065          8384     6416      7480    -904  -10.8
drv-genet         17504    15524     16736    -768   -4.4
total (139)     2474196  2181644   2375304  -98892   -4.0
kernel.bin       166876   165024    166864     -12   -0.0
```

**The rule applied (section 6 step 3):**
- *No program more than 256 bytes or 2% bigger:* **fails for 13 files**, among them two hot ones: `grep` +1,256
  (+4.4%) and `less` +900 (+3.1%); the largest `zup` +3,812, `gzip` +3,588, `edit` +1,704, `diskd` +404 (in the
  kernel). 117 files shrink, 22 grow.
- *The sum not bigger:* passes, **98,892 bytes smaller (-4.0%)** than today.
- *`kernel.bin` not bigger:* passes, 12 bytes smaller (diskd +404 and boot +632 inside it, fsd smaller); the mount
  tests' floppy keeps 4,144 bytes (4,132 today).
- *The hot programs not slower* (static instruction counts, objdump; the A1200's timing is the owner's): the C
  library's string functions take as many or fewer instructions per byte: `strlen` 2 per byte (libz 3), `memcpy` 5
  per 16 bytes (libz 3 per 4, with a compare against the stack, and a byte loop when the pointers are not aligned),
  `memset` 2 per 4 bytes (libz 3), `strcmp` about 11 per 4 aligned bytes (libz 7 per byte), `strchr` about 14 per 4
  aligned bytes (libz 5 per byte); their start costs about 10 instructions more, which short strings pay. The
  formatter's per-character path is shorter (the stream's `put` about 12 instructions plus an indirect call, libz's
  `put` about 22 plus a call). `cat` copies with `read`/`write` either way. **No loss by these counts**; the stock
  A1200's `bench` and text-tool runs (section 10) are not done.
- *Memory per process:* the same within 8 bytes for `ls`, `cat`, `grep`, `less`; `sh` +1,296 bytes of BSS (the
  environment's store, which libz keeps elsewhere).

**Where the bytes go** (summed over the 139 files, by symbol): C's formatter is smaller (the `-Os` `vfprintf` and
`__ultoa_invert`, 1.66 KB, against libz's `-O2` `vfmt` + `fmt_num` + `put`, 3.3 KB: -235 KB in all); the losses are the
C library's faster string code (`strstr`'s two-way search 1.5 KB in 15 of them, `strchr` 242 bytes, `strtol` 282),
`memset`, the ctype table (384 bytes), the start and end (`__libc_init_array`, `exit`), and `sys3` no longer inlined in
some places. `grep`, `sed`, `expr`, `less` and `edit` grow by the two-way `strstr` and `strchr` mostly.

**The finding the table also shows:** libz at `-Os` alone is **292,552 bytes (11.8%) smaller** than today, 132 of the
139 files smaller (`diskd` +164; `echo`, `sync`, `yes`, `true`, `false`, `beep` +220 each), and `kernel.bin` 1,852
bytes smaller (the mount tests' floppy 5,984 left). Against that build the C library is 193,660 bytes bigger.
libz's hot string functions at `-Os` need their own count before that is taken (its byte loops are the same code at
either setting; the formatter's speed is the open question).

**Conclusion: not a clear win.** The total and the kernel pass, the speed counts pass, but 13 of the files break the
per-program rule, two of them hot, and the cheaper size win is libz at `-Os`, which the C library does not beat. As
decided (2026-10-29), **nothing is switched and both libraries stay**. For the main session: (1) libz's cold parts at
`-Os` (the formatter, `qsort`, `strtol`, the helpers), its hot string functions kept at `-O2` or taken from picolibc's
assembly, is a measured -290 KB with no switch; (2) if the switch is wanted later, libzenix could carry libz's small
`strstr`/`strchr` in place of picolibc's (most of the 13 overs), then the table again and the A1200's timings.
Measured 2026-10-30 (builds in /tmp at 63d65e4, ZEXE bytes, objdump counts; no emulator or hardware runs).
