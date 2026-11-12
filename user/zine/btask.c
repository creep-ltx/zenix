/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: more than one task (docs/d29-zine.md 16.16). The program's tasks are switched as exec switches them,
 * from the exec and dos autodocs: the highest-priority ready task runs, equals take turns of a quantum (4 frames), a
 * task that waits gives the CPU away, a Signal that readies a higher-priority task switches to it. The lists are
 * ExecBase's own (ThisTask, TaskReady by priority, TaskWait); each task's registers and FPU state are in a struct zt.
 * The switch itself is in blobs.S (zt_swout, zt_swin: the privilege-violation trick of 5.2, or an interrupt's
 * return); zt_pick below chooses, in supervisor mode with interrupts masked. Nothing here runs until the program
 * makes a second task (zt_on). */
#include "blob.h"

/* a task's record: the part blobs.S and bfpu.S use at fixed offsets first */
struct zt {
    u32 r[15];                          /* 0: d0-d7, a0-a6 */
    u32 usp, pc;                        /* 60, 64 */
    u16 sr, fpok;                       /* 68; 70: 1 when the FP registers below are kept */
    u32 fpc[3];                         /* 72: FPCR, FPSR, FPIAR */
    u8 fpr[96];                         /* 84: FP0-FP7 */
    u8 fsv[216];                        /* 180: the FSAVE frame (zeros: a null frame; 216 the 68882's largest) */
    struct zt *next;                    /* 396: every task's, the first task's first */
    u32 task;
    u32 wake;                           /* a sleeper's frame (zt_sleep) */
    u32 mem, memsize;                   /* what ZINE allocated for it (this record first), freed when it ends */
    u32 exitcode, exitdata, seglist;    /* CreateNewProc's NP_ExitCode, NP_ExitData, NP_Seglist with NP_FreeSeglist */
    u8 sleeping;
};
_Static_assert(__builtin_offsetof(struct zt, next) == 396, "struct zt: blobs.S offsets");

enum { TS_RUN = 2, TS_READY = 3, TS_WAIT = 4, TS_REMOVED = 6 };
enum { ZT_YIELD, ZT_WAIT, ZT_SLEEP, ZT_EXIT };
#define THIS       EB(ZO_ExecBase_ThisTask, u32)
#define T8(t, o)   (*(volatile s8 *)((t) + ZO_Task_##o))
#define T32(t, o)  (*(volatile u32 *)((t) + ZO_Task_##o))
#define PRI(t)     (((struct znode *)(t))->pri)
#define READY      ((struct zlist *)(ZEXEC + ZO_ExecBase_TaskReady))
#define WAITING    ((struct zlist *)(ZEXEC + ZO_ExecBase_TaskWait))
#define SIGF_SINGLE 0x10u

u8 zt_on, zt_need, zt_fpu;              /* zt_fpu: 0 none, 1 68881/2 or 68040's, 2 the 68060's (blobs.S, bfpu.S) */
struct zt *zt_cur;                      /* the running task's record */
u32 zt_switches;
static struct zt zt_first;              /* the program's first process */
static s16 quantum;
extern u8 zm_fpu;                       /* bfpu.S: an FPU (AttnFlags) */
extern const u8 zs_switch_ret[];        /* blobs.S: where a task that switched away goes on */
u32 zt_yield(u32 why);                  /* blobs.S: 1 when it switched (and this task runs again), 0 in supervisor mode */
void zt_idle(void);                     /* blobs.S: stop #$2000 until an interrupt */
extern void zt_ret(void);               /* blobs.S: a task's entry returned (d0 its code) */

u16 zt_dis(void)
{
    u16 s = INTENAR & 0x4000;
    INTENA = 0x4000;
    return s;
}

void zt_en(u16 s)
{
    if (s)
        INTENA = 0xC000;
}

static struct zt *rec(u32 task)
{
    for (struct zt *z = &zt_first; z; z = z->next)
        if (z->task == task)
            return z;
    return 0;
}

