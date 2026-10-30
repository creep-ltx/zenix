/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's kernel part (DESIGN D29, docs/d29-zine.md sections 3, 7.2, 9.2, 9.6; step 2): SYS_zine. The kernel keeps
 * only what needs Zenix's own state at hand: who may, the disks synced, the network's drivers quiesced, the notes
 * for a cold start, the hand-over itself (the CPU's state saved, the blob called in supervisor mode with interrupts
 * masked) and the resume (the CPU's state back, the devices restarted, DMA and interrupts on last). Everything
 * AmigaOS-shaped (the memory lists, saving chip RAM, the small exec, the loader, the chips' register set on the way
 * back, the CIAs' timers) is the blob's (/lib/zine/zine.blob), which costs nothing until `zine` runs.
 *
 * Nothing of Zenix runs while the program runs: the session is this one system call with interrupts masked in the
 * kernel (the program's interrupts go through ZINE's vectors at VBR 0), so every process is paused where it stood
 * (owner answer 2) and the tick does not count the session (9.7: Zenix's timers do not fire in a burst after it).
 * Cold code, -Os in its own file. */
#include "kernel.h"
#include "hw.h"
#include "zine.h"

#define ADKCONR REG16(0xDFF010)

_Static_assert(sizeof(struct uzine_info) == UZINE_INFO_FIRST, "uzine_info's first size is frozen");
_Static_assert(sizeof(struct uzine_go) == UZINE_GO_FIRST, "uzine_go's first size is frozen");

long zine_enter(u32 entry, struct zine_args *a);       /* below: saves the CPU's state, calls the blob */
void zine_resume(void);
u32 zine_ksp, zine_secs;
static int busy;

/* zine_enter(entry, args): d2-d7/a2-a6, USP, VBR and CACR onto the kernel stack, its pointer in zine_ksp, then the
 * blob's entry with args. The blob comes back through zine_resume (d0 the answer, d1 the seconds; supervisor mode,
 * interrupts masked, any stack), or by its own rts when it refused before taking anything (then d1 is 0). The
 * resume puts CACR back and flushes both caches (Zenix's code is where a program's may have been cached, and Emu68
 * would run stale translations), then VBR and USP, and returns from zine_enter with interrupts still masked. */
__asm__(
    "    .text\n"
    "    .globl zine_enter\n"
    "zine_enter:\n"
    "    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "    move.l  %usp,%a0\n"
    "    move.l  %a0,-(%sp)\n"
    "    movec   %vbr,%a0\n"
    "    move.l  %a0,-(%sp)\n"
    "    movec   %cacr,%d0\n"
    "    move.l  %d0,-(%sp)\n"
    "    move.l  %sp,zine_ksp\n"
    "    move.l  60(%sp),%a0\n"            /* entry: 11 registers + 3 longs + the return address = 60 */
    "    move.l  64(%sp),-(%sp)\n"
    "    moveq   #0,%d1\n"
    "    jsr     (%a0)\n"
    "    .globl zine_resume\n"
    "zine_resume:\n"
    "    move.w  #0x2700,%sr\n"
    "    move.l  zine_ksp,%sp\n"
    "    move.l  %d1,zine_secs\n"
    "    move.l  %d0,%a2\n"
    "    move.l  (%sp)+,%d1\n"
    "    movec   %d1,%cacr\n"
    "    jsr     cache_flush_code\n"
    "    move.l  (%sp)+,%a0\n"
    "    movec   %a0,%vbr\n"
    "    move.l  (%sp)+,%a0\n"
    "    move.l  %a0,%usp\n"
    "    move.l  %a2,%d0\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "    rts\n");

/* 7.2 step 1 (owner answer 1 and question 1): root, at the Amiga's own console: descriptors 0-2 are console ttys
 * with the caller in that console's foreground. An ssh, telnet or serial login has none; keys typed through
 * wasabid's CON_INPUT are the console's. (The group `zine` needs the group claim: the main session's, 13.) */
static int allowed(void)
{
    if (current->uid != 0)
        return 0;
    for (int i = 0; i < 3; i++) {
        struct file *f = current->fd[i];
        if (!f || f->type != FT_TTY || f->off < 1 || f->off > NCONS || !tty_in_fg((int)f->off, current))
            return 0;
    }
    return 1;
}

static long go(const struct uzine_go *g)
{
    if (!allowed())
        return -EPERM;
    if (busy || upd_pa || (upd_flags & UPD_F_TRIAL))
        return -EBUSY;                  /* a second session, or an update not yet confirmed (D15, D22 4.3) */
    if (!user_ok_wr(current, g->blob, g->blob_size) || !user_ok_wr(current, g->vol, g->vol_size) ||
        !user_ok_wr(current, g->blk, g->blk_size) || g->blob_size < ZINB_OFF + 12 || (g->blob & 3) || (g->blk & 3))
        return -EFAULT;
    const u32 *h = (const u32 *)(g->blob + ZINB_OFF);
    if (h[0] != ZINB_MAGIC || h[1] != ZINE_LAYOUT)
        return -ENOEXEC;
    u32 c, f;
    mem_totals(&c, &f);
    if (!f)
        return -ENODEV;                 /* a chip-only machine: the saved chip RAM has nowhere to go */
    busy = 1;
    /* 7.2 step 5: every mount's server writes out what it holds (local first, network last within 2 s). Then nothing
     * runs again before the hand-over: this call does not sleep from here on */
    if (vfs_sync(VS_BOUNDED) < 0) {
        busy = 0;
        return -EIO;
    }
    drv_zine();                         /* 7.2 step 6: the network's devices stopped, their drivers die after */
    kprintf("zine: filesystems synced; handing the machine to an Amiga program\n");
    klog_set_reason(KLOG_END_ZINE);     /* 7.2 step 7: a cold start during the session says why (9.4) */
    bootnote_set(BOOTNOTE_ZENIX);       /* and starts Zenix where the boot menu runs */
    fd_zine();                          /* 7.2 step 8: the floppy's motors off (audio: the blob turns all DMA off) */
    serial_flush();
    struct zine_args a;
    memset(&a, 0, sizeof a);
    a.layout = ZINE_LAYOUT;
    a.resume = (u32)zine_resume;
    a.vol = g->vol, a.vol_size = g->vol_size, a.blk = g->blk, a.blk_size = g->blk_size;
    a.nranges = mem_ranges(&a.ranges);
    const struct bootinfo *bi = (const struct bootinfo *)0x1000;
    a.kernel_pa = bi->kernel_pa;
    a.kernel_mem = *(u32 *)(bi->kernel_pa + 12);
    a.klog_pa = klog_area_base();
    a.klog_size = a.klog_pa ? KLOG_AREA_SIZE : 0;
    a.ram_pa = boot_ram_pa, a.ram_size = boot_ram_size;
    a.cop = gfx_zine(&a.diwstrt);
    a.serper = 30;                      /* serial_init's 115200 baud */
    a.potgo = 0xFF00;                   /* input_init's */
    a.eclock_hz = eclock_hz;
    u16 s = irq_save();
    a.adkcon = ADKCONR;
    u16 dma = DMACONR, ena = INTENAR;
    a.hr_at = hr_time();
    long r = zine_enter(g->blob, &a);
    /* 9.6: the blob put the chips' registers and the CIAs back (the E-clock counter where it stood); the kernel's
     * devices now, then DMA and the interrupts Zenix had, last */
    INTREQ = 0x7FFF;
    kbd_zine();                         /* held keys forgotten, one handshake */
    fd_zine();                          /* the head position unknown, the track cache dropped */
    INTENA = 0x7FFF;
    INTENA = (u16)(INTF_SETCLR | (ena & 0x7FFF));
    DMACON = 0x07FF;
    DMACON = (u16)(DMAF_SETCLR | (dma & 0x07FF));
    wclock.base += zine_secs;           /* 9.7: the wall clock moved on by the session, the tick did not count it */
    klog_set_reason(KLOG_END_RUNNING);
    bootnote_set(0);                    /* no note: a later reset boots as usual */
    irq_restore(s);
    kprintf("zine: back after %u s (%d)\n", zine_secs, r);
    busy = 0;
    return r;
}

long zine_sys(u32 op, u32 a, u32 b)
{
    if (op == ZINE_INFO) {
        struct uzine_info i;
        u32 n = b < sizeof i ? b : sizeof i;
        if (n < UZINE_INFO_FIRST)
            return -EINVAL;
        if (!user_ok_wr(current, a, n))
            return -EFAULT;
        i.size = n;
        i.layout = ZINE_LAYOUT;
        i.attn = ((const struct bootinfo *)0x1000)->attn;
        mem_totals(&i.chip_total, &i.fast_total);
        mem_stats(&i.chip_free, &i.fast_free);
        memcpy((void *)a, &i, n);
        return 0;
    }
    if (op != ZINE_GO)
        return -EINVAL;
    struct uzine_go g;
    if (!user_ok_rd(current, a, UZINE_GO_FIRST))
        return -EFAULT;
    memcpy(&g, (const void *)a, sizeof g);
    if (g.size < UZINE_GO_FIRST || g.flags)
        return -EINVAL;
    return go(&g);
}
