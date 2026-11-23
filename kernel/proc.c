/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "mmu.h"
#include "shebang.h"

/* Processes. No MMU: all processes share one address space, so the
 * kernel never trusts a user pointer (user_ok_rd/wr) and contains a crashing
 * process by killing it and giving back everything it owned. */

struct proc procs[NPROC];
struct proc *current;
static int next_pid = 1;
int ntimer;                             /* how many are in P_SLEEP, plus the alarms set (D16 rule 3) */
volatile u32 wake_min;                  /* while ntimer: no sleeper or alarm is due before this tick */
volatile u8 need_resched;               /* a better process than the running one is ready, or a sleeper is due */

/* ---- the run queue (DESIGN D10) ----
 * One list per level (0 the highest) and a mask of the non-empty ones, level L as bit 31 - L, so that the 68020's
 * BFFFO (find the first set bit from the top; on every CPU up to the 68060) gives the best level in one instruction:
 * O(1). Round robin within a level: a process joins at the back. pid 0 (idle) is never queued; it runs when the mask is
 * empty. Changed with interrupts masked where an interrupt path could also reach it (ready). */
static struct proc *rq_head[NLEVEL], *rq_tail[NLEVEL];
static u32 rq_mask;

static inline int rq_top(void)          /* the best non-empty level, NLEVEL if none */
{
    int off;
    __asm__("bfffo %1{#0:#32},%0" : "=d"(off) : "d"(rq_mask) : "cc");
    return off;
}

static void rq_push(struct proc *p)
{
    u8 l = p->prio;
    p->rnext = 0;
    p->queued = 1;
    p->qlevel = l;
    if (rq_tail[l])
        rq_tail[l]->rnext = p;
    else
        rq_head[l] = p;
    rq_tail[l] = p;
    rq_mask |= 0x80000000u >> l;
}

static void rq_remove(struct proc *p)   /* p is queued: take it out of its level's list (short lists: a walk) */
{
    u8 l = p->qlevel;
    struct proc *prev = 0;
    for (struct proc *q = rq_head[l]; q && q != p; q = q->rnext)
        prev = q;
    if (prev)
        prev->rnext = p->rnext;
    else
        rq_head[l] = p->rnext;
    if (rq_tail[l] == p)
        rq_tail[l] = prev;
    if (!rq_head[l])
        rq_mask &= ~(0x80000000u >> l);
    p->rnext = 0;
    p->queued = 0;
}

/* a ready process better than the running one: switch at the next return to user mode (kwork's points) */
static inline void want_switch(const struct proc *p)
{
    if (p->prio < current->prio)
        need_resched = 1;
}

/* make a blocked process runnable (never call this on the running one). ready() leaves a stopped process stopped
 * (DESIGN D16 section 14 rule 2): a waker - a message for a server stopped in receive (the port's waiter still names
 * it), a reply, a timer - must not run it; only proc_cont and proc_kill end a stop, through ready_force. */
static void ready_(struct proc *p, int force)
{
    u16 s = irq_save();                 /* interrupts wake processes too */
    if (p->state != P_READY && p->state != P_FREE && p->state != P_ZOMBIE && (force || p->state != P_STOP)) {
        if (p->state == P_SLEEP)
            ntimer--;
        p->state = P_READY;
        if (p->pid != 0 && !p->queued) {
            rq_push(p);
            want_switch(p);
        }
    }
    irq_restore(s);
}
void ready(struct proc *p) { ready_(p, 0); }
static void ready_force(struct proc *p) { ready_(p, 1); }

/* the effective level of p becomes l: a queued p moves to that level's list; a client in a call passes the change on
 * to the server of its request (fsd calling diskd for a client: the chain is short, depth caps it) */
static void prio_set(struct proc *p, u8 l, int depth)
{
    u8 old = p->prio;
    if (l == old)
        return;
    u16 s = irq_save();
    if (p->queued) {
        rq_remove(p);
        p->prio = l;
        rq_push(p);
        want_switch(p);
    } else {
        p->prio = l;
        if (p == current && rq_mask && rq_top() < l)
            need_resched = 1;           /* it dropped below a ready one */
    }
    irq_restore(s);
#ifndef NO_PRIO_INHERIT
    if (p->state == P_MSG && p->wait_msg && depth < 8) {
        struct umsg *m = p->wait_msg;
        struct proc *srv = m->state == MSG_SERVING ? proc_by_pid(m->server) : 0;
        if (m->state == MSG_QUEUED)
            srv = port_owner(p->wait_port);
        if (srv) {
            if (l < srv->prio)
                prio_set(srv, l, depth + 1);
            else if (old == srv->prio && srv->prio < srv->blevel) {   /* its boost may have come from p */
                u8 c = ipc_best_client(srv->pid);
                prio_set(srv, c < srv->blevel ? c : srv->blevel, depth + 1);
            }
        }
    }
#else
    (void)depth;
#endif
}

/* QNX-style priority inheritance (DESIGN D10): a server holding or about to receive a request from a client at level l
 * runs at least at l until it replies; then it drops to its base or to the best client still waiting for it */
void prio_raise(struct proc *p, u8 l)
{
#ifndef NO_PRIO_INHERIT                 /* a test-only build (tools/prio-test.sh) shows what inheritance prevents */
    if (l < p->prio)
        prio_set(p, l, 0);
#else
    (void)p; (void)l;
#endif
}

void prio_recalc(struct proc *p)
{
    u8 l = p->blevel;
#ifndef NO_PRIO_INHERIT
    if (p->prio < l) {                  /* only a boosted one has anything to recompute */
        u8 c = ipc_best_client(p->pid);
        if (c < l)
            l = c;
    }
#endif
    prio_set(p, l, 0);
}

/* the base priority (nice) of p; its effective level follows, keeping a boost it has while serving */
int proc_renice(struct proc *p, int nice)
{
    if (nice < NICE_MIN)
        nice = NICE_MIN;
    if (nice > NICE_MAX)
        nice = NICE_MAX;
    p->nice = (s8)nice;
    p->blevel = NICE_LEVEL(nice);
    if (p->pid != 0)
        prio_recalc(p);                 /* the new base, or a client's level while it serves a better one */
    return nice;
}

/* current goes to sleep until tick t (the caller schedules); wake_min lets the tick and schedule() skip the scan of
 * the process table until a sleeper is due */
