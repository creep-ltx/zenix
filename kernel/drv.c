/* SPDX-License-Identifier: GPL-2.0-only */
/* The network's drivers (DESIGN D19 2.3, 2.4, 5; docs/d19-network.md step 7): SYS_drv. A driver is a user process
 * the network server starts as its child; the kernel gives it only what user mode cannot do safely:
 *   - a claim on a device the hardware table names (the only owner; a second claim is -EBUSY);
 *   - DMA memory that belongs to the claim, not to a process, so hardware still writing after a death never writes
 *     into memory the kernel gave someone else;
 *   - a quiesce program (register writes that stop the device, and bounded waits for it to say so: D46), run when the driver ends and when the network server
 *     ends, before anything of either is freed; then the claim's DMA memory is freed and the claim ends;
 *   - cache maintenance over its DMA memory (Emu68's ARM cache; a real 68040/68060's data cache, D37);
 *   - its blocks (DRV_ATTACH, DRV_XFER) carried to "net" as kernel messages that never start a server.
 *   - the parallel port (DESIGN D39): a claim of CIA-A's port B and CIA-B's bits 0-2, the ACK line's interrupt on
 *     while it is held, and the count of its edges (DRV_PARACK);
 *   - Paula's audio channels (DESIGN D40): a claim for the sound server (port "audio") alone, the bell silent while it
 *     is held, the channels stopped by the kernel itself at its end and before ZINE;
 *   - the xHCI USB controller (DESIGN D46): a claim for the USB server (service "usb") alone, which may do nothing
 *     else here: its claim, the claim's DMA memory, quiesce and cache calls, and the hardware table;
 *   - (DESIGN D60 6.2, with full memory protection only: in its span) Paula driven by the kernel for the sound server,
 *     checked (DRV_AUDIO: every channel's DMA inside the caller's own chip RAM), and the plipbox's transfers run by
 *     the kernel on the parallel port's lines (DRV_PARXFER): neither server holds a register page then;
 * Every other op is for a child of the running network server only (not merely root: no sudo session claims a
 * device); the claim's ops for its holder only. Cold code: -Os in its own file. */
#include "kernel.h"
#include "netproto.h"
#include "hwinfo.h"
#include "zine.h"
#include "hw.h"
#include "mmu.h"

_Static_assert(sizeof(struct udrvattach) == UDRVATTACH_FIRST, "udrvattach's first size is frozen");
_Static_assert(sizeof(struct udrvxfer) == UDRVXFER_FIRST, "udrvxfer's first size is frozen");
_Static_assert(sizeof(struct udrvaudio) == UDRVAUDIO_FIRST, "udrvaudio's first size is frozen");
_Static_assert(sizeof(struct udrvparx) == UDRVPARX_FIRST, "udrvparx is frozen");
_Static_assert(__builtin_offsetof(struct hwinfo, fb) == HWINFO_FIRST && HWFB_OFF == HWINFO_FIRST &&
               sizeof(struct hwinfo) == HWINFO_FIRST + 32, "the hardware table's first size is frozen; it grows at its end");

#define NCLAIM 4
#define DRV_OWNER(i) (0x80000100u + (u32)(i))  /* a DMA region's owner: its claim, never a pid (as SEG_OWNER) */
#define DMA_MAX (4u << 20)                      /* the most DMA memory a claim may have */
static struct claim {
    int pid;                    /* the holder; 0: free */
    int page;                   /* its register page was granted (an old sndd's Paula, D60 5.8) */
    u32 base, size;             /* the device's register window */
    u32 dma, dmalen;            /* the DMA region (0: none) */
    void *blk;                  /* the block it was cut from */
    u32 nq;
    struct udrvstep q[DRV_QSTEPS];
} claims[NCLAIM];

static int old_sndd(const struct proc *p);

static const struct hwinfo *hw(void)
{
    const struct hwinfo *h = HWINFO_K;
    return h->magic == HWINFO_MAGIC ? h : 0;
}

/* DESIGN D36: memory in the PC Card slot's addresses (0x600000-0xA4FFFF)? Zenix's pools, and any RAM board the boot
 * listed (one Zenix did not take still answers there); the first clash is said */
