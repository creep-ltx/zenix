# D16: events and signals (design note, revision 3)

Status: **ready to build (revision 3, 2026-10-26), after two independent reviews** (deep core: the scheduler's waits and the
way into user code). The first item of roadmap block 2 ("Zenix reaches the world"), in the order of work the owner
approved on 2026-10-25. The short version is in [DESIGN.md](DESIGN.md) D16. Facts about today's code are *read* from
main (2026-10-26). Revision 2 takes in the first review: sigreturn masks the status register, the level-2 interrupt
saves every register, Exec's order for waiting (no check under masked interrupts), compatible `kill`, real process
groups (fixing ^C in pipelines), which signals interrupt what, slot masks instead of waiter lists, fixed event bits,
alarm as a kernel deadline, `SA_RESTART`, and Exec's single entry point for handlers. Revision 3 adds the second
review's exact rules (section 14), which the build follows where they are more precise than the sections above.

## 1. The problem

1. **A process can wait for only one thing at a time:** one port, a timer, one child, or one pipe/tty (`block_io`). A
   terminal program cannot wait for the keyboard *and* the serial port (`cu` uses two processes, D12); a network program
   cannot wait for several connections; the shell polls the tty's interrupt flag (`tty_take_intr`).
2. **`wake_io()` wakes every process waiting for any I/O** on every pipe or tty event.
3. **No Unix signals:** `kill` sets a flag that ends (or, for ^Z, stops) a process; no handlers, no SIGCHLD, SIGALRM or
   SIGPIPE. Ported programs (block 3) expect `signal`/`sigaction`, `select`/`poll`, `alarm`.
4. **^C in a pipeline stops only its last program** (found by the review): the tty's foreground is one pid (`tty->fg`),
   which the shell sets to the pipeline's last process, so in `sleep 100 | cat` ^C leaves `sleep` running.

## 2. The idea: Exec's model underneath, Unix on top

AmigaOS Exec waits on **signal bits** (ROM Kernel Reference Manual: Exec, "Signals" and "Waiting for a signal"): a task
has pending bits and bits it waits for, an event sets its bit, `Wait` returns the bits that fired and clears them, and
signals are held until the task waits. Exec also runs a task's **exception code** through one entry point when chosen
signals arrive (RKM Exec, "Exceptions": `tc_ExceptCode`, `tc_SigExcept`). D16 uses both ideas:

- **Fixed event bits, per process** (kernel): I/O ready, a signal arrived, the timer, a child changed state, a console
  switch, a message on an owned port. No bit allocator. `poll` rescans its file descriptors (at most 16, `NFD`) after
  each wake, which it must do anyway to fill in the results.
- **Slot masks on objects:** every pipe end, tty, the serial device and every port carries a 32-bit mask of the
  *process slots* waiting on it (`NPROC` is 32), so registering is one OR and there is no overflow. A waker readies only
  a process that is in the event-wait state with `ev_pending & ev_wait` non-zero; a stale bit (a process that left or
  died) is harmless, and `ready()` is never called here on a stopped or sleeping process. Every wait re-checks in a
  loop. Plain blocking `read`/`write` register the same way, so `wake_io` and `nio` go. Ports keep their single
  `waiter` alongside.
- **One system call for waiting, `SYS_evwait`:** an array of (fd, events) and a timeout. `poll` and `select` are libz.

## 3. Waiting without lost wake-ups (Exec's order)

Zenix's rule is that interrupts only queue; every producer of an event (tty input, ^C/^Z, serial bytes, pipes, console
switches) runs in process context at the four `kwork` points, and the kernel is never preempted. So:
1. register on every object and clear the bit;
2. check readiness **with interrupts on** (a tty's line check walks up to 1 KB; masking that would overrun Paula's
   one-byte receive buffer at 115200 baud);
3. with interrupts masked, **only** test `ev_pending & ev_wait` and set the state.
The producer sets the bit and readies the process under the same mask (`ready()` already masks). This also covers a
check that itself blocks, such as a socket served by a user-space network server later. **The rule for new drivers:**
the network interrupt only queues; `kwork` produces the event.