/* ready: by priority; front 1 puts it before its equals (a task that was preempted keeps its turn) */
static void ready(u32 t, int front)
{
    T8(t, tc_State) = TS_READY;
    struct znode *n = (struct znode *)t, *s = READY->head;
    while (s->succ && (front ? s->pri > n->pri : s->pri >= n->pri))
        s = s->succ;
    n->succ = s;
    n->pred = s->pred;
    s->pred->succ = n;
    s->pred = n;
    if (n->pri > PRI(THIS))
        zt_need = 1;
}

static void wake_sleepers(void)
{
    u32 now = zb_frames();
    for (struct zt *z = &zt_first; z; z = z->next)
        if (z->sleeping && T8(z->task, tc_State) == TS_WAIT && (s32)(now - z->wake) >= 0) {
            z->sleeping = 0;
            c_Remove((struct znode *)z->task);
            ready(z->task, 0);
        }
}

static void poll(void)
{
    u16 s = zt_dis();
    wake_sleepers();
    zt_en(s);
    zb_timer_poll();
}

/* the vertical blank (zb_irq, VERTB): sleepers and timer requests due woken; the running task's quantum counted */
void zt_tick(void)
{
    poll();
    if (--quantum <= 0) {
        quantum = 0;
        struct znode *h = READY->head;
        if (h->succ && h->pri >= PRI(THIS))
            zt_need = 1;
    }
}

/* the switch on: the first task gets its record (at the first AddTask or CreateNewProc) */
static void start(void)
{
    if (zt_on)
        return;
    zt_first.task = THIS;
    zt_cur = &zt_first;
    T8(THIS, tc_State) = TS_RUN;
    zt_fpu = zm_fpu ? (Z.mmu[0] & 2 ? 2 : 1) : 0;
    if (!EB(ZO_ExecBase_Quantum, u16))
        EB(ZO_ExecBase_Quantum, u16) = 4;
    quantum = (s16)EB(ZO_ExecBase_Quantum, u16);
    zt_on = 1;
}

void zt_check(void)
{
    if (zt_on && zt_need && EB(ZO_ExecBase_TDNestCnt, s8) < 0 && EB(ZO_ExecBase_IDNestCnt, s8) < 0)
        zt_yield(ZT_YIELD);
}

static void free_task(struct zt *z)
{
    u32 t = z->task;
    struct zlist *ml = (struct zlist *)(t + ZO_Task_tc_MemEntry);  /* CreateTask's MemLists: the Task, its stack */
    if (ml->head && ml->tailpred && c_TypeOfMem((u32)ml->head)) {
        struct znode *n = ml->head;
        for (int k = 0; k < 64 && n && n->succ && c_TypeOfMem((u32)n); k++) {
            struct znode *nx = n->succ;
            u16 cnt = *(u16 *)((u8 *)n + 14);
            const u32 *me = (const u32 *)((u8 *)n + 16);
            for (u32 i = 0; i < cnt && i < 64; i++)
                if (me[2 * i] && me[2 * i + 1])
                    zx_FreeMem(me[2 * i], me[2 * i + 1]);
            zx_FreeMem((u32)n, 16 + 8u * (cnt ? cnt : 1));
            n = nx;
        }
    }
    if (z->mem)
        zx_FreeMem(z->mem, z->memsize);
}

static void unlink_rec(struct zt *z)
{
    for (struct zt *p = &zt_first; p; p = p->next)
        if (p->next == z) {
            p->next = z->next;
            return;
        }
}

/* the next task to run, from zt_swout (supervisor mode, interrupts masked): the one that ran is put where it
 * belongs (ready, waiting, or gone), the ready one of the highest priority taken; none: the CPU waits */
