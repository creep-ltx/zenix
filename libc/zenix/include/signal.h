/* SPDX-License-Identifier: GPL-2.0-only */
/* Signals with Zenix's numbers (DESIGN D16, Linux's; picolibc's own signal.h has newlib's: SIGUSR1 30 there, 10
 * here), from the kernel's headers through the generated zenix-consts.h (DESIGN D27 section 5). The calls are the
 * ones libz has (D16): sigaction with a handler, a mask and SA_RESTART, sigprocmask, kill, raise, alarm, pause. */
#ifndef _SIGNAL_H_
#define _SIGNAL_H_
#include <sys/cdefs.h>
#include <sys/_types.h>
#include <zenix-consts.h>
_BEGIN_STD_C
#define SIGHUP   ZX_SIGHUP
#define SIGINT   ZX_SIGINT
#define SIGQUIT  ZX_SIGQUIT
#define SIGILL   ZX_SIGILL
#define SIGTRAP  ZX_SIGTRAP
#define SIGABRT  ZX_SIGABRT
#define SIGIOT   SIGABRT
#define SIGBUS   ZX_SIGBUS
#define SIGFPE   ZX_SIGFPE
#define SIGKILL  ZX_SIGKILL
#define SIGUSR1  ZX_SIGUSR1
#define SIGSEGV  ZX_SIGSEGV
#define SIGUSR2  ZX_SIGUSR2
#define SIGPIPE  ZX_SIGPIPE
#define SIGALRM  ZX_SIGALRM
#define SIGTERM  ZX_SIGTERM
#define SIGCHLD  ZX_SIGCHLD
#define SIGCONT  ZX_SIGCONT
#define SIGSTOP  ZX_SIGSTOP
#define SIGTSTP  ZX_SIGTSTP
#define SIGTTIN  ZX_SIGTTIN
#define SIGTTOU  ZX_SIGTTOU
#define SIGWINCH ZX_SIGWINCH
#define NSIG     ZX_NSIG
#define _NSIG    ZX_NSIG
typedef int sig_atomic_t;
typedef void (*_sig_func_ptr)(int);
typedef _sig_func_ptr sighandler_t;
#define SIG_DFL ((_sig_func_ptr)0)
#define SIG_IGN ((_sig_func_ptr)1)
#define SIG_ERR ((_sig_func_ptr)-1)
#ifndef _SIGSET_T_DECLARED
typedef unsigned long sigset_t;
#define _SIGSET_T_DECLARED
#endif
#ifndef _PID_T_DECLARED
typedef __pid_t pid_t;
#define _PID_T_DECLARED
#endif
struct sigaction { _sig_func_ptr sa_handler; sigset_t sa_mask; int sa_flags; };
#define SA_RESTART  ZX_SA_RESTART
#define SIG_BLOCK   ZX_SIG_BLOCK
#define SIG_UNBLOCK ZX_SIG_UNBLOCK
#define SIG_SETMASK ZX_SIG_SETMASK
static __inline int sigemptyset(sigset_t *s) { *s = 0; return 0; }
static __inline int sigfillset(sigset_t *s) { *s = ~1ul; return 0; }
static __inline int sigaddset(sigset_t *s, int n) { *s |= 1ul << n; return 0; }
static __inline int sigdelset(sigset_t *s, int n) { *s &= ~(1ul << n); return 0; }
static __inline int sigismember(const sigset_t *s, int n) { return (int)((*s >> n) & 1); }
_sig_func_ptr signal(int, _sig_func_ptr);
int raise(int);
int kill(pid_t, int);
int sigaction(int, const struct sigaction *, struct sigaction *);
int sigprocmask(int, const sigset_t *, sigset_t *);
_END_STD_C
#endif
