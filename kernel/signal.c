/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Unix signals (DESIGN D16, docs/d16-events-and-signals.md sections 4-7 and 14). Numbers and default actions as Linux.
 * The kernel keeps per process the masks (pending, blocked, caught, ignored, restart), one sa_mask per signal and one
 * entry point in the program (Exec's tc_ExceptCode idea): libz's table of handlers lives in the program, the kernel only
 * knows "caught". A signal that is caught and not blocked is pending until the process next returns to user mode;
 * there sig_deliver writes a frame on its user stack and sends it to the entry, which calls the handler and then
 * SYS_sigreturn. Everything else - ignore, stop, continue, terminate - is done at once by sig_send. */

#define DFL_IGN   (SIGBIT(SIGCHLD) | SIGBIT(SIGWINCH) | SIGBIT(SIGCONT))
#define STOPSIGS  (SIGBIT(SIGSTOP) | SIGBIT(SIGTSTP) | SIGBIT(SIGTTIN) | SIGBIT(SIGTTOU))

/* wake p out of an interruptible wait: the signal will run a handler (section 4: only then is a call interrupted) */
static void sig_wake(struct proc *p)
{
    if (p->state == P_MSG && !p->wait_msg)
        ready(p);                       /* a server in an untimed receive: -EINTR there */
    else
        ev_post(p, EV_SIG);             /* event waits, sleep, wait: rule 1 decides */
}

/* the default action of sig for p (not caught, not ignored, not blocked) */
static void sig_default(struct proc *p, int sig)
{
    u32 b = SIGBIT(sig);
    if ((DFL_IGN & b) || p->pid == 1)   /* rule 6: boot (pid 1) is never ended or stopped by a default action */
        return;
    if (STOPSIGS & b) {
        proc_stop(p);
        return;
    }
    if (!p->kill_sig)
        p->kill_sig = (u8)sig;          /* the wait status: 128 + sig */
    proc_kill(p);
    if (sig == SIGKILL && p->killed == 1)
        p->killed = 3;                  /* at once, as a second kill (D11 5.6) */
}

/* send sig (1..31) to p; the caller checked the permission */
void sig_send(struct proc *p, int sig)
{
    u32 b = SIGBIT(sig);
    if (p->state == P_FREE || p->state == P_ZOMBIE || p->exiting || p->pid == 0)
        return;
    if (sig == SIGKILL || sig == SIGSTOP) {
        sig_default(p, sig);            /* never caught, blocked or ignored */
        return;
    }
    if (sig == SIGCONT) {               /* continues it always, then (if caught) runs its handler */
        p->sig_pend &= ~STOPSIGS;
        proc_cont(p);
    }
    if (STOPSIGS & b)
        p->sig_pend &= ~SIGBIT(SIGCONT);
    if (p->sig_ign & b)
        return;
    if (!(p->sig_catch & b) && !(p->sig_block & b)) {
        sig_default(p, sig);
        return;
    }
    p->sig_pend |= b;                   /* caught, or blocked: acted on when unblocked */
    if ((p->sig_catch & b) && !(p->sig_block & b))
        sig_wake(p);
}

/* pending signals that are no longer blocked and not caught take their default action now */
static void sig_unblocked(struct proc *p)
{
    u32 m = p->sig_pend & ~p->sig_block & ~p->sig_catch;
    for (int s = 1; m; s++)
        if (m & SIGBIT(s)) {
            m &= ~SIGBIT(s);
            p->sig_pend &= ~SIGBIT(s);
            sig_send(p, s);
        }
}

/* kill (new form) and the tty's ^C/^Z: a group. check: the may_signal rule per member (syscall.c). Rule 15: 0 if at
 * least one process got the signal, -EPERM if none was allowed, -ESRCH if the group is empty */
long sig_group(int g, int sig, int check)
{
    int found = 0, sent = 0;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state == P_FREE || p->state == P_ZOMBIE || p->pgid != g)
            continue;
        found = 1;
        if (check && may_signal(p))
            continue;
        sent = 1;
        if (sig)
            sig_send(p, sig);
    }
    return sent ? 0 : found ? -EPERM : -ESRCH;
}