struct zt *zt_pick(void);
struct zt *zt_pick(void)
{
    struct zt *o = zt_cur, *dead = 0;
    u32 t = o->task;
    u32 why = o->pc == (u32)zs_switch_ret ? o->r[0] : ZT_YIELD;
    T32(t, tc_SPReg) = o->usp;
    T8(t, tc_IDNestCnt) = EB(ZO_ExecBase_IDNestCnt, s8);
    T8(t, tc_TDNestCnt) = EB(ZO_ExecBase_TDNestCnt, s8);
    if (EB(ZO_ExecBase_IDNestCnt, s8) >= 0)
        INTENA = 0xC000;                /* it waited with interrupts disabled: on while it is away */
    EB(ZO_ExecBase_IDNestCnt, s8) = -1;
    EB(ZO_ExecBase_TDNestCnt, s8) = -1;
    switch (why) {
    case ZT_WAIT:
        if (T32(t, tc_SigRecvd) & T32(t, tc_SigWait))
            ready(t, 1);                /* the signal came in time: it goes on */
        else {
            T8(t, tc_State) = TS_WAIT;
            c_AddTail(WAITING, (struct znode *)t);
        }
        break;
    case ZT_SLEEP:
        T8(t, tc_State) = TS_WAIT;
        c_AddTail(WAITING, (struct znode *)t);
        break;
    case ZT_EXIT:
        T8(t, tc_State) = TS_REMOVED;
        unlink_rec(o);
        dead = o;
        break;
    default:
        ready(t, quantum > 0);          /* its quantum used up: behind its equals */
    }
    struct znode *n;
    for (;;) {
        poll();
        n = READY->head;
        if (n->succ)
            break;
        /* nobody ready: wait for an interrupt, unless none can come and nothing is timed */
        int timed = zb_timer_busy();
        for (struct zt *z = &zt_first; z && !timed; z = z->next)
            timed = z->sleeping;
        if ((INTENAR & 0x4000) && (INTENAR & 0x3FFF))
            zt_idle();
        else if (!timed && !zb_audio_busy())
            zb_stop(ZH_STOP, ZS_WAIT);
    }
    c_Remove(n);
    if (dead)
        free_task(dead);                /* its stack too: this runs on the system stack */
    u32 nt = (u32)n;
    T8(nt, tc_State) = TS_RUN;
    THIS = nt;
    EB(ZO_ExecBase_IDNestCnt, s8) = T8(nt, tc_IDNestCnt);
    EB(ZO_ExecBase_TDNestCnt, s8) = T8(nt, tc_TDNestCnt);
    if (T8(nt, tc_IDNestCnt) >= 0)
        INTENA = 0x4000;
    struct zt *z = rec(nt);
    if (!z)
        zb_stop(ZH_STOP, ZS_WAIT);      /* cannot be: every task in the lists has a record */
    zt_cur = z;
    quantum = (s16)(EB(ZO_ExecBase_Quantum, u16) ? EB(ZO_ExecBase_Quantum, u16) : 4);
    zt_need = 0;
    if (z != o)
        zt_switches++;
    return z;
}

/* a new task into the lists (AddTask, CreateNewProc): its registers 0 but the PC (and d0/a0 for a process), fin
 * the return address on its stack */
static u32 add(u32 t, struct zt *z, u32 pc, u32 fin, u32 d0, u32 a0)
{
    start();
    u32 sp = T32(t, tc_SPReg) - 4;
    *(u32 *)sp = fin;
    z->usp = sp;
    z->pc = pc;
    z->r[0] = d0;
    z->r[8] = a0;
    z->task = t;
    T8(t, tc_Flags) = 0;
    T8(t, tc_IDNestCnt) = -1;
    T8(t, tc_TDNestCnt) = -1;
    T32(t, tc_SigAlloc) |= 0xFFFF;      /* the system's signals, as ExecBase's TaskSigAlloc */
    T32(t, tc_SigWait) = 0;
    T32(t, tc_SPReg) = sp;
    u16 s = zt_dis();
    z->next = zt_first.next;
    zt_first.next = z;
    ready(t, 0);
    zt_en(s);
    return t;
}