void sleep_until(u32 t)
{
    u16 s = irq_save();
    current->wake = t;
    current->state = P_SLEEP;
    if (ntimer++ == 0 || (s32)(t - wake_min) < 0)
        wake_min = t;
    irq_restore(s);
}

/* ready every sleeper that is due (each to its own level), send SIGALRM for every alarm that is due (D16 rule 3: every
 * live process with one, also a running one: a CPU-bound program gets it at the tick), and note when the next one is;
 * interrupts masked */
static void wake_sleepers(void)
{
    if (!ntimer || (s32)(ticks - wake_min) < 0)
        return;
    u32 next = ticks + 0x7FFFFFFFu;
    for (int i = 1; i < NPROC; i++) {
        struct proc *q = &procs[i];
        if (q->alarm_on) {
            if ((s32)(ticks - q->alarm_at) >= 0) {
                q->alarm_on = 0;
                ntimer--;
                sig_send(q, SIGALRM);
            } else if ((s32)(q->alarm_at - next) < 0)
                next = q->alarm_at;
        }
        if (q->state != P_SLEEP)
            continue;
        if ((s32)(ticks - q->wake) >= 0)
            ready(q);
        else if ((s32)(q->wake - next) < 0)
            next = q->wake;
    }
    wake_min = next;
}
static int slice;
#define KSTACK 4096u
#define USTACK_DEFAULT 16384u

/* ---- the stack guard (future-ideas.md section 3, a fix since 2026-10-23) ----
 * Without an MMU nothing stops a stack from growing past its bottom into the memory below (the program's own bss for a
 * user stack, another block of the kernel heap for a kernel stack). So a pattern of GUARD_WORDS long words lies at the
 * bottom of every stack, and the stack pointers are compared with the stack's bounds: at every task switch (the
 * process leaving the CPU, and the saved pointers of the one coming in), on every system call, at the tick when it
 * interrupts user code, and when a program faults. A process that failed is told off on the kernel log - "pid N
 * (name) overflowed its user stack" - and killed (exit 139, as for a fault); pid 0 (idle, the boot stack, where
 * kmain and the interrupts taken while idle run) panics with the text in the kernel log, so the next boot shows it.
 * This finds an overflow, it cannot prevent one: what was written below the stack before the next check stays
 * written. A deep recursion that makes system calls is caught within one frame; one that never enters the kernel
 * within one tick (10 ms). */
#define GUARD_WORDS 4
#define GUARD_BYTES (GUARD_WORDS * 4u)
#define KSP_SLACK   64u                 /* what switch_to and its caller still push below the C frame that checks */
static const u32 guard_pat[GUARD_WORDS] = { STACK_GUARD0, 0xA5ACABB4u, 0x4B53545Au, 0xB4ABACA5u };   /* "ZSTK" ... */

static void guard_set(u32 bottom)
{
    u32 *g = (u32 *)bottom;
    for (int i = 0; i < GUARD_WORDS; i++)
        g[i] = guard_pat[i];
}

static int guard_ok(u32 bottom)
{
    const u32 *g = (const u32 *)bottom;
    return g[0] == guard_pat[0] && g[1] == guard_pat[1] && g[2] == guard_pat[2] && g[3] == guard_pat[3];
}

int stack_bad(const struct proc *p, u32 ksp, u32 usp)
{
    if (!guard_ok(p->kstk) || ksp < p->kstk + GUARD_BYTES || ksp > p->ktop)
        return STK_KERNEL;
    if (p->ustk && (!guard_ok(p->ustk) || (usp && (usp < p->ustk + GUARD_BYTES || usp > p->base + p->span))))
        return STK_USER;
    return 0;
}

void stack_fail(struct proc *p, int which)
{
    const char *k = which == STK_KERNEL ? "kernel" : "user";
    if (p->pid == 0) {
        kprintf("\n[kernel] pid 0 (idle) overflowed its kernel stack\n");
        panic("pid 0 (idle) overflowed its kernel stack");
    }
    kprintf("\n[kernel] pid %d (%s) overflowed its %s stack: killed\n", p->pid, p->name, k);
    proc_kill(p);                       /* a blocked one is woken: it dies the moment it runs again (schedule) */
    p->killed = 2;                      /* 2: exit code 139, as for a fault */
}

KINIT void proc_init(void)         /* the boot: once, size first */
{
    memset(procs, 0, sizeof procs);
    procs[0].state = P_READY;           /* pid 0: idle, runs on the boot stack */
    procs[0].pid = 0;
    memcpy(procs[0].name, "idle", 5);
    procs[0].prio = procs[0].blevel = PRI_IDLE;     /* below every level: anything ready is better */
    procs[0].kstk = (u32)kstack_bottom;
    procs[0].ktop = (u32)kstack_bottom + 8192;
    guard_set(procs[0].kstk);           /* kmain is far above it by now */
    current = &procs[0];
}

/* User pointers (D13, docs/d13-shared-code.md section 5). What a process may hand the kernel to write into: its own
 * block and its memalloc regions. To read from: those and its code (a ZEX2 program's string literals live in the
 * shared part; nothing may ever write there: every process running the program runs that code). */
int user_ok_wr(const struct proc *p, u32 ptr, u32 len)
{
    if (ptr + len < ptr)
        return 0;
    if (ptr >= p->base && ptr + len <= p->base + p->span)
        return 1;
    for (int i = 0; i < NREG; i++)
        if (p->reg[i].base && ptr >= p->reg[i].base &&
            ptr + len <= p->reg[i].base + p->reg[i].size)
            return 1;
    return 0;
}

int user_ok_rd(const struct proc *p, u32 ptr, u32 len)
{
    if (user_ok_wr(p, ptr, len))
        return 1;
    return p->csize && ptr + len >= ptr && ptr >= p->code && ptr + len <= p->code + p->csize;
}

/* D60 5.7: own memory, or (protection on) a window of a request p holds; mmu.c gives a copy page back to its client
 * for the rest of the system call when p is current */
int user_ok_call_wr(struct proc *p, u32 ptr, u32 len)
{
    return user_ok_wr(p, ptr, len) || (__builtin_expect(mmu_on, 0) && mmu_ops->callok(p, ptr, len, 1));    /* (its
                                                 * inbox, or a window) */
}

int user_ok_call_rd(struct proc *p, u32 ptr, u32 len)
{
    return user_ok_rd(p, ptr, len) || (__builtin_expect(mmu_on, 0) && mmu_ops->callok(p, ptr, len, 0));
}