/* SYS_sigaction: sig, struct usigaction * (0: only ask) -> the old disposition (0 default, 1 ignored, 2 caught) */
long sig_action(u32 sig, u32 uact)
{
    struct proc *c = current;
    if (sig == 0 || sig >= NSIG)
        return -EINVAL;
    u32 b = SIGBIT(sig);
    long old = (c->sig_ign & b) ? 1 : (c->sig_catch & b) ? 2 : 0;
    if (!uact)
        return old;
    if ((b & SIG_UNCATCHABLE) || !user_ok_rd(c, uact, USIGACT_FIRST))
        return (b & SIG_UNCATCHABLE) ? -EINVAL : -EFAULT;
    const struct usigaction *a = (const struct usigaction *)uact;
    if (a->size < USIGACT_FIRST || (a->flags & ~SA_RESTART))
        return -EINVAL;
    u32 mask = a->mask & ~SIG_UNCATCHABLE;
    if (mask && !c->sig_mask) {         /* the masks' table, at the first one (most programs never set one) */
        if (!(c->sig_mask = kmalloc(NSIG * 4)))
            return -ENOMEM;
        memset(c->sig_mask, 0, NSIG * 4);
    }
    if (a->handler > 1) {               /* caught: the entry is registered (or was by an earlier call) and checked */
        u32 e = a->entry ? a->entry : c->sig_entry;
        if (!e || (e & 1) || !user_ok_rd(c, e, 2))
            return -EINVAL;
        c->sig_entry = e;
        c->sig_catch |= b;
        c->sig_ign &= ~b;
    } else {
        c->sig_catch &= ~b;
        if (a->handler)
            c->sig_ign |= b;
        else
            c->sig_ign &= ~b;
    }
    if (c->sig_mask)
        c->sig_mask[sig] = mask;
    if (a->flags & SA_RESTART)
        c->sig_rest |= b;
    else
        c->sig_rest &= ~b;
    if ((c->sig_pend & b) && !(c->sig_catch & b)) {
        c->sig_pend &= ~b;              /* now ignored (dropped) or default (acted on, or pending while blocked) */
        sig_send(c, (int)sig);
    }
    return old;
}

/* SYS_sigprocmask: how (SIG_BLOCK, SIG_UNBLOCK, SIG_SETMASK), set, u32 *old (0: not wanted) */
long sig_procmask(u32 how, u32 set, u32 uold)
{
    struct proc *c = current;
    if (how > SIG_SETMASK)
        return -EINVAL;
    if (uold) {
        if (!user_ok_wr(c, uold, 4))
            return -EFAULT;
        *(u32 *)uold = c->sig_block;
    }
    u32 m = how == SIG_BLOCK ? c->sig_block | set : how == SIG_UNBLOCK ? c->sig_block & ~set : set;
    c->sig_block = m & ~SIG_UNCATCHABLE & ~1u;
    sig_unblocked(c);
    return 0;
}

/* SYS_alarm: SIGALRM after t ticks (0: none); the ticks the previous one had left (0: none). A kernel deadline that
 * joins the sleepers' (ntimer, wake_min: proc.c), so it fires in CPU-bound code too (rule 3) */
long sig_alarm(u32 t)
{
    struct proc *c = current;
    u16 s = irq_save();
    u32 left = 0;
    if (c->alarm_on) {
        left = (s32)(c->alarm_at - ticks) > 0 ? c->alarm_at - ticks : 1;
        c->alarm_on = 0;
        ntimer--;
    }
    if (t) {
        c->alarm_at = ticks + t;
        c->alarm_on = 1;
        if (ntimer++ == 0 || (s32)(c->alarm_at - wake_min) < 0)
            wake_min = c->alarm_at;
    }
    irq_restore(s);
    return (long)left;
}

/* SYS_setpgid: pid (0: the caller), pgid (0: pid). Rule 7: only oneself or one's own children (a zombie child too);
 * the group is the process's own pid or one that exists */
long sig_setpgid(int pid, int pg)
{
    struct proc *p = pid ? proc_by_pid(pid) : current;
    if (!p || (p != current && p->ppid != current->pid))
        return -ESRCH;
    if (pg < 0)
        return -EINVAL;
    if (!pg)
        pg = p->pid;
    if (pg != p->pid) {
        int ok = 0;
        for (int i = 1; i < NPROC; i++)
            if (procs[i].state != P_FREE && procs[i].pgid == pg)     /* a zombie member keeps it */
                ok = 1;
        if (!ok)
            return -EPERM;
    }
    p->pgid = pg;
    return 0;
}

