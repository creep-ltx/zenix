# D53: starting a script by its `#!` line

Approved by the partner 2026-11-13 (docs/decisions.md, "Block 7's choices", item 4: "Yes" to scripts started by their
`#!` line, as on Unix). A change to the kernel's program loader, so this note had two independent design reviews
before it was built (section 8) and one code review after (section 9). This is revision 2, after the reviews; built
on branch `shebang` 2026-11-13, checked on the PC and in FS-UAE, not yet on a real Amiga.

## 1. In plain words

`./game.lua`, `./build.sh` and `./x.mk` (whose first line is `#!/bin/make -f`) start like programs: the kernel reads
the file's first line and starts the program it names with the script's name after it. Before, a file that was not a
Zenix program could not be started by name at all (the shell said "cannot run"); `lua game.lua` was the only way.

## 2. Where it lives: the kernel's spawn

Three places were possible:

| Place | Who gets it | Cost |
|---|---|---|
| the kernel's `proc_spawn` | every program that starts another: the shell, `env`, `xargs`, `find -exec`, `timeout`, `make`'s recipes (through `sh -c`), `posix_spawn`, `system`, init's boot lines, the service manager | 712 bytes of kernel.bin (measured, section 6); the kernel heap for one 128-byte line and one argument vector during the start |
| libz's spawn wrapper | only programs linked with libz, and only after they are rebuilt; the C library's `posix_spawn` separately | the code in every program that starts others (sh, env, xargs, find, timeout, make, ...): a copy each (libz is static) |
| the shell only | `./x` typed at the prompt and in scripts; never `env`, `xargs`, `find -exec`, `make`'s direct runs, or any C program | small, but half a feature |

**Chosen: the kernel**, as on Unix and Linux (`binfmt_script`). It is the one place every way of starting a program
passes through, the code is one copy, and Zenix's kernel already has the same kind of redirection: an AmigaOS
program started by name runs as `/bin/zine PATH ARGS...` (DESIGN D29 12.1, `proc_spawn` in `kernel/proc.c`). The
`#!` case sits beside it.

## 3. The rules

A file that a program spawns, that passes the spawn's usual checks (it exists, the caller may execute it: the `x`
bit, and read it: the open is `O_RDONLY | O_EXEC`; Linux needs only `x` and lets the interpreter fail to read, the same
in practice), and whose first two bytes are `#!`:

1. **The line.** The kernel reads the file's first 128 bytes (fewer for a shorter file, the rest of its block zeros)
   and checks `#!` again on them (the file may have changed since the header was read). The first line runs from `#!`
   to the first newline or 0 byte (so also to the end of a shorter file). It may be at most **127 bytes** long, `#!`
   included, the newline not. A longer line refuses the spawn with `ENOEXEC`, never cut (Linux 5.1+ reads 256 bytes and
   cuts a long argument silently; Zenix's rule everywhere is "never a silent cut"). 127 is plenty for a path and one
   option, and the block is taken from the kernel heap, not the 4 KB kernel stack. A file of 2 or 3 bytes (`#!a`) is
   a script too.
2. **The interpreter.** After `#!` and any spaces or tabs, the interpreter's path runs to the next space, tab or the
   line's end. An empty one refuses the spawn with `ENOEXEC`. It is a path as any spawn takes: relative paths are
   from the caller's working directory, as on Linux (a service, spawned with no parent, has `/`); no `$PATH` search
   (that is what `#!/bin/env NAME` is for).
3. **One optional argument.** After the interpreter and its spaces or tabs, the rest of the line, with spaces and
   tabs at its end removed, is **one** argument, spaces inside it kept (Linux's rule: `#!/bin/env lua -i` gives env
   the single argument `lua -i`). Nothing left: no argument.
4. **The new argument list:** the interpreter's path as written on the line, the optional argument, the path the
   spawn was given for the script (`./game.lua`, or the full path the shell found on `$PATH`), then the original
   arguments after `argv[0]` (the original `argv[0]` is dropped, as on Linux). So the script sees its own name as
   `$0` (sh) or `arg[0]` (Lua), and its arguments as `$1...` / `arg[1]...`. The interpreter opens the script again by
   that path, from the same working directory.
5. **No script as interpreter.** The interpreter must be a Zenix program. An interpreter that is itself a `#!` script
   or an AmigaOS program refuses the spawn with `ENOEXEC`. Linux allows four levels of nesting; one is enough here
   (`#!/bin/env X` still chains, since env spawns again), keeps the kernel stack's depth what it was (the
   interpreter's start is a second call of the same function, after the first has returned, as ZINE's is) and needs
   no counter. `/bin/zine` itself must be a Zenix program by the same rule.