static int slot_clash(const struct hwinfo *h)
{
    const struct zine_range *r;
    u32 n = mem_ranges(&r), lo = 0, hi = 0;
    for (u32 i = 0; i < n + (h ? h->nboards : 0) && i < n + HW_MAXBOARDS; i++) {
        if (i < n) { lo = r[i].lo; hi = r[i].hi; }
        else {
            const struct hwboard *b = &h->board[i - n];
            if (!(b->type & 0x20)) continue;
            lo = b->addr; hi = b->addr + b->size;
        }
        if (lo < DRV_PCMCIA + DRV_PCMCIA_SIZE && DRV_PCMCIA_LOW < hi) {
            kprintf("drv: memory at %p-%p is in the PC Card slot's addresses: the slot is not claimed\n", lo, hi - 1);
            return 1;
        }
    }
    return 0;
}

/* the register window of the device at base: GENET's, a Zorro board's (never a RAM board), or the PC Card slot */
static long window(u32 base, u32 *size)
{
    const struct hwinfo *h = hw();
    if (base == DRV_PCMCIA) {
        if (!ide_gayle())
            return -ENODEV;
        if (slot_clash(h))
            return -EADDRINUSE;
        *size = DRV_PCMCIA_SIZE;
        return 0;
    }
    if (base == DRV_PAULA) {
        *size = 0x40;                   /* the four channels' registers: nothing else of a claim uses the window */
        return 0;
    }
    if (base == DRV_PARPORT) {
        *size = 1;                      /* not a window: its quiesce steps are checked one by one (par_step_ok) */
        return 0;
    }
    if (!h || !base)
        return -ENODEV;
    if (h->genet && base == h->genet) {
        *size = 0x10000;
        return 0;
    }
    if (base == h->xhci) {              /* D46: only the USB server gets here (drv_sys) */
        *size = h->xhci_size;
        return 0;
    }
    for (u32 i = 0; i < h->nboards && i < HW_MAXBOARDS; i++) {
        const struct hwboard *b = &h->board[i];
        if (b->addr == base && b->size && !(b->type & 0x20)) {
            const struct bootinfo *bi = BOOTINFO_K;
            for (int k = 0; k < bi->nregions && k < 8; k++)     /* (review 2, stage 4: never RAM the boot listed,
                                                                  * whatever a board's entry says) */
                if (base < bi->r[k].upper && bi->r[k].lower < base + b->size)
                    return -ENODEV;
            *size = b->size;
            return 0;
        }
    }
    return -ENODEV;
}

/* DESIGN D39: a quiesce step of the parallel port's claim writes one byte of the data port or its direction, or
 * changes bits 0-2 (BUSY, POUT, SEL) of CIA-B's port A or its direction and leaves bits 3-7 (the serial port's) */
static int par_step_ok(const struct udrvstep *s)
{
    if (s->size != 1)
        return 0;
    if (s->addr == DRV_PARPORT || s->addr == DRV_PAR_DDRB)
        return 1;
    if (s->addr == DRV_PAR_PRA || s->addr == DRV_PAR_DDRA)
        return s->op == DRVS_RMW && !(s->mask & ~7u) && !(s->value & ~s->mask);
    return 0;
}

/* the ACK line's interrupt (CIA-A's FLG): on while the parallel port is claimed. An ICR write changes only the bits
 * written as 1 (bit 7: set, else clear), so the timer's and the keyboard's stay as they are */
static void par_flag(int on)
{
    CIAA(CIA_ICR) = on ? 0x90 : 0x10;
}

/* what a claim's base needs besides its quiesce program, when it stops (held: 1 for ZINE, the claim kept until its
 * holder dies; 0 at the release): the ACK line's interrupt off (D39); Paula's channels stopped, the bell back (D40) */
static void stop_base(const struct claim *c, int held)
{
    if (c->base == DRV_PARPORT)
        par_flag(0);
    else if (c->base == DRV_PAULA)
        audio_hold(held);
}

static u32 rd(const struct udrvstep *s)
{
    return s->size == 1 ? *(volatile u8 *)s->addr : s->size == 2 ? *(volatile u16 *)s->addr : *(volatile u32 *)s->addr;
}

