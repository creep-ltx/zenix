/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D54 (docs/d54-debugger.md): the live debugger's kernel part. SYS_debug's operations, the holds in the
 * exception path, the hooks the scheduler, the signals, the loader and the exit call when a process has a debugger.
 * Nothing here runs unless a debugger is attached (section 16): every hook is behind a test of struct proc's dbgf byte
 * or of ndbg. Cold code: size first. */
#pragma GCC optimize("Os")
#include "kernel.h"
#include "dbgproto.h"
#include "hwinfo.h"
#include "mmu.h"

struct dbg {
    int tracer;                         /* the debugger's pid */
    int pid;                            /* the target's pid (0: reserved by DBG_NEXT, no target yet) */
    struct proc *p;                     /* its slot (0: pending, or an exit record) */
    u32 *rb;                            /* its user registers' block while held inside exc_c (section 7) */
    u8 held;                            /* a debugger's stop (only the debugger ends it) */
    u8 why, req;                        /* DBGW_*: why it stopped / why a stop was asked (stop_req) */
    u8 started;                         /* started under the debugger: killed when the debugger ends */
    u8 jstop;                           /* a job-control stop arrived while held (section 5) */
    u8 exited;                          /* an exit record: status */
    u8 passing;                         /* DBG_CONT is sending a signal: SIGINT is not intercepted */
    u8 nbp;
    u32 info, pc, addr;
    int status;
    u32 entry;                          /* the entry's breakpoint (DBG_NEXT), 0 once taken */
    u16 entryw;
    u16 bpw[NBP];                       /* the words the breakpoints replaced */
    u32 bpat[NBP];
};

static struct dbg *dbgs[NDBG];
u8 ndbg;

static struct dbg *of(const struct proc *p)
{
    for (int i = 0; i < NDBG; i++)
        if (dbgs[i] && dbgs[i]->p == p)
            return dbgs[i];
    return 0;
}

static struct dbg *mine(int pid)
{
    for (int i = 0; i < NDBG; i++)
        if (dbgs[i] && dbgs[i]->pid == pid && pid && dbgs[i]->tracer == current->pid)
            return dbgs[i];
    return 0;
}

static struct dbg *dbg_new(void)
{
    for (int i = 0; i < NDBG; i++)
        if (!dbgs[i]) {
            struct dbg *d = kmalloc(sizeof *d);
            if (d) {
                memset(d, 0, sizeof *d);
                d->tracer = current->pid;
                dbgs[i] = d;
                ndbg++;
            }
            return d;
        }
    return 0;
}

static void dbg_free(struct dbg *d)
{
    for (int i = 0; i < NDBG; i++)
        if (dbgs[i] == d) {
            dbgs[i] = 0;
            ndbg--;
            kfree(d);
        }
}

static void tell(const struct dbg *d)
{
    struct proc *t = proc_by_pid(d->tracer);
    if (t)
        ev_post(t, EV_CHILD);
}

#define SRW(rb) (*(u16 *)((u8 *)(rb) + 60))
#define PCW(rb) (*(u32 *)((u8 *)(rb) + 62))

/* section 7: the stopped process's user block - held in exc_c, else the trap #0 / level-2 block at ktop - 68 (checked:
 * format 0, user mode) */
static u32 *ublock(const struct proc *p, const struct dbg *d)
{
    if (d && d->rb)
        return d->rb;
    u32 *rb = (u32 *)(p->ktop - 68);
    if ((*(u16 *)((u8 *)rb + 66) >> 12) || (SRW(rb) & 0x2000))
        return 0;
    return rb;
}

/* the breakpoints' words back (a living target: its image lives as long as it does) */
static void unbreak_all(struct dbg *d)
{
    for (int i = 0; i < d->nbp; i++)
        *(u16 *)d->bpat[i] = d->bpw[i];
    if (d->entry)
        *(u16 *)d->entry = d->entryw;
    d->nbp = 0;
    d->entry = 0;
    cache_flush_code();
}

/* detach a living target: breakpoints out, the trace bits off, a debugger's stop ended (section 13) */
static void detach(struct dbg *d)
{
    struct proc *p = d->p;
    unbreak_all(d);
    u32 *rb = ublock(p, d);
    if (rb)
        SRW(rb) &= 0x3FFF;
    p->dbgf &= (u8)~DBGF_TRACED;
    if (d->req && p->state != P_STOP)
        p->stop_req = 0;                /* a debugger's stop not yet taken: nobody's now (review 4) */
    int was = d->held;
    dbg_free(d);
    if (was)
        proc_cont(p);
}

