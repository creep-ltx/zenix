#include "kernel.h"

/* Processes. No MMU: all processes share one address space, so the
 * kernel never trusts a user pointer (user_ok) and contains a crashing
 * process by killing it and giving back everything it owned. */

struct proc procs[NPROC];
struct proc *current;
static int next_pid = 1;
static int slice;
#define KSTACK 4096u
#define USTACK_DEFAULT 16384u

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
    return ptr >= p->base && ptr + len >= ptr && ptr + len <= p->base + p->span;
}

static struct proc *by_pid(int pid)
{
    for (int i = 0; i < NPROC; i++)
        if (procs[i].state != P_FREE && procs[i].pid == pid)
            return &procs[i];
    return 0;
}

int proc_spawn(const char *name, int ppid)
{
    u32 fsize;
    u8 *file = bootdisk_load(name, &fsize);
    if (!file)
        return -1;
    struct zexe *h = (struct zexe *)file;
    if (fsize < sizeof *h || h->magic != ZEXE_MAGIC ||
        h->image_size > fsize || h->mem_size < h->image_size ||
        h->reloc_off + h->nreloc * 4 > fsize) {
        kfree(file);
        return -2;
    }

    struct proc *p = 0;
    for (int i = 1; i < NPROC; i++)
        if (procs[i].state == P_FREE) {
            p = &procs[i];
            break;
        }
    if (!p) {
        kfree(file);
        return -3;
    }

    int pid = next_pid++;
    u32 stack = h->stack_size ? h->stack_size : USTACK_DEFAULT;
    u32 span = ((h->mem_size + 15) & ~15u) + ((stack + 15) & ~15u);
    u8 *base = mem_alloc(span, 0, (u32)pid);
    u32 *ks = mem_alloc(KSTACK, 0, (u32)pid);
    if (!base || !ks) {
        mem_free_owner((u32)pid);
        kfree(file);
        return -4;
    }
    memcpy(base, file, h->image_size);
    memset(base + h->image_size, 0, span - h->image_size);
    const u32 *rel = (const u32 *)(file + h->reloc_off);
    for (u32 i = 0; i < h->nreloc; i++)
        *(u32 *)(base + rel[i]) += (u32)base;
    kfree(file);

    /* kernel stack for the first switch_to: d2-d7,a2-a6 (11 longs) and
     * the return address */
    u32 *top = ks + KSTACK / 4;
    *--top = (u32)user_first_entry;
    for (int i = 0; i < 11; i++)
        *--top = 0;

    memset(p, 0, sizeof *p);
    p->ksp = top;
    p->usp = (u32)base + span;
    p->entry = (u32)base;
    p->pid = pid;
    p->ppid = ppid;
    p->base = (u32)base;
    p->span = span;
    for (int i = 0; i < 15 && name[i]; i++)
        p->name[i] = name[i];
    p->state = P_READY;
    return pid;
}

void schedule(void)
{
    u16 s = irq_save();
    struct proc *old = current, *n = 0;
    int oi = (int)(old - procs);

    for (int i = 1; i < NPROC; i++)
        if (procs[i].state == P_SLEEP && (s32)(ticks - procs[i].wake) >= 0)
            procs[i].state = P_READY;
    for (int k = 1; k <= NPROC; k++) {
        struct proc *p = &procs[(oi + k) % NPROC];
        if (p->state == P_READY && p->pid != 0) {
            n = p;
            break;
        }
    }
    if (!n)
        n = &procs[0];
    slice = SLICE;
    if (n != old) {
        current = n;
        switch_to(old, n);
    }
    irq_restore(s);
}

void sched_tick(u16 sr)
{
    if (!(sr & 0x2000) && current->pid != 0 && --slice <= 0)
        schedule();
}

void proc_exit(int code)
{
    ints_off();
    struct proc *p = current;
    p->exit_code = code;
    p->state = P_ZOMBIE;
    struct proc *par = by_pid(p->ppid);
    if (par && par->state == P_WAIT &&
        (par->wait_for == -1 || par->wait_for == p->pid))
        par->state = P_READY;
    schedule();
    for (;;)
        ;
}

/* wait for a child; returns its exit code and releases it, -1 if none */
int proc_wait(int pid)
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
        if (c->state == P_ZOMBIE && !by_pid(c->ppid)) {
            mem_free_owner((u32)c->pid);
            c->state = P_FREE;
        }
    }
}

/* ---- boot disk: raw table in sector 0, files at their LBA ---- */

static struct zdir zd;

static u32 zd_base;                     /* sector where the ZDIR table sits */

/* FS-UAE (and real disks) put an RDB in front of a raw image, so the
 * table is not at sector 0: scan the first sectors for it. M2 replaces
 * this with RDB partition parsing and the real filesystem. */
int bootdisk_init(void)
{
    if (ide_init())
        return -1;
    for (u32 lba = 0; lba < 2048; lba++) {
        if (ide_read(lba, 1, &zd))
            return -1;
        if (zd.magic == ZDIR_MAGIC) {
            zd_base = lba;
            kprintf("bootdisk: %u programs at sector %u\n", zd.count, lba);
            return 0;
        }
    }
    kprintf("bootdisk: no ZDIR table in the first 2048 sectors\n");
    return -1;
}

void *bootdisk_load(const char *name, u32 *bytes)
{
    for (u32 i = 0; i < zd.count && i < 15; i++) {
        const char *a = zd.e[i].name, *b = name;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a || *b)
            continue;
        u32 n = zd.e[i].bytes, secs = (n + 511) / 512;
        u8 *buf = kmalloc(secs * 512);
        if (!buf || ide_read(zd_base + zd.e[i].lba, secs, buf)) {
            kfree(buf);
            return 0;
        }
        *bytes = n;
        return buf;
    }
    return 0;
}
