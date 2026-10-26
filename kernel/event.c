/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Events (DESIGN D16, docs/d16-events-and-signals.md sections 2, 3 and 14). Exec's model underneath: every process has
 * fixed event bits (EV_*) that are pending and bits it waits for; an object a process waits on - a tty, a pipe end,
 * the serial device, a port - carries a mask of the process slots waiting on it, so registering is one OR. A waker
 * sets the bit and readies only a process that waits for it (rule 1); a stale slot bit (a process that left the wait
 * or died) is harmless, because every wait checks again in a loop.
 *
 * The order of a wait (section 3, Exec's order): register on the objects and clear the bit; check readiness with
 * interrupts on; then with interrupts masked only test the bits and set the state (ev_block). Producers run in process
 * context (kwork, or the producer's own system call), never in an interrupt, and the kernel is not preempted, so the
 * only producer that can run between the check and the block is one the check itself caused; the bit catches it. */

/* rule 1: p waits for bits it has, in the event-wait state or a timed one */
static int ev_due(const struct proc *p)
{
    return (p->state == P_EV || (p->state == P_SLEEP && p->ev_wait)) && (p->ev_pending & p->ev_wait);
}

void ev_post(struct proc *p, u32 bits)
{
    u16 s = irq_save();
    p->ev_pending |= bits;
    if (ev_due(p))
        ready(p);
    irq_restore(s);
}

/* every process registered on an object: EV_IO; the mask is emptied (a woken waiter registers again) */
void ev_wake(u32 *mask)
{
    u32 m = *mask;
    if (!m)
        return;
    *mask = 0;
    for (int i = 1; i < NPROC; i++)
        if (m & (1u << i))
            ev_post(&procs[i], EV_IO);
}

/* step 1 for one object: register on it and clear EV_IO */
void ev_arm(u32 *mask)
{
    *mask |= 1u << (current - procs);
    current->ev_pending &= ~EV_IO;
}

/* Step 3: block until one of bits arrives (or, if timed, until the tick `until`); EV_SIG always ends the wait. Never
 * blocks when killed, asked to stop (it stops here, then goes on) or a caught unblocked signal is due. Returns 0 after
 * a wake-up (the caller checks again), -EINTR when killed or a handler is due; restart: the system call may be
 * restarted after the handler (SA_RESTART, rule 4: sig_intr), 0 for those that never are (sleep, evwait). */
long ev_block(u32 bits, int timed, u32 until, int restart)
{
    struct proc *c = current;
    u16 s = irq_save();
    c->ev_pending &= ~EV_SIG;           /* only a wake-up: a signal is tested itself (SIG_DUE), so none goes stale */
    c->ev_wait = bits | EV_SIG;
    if (!(c->ev_pending & c->ev_wait) && !c->killed && !c->stop_req && !SIG_DUE(c)) {
        if (timed)
            sleep_until(until);
        else
            c->state = P_EV;
        schedule();
    }
    c->ev_wait = 0;
    irq_restore(s);
    if (c->stop_req && !c->killed)
        schedule();                     /* stopped there (P_READY with stop_req), until SIGCONT */
    if (c->killed)
        return -EINTR;
    if (SIG_DUE(c)) {
        c->sig_intr = (u8)restart;
        return -EINTR;
    }
    return 0;
}

/* SYS_evwait (poll and select in libz): n items { fd, events, revents }, timeout in ticks (-1: none, 0: only check).
 * An item with POLLPORT is a port the caller owns: POLLIN when a message is queued. The count of items with revents
 * set, 0 at the timeout, -EINTR when a handler is due (never restarted). */
long ev_wait_items(u32 uitems, u32 n, s32 timeout)
{
    struct upollfd *it = (struct upollfd *)uitems;
    if (n > EV_MAXITEMS)
        return -EINVAL;
    if (n && !user_ok_wr(current, uitems, n * sizeof *it))
        return -EFAULT;
    u32 until = ticks + (u32)timeout;
    for (;;) {
        current->ev_pending &= ~EV_IO;  /* step 1: every check below registers on its object */
        long cnt = 0;
        for (u32 i = 0; i < n; i++) {
            u16 ev = it[i].events, r = 0;
            if (it[i].fd < 0)
                ;                       /* POSIX: ignored */
            else if (ev & POLLPORT)
                r = port_poll(it[i].fd);
            else if ((u32)it[i].fd >= NFD || !current->fd[it[i].fd])
                r = POLLNVAL;
            else
                r = vfs_poll(current->fd[it[i].fd]);
            r &= ev | POLLERR | POLLHUP | POLLNVAL;
            it[i].revents = r;
            if (r)
                cnt++;
        }
        if (cnt || !timeout)
            return cnt;
        if (timeout > 0 && (s32)(ticks - until) >= 0)
            return 0;
        long e = ev_block(EV_IO, timeout > 0, until, 0);
        if (e)
            return e;
    }
}
