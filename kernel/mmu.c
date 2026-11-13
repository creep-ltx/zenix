/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D41 (docs/d41-memory-protection.md): optional memory protection with the MMU of a 68030, 68040 or 68060.
 * The MMU only protects, never translates: one tree for everybody, every page mapped to itself, a page valid only
 * while it belongs to the protected process flipped in (the tenant) or to a client it serves. The kernel never uses
 * the tree: the transparent translation registers match its supervisor accesses (and every access of a system
 * process), as D37 set them up. Off, the gates in mmu.h call nothing and mem.c lays blocks out as before.
 * DESIGN D45 section 13: the runtime code (KMMU, the asm) lies in the span .zhwmmu, reached only through the table
 * mmu_ops points at, and is given back after the boot when protection is off.
 * Cold code at -Os; the flip loop and the switch at -O2 (they run at a task switch). */
#pragma GCC optimize("Os")
#include "kernel.h"
#include "hw.h"
#include "hwinfo.h"
#include "cache_plan.h"
#include "zine.h"
#include "mmu.h"

/* the registers as one image (offsets used by the assembly below): 030: dtt0 = TT0, dtt1 = TT1, crp = CRP's two longs
 * (crp_hi, root); 040/060: root goes to URP and SRP */
struct mregs { u32 fam, tc, dtt0, dtt1, itt0, itt1, crp_hi, root; };

u32 mmu_regs_load(const struct mregs *m);       /* below: 0, or -1 if a register trapped (nothing is then sure) */
u32 mmu_regs_save(struct mregs *m);             /* the registers as they are; -1 if one is missing */
void mmu_ttr(const u32 *v, u32 fam);            /* the windows: 040/060 DTT0, DTT1, ITT0 = v[0..2]; 030 TT0 = v[0] */
void mmu_flush(u32 fam);                        /* PFLUSHA */
u32 mmu_probe_hw(const struct mregs *m, u32 t, u32 r);

u8 mmu_on;
static u8 fam;                  /* 1: 68030; 2: 68040/68060 */
static u8 mode;                 /* the windows now: 0 system (every access), 1 protected (supervisor only) */
static u8 dpush;                /* the table writes must be pushed (a 040/060 with the data cache on) */
static u32 *root;
static void *tblk;              /* the tables' block */
static u32 tbytes;
static struct mregs on, off;    /* translation on (the windows in system mode), and as it was before (D37's) */
static u32 win[2][3];           /* the windows' values: [mode][DTT0/TT0, DTT1, ITT0] */
static u32 sysmask;             /* bit n: procs[n] is a system process */
static struct proc *tenant;     /* the protected process whose view is flipped in (0: none) */
static u32 lent;                /* bit n: procs[n]'s view is lent to the tenant (it holds that client's request) */

#define SLOT(p) (1u << ((p) - procs))
#define D_TABLE 0x0Au           /* a table descriptor: U, resident (DT/UDT 2; the same bits on 030 and 040/060) */
#define D_PAGE  0x18u           /* a page descriptor's fixed bits: U and M set (the table search never writes) */
#define D_VALID 1u              /* bit 0: resident (030 DT 1, 040/060 PDT 01) */
#define D_WP    4u              /* bit 2: write-protected (030 WP, 040/060 W) */
#define CM_INH  0x40u           /* 040/060 CM 10: inhibited, serialized / precise */
#define CM_CB   0x20u           /* 040/060 CM 01: copyback; 00 write-through */

/* ---- the CPU's registers (supervisor, interrupts masked); 030 PMOVE and 040/060 MOVEC by their encodings. In the span
 * (D45 section 13), local to this file: nothing outside mmu.c can name them ---- */
