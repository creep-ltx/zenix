#include "kernel.h"

/* Processes. No MMU: all processes share one address space, so the
 * kernel never trusts a user pointer (user_ok) and contains a crashing
 * process by killing it and giving back everything it owned. */

struct proc procs[NPROC];
struct proc *current;
static int next_pid = 1;
static struct proc *rq_head, *rq_tail;  /* runnable processes, round robin */
int nsleep, nio;                        /* how many are in P_SLEEP / P_IO */

static void rq_push(struct proc *p)
{
    p->rnext = 0;
    p->queued = 1;
    if (rq_tail)
        rq_tail->rnext = p;
    else
        rq_head = p;
    rq_tail = p;
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
        if (p->pid != 0 && !p->queued)
            rq_push(p);
    }
    irq_restore(s);
}
static int slice;
#define KSTACK 4096u
#define USTACK_DEFAULT 16384u
#define FILE_MAX (256u * 1024)

void proc_init(void)
{
    memset(procs, 0, sizeof procs);
    procs[0].state = P_READY;           /* pid 0: idle, runs on the boot stack */
    procs[0].pid = 0;
    memcpy(procs[0].name, "idle", 5);
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
static int suid_override = -1;        /* set by proc_spawn for a set-user-id program */

static int proc_load(const u8 *file, u32 fsize, const char *path, char *const *argv,
                     int argc, struct proc *parent, const int *stdio, int tty, int priv)
{
    const struct zexe *h = (const struct zexe *)file;
    if (fsize < sizeof *h || h->magic != ZEXE_MAGIC || h->image_size > fsize ||
        h->mem_size < h->image_size || h->reloc_off + h->nreloc * 4 > fsize)
        return -5;

    struct proc *p = 0;
    for (int i = 1; i < NPROC; i++)
        if (procs[i].state == P_FREE) {
            p = &procs[i];
            break;
        }
    if (!p)
        return -6;

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
        return -7;
    }
    memcpy(base, file, h->image_size);
    memset(base + h->image_size, 0, span - h->image_size);
    const u32 *rel = (const u32 *)(file + h->reloc_off);
    for (u32 i = 0; i < h->nreloc; i++)
        *(u32 *)(base + rel[i]) += (u32)base;

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
    p->cwd_ino = parent ? parent->cwd_ino : 2;
    p->uid = parent ? parent->uid : 0;
    p->ruid = parent ? parent->ruid : 0;
    p->gid = parent ? parent->gid : 0;
    p->umask = parent ? parent->umask : 022;
    if (suid_override >= 0) p->uid = (u16)suid_override;
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

    /* file descriptors */
    if (parent && tty > 0) {
        /* started on a workspace (by the session manager): own tty, parent cwd */
        struct file *t = file_new(FT_TTY);
        t->off = (u32)tty;
        for (int i = 0; i < 3; i++)
            p->fd[i] = i == 0 ? t : file_dup(t);
    } else if (parent) {
        /* only fds 0-2 are passed on (so a pipe's write end cannot leak
         * into a reader and keep it from seeing end-of-file) */
        for (int i = 0; i < 3; i++) {
            int src = (stdio && stdio[i] >= 0) ? stdio[i] : i;
            if (src < NFD && parent->fd[src])
                p->fd[i] = file_dup(parent->fd[src]);
        }
    } else {
        struct file *t = file_new(FT_TTY);
        t->off = (u32)tty;
        for (int i = 0; i < 3; i++)
            p->fd[i] = i == 0 ? t : file_dup(t);
    }
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
    u8 *file;
    u32 size;
    struct ustat st;
    long r = fs_read_file(path, parent ? parent->cwd_ino : 2, &file, &size, &st);
    if (r < 0)
        return (int)r;
    suid_override = (st.mode & 04000) ? (int)st.uid : -1;   /* set-user-id: the program runs as its owner (su, passwd) */
    int pid = proc_load(file, size, path, argv, argc, parent, stdio, tty, 0);
    suid_override = -1;
    kfree(file);
    return pid;
}

/* a program built into the kernel image (no filesystem needed) */
int proc_spawn_image(const void *img, u32 len, const char *name, int tty, int priv)
{
    char *argv[] = { (char *)name, 0 };
    return proc_load(img, len, name, argv, 1, 0, 0, tty, priv);
}

void schedule(void)
{
    u16 s = irq_save();
    struct proc *old = current, *n;

    if (old->state == P_READY && old->pid != 0 && !old->queued)
        rq_push(old);                   /* yielded or preempted: back of the line */
    if (nsleep)
        for (int i = 1; i < NPROC; i++)
            if (procs[i].state == P_SLEEP && (s32)(ticks - procs[i].wake) >= 0)
                ready(&procs[i]);
    n = rq_head;
    if (n) {
        rq_head = n->rnext;
        if (!rq_head)
            rq_tail = 0;
        n->queued = 0;
    } else {
        n = &procs[0];                  /* nothing to run: idle */
    }
    slice = SLICE;
    if (n != old) {
        current = n;
        switch_to(old, n);
    }
    irq_restore(s);
    /* a process that was killed while blocked or preempted dies here */
    if (current->killed && current->state == P_READY && current->pid != 0)
        proc_exit(130);
}

void sched_tick(u16 sr)
{
    if (!(sr & 0x2000) && current->pid != 0 && --slice <= 0)
        schedule();
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

void proc_kill(struct proc *p)
{
    p->killed = 1;
    if (p->state == P_IO || p->state == P_SLEEP || p->state == P_WAIT ||
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
    wake_io();
    schedule();
    for (;;)
        ;
}

/* wait for a child; returns its exit code and releases it, -1 if none */
int proc_wait(int pid, int nohang)
{
    for (;;) {
        int have = 0;
        for (int i = 1; i < NPROC; i++) {
            struct proc *c = &procs[i];
            if (c->state == P_FREE || c->ppid != current->pid ||
                (pid != -1 && c->pid != pid))
                continue;
            have = 1;
            if (c->state == P_ZOMBIE) {
                int code = c->exit_code;
                mem_free_owner((u32)c->pid);
                c->state = P_FREE;
                return code;
            }
        }
        if (!have)
            return -1;
        if (nohang)
            return -2;
        current->wait_for = pid;
        current->state = P_WAIT;
        schedule();
    }
}

/* idle calls this: free zombies whose parent is gone */
void proc_reap_orphans(void)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *c = &procs[i];
        if (c->state == P_ZOMBIE && (c->ppid == 0 || !proc_by_pid(c->ppid))) {
            mem_free_owner((u32)c->pid);
            c->state = P_FREE;
        }
    }
}
