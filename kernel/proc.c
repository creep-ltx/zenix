/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* Processes. No MMU: all processes share one address space, so the
 * kernel never trusts a user pointer (user_ok) and contains a crashing
 * process by killing it and giving back everything it owned. */

struct proc procs[NPROC];
struct proc *current;
static int next_pid = 1;
int nsleep, nio;                        /* how many are in P_SLEEP / P_IO */
volatile u32 wake_min;                  /* while nsleep: no sleeper is due before this tick */
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

/* make a blocked process runnable (never call this on the running one) */
void ready(struct proc *p)
{
    u16 s = irq_save();                 /* interrupts wake processes too */
    if (p->state != P_READY && p->state != P_FREE && p->state != P_ZOMBIE) {
        if (p->state == P_SLEEP)
            nsleep--;
        else if (p->state == P_IO)
            nio--;
        p->state = P_READY;
        if (p->pid != 0 && !p->queued) {
            rq_push(p);
            want_switch(p);
        }
    }
    irq_restore(s);
}

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
    if (nsleep++ == 0 || (s32)(t - wake_min) < 0)
        wake_min = t;
    irq_restore(s);
}

/* ready every sleeper that is due (each to its own level), and note when the next one is; interrupts masked */
static void wake_sleepers(void)
{
    if (!nsleep || (s32)(ticks - wake_min) < 0)
        return;
    u32 next = ticks + 0x7FFFFFFFu;
    for (int i = 1; i < NPROC; i++) {
        struct proc *q = &procs[i];
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
#define FILE_MAX (256u * 1024)

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

void proc_init(void)
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

int user_ok(const struct proc *p, u32 ptr, u32 len)
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

struct proc *proc_by_pid(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid)
            return &procs[i];
    return 0;
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
 * memory (plus the relocation table, a few KB) */
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
    if (h->mem_size < h->image_size || h->mem_size > ZEXE_MEM_MAX)
        return "memory size impossible";
    if (h->stack_size && (h->stack_size < ZEXE_STACK_MIN || h->stack_size > ZEXE_STACK_MAX))
        return "stack size impossible";
    return 0;
}

static int proc_load(const struct zexe *h, u32 fsize, fillfn fill, const void *fillctx, const char *path, char *const *argv,
                     int argc, struct proc *parent, const int *stdio, int tty, int priv, int suid)
{
    const char *bad = zexe_bad(h, fsize);
    if (bad) {
        kprintf("exec: %s refused: %s\n", path, bad);
        return -ENOEXEC;
    }

    u32 strsz = 0;
    for (int i = 0; i < argc; i++) {
        u32 n = 0;
        while (argv[i][n])
            n++;
        strsz += n + 1;
    }
    u32 argsz = (strsz + (u32)(argc + 1) * 4 + 8 + 15) & ~15u;
    int pid = next_pid++;
    u32 stack = h->stack_size ? h->stack_size : USTACK_DEFAULT;
    u32 span = ((h->mem_size + 15) & ~15u) + ((stack + 15) & ~15u) + argsz;
    u8 *base = mem_alloc(span, 0, (u32)pid);
    u32 *ks = mem_alloc(KSTACK, 0, (u32)pid);
    if (!base || !ks) {
        mem_free_owner((u32)pid);
        return -ENOMEM;
    }
    /* Reading the image can block (messages to the filesystem server), and another process may spawn meanwhile: so the
     * process slot is chosen only afterwards, and nothing below blocks. */
    int fr = fill(fillctx, base, h);
    if (fr) {
        if (fr == -ENOEXEC)
            kprintf("exec: %s refused: a relocation points outside the program\n", path);
        mem_free_owner((u32)pid);
        return fr;
    }
    memset(base + h->image_size, 0, span - h->image_size);
    u32 ustk = (u32)base + ((h->mem_size + 15) & ~15u);         /* the user stack starts above image and bss */
    guard_set(ustk);
    guard_set((u32)ks);
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

    /* kernel stack for the first switch_to: d2-d7,a2-a6 and the return address */
    u32 *kt = ks + KSTACK / 4;
    *--kt = (u32)user_first_entry;
    for (int i = 0; i < 11; i++)
        *--kt = 0;

    memset(p, 0, sizeof *p);
    p->ksp = kt;
    p->usp = (u32)usp;
    p->entry = (u32)base;
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
    p->umask = parent ? parent->umask : 022;
    p->nice = parent ? parent->nice : 0;            /* a child inherits the base priority (not a server's boost) */
    p->blevel = p->prio = NICE_LEVEL(p->nice);
    if (suid >= 0) p->uid = (u16)suid;                 /* set-user-id program: runs as its owner */
    if (parent) {
        memcpy(p->env, parent->env, parent->envlen);
        p->envlen = parent->envlen;
    }
    if (parent)
        memcpy(p->cwd, parent->cwd, sizeof p->cwd);
    else
        p->cwd[0] = '/';
    for (int i = 0; i < 15 && path[i]; i++)
        p->name[i] = path[i];
    /* name = last path component */
    {
        const char *b = path;
        for (const char *q = path; *q; q++)
            if (*q == '/')
                b = q + 1;
        for (int i = 0; i < 15; i++)
            p->name[i] = b[i] ? b[i] : 0;
        p->name[15] = 0;
    }

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
        struct file *t = file_new(FT_TTY);
        if (!t) {                       /* every open-file slot taken: the slot p is still free (state 0) */
            mem_free_owner((u32)pid);
            return -ENFILE;
        }
        t->off = (u32)tty;
        for (int i = 0; i < 3; i++)
            p->fd[i] = i == 0 ? t : file_dup(t);
        p->ctty = (u8)tty;
    }
    if (p->ctty < 1 || p->ctty > NCONS)
        p->ctty = 1;
    p->priv = priv;
    p->state = P_READY;
    {
        u16 s = irq_save();
        rq_push(p);
        irq_restore(s);
    }
    return pid;
}