/* section 4: may current debug p? 0 or -errno */
static long may(const struct proc *p)
{
    struct proc *c = current;
    if (!p || p->state == P_ZOMBIE || p->exiting || p->pid <= 1 || !p->ppid || p->priv || mmu_is_sys(p) || p == c)
        return p ? -EPERM : -ESRCH;
    if (p->dbgf & DBGF_TRACED)
        return -EPERM;
    if (p->dbgf & DBGF_INEXC)
        return -EBUSY;                  /* job-stopped inside exc_c: its registers are no system call's (review 1) */
    if (c->uid && (c->uid != p->uid || c->ruid != p->ruid || p->uid != p->ruid || (p->dbgf & DBGF_NODEBUG)))
        return -EPERM;
    const struct proc *t = c;           /* no cycle: p is none of the caller's own debuggers */
    for (int n = 0; n <= NDBG; n++) {
        const struct dbg *d = of(t);
        if (!d)
            break;
        if (d->tracer == p->pid)
            return -EPERM;
        if (!(t = proc_by_pid(d->tracer)))
            break;
    }
    return 0;
}

/* the caller's struct at u: its size (0: the first), -EINVAL below the first, -EFAULT outside its memory */
static long usize(u32 u, u32 first, int wr)
{
    if (!user_ok_rd(current, u, 4))
        return -EFAULT;
    u32 n = *(u32 *)u;
    if (!n)
        n = first;
    if (n < first)
        return -EINVAL;
    if (!(wr ? user_ok_wr(current, u, n) : user_ok_rd(current, u, n)))
        return -EFAULT;
    return (long)n;
}

/* ZEX2 code another process runs cannot be written; one only p runs becomes its own (section 10) */
static long own_code(struct proc *p)
{
    if (!p->seg)
        return 0;
    if (p->seg->refs > 1)
        return -ETXTBSY;
    seg_disown(p);
    return 0;
}

/* section 9: a breakpoint's place - even, inside the image (a ZEXE block below its stack, a ZEX2 program's code) */
static int in_image(const struct proc *p, u32 a)
{
    if (a & 1)
        return 0;
    if (p->csize)
        return a >= p->code && a + 2 <= p->code + p->csize;
    return a >= p->base && a + 2 <= p->ustk;
}

static long resume(struct proc *p, struct dbg *d, int step, u32 sig)
{
    if (sig > 31)
        return -EINVAL;
    u32 *rb = ublock(p, d);
    if (!rb)
        return -EIO;
    if (step) {
        const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
        if (hw->magic == HWINFO_MAGIC && (hw->flags & HW_DT))
            return -EOPNOTSUPP;         /* Emu68 never traces (section 8) */
    }
    if (sig) {
        d->passing = 1;
        sig_send(p, (int)sig);
        d->passing = 0;
    }
    SRW(rb) &= 0x3FFF;
    if (d->jstop) {                     /* a job-control stop came while held: it becomes one now (section 5) */
        d->jstop = 0;
        d->held = 0;
        d->why = DBGW_JOBSTOP;
        p->stop_rep = 0;
        struct proc *par = proc_by_pid(p->ppid);
        if (par) {
            ev_post(par, EV_CHILD);
            sig_send(par, SIGCHLD);
        }
        return 0;
    }
    if (step)
        SRW(rb) |= 0x8000;              /* T1: one instruction */
    d->held = 0;
    proc_cont(p);
    return 0;
}

/* exc_c: hold the current process for its debugger (section 5). Returns when resumed; ends it when killed. A stop
 * asked while it was on its way out (DBG_STOP, SIGINT, ^Z before it ran) holds it again here, with this block. */
static void hold(struct dbg *d, u32 *rb, u32 why, u32 info, u32 addr)
{
    struct proc *p = current;
    for (;;) {
        int held = why != DBGW_JOBSTOP;
        d->rb = rb;
        d->why = (u8)why;
        d->info = info;
        d->addr = addr;
        d->held = (u8)held;
        SRW(rb) &= 0x3FFF;
        p->dbgf |= DBGF_INEXC;
        u16 s = irq_save();
        p->stop_req = 0;
        p->state = P_STOP;
        tell(d);
        if (!held) {
            p->stop_rep = 0;
            struct proc *par = proc_by_pid(p->ppid);
            if (par) {
                ev_post(par, EV_CHILD);
                sig_send(par, SIGCHLD);
            }
        }
        schedule();
        irq_restore(s);
        p->dbgf &= (u8)~DBGF_INEXC;
        if (p->killed)
            proc_exit(kill_status(p));  /* at once: a user-mode exception is never inside a message call */
        if (!(p->dbgf & DBGF_TRACED) || !(d = of(p)))
            return;                     /* detached while held */
        d->rb = 0;
        if (!p->stop_req)
            return;
        why = d->req ? d->req : DBGW_JOBSTOP;
        info = why == DBGW_SIGNAL ? SIGINT : 0;
        addr = 0;
        d->req = 0;
    }
}