/* exec AddTask(task a1, initialPC a2, finalPC a3) (-282) */
u32 c_AddTask(u32 t, u32 pc, u32 fin)
{
    if (!t || !pc || !c_TypeOfMem(t))
        return 0;
    struct zt *z = (struct zt *)zx_AllocMem(sizeof *z, MEMF_PUBLIC | MEMF_CLEAR);
    if (!z)
        return 0;
    z->mem = (u32)z;
    z->memsize = sizeof *z;
    return add(t, z, pc, fin ? fin : (u32)zt_ret, 0, 0);
}

/* the running task ends (its entry returned, dos Exit, RemTask(0)): its exit hook first; never returns */
void zt_exit(u32 code) __attribute__((noreturn));
void zt_exit(u32 code)
{
    if (!zt_on || THIS == Z.proc) {     /* the first process: the session ends (9.1) */
        zr_code = code;
        zb_stop(ZH_RETURN, 0);
    }
    struct zt *z = zt_cur;
    if (z->exitcode)
        zb_libcall(z->exitcode, ZEXEC, code, 0, z->exitdata, 0);
    if (z->seglist)
        c_UnLoadSeg(z->seglist);
    zt_yield(ZT_EXIT);
    zb_stop(ZH_STOP, ZS_WAIT);          /* in supervisor mode: cannot leave */
}

u32 zt_exit_child(u32 code)
{
    if (zt_on && THIS != Z.proc)
        zt_exit(code);
    return code;
}

/* exec RemTask(task a1) (-288): 0 or the caller ends; another task leaves the lists and its memory is freed */
void c_RemTask(u32 t)
{
    if (!t || t == THIS)
        zt_exit(0);
    struct zt *z = zt_on ? rec(t) : 0;
    if (!z)
        return;
    if (t == Z.proc) {                  /* the first process removed by another: the session ends */
        zr_code = 0;
        zb_stop(ZH_RETURN, 0);
    }
    u16 s = zt_dis();
    if (T8(t, tc_State) == TS_READY || T8(t, tc_State) == TS_WAIT)
        c_Remove((struct znode *)t);
    T8(t, tc_State) = TS_REMOVED;
    unlink_rec(z);
    zt_en(s);
    free_task(z);
}

/* ---- the calls that see more than one task (blibs.c keeps the one-task forms) ---- */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

u32 c_FindTask(const char *name)
{
    u32 t = THIS;
    if (!name)
        return t;
    const char *n = ((struct znode *)t)->name;
    if (n && streq(name, n))
        return t;
    if (!zt_on)
        return 0;
    u16 s = zt_dis();
    u32 r = c_FindName(READY, name);
    if (!r)
        r = c_FindName(WAITING, name);
    zt_en(s);
    return r;
}

u32 c_SetTaskPri(u32 t, u32 pri)
{
    if (!t)
        return 0;
    u16 s = zt_dis();
    s8 old = PRI(t);
    PRI(t) = (s8)pri;
    if (zt_on && T8(t, tc_State) == TS_READY) {
        c_Remove((struct znode *)t);
        ready(t, 0);
    }
    if (zt_on && t == THIS && READY->head->succ && READY->head->pri > (s8)pri)
        zt_need = 1;
    zt_en(s);
    return (u32)(s32)old;
}

void c_Signal(u32 t, u32 sigs)
{
    if (!zt_on) {
        if (t == Z.proc)
            T32(t, tc_SigRecvd) |= sigs;
        return;
    }
    if (!t)
        return;
    u16 s = zt_dis();
    T32(t, tc_SigRecvd) |= sigs;
    struct zt *z = T8(t, tc_State) == TS_WAIT ? rec(t) : 0;
    if (z && !z->sleeping && (T32(t, tc_SigRecvd) & T32(t, tc_SigWait))) {
        c_Remove((struct znode *)t);
        ready(t, 0);
    }
    zt_en(s);
}