u32 user_rd_extent(const struct proc *p, u32 ptr)
{
    if (ptr >= p->base && ptr < p->base + p->span)
        return p->base + p->span - ptr;
    for (int i = 0; i < NREG; i++)
        if (p->reg[i].base && ptr >= p->reg[i].base && ptr < p->reg[i].base + p->reg[i].size)
            return p->reg[i].base + p->reg[i].size - ptr;
    if (p->csize && ptr >= p->code && ptr < p->code + p->csize)
        return p->code + p->csize - ptr;
    return 0;
}

struct proc *proc_by_pid(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid)
            return &procs[i];
    return 0;
}

/* a process's name (ps, pgrep): the last part of PATH, at most 15 bytes */
static void set_name(struct proc *p, const char *path)
{
    const char *b = path;
    for (const char *q = path; *q; q++)
        if (*q == '/')
            b = q + 1;
    for (int i = 0; i < 15; i++)
        p->name[i] = b[i] ? b[i] : 0;
    p->name[15] = 0;
}

/* Create a process from a ZEXE image in memory. `argv` points to kernel
 * memory. stdio[i] (parent's fd numbers, -1 = same number) picks the
 * child's fds 0-2; the child gets no other fds. */
/* put the image into the process's memory at base and relocate it: from a program built into the kernel ... */
typedef int (*fillfn)(const void *ctx, u8 *base, const struct zexe *h);
/* Every relocation offset must name a whole 32-bit word inside the image, at an even address (where the linker puts
 * addresses): a corrupt table must not write outside the program. All are checked before the first is applied. */
static int relocs_ok(const u32 *rel, const struct zexe *h)
{
    for (u32 i = 0; i < h->nreloc; i++)
        if (rel[i] > h->image_size - 4 || (rel[i] & 1))
            return 0;
    return 1;
}
static void relocate(u8 *base, const u32 *rel, u32 n)
{
    for (u32 i = 0; i < n; i++)
        *(u32 *)(base + rel[i]) += (u32)base;
}
/* From here to proc_spawn_image, the program loader is -Os (audit block 2, the kernel's shrink): its time goes to
 * reading the file; the two loops over the relocations above stay -O2 */
#pragma GCC push_options
#pragma GCC optimize("Os")
static int fill_mem(const void *ctx, u8 *base, const struct zexe *h)
{
    const u8 *file = ctx;
    const u32 *rel = (const u32 *)(file + h->reloc_off);
    if (!relocs_ok(rel, h))
        return -ENOEXEC;
    memcpy(base, file, h->image_size);
    relocate(base, rel, h->nreloc);
    return 0;
}
/* ... or straight from the file system: no buffer holding the whole file, so a spawn needs only the process's own
 * memory (plus the relocation table on the kernel heap: a few KB for most programs, never more than half the image) */
static int fill_fs(const void *ctx, u8 *base, const struct zexe *h)
{
    const struct exec_src *x = ctx;
    if (fs_exec_read(x, 0, base, h->image_size))
        return -EIO;
    u32 rsz = h->nreloc * 4;                    /* nreloc was checked against the file size: no wrap */
    u32 *rel = kmalloc(rsz ? rsz : 4);
    if (!rel)
        return -ENOMEM;
    if (rsz && fs_exec_read(x, h->reloc_off, rel, rsz)) {
        kfree(rel);
        return -EIO;
    }
    if (!relocs_ok(rel, h)) {
        kfree(rel);
        return -ENOEXEC;
    }
    relocate(base, rel, h->nreloc);
    kfree(rel);
    return 0;
}

/* The ZEXE header comes from a file: every field is checked, with arithmetic that cannot wrap, before anything is
 * allocated or read (audit B7). Returns 0, or what is wrong for the message. */
static const char *zexe_bad(const struct zexe *h, u32 fsize)
{
    if (fsize < sizeof *h || h->magic != ZEXE_MAGIC)
        return "not a Zenix program (no ZEXE header)";
    if (h->image_size < sizeof *h || h->image_size > fsize)
        return "image size outside the file";
    if (h->reloc_off > fsize || h->nreloc > (fsize - h->reloc_off) / 4)
        return "relocation table outside the file";
    if (h->nreloc && h->image_size < 4)
        return "relocations in an empty image";
    if (h->nreloc > h->image_size / 2)  /* each names its own even word: more is no linker's (the table's kernel-heap
                                         * block is never bigger than the image) */
        return "too many relocations";
    if (h->mem_size < h->image_size || h->mem_size > ZEXE_MEM_MAX)
        return "memory size impossible";
    if (h->stack_size && (h->stack_size < ZEXE_STACK_MIN || h->stack_size > ZEXE_STACK_MAX))
        return "stack size impossible";
    return 0;
}

/* ---- ZEX2: a program with shared code (D13, docs/d13-shared-code.md section 4) ----
 * The header comes from a file: every field is checked first, as for ZEXE */
static const char *zex2_bad(const struct zex2 *h, u32 fsize)
{
    if (fsize < sizeof *h)
        return "not a Zenix program (no ZEX2 header)";
    if (h->text_size < sizeof *h || (h->text_size & 15) || h->text_size > fsize)
        return "shared part outside the file";
    if (h->data_size > fsize - h->text_size || (h->data_size & 3))
        return "data image outside the file";
    if (h->reloc_off < h->text_size + h->data_size || h->reloc_off > fsize)
        return "relocation table overlaps the image or lies outside the file";
    u32 room = (fsize - h->reloc_off) / 4;
    if (h->nrel_code > room || h->nrel_data > room - h->nrel_code)
        return "relocation table outside the file";
    if (h->nrel_code + h->nrel_data > h->data_size / 2)    /* (as zexe_bad's; the sum cannot wrap: both fit room) */
        return "too many relocations";
    if (h->mem_size < h->data_size || h->mem_size > ZEXE_MEM_MAX)
        return "memory size impossible";
    if (h->stack_size && (h->stack_size < ZEXE_STACK_MIN || h->stack_size > ZEXE_STACK_MAX))
        return "stack size impossible";
    return 0;
}

/* Every relocation (link addresses: the first nrel_code point into the shared part, the rest into the data part) must
 * name a whole word of the data image at an even address (stage 1: none lies in the shared part), and the word's value
 * must lie in the part its list says. All are checked before the first is applied. */
