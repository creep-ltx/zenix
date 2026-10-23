# Zenix compared with Unix and Linux

Statements about Unix, Linux, Minix, QNX and Fuzix are from general knowledge; Zenix statements are read from the
source (last checked 2026-10-23). Not benchmarked against any of them.

## What Zenix keeps from Unix

- The **user experience**: a tree rooted at `/`, files/directories/links, pipes and redirects, a shell with
  scripting and job control, a text userland (`ls cat grep sed sort find tar gzip diff ...`) with standard flags
  and standard output formats, `man`, `/etc/motd`, `$USER`, `$PATH`-style environment, exit statuses, `^C` and
  `^D`.
- **Everything is a file descriptor**: files, pipes, the terminal, `/dev/null`, `/dev/zero`, `/dev/tty`.
- **The process model's skeleton:** `spawn` + `wait` + exit status, per-process cwd, environment and fd table,
  zombies until reaped (one nobody will wait for is freed by the kernel).
- **ext2** as the on-disk format, so a Zenix disk can be mounted on Linux and fsck'd with `e2fsck`; the Zenix
  `mkfs.ext2` and `fsck.ext2` are checked against the host tools.
- A **POSIX-flavoured libc** (small and incomplete).

## What is different - and why

| | Unix / Linux | Zenix |
|---|---|---|
| Hardware | MMU required (Linux: separate no-MMU uClinux port) | No MMU: one shared address space |
| Process creation | `fork` + `exec` | `spawn` only. `fork` cannot work without an MMU (a copy would need different addresses) |
| Memory protection | Per-process virtual memory; a wild write is a segfault | None. Pointers are checked at system calls only; a wild write inside user code can corrupt anyone |
| Kernel structure | Monolithic (Linux) with loadable modules | Small kernel + servers (filesystems, disk) behind synchronous message ports; drivers, tty and display still in the kernel for now |
| Kernel concurrency | Preemptive kernel, locks, RCU, per-CPU data | Kernel never preempted: single thread of control, no locks; sleeping = blocking in `schedule()` |
| System calls | Hundreds, `int 0x80`/`syscall` | Fewer than a hundred (architecture.md section 7), `TRAP #0` |
| Syscall into the filesystem | Function call into a VFS inside the kernel | Message to a user-mode server; the client blocks, the server writes straight into the client's buffer |
| IPC | Pipes, sockets, signals, SysV/POSIX IPC, shared memory, futexes | Pipes and kernel messages; the same address space makes shared memory unnecessary |
| Signals | `sigaction`, handlers, masks | No handlers: `kill` terminates; `^C` ends the foreground program, `^Z` stops it and `fg`/`bg` continue it (job control); the tty has a poll flag for shells |
| Users/permissions | uid/gid enforced by the kernel with MMU backing | Built (D8): real and effective uids, set-user-id programs, permission checks in the filesystem servers, `login`/`su`/`passwd`; honest limit: no security boundary against a program written to cheat |
| Memory allocation | Virtual memory, demand paging, swap, overcommit | Physical blocks, no paging, no swap; fragmentation is managed by allocation policy; every block has an owner |
| Executables | ELF (shared objects, dynamic linking) | ZEXE: one flat image + relocation table, static linking, no shared libraries yet |
| Scheduling | CFS/EEVDF, priorities, nice, multicore | Round robin on a 100 Hz tick, one CPU, no priorities yet |
| Time | 64-bit time, NTP | Seconds since 1970 in 32 bits; Amiga battery clock (MSM6242 / RP5C01) when present |
| Terminals | tty layer, ptys, termios, virtual consoles (Alt+F1..) | Ten virtual consoles /dev/tty1..10 (Amiga+F1..F10) with a login each, a controlling tty per process, partial termios (raw / no-signal flags); ANSI/VT100 interpreted in the kernel; no ptys |
| Networking, graphics stack, sound | Full | None yet (planned as servers) |

## Closest relatives

- **Linux without an MMU (uClinux).** Same hardware assumption, but Linux inherits the `fork` problem
  (`vfork` only), needs relocatable "FLAT" binaries and is sensitive to memory fragmentation. The authors tried
  it and rejected it as slow and fragile on the Amiga (see `Knowledge/amiga/zenix-restart-decisions.md`).
  Zenix is smaller, runs on 2 MB of chip RAM and was designed around that.
- **Fuzix.** A Unix for small machines (8-bit design, also ported to the Amiga by the same owner). Zenix is
  32-bit-first and message-structured; code from Fuzix could be taken because both are GPLv2.
- **Minix 3 and QNX.** Zenix's *architecture* is closest to these: a small kernel, filesystem and drivers as
  user-mode servers, **synchronous** send/receive/reply messages. The difference is the missing MMU: Minix 3 and QNX
  rely on it to isolate the servers, Zenix cannot, so its servers are trusted code.
- **AmigaOS** shares more with Zenix than Linux does at the level of mechanics - see vs-amigaos.md.

## Practical consequences for porting software

- Programs that only use `read/write/open/spawn/pipe` and the libc subset port easily (the authors wrote or ported
  all the commands natively; the regex engine came from CUtils).
- Programs that use `fork`, `mmap`, threads, sockets, dynamic loading or signal handlers do not run as they are.
- There is no `/proc` yet; `ps`, `free` and `uptime` use dedicated syscalls (`procinfo`, `meminfo`).

## Licence

Zenix is **GPL-2.0-only**, so Linux and Fuzix code can be reused (DESIGN.md D9).