u32 c_SetSignal(u32 sigs, u32 mask)
{
    u32 t = THIS;
    u16 s = zt_dis();
    u32 old = T32(t, tc_SigRecvd);
    T32(t, tc_SigRecvd) = (old & ~mask) | (sigs & mask);
    zt_en(s);
    return old;
}

/* Wait: with one task only an interrupt server can send the signal, so the CPU goes round until one does; with
 * interrupts disabled and nothing timed nothing can (5.2). With more tasks the caller waits in TaskWait. */
u32 c_Wait(u32 sigs)
{
    u32 t = THIS;
    for (;;) {
        u16 s = zt_dis();
        u32 got = T32(t, tc_SigRecvd) & sigs;
        if (got)
            T32(t, tc_SigRecvd) &= ~got;
        zt_en(s);
        if (got)
            return got;
        if (zt_on && sigs) {
            T32(t, tc_SigWait) = sigs;
            if (zt_yield(ZT_WAIT))
                continue;
            poll();                     /* in supervisor mode: no switch, so round as for one task */
            continue;
        }
        if (zb_timer_poll())            /* a timer request came due: its signal, then again */
            continue;
        if (!zb_timer_busy() && !zb_audio_busy() && (!sigs || EB(ZO_ExecBase_IDNestCnt, s8) >= 0))
            zb_stop(ZH_STOP, ZS_WAIT);
    }
}

/* Delay and WaitTOF with more than one task: the caller sleeps until its frame comes (the vertical blank or the
 * idle loop wakes it); 0 with one task (the caller waits by the beam as before) */
int zt_sleep(u32 frames)
{
    if (!zt_on)
        return 0;
    struct zt *z = zt_cur;
    z->wake = zb_frames() + frames;
    z->sleeping = 1;
    if (zt_yield(ZT_SLEEP))
        return 1;
    z->sleeping = 0;
    return 0;
}

/* ---- dos CreateNewProc (-498) and CreateProc (-138): a process of the creator's kind, from NP_Entry or a
 * segment list (dos/dostags.h, NDK 3.2) ---- */
