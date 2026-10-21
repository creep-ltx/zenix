#include "kernel.h"
#include "hw.h"

struct proc procs[NPROC];
struct proc *current;
static int nextpid = 1;

static u8 xbuf[131072];         /* ELF load buffer (newlib apps are bigger) */

#define MAXARG 8
#define ARGLEN 64

/* Build the initial user stack in a (new) address space:
 * strings at the top, then a NULL-terminated pointer array, then argc.
 * crt0 pops argc and takes the array address as argv. Returns the USP. */
static u32 build_user_stack(struct proc *p, int argc,
                            char args[MAXARG][ARGLEN])
{
    u32 sp = USTACK_TOP;
    u32 straddr[MAXARG];
    for (int i = argc - 1; i >= 0; i--) {
        u32 len = 1;
        while (args[i][len - 1])
            len++;
        sp -= len;
        copy_to_user(p, sp, args[i], len);
        straddr[i] = sp;
    }
    sp &= ~3u;
    sp -= 4;
    u32 zero = 0;
    copy_to_user(p, sp, &zero, 4);
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 4;
        copy_to_user(p, sp, &straddr[i], 4);
    }
    sp -= 4;
    u32 ac = (u32)argc;
    copy_to_user(p, sp, &ac, 4);
    return sp;
}

void proc_init(void)
{
    procs[0].state = P_READY;   /* proc 0 = boot/idle context */
    procs[0].pid = 0;
    procs[0].crp[0] = crp_empty[0];
    procs[0].crp[1] = crp_empty[1];
    current = &procs[0];
}

static struct proc *proc_alloc(void)
{
    for (int i = 1; i < NPROC; i++) {
        if (procs[i].state != P_FREE)
            continue;
        struct proc *p = &procs[i];
        memset(p, 0, sizeof(*p));
        p->pid = nextpid++;
        p->ppid = current ? current->pid : 0;
        p->crp[0] = 0x7FFF0002u;
        *(u16 *)p->fpframe = 0;             /* null FPU state */
        for (int f = 0; f < 3; f++)
            p->files[f].type = FD_CONS;     /* stdin/out/err = console */
        return p;
    }
    return 0;
}

static void map_ustack(u32 *uroot)
{
    for (u32 i = 1; i <= USTACK_PAGES; i++)
        umem_map(uroot, USTACK_TOP - i * PAGE_SIZE, pmm_alloc(), 0);
}

/* kstack for first dispatch via user_first_entry (fresh user context) */
static void kstack_fresh(struct proc *p)
{
    u32 *sp = &p->kstack[1024];
    *--sp = (u32)user_first_entry;
    for (int i = 0; i < 11; i++)
        *--sp = 0;
    p->ksp = sp;
}

/* kernel-side spawn (init): load an ELF from FAT into a new process */
int proc_spawn_path(const char *name)
{
    int n = fat_read_file(name, xbuf, sizeof xbuf);
    if (n <= 0)
        return -1;
    struct proc *p = proc_alloc();
    if (!p)
        return -1;
    p->uroot = (u32 *)pmm_alloc();
    u32 entry;
    if (elf_load(p->uroot, xbuf, (u32)n, &entry, &p->brk)) {
        umem_free_root(p->uroot);
        p->state = P_FREE;
        return -1;
    }
    map_ustack(p->uroot);
    char args[MAXARG][ARGLEN];
    int i;
    for (i = 0; name[i] && i < ARGLEN - 1; i++)
        args[0][i] = name[i];
    args[0][i] = 0;
    p->usp = build_user_stack(p, 1, args);
    p->entry = entry;
    p->crp[1] = (u32)p->uroot;
    kstack_fresh(p);
    p->state = P_READY;
    kprintf("proc: spawned %s as pid %d (entry %p)\n", name, p->pid, entry);
    return p->pid;
}

void schedule(void)
{
    u16 s = irq_save();
    struct proc *prev = current;
    int start = (int)(prev - procs);
    for (int k = 1; k <= NPROC; k++) {
        struct proc *p = &procs[(start + k) % NPROC];
        if (p->state != P_READY)
            continue;
        if (p == prev)
            break;
        fpu_save(prev);
        current = p;
        switch_to(prev, p);
        fpu_restore(current);
        irq_restore(s);
        return;
    }
    irq_restore(s);
    if (current->state != P_READY)
        panic("schedule: nothing runnable");
}

/* ---- console wakeup (called from the serial RX interrupt) ---- */

static struct proc *console_waiter;

void console_wake(void)
{
    if (console_waiter && console_waiter->state == P_WAITING) {
        console_waiter->state = P_READY;
        console_waiter = 0;
    }
}

/* ---- file descriptors ---- */

