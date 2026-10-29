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
  `SYS_memalloc` (size classes, made for `NREG` = 8 regions per process, tags checked by `free`; architecture.md
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

`malloc` is libz's: size classes cut from chunks that grow 4 KB, 16, 64, 256, 512 KB, so a program uses few of its
eight regions; big blocks get a region and go back on `free`; a pointer `malloc` did not give out is reported, never
followed ("safety stays"). picolibc's own `malloc` needs `sbrk`, one growing heap, which a no-MMU Zenix cannot give.
The source moves to one file both libraries build (section 6). It gains what the standard asks and libz lacks:
`posix_memalign`/`aligned_alloc`, `malloc_usable_size`, `reallocarray`.

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