#define NP(n) (0x80000000u + 1000 + (n))
#define MP_SIGBIT 15
extern u32 zb_ioerr_set(u32 e);        /* blibs.c: IoErr's value */
u32 c_NextTagItem(u32 **pp);

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static u32 new_proc(u32 seg, u32 entry, u32 freeseg, u32 stack, const char *name, s32 pri, u32 args, u32 in,
                    u32 out, u32 cd, u32 win, u32 ecode, u32 edata, int has_in, int has_out, int has_cd)
{
    if (seg)
        entry = (seg << 2) + 4;
    if (!entry) {
        zb_ioerr_set(116);              /* ERROR_REQUIRED_ARG_MISSING: neither NP_Seglist nor NP_Entry */
        return 0;
    }
    if (stack < 1024)
        stack = 1024;
    stack = (stack + 3) & ~3u;
    if (!name || !c_TypeOfMem((u32)name))
        name = "New Process";
    u32 nl = (slen(name) + 4) & ~3u;
    u32 size = sizeof(struct zt) + ZO_Process_SIZEOF + nl + stack;
    u32 m = zx_AllocMem(size, MEMF_PUBLIC | MEMF_CLEAR);
    if (!m) {
        zb_ioerr_set(103);              /* ERROR_NO_FREE_STORE */
        return 0;
    }
    struct zt *z = (struct zt *)m;
    u8 *pr = (u8 *)m + sizeof *z;
    char *nm = (char *)pr + ZO_Process_SIZEOF;
    u32 lo = (u32)nm + nl, top = lo + stack;
    zmemcpy(nm, name, slen(name) + 1);
    z->mem = m;
    z->memsize = size;
    z->exitcode = ecode;
    z->exitdata = edata;
    z->seglist = freeseg ? seg : 0;
    struct znode *pn = (struct znode *)pr;
    pn->type = NT_PROCESS;
    pn->pri = (s8)pri;
    pn->name = nm;
    u8 *mp = pr + ZO_Process_pr_MsgPort;
    struct zlist *l = (struct zlist *)(mp + ZO_MsgPort_mp_MsgList);
    l->head = (struct znode *)&l->tail;
    l->tailpred = (struct znode *)&l->head;
    l->type = NT_MESSAGE;
    mp[ZO_Node_ln_Type] = NT_MSGPORT;
    *(u32 *)(mp + ZO_MsgPort_mp_SigTask) = (u32)pr;
    mp[MP_SIGBIT] = 8;                  /* SIGB_DOS */
    u32 me = THIS;
    int creator_proc = ((struct znode *)me)->type == NT_PROCESS;
    *(u32 *)(pr + ZO_Process_pr_WindowPtr) = win;
    *(u32 *)(pr + ZO_Process_pr_CIS) = has_in ? in : Z.in_fh;
    *(u32 *)(pr + ZO_Process_pr_COS) = has_out ? out : Z.out_fh;
    *(u32 *)(pr + ZO_Process_pr_CurrentDir) = has_cd ? cd : creator_proc ?
        *(u32 *)(me + ZO_Process_pr_CurrentDir) : 0;
    *(u32 *)(pr + ZO_Process_pr_StackSize) = stack;
    *(u32 *)(pr + ZO_Process_pr_StackBase) = top >> 2;
    *(u32 *)(pr + ZO_Process_pr_Arguments) = args;
    *(u32 *)(pr + ZO_Process_pr_ExitCode) = ecode;
    *(u32 *)(pr + ZO_Process_pr_ExitData) = edata;
    *(u32 *)(pr + ZO_Task_tc_SPLower) = lo;
    *(u32 *)(pr + ZO_Task_tc_SPUpper) = top;
    *(u32 *)(top - 4) = stack;          /* 4(sp): the stack's size, as a Shell starts a program */
    *(u32 *)(pr + ZO_Task_tc_SPReg) = top - 4;
    u32 al = args && c_TypeOfMem(args) ? slen((const char *)args) : 0;
    zb_ioerr_set(0);
    return add((u32)pr, z, entry, (u32)zt_ret, al, args);
}

u32 c_CreateNewProc(u32 *tags)
{
    u32 seg = 0, entry = 0, freeseg = 0, stack = 4000, args = 0, in = 0, out = 0, cd = 0, win = 0, ec = 0, ed = 0;
    s32 pri = 0;
    int hi = 0, ho = 0, hc = 0;
    const char *name = 0;
    u32 *t = c_TypeOfMem((u32)tags) ? tags : 0, *ti;
    for (u32 n = 0; n < 256 && (ti = (u32 *)c_NextTagItem(&t)); n++)
        switch (ti[0]) {
        case NP(1): seg = ti[1]; break;
        case NP(2): freeseg = ti[1]; break;
        case NP(3): entry = ti[1]; break;
        case NP(4): in = ti[1], hi = 1; break;
        case NP(5): out = ti[1], ho = 1; break;
        case NP(10): cd = ti[1], hc = 1; break;
        case NP(11): stack = ti[1]; break;
        case NP(12): name = (const char *)ti[1]; break;
        case NP(13): pri = (s32)ti[1]; break;
        case NP(15): win = ti[1]; break;
        case NP(21): args = ti[1]; break;
        case NP(24): ec = ti[1]; break;
        case NP(25): ed = ti[1]; break;
        }
    return new_proc(seg, entry, freeseg, stack, name, pri, args, in, out, cd, win, ec, ed, hi, ho, hc);
}

/* CreateProc(name d1, pri d2, seglist d3, stack d4): the process's pr_MsgPort, its "process identifier" */
u32 c_CreateProc(u32 name, u32 pri, u32 seg, u32 stack)
{
    u32 pr = seg ? new_proc(seg, 0, 0, stack, (const char *)name, (s32)pri, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) : 0;
    if (!seg)
        zb_ioerr_set(116);
    return pr ? pr + ZO_Process_pr_MsgPort : 0;
}