6. **The interpreter's own errors** pass through: a missing interpreter is `ENOENT`, one without `x` is `EACCES`,
   a broken one `ENOEXEC`. The kernel log gets a line only for a refused line (`exec: PATH refused: WHY`, the path
   being the caller's, never the file's bytes); the shell explains the rest (section 5).
7. **Set-user-id is not honoured for scripts.** The script's own bit has no effect: the script is never loaded, only
   its first line read (as on Linux). Unlike Linux, **the interpreter's set-user-id bit is not honoured either** when
   it is started for a script: a set-id interpreter would open the script's path again with its owner's rights after
   the kernel checked the caller's (a link swapped in between makes it read what the caller may not; Zenix has no
   `/dev/fd/N` to close that), and `#!/bin/sudo` would start itself without end. `sudo ./x` is the way.
8. **The process's name** (`ps`, `pgrep`, `top`) is the script's file name (`game.lua`), as on Linux, not the
   interpreter's. (An AmigaOS program still shows as `zine`, as before.)
9. **A carriage return is not removed**: a line ending `\r\n` names the interpreter `/bin/sh\r`, which is not found
   (`ENOENT`), as on Linux; the shell says "its #! line ends in a carriage return (a DOS file?)".
10. **Arguments.** The rewritten list is held to spawn's own limits, `SPAWN_ARGC` 256 words and `SPAWN_ARGBYTES` 8 KB
    (`kernel/kernel.h`; frozen), and is refused with `E2BIG` beyond them, as Linux checks its limits on the rewritten
    list: no program is ever given more than the limits promise (without memory protection a program with a fixed
    array of 256 pointers would otherwise be overrun). `xargs` leaves room for a script's three words (253 words,
    7,808 bytes a run), so its full runs start a script too.
11. **Size.** A script may be of any size: the `#!` test comes before the 256 KB limit, which is a program's.

What the script file holds after the first line is the interpreter's business. A file with neither a program's
magic, nor `#!`, nor AmigaOS's `HUNK_HEADER` is refused with `ENOEXEC` as before.

## 4. How it is built

- `kernel/shebang.h` (new): `shebang_parse(buf, &interp, &arg)`, a dependency-free inline function over the
  128-byte block that checks `#!`, cuts the line in place and returns 0 or why it is refused. The host test and the
  shell include the same header.
- `kernel/proc.c`: `spawn1` (which opens the file and reads its header; marked `noinline`, so its frame is on the stack
  once) sees `#!` before the size limit. When it was asked for a script, it takes 128 bytes from the kernel heap,
  zeroes them, reads the file's first 128 bytes (or fewer) into them, closes the file and answers "a script" with the
  block; when it was starting an interpreter or ZINE, a script or an AmigaOS program is `ENOEXEC`. `proc_spawn` parses
  the line, builds the new argument vector on the kernel heap, checks it against the limits, returns `EINTR` if the
  caller was killed meanwhile, calls `spawn1` for the interpreter (no script, no set-user-id) with the script's path as
  the process's name, which `proc_load` sets before the process is made ready (`set_name`), and frees both blocks on
  every path.
- `SPAWN_ARGC` and `SPAWN_ARGBYTES` moved from `kernel/syscall.c` to `kernel/kernel.h` (unchanged), so both checks use
  them.
- No new system call, error number, message or flag: nothing to claim. The behaviour joins
  `docs/frozen-interfaces.md` beside D29 12.1.

## 5. The shell

The shell finds a command on `$PATH` (or takes a path with a `/`) and spawns it; it does not read files to start
them. So `./build.sh` now runs through the kernel as above. When a spawn fails, and only then (a start costs nothing
more), the shell reads the file's first 128 bytes and, for a `#!` line, parses it with the kernel's own header and
says what it names:

    sh: ./x: #! interpreter /bin/bash: no such file or directory
    sh: ./x: #! line over 127 bytes
    sh: ./x: #! interpreter /tests/a.sh: not a Zenix program (a script, an AmigaOS program?)

A failure with `EACCES` is said plainly ("permission denied"), since it is the script's own `x` bit as often as the
interpreter's. Other programs (`env`, `xargs`, `find`) print the bare error text, as on Linux.

**A file with no `#!` and no program magic** stays refused ("not a Zenix program"), as before. POSIX shells and
`execvp` run such a file as a shell script; Zenix's shell and C library did not, and keep not doing so (`sh FILE`
runs it).

## 6. Size

Measured on the build: kernel.bin 198,780 -> 199,492 bytes (+712): `proc` 9,363 -> 10,070 code and data (+707: the
parser and its three texts, `spawn1`'s `#!` test and 128-byte read, `proc_spawn`'s rewritten list and its limits,
`set_name` shared with `proc_load`). The mount tests' RAM disk 684 -> 683 KB, so their floppy keeps its 800 bytes of
room (1,212 left). `sh` grows by the explanation (section 5). Memory at run time: 128 bytes and the argument vector
on the kernel heap while a script starts, freed before the spawn returns.