/* -> 1 when a wait step ran out (the device may still be writing) */
static int quiesce(const struct claim *c)
{
    int stuck = 0;
    for (u32 k = 0; k < c->nq; k++) {
        const struct udrvstep *s = &c->q[k];
        u32 v = s->value;
        if (s->op == DRVS_WAIT) {       /* D46: until (*addr & mask) == value, at most 20 ms by the E-clock (an xHCI's
                                         * halt after Run/Stop is cleared: 16 ms at most, xHCI 5.4.1) */
            u32 t = hr_time();
            while ((rd(s) & s->mask) != v)
                if (hr_time() - t > eclock_hz / 50) { stuck = 1; break; }
        } else if (s->size == 1) {
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
    return stuck;
}

/* the claim ends: the device stopped first; its DMA memory is given to the driver's process, freed with the rest of
 * it when its zombie is reaped (audit 2026-10-28 S2). Not at once: when a driver ends, netd learns it only at its next
 * wait for its children, and until then it may still copy a frame into the send buffers there; when netd ends, the
 * driver still runs until it sees its doorbell close and may still touch the region. */
/* D60 stage 3 (6.2): the pages of the claim's registers in its holder's tree (the custom chips' page for Paula, CIA-A's
 * and CIA-B's for the parallel port, Gayle's two card pages besides the slot's window), inhibited; in or out */
static int regs(const struct claim *c, struct proc *p, int in)
{
    u32 a = c->base, b = c->base + c->size;
    /* D60 stage 4: the parallel port's lines are the kernel's (DRV_PARXFER: CIA-A and CIA-B mix them with the floppy's,
     * the serial port's and the keyboard's), and so are Paula's (DRV_AUDIO), but for a sound server built before
     * D60 (its port not marked PEER_EXACT: 5.8, the old lend's rule), which writes the chips itself */
    if (c->base == DRV_PARPORT || (c->base == DRV_PAULA && !(in ? old_sndd(p) : c->page)))
        return 0;
    if (c->base == DRV_PAULA) {
        a = c->base & ~0xFFFu;
        b = a + 0x100;
    }
    /* the second and third pages: Gayle's card status and its configuration (0xDA8000, 0xDAB000, a page each with 4 KB
     * pages; the IDE's latch and the interrupt enable between them stay the kernel's) for the PC Card slot */
    u32 x = c->base == DRV_PCMCIA && MMU_PG == 4096 ? 0xDA8000u : 0;
    u32 y = c->base == DRV_PCMCIA && MMU_PG == 4096 ? 0xDAB000u : 0;
    if (!in) {
        mmu_ungrant(p, a);
        if (x)
            mmu_ungrant(p, x);
        if (y)
            mmu_ungrant(p, y);
        return 0;
    }
    if (mmu_grant(p, a, b, G_IO) || (x && mmu_grant(p, x, x + 0x100, G_IO)) || (y && mmu_grant(p, y, y + 0x100, G_IO))) {
        regs(c, p, 0);
        return -ENOMEM;
    }
    return 0;
}

static void release(struct claim *c, const char *why)
{
    int stuck = quiesce(c);
    stop_base(c, 0);
    if (c->base == DRV_PAULA)
        aud_valid = 0;                  /* (audio_hold stopped every channel) */
    struct proc *p = proc_by_pid(c->pid);
    if (p)
        regs(c, p, 0);                  /* (its DMA memory stays in its tree: it is the driver's until it is reaped) */
    if (stuck)                          /* D46: its DMA may still run: the memory stays the claim's, never freed */
        kprintf("drv: the device at %p did not say it stopped: its DMA memory is kept\n", c->base);
    else if (c->blk)
        mem_set_owner(c->blk, (u32)c->pid);
    kprintf("drv: claim of %p (pid %d) released (%s): %u quiesce steps run%s\n", c->base, c->pid, why, c->nq,
            c->blk ? ", its DMA memory freed when the driver is reaped" : "");
    memset(c, 0, sizeof *c);
}

/* p is dying (proc_exit, before anything of it is freed): its claims end; the network server's end ends every claim
 * its children hold (not the sound server's Paula, D40, nor the USB server's controller, D46) */
void drv_exit(struct proc *p)
{
    int srv = svc_pid("net") == p->pid;
    for (int i = 0; i < NCLAIM; i++) {
        struct proc *q = claims[i].pid ? proc_by_pid(claims[i].pid) : 0;
        if (claims[i].pid && (claims[i].pid == p->pid || (srv && q && q->ppid == p->pid)))
            release(&claims[i], claims[i].pid == p->pid ? "the driver ended" : "the network server ended");
    }
}

/* DESIGN D29 7.2 step 6: before ZINE hands the machine over, every claimed device is stopped by its quiesce program
 * and its driver killed: it is not scheduled before the resume, dies then (its claim released as above), and the
 * network server starts it again (D19's restart) */
void drv_zine(void)
{
    for (int i = 0; i < NCLAIM; i++)
        if (claims[i].pid) {
            quiesce(&claims[i]);
            stop_base(&claims[i], 1);
            struct proc *p = proc_by_pid(claims[i].pid);
            if (p)
                proc_kill(p);
        }
}

/* D60 7.4 (mmu.c's SYS_shm): current holds a claim whose DMA memory starts at a: 1 and its bytes */
int drv_dmamem(u32 a, u32 *size)
{
    for (int i = 0; i < NCLAIM; i++)
        if (claims[i].pid == current->pid && claims[i].dma == a && a) {
            *size = claims[i].dmalen;
            return 1;
        }
    return 0;
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
    u32 lo = a & ~63u, hi = (a + n + 63) & ~63u;    /* whole lines, inside the 64-byte aligned region */
    if (!h || !(h->flags & HW_DT)) {
        cache_push_range(lo, hi);       /* a real 68040/68060 with its data cache on (D37): cpushl; else nothing */
        return 0;
    }
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
    long r = ipc_kcall(port, &m, SVC_NS_SKIP, 0, KP_NET);
    return (m.kflags & UMSG_NETREPLY) ? r : -ECONNRESET;
}

/* ---- DRV_AUDIO (D60 6.2): Paula for the sound server, through the kernel (with full memory protection) ---- */

/* the kernel's view of each channel's pointer and length (in words, 0: 65,536), checked for the channels in aud_valid
 * (set by DRV_AUDIO, dropped at the claim's end and when a block under a channel is freed); and for a channel whose
 * DMA is on, every pair it had since it was turned on, as one span [aud_lo, aud_hi): Paula takes a channel's pair
 * only at its block's end, so it may still read an earlier one (review 2, stage 4) */
static u32 aud_lc[4], aud_len[4], aud_lo[4], aud_hi[4];
u8 aud_valid;

/* the sound server built before D60: its port "audio" is not marked PEER_EXACT; with full protection only it still
 * gets the custom chips' page at its claim (it writes Paula itself, as the old lend's rule allows: 5.8), but not with
 * MMU=STRICT */
static int old_sndd(const struct proc *p)
{
    return mmu_on && !mmu_strict && !port_exact_of(p->pid, "audio");     /* (MMU=STRICT: none, 5.8) */
}

static u32 aud_bytes(u32 len) { return (len ? len : 0x10000u) * 2; }

/* the channel's DMA inside the caller's own chip RAM: the memory it may write (no code, no window: 5.3), even */
KMMU static int aud_ok(u32 lc, u32 len)
{
    u32 n = aud_bytes(len);
    return !(lc & 1) && user_ok_wr(current, lc, n) && chip_ok(lc, n);
}

/* mem.c: the block [lo, hi) is freed or handed to another process (aud_valid set): a channel that may read from it
 * stops first, and its pointer is no longer taken as checked (turning it on again needs a new one) */
void drv_freed(u32 lo, u32 hi)
{
    u16 m = 0;
    for (int c = 0; c < 4; c++)
        if ((aud_valid & (1u << c)) && aud_lo[c] < hi && lo < aud_hi[c])
            m |= (u16)(1u << c);
    if (!m)
        return;
    for (int c = 0; c < 4; c++)
        if (m & (1u << c))
            REG16(0xDFF0A8 + 16 * c) = 0;       /* its volume, then its DMA off */
    DMACON = m;
    aud_valid &= (u8)~m;
}

/* the span a channel may read from: its pair now, and while its DMA stays on every pair since */
KMMU static void aud_span(u32 c, int keep)
{
    u32 lo = aud_lc[c], hi = lo + aud_bytes(aud_len[c]);
    if (!keep || lo < aud_lo[c])
        aud_lo[c] = lo;
    if (!keep || hi > aud_hi[c])
        aud_hi[c] = hi;
}

KMMU static long audio(struct claim *c, u32 a)
{
    struct udrvaudio q;
    if (!c || c->base != DRV_PAULA)
        return -EPERM;
    if (!user_ok_rd(current, a, UDRVAUDIO_FIRST) || (a & 1))
        return -EFAULT;
    memcpy(&q, (const void *)a, sizeof q);
    if (q.n > DRV_AUDIO_MAX || (q.wait & ~0x0780u) || q.limit > eclock_hz / 20)
        return -EINVAL;
    /* the batch checked whole, in its order, before anything is written: every state a channel whose DMA is on
     * passes through must be checked (a channel's AUDxLCH, AUDxLCL and the AUDxLEN right after them are one step,
     * written with interrupts masked; Paula could still take the pair in the few bus cycles between those writes) */
    u32 lc[4], len[4], ok = aud_valid, unit[DRV_AUDIO_MAX];
    memcpy(lc, aud_lc, sizeof lc);
    memcpy(len, aud_len, sizeof len);
    for (u32 k = 0; k < q.n; k++) {
        u32 r = q.w[k].reg, v = q.w[k].val, ch = (r - 0xA0) >> 4;
        unit[k] = 1;
        if (r >= 0xA0 && r < 0xE0 && (r & 15) <= 4) {
            u32 o = r & 15;
            if (o == 2)
                return -EINVAL;         /* an LCL alone */
            if (o == 0) {               /* AUDxLCH, then its AUDxLCL: one pointer (and its LEN, when it follows) */
                if (k + 1 >= q.n || q.w[k + 1].reg != r + 2)
                    return -EINVAL;
                lc[ch] = v << 16 | q.w[k + 1].val;
                unit[k] = 2;
                if (k + 2 < q.n && q.w[k + 2].reg == r + 4) {
                    len[ch] = q.w[k + 2].val;
                    unit[k] = 3;
                }
            } else
                len[ch] = v;
            if (!aud_ok(lc[ch], len[ch]))
                return -EFAULT;         /* every pair a channel is given, as each step leaves it, is the caller's */
            ok |= 1u << ch;
            k += unit[k] - 1;
        } else if (r >= 0xA0 && r < 0xE0) {
            if ((r & 15) > 10)
                return -EINVAL;         /* (PER, VOL, DAT; the two words after DAT are nothing) */
        } else if (r == 0x096) {        /* DMACON: SET and the four channels, never DMAEN or another device's bit */
            if (v & 0x7FF0)
                return -EINVAL;
            if ((v & 0x8000) && (v & 15 & ~ok))
                return -EFAULT;         /* turned on with a pointer the kernel has not checked */
        } else if (r == 0x09A || r == 0x09C) {      /* INTENA, INTREQ: SET and the channels' bits, never INTEN */
            if (v & 0x787F)
                return -EINVAL;
        } else if (r == 0x09E) {        /* ADKCON: the audio half (0-7) and SET */
            if (v & 0x7F00)
                return -EINVAL;
        } else
            return -EINVAL;
    }
    for (u32 k = 0; k < q.n; k += unit[k]) {
        u32 r = q.w[k].reg, ch = (r - 0xA0) >> 4;
        if (unit[k] > 1) {
            u16 s = irq_save();
            *(volatile u32 *)(0xDFF000u + r) = (u32)q.w[k].val << 16 | q.w[k + 1].val;
            if (unit[k] == 3)
                REG16(0xDFF000u + r + 4) = q.w[k + 2].val;
            irq_restore(s);
        } else
            REG16(0xDFF000u + r) = q.w[k].val;
        if (r >= 0xA0 && r < 0xE0 && ((r & 15) == 0 || (r & 15) == 4)) {
            if (unit[k] > 1)
                aud_lc[ch] = (u32)q.w[k].val << 16 | q.w[k + 1].val;
            if (unit[k] == 3 || (r & 15) == 4)
                aud_len[ch] = q.w[k + unit[k] - 1].val;
            aud_span(ch, (DMACONR >> ch) & 1);      /* (while it plays, the pairs before stay in its span) */
        } else if (r == 0x096 && (q.w[k].val & 0x8000))
            for (u32 c2 = 0; c2 < 4; c2++)
                if (q.w[k].val & (1u << c2))
                    aud_span(c2, 0);    /* turned on: from its pair now */
    }
    aud_valid = (u8)ok;                 /* the channels whose pair as the batch left it is the caller's */
    u32 t0 = hr_time();
    if (q.wait)                         /* Paula has taken a channel's pointer (its request bit): a bounded wait */
        while ((INTREQR & q.wait) != q.wait && hr_time() - t0 < q.limit)
            ;
    u32 ir = INTREQR & 0x0780u, dm = DMACONR & 15u, vp = VPOSR & 0x7FFFu;
    if (q.wait & ir)
        INTREQ = (u16)(q.wait & ir);
    return (long)(vp << 16 | ir | dm);
}

/* ---- DRV_PARXFER (D60 6.2): the plipbox's line run here, on the claim's lines only ---- */

static struct claim *parx_c;            /* the claim a transfer runs for (it may yield inside a frame) */
static inline __attribute__((always_inline)) unsigned char pb_rd(u32 a) { return *(volatile u8 *)a; }
static inline __attribute__((always_inline)) void pb_wr(u32 a, unsigned char v) { *(volatile u8 *)a = v; }
/* one instruction each: an interrupt cannot fall between the read and the write of CIA-B's port A (bits 3-7 are the
 * serial port's) */
static inline __attribute__((always_inline)) void pb_bset(u32 a, int bit) { __asm__ volatile("bset %1,(%0)" :: "a"(a), "d"(bit) : "memory", "cc"); }
static inline __attribute__((always_inline)) void pb_bclr(u32 a, int bit) { __asm__ volatile("bclr %1,(%0)" :: "a"(a), "d"(bit) : "memory", "cc"); }
static inline __attribute__((always_inline)) void pb_clrbits(u32 a, unsigned m) { __asm__ volatile("and.b %1,(%0)" :: "a"(a), "d"(~m) : "memory", "cc"); }
static inline __attribute__((always_inline)) u32 pb_ticks(void) { return ticks; }
static inline __attribute__((always_inline)) long pb_acks(void) { return (long)(par_acks & 0x7FFFFFFFu); }
/* the yield point inside a handshaked frame (every 128 bytes): the kernel is not preempted, a driver in user mode was */
KMMU static int pb_yield(void)
{
    if (kwork_pending)
        kwork();
    if (need_resched)
        resched();
    return current->killed || !parx_c || parx_c->pid != current->pid;
}
#define PB_FN KMMU
#include "../net/drv/plipbox.h"
#include "../net/drv/plipbox-line.c"

KMMU static long parxfer(struct claim *c, u32 a)
{
    struct udrvparx x;
    if (!c || c->base != DRV_PARPORT)
        return -EPERM;
    if (!user_ok_wr(current, a, UDRVPARX_FIRST) || (a & 1))
        return -EFAULT;
    memcpy(&x, (const void *)a, sizeof x);
    u32 need = x.op == PARX_SEND ? x.len + 3 : x.op == PARX_RECV ? x.len : 0;
    if ((x.op == PARX_SEND && (x.len < PB_HDR || x.len > PB_MAXFRAME)) || (x.op == PARX_RECV && x.len > PB_BUF) ||
        x.deadline > PB_WAIT || x.op - PARX_SEND > PARX_OFF - PARX_SEND || (x.flags & ~PARXF_BURST))
        return -EINVAL;
    /* the driver's own memory or a region it took (netd's frame pool, SYS_shm): user_ok_call_wr (a window's copy page
     * is given back to its client for the call, 5.7) */
    if (need && !user_ok_call_wr(current, x.buf, need))
        return -EFAULT;
    if (parx_c)
        return -EBUSY;                  /* an earlier driver's transfer, yielded, has not seen its claim go yet */
    parx_c = c;
    long r = pb_xfer(&x, (unsigned char *)x.buf);
    parx_c = 0;
    memcpy((void *)a, &x, sizeof x);
    return r;
}

/* D60 6.2 (D45 13: in the memory protection span, through mmu_ops; without protection -EOPNOTSUPP: sndd writes Paula
 * and the plipbox driver runs the line itself, as before D60, so a machine without an MMU pays no byte or call for
 * them) */
KMMU long drv_span_(u32 op, void *c, u32 a)
{
    return op == DRV_AUDIO ? audio(c, a) : parxfer(c, a);
}

/* SYS_drv (op, a, b) */
long drv_sys(u32 opw, u32 a, u32 b)
{
    u32 op = opw & 0xFF;
    const struct hwinfo *h = hw();
    int usb = svc_pid("usb") == current->pid;
    int dpy = op == DRV_HWINFO && svc_is("display");  /* D60 stage 3: the display server reads the whole table (D43's
                                         * fb after the drivers' part) through the kernel, not at 0x1100 */
    if (dpy) {
    } else if (usb) {                   /* D46: the USB server: its claim (the xHCI's, below) and that claim's ops only */
        if (op - 1 > DRV_CACHE - 1)
            return -EPERM;
    } else if ((op == DRV_CLAIM && a == DRV_PAULA) || op == DRV_AUDIO) {
        if (svc_pid("audio") != current->pid)
            return -EPERM;              /* D40: only the sound server the service manager runs (DRV_AUDIO: its claim) */
    } else {
        int srv = svc_pid("net");
        if (!srv)
            return -ECONNRESET;         /* no network server: an orphaned driver learns it is gone */
        if (current->ppid != srv)
            return -EPERM;              /* only the running network server's children */
    }
    switch (op) {
    case DRV_HWINFO: {
        u32 n = b ? b : HWINFO_FIRST;
        if (!h)
            return -ENODEV;
        if (n < HWINFO_FIRST)
            return -EINVAL;
        if (n > (dpy ? sizeof *h : HWINFO_FIRST))
            n = dpy ? sizeof *h : HWINFO_FIRST;     /* the drivers' part (D43's fb after it is the display server's) */
        if (!user_ok_wr(current, a, n))
            return -EFAULT;
        memcpy((void *)a, h, n);
        return (long)n;
    }
    case DRV_CLAIM: {
        if (usb != (h && a && h->xhci == a))
            return -EPERM;              /* D46: the xHCI is the USB server's, and it claims nothing else */
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
        claims[f].base = a;
        claims[f].size = size;
        if (regs(&claims[f], current, 1))
            return -ENOMEM;
        claims[f].page = a == DRV_PAULA && old_sndd(current);   /* (its release takes out what was granted) */
        claims[f].pid = current->pid;
        mmu_sysmark(current);           /* (MMU=D41, D60 12: D41 7's system process) */
        if (a == DRV_PAULA)
            audio_hold(1);              /* D40: the bell stopped and silent; the log line says the base (0xdff0a0) */
        if (a == DRV_PARPORT) {
            par_flag(1);
            kprintf("drv: pid %d (%s) claims the parallel port\n", current->pid, current->name);
        } else
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
        void *blk = mem_alloc(n + 64, (h && (h->flags & HW_DT) ? MEM_FASTONLY : 0) | MEM_USER, DRV_OWNER(c - claims));
                                        /* D46: the Pi's DMA reaches Emu68's RAM only, never chip RAM; D60: whole pages
                                         * with protection on, valid in the driver's tree */
        if (!blk)
            return -ENOMEM;
        if (mmu_grant(current, (u32)blk, (u32)blk + n + 64, 0)) {
            mem_free(blk);
            return -ENOMEM;
        }
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
        int waits = 0;
        for (u32 k = 0; k < q.n; k++) {
            const struct udrvstep *s = &q.step[k];
            if (s->op == DRVS_WAIT && waits++)
                return -EINVAL;         /* D46: one wait a program (20 ms at most with interrupts off) */
            if ((s->op != DRVS_WRITE && s->op != DRVS_RMW && (s->op != DRVS_WAIT || c->base == DRV_PARPORT)) ||
                (s->size != 1 && s->size != 2 && s->size != 4) ||
                (s->addr & (s->size - 1)))
                return -EINVAL;
            if (c->base == DRV_PARPORT ? !par_step_ok(s) : s->addr < c->base || s->addr - c->base > c->size - s->size)
                return -EINVAL;         /* a bus error here would be a kernel crash: only the claimed window */
        }
        memcpy(c->q, q.step, sizeof c->q);
        c->nq = q.n;
        return 0;
    }
    case DRV_CACHE:
        return cache(opw >> 8, a, b);
    case DRV_PARACK: {
        struct claim *c = mine(a);
        if (!c || c->base != DRV_PARPORT)
            return -EPERM;
        return (long)(par_acks & 0x7FFFFFFFu);
    }
    case DRV_AUDIO:
    case DRV_PARXFER:
        return mmu_ops->drv(op, mine(a), b);
    case DRV_ATTACH:
        return forward(NETDRV_ATTACH, a, b, UDRVATTACH_FIRST);
    case DRV_XFER:
        return forward(NETDRV_XFER, a, b, UDRVXFER_FIRST);
    }
    return -EINVAL;
}
