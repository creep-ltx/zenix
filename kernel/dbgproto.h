/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* DESIGN D54 (docs/d54-debugger.md section 3, frozen): SYS_debug (100), debug(op, pid, arg). Shared by the kernel
 * (kernel/dbg.c) and the programs (user/usys.h: zdb, the tests). Every field is a big-endian 32-bit word. The structs
 * the kernel writes or reads start with size, the caller's buffer size: 0 means the first size (*_FIRST, frozen), a
 * smaller one is -EINVAL; they only grow at their end. */
#define DBG_ATTACH   1      /* 0: become pid's debugger and stop it */
#define DBG_DETACH   2      /* 0: breakpoints out, the trace bit cleared, a debugger's stop ended */
#define DBG_NEXT     3      /* pid 0: the caller's next spawn starts traced, stopped at its first instruction */
#define DBG_STOP     4      /* 0: stop it (a debugger's stop) */
#define DBG_WAIT     5      /* struct udbgstop *: until it is stopped or has ended */
#define DBG_CONT     6      /* a signal 1-31 sent to it first, 0: none */
#define DBG_STEP     7      /* as DBG_CONT, for one instruction (the trace bit) */
#define DBG_GETREGS  8      /* struct udbgregs * */
#define DBG_SETREGS  9      /* struct udbgregs *: d0-d7, a0-a7, pc (even), sr (only its condition codes) */
#define DBG_PEEK    10      /* struct udbgmem *: its memory into the caller's */
#define DBG_POKE    11      /* struct udbgmem *: the caller's memory into its */
#define DBG_BREAK   12      /* an address in its image: an ILLEGAL there */
#define DBG_UNBREAK 13      /* the address: the word back */
#define DBG_INFO    14      /* struct udbginfo * */

/* why it stopped (struct udbgstop.why) */
#define DBGW_ATTACH  1
#define DBGW_STOP    2
#define DBGW_EXEC    3      /* started under the debugger: at its entry */
#define DBGW_BREAK   4      /* pc: the breakpoint */
#define DBGW_STEP    5
#define DBGW_SIGNAL  6      /* info: the signal (SIGINT) */
#define DBGW_FAULT   7      /* info: the vector; addr: the fault address where the frame has one */
#define DBGW_JOBSTOP 8      /* SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU */
#define DBGW_EXIT    9      /* status: its exit status as wait gives it */

struct udbgregs { unsigned int size, d[8], a[8], pc, sr; };                 /* a[7]: the user stack pointer */
struct udbgmem  { unsigned int addr, len, buf; };                           /* len 1 .. DBG_MEMMAX */
struct udbgstop { unsigned int size, why, info, pc, addr, status; };
struct udbginfo { unsigned int size, base, span, code, csize, a5, shared; }; /* shared: other processes running its code */
#define UDBGREGS_FIRST 76
#define UDBGSTOP_FIRST 24
#define UDBGINFO_FIRST 28
#define DBG_MEMMAX   4096
#define DBG_ILLEGAL  0x4AFC /* the breakpoint instruction */