__asm__(
    "    .pushsection .zhwmmu.text,\"ax\",@progbits\n"
    /* u32 mmu_regs_load(const struct mregs *m): translation off first (E clear), then the windows, the root
     * pointers, an ATC flush, and last TC/TCR as m says. A trap (a missing register): -1, the vectors back. */
    "mmu_regs_load:\n"
    "    move.l  4(%sp),%a1\n"
    "    movem.l %d2/%a2-%a3,-(%sp)\n"
    "    lea     9f(%pc),%a0\n"
    "    bsr     mt_on\n"
    "    cmp.l   #1,(%a1)\n"
    "    beq.s   3f\n"
    "    move.l  4(%a1),%d0\n"
    "    and.l   #0xFFFF7FFF,%d0\n"
    "    .long   0x4E7B0003\n"           /* movec d0,tc: translation off while the rest is written */
    "    move.l  8(%a1),%d0\n"
    "    .long   0x4E7B0006\n"           /* dtt0 */
    "    move.l  12(%a1),%d0\n"
    "    .long   0x4E7B0007\n"           /* dtt1 */
    "    move.l  16(%a1),%d0\n"
    "    .long   0x4E7B0004\n"           /* itt0 */
    "    move.l  20(%a1),%d0\n"
    "    .long   0x4E7B0005\n"           /* itt1 */
    "    move.l  28(%a1),%d0\n"
    "    .long   0x4E7B0806\n"           /* urp */
    "    .long   0x4E7B0807\n"           /* srp */
    "    nop\n"
    "    .word   0xF518\n"               /* pflusha */
    "    move.l  4(%a1),%d0\n"
    "    .long   0x4E7B0003\n"           /* tc */
    "    nop\n"
    "    bra.s   8f\n"
    "3:  clr.l   -(%sp)\n"               /* 030: a zero long for TC off */
    "    move.l  %sp,%a0\n"
    "    .long   0xF0104000\n"           /* pmove (a0),tc: off */
    "    addq.l  #4,%sp\n"
    "    lea     8(%a1),%a0\n"
    "    .long   0xF0100900\n"           /* pmovefd (a0),tt0 */
    "    lea     12(%a1),%a0\n"
    "    .long   0xF0100D00\n"           /* pmovefd (a0),tt1 */
    "    lea     24(%a1),%a0\n"
    "    .long   0xF0104C00\n"           /* pmove (a0),crp */
    "    .long   0xF0002400\n"           /* pflusha */
    "    lea     4(%a1),%a0\n"
    "    .long   0xF0104000\n"           /* pmove (a0),tc */
    "8:  moveq   #0,%d2\n"
    "    bra.s   7f\n"
    "9:  moveq   #-1,%d2\n"
    "7:  bsr     mt_off\n"
    "    move.l  %d2,%d0\n"
    "    movem.l (%sp)+,%d2/%a2-%a3\n"
    "    rts\n"
    /* u32 mmu_regs_save(struct mregs *m): fam must be set; -1 if a register is missing */
    "mmu_regs_save:\n"
    "    move.l  4(%sp),%a1\n"
    "    movem.l %d2/%a2-%a3,-(%sp)\n"
    "    lea     9f(%pc),%a0\n"
    "    bsr     mt_on\n"
    "    cmp.l   #1,(%a1)\n"
    "    beq.s   3f\n"
    "    .long   0x4E7A0003\n"           /* movec tc,d0 */
    "    move.l  %d0,4(%a1)\n"
    "    .long   0x4E7A0006\n"
    "    move.l  %d0,8(%a1)\n"
    "    .long   0x4E7A0007\n"
    "    move.l  %d0,12(%a1)\n"
    "    .long   0x4E7A0004\n"
    "    move.l  %d0,16(%a1)\n"
    "    .long   0x4E7A0005\n"
    "    move.l  %d0,20(%a1)\n"
    "    .long   0x4E7A0806\n"           /* urp */
    "    move.l  %d0,28(%a1)\n"
    "    bra.s   8f\n"
    "3:  lea     4(%a1),%a0\n"
    "    .long   0xF0104200\n"           /* pmove tc,(a0) */
    "    lea     8(%a1),%a0\n"
    "    .long   0xF0100A00\n"           /* pmove tt0,(a0) */
    "    lea     12(%a1),%a0\n"
    "    .long   0xF0100E00\n"           /* pmove tt1,(a0) */
    "    lea     24(%a1),%a0\n"
    "    .long   0xF0104E00\n"           /* pmove crp,(a0) */
    "8:  moveq   #0,%d2\n"
    "    bra.s   7f\n"
    "9:  moveq   #-1,%d2\n"
    "7:  bsr     mt_off\n"
    "    move.l  %d2,%d0\n"
    "    movem.l (%sp)+,%d2/%a2-%a3\n"
    "    rts\n"
    /* mt_on: vectors 2 (access fault), 4 (illegal), 11 (line F), 56 (030 MMU configuration) to mt_trap, which
     * goes on at a0 with the stack as at the call of mt_on's caller's body. Uses a2 (the saved vectors' place), a3. */
    "mt_on:\n"
    "    move.l  (%sp)+,%a3\n"
    "    move.l  %a0,mt_pc\n"
    "    movec   %vbr,%a2\n"
    "    move.l  0x08(%a2),-(%sp)\n"
    "    move.l  0x10(%a2),-(%sp)\n"
    "    move.l  0x2C(%a2),-(%sp)\n"
    "    move.l  0xE0(%a2),-(%sp)\n"
    "    lea     mt_trap(%pc),%a0\n"
    "    move.l  %a0,0x08(%a2)\n"
    "    move.l  %a0,0x10(%a2)\n"
    "    move.l  %a0,0x2C(%a2)\n"
    "    move.l  %a0,0xE0(%a2)\n"
    "    move.l  %sp,mt_sp\n"
    "    jmp     (%a3)\n"
    "mt_off:\n"
    "    move.l  (%sp)+,%a3\n"
    "    move.l  mt_sp,%sp\n"
    "    movec   %vbr,%a2\n"
    "    move.l  (%sp)+,0xE0(%a2)\n"
    "    move.l  (%sp)+,0x2C(%a2)\n"
    "    move.l  (%sp)+,0x10(%a2)\n"
    "    move.l  (%sp)+,0x08(%a2)\n"
    "    jmp     (%a3)\n"
    "mt_trap:\n"
    "    move.l  mt_sp,%sp\n"
    "    move.l  mt_pc,-(%sp)\n"
    "    rts\n"
    /* u32 mmu_probe_hw(m, t, r): m loaded (translation on, the windows for supervisor accesses only), then two user
     * data reads with MOVES (SFC 1): t, mapped onto r, must give r's word (bit 0); r, invalid, must fault (bit 1).
     * Bit 2: a register trapped. Translation off again at the end (TC/TCR E clear, ATC flushed). */
    "mmu_probe_hw:\n"
    "    movem.l %d2-%d4/%a2-%a5,-(%sp)\n"
    "    move.l  32(%sp),%a4\n"           /* m */
    "    move.l  36(%sp),%a5\n"           /* t */
    "    move.l  40(%sp),%d4\n"           /* r */
    "    moveq   #0,%d2\n"
    "    move.l  %a4,-(%sp)\n"
    "    bsr     mmu_regs_load\n"
    "    addq.l  #4,%sp\n"
    "    tst.l   %d0\n"
    "    beq.s   1f\n"
    "    moveq   #4,%d2\n"
    "    bra.s   5f\n"
    "1:  movec   %sfc,%d3\n"
    "    moveq   #1,%d0\n"
    "    movec   %d0,%sfc\n"             /* user data */
    "    lea     2f(%pc),%a0\n"
    "    bsr     mt_on\n"
    "    moves.l (%a5),%d0\n"            /* t as a user sees it */
    "    move.l  %d4,%a1\n"
    "    cmp.l   (%a1),%d0\n"            /* r's word (the kernel's window) */
    "    bne.s   2f\n"
    "    or.b    #1,%d2\n"
    "2:  lea     3f(%pc),%a0\n"
    "    move.l  %a0,mt_pc\n"
    "    move.l  %d4,%a1\n"
    "    moves.l (%a1),%d0\n"            /* r: invalid, must fault */
    "    bra.s   4f\n"
    "3:  or.b    #2,%d2\n"
    "4:  bsr     mt_off\n"
    "    movec   %d3,%sfc\n"
    "5:  lea     6f(%pc),%a0\n"          /* translation off whatever happened */
    "    bsr     mt_on\n"
    "    cmp.l   #1,(%a4)\n"
    "    beq.s   7f\n"
    "    move.l  4(%a4),%d0\n"
    "    and.l   #0xFFFF7FFF,%d0\n"
    "    .long   0x4E7B0003\n"
    "    nop\n"
    "    .word   0xF518\n"
    "    bra.s   6f\n"
    "7:  clr.l   -(%sp)\n"
    "    move.l  %sp,%a0\n"
    "    .long   0xF0104000\n"
    "    addq.l  #4,%sp\n"
    "    .long   0xF0002400\n"
    "6:  bsr     mt_off\n"
    "    move.l  %d2,%d0\n"
    "    movem.l (%sp)+,%d2-%d4/%a2-%a5\n"
    "    rts\n"
    /* void mmu_ttr(const u32 *v, u32 fam): the windows (no ATC flush: a window match wins over the ATC) */
    "mmu_ttr:\n"
    "    move.l  4(%sp),%a0\n"
    "    cmp.l   #1,8(%sp)\n"
    "    beq.s   1f\n"
    "    move.l  (%a0)+,%d0\n"
    "    .long   0x4E7B0006\n"           /* dtt0 */
    "    move.l  (%a0)+,%d0\n"
    "    .long   0x4E7B0007\n"           /* dtt1 */
    "    move.l  (%a0),%d0\n"
    "    .long   0x4E7B0004\n"           /* itt0 */
    "    nop\n"
    "    rts\n"
    "1:  .long   0xF0100900\n"           /* pmovefd (a0),tt0 */
    "    rts\n"
    "mmu_flush:\n"
    "    cmp.l   #1,4(%sp)\n"
    "    beq.s   1f\n"
    "    nop\n"
    "    .word   0xF518\n"               /* pflusha */
    "    nop\n"
    "    rts\n"
    "1:  .long   0xF0002400\n"           /* pflusha (030) */
    "    rts\n"
    "    .pushsection .bss\n"
    "    .balign 4\n"
    "mt_pc: .space 4\n"            /* (after the release with protection off, stale: only mt_trap, in the span, reads it) */
    "mt_sp: .space 4\n"
    "    .popsection\n"
    "    .popsection\n");