**Timeouts:** an `evwait` with a timeout blocks as `P_SLEEP` with `ev_wait` set (as `ipc_recv`'s timed wait does);
without one, in a new event-wait state. **`alarm`** is a per-process kernel deadline that joins `wake_min`, so it fires
even in CPU-bound code.

## 4. Signals

- **Numbers and meanings as Linux:** SIGHUP 1, SIGINT 2, SIGQUIT 3, SIGKILL 9, SIGUSR1 10, SIGUSR2 12, SIGPIPE 13,
  SIGALRM 14, SIGTERM 15, SIGCHLD 17, SIGCONT 18, SIGSTOP 19, SIGTSTP 20, SIGTTIN 21, SIGTTOU 22, SIGWINCH 28.
  **Reserved, not generated in revision 1:** SIGWINCH (consoles have a fixed size; telnet's window-size option later),
  SIGHUP (telnet hang-up or logout later; `cu` has no carrier detect), SIGTTIN/SIGTTOU, SIGQUIT.
- **Faults** (SIGSEGV, SIGBUS, SIGILL, SIGFPE) stay "killed, status 139" with no handlers in revision 1.
- **SIGKILL and SIGSTOP** can never be caught, blocked or ignored.
- **Per process (kernel):** pending, blocked, caught, ignored and restart masks, one `sa_mask` per signal (32 words),
  and **one entry point** (Exec's model): the handler table itself lives in libz, so the kernel needs no per-signal
  handler addresses or checks, and `struct proc` grows by about 160 bytes instead of 290.
- **Default actions:** terminate (through `proc_kill`, so the G3/G4 rules, D11's kill rules and `kill_now` apply
  unchanged), stop (today's `proc_stop`), continue, ignore. A numbered death gives the wait status 128+n (143 TERM, 137
  KILL, 141 PIPE): new values, not changed ones. **pid 1 is `boot`** (the built-in starter, which restarts `/bin/init`): never ended by a default action;
  the old form of `kill` keeps root's power to end it, as today (section 14, rule 6).
- **What interrupts a blocking call:** a signal wakes or interrupts a call **only if it will run a handler**. Ignored and
  default-ignore signals (SIGCHLD, SIGWINCH) wake nothing; blocked ones stay pending (a default-terminate signal
  unblocked later becomes a `proc_kill` then). A write that already moved bytes returns the count. No handler runs once
  `killed` or `exiting` is set. The blocking sites are listed in the build (tty reads, pipe reads and writes, the serial
  device, `sleep`, `wait`, `ipc_recv` for servers that install handlers - `-EINTR` gets a defined meaning there - and the
  service manager's waits).
- **A client in a server call** is protected by G4's rule (`kill_now` waits while `wait_msg` is set): the signal is
  delivered when the call returns.
- **`SA_RESTART` is offered:** on 020-060 a TRAP's stacked PC is the next instruction and `trap #0` is 2 bytes, so a
  restart is `pc -= 2` with `d0` = the original call number (saved at entry). Never for `sleep` and `evwait` (as Linux).
- **SIGCHLD:** default keeps zombies for `wait`; an explicit `SIG_IGN` reaps automatically (POSIX); also sent when a
  child stops (the shell's `ST_STOPPED`).
- **A stopped process:** a default-terminate signal ends it (as today); a caught one waits for SIGCONT; SIGCONT always
  continues it, then runs its handler.
- **SIGPIPE** changes what old programs see on writing to a closed pipe: today `-EPIPE`, afterwards death with 141.
  Accepted (it is what every Unix does; the shell's pipelines rely on it).

## 5. Delivering a signal to the handler

- **Where:** one function, `sig_deliver(registers)`, runs as the very last step before returning to user mode on both
  paths: at the end of a system call (after `proc_kill_now`, after the result is stored, so the frame saves `-EINTR`),
  and on every return path of the level-2 interrupt when it cut into user code (including the early `schedule(); return`
  paths). **The level-2 entry saves all 15 registers** like the system-call entry and passes the block to C (today it
  saves only d0-d1/a0-a1, so a CPU-bound program could never receive a handler; the cost is about 11 more longs each way
  per tick, roughly 0.1-0.2% of a 14 MHz 68020, an estimate). No other return to user mode needs it.
- **A stop asked by the process itself** (`kill(getpid(), SIGSTOP)` from a SIGTSTP handler) is acted on at once: the end
  of a system call schedules when `stop_req` is set.
- **The frame** is written on the process's user stack after checking `user_ok_wr` over its whole length and the stack
  guard (else "stack overflow in a signal", killed): a size word first (frozen, grows at the end), the saved d0-d7,
  a0-a6, the user PC and condition codes, the signal number and the old blocked mask. No FPU state: programs are built
  `-msoft-float`. **The kernel never copies or writes a format word:** it edits the PC and the condition codes of the
  format-0 frame already on its stack (TRAP and interrupts give format 0 with "PC = next instruction" on 020-060, MC68060
  UM 8.4.1).
- **Into user code:** `a5` = the process's data base (D13; 0 for v1 programs, harmless since all registers are restored
  after), the PC = the process's entry point (registered once by libz at start-up, like Linux's `sa_restorer`), the
  signal number in a register; the blocked mask gains the signal and its `sa_mask`. libz's entry calls the handler from
  its table, then `SYS_sigreturn`.
- **`sigreturn` checks (a forged frame can do no more than the process could do anyway):** the frame lies in the
  process's memory (`user_ok_rd` over its whole length **before** the kernel reads it: a read of non-existent memory in
  supervisor mode is a bus error, a panic); the PC is even (else killed); **the status register is never taken from the
  frame: only its condition codes** (`sr = frame & 0x001F`, the upper byte always 0, as Linux m68k does). A forged SR of
  0x0700 would otherwise run user code with every interrupt masked, and one with the M bit (0x1000) would make the next
  trap use an undefined master stack on 020-040.

## 6. Process groups

- **`pgid`** in `struct proc`, inherited at spawn; **`SYS_setpgid`**: a parent sets its child's group just after spawn
  (there is no `fork`).
- **The tty's foreground becomes a group:** `setfg(pid)` keeps its frozen meaning ("this one process"); the group
  meaning comes through a new argument old callers pass as 0 (or a new number). When nobody is in the foreground the
  tty still sets `intr`, so an old shell's `SYS_ttyintr` keeps working.
- **^C sends SIGINT to the foreground group, ^Z SIGTSTP**: every process of a pipeline (fixes problem 4).
- **Sessions, controlling terminals other than consoles, SIGHUP on hang-up** come with telnet's pseudo-terminal design
  (block 2), not here.

## 7. Compatible `kill`

- Old callers pass 0 in the second and third argument (`sys_kill(pid)` has been `sys3(24, pid, 0, 0)` in every version
  of `usys.h`). The **old form** (third argument 0) stays exactly as today: `proc_kill`, status 130, D11's second-kill and
  5-second rules (tests and the frozen wait statuses depend on it).
- The **new form** sets the third argument to 1; then the second is the signal number, and **0 means "does it exist?"**
  (POSIX `kill(pid, 0)`, which ports use constantly): no signal is sent.
- **Groups:** `kill(0, sig)` is the caller's group, `kill(-pgid, sig)` a group; `may_signal` applies to each member.

## 8. System calls (claimed in CLAUDE.md before building)

`SYS_evwait`, `SYS_sigaction` (a pointer to a struct with its size, under the "grow at the end" rule: three-argument
calls cannot carry handler, mask, flags), `SYS_sigprocmask`, `SYS_sigreturn`, `SYS_alarm`, `SYS_setpgid`, and the
compatible `kill` and `setfg` extensions. **Frozen from then on:** the signal numbers and default actions, the frame
layout and `sigreturn`'s contract, `poll`'s event bits (Linux values), the 128+n statuses.

## 9. Programs that change

- **sh:** a SIGINT and a SIGCHLD handler replace the `intr` polling (with `SA_RESTART`, so the line editor is not
  interrupted by every child's exit); each pipeline gets its own group (`setpgid`) and becomes the foreground group;
  `trap` for scripts, compared with bash.
- **kill, pgrep, timeout:** signal names and numbers (`kill -TERM`, `kill -0`).
- **less, edit:** restore the terminal on SIGINT/SIGTERM; on SIGTSTP restore it, stop, and redraw on SIGCONT.
- **cu:** may become one process with `poll` (kept as two until tested).

## 10. Tests

- **Host:** the event logic under every interleaving of producer and waiter (a model of the mask and `kwork`), no lost
  wake-up; slot masks with stale bits; `sigreturn`'s checks (a forged SR of 0x0700 or 0x1000 is masked, an odd PC kills,
  a frame outside the process is refused before it is read).
- **Emulator:** `poll` on a pipe, the tty and a timeout together; ^C runs a handler; **^C reaches every process of a
  pipeline**; SIGCHLD (and auto-reap with `SIG_IGN`); SIGALRM in a CPU-bound loop; SIGPIPE; `kill(pid, 0)` only probes; an
  old-form kill still gives 130; `SA_RESTART` on a tty read; a handler in a pure program (`a5`); a signal during a server
  call is delivered after it; nested signals masked; a stopped process and SIGCONT; `trap` in sh compared with bash; a
  signal arriving near the stack's end; every existing test unchanged.

## 11. Kernel size

The RAM-floppy kernel has 8008 bytes left (`make` prints it). The reviewer's prototype of the signal core alone is
about 1.2 KB at -O2; with `evwait`, the waiter masks, restart, groups, `alarm` and the blocking sites the estimate is
**3.5-4.5 KB**. It fits; the new code is built `-Os`. The network driver (block 2) and later work need room too: if the
measured size passes 4 KB, the next step frees room first (more cold files at `-Os`, measured).

## 12. Order of work

1. Event bits, slot masks and the Exec-order wait; `wake_io` and `nio` replaced; `SYS_evwait`; `poll`/`select` in libz;
   host and emulator tests.
2. Process groups (`pgid`, `setpgid`, the group foreground, ^C/^Z to the group).
3. Signals: the masks and entry point, delivery on both return paths (the level-2 entry saving all registers),
   `sigreturn` with its checks, `sigaction`/`sigprocmask`, compatible `kill`, SIGCHLD/SIGPIPE/SIGALRM, `SA_RESTART`.
4. Programs: sh (`trap`, handlers, groups), kill/pgrep/timeout, less/edit; docs; the frozen list in CLAUDE.md.

## 13. The first review's answers to the open questions

1. Event bits: fixed per process, no per-fd bits; `poll` rescans (section 2).
2. `SA_RESTART`: offered now (section 4).
3. Process groups: needed now (section 6); sessions and SIGHUP with telnet.
4. Size: 3.5-4.5 KB of the 8 KB left (section 11).

## 14. Exact rules from the second review (binding for the build)

1. **The wake rule:** a waker readies a process only if `(state == P_EV || (state == P_SLEEP && ev_wait)) && (ev_pending &
   ev_wait)`; `ev_wait` is 0 at every other time (plain `sleep`, ipc's timed receive). The masked test in step 3 of the
   wait also refuses to block when `killed`, `stop_req`, or a caught unblocked signal is pending. `proc_kill`,
   `proc_stop` and `sig_send` list the new `P_EV` state wherever they list `P_IO` today.
2. **`ready()` itself refuses `P_STOP`** (`proc_cont` and `proc_kill` use a forcing variant). This also fixes a bug that
   exists today: `proc_stop` readies a server sitting in receive, it becomes stopped inside `schedule`, but the port's
   `waiter` still names it and `ipc_enqueue` readies it, so a stopped server runs. Fixed in the build's first commit.
3. **Alarm:** one count of timers (sleepers plus alarms) gates the tick's scan, and the scan covers every live process
   with an alarm set. On both return paths `sig_deliver` first acts on `killed` (with `kill_now`: exit) and `stop_req`
   (schedule), then runs a handler, so a default SIGALRM ends a CPU-bound process.
4. **Restart is decided by a flag, not by the value -EINTR:** `sig_intr`, set only by a blocking site that gave up
   because of a signal, and only on the system-call path. Never restarted: `sleep`, `evwait`, a timed `ipc_recv` (a
   restart would start its deadline again). The service manager's waits (loading, the deadline, registration) are not
   interrupted by caught signals, only by a kill, like a server call (G4); otherwise a handler would make `open` or
   `spawn` fail while a server loads.
5. **`sigreturn`:** returns the frame's `d0` (the system call's result store would otherwise overwrite it); sets the
   live user stack pointer with a new `set_usp` (not `current->usp`, which `switch_to` only uses at a switch); finds the
   frame through the address the kernel passed to the entry in a register and libz passes back; the restored blocked
   mask never contains SIGKILL or SIGSTOP. **The frame:** aligned down to 4 bytes; refused if it would leave less than
   512 bytes above the stack guard for the handler ("stack overflow in a signal", killed). **The entry** is registered in
   the first `sigaction` (a field of its struct), checked there (even, inside the process by `user_ok_rd`), written in C
   or in assembler with only pc-relative branches and `a5`-relative data (D13: no relocation in the shared part); `a5`
   comes from `current->a5`.
6. **pid 1 is `boot`**, protected from default actions; the old form of `kill` may still end it (root only), as today.
7. **Process groups:** the tty stores whether its foreground is a single pid or a group (so a group's leader exiting
   first, as in `cat f | less`, does not take ^C away from the rest); ^C to a group with no living member sets `intr`;
   every process starts in a sensible group: a process with no parent (`boot`, the D11 servers) gets its own pid as
   pgid, and `login` and each interactive `sh` make their own group (else `kill 0` from a root shell would reach init and
   every login); the shell sets the foreground group only after every member joined; `setpgid` on a child that is already
   a zombie succeeds; `setpgid` only on oneself or one's own children.
8. **Frozen additions:** `ST_STOPPED` stays 0x114 for every stop, SIGSTOP included; getpriority's `PRIO_PGRP` keeps
   answering -EINVAL; a pgid for `ps` goes at the end of `uproc`.
9. **`SYS_jobctl`** (the shell's `fg`/`bg`) means SIGSTOP/SIGCONT, so SIGCONT handlers (the redraw in `less`/`edit`) run.
10. **SIGCHLD ignored:** automatic reaping in `proc_reap_orphans`, which treats "the parent ignores SIGCHLD" like "no
    parent"; `wait` then returns -ECHILD when nothing is left. **The shell installs no reaping SIGCHLD handler** (at most
    a flag): reaping in a handler would race with `fg_wait`, and printing would cut into the line editor and PS2.
11. **SIGPIPE:** `zup` ignores it (D15: `zup | less` and quitting `less` must not end an update half-way). The serial
    device and the ttys are reported always writable by `poll` (the tick empties their send rings).
12. **Stopped children of an exiting process** get SIGCONT and SIGHUP (the default ends them) in `proc_exit`, so they do
    not stay stopped for ever.
13. **Producers (the full list for the build), each setting the bit and readying under the mask:** `tty_input`,
    `tty_setmode`, `tty_inject`, `serial_work`, the pipes and `file_close`, the stop path in `schedule` and `proc_exit`
    (for the parent), `con_switch`, `ipc_enqueue`, `sig_send`, the alarm (inside `wake_sleepers`, masked). Not all run at
    the `kwork` points (pipes, kill, a child's exit or stop, messages and `sig_send` run in the producer's own system
    call); all are safe by the same rule. `proc_exit`'s own `wake_io` goes.
14. **Size:** the reviewers' prototype of the core is 2.3 KB at -Os; with the blocking sites, the tty/pipe/serial changes,
    the new calls and the level-2 entry, expect **4-5 KB of the 8 KB left**. The new code lives in its own `-Os` files
    (`signal.c`, `event.c`), with only the one-word "anything to deliver?" test inline on the return paths. If the
    measured size passes 4 KB, room is freed first (more cold kernel files at `-Os`, measured), as section 11 says.
15. **Group kill results:** 0 if at least one process got the signal, -EPERM if none was allowed, -ESRCH if the group is
    empty; `kill(-1, sig)` gives -EINVAL. Old programs that call `setfg(child)` then `setfg(0)` (man, time, nice,
    timeout) keep working and move to the group form. Noted for the shell's plan: sh does not stop a loop when its child
    died from ^C (status 130), unlike bash.

## 15. As built

Built on branch `d16` (2026-10-26), in this order:

1. **Rule 2, on its own:** `ready()` leaves a `P_STOP` process stopped; `proc_cont` and `proc_kill` end a stop through
   `ready_force` (`kernel/proc.c`). `sigtest stopsrv` (`tools/signal-test.sh`) stops a server in receive, calls it,
   and checks that the client still waits a second later and the server is still stopped; on the old kernel the server
   answered at once (checked: the check fails there).

Last checked against the code: 2026-10-26.
