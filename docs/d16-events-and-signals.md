# D16: events and signals (design note, revision 1)

Status: **proposed, awaiting two independent reviews** (deep core: the scheduler's waits and the way into user code).
The first item of roadmap block 2 ("Zenix reaches the world"), in the order of work the owner approved on 2026-10-25;
the network, telnet and every interactive program after it need it. The short version is in [DESIGN.md](DESIGN.md) D16.
Facts about today's code are *read* from main (2026-10-26).

## 1. The problem

1. **A process can wait for only one thing at a time:** one port, a timer, one child, or one pipe/tty (`block_io`).
   A terminal program cannot wait for the keyboard *and* the serial port (`cu` uses two processes for that, D12); a
   network program cannot wait for several connections; the shell polls the tty's interrupt flag (`tty_take_intr`).
2. **`wake_io()` wakes every process waiting for any I/O** on every pipe or tty event (proc.c); each wakes, checks and
   sleeps again.
3. **There are no Unix signals:** `kill` sets a flag that ends the process (or stops it, for ^Z), with no handlers, so
   programs cannot clean up on Ctrl-C, catch a child's exit, set an alarm, or notice a closed pipe (`SIGPIPE`). Ported
   programs (block 3, newlib) expect `signal`/`sigaction`, `select`/`poll`, `alarm`.

## 2. The idea: Exec's signal bits underneath, Unix on top

AmigaOS Exec solved waiting for many things with **signal bits** (ROM Kernel Reference Manual: Exec, "Signals":
`AllocSignal`, `Signal`, `Wait` and the mask `Wait` returns): each task has a word of *pending* bits and a word of bits
it *waits for*; any event sets its bit; one AND decides every wake-up. D16 builds Zenix's waiting on that, then puts
the Unix interfaces on top of it:

- **Event bits (kernel, new):** per process `ev_pending` and `ev_wait` (32 bits each). Every source a process may wait
  for gets one bit, which the process assigns: a file descriptor's readiness (readable, writable), a port with
  messages, a timer, child exit, a console switch. An event ORs the bit into `ev_pending`; if `ev_pending & ev_wait`
  is non-zero and the process is blocked, it becomes ready. A wait with the AND already non-zero returns at once, so no
  wake-up is ever lost (the check and the block happen with interrupts masked: audit B5's rule).
- **Per-object waiters:** each pipe end, tty and the serial device records which processes wait on it and with which
  bit, so an event wakes exactly those (problem 2). A small fixed list per object (4 entries), overflowing to today's
  wake-all behaviour, said in limits.md.
- **`poll` and `select` (libz, on one new system call):** `SYS_evwait` takes an array of (fd, events) and a timeout,
  registers the bits, waits once, returns the ready set. `select` is a thin layer over `poll` in libz.
- **Unix signals for programs:** numbers and meanings as Linux (SIGHUP 1, SIGINT 2, SIGQUIT 3, SIGKILL 9, SIGUSR1 10,
  SIGUSR2 12, SIGPIPE 13, SIGALRM 14, SIGTERM 15, SIGCHLD 17, SIGCONT 18, SIGSTOP 19, SIGTSTP 20, SIGTTIN 21, SIGTTOU
  22, SIGWINCH 28); a pending-signal word and a blocked mask per process; actions default / ignore / a handler.

## 3. Delivering a signal to a handler (without an MMU)

- **When:** on the way back to user mode (the end of a system call, or the level-2 interrupt that cut into user code:
  the places where `resched()` already runs), if a pending, unblocked signal has a handler.
- **How:** the kernel pushes a **signal frame** on the process's *user* stack: the saved user registers (d0-d7, a0-a6,
  sr, pc), the signal number and the old blocked mask; it sets the blocked mask to include the signal (and the
  handler's `sa_mask`), sets `a5` to the process's data base (D13: a pure program's handler needs it; the kernel keeps
  it in `struct proc`), and continues at a **trampoline** in libz that calls the handler and then `SYS_sigreturn`, which
  restores the frame. The frame is checked on return (it lies in the process's own stack, its saved `sr` has the
  supervisor bit clear): a damaged frame kills the process.
- **Stack space:** the frame is about 80 bytes; delivery checks the stack guard first and kills with "stack overflow
  in a signal" rather than writing below the guard.
- **Interrupted system calls** return `-EINTR` (`SA_RESTART` is not offered in revision 1; libz's wrappers do not retry
  silently). The calls that block (read, write on pipes/ttys, wait, sleep, `SYS_evwait`, message calls) are listed.
- **Message calls (D1/D11):** a client blocked in a server call is not interrupted mid-request (the server is working
  for it; D11's rule "a kill acts only between requests" stays): the signal is delivered when the call returns.

## 4. What changes for today's mechanisms

| Today | With D16 |
|---|---|
| `kill(pid)` sets `killed` | `kill(pid, sig)`; SIGKILL keeps today's `killed` path exactly; other signals go through pending/handlers; the old one-argument form (signal 0 in the second argument, which old programs pass) means SIGTERM, which ends a program with no handler as today |
| ^C: kill the foreground job, or set the tty's `intr` flag for the shell | ^C sends SIGINT to the foreground process group (today's `setfg` group); the shell has a handler; `intr` polling goes; `TTY_NOSIG` keeps ^C as a byte |
| ^Z: `proc_stop` | SIGTSTP to the foreground group; default action stop (today's path); a handler may catch it |
| a child's exit wakes a `wait` | also SIGCHLD (default: ignore) and its event bit |
| `cu` uses two processes | may use one with `poll` (kept two until tested) |
| D11 services | SIGKILL as today (between requests); servers ignore other signals unless they install handlers |

**Frozen interfaces (CLAUDE.md):** the signal numbers and their default actions, the signal frame layout and
`SYS_sigreturn`'s contract, `poll`'s event bits (Linux values) join the list.

## 5. New system calls (to claim in CLAUDE.md first)

`SYS_evwait` (poll), `SYS_sigaction`, `SYS_sigprocmask`, `SYS_sigreturn`, `SYS_alarm` (or a timer through
`evwait`'s timeout and a SIGALRM bit), and `SYS_kill` extended in a compatible way (section 4). Numbers from 83 on.

## 6. Programs that change

- **sh:** SIGINT/SIGCHLD handlers replace the `intr` polling; `trap` (the shell builtin) for scripts; job control as
  today but through SIGTSTP/SIGCONT/SIGCHLD.
- **kill, pgrep's -signal, timeout:** signal names and numbers.
- **less, edit:** clean up the terminal on SIGINT/SIGTERM.
- **cu:** optionally one process with `poll`.
- **New:** `sleep` with SIGALRM is not needed; `trap` in sh; man pages for signal(7)-style overview.

## 7. Tests

- Host: the bit logic (no lost wake-up under every interleaving of event and wait, with a model of the interrupt mask).
- Emulator: `poll` on a pipe, the tty and a timeout together; ^C runs a handler (a test program prints and exits);
  SIGCHLD; SIGALRM; SIGPIPE on writing to a closed pipe; a handler in a pure program (D13: `a5` right); a damaged
  signal frame kills; a signal during a server call is delivered after it; nested signals masked; `trap` in sh compared
  with bash; every existing test (job control especially) unchanged.
- The stack guard with a signal arriving near the stack's end.

## 8. Order of work

1. Event bits and per-object waiters in the kernel (`wake_io` replaced); `SYS_evwait`; `poll`/`select` in libz; tests.
2. Signals: pending/blocked words, `sigaction`, `sigprocmask`, delivery frame and trampoline, `sigreturn`, `kill` with
   numbers, ^C/^Z as SIGINT/SIGTSTP, SIGCHLD, SIGPIPE, SIGALRM; tests.
3. Programs: sh (`trap`, handlers instead of polling), kill/pgrep/timeout, less/edit clean-up; docs.

## 9. Open questions for the reviews

1. One event bit per fd/event pair assigned per `poll` call (kernel-managed, temporary), or bits a program allocates
   and keeps (Exec's `AllocSignal`)? Proposed: temporary per call for `poll`; a later `kqueue`-like registration may
   keep them.
2. `SA_RESTART`: offer it now (bash and many ports expect it), or EINTR only?
3. Process groups and sessions: today's `setfg` group is enough for ^C/^Z; is `setpgid`/`setsid` needed now for the
   shell's job control with signals, or with telnet's logins (block 2)?
4. Kernel size: the frame code, the calls and the per-object waiters are estimated at 2-3 KB of the 8 KB left on the RAM
   floppy; enough?

Last checked against the code: 2026-10-26.