/* The kernel's default options (D41 section 13): tools/kopts.py finds the marker in a kernel file and sets the long
 * after it. 0 in every build: protection off unless zenixboot MMU=ON asks. */
__attribute__((used, aligned(4))) static volatile const u32 kopt_default[3] = { 0x5A4B4F50u, 0x54444546u, 0 };

/* ---- the tree ---- */
KMMU static u32 *pte(u32 a)          /* the page descriptor of a, or 0 where the pools have no page table */
{
    u32 d = root[a >> 25];
    if (!(d & 2))
        return 0;
    d = ((u32 *)(d & ~0x1FFu))[(a >> 18) & 127];
    if (!(d & 2))
        return 0;
    return (u32 *)(d & ~0xFFu) + ((a >> 12) & 63);
}

/* every page of [lo, hi) (rounded out to pages): bit 0 and W set to how (0: invalid; D_VALID; D_VALID | D_WP) */
KMMU __attribute__((optimize("O2"))) static void flip(u32 lo, u32 hi, u32 how)
{
    lo &= ~(MMU_PG - 1);
    hi = (hi + MMU_PG - 1) & ~(MMU_PG - 1);
    while (lo < hi) {
        u32 end = (lo | 0x3FFFFu) + 1;              /* the end of this page table's 256 KB */
        if (end > hi || !end)
            end = hi;
        u32 *d = pte(lo), *d0 = d;
        if (d) {
            for (u32 a = lo; a < end; a += MMU_PG, d++)
                *d = (*d & ~(D_VALID | D_WP)) | how;
            if (dpush)
                cache_push_range((u32)d0, (u32)d);  /* the 060's table search does not look in the data cache */
        }
        lo = end;
    }
}