int dbg_exc(u32 *rb, u32 vec, u32 pc, u32 faddr)
{
    struct proc *p = current;
    struct dbg *d = (p->dbgf & DBGF_TRACED) ? of(p) : 0;
    if (!d) {
        if (vec != 9)
            return 0;
        SRW(rb) &= 0x3FFF;              /* a trace bit a debugger left (section 8): harmless */
        return 1;
    }
    if (vec == 9) {
        hold(d, rb, DBGW_STEP, 0, 0);
        return 1;
    }
    if (vec == 4) {
        if (d->entry && pc == d->entry) {       /* started under the debugger: its first instruction (section 11) */
            *(u16 *)pc = d->entryw;
            d->entry = 0;
            cache_flush_code();
            hold(d, rb, DBGW_EXEC, 0, 0);
            return 1;
        }
        for (int i = 0; i < d->nbp; i++)
            if (d->bpat[i] == pc) {
                hold(d, rb, DBGW_BREAK, 0, 0);
                return 1;
            }
    }
    hold(d, rb, DBGW_FAULT, vec, faddr);        /* a look first; then it ends as without a debugger (section 12) */
    return 0;
}

int dbg_stopped(struct proc *p)
{
    struct dbg *d = of(p);
    if (!d)
        return 0;
    d->held = d->req != 0;
    d->why = d->req ? d->req : DBGW_JOBSTOP;
    d->info = d->req == DBGW_SIGNAL ? SIGINT : 0;
    d->req = 0;
    d->rb = 0;
    tell(d);
    return d->held;
}

int dbg_held(const struct proc *p)     /* (or a debugger's stop asked and not yet taken: SIGCONT leaves it) */
{
    const struct dbg *d = of(p);
    return d && ((d->held && p->state == P_STOP) || (d->req && p->state != P_STOP));
}

int dbg_sigint(struct proc *p)
{
    struct dbg *d = of(p);
    if (!d || d->passing)
        return 0;
    if (p->state != P_STOP && !d->req) {       /* (stopped already, or a stop asked: dropped) */
        d->req = DBGW_SIGNAL;
        proc_stop(p);
    }
    return 1;
}

void dbg_sysmark(struct proc *p)       /* it became a system process (mmu_sysmark): no debugger stops one (section 4) */
{
    struct dbg *d = of(p);
    if (d && d->p)
        detach(d);
}

void dbg_jobstop(struct proc *p)
{
    struct dbg *d = of(p);
    if (d && d->held)
        d->jstop = 1;
}

/* proc_exit: p ends - as a debugger (what it attached to is detached, what it started killed) and as a target (its
 * block becomes an exit record for its debugger's DBG_WAIT) */
void dbg_exit(struct proc *p, int code)
{
    for (int i = 0; i < NDBG; i++) {
        struct dbg *d = dbgs[i];
        if (!d)
            continue;
        if (d->tracer == p->pid) {
            struct proc *t = d->p;
            if (t && d->started) {
                t->dbgf &= (u8)~DBGF_TRACED;
                dbg_free(d);
                sig_send(t, SIGKILL);
            } else if (t)
                detach(d);
            else
                dbg_free(d);
        } else if (d->p == p) {
            d->p = 0;
            d->exited = 1;
            d->held = 0;
            d->status = code;
            p->dbgf &= (u8)~DBGF_TRACED;
            tell(d);
        }
    }
}

/* proc_load, the parent's DBGF_NEXT: the block DBG_NEXT reserved */
struct dbg *dbg_pending(const struct proc *parent)
{
    for (int i = 0; i < NDBG; i++)
        if (dbgs[i] && dbgs[i]->tracer == parent->pid && !dbgs[i]->pid)
            return dbgs[i];
    return 0;
}