static struct file *fd_get(long fd)
{
    if (fd < 0 || fd >= NFD || current->files[fd].type == FD_NONE)
        return 0;
    return &current->files[fd];
}

static long fd_close(struct proc *p, long fd)
{
    struct file *f = &p->files[fd];
    long r = 0;
    if (f->type == FD_WFILE) {
        if (fat_write_file(f->name, f->wbuf, f->wlen))
            r = -1;
        kfree(f->wbuf);
    }
    f->type = FD_NONE;
    return r;
}

static void fd_close_all(struct proc *p)
{
    for (int i = 0; i < NFD; i++)
        if (p->files[i].type != FD_NONE)
            fd_close(p, i);
}

static long sys_open(u32 upath, long mode)
{
    char path[16];
    if (strncpy_from_user(current, path, upath, sizeof path) < 0)
        return -1;
    int fd = -1;
    for (int i = 0; i < NFD; i++)
        if (current->files[i].type == FD_NONE) {
            fd = i;
            break;
        }
    if (fd < 0)
        return -1;
    struct file *f = &current->files[fd];
    memset(f, 0, sizeof(*f));
    if (mode == 0) {
        if (fat_stat(path, &f->first, &f->size))
            return -1;
        f->type = FD_RFILE;
    } else {
        f->wbuf = kmalloc(WFILE_MAX);
        f->wlen = 0;
        int i;
        for (i = 0; path[i] && i < 12; i++)
            f->name[i] = path[i];
        f->name[i] = 0;
        f->type = FD_WFILE;
    }
    return fd;
}

static long sys_read(long fd, u32 ubuf, long n)
{
    struct file *f = fd_get(fd);
    if (!f || n < 0)
        return -1;
    if (f->type == FD_CONS) {
        /* block until at least one byte (keyboard or serial); the ISRs
         * fill the console ring and wake us. Pending signals interrupt
         * the wait (EINTR-style). */
        u8 tmp[64];
        long got = 0;
        for (;;) {
            u16 s = irq_save();
            int c;
            while (got < n && got < 64 && (c = con_getc_buffered()) >= 0)
                tmp[got++] = (u8)c;
            if (got) {
                irq_restore(s);
                break;
            }
            console_waiter = current;
            current->state = P_WAITING;
            irq_restore(s);
            schedule();
            if (sig_deliverable(current))
                return -1;
        }
        if (copy_to_user(current, ubuf, tmp, (u32)got))
            return -1;
        return got;
    }
    if (f->type != FD_RFILE)
        return -1;
    static u8 kbuf[512];
    long total = 0;
    while (n > 0) {
        u32 chunk = n > 512 ? 512 : (u32)n;
        int got = fat_read_at(f->first, f->size, f->pos, kbuf, chunk);
        if (got < 0)
            return -1;
        if (got == 0)
            break;
        if (copy_to_user(current, ubuf, kbuf, (u32)got))
            return -1;
        f->pos += (u32)got;
        ubuf += (u32)got;
        n -= got;
        total += got;
    }
    return total;
}

static long sys_write(long fd, u32 ubuf, long n)
{
    struct file *f = fd_get(fd);
    if (!f || n < 0)
        return -1;
    static u8 kbuf[256];
    long total = 0;
    while (n > 0) {
        u32 chunk = n > 256 ? 256 : (u32)n;
        if (copy_from_user(current, kbuf, ubuf, chunk))
            return -1;
        if (f->type == FD_CONS) {
            for (u32 i = 0; i < chunk; i++)
                con_putc((char)kbuf[i]);
        } else if (f->type == FD_WFILE) {
            if (f->wlen + chunk > WFILE_MAX)
                return -1;
            memcpy(f->wbuf + f->wlen, kbuf, chunk);
            f->wlen += chunk;
        } else
            return -1;
        ubuf += chunk;
        n -= chunk;
        total += chunk;
    }
    return total;
}

/* ---- process lifecycle ---- */

static void wake_parent(struct proc *p)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].pid == p->ppid && procs[i].state == P_WAITING)
            procs[i].state = P_READY;
}

static void proc_exit(int code) __attribute__((noreturn));
static void proc_exit(int code)
{
    fd_close_all(current);
    if (current->uroot) {
        umem_free_root(current->uroot);
        current->uroot = 0;
        current->crp[1] = crp_empty[1];
        mmu_switch_crp(crp_empty);
    }
    current->exit_code = code;
    current->state = P_ZOMBIE;
    if (current->pid == 1)
        panic("init exited");
    wake_parent(current);
    schedule();
    panic("zombie resumed");
}