/* p's view (D41 section 6): what user_ok_wr/rd let it hand to a system call; only page-aligned starts are flipped in */
KMMU __attribute__((optimize("O2"))) static void view(const struct proc *p, int in)
{
    u32 rw = in ? D_VALID : 0;
    if (!in || !(p->base & (MMU_PG - 1)))
        flip(p->base, p->base + p->span, rw);
    for (int i = 0; i < NREG; i++)
        if (p->reg[i].base && (!in || !(p->reg[i].base & (MMU_PG - 1))))
            flip(p->reg[i].base, p->reg[i].base + p->reg[i].size, rw);
    if (p->csize && (!in || !(p->code & (MMU_PG - 1))))
        flip(p->code, p->code + p->csize, in ? D_VALID | D_WP : 0);
    if (upd_pa && (p->uid == 0 || !in)) /* D15: zup (root) reads the update bundle in place, read only; out whatever
                                         * the uid is now (a root tenant that called setuid kept it: audit A6-10) */
        flip(upd_pa, upd_pa + upd_size, in ? D_VALID | D_WP : 0);
}

KMMU static int serves(const struct proc *c, const struct proc *s)
{
    return c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING && c->wait_msg->server == s->pid;
}

/* the tenant's view and its lent clients' out, n's and its held clients' in, then the ATC flushed */
/* the flips of a retenant run with the serial port's receive (level 5: it only queues the byte) let in: a big view
 * takes longer than one character at 115200 baud. Never lowers a mask below 4. */