static int rel2_ok(const u32 *rel, const struct zex2 *h, const u8 *data)
{
    u32 ts = h->text_size, n = h->nrel_code + h->nrel_data;
    for (u32 i = 0; i < n; i++) {
        u32 at = rel[i] - ts;
        if (rel[i] < ts || h->data_size < 4 || at > h->data_size - 4 || (at & 1))
            return 0;
        u32 v = *(const u32 *)(data + at);
        if (i < h->nrel_code ? v >= ts : v < ts || v - ts > h->mem_size)
            return 0;
    }
    return 1;
}

/* Load a ZEX2 program's data part into DATA (the process's block) and its code: the segment *segp already found (a
 * reference taken), else a copy of its own - listed as a segment for the next spawns when the file cannot have changed
 * (seg_enter), else kept private (owned by pid, freed with the process). *codep: where the code is. Nobody waits on a
 * half-loaded segment: each spawn that finds none loads its own copy; it is listed only when complete. */
static int fill_v2(struct exec_src *x, u8 *data, const struct zex2 *h, u32 pid, const char *path, struct seg **segp,
                   u32 *codep, int nolist)
{
    u32 ts = h->text_size, nrel = h->nrel_code + h->nrel_data, rsz = nrel * 4;     /* checked: no wrap */
    u8 *own = 0;
    if (!*segp) {
        if (!(own = mem_alloc(ts, MEM_USER, pid)))
            return -ENOMEM;
        if (fs_exec_read(x, 0, own, ts))
            return -EIO;
        for (u32 i = 0; i < sizeof *h; i++)     /* the file changed between the two reads: never run a mix */
            if (own[i] != ((const u8 *)h)[i])
                return -EIO;
    }
    if (h->data_size && fs_exec_read(x, ts, data, h->data_size))
        return -EIO;
    u32 *rel = kmalloc(rsz ? rsz : 4);
    if (!rel)
        return -ENOMEM;
    if (rsz && fs_exec_read(x, h->reloc_off, rel, rsz)) {
        kfree(rel);
        return -EIO;
    }
    if (!rel2_ok(rel, h, data)) {
        kfree(rel);
        return -ENOEXEC;
    }
    if (own && !nolist) {               /* (nolist: started under a debugger, a private copy: D54 section 10) */
        int made;
        struct seg *s = seg_enter(x, (u32)own, ts, seg_sum((u32)own, ts), path, &made);
        if (s && !made)
            mem_free(own);              /* another spawn listed the same file first: use that one */
        *segp = s;
    }
    u32 code = *segp ? (*segp)->code : (u32)own, dd = (u32)data - ts;
    for (u32 i = 0; i < nrel; i++)
        *(u32 *)(data + rel[i] - ts) += i < h->nrel_code ? code : dd;
    kfree(rel);
    *codep = code;
    return 0;
}

