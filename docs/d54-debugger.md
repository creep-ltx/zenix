# D54: the live debugger, `zdb`, and the kernel's part

Approved by the partner 2026-11-13 (docs/decisions.md, "Block 7's choices", item 2: "Yes" to a live debugger with
kernel support - stopping a process, single steps, breakpoints, a private copy of shared code - after a design note
with two independent reviews). Roadmap block 7, item 6 (docs/d52-self-hosting.md section 8 sketched it). A change to
the kernel's exception path, its signals and its loader: two independent design reviews before it was built
(section 20), one code review after (section 23). This is revision 2, after the reviews; built on branch `debugger`
2026-11-13 (section 22), checked in FS-UAE, not yet on a real Amiga.

## 1. In plain words

`zdb ./prog args` starts a program stopped at its first instruction; `zdb -p PID PROG` stops a running one of your
own. Then: `b main` (a breakpoint), `c` (continue), `s` (one instruction), `r` (the registers), `x ADDR` (memory),
`p counter` (a variable), `bt` (the calls that led here), `k` (kill), `q`. `zdb -c LOG PROG` reads a crash's register
lines from the kernel log (`dmesg > LOG`) after the fact. The kernel stops and resumes the program, reads and writes its
registers and memory for the debugger, and keeps the breakpoints, so that a debugger that dies leaves nothing behind.
Nothing of it costs anything on the hot paths while no debugger is attached (section 16).

## 2. What exists before