KMMU static inline u16 sr_to4(void)
{
    u16 s;
    __asm__ volatile("move.w %%sr,%0" : "=d"(s));
    if ((s & 0x0700) > 0x0400)
        __asm__ volatile("move.w %0,%%sr" :: "d"((u16)((s & 0xF8FF) | 0x0400)) : "memory");
    return s;
}
KMMU static inline void sr_back(u16 s) { __asm__ volatile("move.w %0,%%sr" :: "d"(s) : "memory"); }

KMMU __attribute__((optimize("O2"))) static void retenant(struct proc *n)
{
    u16 sr = sr_to4();
    if (tenant) {
        view(tenant, 0);
        for (int i = 1; i < NPROC; i++)
            if (lent & (1u << i))
                view(&procs[i], 0);
    }
    lent = 0;
    tenant = n;
    if (n) {
        view(n, 1);
        for (int i = 1; i < NPROC; i++)
            if (serves(&procs[i], n)) {
                view(&procs[i], 1);
                lent |= 1u << i;
            }
    }
    sr_back(sr);
    mmu_flush(fam);
}

KMMU static void setmode(int m)
{
    if (mode != m) {
        mode = (u8)m;
        mmu_ttr(win[m], fam);
    }
}

KMMU __attribute__((optimize("O2"))) static void mmu_switch_(struct proc *n)
{
    if (n->pid == 0 || n->exiting)  /* idle and a process in proc_exit never run user code again */
        return;
    if (sysmask & SLOT(n)) {
        setmode(0);
        return;
    }
    if (n != tenant)
        retenant(n);
    setmode(1);
}

KMMU static void mmu_sysnow_(void)    /* the current process made a system one: system mode now */
{
    u16 s = irq_save();
    setmode(0);
    irq_restore(s);
}

void mmu_sysmark(struct proc *p)
{
    if (p->dbgf & DBGF_TRACED)
        dbg_sysmark(p);                 /* D54 section 4: a system process has no debugger */
    sysmask |= SLOT(p);
    if (mmu_on && p == current)         /* from now on, also on its way back to user mode now */
        mmu_ops->sysnow();
}
void mmu_slot_new(struct proc *p) { sysmask &= ~SLOT(p); }
int mmu_is_sys(const struct proc *p) { return (sysmask & SLOT(p)) != 0; }


/* proc_exit, at its start (before seg_put drops the code): never seen again */
KMMU static void mmu_forget_(struct proc *p)
{
    u16 s = irq_save();
    if (p == tenant)
        retenant(0);
    else if (lent & SLOT(p))
        retenant(tenant);               /* a lent client that ends: the tenant's set again without it */
    irq_restore(s);
}

KMMU static void mmu_lend_(struct proc *c)
{
    if (!c || current != tenant || c == current || (lent & SLOT(c)))
        return;
    u16 s = irq_save();
    sr_to4();                           /* as retenant: the serial port's receive let in while a big view flips (A6-12) */
    view(c, 1);
    lent |= SLOT(c);
    mmu_flush(fam);
    irq_restore(s);
}

/* [lo, hi) to how, the ATC flushed (a block made for the tenant; a block freed) */
KMMU static void flipf(u32 lo, u32 hi, u32 how)
{
    u16 s = irq_save();
    sr_to4();                           /* (A6-12, as mmu_lend) */
    flip(lo, hi, how);
    mmu_flush(fam);
    irq_restore(s);
}

KMMU static void mmu_block_(u32 data, u32 len, u32 owner)
{
    if (tenant && (u32)tenant->pid == owner)        /* a lent client never allocates: it waits for its reply */
        flipf(data, data + len, D_VALID);
}

KMMU static void mmu_upd_gone_(void)     /* the bundle's pages (their partial ends too) out before it is freed */
{
    if (upd_pa)
        flipf(upd_pa, upd_pa + upd_size, 0);
}

KMMU static void mmu_freed_(u32 data, u32 len)
{
    u32 lo = (data + MMU_PG - 1) & ~(MMU_PG - 1), hi = (data + len) & ~(MMU_PG - 1);
    if (lo < hi)
        flipf(lo, hi, 0);
}

/* ZINE (D41 section 11), interrupts masked: off as D37 left the registers; back with the windows of the mode now */
KMMU static void mmu_hand_(int back)
{
    if (!back) {
        mmu_regs_load(&off);
        return;
    }
    struct mregs m = on;
    m.dtt0 = win[mode][0];
    if (fam == 2) {
        m.dtt1 = win[mode][1];
        m.itt0 = win[mode][2];
    }
    mmu_regs_load(&m);
}