static long sys_fork(struct uregs *r)
{
    struct proc *c = proc_alloc();
    if (!c)
        return -1;
    c->uroot = (u32 *)pmm_alloc();

    /* eager copy of every mapped page, preserving WP */
    u32 *pr = current->uroot;
    for (u32 ai = 0; ai < 128; ai++) {
        if ((pr[ai] & 3) != 2)
            continue;
        u32 *bt = (u32 *)(pr[ai] & ~0xFu);
        for (u32 bi = 0; bi < 128; bi++) {
            if ((bt[bi] & 3) != 2)
                continue;
            u32 *ct = (u32 *)(bt[bi] & ~0xFu);
            for (u32 ci = 0; ci < 64; ci++) {
                if ((ct[ci] & 3) != 1)
                    continue;
                u32 np = pmm_alloc();
                memcpy((void *)np, (void *)(ct[ci] & ~(PAGE_SIZE - 1)),
                       PAGE_SIZE);
                u32 va = (ai << 25) | (bi << 18) | (ci << 12);
                umem_map(c->uroot, va, np, ct[ci] & PD_WP);
            }
        }
    }

    c->usp = get_usp();
    c->entry = current->entry;
    c->crp[1] = (u32)c->uroot;

    /* fds: inherit read/console fds; write-buffered fds stay parent-only */
    for (int i = 0; i < NFD; i++)
        if (current->files[i].type == FD_CONS ||
            current->files[i].type == FD_RFILE)
            c->files[i] = current->files[i];

    fpu_save(current);
    memcpy(c->fpframe, current->fpframe, sizeof c->fpframe);
    memcpy(c->fpregs, current->fpregs, sizeof c->fpregs);
    memcpy(c->sighandler, current->sighandler, sizeof c->sighandler);

    /* child kstack: rte frame + parent's user regs (d0=0) + trampoline */
    u8 *top = (u8 *)&c->kstack[1024];
    top -= 8;
    *(u16 *)top = FRAME_SR(r);
    *(u32 *)(top + 2) = FRAME_PC(r);
    *(u16 *)(top + 6) = 0;
    top -= 60;
    memcpy(top, r, 60);
    ((u32 *)top)[0] = 0;                /* child's fork() returns 0 */
    top -= 4;
    *(u32 *)top = (u32)fork_child_entry;
    u32 *sp = (u32 *)top;
    for (int i = 0; i < 11; i++)
        *--sp = 0;
    c->ksp = sp;

    c->state = P_READY;
    return c->pid;
}

static long sys_exec(struct uregs *r, u32 upath, u32 uargv)
{
    char path[16];
    if (strncpy_from_user(current, path, upath, sizeof path) < 0)
        return -1;

    /* marshal argv while the OLD address space is still live */
    static char args[MAXARG][ARGLEN];
    int argc = 0;
    if (uargv) {
        for (; argc < MAXARG; argc++) {
            u32 up;
            if (copy_from_user(current, &up, uargv + 4 * (u32)argc, 4))
                return -1;
            if (!up)
                break;
            if (strncpy_from_user(current, args[argc], up, ARGLEN) < 0)
                return -1;
        }
    } else {
        int i;
        for (i = 0; path[i]; i++)
            args[0][i] = path[i];
        args[0][i] = 0;
        argc = 1;
    }

    int n = fat_read_file(path, xbuf, sizeof xbuf);
    if (n <= 0)
        return -1;

    u32 *newroot = (u32 *)pmm_alloc();
    u32 entry, newbrk;
    if (elf_load(newroot, xbuf, (u32)n, &entry, &newbrk)) {
        umem_free_root(newroot);
        return -1;
    }
    map_ustack(newroot);

    u32 *old = current->uroot;
    current->uroot = newroot;
    current->crp[1] = (u32)newroot;
    current->entry = entry;
    current->brk = newbrk;
    mmu_switch_crp(current->crp);
    umem_free_root(old);

    fpu_reset();
    *(u16 *)current->fpframe = 0;
    sig_on_exec(current);

    /* rebuild the user context: fresh registers, argv stack, new entry */
    u32 usp = build_user_stack(current, argc, args);
    memset(r, 0, 60);
    FRAME_SR(r) = 0;
    FRAME_PC(r) = entry;
    set_usp(usp);
    return 0;
}

static long sys_lseek(long fd, long off, long whence)
{
    struct file *f = fd_get(fd);
    if (!f)
        return -1;
    if (f->type != FD_RFILE)
        return 0;
    s32 base = whence == 1 ? (s32)f->pos : whence == 2 ? (s32)f->size : 0;
    s32 np = base + (s32)off;
    if (np < 0)
        return -1;
    f->pos = (u32)np;
    return (long)f->pos;
}