int proc_spawn(const char *path, char *const *argv, int argc,
               struct proc *parent, const int *stdio, int tty)
{
    struct exec_src x;
    struct zexe h;
    long r = fs_exec_open(path, parent, &x);
    if (r < 0)
        return (int)r;
    memset(&h, 0, sizeof h);
    if (x.st.size >= sizeof h && fs_exec_read(&x, 0, &h, sizeof h)) {    /* shorter: proc_load says why it is refused */
        fs_exec_close(&x);
        return -EIO;
    }
    int pid = proc_load(&h, x.st.size, fill_fs, &x, path, argv, argc, parent, stdio, tty, 0,
                        (x.st.mode & 04000) ? (int)x.st.uid : -1);    /* set-user-id: runs as its owner (su, passwd) */
    fs_exec_close(&x);                  /* the image is in memory: the server may forget this open */
    return pid;
}

/* a program built into the kernel image (no filesystem needed) */
int proc_spawn_image(const void *img, u32 len, const char *name, int tty, int priv)
{
    char *argv[] = { (char *)name, 0 };
    return proc_load(img, len, fill_mem, img, name, argv, 1, 0, 0, tty, priv, -1);
}

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
        switch_to(old, n);
    }
    irq_restore(s);
    /* a process that was killed while blocked or preempted dies here */
    if (current->killed && current->state == P_READY && current->pid != 0)
        proc_exit(current->killed == 2 ? 139 : 130);
    if (current->stop_req && current->state == P_READY && current->pid != 0 && !current->wait_msg) {
        struct proc *p = current;                       /* stopped: not runnable until proc_cont */
        p->stop_req = 0;
        p->stop_rep = 0;
        p->state = P_STOP;
        struct proc *par = proc_by_pid(p->ppid);
        if (par && par->state == P_WAIT && (par->wait_for == -1 || par->wait_for == p->pid))
            ready(par);
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

void wake_io(void)
{
    if (!nio)
        return;
    for (int i = 1; i < NPROC; i++)
        if (procs[i].state == P_IO)
            ready(&procs[i]);
}

void block_io(void)
{
    u16 s = irq_save();                 /* state and count change together */
    current->state = P_IO;
    nio++;
    irq_restore(s);
    schedule();
}

void proc_stop(struct proc *p)
{
    if (p->pid == 0 || p->priv || p->state == P_ZOMBIE || p->state == P_FREE || p->state == P_STOP)
        return;
    p->stop_req = 1;
    if (p->state == P_IO || p->state == P_SLEEP || p->state == P_WAIT ||
        (p->state == P_MSG && !p->wait_msg))            /* wake it so that it notices */
        ready(p);
}

void proc_cont(struct proc *p)
{
    p->stop_req = 0;
    if (p->state == P_STOP)
        ready(p);
}

void proc_kill(struct proc *p)
{
    p->killed = 1;
    if (p->state == P_STOP || p->state == P_IO || p->state == P_SLEEP || p->state == P_WAIT ||
        (p->state == P_MSG && !p->wait_msg))        /* a server in recv, not a client in call */
        ready(p);
}

void proc_exit(int code)
{
    ints_off();
    struct proc *p = current;
    for (int i = 0; i < NFD; i++)
        if (p->fd[i]) {
            file_close(p->fd[i]);
            p->fd[i] = 0;
        }
    if (tty_getfg(tty_of(p)) == p->pid)
        tty_setfg(tty_of(p), 0);
    p->exit_code = code;
    p->state = P_ZOMBIE;
    struct proc *par = proc_by_pid(p->ppid);
    if (par && par->state == P_WAIT &&
        (par->wait_for == -1 || par->wait_for == p->pid))
        ready(par);
    tty_release(p->pid);
    ipc_proc_exit(p->pid);
    proc_reap_orphans();                /* its zombie children have nobody to wait for them now */
    wake_io();
    schedule();
    for (;;)
        ;
}

/* wait for a child; returns its exit code and releases it, -1 if none. With WAIT_CONSOLE (root, init) it also returns
 * ST_CONSOLE + n when console n was shown with nobody on it (con_switch), so that init starts a login there. */
int proc_wait(int pid, int flags)
{
    int nohang = flags & WAIT_NOHANG;
    int con = (flags & WAIT_CONSOLE) && current->uid == 0;
    for (;;) {
        int have = 0;
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
            have = 1;
            if (c->state == P_STOP && (flags & WAIT_UNTRACED) && !c->stop_rep) {
                c->stop_rep = 1;
                return ST_STOPPED;
            }
            if (c->state == P_ZOMBIE) {
                int code = c->exit_code;
                mem_free_owner((u32)c->pid);
                c->state = P_FREE;
                return code;
            }
        }
        if (!have && !con)
            return -1;
        if (nohang)
            return have ? -2 : -1;
        current->wait_for = pid;
        current->wait_con = (u8)con;    /* con_switch readies it; the check above and this block have no kwork between */
        current->state = P_WAIT;
        schedule();
        current->wait_con = 0;
    }
}

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
            if (!par || par->state == P_ZOMBIE) {
                mem_free_owner((u32)c->pid);
                c->state = P_FREE;
            }
        }
}