static int proc_load(const union zhdr *hu, u32 fsize, fillfn fill, const void *fillctx, const char *path, const char *name,
                     char *const *argv, int argc, struct proc *parent, const int *stdio, int tty, int priv, int suid,
                     int rdok)
{
    const struct zexe *h = &hu->v1;
    int v2 = h->magic == ZEX2_MAGIC && fill == fill_fs;    /* the programs built into the kernel stay ZEXE */
    const char *bad = v2 ? zex2_bad(&hu->v2, fsize) : zexe_bad(h, fsize);
    if (bad) {
        kprintf("exec: %s refused: %s\n", path, bad);
        return -ENOEXEC;
    }
    u32 img = v2 ? hu->v2.data_size : h->image_size, memsz = v2 ? hu->v2.mem_size : h->mem_size;
    struct exec_src *x = v2 ? (struct exec_src *)fillctx : 0;
    struct dbg *dd = 0;
    if (parent && (parent->dbgf & DBGF_NEXT)) {     /* D54 section 11: started under its debugger - the parent's bit,
                                                     * never current's (a service's server); any attempt takes it */
        parent->dbgf &= (u8)~DBGF_NEXT;
        if (!rdok)
            return -EACCES;             /* a program the caller may not read is not dumped through a debugger */
        if (!(dd = dbg_pending(parent)))
            return -EAGAIN;
        suid = -1;                      /* set-user-id not honoured */
    }
    /* a segment of this file already in memory: the reference is taken now, before anything below can block */
    struct seg *seg = v2 && !dd ? seg_find(x, hu->v2.text_size, path) : 0;
    u32 code = 0;

    u32 strsz = 0;
    for (int i = 0; i < argc; i++) {
        u32 n = 0;
        while (argv[i][n])
            n++;
        strsz += n + 1;
    }
    u32 argsz = (strsz + (u32)(argc + 1) * 4 + 8 + 15) & ~15u;
    int pid = next_pid++;
    u32 stk = v2 ? hu->v2.stack_size : h->stack_size;
    u32 stack = stk ? stk : USTACK_DEFAULT;
    u32 span = ((memsz + 15) & ~15u) + ((stack + 15) & ~15u) + argsz;
    u8 *base = mem_alloc(span, MEM_USER, (u32)pid);    /* D41: whole pages while protection is on (D60: services too) */
    u32 *ks = mem_alloc(KSTACK, 0, (u32)pid);
    u32 envlen = parent ? parent->envlen : 0;
    char *env = envlen ? mem_alloc(envlen, 0, (u32)pid) : 0;      /* the parent cannot change it while it spawns */
    if (!base || !ks || (envlen && !env)) {
        mem_free_owner((u32)pid);
        seg_put(seg);
        return -ENOMEM;
    }
    /* Reading the image can block (messages to the filesystem server), and another process may spawn meanwhile: so the
     * process slot is chosen only afterwards, and nothing below blocks. */
    int fr = v2 ? fill_v2(x, base, &hu->v2, (u32)pid, path, &seg, &code, dd != 0) : fill(fillctx, base, h);
    if (fr) {
        if (fr == -ENOEXEC)
            kprintf("exec: %s refused: a relocation points outside the program\n", path);
        mem_free_owner((u32)pid);
        seg_put(seg);
        return fr;
    }
    memset(base + img, 0, span - img);
    u32 ustk = (u32)base + ((memsz + 15) & ~15u);               /* the user stack starts above image and bss */
    guard_set(ustk);
    guard_set((u32)ks);
    if (dd)
        dbg_entry(dd, v2 ? code : (u32)base);     /* D54: an ILLEGAL at its entry, before the flush */
    cache_flush_code();         /* the image is code the CPU has not seen: no stale I-cache lines or Emu68 translations
                                 * (another program ran at this address before) may run instead */
    struct proc *p = 0;
    proc_reap_orphans();                /* zombies nobody will wait for give their slots back first */
    for (int i = 1; i < NPROC; i++)
        if (procs[i].state == P_FREE) {
            p = &procs[i];
            break;
        }
    if (!p) {
        mem_free_owner((u32)pid);
        seg_put(seg);
        return -EAGAIN;
    }

    /* arguments at the top of the stack: usp -> argc, argv, argv[]..., strings */
    u8 *top = base + span;
    u8 *sp = top - strsz;
    u32 *vec = (u32 *)(top - argsz + 8);
    u8 *str = sp;
    for (int i = 0; i < argc; i++) {
        vec[i] = (u32)str;
        const char *a = argv[i];
        while ((*str++ = *a++))
            ;
    }
    vec[argc] = 0;
    u32 *usp = (u32 *)(top - argsz);
    usp[0] = (u32)argc;
    usp[1] = (u32)vec;

    /* kernel stack for the first switch_to: d2-d7,a2-a6 and the return address. A ZEX2 program's code needs a5 = the
     * start of its data part (the table): the 10th of the 11 words movem pops (index 9); user_first_entry keeps it. */
    u32 *kt = ks + KSTACK / 4;
    *--kt = (u32)user_first_entry;
    for (int i = 0; i < 11; i++)
        *--kt = 0;
    if (v2)
        kt[9] = (u32)base;

    memset(p, 0, sizeof *p);
    mmu_slot_new(p);                                /* D41: nothing of the slot's last tree */
    p->ksp = kt;
    p->usp = (u32)usp;
    p->entry = v2 ? code : (u32)base;
    p->seg = seg;
    p->code = code;
    p->csize = v2 ? hu->v2.text_size : 0;
    p->a5 = v2 ? (u32)base : 0;
    p->pid = pid;
    p->ppid = parent ? parent->pid : 0;
    p->base = (u32)base;
    p->span = span;
    p->kstk = (u32)ks;
    p->ktop = (u32)ks + KSTACK;
    p->ustk = ustk;
    p->cwd_ino = parent ? parent->cwd_ino : 2;
    p->uid = parent ? parent->uid : 0;
    p->ruid = parent ? parent->ruid : 0;
    p->gid = parent ? parent->gid : 0;
    if (parent) {                                   /* the supplementary groups too (kept by set-user-id) */
        p->ngroups = parent->ngroups; p->grp_id = parent->grp_id;
        for (int i = 0; i < NGROUPS; i++) p->groups[i] = parent->groups[i];
    }
    p->umask = parent ? parent->umask : 022;
    p->nice = parent ? parent->nice : 0;            /* a child inherits the base priority (not a server's boost) */
    p->pgid = parent ? parent->pgid : pid;          /* D16 rule 7: no parent (boot, the servers): its own group */
    if (parent) {                                   /* as fork and exec: ignored and blocked signals are kept, handled
                                                     * ones (the new program has no handlers) go back to the default */
        p->sig_ign = parent->sig_ign;
        p->sig_block = parent->sig_block;
    }
    p->blevel = p->prio = NICE_LEVEL(p->nice);
    if (suid >= 0) p->uid = (u16)suid;                 /* set-user-id program: runs as its owner */
    if (env) {
        memcpy(env, parent->env, envlen);
        p->env = env;
        p->envlen = (u16)envlen;
    }
    if (parent)
        memcpy(p->cwd, parent->cwd, sizeof p->cwd);
    else
        p->cwd[0] = '/';
    set_name(p, name);                 /* (a script's interpreter: the script's, D53 3.8) */

    /* file descriptors and the controlling tty */
    if (parent) {
        /* only fds 0-2 are passed on (so a pipe's write end cannot leak
         * into a reader and keep it from seeing end-of-file) */
        for (int i = 0; i < 3; i++) {
            int src = (stdio && stdio[i] >= 0) ? stdio[i] : i;
            if (src < NFD && parent->fd[src])
                p->fd[i] = file_dup(parent->fd[src]);
        }
        /* a console as standard input makes it the controlling tty (init starting login on /dev/ttyN); otherwise the
         * parent's, so the second stage of `ls | less` still reads the keys of the console it runs on */
        p->ctty = (p->fd[0] && p->fd[0]->type == FT_TTY) ? (u8)p->fd[0]->off : parent->ctty;
    } else {
        struct file *t = file_new(tty ? FT_TTY : FT_KLOG);  /* no parent: boot on console 1, a service's server
                                                             * writes into the kernel log (D11 step 8) */
        if (!t) {                       /* every open-file slot taken: the slot p is still free (state 0) */
            mem_free_owner((u32)pid);
            seg_put(seg);
            p->seg = 0;
            return -ENFILE;
        }
        t->off = (u32)tty;
        for (int i = 0; i < 3; i++)
            p->fd[i] = i == 0 ? t : file_dup(t);
        p->ctty = (u8)tty;
    }
    if (p->ctty < 1 || p->ctty >= NTTYS)
        p->ctty = 1;
    if (parent)
        tty_spawned(p, parent->ctty);   /* the session leader rule (D20 7.2) */
    p->priv = priv;
    if (suid >= 0 || !rdok)
        p->dbgf |= DBGF_NODEBUG;        /* D54 section 4: only root may debug it */
    if (dd)
        dbg_link(dd, p);
    p->state = P_READY;
    {
        u16 s = irq_save();
        rq_push(p);
        irq_restore(s);
    }
    if (current->svc_depth) {
        svc_born(pid);                  /* a service's server (D11): its entry is starting from now on (confined: D60) */
        mmu_sysmark(p);                 /* (MMU=D41, D60 12: D41 7's system process) */
    }
    return pid;
}

#define SPAWN_HUNK (-4096)                  /* not an errno: spawn1 found an AmigaOS program */
#define SPAWN_SCRIPT (-4097)                /* not an errno: spawn1 found a #! script, its first line in *LINE */
/* load the program PATH, the process named after NAME. LINE 0 (an interpreter, ZINE): a script or an AmigaOS program is -ENOEXEC. NOSUID: a
 * script's interpreter, whose set-user-id bit is not honoured (D53 3.7). Not inlined:
 * its frame (exec_src, the header) is on the stack once, whichever call proc_spawn makes (D53 4). */
