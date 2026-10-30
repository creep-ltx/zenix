/* SPDX-License-Identifier: GPL-2.0-only */
/* The network's drivers (DESIGN D19 2.3, 2.4, 5; docs/d19-network.md step 7): SYS_drv. A driver is a user process
 * the network server starts as its child; the kernel gives it only what user mode cannot do safely:
 *   - a claim on a device the hardware table names (the only owner; a second claim is -EBUSY);
 *   - DMA memory that belongs to the claim, not to a process, so hardware still writing after a death never writes
 *     into memory the kernel gave someone else;
 *   - a quiesce program (register writes that stop the device), run when the driver ends and when the network server
 *     ends, before anything of either is freed; then the claim's DMA memory is freed and the claim ends;
 *   - cache maintenance over its DMA memory (Emu68's ARM cache; a 68k runs with the data cache off);
 *   - its blocks (DRV_ATTACH, DRV_XFER) carried to "net" as kernel messages that never start a server.
 * Every op is for a child of the running network server only (not merely root: no sudo session claims a device);
 * the claim's ops for its holder only. Cold code: -Os in its own file. */
#include "kernel.h"
#include "netproto.h"
#include "hwinfo.h"

_Static_assert(sizeof(struct udrvattach) == UDRVATTACH_FIRST, "udrvattach's first size is frozen");
_Static_assert(sizeof(struct udrvxfer) == UDRVXFER_FIRST, "udrvxfer's first size is frozen");
_Static_assert(sizeof(struct hwinfo) == HWINFO_FIRST, "the hardware table's first size is frozen");

#define NCLAIM 4
#define DRV_OWNER(i) (0x80000100u + (u32)(i))  /* a DMA region's owner: its claim, never a pid (as SEG_OWNER) */
#define DMA_MAX (4u << 20)                      /* the most DMA memory a claim may have */
static struct claim {
    int pid;                    /* the holder; 0: free */
    u32 base, size;             /* the device's register window */
    u32 dma, dmalen;            /* the DMA region (0: none) */
    void *blk;                  /* the block it was cut from */
    u32 nq;
    struct udrvstep q[DRV_QSTEPS];
} claims[NCLAIM];

static const struct hwinfo *hw(void)
{
    const struct hwinfo *h = (const struct hwinfo *)HWINFO_ADDR;
    return h->magic == HWINFO_MAGIC ? h : 0;
}

/* the register window of the device at base: GENET's, or a Zorro board's (never a RAM board) */
static long window(u32 base, u32 *size)
{
    const struct hwinfo *h = hw();
    if (!h || !base)
        return -ENODEV;
    if (h->genet && base == h->genet) {
        *size = 0x10000;
        return 0;
    }
    for (u32 i = 0; i < h->nboards && i < HW_MAXBOARDS; i++) {
        const struct hwboard *b = &h->board[i];
        if (b->addr == base && b->size && !(b->type & 0x20)) {
            *size = b->size;
            return 0;
        }
    }
    return -ENODEV;
}

static void quiesce(const struct claim *c)
{
    for (u32 k = 0; k < c->nq; k++) {
        const struct udrvstep *s = &c->q[k];
        u32 v = s->value;
        if (s->size == 1) {
            volatile u8 *a = (volatile u8 *)s->addr;
            *a = (u8)(s->op == DRVS_RMW ? (*a & ~s->mask) | v : v);
        } else if (s->size == 2) {
            volatile u16 *a = (volatile u16 *)s->addr;
            *a = (u16)(s->op == DRVS_RMW ? (*a & ~s->mask) | v : v);
        } else {
            volatile u32 *a = (volatile u32 *)s->addr;
            *a = s->op == DRVS_RMW ? (*a & ~s->mask) | v : v;
        }
    }
}