/* The last step before a return to user mode, on both paths: the end of a system call (nr: its number) and the level-2
 * interrupt when it cut into user code (nr 0). Rule 3: a kill is acted on first, then a stop, then one handler: the
 * frame goes on the user stack and the process continues at its entry. More pending ones follow at the end of the
 * entry's SYS_sigreturn (nested, each masked as its sa_mask says). rb: d0-d7, a0-a6, then the CPU's format-0 frame
 * (SR word, PC long), which is edited in place: the kernel never writes a format word. */
void sig_deliver(u32 *rb, u32 nr)
{
    struct proc *c = current;
    if (c->pid == 0)
        return;
    if (proc_kill_now())
        proc_exit(kill_status(c));
    if (c->stop_req && !c->wait_msg) {
        schedule();                     /* a stop asked by the process itself, or at a tick: it stops now */
        if (proc_kill_now())
            proc_exit(kill_status(c));
    }
    u32 due = SIG_DUE(c);
    if (!due || c->killed || c->exiting) {
        c->sig_intr = 0;
        return;
    }
    int sig = 1;
    while (!(due & SIGBIT(sig)))
        sig++;
    u32 b = SIGBIT(sig);
    c->sig_pend &= ~b;
    u8 *fr = (u8 *)(rb + 15);
    u32 pc = *(u32 *)(fr + 2);
    if (c->sig_intr && nr && (c->sig_rest & b)) {   /* SA_RESTART: trap #0 again (2 bytes), d0 = the call */
        pc -= 2;
        rb[0] = nr;
    }
    c->sig_intr = 0;
    u32 usp = get_usp(), f = (usp - sizeof(struct sigframe)) & ~3u;
    int ok = f < usp && user_ok_wr(c, f, sizeof(struct sigframe));
    if (ok && c->ustk && f >= c->ustk && f <= c->base + c->span)
        ok = f >= c->ustk + SIG_STACK_ROOM;     /* rule 5: 512 bytes above the guard left for the handler */
    if (!ok) {
        kprintf("\n[kernel] pid %d (%s): stack overflow in a signal: killed\n", c->pid, c->name);
        c->killed = 2;
        proc_exit(139);
    }
    struct sigframe *sf = (struct sigframe *)f;
    sf->size = SIGFRAME_FIRST;
    memcpy(sf->regs, rb, 60);
    sf->regs[15] = usp;
    sf->pc = pc;
    sf->ccr = *(u16 *)fr & 0x1F;
    sf->sig = (u32)sig;
    sf->blocked = c->sig_block;
    c->sig_block = (c->sig_block | b | (c->sig_mask ? c->sig_mask[sig] : 0)) & ~SIG_UNCATCHABLE;
    set_usp(f);
    rb[0] = (u32)sig;                   /* d0: the signal, a0: the frame (passed back to sigreturn), a5: D13's base */
    rb[8] = f;
    rb[13] = c->a5;
    *(u32 *)(fr + 2) = c->sig_entry;
}

/* SYS_sigreturn: back from a handler. Rule 5 and section 5: the frame must lie in the process's memory before a word
 * of it is read; the PC must be even; only the condition codes come from the frame (the status register's upper byte
 * is always 0: user mode, interrupts on); the blocked mask never holds SIGKILL or SIGSTOP. A bad frame kills the
 * process (139). Returns the frame's d0 (the system call's result store would otherwise overwrite it). */
long sig_return(u32 *rb, u32 f)
{
    struct proc *c = current;
    const struct sigframe *sf = (const struct sigframe *)f;
    if ((f & 1) || !user_ok_rd(c, f, SIGFRAME_FIRST) || sf->size < SIGFRAME_FIRST || !user_ok_rd(c, f, sf->size) ||
        (sf->pc & 1)) {
        kprintf("\n[kernel] pid %d (%s): bad signal frame at %p: killed\n", c->pid, c->name, f);
        c->killed = 2;
        proc_exit(139);
    }
    u8 *fr = (u8 *)(rb + 15);
    memcpy(rb, sf->regs, 60);
    set_usp(sf->regs[15]);
    *(u16 *)fr = (u16)(sf->ccr & 0x1F);
    *(u32 *)(fr + 2) = sf->pc;
    c->sig_block = sf->blocked & ~SIG_UNCATCHABLE & ~1u;
    sig_unblocked(c);
    return (long)sf->regs[0];
}
