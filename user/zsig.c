/* SPDX-License-Identifier: GPL-2.0-only */
/* Signals (DESIGN D16, docs/d16-events-and-signals.md): sigaction and signal with the handler table and the one entry
 * the kernel calls, sigprocmask, kill, raise, alarm, pause, the process groups and the signals' names. One source for
 * libz and the C library (user/zshare.h). */
#include "zshare.h"

/* The kernel sends a handler to one entry (Exec's model, docs/d16-events-and-signals.md section 5): d0 the signal, a0
 * the frame it wrote on the stack, a5 the program's data (D13). The entry hands both to _zsig_run on the stack (the C
 * rules), which calls the handler from this table and gives the frame back with SYS_sigreturn. Only pc-relative
 * branches here: nothing to relocate in a program's shared code. */
static sighandler_t _zsigtab[NSIG];
void _zsig_run(int sig, void *frame) __attribute__((used, noreturn));
void _zsig_run(int sig, void *frame)
{
    int e = zerrno;                         /* the interrupted code's errno stays */
    sighandler_t h = (u32)sig < NSIG ? _zsigtab[sig] : 0;
    if ((u32)h > 1)
        h(sig);
    zerrno = e;
    sys_sigreturn(frame);
    for (;;)
        ;
}
void _zsigentry(void);
__asm__(".section .text._zsigentry,\"ax\",@progbits\n"
        "    .globl _zsigentry\n"
        "_zsigentry:\n"
        "    move.l %a0,-(%sp)\n"
        "    move.l %d0,-(%sp)\n"
        "    bsr.l _zsig_run\n"
        "1:  bra.s 1b\n"
        "    .previous\n");

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (sig <= 0 || sig >= NSIG) { zerrno = EINVAL; return -1; }
    sighandler_t prev = _zsigtab[sig];
    struct usigaction u;
    if (act) {
        u.size = sizeof u; u.handler = (u32)act->sa_handler; u.mask = act->sa_mask; u.flags = (u32)act->sa_flags;
        u.entry = (u32)_zsigentry;
        if ((u32)act->sa_handler > 1)
            _zsigtab[sig] = act->sa_handler;    /* first: one may be sent at the end of this very call */
    }
    int r = sys_sigaction(sig, act ? &u : 0);
    if (r < 0) { _zsigtab[sig] = prev; return -1; }
    if (old) { old->sa_handler = r == 1 ? SIG_IGN : r == 2 ? prev : SIG_DFL; old->sa_mask = 0; old->sa_flags = 0; }
    return 0;
}
sighandler_t signal(int sig, sighandler_t h)
{
    struct sigaction a, o;
    a.sa_handler = h; a.sa_mask = 0; a.sa_flags = SA_RESTART;
    return sigaction(sig, &a, &o) < 0 ? SIG_ERR : o.sa_handler;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    u32 o;
    int r = set ? sys_sigprocmask(how, *set, &o) : sys_sigprocmask(SIG_BLOCK, 0, &o);
    if (r < 0) return -1;
    if (old) *old = o;
    return 0;
}
int kill(int pid, int sig) { return sys_killsig(pid, sig) < 0 ? -1 : 0; }
int raise(int sig) { return kill(sys_getpid(), sig); }
unsigned alarm(unsigned s) { int t = sys_alarm(s * 100); return t > 0 ? (unsigned)(t + 99) / 100 : 0; }
int pause(void) { sys_evwait(0, 0, -1); return -1; }
int setpgid(int pid, int pg) { return sys_setpgid(pid, pg) < 0 ? -1 : 0; }
int getpgid(int pid)
{
    struct uproc p;
    if (!pid) pid = sys_getpid();
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && (int)p.pid == pid) return (int)p.pgid;
    zerrno = ESRCH;
    return -1;
}
int getpgrp(void) { return getpgid(0); }
int tcsetpgrp(int fd, int pg) { if (!sys_isatty(fd)) { zerrno = ENOTTY; return -1; } return sys_setfgpg(pg) < 0 ? -1 : 0; }
static const char *const signames[NSIG] = { 0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
    "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
    "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS" };
const char *zsig_name(int sig) { return sig > 0 && sig < NSIG ? signames[sig] : 0; }
int zsig_num(const char *s)
{
    if (isdigit((unsigned char)*s)) { char *e; long n = strtol(s, &e, 10); return *e || n < 0 || n >= NSIG ? -1 : (int)n; }
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 1; i < NSIG; i++) if (!strcmp(s, signames[i])) return i;
    return -1;
}
