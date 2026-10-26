/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of DESIGN D16's core (kernel/event.c, kernel/signal.c), built 32-bit against a model of the scheduler:
 * the wake rule (section 14 rule 1) for every state; slot masks with stale bits; the wait's order (section 3) under
 * every interleaving of one producer and the waiter - before the registration, between it and the check, between the
 * check and the masked test, after the block - never a lost wake-up; signal sending (defaults, pid 1, blocked, caught,
 * SIGCONT, groups and their results); delivery (the frame, the stack room, SA_RESTART, the masks); and sigreturn's
 * checks (a forged SR of 0x0700, 0x1000 or 0x2700 keeps only the condition codes, an odd PC kills, a frame outside the
 * process is refused before a byte of it is read: it lies on an unmapped page here, so a read would crash the test).
 * Build and run: tools/signal-host-test.sh */
#include <stdio.h>
#include <setjmp.h>
#include <sys/mman.h>
#include "../kernel/event.c"
#include "../kernel/signal.c"

void exit(int) __attribute__((noreturn));
int printf(const char *, ...);
void *memcpy(void *d, const void *s, unsigned long n) { char *a = d; const char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { char *a = d; while (n--) *a++ = (char)c; return d; }

/* ---- the model ---- */
struct proc procs[NPROC];
struct proc *current;
volatile u32 ticks, wake_min;
int ntimer;
static int readied[NPROC], schedules, blocked_in_ev, killed_exit = -1, stopped_calls, cont_calls, kills;
static jmp_buf out;
static void (*producer_hook)(void);     /* runs once at the next irq_save: "just before the masked test" */
u16 irq_save(void) { if (producer_hook) { void (*h)(void) = producer_hook; producer_hook = 0; h(); } return 0; }
void irq_restore(u16 s) { (void)s; }
void ready(struct proc *p)
{
    if (p->state == P_STOP || p->state == P_READY || p->state == P_FREE || p->state == P_ZOMBIE)
        return;
    if (p->state == P_SLEEP)
        ntimer--;
    p->state = P_READY;
    readied[p - procs]++;
}
static void (*sched_hook)(void);        /* runs once while the waiter sleeps: another process's producer */
void schedule(void)
{
    schedules++;
    if (current->state == P_EV || current->state == P_SLEEP)
        blocked_in_ev++;                /* it went to sleep: someone must ready it later */
    if (sched_hook) {
        void (*h)(void) = sched_hook;
        sched_hook = 0;
        struct proc *w = current;
        current = &procs[2];
        procs[2].state = P_READY;
        h();
        current = w;
    }
}
void sleep_until(u32 t) { current->wake = t; current->state = P_SLEEP; ntimer++; }
void proc_stop(struct proc *p) { stopped_calls++; p->stop_req = 1; }
void proc_cont(struct proc *p) { cont_calls++; p->stop_req = 0; if (p->state == P_STOP) p->state = P_READY; }
void proc_kill(struct proc *p) { kills++; if (p->killed == 1) p->killed = 3; else if (!p->killed) p->killed = 1; }
int proc_kill_now(void) { return current->killed != 0; }
int kill_status(const struct proc *p) { return p->killed == 2 ? 139 : p->kill_sig ? 128 + p->kill_sig : 130; }
void proc_exit(int code) { killed_exit = code; longjmp(out, 1); }
static u32 mem_lo, mem_hi;              /* the process's memory in the model */
int user_ok_wr(const struct proc *p, u32 ptr, u32 len) { (void)p; return ptr >= mem_lo && ptr + len <= mem_hi && ptr + len >= ptr; }
int user_ok_rd(const struct proc *p, u32 ptr, u32 len) { return user_ok_wr(p, ptr, len); }
static u32 usp_reg;
u32 get_usp(void) { return usp_reg; }
void set_usp(u32 u) { usp_reg = u; }
void kprintf(const char *fmt, ...) { (void)fmt; }
int may_signal(const struct proc *p) { return p->uid == 7 ? -EACCES : 0; }
struct proc *proc_by_pid(int pid) { for (int i = 0; i < NPROC; i++) if (procs[i].state && procs[i].pid == pid) return &procs[i]; return 0; }
static int data_ready;                  /* the object of the interleaving test: a tty with a line or not */
static u32 obj_mask;
u16 vfs_poll(struct file *f) { (void)f; ev_arm(&obj_mask); return data_ready ? POLLIN : 0; }
u16 port_poll(int port) { (void)port; return POLLNVAL; }

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static void clear(void)
{
    memset(procs, 0, sizeof procs);
    memset(readied, 0, sizeof readied);
    for (int i = 0; i < NPROC; i++) { procs[i].pid = i ? 100 + i : 0; procs[i].pgid = procs[i].pid; }
    schedules = blocked_in_ev = stopped_calls = cont_calls = kills = 0;
    killed_exit = -1;
    ntimer = 0;
    current = &procs[1];
    procs[1].state = P_READY;
}

/* the producer of the interleaving test: a line arrives and its object wakes the waiters */
static void produce(void) { data_ready = 1; ev_wake(&obj_mask); }

/* the waiter: tty_read's loop, with the producer at point `at` (0 before the registration, 1 between it and the
 * check, 2 between the check and the masked test, 3 after it blocked); 1 if the line was found or the waiter was
 * woken to find it, 0 if it would sleep for ever */
static int waiter(int at)
{
    clear();
    data_ready = 0;
    obj_mask = 0;
    struct proc *w = current;
    for (int round = 0; round < 3; round++) {
        if (at == 0 && round == 0) produce();
        ev_arm(&obj_mask);
        if (at == 1 && round == 0) produce();
        if (data_ready)
            return 1;
        if (at == 2 && round == 0) producer_hook = produce;
        if (at == 3 && round == 0) sched_hook = produce;    /* another process, while this one sleeps */
        int before = blocked_in_ev;
        ev_block(EV_IO, 0, 0, 1);
        if (blocked_in_ev > before && w->state != P_READY)
            return 0;                   /* asleep with nobody to wake it: a lost wake-up */
        w->state = P_READY;             /* it runs again */
    }
    return data_ready;
}

int main(void)
{
    /* 1. the wake rule (rule 1) */
    clear();
    struct proc *p = &procs[3];
    p->state = P_EV; p->ev_wait = EV_IO | EV_SIG;
    ev_post(p, EV_CHILD);
    EXPECT_TRUE(p->state == P_EV && !readied[3], "rule 1: a bit it does not wait for leaves it asleep (kept pending)");
    ev_post(p, EV_IO);
    EXPECT_TRUE(p->state == P_READY && readied[3] == 1, "... one it waits for readies it");
    p->state = P_SLEEP; p->ev_wait = 0; ntimer = 1;
    ev_post(p, EV_SIG);
    EXPECT_TRUE(p->state == P_SLEEP, "a plain sleep (ev_wait 0) is never woken by an event");
    p->ev_wait = EV_SIG; p->ev_pending = 0;
    ev_post(p, EV_SIG);
    EXPECT_TRUE(p->state == P_READY && ntimer == 0, "a timed event wait (P_SLEEP with ev_wait) is");
    p->state = P_STOP; p->ev_wait = EV_IO;
    ev_post(p, EV_IO);
    EXPECT_TRUE(p->state == P_STOP, "a stopped process is never readied by an event");
    p->state = P_MSG; p->ev_wait = 0;
    ev_post(p, EV_IO);
    EXPECT_TRUE(p->state == P_MSG, "nor a client in a server call");

    /* 2. slot masks with stale bits */
    clear();
    procs[4].state = P_EV; procs[4].ev_wait = EV_IO;
    procs[5].state = P_READY;           /* left its wait */
    procs[6].state = P_FREE;            /* died */
    procs[7].state = P_SLEEP;           /* a plain sleep now */
    u32 mask = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
    ev_wake(&mask);
    EXPECT_TRUE(mask == 0 && procs[4].state == P_READY && readied[4] == 1, "a mask wakes the slot that waits on it");
    EXPECT_TRUE(!readied[5] && procs[6].state == P_FREE && procs[7].state == P_SLEEP,
                "stale bits (a process that left, one that died, one in a plain sleep) do nothing");

    /* 3. the wait's order under every interleaving with one producer */
    static const char *const where[] = { "before the registration", "between the registration and the check",
                                         "between the check and the masked test", "after the block" };
    for (int at = 0; at < 4; at++) {
        char msg[120];
        snprintf(msg, sizeof msg, "no lost wake-up with the producer %s", where[at]);
        EXPECT_TRUE(waiter(at), msg);
    }
    clear();
    current->killed = 1;
    EXPECT_TRUE(ev_block(EV_IO, 0, 0, 1) == -EINTR && !schedules, "the masked test refuses to block when killed");
    clear();
    current->sig_catch = current->sig_pend = SIGBIT(SIGUSR1);
    EXPECT_TRUE(ev_block(EV_IO, 0, 0, 1) == -EINTR && current->sig_intr == 1 && !blocked_in_ev,
                "... or with a caught unblocked signal pending (and marks the call restartable)");
    clear();
    current->sig_catch = current->sig_pend = current->sig_block = SIGBIT(SIGUSR1);
    procs[1].state = P_READY;
    int b0 = blocked_in_ev;
    ev_block(EV_IO, 0, 0, 1);
    EXPECT_TRUE(blocked_in_ev == b0 + 1, "... but a blocked one does not stop the wait");
    clear();
    current->sig_catch = current->sig_pend = SIGBIT(SIGUSR1);
    ev_post(current, EV_SIG);           /* the wake-up of a signal, then the handler ran (nothing pending) */
    current->sig_pend = 0;
    b0 = blocked_in_ev;
    ev_block(0, 1, 100, 0);
    EXPECT_TRUE(blocked_in_ev == b0 + 1, "a signal's wake-up bit never goes stale: the next sleep still sleeps");
    clear();
    current->stop_req = 1;
    ev_block(EV_IO, 0, 0, 1);
    EXPECT_TRUE(!blocked_in_ev && schedules == 1, "a stop asked for: no block, it goes through schedule (to stop)");

    /* 4. sending */
    clear();
    p = &procs[3]; p->state = P_EV; p->ev_wait = EV_IO;
    sig_send(p, SIGCHLD);
    EXPECT_TRUE(!kills && p->state == P_EV && !p->sig_pend, "SIGCHLD by default: nothing at all, no wake-up");
    sig_send(p, SIGTERM);
    EXPECT_TRUE(kills == 1 && p->kill_sig == SIGTERM && kill_status(p) == 143, "SIGTERM by default ends it: 143");
    clear();
    p = &procs[1]; p->pid = 1;
    sig_send(p, SIGTERM); sig_send(p, SIGTSTP);
    EXPECT_TRUE(!kills && !stopped_calls, "rule 6: pid 1 (boot) is never ended or stopped by a default action");
    clear();
    p = &procs[3]; p->state = P_EV; p->ev_wait = EV_IO | EV_SIG;      /* as ev_block sets it */
    p->sig_catch = SIGBIT(SIGUSR1);
    sig_send(p, SIGUSR1);
    EXPECT_TRUE(p->sig_pend == SIGBIT(SIGUSR1) && p->state == P_READY, "a caught one is pending and wakes the waiter");
    clear();
    p = &procs[3]; p->state = P_EV; p->ev_wait = EV_IO;
    p->sig_catch = p->sig_block = SIGBIT(SIGUSR1);
    sig_send(p, SIGUSR1);
    EXPECT_TRUE(p->sig_pend == SIGBIT(SIGUSR1) && p->state == P_EV, "a caught and blocked one is pending, no wake-up");
    clear();
    p = &procs[3]; p->state = P_EV; p->sig_block = SIGBIT(SIGTERM);
    sig_send(p, SIGTERM);
    EXPECT_TRUE(!kills && p->sig_pend == SIGBIT(SIGTERM), "a blocked SIGTERM waits ...");
    current = p; p->state = P_READY;
    sig_procmask(SIG_UNBLOCK, SIGBIT(SIGTERM), 0);
    EXPECT_TRUE(kills == 1 && !p->sig_pend, "... and ends it when unblocked");
    clear();
    p = &procs[3]; p->state = P_STOP; p->sig_pend = SIGBIT(SIGTSTP);
    p->sig_catch = SIGBIT(SIGCONT);
    sig_send(p, SIGCONT);
    EXPECT_TRUE(cont_calls == 1 && p->sig_pend == SIGBIT(SIGCONT), "SIGCONT continues, drops pending stops, then its "
                "handler is pending");
    clear();
    p = &procs[3]; p->state = P_READY; p->sig_ign = SIGBIT(SIGKILL) | SIGBIT(SIGSTOP); p->sig_block = SIGBIT(SIGKILL);
    sig_send(p, SIGSTOP);
    sig_send(p, SIGKILL);
    EXPECT_TRUE(stopped_calls == 1 && kills == 1 && p->killed == 3 && kill_status(p) == 137,
                "SIGSTOP and SIGKILL are never ignored or blocked; SIGKILL acts at once (137)");
    clear();
    for (int i = 3; i < 6; i++) { procs[i].state = P_READY; procs[i].pgid = 50; }
    procs[4].uid = 7;                   /* not allowed */
    EXPECT_TRUE(sig_group(50, SIGTERM, 1) == 0 && kills == 2, "a group: every member the caller may signal (rule 15: 0)");
    clear();
    procs[3].state = P_READY; procs[3].pgid = 60; procs[3].uid = 7;
    EXPECT_TRUE(sig_group(60, SIGTERM, 1) == -EPERM && !kills, "... none allowed: -EPERM");
    EXPECT_TRUE(sig_group(61, SIGTERM, 1) == -ESRCH, "... an empty group: -ESRCH");
    clear();
    procs[1].state = P_READY;
    EXPECT_TRUE(sig_action(SIGKILL, 0) == 0 && sig_action(SIGSTOP, 0) == 0, "sigaction may ask about KILL and STOP");

    /* 5. delivery and sigreturn, in a memory block mapped for the test, with an unmapped page below it */
    u8 *m = mmap((void *)0x20000000, 0x6000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)0x20000000) { printf("FAIL: cannot map the test memory\n"); return 1; }
    munmap(m, 0x1000);                  /* 0x20000000..0x20000FFF: unmapped */
    mem_lo = 0x20001000; mem_hi = 0x20006000;
    clear();
    struct proc *c = current;
    c->ustk = 0x20002000; c->base = 0x20001000; c->span = 0x5000;
    c->sig_entry = 0x20001100; c->a5 = 0x20001800;
    c->sig_catch = SIGBIT(SIGUSR1); c->sig_pend = SIGBIT(SIGUSR1); c->sig_rest = SIGBIT(SIGUSR1);
    c->sig_mask[SIGUSR1] = SIGBIT(SIGUSR2) | SIGBIT(SIGKILL);
    c->sig_intr = 1;
    u32 rb[17];
    for (int i = 0; i < 15; i++) rb[i] = 0x1000 + (u32)i;
    rb[0] = (u32)-EINTR;
    u16 *srw = (u16 *)(rb + 15);
    srw[0] = 0x0004;                    /* user mode, Z set */
    *(u32 *)((u8 *)rb + 62) = 0x20001200;
    usp_reg = 0x20004001;               /* odd: the frame aligns down */
    if (!setjmp(out)) sig_deliver(rb, SYS_read);
    struct sigframe *sf = (struct sigframe *)usp_reg;
    EXPECT_TRUE(killed_exit == -1 && usp_reg == ((0x20004001u - sizeof *sf) & ~3u), "the frame goes below the stack "
                "pointer, aligned to 4");
    EXPECT_TRUE(sf->size == SIGFRAME_FIRST && sf->regs[15] == 0x20004001 && sf->ccr == 4 && sf->sig == SIGUSR1,
                "... size first, the old stack pointer, the condition codes, the signal");
    EXPECT_TRUE(sf->pc == 0x200011FE && sf->regs[0] == SYS_read, "SA_RESTART after an interrupted call: the PC back "
                "on the trap, d0 the call's number");
    EXPECT_TRUE(*(u32 *)((u8 *)rb + 62) == 0x20001100 && rb[0] == SIGUSR1 && rb[8] == usp_reg && rb[13] == 0x20001800,
                "into the entry: d0 the signal, a0 the frame, a5 the program's data");
    EXPECT_TRUE(c->sig_block == (SIGBIT(SIGUSR1) | SIGBIT(SIGUSR2)) && !c->sig_pend && !c->sig_intr,
                "the signal and its sa_mask blocked (never SIGKILL), nothing pending");
    /* sigreturn of that frame, with a forged status register */
    u32 f = usp_reg;
    sf->ccr = 0x2700 | 0x1F;
    sf->blocked = SIGBIT(SIGSTOP) | SIGBIT(SIGHUP);
    srw[0] = 0;
    volatile long r = 0;
    if (!setjmp(out)) r = sig_return(rb, f);
    EXPECT_TRUE(killed_exit == -1 && srw[0] == 0x1F && r == SYS_read && usp_reg == 0x20004001 && rb[1] == 0x1001 &&
                *(u32 *)((u8 *)rb + 62) == 0x200011FE, "sigreturn: registers, stack pointer and PC back, d0 returned");
    EXPECT_TRUE(c->sig_block == SIGBIT(SIGHUP), "... the blocked mask back, never with SIGSTOP");
    sf->ccr = 0x0700; if (!setjmp(out)) sig_return(rb, f);
    EXPECT_TRUE(srw[0] == 0, "a forged SR of 0x0700 (every interrupt masked) keeps only the condition codes");
    sf->ccr = 0x1000; if (!setjmp(out)) sig_return(rb, f);
    EXPECT_TRUE(srw[0] == 0, "... and one of 0x1000 (the M bit)");
    sf->pc = 0x20001201; killed_exit = -1;
    if (!setjmp(out)) sig_return(rb, f);
    EXPECT_TRUE(killed_exit == 139, "an odd PC kills it (139)");
    killed_exit = -1;
    if (!setjmp(out)) sig_return(rb, 0x20000F00);   /* unmapped: a read would crash this test */
    EXPECT_TRUE(killed_exit == 139, "a frame outside the process is refused (killed) before it is read");
    killed_exit = -1;
    if (!setjmp(out)) sig_return(rb, 0x20005FD0);   /* only 48 bytes left in the process */
    EXPECT_TRUE(killed_exit == 139, "... also one that runs past the process's end");
    /* the stack room (rule 5) */
    clear();
    c = current;
    c->ustk = 0x20002000; c->base = 0x20001000; c->span = 0x5000; c->sig_entry = 0x20001100;
    c->sig_catch = c->sig_pend = SIGBIT(SIGUSR1);
    usp_reg = 0x20002000 + SIG_STACK_ROOM + sizeof(struct sigframe) - 4;
    if (!setjmp(out)) sig_deliver(rb, 0);
    EXPECT_TRUE(killed_exit == 139, "a frame that would leave less than 512 bytes above the guard: killed (139)");
    clear();
    c = current;
    c->ustk = 0x20002000; c->base = 0x20001000; c->span = 0x5000; c->sig_entry = 0x20001100;
    c->sig_catch = c->sig_pend = SIGBIT(SIGUSR1);
    usp_reg = 0x20002000 + SIG_STACK_ROOM + sizeof(struct sigframe);
    if (!setjmp(out)) sig_deliver(rb, 0);
    EXPECT_TRUE(killed_exit == -1 && usp_reg == 0x20002000 + SIG_STACK_ROOM, "... with exactly that room it is sent");
    clear();
    c = current;
    c->killed = 1;
    c->sig_catch = c->sig_pend = SIGBIT(SIGUSR1);
    if (!setjmp(out)) sig_deliver(rb, 0);
    EXPECT_TRUE(killed_exit == 130, "rule 3: a kill is acted on before any handler");

    printf("%s: %d failure(s)\n", bad ? "FAIL" : "PASS", bad);
    return bad != 0;
}