static __attribute__((noinline)) int spawn1(const char *path, const char *name, char *const *argv, int argc,
                                            struct proc *parent, const int *stdio, int tty, char **line, int nosuid)
{
    struct exec_src x;
    union zhdr h;                       /* a ZEXE header, or the longer ZEX2 one */
    long r = fs_exec_open(path, parent, &x);
    if (r < 0)
        return (int)r;
    memset(&h, 0, sizeof h);
    u32 hn = x.st.size < sizeof h ? x.st.size : sizeof h;
    if (x.st.size >= 2 && fs_exec_read(&x, 0, &h, hn)) {      /* shorter: proc_load says why it is refused */
        fs_exec_close(&x);
        return -EIO;
    }
    r = 0;
    if (*(const u32 *)&h == 0x3F3)      /* HUNK_HEADER: an AmigaOS program (proc_spawn below runs zine) */
        r = SPAWN_HUNK;
    else if (*(const u16 *)&h == 0x2321) {     /* "#!": a script (D53): its first SHEBANG_MAX bytes for proc_spawn */
        r = SPAWN_SCRIPT;
        if (line) {
            u32 n = x.st.size < SHEBANG_MAX ? x.st.size : SHEBANG_MAX;
            char *b = *line = kmalloc(SHEBANG_MAX);
            if (!b)
                r = -ENOMEM;
            else {
                memset(b, 0, SHEBANG_MAX);              /* a shorter file's line ends at its end */
                if (fs_exec_read(&x, 0, b, n))
                    r = -EIO;
            }
        }
    } else if (x.st.size > ZEXE_FILE_MAX) {    /* the largest Zenix program file, as the most memory one may ask for
                                                 * (an AmigaOS one may be bigger: zine reads it) */
        kprintf("exec: %s refused: %s\n", path, "larger than 16 MB");
        r = -EFBIG;
    }
    if (r && !line && r < -4095)        /* an interpreter, or zine itself, that is not a Zenix program (D53 3.5) */
        r = -ENOEXEC;
    if (r) {
        fs_exec_close(&x);
        return (int)r;
    }
    int pid = proc_load(&h, x.st.size, fill_fs, &x, path, name, argv, argc, parent, stdio, tty, 0,
                        (x.st.mode & 04000) && !nosuid ? (int)x.st.uid : -1,   /* set-user-id: runs as its owner (su,
                                                                                * passwd) */
                        dbg_readable(x.st.mode, x.st.uid, x.st.gid, parent));
    fs_exec_close(&x);                  /* the image is in memory: the server may forget this open */
    return pid;
}

/* D29 12.1, running by name: an executable AmigaOS program (an x bit, the file starts with HUNK_HEADER 0x3F3) runs as
 * `/bin/zine PATH ARGS...` (frozen behaviour); without /bin/zine it is -ENOEXEC, said in the kernel log.
 * D53: a script (an x bit, the file starts with "#!") runs as `INTERPRETER [ARG] PATH ARGS...`, the interpreter and
 * its one argument from the script's first line (kernel/shebang.h); the process is named after the script (before it
 * is made ready). */
int proc_spawn(const char *path, char *const *argv, int argc, struct proc *parent, const int *stdio, int tty)
{
    char *line = 0, *interp = "/bin/zine", *arg = 0;
    int pid = spawn1(path, path, argv, argc, parent, stdio, tty, &line, 0);
    if (pid != SPAWN_HUNK && pid != SPAWN_SCRIPT) {
        kfree(line);                    /* a script's line read, then -EIO / -ENOMEM: given back */
        if (parent)
            parent->dbgf &= (u8)~DBGF_NEXT;     /* D54 11: any attempt takes it (a failed open too) */
        return pid;
    }
    char **v = 0;
    if (line) {
        const char *bad = shebang_parse(line, &interp, &arg);
        if (bad) {
            kprintf("exec: %s refused: %s\n", path, bad);
            pid = -ENOEXEC;
            goto out;
        }
    }
    pid = -ENOMEM;
    if (!(v = kmalloc((u32)(argc + 3) * 4)))
        goto out;
    int n = 0;
    u32 bytes = 0;
    v[n++] = line ? interp : "zine";
    if (arg)
        v[n++] = arg;
    v[n++] = (char *)path;
    for (int i = 1; i < argc; i++)
        v[n++] = argv[i];
    v[n] = 0;
    for (int i = 0; i < n; i++)
        for (const char *q = v[i]; ; q++, bytes++)
            if (!*q) { bytes++; break; }
    if (line && (n > SPAWN_ARGC || bytes > SPAWN_ARGBYTES))   /* a script's rewritten list keeps spawn's limits
                                                                 * (D53 3.10; ZINE's as it was) */
        pid = -E2BIG;
    else
        pid = line && current->killed ? -EINTR : spawn1(interp, line ? path : interp, v, n, parent, stdio, tty, 0,
                                                        line != 0);
    if (!line && (pid == -ENOENT || pid == -ENOEXEC)) {     /* (-ENOEXEC: /bin/zine is not a Zenix program) */
        kprintf("exec: %s is an AmigaOS program: ZINE is not installed\n", path);
        pid = -ENOEXEC;
    }
out:
    kfree(v);
    kfree(line);
    if (parent)
        parent->dbgf &= (u8)~DBGF_NEXT;         /* (the interpreter's spawn took it, or nothing did) */
    return pid;
}

/* a program built into the kernel image (no filesystem needed): no parent (uid 0, /, nice 0), output to console tty
 * (0: the kernel log, a service's server) */
int proc_spawn_image(const void *img, u32 len, char *const *argv, int argc, int priv, int tty)
{
    return proc_load((const union zhdr *)img, len, fill_mem, img, argv[0], argv[0], argv, argc, 0, 0, tty, priv, -1, 1);
}

#pragma GCC pop_options

/* may killed process p end now? Never inside a message call (the server may still use the client's buffers, and the
 * kernel code around the call - a program load, the list of loads in progress through the kernel stacks - must reach
 * its end): it ends at the end of its system call. A service process killed while serving a request ends only between
 * requests (D11 5.6) - unless it was killed twice or 5 s ago (a hung server); a fault ends it at once. */
static int kill_now(const struct proc *p)
{
    if (p->exiting || p->wait_msg)
        return 0;
    return p->killed != 1 || (s32)(ticks - p->kill_at) >= 500 || !ipc_serving(p->pid);
}
int proc_kill_now(void) { return current->killed && kill_now(current); }