A crash prints `[kernel] pid N (name) killed: vector V pc=P addr=A (code+OFF)` and the process ends with 139
(architecture.md section 6); nothing stops a process for anyone but job control (`SIGSTOP`/`SIGTSTP`: `stop_req`,
`P_STOP`, `proc_cont`, D16). The kernel has no way to read another process's registers, no trace bit is ever set, and
`exc_c` (kernel/main.c) ends every user-mode exception with a kill. Without an MMU the kernel can read any process's
memory (D41's translation windows match every supervisor access: d41 section 3), so reading is easy; stopping,
resuming and the trace bit are new.

## 3. The kernel interface: one system call, `SYS_debug` (100)

A ptrace-like call, not a message protocol: every operation acts on one process the kernel holds stopped, needs the
caller's identity checked at each call, and must block (the wait) the way `wait` does; a port would add a server
and a copy for nothing. `debug(op, pid, arg)`: `d1` the operation, `d2` the target's pid, `d3` an argument (a number or
a pointer into the caller's memory, checked with `user_ok_rd`/`user_ok_wr`). Returns 0 or a value, or -errno.

| op | name | arg | what |
|---|---|---|---|
| 1 | `DBG_ATTACH` | 0 | become pid's debugger and stop it (section 5) |
| 2 | `DBG_DETACH` | 0 | its breakpoints taken out, the trace bit cleared, a debugger's stop ended (section 13) |
| 3 | `DBG_NEXT` | 0 | (pid 0) the caller's next spawn starts traced, stopped at its first instruction (section 11) |
| 4 | `DBG_STOP` | 0 | stop it (a debugger's stop) |
| 5 | `DBG_WAIT` | `struct udbgstop *` | block until it is stopped or has ended; -EINTR when a handler is due |
| 6 | `DBG_CONT` | signal | resume it; a signal 1-31 is sent to it first (0: none) |
| 7 | `DBG_STEP` | signal | resume it for one instruction (the trace bit, section 8); the signal as `DBG_CONT`'s |
| 8 | `DBG_GETREGS` | `struct udbgregs *` | its registers |
| 9 | `DBG_SETREGS` | `struct udbgregs *` | set them (the PC even; of SR only the condition codes) |
| 10 | `DBG_PEEK` | `struct udbgmem *` | copy from its memory into the caller's |
| 11 | `DBG_POKE` | `struct udbgmem *` | copy from the caller's memory into its |
| 12 | `DBG_BREAK` | address | a breakpoint there (section 9) |
| 13 | `DBG_UNBREAK` | address | take it out |
| 14 | `DBG_INFO` | `struct udbginfo *` | where its memory and code are |

Structs (all fields `u32`, big-endian as everything; they grow only at the end, the frozen rule for structs the kernel
writes into a program): each starts with `size`, the caller's buffer size; 0 means the struct's first size (a frozen
constant, `*_FIRST`, never `sizeof`), a size below the first size is -EINVAL, and the kernel reads or writes at most
`size` bytes.
- `struct udbgregs` (first size 76, `UDBGREGS_FIRST`): `size`, `d[8]`, `a[8]` (`a[7]` the user stack pointer), `pc`,
  `sr`. Room to grow: the FPU's registers, should the kernel ever save them (it does not today).
- `struct udbgmem` (read only by the kernel, 12 bytes, fixed): `addr` (the target's), `len` (1 .. 4096: larger copies
  are the debugger's loop), `buf` (the caller's).
- `struct udbgstop` (first size 24, `UDBGSTOP_FIRST`): `size`, `why`, `info`, `pc`, `addr`, `status`. `why`:
  `DBGW_ATTACH` 1 (stopped by `DBG_ATTACH`), `DBGW_STOP` 2 (`DBG_STOP`), `DBGW_EXEC` 3 (started under the debugger: at
  its first instruction), `DBGW_BREAK` 4 (a breakpoint; `pc` its address), `DBGW_STEP` 5 (one instruction done),
  `DBGW_SIGNAL` 6 (`info` the signal: `SIGINT`, section 6), `DBGW_FAULT` 7 (`info` the vector, `addr` the fault address
  where the frame has one: section 12), `DBGW_JOBSTOP` 8 (stopped by job control: `SIGSTOP`/`SIGTSTP`/`SIGTTIN`/
  `SIGTTOU`), `DBGW_EXIT` 9 (it ended: `status` its exit status as `wait` gives it). No sequence number: `DBG_WAIT`
  reports the stop it is in (section 5), and the debugger always waits after it resumed the target.
- `struct udbginfo` (first size 28, `UDBGINFO_FIRST`): `size`, `base`, `span` (its block: image or data part, BSS,
  stack, arguments), `code`, `csize` (a ZEX2 program's code; 0, 0 for a ZEXE program, whose code is in the block), `a5`,
  `shared` (the number of other processes running its code: section 10).

Errors: `ESRCH` no such process, or not traced by the caller; `EPERM` not allowed (section 4), or already traced;
`EBUSY` not stopped (`DBG_CONT`, `DBG_STEP`, the registers, `DBG_POKE` and the breakpoints need it stopped;
`DBG_STOP`, `DBG_WAIT`, `DBG_DETACH`, `DBG_INFO` and `DBG_PEEK` also work while it runs: a read cannot hurt it);
`EAGAIN` the table of traced processes is full (`NDBG` 4, the whole system: a debugger session is rare, and a fifth
waits for one to end); `EINVAL` an unknown op, an odd breakpoint or PC, a length outside 1..4096, a signal outside
0..31, a size below the first; `EFAULT` a buffer of the caller's, or an address range of the target's, outside its
memory; `ETXTBSY` a write into code other processes run (section 10); `ENOSPC` 16 breakpoints already (`NBP`);
`EEXIST`/`ENOENT` a breakpoint already there / not there; `EOPNOTSUPP` a step on Emu68 (section 8); `EIO` the saved
user frame does not look like one (section 7: a broken rule, nothing written).

## 4. Permissions

- **Whom:** your own processes only, root any. Precisely: the caller is root (uid 0), or the caller's effective and
  real ids are both the target's effective and real id **and the target is not marked `nodebug`**.
- **`nodebug`** (a sticky bit in the process's `dbgf` byte; only root may attach to such a process, and its crash
  lines leave out the registers, section 17): set by the loader when it honours a set-user-id bit, or when the
  program file is not readable by the caller's real id (owner, group or other read bit as the file's mode and ids say:
  a program you may run but not read cannot be dumped through the debugger); set by `SYS_setuid` whenever it changes
  an id (a set-user-id program that drops back to its real id still holds what it read as root). A new program starts
  without it (a spawn is a fresh image).
- **Never:** pid 0 and 1, a process with no parent (the built-in servers diskd, fsd, boot, a service's server: they
  are started by the kernel), a system process (D41 section 7's definition, `mmu_is_sys`: netd's drivers, a mounted
  filesystem's server, a reserved port's owner, a display grabber; stopping one would stall everyone, the debugger
  too), a process with `priv` (the raw disk), oneself, a process already traced (`EPERM`), one that is exiting or a
  zombie, and any process in the chain of the caller's own debuggers (the caller's debugger, its debugger, ...: no
  cycle of any length).
- **ZINE:** `ZINE_GO` (handing the machine to AmigaOS) from a traced process is refused with `EBUSY`: the exception
  vectors and the scheduler a debugger needs are gone during a session.
- **Started under the debugger** (`DBG_NEXT`): a set-user-id program is not honoured, it runs as the caller (as
  Linux under ptrace; otherwise `zdb /bin/su` would hand root's process to a user); a program file the caller cannot
  read is refused (`EACCES`). D53's scripts: the `#!` interpreter is the program traced. The bit is the **parent's**
  (`proc_load`'s `parent`), never `current`'s: the service manager spawns a server with no parent in its client's
  context (svc.c), which must never start traced.
- Every op after `DBG_ATTACH` checks that the caller is still the one recorded (`ESRCH` otherwise). Root has no
  further power over a process someone else traces (it may kill it, or the debugger).
- The debugger's own memory view is not widened: with D41 on it still sees only its own memory; the kernel copies.

## 5. Stopping and continuing

A traced process has a **debug block** (`struct dbg`, kernel heap, about 130 bytes: the debugger's pid, the target's
pid and slot, what it stopped for, where its registers are, 16 breakpoints). At most `NDBG` = 4 blocks exist at once
(the table `dbgs[]` and a count, 18 bytes of BSS). `struct proc` gets one byte, `dbgf` (bit 0 traced, bit 1 the next
spawn is traced, bit 2 `nodebug`, bit 3 held inside `exc_c`), which fits the 1-byte gap after `stop_rep` (m68k gcc
aligns `int` to 2): no BSS growth (the build's check of `sizeof(struct proc)` proves it).

Two kinds of stop, one state (`P_STOP`, so `ps`, `wait`, signals and the scheduler need no new state):
- **A debugger's stop** (`held`): attach, `DBG_STOP`, `DBGW_EXEC`, a breakpoint, a step, a fault, an intercepted
  `SIGINT`. Only `DBG_CONT`, `DBG_STEP`, `DBG_DETACH` and a kill end it. `SIGCONT` from anyone else does not (the
  check is in `sig_send`'s `SIGCONT` path, which then skips `proc_cont`), and the parent's `wait` does not report it
  (`WAIT_UNTRACED` skips a held child: a shell does not print "Stopped" and take the terminal back because a
  debugger stopped its job).
- **A job-control stop** (`SIGSTOP`, `^Z`): as today; the parent is told as today, and the debugger too
  (`DBGW_JOBSTOP`); `SIGCONT` ends it, and so does `DBG_CONT`. A job-control stop that arrives while the process is
  held is remembered in the block (`proc_stop` returns early on `P_STOP`, so it would be lost): `DBG_CONT`/`DBG_STEP`
  then turn the held stop into a job-control stop (the parent told, `DBGW_JOBSTOP`) instead of resuming it.

Where a process stops:
- **At its next return to user mode or while blocked in a system call:** `DBG_ATTACH`/`DBG_STOP` set `stop_req` (the
  `proc_stop` path, which already wakes a blocked process so that it notices) and the block's `req` (why). The stop
  itself is the existing branch at the end of `schedule()`, reached from `sig_deliver` or from `ev_block`; that branch
  calls `dbg_stopped(p)` when `p->dbgf` is set (in the branch only: not on the scheduler's path). A process in a
  message call stops after the reply, as today. A child that has never run stops at its first tick or system call
  (`switch_to` returns straight into `user_first_entry` for it), after some instructions: harmless.
- **Never while it loads a service** (`svc_depth` not 0, svc.c): the stop branch waits until the load is done (the
  service would stay "loading" and every client of it wait, whoever they are). This also closes the same hole for `^Z`,
  which exists today.
- **Never in `schedule()` while held inside `exc_c`** (`dbgf`'s bit 3, set by the hold, cleared just before `exc_c`
  returns): there the top of its kernel stack is the exception's block, not a system call's (section 7). The stop
  branch leaves `stop_req` set for `exc_c`, which holds the process again with its known block (`DBGW_STOP`,
  `DBGW_SIGNAL` or `DBGW_JOBSTOP` as `req` says). Review finding 1 (section 20).
- **In the exception path** (new): a breakpoint, a step and a fault hold the process inside `exc_c` (`dbg_hold`):
  state `P_STOP`, the debugger told, `schedule()`; when resumed (the block's register pointer cleared), `exc_c` returns
  and `exc_common` (start.S) restores the registers and ends with `rte` (until now it never returned). That return
  skips `kwork`, `resched` and `sig_deliver`: they run at the next tick or system call, at most 10 ms later. **A kill
  while held ends it there at once** (`proc_exit(kill_status)` straight after the hold, not `schedule()`'s
  `kill_now` rule, which lets a user server finish a request: a user-mode exception is never inside a message call).
- **Already job-stopped at attach:** made held at once (`DBGW_ATTACH`).

The debugger is told by `EV_CHILD` on **the debugger** (the stop branch and `proc_exit` post the parent today; they
post the debugger too). `DBG_WAIT` goes in the order `proc_wait` uses: clear `EV_CHILD` first, then check the target -
ended (the block is an exit record, section 14): report it and free the block; stopped: report it (again on every call
while it stays stopped: the call is idempotent) - and only then `ev_block(EV_CHILD)` and loop. A stale bit makes one
extra round, never a lost wake-up.

## 6. Signals

- **`SIGINT` to a traced process becomes a debugger's stop** (`DBGW_SIGNAL`, `info` 2) and is dropped, in
  `sig_send`. `^C` at the terminal reaches every process of the foreground group: the program started by `zdb` is in
  zdb's group, and without this rule `^C` would end the program being debugged instead of stopping it (gdb's default:
  `SIGINT` stops, is not passed on). zdb catches `SIGINT` itself (else `^C` would end zdb, and with it the program it
  started). `DBG_CONT 2` passes one on when wanted.
- Every other signal does what it does without a debugger: a handler runs, a default action ends or stops the
  process (the debugger then sees `DBGW_EXIT` or `DBGW_JOBSTOP`). `SIGKILL` always ends it, held or not.
- **`sigreturn` keeps the trace bits** of the status register it returns to (the kernel's own saved SR, not the
  user's frame; only a debugger ever sets them): before, it took only the condition codes and cleared T1, so a step
  through a handler's return ran free. Programs see no difference (`SYS_sigreturn` still takes only the condition codes
  from their frame).

## 7. Registers

A stopped process's user registers are in one of two places, both on its kernel stack:
- **Held in `exc_c`:** the block `exc_common` saved (d0-d7, a0-a6, then the CPU's frame: SR, PC, format word), its
  address kept in the debug block (`rb`) while held, cleared on every resume.
- **Stopped in `schedule()`** (from a system call, from the level-2 interrupt that cut into user code, from `ev_block`
  or `sig_deliver`): the block `trap0_entry` or `irq2_entry` saved. Both enter from user mode with a format-0 frame of
  8 bytes on an empty kernel stack (in user mode the supervisor stack pointer is always `ktop`; the kernel never uses
  the master stack), so the block is at `ktop - 68`. A process held in `exc_c` never stops here (section 5). The kernel
  still checks the frame before trusting it: format nibble 0 and the S bit clear in the saved SR; else `EIO`.

`a7` is the user stack pointer the switch saved (`p->usp`; `switch_to` loads it back). `DBG_GETREGS` copies them out;
`DBG_SETREGS` writes d0-d7 and a0-a6 into the block, `a7` into `p->usp`, the PC (even, else `EINVAL`) into the frame,
and of SR only the condition codes (bits 0-4); the trace bits and the upper byte are the kernel's. A PC written clears
`sig_intr`, so an `SA_RESTART` restart (`sig_deliver`'s `pc -= 2`) never lands beside the new PC. **A process stopped
inside a system call** (blocked in a read, say) shows d0 as the call's number and its PC after the `trap #0`; a d0
written while it is blocked is overwritten by the call's result (one written after the call stored its result, a stop
in `sig_deliver`, stays): zdb's manual page says "d0 inside a system call is the kernel's".

## 8. Single steps: the trace bit

`DBG_STEP` sets **T1** (SR bit 15) in the saved SR and resumes; after one user instruction the CPU takes the trace
exception (vector 9, a format 2 frame on every CPU from the 68020 to the 68060: MC68020UM 6.1.7, MC68060UM 8.2.6) and
`exc_c` holds it (`DBGW_STEP`). Every hold clears T1 and T0 in the saved SR, so a resume after a step runs free.
- **T0** (trace on change of flow) is not used: the 68060 does not implement it (MC68060UM 11.1.2.4); one rule for
  all CPUs.
- **A step over a group 2 exception** (`TRAP #0` - a system call - `TRAPV`, `CHK`, `TRAPcc`, a division by zero): the
  68020's manual says both that the trace comes right after the trap's processing, in supervisor mode at the handler's
  first instruction (MC68020UM 6.1.11's priority example), and that it comes after the handler's `rte` (6.1.4); the
  68040's 8.2.6 reads as the first; the 68060 takes none and traces the instruction after the trap instead (MC68060UM
  8.2.6). The kernel handles every reading: **a trace exception in supervisor mode whose frame PC is `trap0_entry` or
  `exc_common` (the only handlers a user-mode group 2 exception enters) returns at once, whoever is `current`**; any
  other supervisor-mode trace stays a panic. **A trace exception in user mode in a process that is not traced** clears
  T1/T0 in its frame and returns (programs cannot set T1; only a debugger that went away, mid-step, leaves one). So a
  leftover T1 can neither panic the kernel nor kill a program. **zdb never steps a `TRAP` instruction** (opcode
  0x4E40-0x4E4F): it puts a temporary breakpoint after it and continues, so a step over a system call stops right after
  it on every CPU.
- **An interrupt** during a step: its handler runs with T cleared and its `rte` restores the user's SR with T1: the
  instruction then executes and traces (MC68020UM 6.1.7: trace before interrupt).
- **A signal** delivered at the end of a stepped system call sends the process to its handler with T1 kept: the step
  stops at the handler's first instruction (zdb says where it is).
- **Other processes** never see T1: it lives in this process's saved SR only.
- **Emu68** (the PiStorm): nothing in its source tests T1 or raises vector 9 (read 2026-11-13: only
  `src/ExecutionLoop.c` and `src/M68k_ExceptionEntry.c` touch the trace bits, to clear them at an exception), so T1
  would be ignored and a "step" would run free. `DBG_STEP` answers `EOPNOTSUPP` there (the hardware table's `HW_DT`
  flag, as ZINE's `UZI_EMU68`); breakpoints work (ILLEGAL raises its exception on Emu68 too, and the write is followed
  by the cache flush that drops its translations). zdb then continues from a breakpoint by taking it out (and says so).

## 9. Breakpoints

- **The instruction: `ILLEGAL` (0x4AFC)**, vector 4, on every CPU. Not `BKPT`: it runs a breakpoint-acknowledge bus
  cycle that the motherboard or accelerator must terminate (MC68060UM 7.x, "Breakpoint Acknowledge"; on a board that
  does not, the CPU waits); not `TRAP #n`: one more vector to own, and no smaller. 2 bytes, the smallest instruction,
  so it never covers more than the instruction it replaces.
- **Only in the program's image:** a ZEXE program's block below its stack (`base` .. `ustk`: the image and BSS), a
  ZEX2 program's own code copy (`code` .. `code + csize`). Not a `memalloc` region: the program may free it, someone
  else's block may then lie there, and a write-back would land in it. The image lives exactly as long as the process,
  and a write-back happens only while it lives (an exit record never writes).
- **The kernel keeps them**, 16 per process (`NBP`): `DBG_BREAK` checks the address (even, inside the image), saves the
  original word, writes 0x4AFC and flushes (`cache_flush_code()`: CACR's CI on 020/030, `cpusha bc` on 040/060 - never
  `cinva` - and Emu68's translations); `DBG_UNBREAK` and detach write the word back and flush the same way. So a
  debugger that dies cannot leave an `ILLEGAL` in a program that goes on.
- **`DBG_POKE`** into the image updates the saved word of a breakpoint it covers (the program's word changes, the
  `ILLEGAL` stays), and flushes when it touched the image or the code copy (a ZEXE program's code and data share one
  block: always, for a poke there; rare and cheap).
- **A hit:** vector 4 in user mode in a traced process whose PC is one of its breakpoints holds it (`DBGW_BREAK`, PC
  the breakpoint). An `ILLEGAL` that is not one of its breakpoints is a fault (section 12).
- **Continuing from one** is the debugger's: take it out, step, put it back, continue (zdb does this; on Emu68 it
  takes it out for good, section 8). The kernel does not hide its breakpoints from `DBG_PEEK` (zdb knows them and
  shows the original word).

## 10. Shared code (ZEX2)

A ZEX2 program's code is one copy (a segment, D13) for every process running it; an `ILLEGAL` written there would stop
every one of them (and the untraced ones would die of it). Steps, registers and reads need no write and work on
shared code; only writes into code (breakpoints, `DBG_POKE` there) need a copy of the process's own:
- **Started under the debugger** (`DBG_NEXT`): the loader gives it a private copy: `proc_load` passes "do not share"
  to `seg_find` and `fill_v2` (a flag of its own, set before `seg_find`, not `exec_src.wopen`), which then neither take
  a listed segment nor list the new copy; the copy is the process's, freed with it, as today's private copy of a file
  open for writing. Other processes starting the same program keep using (or making) the shared one.
- **Attached:** a segment only this process runs (`refs` 1: references are taken before a spawn can block, so a spawn
  still loading counts) is taken out of the table at the first write into code (`seg_disown`: the slot freed, the code
  block now owned by the pid, `p->seg` 0); the same program started later loads a fresh shared copy. A segment others
  run too stays shared: writes into it answer `ETXTBSY` (zdb: "its code is shared with N other processes, `refs - 1`:
  start it under zdb to set breakpoints"). A private copy cannot be made for a running process: return addresses on
  its stack and function pointers in its data and heap point into the shared copy and cannot all be found.
- **The proof** (section 21): copy B started first and held at a pipe; A started under zdb with a breakpoint in the
  function B runs; A's `shared` 0 and its code address not B's; B released, runs that function in a loop past the
  breakpoint and ends with its normal output and status; C started next shares B's segment (`free`'s shared-code
  count).

## 11. Starting a program under the debugger

`zdb PROG ARGS` calls `DBG_NEXT`, then an ordinary `spawn`. The loader sees the **parent's** bit and clears it at once
(any spawn attempt takes it: one that fails leaves nothing behind for a later `system()` or pager), then: a ZEX2
program gets a private copy (section 10), a set-user-id bit is not honoured and an unreadable file refused (section
4), a debug block is made for the child (`EAGAIN` when `NDBG` blocks exist), and **a breakpoint at its entry** (the
`bra.w` at offset 0 of its code) is written before the loader's cache flush. Its first instruction is that `ILLEGAL`:
`exc_c` takes the word back out (flush) and holds it with `DBGW_EXEC`, PC = the entry. This works on every CPU and on
Emu68 (the trace bit would not). The child is zdb's child, as any.

## 12. Faults in a traced process

A user-mode exception that is not a breakpoint or a step (a bus or address error, an illegal instruction, a privilege
violation, line A or F, a division by zero, CHK, TRAPV, D41's protection fault) holds the process **first**, before
the kernel's "killed" line, `mmu_fault`'s line and `seg_crash` (`DBGW_FAULT`, `info` the vector, `addr` the fault
address as `exc_c` reads it today): the debugger sees the registers, the memory and the stack at the moment of the
crash. When it is resumed (continue, step or detach) or the debugger dies, it ends exactly as without a debugger: the
kernel's lines, 139 (a kill while held: 128 + the signal, as any kill). A fault is never re-run (a 68020 bus error
frame's `rte` would rerun the faulted cycle, not the instruction; one rule for every frame). **Not held:** a fault
whose stack check says the kernel stack overflowed (`STK_KERNEL`): killed at once as today, without the register lines
(its kernel stack cannot be trusted to hold a stop).

## 13. Detach, and when the debugger dies

`DBG_DETACH`: every breakpoint's word back (flush), T1/T0 cleared in the saved SR - also for a target that is not
stopped (blocked in a system call after a step was asked: the block at `ktop - 68`, checked as in section 7; a target
held in `exc_c` has its own) - a held stop ended (it runs on), a job-control stop left as it is; the block freed. **The
debugger ending** (`proc_exit`, also a crash or a kill): every process it traces is detached the same way, except one
it started (`DBG_NEXT`), which is killed (`SIGKILL`'s status, 137) as gdb does with a program it started: a program
started for debugging is not left running without its debugger. A held fault ends with 139 as usual (section 12).
Should a T1 survive anyway, section 8's rule clears it harmlessly. The cost in `proc_exit`: one test of a global
count (`ndbg`) and of the byte `dbgf`.

## 14. When the target ends

`proc_exit` of a traced process (any way: exit, kill, fault) tells the debugger (`EV_CHILD`) and turns its block into
an exit record (target slot cleared, `DBGW_EXIT`, the status); the next `DBG_WAIT` for that pid reports it and frees
the block (the zombie may already have been reaped by its parent: the record is the debugger's). A record still there
when the debugger ends is freed then.

## 15. D41 memory protection and the MMU

- The kernel's own accesses never use the translation tree (d41 section 3: the transparent windows match every
  supervisor access, mmu.c), so peeks, pokes, breakpoint writes and register access work the same with protection on
  or off, whoever the tenant is; no view is lent, and the debugger's own view is not widened.
- A protected process's code is read only for user mode (`D_WP`): a breakpoint is a supervisor write, allowed; the
  pages stay read only for the program itself.
- A private code copy (started under zdb, or disowned) is a `MEM_USER` block the MMU flips through `p->code`,
  `p->csize` as it does a segment; `seg_disown` changes nothing the MMU looks at.
- D41's protection fault in a traced process is a fault stop (section 12): the debugger sees where it went.
- System processes (D41 section 7) cannot be debugged (section 4).

## 16. The cost with no debugger attached

Zero on the hot paths: the system call entry and exit, the level-2 interrupt, `schedule()`'s path, the task switch,
`ev_block`. What is added runs only where it is not hot:
- `exc_c` (only for an exception): a byte test before the kill;
- the stop branch at the end of `schedule()` (only when a stop happens): a byte test and the `svc_depth` test;
- `sig_send` (a signal): a byte test for `SIGINT`, one for `SIGCONT`; `proc_stop`: a byte test when already stopped;
- `proc_load` (a spawn): a byte test of the parent's `dbgf` (two instructions against a file read);
- `proc_exit`: a test of `ndbg` and `dbgf`; `proc_wait` (`WAIT_UNTRACED`): a byte test;
- `sig_return` (a handler's end): keeping two bits of the old SR (two instructions);
- `SYS_setuid`: setting a bit;
- `exc_common`: its `rte` path is new code but runs only when `exc_c` returns (the debugger's cases);
- `spawn1`: `dbg_readable` for the `nodebug` bit (a few compares beside the file's read); `mmu_sysmark`: a byte test.
Memory: 18 bytes of BSS for the table and its count; no byte per process slot; a debug block only while debugging.
The kernel code: estimated about 2.5 KB (`kernel/dbg.c` at -Os, the hooks, the crash lines), measured at the build
and recorded in tools/ksize-budget; the mount tests' RAM disk gives the room on their floppy, as for D49 and D53.

## 17. After a crash: the register lines and `zdb -c`

Today's crash line is kept word for word (tests and people read it). Two lines follow it for every user-mode crash
of a process not marked `nodebug` (section 4) and not root's (the kernel log is readable by everyone; `login` reads
passwords as root: code review finding 8) whose kernel stack is sound:

```
[kernel] pid N regs: base B code C sr S pc P d0..d7 X X X X X X X X a0..a7 X X X X X X X X
[kernel] pid N frames: K R1 R2 ... RK
```

`base`/`code` place the program (a ZEXE program's code is at `base`; a ZEX2 program's at `code`, its data part at
`base`); `frames` is a count K, then up to 8 return addresses read by walking the frame-pointer chain (`a6`): each
frame's 8 bytes (`a6` .. `a6 + 8`) checked to lie in the process's own block, `a6` even and above the last, so a
garbage `a6` stops the walk. The lines are printed by several small `kprintf` calls (no 20-argument call on a kernel
stack that just took a fault). Words may only be added at the end of each line. `zdb -c LOG PROG` (LOG a file, `-`
standard input: `dmesg | zdb -c - ./prog`) finds the last crash of PROG's name in the log and prints the vector's
name, the PC and every frame as `symbol+offset` and the registers. The lines go to the kernel log, so they are in the
previous-boot lines after a reboot too. A frame pointer exists only where the code keeps one: tcc's code always does
(`a6`, d52 section 7); gcc's at -O2 does not (`-fomit-frame-pointer` is its default): build with
`-fno-omit-frame-pointer` for a full chain. Live, zdb's `bt` also offers a scan of the stack for words that point
just after a `jsr`/`bsr` in the code (marked "scan": a guess, not a chain) when the chain is short.

## 18. `zdb`

A small program (target under 1,000 lines of C over libz), `/bin/zdb` in the `devel` package. Commands (one or two
letters, gdb's where they exist): `b LOC` break, `d LOC` delete, `i` breakpoints, `c` continue, `s [N]` step, `n`
(step, but over a `jsr`/`bsr`: a temporary breakpoint after it), `r` registers, `x LOC [N]` N long words, `p LOC` the
long word at a symbol (as hex and decimal), `set LOC VALUE` write a long word, `bt` backtrace, `k` kill, `detach`,
`q` (kills a program it started, detaches from one it attached to). `LOC`: a symbol, `symbol+0xOFF`, an address
`0x...`, or a register `$pc`, `$a6`, `$sp`. Output is plain lines a test can compare. zdb catches `SIGINT`: during a
`c` it stops the program (`DBG_STOP`), at the prompt it does nothing.

**Symbols** come from a map file in `nm -n`'s format (`OFFSET T NAME`, offsets from the program's start: programs
are linked at 0): `PROG.map` beside the program, or `-m FILE`. zdb takes only lines of defined symbols (`T t D d B b R
r`), skipping `U`, `A`, `w` and the rest. The PC's build writes one with `m68k-elf-nm -n build/NAME.celf`; on Zenix
`zld -M FILE` writes the same (a new zld option, its defined, non-absolute symbols only, so `tcc -o prog
-Wl,-M,prog.map` gives one: tcc already passes `-Wl,` options to zld). zdb reads the program file's header for its kind
(ZEXE, or ZEX2 and `text_size`) and places a symbol: ZEXE at `base + offset`; ZEX2 at `code + offset` below
`text_size`, at `base + offset - text_size` above. Without a map, addresses only (and `code+OFF` as the crash line).
**Not in stage 1:** line numbers (no DWARF reader: too big for its use now), a disassembler (zdb prints the
instruction words; d52's 1,500-line disassembler stays a later step), watchpoints, following a program's children.

## 19. Frozen and claimed

Claimed in docs/claims.md: system call 100 `debug`, its ops 1-14 and their argument meanings, `DBGW_*` 1-9, the
structs' first sizes (`UDBGREGS_FIRST` 76, `UDBGSTOP_FIRST` 24, `UDBGINFO_FIRST` 28, `udbgmem` 12), `NDBG` 4 and `NBP`
16 as minimums. Frozen (docs/frozen-interfaces.md): those; the rules of sections 4 (who may, `nodebug`), 6 (`SIGINT` to
a traced process stops it; `sigreturn` keeps T1/T0), 8 (a stray trace exception is harmless), 9 (`ILLEGAL` is the
breakpoint, only in the image), 11 (stopped at the entry, PC = the entry; set-user-id not honoured), 12, 13 (detach,
the debugger's death) and 14; the two crash lines' format (section 17: zdb reads them; words may be added at the end,
never moved). `zld -M`'s format is `nm -n`'s.

## 20. The design reviews (two, independent, 2026-11-13, on revision 1)

**Review 1 (the 68k mechanics).** Found, all taken into revision 2:
1. A held process resumed and stopped again (by `^Z`, `DBG_STOP` or `SIGINT` before it ran) would stop in
   `schedule()`'s branch while its kernel stack's top is an exception block (a trace frame is 12 bytes, a fault frame
   more), so the `ktop - 68` rule read garbage and a `SETREGS` could write over the format word: a kernel crash at the
   `rte`. Now: the "held in `exc_c`" bit; the branch leaves the stop to `exc_c` (section 5).
2. A T1 left by a step whose target went back to blocking, then detached or orphaned, would kill the program, or on a
   020/030/040 panic the kernel through a supervisor-mode trace (a user could cause it). Now: section 8's two rules and
   detach clearing T1 in a blocked target (section 13).
3. A breakpoint in a `memalloc` region could be written back into someone else's memory after the region was freed.
   Now: the image only (section 9).
4. A kill while held went through `kill_now`, which lets a user server finish a request: it could loop on its
   breakpoint. Now: `proc_exit` straight after the hold.
5. `DBG_WAIT`'s order and the debugger's own `EV_CHILD` were not stated (a spin or a lost wake-up). Now stated.
6. A PC written while stopped in a system call could meet an `SA_RESTART` restart. Now `sig_intr` is cleared; the d0
   text corrected.
7. `DBG_NEXT` must be the parent's bit, never `current`'s (the service manager's spawns). Stated.
8. A stop inside a service load (`svc_depth`) stalls the service for everyone (also with `^Z` today). Now waited out.
9. A job-control stop arriving while held was lost. Now remembered and applied at the continue.
10-14. The two readings of the 68020 manual on TRAP and trace (the kernel rule covers both), the Emu68 reason
    reworded, an attach to a child that never ran stops after a few instructions, the hold before the crash lines,
    the skipped `kwork` on the `rte` path: all stated.
Found sound: the format-0 frame of `TRAP #0` and interrupts on every CPU, `ktop` in user mode, `ktop - 68` for every
`schedule()` stop path, message calls excluded, the entry breakpoint and its flush, the format 2 `rte`, faults never
re-run, attach without races (the kernel is not preempted), `sigreturn` keeping T1 from the kernel's frame.

**Review 2 (interfaces, security, shared code, D41).** Found, all taken into revision 2:
1. `udbgregs` had no size: an FPU could never be added. Now every struct starts with `size`, 0 meaning the first size.
2. Changed credentials leaked memory: a set-user-id program that dropped back to its real id, a program file without
   read permission dumped through `DBG_NEXT`, and the crash lines printing a set-user-id program's registers into a log
   anyone reads. Now the sticky `nodebug` bit (section 4) and an `EACCES` for an unreadable file under `DBG_NEXT`.
3. System processes with a parent (netd's drivers, a mounted filesystem's server, a reserved port's owner, a display
   grabber) were not refused. Now `mmu_is_sys`.
4. `DBG_POKE` into code had no flush and would leave a breakpoint's saved word stale. Fixed (section 9).
5. `DBG_NEXT`'s bit outlived a failed spawn (a later `system()` would start traced). Now any spawn clears it.
6. A signal on resume: `DBG_CONT`/`DBG_STEP`'s argument is now the signal to send (0 none), so no later op is needed.
7. The service-load stop (as review 1's 8).
8. The crash path when the kernel stack overflowed: no hold, no register lines; small `kprintf` calls; the frame walk
   checks `a6 .. a6 + 8`.
9. The shared-code test could pass without proving anything. Now section 10's proof, and more tests (section 21).
10. zdb must catch `SIGINT`. Stated.
11. Nits: `EPERM` for "already traced" so `EBUSY` means only "not stopped"; cycles of any length refused; the frames
    line's count first; `nm`'s `U`/`A` lines skipped; the BSS figure; the size estimate raised to 2.5 KB. Kept as it
    was: `NDBG` 4 for the whole system (no per-user cap: four debugger sessions at once is already more than this
    machine sees).
Found sound: system call 100 free, the errnos, the `dbgf` gap, the private copy for a program started under the
debugger, `seg_disown` with `refs` 1 (D41 uses only `p->code`/`csize`, `uproc.mem` already counts a private copy,
`seg_crash` skipped), pokes cannot reach kernel memory or a segment, a breakpoint cannot reach another process's code,
the D41 windows, ZINE, the ZEX2 symbol placement, tcc passing `-Wl,` options.

## 21. Tests

- `tools/zdb-test.sh` (FS-UAE, the A1200 with 4 MB of fast RAM, a 68020): a test program `dbgtest` (a loop over a
  global counter, a function a few calls deep built with a frame pointer, a crash on request), its map from the PC's
  `nm -n`, driven through zdb by a script: start under zdb, break at a function by name, continue to it, read the
  counter by name and check its value, step and see the PC move by the instruction's length, a backtrace naming the
  callers, set a variable and see the program print the new value, continue to the end (status), kill another run;
  attach to a running copy, stop it, detach and see it run on; section 10's shared-code proof with the ZEX2 build of
  `dbgtest`; attach to a ZEX2 program that shares its code: a breakpoint refused with `ETXTBSY`, then (the other copy
  ended) the same attach disowns the segment and the breakpoint works, no "DAMAGED" line at its end; the debugger
  killed while its program is held at a breakpoint: an attached program runs on (its breakpoint gone), a started one
  ends with 137; `zdb -c` on a crash's lines names the function and the frames.
- `dbgcheck` (a test program making the raw calls, in the same run): another user's process and pid 1 refused
  (`EPERM`), a `nodebug` process refused for a user, a set-user-id program under `DBG_NEXT` runs as the caller, a
  `DBG_POKE` at a kernel address and into another process's block `EFAULT`, a fifth traced process `EAGAIN`, a raw
  `DBG_STEP` on a `TRAP #0` (the supervisor-mode trace must not panic), a step left pending when the debugger detaches
  from a blocked target (the program runs on, nothing killed), `SIGCONT` from the shell not ending a held stop.
- Stepping on the 68060 too: the same step checks and `dbgcheck`'s TRAP step on an A4000 with a 68060 (FS-UAE).
- D41: the breakpoint and a fault stop with memory protection on (the `mmu-test` machine).
- The core tests: `svc-test`, `signal-test` (`sigreturn`, stops, `SIGCONT`), `memtest`, `mmu-test`, `cmds-test`,
  `pure-test`, and `usize-test`.

## 22. As built (2026-11-13)

- **Kernel:** `kernel/dbg.c` (4,030 bytes at -Os, 17 of BSS), the hooks in `proc.c` (the loader's start under a
  debugger, the stop branch, `proc_stop`, the exit and the wait), `main.c` (`exc_c`), `start.S` (`exc_common`'s
  `rte`), `signal.c`, `syscall.c` (the call, `setuid`'s `nodebug`), `seg.c` (`seg_disown`), `zine.c`. kernel.bin
  199,608 -> 204,464 bytes (+4,856, about twice the estimate: `dbg_sys`'s 14 operations alone are 1.9 KB); the mount
  tests' RAM disk 683 -> 678 KB for the floppy's 800-byte room (tools/ksize-budget). `struct proc`'s size unchanged.
- **zdb:** `user/zdb.c`, 21.9 KB of image (packaged in `devel`); `zld -M` writes the map. The test programs
  `user/dbgtest.c` (built with `-fno-omit-frame-pointer -fno-optimize-sibling-calls`: gcc's sibling calls leave no
  frame) and `user/dbgcheck.c` go on the test image only (tools/zdb-test.sh).
- **Found while testing:** gcc's prologue with a frame pointer at -O2 is `move.l a6,-(sp); move.l sp,a6`, not `link`;
  at a function's first instruction its caller's return address is on top of the stack (zdb's `bt` reads it there),
  and zdb must remember the words under its own breakpoints (the kernel shows `ILLEGAL` there).
- **Tests:** `tools/zdb-test.sh` on the A1200 with 4 MB of fast RAM (68020), on a 68060 and on a 68030 with memory
  protection on: 41 checks each (section 21) and dbgcheck's 19; `tools/zld-host-test.sh` (`-M`'s map is nm's, the
  program unchanged); the core tests (section 21). **Verified in FS-UAE 2026-11-13; not on a real Amiga yet** (the
  first run there: a step on the stock A1200 and on the 1260).

## 23. The code review (one, independent, 2026-11-13)

Found, all fixed before the merge:
1. A process job-stopped inside `exc_c` with no debugger (held, resumed, `^Z` before it ran, then detached) could be
   attached again: its registers would have been read at `ktop - 68`, where an exception block does not start (a
   panic at the `rte` once a PC was written). `DBG_ATTACH` now refuses a process held inside `exc_c` (`EBUSY`).
2. zdb's `-c` trusted every field of a log line it matched: a line missing one read near address 0. Each field is
   now found or the line skipped; the frame count is bounded.
3. `DBG_NEXT`'s bit was taken only by `proc_load`: a spawn that failed earlier (no such file, no x bit, a script's
   missing interpreter) left it for a later `system()`. `proc_spawn` now takes it on every path.
4. A detach while the debugger's stop was still asked left `stop_req`: the program then stopped as a job, and the
   shell said "Stopped". Detach now takes the request back.
5. A stale `req` (a `SIGINT` to a job-stopped process, a `SIGCONT` from the shell before the debugger's stop was
   taken) made the next `^Z` a debugger's stop the shell never heard of. `SIGINT` sets it only on a running process;
   `SIGCONT` leaves a process with a debugger's stop asked.
6. A traced process that becomes a system process (a mount's server, a reserved port, the display) could still be
   stopped by its debugger. `mmu_sysmark` now detaches it.
7. Tests that could pass while broken: the started program in the debugger's-death check ended by itself (now a
   loop), "it runs on" was matched by any attach (now `ps`'s state), a bare `0` line, dbgcheck's count at least 16
   (now exactly 19).
8. Smaller: `SETREGS` read `sizeof` instead of the caller's size; a poke over the entry's breakpoint was undone;
   zdb's `c` lost a breakpoint and a stop when the step over it ended elsewhere (now said, and the breakpoint put
   back); an unchecked `strdup`; the crash lines also left out for root's processes (the log is everyone's: `login`
   reads passwords as root), so `zdb -c` is for programs a user runs. Kept: Zenix has one group id per process (no
   real one), so `dbg_readable` uses it; `dbg_readable` runs at every spawn (a few compares beside the file's read,
   not behind a byte test).
Found sound: the caller's pointers and sizes (each struct copied once), the lifetimes of the blocks and slots, the
order of `DBG_WAIT`, the frame offsets and the `ktop - 68` check, the cache flushes, the permission chain, nothing
new on the hot paths.