/* the claim ends: the device stopped first; its DMA memory is given to the driver's process, freed with the rest of
 * it when its zombie is reaped (audit 2026-10-28 S2). Not at once: when a driver ends, netd learns it only at its next
 * wait for its children, and until then it may still copy a frame into the send buffers there; when netd ends, the
 * driver still runs until it sees its doorbell close and may still touch the region. */
static void release(struct claim *c, const char *why)
{
    quiesce(c);
    if (c->blk)
        mem_set_owner(c->blk, (u32)c->pid);
    kprintf("drv: claim of %p (pid %d) released (%s): %u quiesce steps run%s\n", c->base, c->pid, why, c->nq,
            c->blk ? ", its DMA memory freed when the driver is reaped" : "");
    memset(c, 0, sizeof *c);
}

/* p is dying (proc_exit, before anything of it is freed): its claims end; the network server's end ends every claim
 * (only its children hold any) */
void drv_exit(struct proc *p)
{
    int srv = svc_pid("net") == p->pid;
    for (int i = 0; i < NCLAIM; i++)
        if (claims[i].pid && (claims[i].pid == p->pid || srv))
            release(&claims[i], claims[i].pid == p->pid ? "the driver ended" : "the network server ended");
}

/* DESIGN D29 7.2 step 6: before ZINE hands the machine over, every claimed device is stopped by its quiesce program
 * and its driver killed: it is not scheduled before the resume, dies then (its claim released as above), and the
 * network server starts it again (D19's restart) */
void drv_zine(void)
{
    for (int i = 0; i < NCLAIM; i++)
        if (claims[i].pid) {
            quiesce(&claims[i]);
            struct proc *p = proc_by_pid(claims[i].pid);
            if (p)
                proc_kill(p);
        }
}

static struct claim *mine(u32 id)
{
    if (id < 1 || id > NCLAIM || claims[id - 1].pid != current->pid)
        return 0;
    return &claims[id - 1];
}

/* the caller's claim whose DMA region holds [a, a + n) */
static struct claim *dma_of(u32 a, u32 n)
{
    for (int i = 0; i < NCLAIM; i++) {
        struct claim *c = &claims[i];
        if (c->pid == current->pid && c->dma && a >= c->dma && a + n >= a && a + n <= c->dma + c->dmalen)
            return c;
    }
    return 0;
}

static long cache(u32 how, u32 a, u32 n)
{
    const struct hwinfo *h = hw();
    if (!n || (how != DRVC_CLEAN && how != DRVC_INVAL))
        return -EINVAL;
    if (!dma_of(a, n))
        return -EFAULT;
    if (!h || !(h->flags & HW_DT))
        return 0;                       /* a real 68k: Zenix keeps the data cache off (start.S) */
    u32 lo = a & ~63u, hi = (a + n + 63) & ~63u;    /* whole lines, inside the 64-byte aligned region */
    if (h->flags & HW_RANGEOPS) {
        /* Emu68's range opcodes (base A0, length D1): clean, or clean and invalidate; never a bare invalidate */
        if (how == DRVC_CLEAN)
            __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1C00" :: "r"(lo), "r"(hi - lo) : "a0", "d1", "memory");
        else
            __asm__ volatile("move.l %0,%%a0\n\tmove.l %1,%%d1\n\t.word 0xF460,0x1800" :: "r"(lo), "r"(hi - lo) : "a0", "d1", "memory");
        return 0;
    }
    for (u32 p = lo; p < hi; p += 16)   /* cpushl dc,(a0): write back and drop the line (Emu68: dc civac) */
        __asm__ volatile("move.l %0,%%a0\n\t.word 0xF468" :: "r"(p) : "a0", "memory");
    return 0;
}

/* a driver's block to the network server: a kernel message that never starts it (an orphaned driver whose server is
 * gone gets -ECONNRESET; it must not start a second server that would start a second driver for the same card) */