void schedule(void)
{
    u16 s = irq_save();
    struct proc *old = current, *n;

    /* the stack guard: the one leaving the CPU (its stacks now), unless it is already on its way out */
    if (!old->killed && old->state != P_ZOMBIE) {
        int bad = stack_bad(old, (u32)__builtin_frame_address(0) - KSP_SLACK, get_usp());
        if (bad)
            stack_fail(old, bad);       /* pid 0: a panic; else killed when it next runs */
    }

    if (old->state == P_READY && old->pid != 0 && !old->queued)
        rq_push(old);                   /* yielded or preempted: back of its level's line */
    wake_sleepers();
    need_resched = 0;
    int l = rq_top();
    if (l < NLEVEL) {
        n = rq_head[l];
        if (!(rq_head[l] = n->rnext)) {
            rq_tail[l] = 0;
            rq_mask &= ~(0x80000000u >> l);
        }
        n->rnext = 0;
        n->queued = 0;
    } else {
        n = &procs[0];                  /* nothing to run: idle */
    }
    slice = SLICE;
    if (n != old) {
        /* ... and the saved stack pointers of the one coming in (pid 0 has no user stack: ustk 0). A kernel stack
         * pointer outside its stack means the process table itself is damaged: it cannot be resumed, nor killed in
         * its own context. */
        if (n->ksp && ((u32)n->ksp < n->kstk + GUARD_BYTES || (u32)n->ksp > n->ktop)) {
            kprintf("\n[kernel] pid %d (%s): saved kernel stack pointer %p outside its stack\n", n->pid, n->name, n->ksp);
            panic("a saved kernel stack pointer is outside its stack");
        }
        if (n->ustk && !n->killed && (n->usp < n->ustk + GUARD_BYTES || n->usp > n->base + n->span))
            stack_fail(n, STK_USER);
        current = n;
        mmu_switch(n);                  /* D41: its view in, or every access let through for a system process */
        switch_to(old, n);
    }
    irq_restore(s);
    /* A process that was killed while blocked or preempted dies here - but not while it is already exiting (proc_exit's
     * own FS_CLOSE calls end up here: a nested exit closed its files twice, the u16 refs wrapped, audit G3), and a kill
     * (not a fault) waits while it is in a message call: the reply readies it, the call returns, and it dies at the end
     * of its system call, so a close or a program load is not cut in half (G4); a killed server ends between requests
     * (G5, D11): kill_now */
    if (current->killed && current->state == P_READY && current->pid != 0 && kill_now(current))
        proc_exit(kill_status(current));
    if (current->stop_req && current->state == P_READY && current->pid != 0 && !current->wait_msg &&
        !current->svc_depth && !(current->dbgf & DBGF_INEXC)) {     /* D54 5: never while loading a service; one held
                                                                     * in exc_c stops there (its own block) */
        struct proc *p = current;                       /* stopped: not runnable until proc_cont */
        p->stop_req = 0;
        p->stop_rep = 0;
        p->state = P_STOP;
        struct proc *par = (p->dbgf & DBGF_TRACED) && dbg_stopped(p) ? 0 : proc_by_pid(p->ppid);
        if (par) {                                      /* the parent's wait sees it (not a debugger's stop, D54);
                                                         * SIGCHLD (D16 4) */
            ev_post(par, EV_CHILD);
            sig_send(par, SIGCHLD);
        }
        schedule();
    }
}

/* from the level-2 interrupt. Every tick counts the running process's slice, whether it cut into user code or into a
 * system call (a program that spends most of its time in system calls has its slice end too); the end of the slice,
 * like a better process becoming ready, only sets need_resched. The switch itself happens when user code was cut (here)
 * or at the end of the system call (syscall_c): the kernel is never preempted. */
void sched_tick(u16 sr, int tick)
{
    if (current->pid == 0)
        return;
    if (tick && slice > 0 && --slice == 0)
        need_resched = 1;
    if (!(sr & 0x2000) && need_resched)
        resched();
}

/* act on need_resched (at a return to user mode): a strictly better level takes the CPU at once; at the end of the
 * slice the next one of the same level gets its turn (equal levels alternate only there); with nobody of the same
 * level waiting, the running one starts a new slice */
void resched(void)
{
    u16 s = irq_save();
    need_resched = 0;
    wake_sleepers();
    int l = rq_top();
    irq_restore(s);
    if (l < current->prio || (slice <= 0 && l <= current->prio))
        schedule();
    else if (slice <= 0)
        slice = SLICE;
}

void proc_stop(struct proc *p)
{
    if (p->pid == 0 || p->priv || p->state == P_ZOMBIE || p->state == P_FREE || p->state == P_STOP) {
        if (p->state == P_STOP && (p->dbgf & DBGF_TRACED))
            dbg_jobstop(p);             /* D54 5: remembered while its debugger holds it */
        return;
    }
    p->stop_req = 1;
    if (p->state == P_EV || p->state == P_SLEEP ||
        (p->state == P_MSG && !p->wait_msg))            /* wake it so that it notices */
        ready(p);
}

void proc_cont(struct proc *p)
{
    p->stop_req = 0;
    if (p->state == P_STOP)
        ready_force(p);
}

int kill_status(const struct proc *p)
{
    return p->killed == 2 ? 139 : p->kill_sig ? 128 + p->kill_sig : 130;
}

/* From proc_kill to proc_wait: a kill, a process's end and the wait for it, once per program: -Os (audit block 2) */
#pragma GCC push_options
#pragma GCC optimize("Os")
void proc_kill(struct proc *p)
{
    if (p->killed == 1)
        p->killed = 3;                  /* a second kill: at once, even in the middle of a request (D11 5.6) */
    else if (!p->killed) {
        p->killed = 1;
        p->kill_at = ticks;
    }
    if (p->state == P_STOP || p->state == P_EV || p->state == P_SLEEP ||
        (p->state == P_MSG && !p->wait_msg))        /* a server in recv, not a client in call */
        ready_force(p);
    else if (p->state == P_MSG && p->msg_cancel)
        ipc_cancel(p);                  /* a socket call not yet taken by the server (audit O9) */
    if (p->wait_msg)
        ipc_cancel_mark(p);             /* D23 5.1: its server may answer -EINTR (a call taken back above is done) */
}