KMMU static void mmu_before_reset_(void)
{
    struct mregs z = off;               /* D37's windows; cache_reset_state clears them after the cache is off */
    z.tc = fam == 2 ? off.tc & ~0x8000u : 0;
    mmu_regs_load(&z);
    mmu_on = 0;
}

static void mmu_arm(u8 f);

/* ---- the boot (D41 section 12) ---- */
KINIT static int off_why(const char *why)
{
    kprintf("mmu: memory protection off: %s\n", why);
    return 0;
}

KINIT static u32 *table(u32 *slot, u8 **next, u32 size)   /* the table *slot names, made from *next if there is none */
{
    if (!(*slot & 2)) {
        u32 *t = (u32 *)*next;
        *next += size;
        *slot = (u32)t | D_TABLE;
    }
    return (u32 *)(*slot & ~(size - 1));
}

KINIT void mmu_setup(const struct bootinfo *bi)
{
    const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
    u32 k = bi->kopts;
    int want = (k & KOPT_MMU_OFF) ? 0 : (k & KOPT_MMU_ON) ? 1 : (kopt_default[2] & KOPT_MMU_ON) != 0;
    if (!(bi->attn & 0x8C))
        return (void)off_why("no MMU on a 68020");
    if (hw->magic != HWINFO_MAGIC)
        return (void)off_why("no hardware table");
    if (hw->flags & HW_DT)
        return (void)off_why("Emu68");
    fam = (bi->attn & 0x88) ? 2 : 1;
    on.fam = off.fam = fam;
    if (fam == 2 && !want)              /* a 68040/060 not asked: no MMU register read (an EC060 has none: audit A6-11);
                                         * the 68030's cleanup below runs on every boot */
        goto not_asked;
    int miss = mmu_regs_save(&off) != 0;
    /* the clean state, translation off: the loader's handoff did that on a 040/060 (D37 then set the windows); a 68030's
     * translation an AmigaOS tool left on (Enforcer, CPU FASTROM) is switched off here, protection or not, its tables
     * being memory Zenix uses */
    off.tc &= fam == 2 ? ~0x8000u : 0;
    off.itt0 = off.itt1 = 0;
    if (fam == 1) {
        int was = !miss && ((off.dtt0 | off.dtt1) & 0x8000u);
        mmu_regs_save(&on);
        was |= !miss && (on.tc & 0x80000000u);
        off.dtt0 = off.dtt1 = 0;
        if (was) {
            mmu_regs_load(&off);
            kprintf("mmu: the 68030's translation was left on: switched off\n");
        }
    }
    if (!want)
not_asked:
        return (void)off_why(k & KOPT_MMU_OFF ? "MMU=OFF" : "not asked for (zenixboot MMU=ON)");
    if (!(CIAA(0) & 0x40))
        return (void)off_why("mouse button held");
    if (miss)
        return (void)off_why("no working MMU (a register is missing)");

    /* the kernel's windows (section 4). A page's cache mode is its window's: the first 16 MB inhibited, the fast
     * block D37's mode, so the kernel and a program never see one line two ways; with the data cache off everything is
     * write-through, cachable: only instruction fetches use it then, as they did before */
    u32 cmhi = cache_dmode == CACHE_D_CB ? CM_CB : 0, cmlo = cache_dmode == CACHE_D_OFF ? 0 : CM_INH;
    if (fam == 2) {
        unsigned io[8], base = 0, mask = 0;
        int pk = cache_block(bi, bi->attn & 0x80, &base, &mask, io);
        u32 dtt1 = 0;
        if (pk != CP_NOFAST)
            dtt1 = (u32)base << 24 | (u32)mask << 16 | 0xC000u |
                   (cache_dmode == CACHE_D_CB && pk == 0 ? CM_CB : cache_dmode == CACHE_D_WT && pk == 0 ? 0 : CM_INH);
        for (unsigned u = 1; u < 256; u++)
            if (cp_bit(io, u) && (pk == CP_NOFAST || (u & ~mask) != base)) {
                kprintf("mmu: memory protection off: a board above 16 MB at %p\n", u << 24);
                return;
            }
        win[0][0] = 0x0000C040u;        /* DTT0: the first 16 MB, inhibited, every access */
        win[0][1] = dtt1;
        win[0][2] = 0x00FFC000u;        /* ITT0: every address, cachable, every access */
        for (int i = 0; i < 3; i++)     /* protected mode: S = 01, supervisor accesses only */
            win[1][i] = win[0][i] ? (win[0][i] & ~0x6000u) | 0x2000u : 0;
        on.tc = (bi->attn & 0x80 ? 0x0200u : 0) | 0x8000u;  /* E, 4 KB pages; the 060's DCO as D37 */
        on.itt1 = 0;
        dpush = cache_dmode != CACHE_D_OFF;
    } else {
        win[0][0] = 0x00FF8107u;        /* TT0: everything, RWM, every function code */
        win[1][0] = 0x00FF8143u;        /* function codes 4-7: supervisor only */
        on.tc = 0x80C07760u;            /* E, PS 4 KB, IS 0, TIA 7, TIB 7, TIC 6 */
        on.crp_hi = 0x80000002u;        /* lower limit 0 (none), short table descriptors */
    }

    /* the tables (section 5): the root, a pointer table per 32 MB of pools, a page table per 256 KB of pools */
    const struct zine_range *r;
    u32 nr = mem_ranges(&r), np = 0, ng = 0;
    for (u32 i = 0; i < nr; i++) {      /* two ranges sharing a piece count it twice: a table or two too many */
        ng += ((r[i].hi - 1) >> 18) - (r[i].lo >> 18) + 1;
        np += ((r[i].hi - 1) >> 25) - (r[i].lo >> 25) + 1;
    }
    tbytes = 512 + np * 512 + ng * 256;
    tblk = mem_alloc(tbytes + 512, 0, 0);
    if (!tblk)
        return (void)off_why("out of memory for the tables");
    u8 *t = (u8 *)(((u32)tblk + 511) & ~511u);
    memset(t, 0, tbytes);
    root = (u32 *)t;
    u8 *nextp = t + 512, *nextg = t + 512 + np * 512;
    for (u32 i = 0; i < nr; i++)
        for (u32 a = r[i].lo & ~0x3FFFFu; a < r[i].hi; a += 0x40000u) {
            u32 *pt = table(&root[a >> 25], &nextp, 512);
            u32 *pg = table(&pt[(a >> 18) & 127], &nextg, 256);
            for (u32 j = 0; j < 64; j++) {
                u32 pa = a + j * MMU_PG;
                pg[j] = pa | D_PAGE | (fam == 2 ? (pa < 0x01000000u ? cmlo : cmhi) : 0);
            }
        }
    on.dtt0 = win[0][0];
    on.dtt1 = fam == 2 ? win[0][1] : 0;
    on.itt0 = fam == 2 ? win[0][2] : 0;
    on.root = (u32)root;
    tbytes = (u32)(nextg - t);           /* what was used, for the log line */
    if (dpush)
        cache_push_range((u32)t, (u32)t + tbytes);

    /* the probe (section 3): T mapped onto R, R invalid; the windows in protected mode */
    u8 *pb = mem_alloc(3 * MMU_PG, 0, 0);
    u32 res = 0;
    if (pb) {
        u32 T = ((u32)pb + MMU_PG - 1) & ~(MMU_PG - 1), R = T + MMU_PG;
        u32 *dt = pte(T), keep = *dt;
        *dt = R | D_PAGE | (keep & 0x60u) | D_VALID;
        *(volatile u32 *)T = 0x5A4D5531u;
        *(volatile u32 *)R = 0x5A4D5532u;
        if (dpush)
            cache_push_range((u32)dt, (u32)dt + 4);
        struct mregs m = on;
        m.dtt0 = win[1][0];
        if (fam == 2) {
            m.dtt1 = win[1][1];
            m.itt0 = win[1][2];
        }
        res = mmu_probe_hw(&m, T, R);
        *dt = keep;
        if (dpush)
            cache_push_range((u32)dt, (u32)dt + 4);
        mem_free(pb);
    }
    if (res != 3) {
        mmu_regs_load(&off);
        mem_free(tblk);
        tblk = 0;
        kprintf("mmu: memory protection off: no working MMU (the probe gave %u)\n", res);
        return;
    }
    if (mmu_regs_load(&on)) {
        mmu_regs_load(&off);
        mem_free(tblk);
        tblk = 0;
        return (void)off_why("no working MMU (a register is missing)");
    }
    mode = 0;
    mmu_arm(fam);                       /* the table to the span's functions, and mmu_on set: together, only here */
    kprintf("mmu: memory protection on: %s, 4 KB pages, %u KB of tables\n",
            bi->attn & 0x80 ? "68060" : bi->attn & 0x08 ? "68040" : "68030", (tbytes + 1023) / 1024);
}