void dbg_entry(struct dbg *d, u32 entry)        /* before the loader's cache flush */
{
    d->entry = entry;
    d->entryw = *(u16 *)entry;
    *(u16 *)entry = DBG_ILLEGAL;
}

void dbg_link(struct dbg *d, struct proc *p)
{
    d->p = p;
    d->pid = p->pid;
    d->started = 1;
    p->dbgf |= DBGF_TRACED;
}

/* a program file the caller may read (section 4): the real id against the owner, the groups, the rest */
int dbg_readable(u32 mode, u32 uid, u32 gid, const struct proc *c)
{
    if (!c || !c->uid)
        return 1;
    if (uid == c->ruid)
        return (mode & 0400) != 0;
    int g = gid == c->gid;
    for (int i = 0; i < c->ngroups; i++)
        g |= c->groups[i] == gid;
    return (mode & (g ? 040 : 04)) != 0;
}

void dbg_crash(const struct proc *p, const u32 *rb)
{
    if ((p->dbgf & DBGF_NODEBUG) || !p->uid)
        return;                         /* the log is everyone's: not a root or nodebug process's registers */
    const u8 *f = (const u8 *)rb + 60;
    kprintf("[kernel] pid %d regs: base %p code %p sr %x pc %p d0..d7", p->pid, p->base, p->code, *(const u16 *)f,
            *(const u32 *)(f + 2));
    for (int i = 0; i < 16; i++)
        kprintf(i == 8 ? " a0..a7 %x" : " %x", i == 15 ? get_usp() : rb[i]);
    u32 fp = rb[14], lo = p->base, hi = p->base + p->span, r[8];
    int n = 0;
    while (n < 8 && !(fp & 1) && fp >= lo && fp <= hi - 8) {    /* a6 .. a6 + 8 in its own block, rising */
        r[n++] = ((const u32 *)fp)[1];
        u32 nx = *(const u32 *)fp;
        if (nx <= fp)
            break;
        fp = nx;
    }
    kprintf("\n[kernel] pid %d frames: %d", p->pid, n);
    for (int i = 0; i < n; i++)
        kprintf(" %p", r[i]);
    kprintf("\n");
}