void proc_exit(int code)
{
    ints_off();
    struct proc *p = current;
    p->exiting = 1;
    if (ndbg)
        dbg_exit(p, code);              /* D54 13-14: as a debugger and as a debugged process */
    mmu_forget(p);                      /* D41: its view never applied again (before seg_put drops its code) */
    drv_exit(p);                        /* D19 2.4: a device it claimed is stopped before anything of it is freed */
    gcon_proc_exit(p->pid);             /* D38: a console it grabbed is given back (its chip RAM goes with it) */
    seg_put(p->seg);                    /* D13: after exc_c's crash check; the last user frees the code */
    if (p->seg)
        p->csize = 0;
    p->seg = 0;
    for (int i = 0; i < NFD; i++)
        if (p->fd[i]) {
            file_close(p->fd[i]);
            p->fd[i] = 0;
        }
    if (tty_fgpid(tty_of(p)) == p->pid)
        tty_setfg(tty_of(p), 0, 0);     /* a single-process foreground only: a group keeps it (D16 rule 7) */
    if (p->alarm_on) {
        p->alarm_on = 0;
        ntimer--;
    }
    kfree(p->sig_mask);                 /* D16: the handlers' masks (no handler runs once exiting) */
    p->sig_mask = 0;
    for (int i = 1; i < NPROC; i++) {   /* rule 12: its stopped children go on, and SIGHUP ends them by default */
        struct proc *c = &procs[i];
        if (c->ppid == p->pid && (c->state == P_STOP || c->stop_req)) {
            sig_send(c, SIGCONT);
            sig_send(c, SIGHUP);
        }
    }
    p->exit_code = code;
    p->state = P_ZOMBIE;
    struct proc *par = proc_by_pid(p->ppid);
    if (par) {
        ev_post(par, EV_CHILD);
        sig_send(par, SIGCHLD);
    }
    tty_release(p->pid);
    tty_proc_gone(p);                   /* D20 7.2: its leadership ends; a pty nobody names any more is freed */
    term_release(p->pid);               /* D20: the console fonts and rules it set */
    ipc_proc_exit(p->pid);
    proc_reap_orphans();                /* its zombie children have nobody to wait for them now */
    schedule();
    for (;;)
        ;
}

/* SYS_exit's second argument (the 2 MB diet, DESIGN D18: login steps aside): the caller's child `pid` becomes the
 * caller's parent's child, as if the parent had started it (Linux's CLONE_PARENT, after the fact); one that has
 * already ended or stopped is told to the new parent at once. Not a child of the caller, or no parent: nothing. */
void proc_handover(int pid)
{
    struct proc *c = proc_by_pid(pid), *par = proc_by_pid(current->ppid);
    if (!c || c == current || c->ppid != current->pid || !par || par->state == P_ZOMBIE)
        return;
    c->ppid = par->pid;
    if (c->ctty == current->ctty)
        tty_lead_move(current->pid, c->pid);    /* D20 7.2: the session's leader is the shell login hands to */
    if (c->state == P_ZOMBIE || c->state == P_STOP) {
        ev_post(par, EV_CHILD);
        sig_send(par, SIGCHLD);
    }
}

/* wait for a child; returns its exit code and releases it, -1 if none. With WAIT_CONSOLE (root, init) it also returns
 * ST_CONSOLE + n when console n was shown with nobody on it (con_switch), so that init starts a login there. */
int proc_wait(int pid, int flags)
{
    int nohang = flags & WAIT_NOHANG;
    int con = (flags & WAIT_CONSOLE) && current->uid == 0;
    int ign = (current->sig_ign & SIGBIT(SIGCHLD)) != 0;   /* SIGCHLD ignored: children are reaped, never reported */
    for (;;) {
        int have = 0;
        current->ev_pending &= ~(EV_CHILD | EV_CON);    /* D16 step 1: children and consoles post to us directly */
        if (con && kbd_reset_state == KRS_HELD)
            kbd_reset_flush();          /* Ctrl+Amiga+Amiga: init syncs the disks before the reset (kbd.c) */
        if (con && con_want) {
            for (int n = 1; n <= NCONS; n++)
                if (con_want & (1u << n)) {
                    con_want &= (u16)~(1u << n);
                    return ST_CONSOLE + n;
                }
        }
        for (int i = 1; i < NPROC; i++) {
            struct proc *c = &procs[i];
            if (c->state == P_FREE || c->ppid != current->pid ||
                (pid != -1 && c->pid != pid))
                continue;
            have++;
            if (c->state == P_STOP && (flags & WAIT_UNTRACED) && !c->stop_rep && !(c->dbgf && dbg_held(c))) {
                c->stop_rep = 1;
                return ST_STOPPED;
            }
            if (c->state == P_ZOMBIE) {
                int code = c->exit_code;
                mem_free_owner((u32)c->pid);
                c->state = P_FREE;
                if (ign) {
                    have--;
                    continue;
                }
                return code;
            }
        }
        if (!have && !con)
            return ign ? -ECHILD : -1;  /* rule 10: -ECHILD when nothing is left (only a program that ignores SIGCHLD) */
        if (nohang)
            return have ? -2 : -1;
        current->wait_con = (u8)con;    /* con_switch posts EV_CON (event.c's order: no lost wake-up) */
        long e = ev_block(EV_CHILD | (con ? EV_CON : 0), 0, 0, 1);
        current->wait_con = 0;
        if (e)
            return (int)e;              /* -EINTR: a handler runs (and the call is restarted with SA_RESTART) */
    }
}

#pragma GCC pop_options

/* Free zombies nobody will wait for: the parent is gone or is itself a zombie (it can never wait). Called by the idle
 * loop, by every process exit (for its own children) and before a spawn looks for a free slot, so a busy machine that
 * never idles does not fill its process table with them. A process never frees itself (it runs on its kernel stack). */
void proc_reap_orphans(void)
{
    for (int pass = 0; pass < 2; pass++)            /* a second pass for the children of zombies freed in the first */
        for (int i = 1; i < NPROC; i++) {
            struct proc *c = &procs[i];
            if (c->state != P_ZOMBIE || c == current)
                continue;
            struct proc *par = c->ppid ? proc_by_pid(c->ppid) : 0;
            if (!par || par->state == P_ZOMBIE || (par->sig_ign & SIGBIT(SIGCHLD))) {    /* D16 rule 10 */
                mem_free_owner((u32)c->pid);
                c->state = P_FREE;
            }
        }
}