/* ---- a fault (D41 section 10) ---- */
KMMU static int mmu_fault_(const u8 *f, u32 pc)
{
    u16 fv = *(const u16 *)(f + 6);
    if (((fv & 0xFFF) >> 2) != 2)
        return 0;
    u32 a = pc, fmt = fv >> 12;
    const char *how = "an instruction fetch at";
    if (fmt == 7) {                     /* 040: FA, SSW's RW and TM */
        u16 ssw = *(const u16 *)(f + 0x0C);
        a = *(const u32 *)(f + 0x14);
        if ((ssw & 7) != 2)
            how = ssw & 0x100 ? "a read of" : "a write to";
    } else if (fmt == 4) {              /* 060: FA, FSLW's IO and RW */
        u32 fslw = *(const u32 *)(f + 0x0C);
        a = *(const u32 *)(f + 0x08);
        if (!(fslw & 0x8000))
            how = ((fslw >> 23) & 3) == 2 ? "a read of" : "a write to";   /* 11: read-modify-write */
    } else if (fmt == 0xA || fmt == 0xB) {      /* 030: the data cycle's fault address when DF says so */
        u16 ssw = *(const u16 *)(f + 0x0A);
        if (ssw & 0x100) {
            a = *(const u32 *)(f + 0x10);
            how = ssw & 0x40 ? "a read of" : "a write to";
        }
    } else
        return 0;
    int used = 0;
    u32 o = mem_owner_of(a, &used);
    const char *w = "the kernel's memory";
    struct proc *op = 0;
    if (current->csize && a - current->code < current->csize)
        w = "its own code (read only)";
    else if (o == MEM_NOT_POOL) {      /* RAM the pools leave out is the kernel's (low memory, its image, the klog
                                         * area, the RAM disk); anything else is the chips, a board or nothing */
        const struct bootinfo *bi = (const struct bootinfo *)0x1000;
        w = "no memory (the hardware, or nothing)";
        for (int i = 0; i < bi->nregions && i < 8; i++)
            if ((bi->r[i].attrs & (MEMF_CHIP | MEMF_FAST)) && a >= bi->r[i].lower && a < bi->r[i].upper)
                w = "the kernel's memory";
        if (a < 0x2000)
            w = "the kernel's memory";
    } else if (!used)
        w = "free memory";
    else if (o == SEG_OWNER)
        w = "shared code";
    else if (o >= 0x80000100u && o < 0x80000200u)
        w = "a driver's DMA memory";
    else if (o && o < 0x80000000u && (op = proc_by_pid((int)o)) != 0)
        w = 0;
    kprintf("\n[kernel] pid %d (%s): memory protection: %s %p (", current->pid, current->name, how, a);
    if (w)
        kprintf("%s", w);
    else
        kprintf("the memory of pid %d (%s)", op->pid, op->name);
    kprintf("): killed (SIGSEGV)");
    return 1;
}