static long forward(u32 type, u32 blk, u32 size, u32 first)
{
    if (size < first || size > 4096)
        return -EINVAL;
    if (!user_ok_wr(current, blk, size) || (blk & 1))
        return -EFAULT;
    if (*(const u32 *)blk != size)
        return -EINVAL;                 /* the block says its own size: both must agree */
    int port = port_find("net");
    if (port < 0)
        return -ECONNRESET;
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.a[0] = (u32)current->pid;
    m.a[1] = blk;
    m.a[2] = size;
    long r = ipc_kcall(port, &m, SVC_NS_SKIP, 0);
    return (m.kflags & UMSG_NETREPLY) ? r : -ECONNRESET;
}

/* SYS_drv (op, a, b) */
long drv_sys(u32 opw, u32 a, u32 b)
{
    u32 op = opw & 0xFF;
    int srv = svc_pid("net");
    if (!srv)
        return -ECONNRESET;             /* no network server: an orphaned driver learns it is gone */
    if (current->ppid != srv)
        return -EPERM;                  /* only the running network server's children */
    switch (op) {
    case DRV_HWINFO: {
        const struct hwinfo *h = hw();
        u32 n = b ? b : HWINFO_FIRST;
        if (!h)
            return -ENODEV;
        if (n < HWINFO_FIRST)
            return -EINVAL;
        if (n > sizeof *h)
            n = sizeof *h;
        if (!user_ok_wr(current, a, n))
            return -EFAULT;
        memcpy((void *)a, h, n);
        return (long)n;
    }
    case DRV_CLAIM: {
        u32 size;
        long e = window(a, &size);
        int f = -1;
        if (e)
            return e;
        for (int i = 0; i < NCLAIM; i++) {
            if (claims[i].pid && claims[i].base == a)
                return -EBUSY;
            if (!claims[i].pid && f < 0)
                f = i;
        }
        if (f < 0)
            return -EBUSY;
        memset(&claims[f], 0, sizeof claims[f]);
        claims[f].pid = current->pid;
        claims[f].base = a;
        claims[f].size = size;
        kprintf("drv: pid %d (%s) claims the device at %p (%u KB)\n", current->pid, current->name, a, size / 1024);
        return f + 1;
    }
    case DRV_DMAMEM: {
        struct claim *c = mine(a);
        if (!c)
            return -EPERM;
        if (c->dma)
            return -EBUSY;
        if (!b || b > DMA_MAX)
            return -EINVAL;
        u32 n = (b + 63) & ~63u;
        void *blk = mem_alloc(n + 64, 0, DRV_OWNER(c - claims));
        if (!blk)
            return -ENOMEM;
        c->blk = blk;
        c->dma = ((u32)blk + 63) & ~63u;
        c->dmalen = n;
        memset((void *)c->dma, 0, n);
        return (long)c->dma;
    }
    case DRV_QUIESCE: {
        struct claim *c = mine(a);
        struct udrvquiesce q;
        if (!c)
            return -EPERM;
        if (!user_ok_rd(current, b, sizeof q))
            return -EFAULT;
        memcpy(&q, (const void *)b, sizeof q);
        if (q.n > DRV_QSTEPS)
            return -EINVAL;
        for (u32 k = 0; k < q.n; k++) {
            const struct udrvstep *s = &q.step[k];
            if ((s->op != DRVS_WRITE && s->op != DRVS_RMW) || (s->size != 1 && s->size != 2 && s->size != 4) ||
                (s->addr & (s->size - 1)) || s->addr < c->base || s->addr - c->base > c->size - s->size)
                return -EINVAL;         /* a bus error here would be a kernel crash: only the claimed window */
        }
        memcpy(c->q, q.step, sizeof c->q);
        c->nq = q.n;
        return 0;
    }
    case DRV_CACHE:
        return cache(opw >> 8, a, b);
    case DRV_ATTACH:
        return forward(NETDRV_ATTACH, a, b, UDRVATTACH_FIRST);
    case DRV_XFER:
        return forward(NETDRV_XFER, a, b, UDRVXFER_FIRST);
    }
    return -EINVAL;
}