static long sys_readdir(long index, u32 ubuf)
{
    struct udirent de;
    memset(&de, 0, sizeof de);
    if (fat_readdir((u32)index, de.name, &de.size))
        return -1;
    if (copy_to_user(current, ubuf, &de, sizeof de))
        return -1;
    return 0;
}

static long sys_brk(u32 addr)
{
    if (addr == 0)
        return (long)current->brk;
    if (addr < UVA_BASE || addr > UBRK_MAX)
        return -1;
    u32 old = (current->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u32 new = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (u32 va = old; va < new; va += PAGE_SIZE)
        if (!uva_to_pa(current->uroot, va))
            umem_map(current->uroot, va, pmm_alloc(), 0);
    current->brk = addr;                /* shrink: keep pages, move mark */
    return (long)addr;
}

static long sys_wait(u32 ustatus)
{
    for (;;) {
        int have_kids = 0;
        for (int i = 1; i < NPROC; i++) {
            struct proc *p = &procs[i];
            if (p->state == P_FREE || p->ppid != current->pid)
                continue;
            have_kids = 1;
            if (p->state == P_ZOMBIE) {
                int pid = p->pid;
                if (ustatus) {
                    s32 st = p->exit_code;
                    if (copy_to_user(current, ustatus, &st, 4))
                        return -1;
                }
                p->state = P_FREE;
                return pid;
            }
        }
        if (!have_kids)
            return -1;
        current->state = P_WAITING;
        schedule();
        if (sig_deliverable(current))
            return -1;
    }
}

void proc_exit_signal(int code)
{
    current->in_kernel = 1;     /* the exit path does filesystem I/O */
    proc_exit(code);
}

static volatile int need_resched;

static long syscall_dispatch(struct uregs *r)
{
    long a1 = (long)r->d[1], a2 = (long)r->d[2], a3 = (long)r->d[3];
    switch (r->d[0]) {
    case SYS_exit:
        proc_exit((int)a1);
    case SYS_putc:
        con_putc((char)a1);
        return 0;
    case SYS_yield:
        schedule();
        return 0;
    case SYS_getpid:
        return current->pid;
    case SYS_fork:
        return sys_fork(r);
    case SYS_exec:
        return sys_exec(r, (u32)a1, (u32)a2);
    case SYS_wait:
        return sys_wait((u32)a1);
    case SYS_open:
        return sys_open((u32)a1, a2);
    case SYS_close:
        return fd_get(a1) ? fd_close(current, a1) : -1;
    case SYS_read:
        return sys_read(a1, (u32)a2, a3);
    case SYS_write:
        return sys_write(a1, (u32)a2, a3);
    case SYS_lseek:
        return sys_lseek(a1, a2, a3);
    case SYS_readdir:
        return sys_readdir(a1, (u32)a2);
    case SYS_brk:
        return sys_brk((u32)a1);
    case SYS_signal:
        return sys_signal(a1, (u32)a2);
    case SYS_kill:
        return sys_kill(a1, a2);
    case SYS_setfg:
        console_fg = (int)a1;
        return 0;
    case SYS_setkmap: {
        char km[4];
        if (strncpy_from_user(current, km, (u32)a1, sizeof km) < 0)
            return -1;
        return kbd_set_keymap(km);
    }
    default:
        kprintf("\n[kernel] pid %d: bad syscall %d\n", current->pid, r->d[0]);
        return -1;
    }
}

/* Syscalls are not preemptible: shared kernel buffers (FAT sector cache,
 * ELF load buffer, copy bounce buffers) assume one syscall at a time.
 * The tick defers the context switch until the syscall returns;
 * voluntary blocking (console read, wait, yield) is still fine.
 * Returns the (possibly moved) register block pointer — see start.S. */
u32 *syscall_c(u32 *rb)
{
    struct uregs *r = (struct uregs *)rb;
    current->in_kernel = 1;
    u32 *nr = rb;
    if (r->d[0] == SYS_sigreturn) {
        nr = sys_sigreturn(rb);
    } else {
        long ret = syscall_dispatch(r);
        r->d[0] = (u32)ret;
    }
    current->in_kernel = 0;
    if (need_resched) {
        need_resched = 0;
        schedule();
    }
    return sig_deliver(nr);
}

static volatile u32 ticks;

u32 *irq2_c(u32 *rb)
{
    u8 icr = CIAA(CIA_ICR);     /* one read acks all CIA-A sources */
    INTREQ = INTF_PORTS;
    if (icr & 0x08)
        kbd_isr();
    if (icr & 0x01) {
        ticks++;
        serial_tx_drain();
        if (current->in_kernel)
            need_resched = 1;
        else
            schedule();
    }
    return sig_deliver(rb);
}

u32 *irq5_c(u32 *rb)
{
    serial_rx_isr();
    return sig_deliver(rb);
}