/* ---- the table (D45 section 13): every call from outside into this span goes through mmu_ops ---- */
static void nop_p(struct proc *p) { (void)p; }
static void nop_v(void) {}
static void nop_3(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; }
static void nop_2(u32 a, u32 b) { (void)a; (void)b; }
static void nop_i(int a) { (void)a; }
static int nop_f(const u8 *f, u32 pc) { (void)f; (void)pc; return 0; }
static const struct mmu_ops mmu_ops_off = { nop_p, nop_v, nop_p, nop_p, nop_3, nop_v, nop_2, nop_i, nop_v, nop_f };
const struct mmu_ops *mmu_ops = &mmu_ops_off;

/* the live table: read-only data in the span (.zhwmmu.rodata), the only place that holds span functions' addresses;
 * tools/check-init.py lets only mmu_arm refer to it, as move.l #mmu_ops_on,mmu_ops */
__attribute__((section(".zhwmmu.rodata"))) static const struct mmu_ops mmu_ops_on = {
    mmu_switch_, mmu_sysnow_, mmu_forget_, mmu_lend_, mmu_block_, mmu_upd_gone_, mmu_freed_, mmu_hand_,
    mmu_before_reset_, mmu_fault_ };

/* the only writer of mmu_on besides mmu_before_reset_ (tools/check-init.py): armed exactly while it is set */
KMMU __attribute__((noinline)) static void mmu_arm(u8 f)
{
    mmu_ops = &mmu_ops_on;
    mmu_on = f;
}

/* mem_boot_release, protection off, before the span is filled and freed: no slot may point into it afterwards */
void mmu_disarm(void) { mmu_ops = &mmu_ops_off; }
