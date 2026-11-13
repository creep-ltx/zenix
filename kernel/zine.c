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
#include "hwinfo.h"
#include "mmu.h"

#define ADKCONR REG16(0xDFF010)

_Static_assert(sizeof(struct uzine_info) == UZINE_INFO_MACHINE, "uzine_info: its first size is frozen, then machine");
_Static_assert(sizeof(struct uzine_go) == UZINE_GO_FIRST, "uzine_go's first size is frozen");

long zine_enter(u32 entry, struct zine_args *a);       /* below: saves the CPU's state, calls the blob */
void zine_resume(void);
u32 zine_ksp, zine_secs;
static int busy;

/* zine_enter(entry, args): d2-d7/a2-a6, USP, VBR and CACR onto the kernel stack, its pointer in zine_ksp, then the
 * blob's entry with args. The blob comes back through zine_resume (d0 the answer, d1 the seconds; supervisor mode,
 * interrupts masked, any stack), or by its own rts when it refused before taking anything (then d1 is 0). The
 * blob is flushed in first (it is code the caller just wrote: CACR's CI on 020/030; on 040/060 the data cache off and
 * cpusha bc, so the program runs with the data cache off as 8.1 says and none of Zenix's lines stay valid behind it;
 * DESIGN D37 section 6). The
 * resume puts VBR back first, then CACR with both caches cleared (Zenix's code and data are where a program's may
 * have been cached, the 030's data cache may hold lines from before the session that the program's writes with the
 * cache off did not update, and Emu68 would run stale translations: CI and CD on 020/030, cpusha bc on 040/060),
 * then USP, and returns from zine_enter with interrupts still masked. The 040/060 MMU registers and the 060's PCR
 * are the blob's (zb_mmu_save, zb_mmu_back). */
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
    "    move.w  0x1000,%d1\n"           /* D37 6: the blob is code /bin/zine just wrote; the program runs with */
    "    and.w   #0x0088,%d1\n"          /* the data cache off (8.1) */
    "    bne.s   3f\n"
    "    or.w    #0x0008,%d0\n"          /* 020/030: CI, the enable bits kept */
    "    movec   %d0,%cacr\n"
    "    bra.s   4f\n"
    "3:  and.l   #0x7FFFFFFF,%d0\n"      /* 040 DE, 060 EDC off, then at once everything pushed and invalid */
    "    movec   %d0,%cacr\n"
    "    .word   0xF4F8\n"               /* cpusha bc (never cinva: CLAUDE.md) */
    "4:  move.l  %sp,zine_ksp\n"
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
    "    move.l  (%sp)+,%a0\n"
    "    movec   %a0,%vbr\n"             /* Zenix's vectors before anything else (review 2026-10-31) */
    "    move.w  0x1000,%d0\n"           /* bootinfo's AttnFlags: 68040/68060? */
    "    and.w   #0x0088,%d0\n"
    "    bne.s   1f\n"
    "    or.w    #0x0808,%d1\n"          /* 020/030: both caches cleared (CI, CD) as CACR is put back */
    "    movec   %d1,%cacr\n"
    "    bra.s   2f\n"
    "1:  movec   %d1,%cacr\n"
    "    .word   0xF4F8\n"               /* cpusha bc (never cinva: CLAUDE.md) */
    "2:  move.l  (%sp)+,%a0\n"
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

/* the battery clock's minutes and seconds as seconds (0-3599), read until two readings agree (no rollover between);
 * the same nibbles on the MSM6242 and the RP5C01 (rtc.c) */
static u32 rtc_ms(void)
{
    u32 a = ~0u, b = 0;
    for (int k = 0; k < 4 && a != b; k++) {
        b = a;
        volatile const u8 *r = (volatile const u8 *)0xDC0003;
        a = ((r[12] & 7) * 10 + (r[8] & 15)) * 60 + (r[4] & 7) * 10 + (r[0] & 15);
    }
    return a % 3600;
}

static long go(const struct uzine_go *g)
{
    if (!allowed())
        return -EPERM;
    if (busy || upd_pa || (upd_flags & UPD_F_TRIAL) || gcon_grabbed())
        return -EBUSY;                  /* a second session, an update not yet confirmed (D15, D22 4.3), or a console
                                         * in graphics mode (D38: its server has the Copper) */
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
    /* 7.2 step 5: every mount's server writes out what it holds (local first, network last within 2 s), with every
     * other process's filesystem requests held from here to the resume (vfs_hold), so each server is idle with its
     * writes out at the hand-over; any local mount that fails refuses it. Nothing sleeps after the sync */
    vfs_hold = current;
    long e = vfs_sync(VS_BOUNDED | VS_LOCALS);
    e = current->killed ? -EINTR : e < 0 ? -EIO : 0;     /* Ctrl-C during the sync: given up, nothing taken */
    if (e) {
        vfs_hold = 0;
        busy = 0;
        return e;
    }
    u32 rt0 = rtc_ms();
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
    mmu_hand(0);                        /* D41 11: the program expects no translation; the blob saves what it finds */
    long r = zine_enter(g->blob, &a);
    mmu_hand(1);
    /* 9.6: the blob put the chips' registers and the CIAs back (the E-clock counter where it stood); the kernel's
     * devices now, then DMA and the interrupts Zenix had, last */
    INTREQ = 0x7FFF;
    kbd_zine();                         /* held keys forgotten, one handshake */
    fd_zine();                          /* the head position unknown, the track cache dropped, the disks changed */
    ide_zine();                         /* Gayle's IDE interrupt enable and latch */
    input_init();                       /* the mouse counters read afresh: the program's moves are no jump */
    if (rtc_ok) {                       /* 9.7: the battery clock's minutes and seconds correct the CIA-A count */
        s32 d = (s32)((rtc_ms() + 3600 - rt0) % 3600) - (s32)(zine_secs % 3600);
        d = d > 1800 ? d - 3600 : d < -1800 ? d + 3600 : d;
        if ((s32)zine_secs + d >= 0)    /* a program that set or stopped the clock: the CIA-A count stands */
            zine_secs += (u32)d;
    }
    INTENA = 0x7FFF;
    INTENA = (u16)(INTF_SETCLR | (ena & 0x7FFF));
    DMACON = 0x07FF;
    DMACON = (u16)(DMAF_SETCLR | (dma & 0x07FF));
    wclock.base += zine_secs;           /* 9.7: the wall clock moved on by the session, the tick did not count it */
    klog_set_reason(KLOG_END_RUNNING);
    bootnote_set(0);                    /* no note: a later reset boots as usual */
    irq_restore(s);
    kprintf("zine: back after %u s (%d)\n", zine_secs, r);
    vfs_hold = 0;
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
        const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
        i.machine = hw->magic == HWINFO_MAGIC && (hw->flags & HW_DT) ? UZI_EMU68 : 0;
        memcpy((void *)a, &i, n);
        return 0;
    }
    if (op != ZINE_GO)
        return -EINVAL;
    if (current->dbgf & DBGF_TRACED)
        return -EBUSY;                  /* D54 section 4: no debugger across a ZINE session */
    struct uzine_go g;
    if (!user_ok_rd(current, a, UZINE_GO_FIRST))
        return -EFAULT;
    memcpy(&g, (const void *)a, sizeof g);
    if (g.size < UZINE_GO_FIRST || g.flags)
        return -EINVAL;
    return go(&g);
}