## 7. Tests

- `tools/shebang-host-test.sh`: `shebang_parse` on the PC under ASan and UBSan, every rule in section 3 (lines of 126,
  127 and 128 bytes, a 128-byte file without a newline; tabs; one argument with spaces; none; trailing spaces; a 0
  byte; an empty interpreter; a CR kept; a 3-byte file; a block that no longer starts with `#!`; an escape byte).
- `tools/shebang-test.sh` in FS-UAE (an A1200 with 4 MB of fast RAM; scripts in `rootfs/tests/shebang`): an sh
  script, a Lua script, `#!/bin/env lua`, a `#!/bin/make -f` makefile with a macro from the command line,
  `#!/bin/echo  one  two  ` (the order and the one argument with its spaces), a 127-byte line; the names and
  arguments each sees; the refusals and the shell's words (a 128-byte line, a missing interpreter, no `x` bit, an
  interpreter that is a script, a file with no magic, which `sh FILE` still runs) and the kernel log's line; scripts
  started from a `make` recipe, `xargs`, `env` and `find -exec`; a 320 KB script; 600 words through `xargs` into a
  script (full runs); the free memory after 400 starts, 300 of them refused; `ps`'s name; a set-user-id script owned
  by uid 1000 still running as root.

## 8. The design reviews (two, independent, 2026-11-13, on revision 1)

Both found the kernel the right place, the 127-byte refusal and one level of nesting sound. What changed:

| Finding | Done |
|---|---|
| ZINE's start must not take a "script" answer either (a `/bin/zine` that is a script would leak the block) | `spawn1` without a line buffer turns both kinds into `ENOEXEC`, for the interpreter and for ZINE (3.5) |
| A 2- or 3-byte file (`#!a`) was never read (the header read needed 4 bytes) | read from 2 bytes (3.1; host test) |
| The file may change between the header's read and the line's | `shebang_parse` checks `#!` on the block it parses; the block is zeroed first (3.1) |
| Inlining `spawn1` twice would double its frame on the 4 KB kernel stack | `noinline` (4) |
| `#!` must come before the 256 KB size test, or a big script is "file too large" | so (3.11; a 320 KB script in the test) |
| Adding words could give a program more than the frozen 256 / 8 KB | the rewritten list is checked, `E2BIG` (3.10); `xargs` leaves room for them |
| A set-user-id interpreter reopens the path with its owner's rights (a race on links), `#!/bin/sudo` loops | the interpreter's bit is not honoured for a script (3.7; a difference from Linux) |
| "Missing interpreter" guessed from `ENOENT`, other refusals said as "not a Zenix program" | the shell reads the line after a failure and names the interpreter and the reason (5) |
| Logging every interpreter error floods the log and copies the file's bytes to the console | one log line, for a refused line only, naming the caller's path (3.6) |
| A killed caller would still load a whole interpreter | `EINTR` before the interpreter's load (4) |
| Every error path must free both heap blocks (owned by no process) | one exit path frees both; the FS-UAE test compares free memory after 400 starts, 300 refused |
| The frozen entry must name the details | `docs/frozen-interfaces.md` D53 lists them |

Noted, not changed: a race where the script is deleted between the shell's lookup and the spawn is reported as a
missing interpreter (harmless); `/usr/bin/env` (the usual line in scripts from elsewhere) does not exist on Zenix;
a rate cap on kernel log lines is a general question (ZINE has the same).

## 9. The code review (one, 2026-11-13) and as built

No must-fix. Bounds, frees and the sentinel numbers were checked and found right. Fixed after it: the process was
renamed after the spawn had returned, when the interpreter could already have run (or ended) under its own name;
now the name goes into `proc_load` and is set before the process is ready. The shell blamed the interpreter for any
error of a valid `#!` script; now only for `ENOENT`, `ENOEXEC` and `EFBIG`, and a line ending in a carriage return is
said as such. The killed-caller check applies to scripts only, so ZINE's start is as before; one difference stays for
ZINE: a `/bin/zine` that is not a Zenix program is logged as "ZINE is not installed" (before: only a missing one).

Tests run 2026-11-13 (FS-UAE in fast mode: emulator results, not hardware): `shebang-host-test` and `shebang-test` (30
checks) pass; `cmds-test`, `cmds2-test`, `svc-test`, `lua-test` and `mounttest` pass; `make-test`'s line-for-line
comparison fails the same way on `main` (two of its targets made within one second: a timing flake, not this change).
A memory finding on the way: the test's own shell keeps about 6 KB more after a long loop line (its work area); the
check therefore adds the shell's size to the free memory, and the rest is the same after 400 starts.