/* SYS_debug */
long dbg_sys(u32 op, u32 pid, u32 arg)
{
    struct proc *c = current, *p;
    struct dbg *d;
    long n;
    if (op == DBG_NEXT) {
        if (!dbg_pending(c) && !dbg_new())
            return -EAGAIN;
        c->dbgf |= DBGF_NEXT;
        return 0;
    }
    if (op == DBG_ATTACH) {
        p = proc_by_pid((int)pid);
        if ((n = may(p)))
            return n;
        if (!(d = dbg_new()))
            return -EAGAIN;
        d->pid = p->pid;
        d->p = p;
        p->dbgf |= DBGF_TRACED;
        if (p->state == P_STOP) {
            d->held = 1;
            d->why = DBGW_ATTACH;
        } else {
            d->req = DBGW_ATTACH;
            proc_stop(p);
        }
        return 0;
    }
    if (!(d = mine((int)pid)))
        return -ESRCH;
    if (op == DBG_WAIT) {
        if ((n = usize(arg, UDBGSTOP_FIRST, 1)) < 0)
            return n;
        struct udbgstop s;
        for (;;) {
            c->ev_pending &= ~EV_CHILD;         /* first, then look, then block (proc_wait's order) */
            memset(&s, 0, sizeof s);
            s.size = sizeof s;
            if (d->exited) {
                s.why = DBGW_EXIT;
                s.status = (u32)d->status;
                dbg_free(d);
                break;
            }
            p = d->p;
            if (p->state == P_STOP) {
                u32 *rb = ublock(p, d);
                s.why = d->held ? d->why : DBGW_JOBSTOP;
                s.info = d->info;
                s.addr = d->addr;
                s.pc = rb ? PCW(rb) : 0;
                break;
            }
            long e = ev_block(EV_CHILD, 0, 0, 0);
            if (e)
                return e;
        }
        memcpy((void *)arg, &s, (u32)n < sizeof s ? (u32)n : sizeof s);
        return 0;
    }
    if (d->exited)
        return op == DBG_DETACH ? (dbg_free(d), 0) : -ESRCH;
    p = d->p;
    if (op == DBG_DETACH) {
        detach(d);
        return 0;
    }
    if (op == DBG_INFO) {
        if ((n = usize(arg, UDBGINFO_FIRST, 1)) < 0)
            return n;
        struct udbginfo i = { sizeof i, p->base, p->span, p->code, p->csize, p->a5, p->seg ? p->seg->refs - 1u : 0 };
        memcpy((void *)arg, &i, (u32)n < sizeof i ? (u32)n : sizeof i);
        return 0;
    }
    if (op == DBG_STOP) {
        if (p->state != P_STOP) {
            d->req = DBGW_STOP;
            proc_stop(p);
        }
        return 0;
    }
    if (op == DBG_PEEK || op == DBG_POKE) {
        struct udbgmem m;
        if (!user_ok_rd(c, arg, sizeof m))
            return -EFAULT;
        memcpy(&m, (const void *)arg, sizeof m);
        if (!m.len || m.len > DBG_MEMMAX)
            return -EINVAL;
        if (op == DBG_PEEK) {
            if (!user_ok_rd(p, m.addr, m.len) || !user_ok_wr(c, m.buf, m.len))
                return -EFAULT;
            memcpy((void *)m.buf, (const void *)m.addr, m.len);
            return 0;
        }
        if (p->state != P_STOP)
            return -EBUSY;
        if (!user_ok_rd(c, m.buf, m.len) || !user_ok_rd(p, m.addr, m.len))
            return -EFAULT;
        if (!user_ok_wr(p, m.addr, m.len) && (n = own_code(p)))
            return n;                           /* (user_ok_rd: then it lies in its code) */
        for (int i = 0; i < d->nbp; i++)        /* the program's words under the breakpoints: write, take, put back */
            *(u16 *)d->bpat[i] = d->bpw[i];
        if (d->entry)
            *(u16 *)d->entry = d->entryw;
        memcpy((void *)m.addr, (const void *)m.buf, m.len);
        for (int i = 0; i < d->nbp; i++) {
            d->bpw[i] = *(u16 *)d->bpat[i];
            *(u16 *)d->bpat[i] = DBG_ILLEGAL;
        }
        if (d->entry) {
            d->entryw = *(u16 *)d->entry;
            *(u16 *)d->entry = DBG_ILLEGAL;
        }
        cache_flush_code();
        return 0;
    }
    if (p->state != P_STOP)
        return -EBUSY;
    if (op == DBG_CONT || op == DBG_STEP)
        return resume(p, d, op == DBG_STEP, arg);
    if (op == DBG_GETREGS || op == DBG_SETREGS) {
        u32 *rb = ublock(p, d);
        if (!rb)
            return -EIO;
        if ((n = usize(arg, UDBGREGS_FIRST, op == DBG_GETREGS)) < 0)
            return n;
        struct udbgregs r;
        if (op == DBG_GETREGS) {
            r.size = sizeof r;
            memcpy(r.d, rb, 60);
            r.a[7] = p->usp;
            r.pc = PCW(rb);
            r.sr = SRW(rb);
            memcpy((void *)arg, &r, (u32)n < sizeof r ? (u32)n : sizeof r);
            return 0;
        }
        memcpy(&r, (const void *)arg, (u32)n < sizeof r ? (u32)n : sizeof r);
        if (r.pc & 1)
            return -EINVAL;
        memcpy(rb, r.d, 60);
        p->usp = r.a[7];
        if (r.pc != PCW(rb))
            p->sig_intr = 0;                    /* no SA_RESTART beside a new PC (section 7) */
        PCW(rb) = r.pc;
        SRW(rb) = (u16)((SRW(rb) & ~0x1Fu) | (r.sr & 0x1F));
        return 0;
    }
    if (op == DBG_BREAK || op == DBG_UNBREAK) {
        int i = 0;
        while (i < d->nbp && d->bpat[i] != arg)
            i++;
        if (op == DBG_UNBREAK) {
            if (i == d->nbp)
                return -ENOENT;
            *(u16 *)arg = d->bpw[i];
            d->nbp--;
            d->bpat[i] = d->bpat[d->nbp];
            d->bpw[i] = d->bpw[d->nbp];
        } else {
            if (i < d->nbp)
                return -EEXIST;
            if (!in_image(p, arg))
                return -EINVAL;
            if (d->nbp == NBP)
                return -ENOSPC;
            if ((n = own_code(p)))
                return n;
            d->bpat[i] = arg;
            d->bpw[i] = *(u16 *)arg;
            d->nbp++;
            *(u16 *)arg = DBG_ILLEGAL;
        }
        cache_flush_code();
        return 0;
    }
    return -EINVAL;
}
