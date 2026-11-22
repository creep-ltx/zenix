/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D41 (docs/d41-memory-protection.md): optional memory protection with the MMU of a 68030, 68040 or 68060.
 * The MMU only protects, never translates: every page is mapped to itself. DESIGN D60 stage 1
 * (docs/d60-full-protection.md section 4): a translation tree per protected process, valid only for its view and the
 * views of the clients whose requests it holds (lent at the request, taken back at its end); the switch is a root
 * pointer write and a flush of the non-global entries; a world-readable shared segment's pages are global on 040/060.
 * The kernel never uses the trees: the transparent translation registers match its supervisor accesses (and every
 * access of a system process, D41 section 7, still), as D37 set them up. Off, the gates in mmu.h call nothing and
 * mem.c lays blocks out as before.
 * DESIGN D45 section 13: the runtime code (KMMU, the asm) lies in the span .zhwmmu, reached only through the table
 * mmu_ops points at, and is given back after the boot when protection is off.
 * Cold code at -Os; the switch, the flips and the flushes at -O2 (they run at a task switch and at every request). */
#pragma GCC optimize("Os")
#include "kernel.h"
#include "hw.h"
#include "hwinfo.h"
#include "cache_plan.h"
#include "zine.h"
#include "mmu.h"

/* the registers as one image (offsets used by the assembly below): 030: dtt0 = TT0, dtt1 = TT1, crp = CRP's two longs
 * (crp_hi, root); 040/060: root goes to URP, srp to SRP */
struct mregs { u32 fam, tc, dtt0, dtt1, itt0, itt1, crp_hi, root, srp; };

u32 mmu_regs_load(const struct mregs *m);       /* below: 0, or -1 if a register trapped (nothing is then sure) */
u32 mmu_regs_save(struct mregs *m);             /* the registers as they are; -1 if one is missing */
void mmu_ttr(const u32 *v, u32 fam);            /* the windows: 040/060 DTT0, DTT1, ITT0 = v[0..2]; 030 TT0 = v[0] */
void mmu_flush(u32 fam);                        /* PFLUSHA */
void mmu_flushan(void);                         /* 040/060: PFLUSHAN (the entries that are not global) */
void mmu_pflush(const u32 *a, u32 n);           /* MMU_PFLUSH_PAGES: PFLUSH (An) of user entries (DFC 1), n pages */
void mmu_root(u32 root, u32 fam);               /* the switch: URP and PFLUSHAN on 040/060, CRP (which flushes) on 030 */
u32 mmu_probe_hw(const struct mregs *m, u32 t, u32 r);
u32 mmu_fpu(void);                              /* 1: an FPU (a full 68040) */
long cpu_pcr(void);                             /* start.S: the 68060's PCR, -1 on a 68040 */

u8 mmu_on;
static u8 fam;                  /* 1: 68030; 2: 68040/68060 */
static u8 mode;                 /* the windows now: 0 system (every access), 1 protected (supervisor only) */
static u8 dpush;                /* the table writes must be pushed (a 040/060 with the data cache on) */
static u32 tbytes;              /* the pool's bytes */
static struct mregs on, off;    /* translation on (the windows in system mode), and as it was before (D37's) */
static u32 win[2][3];           /* the windows' values: [mode][DTT0/TT0, DTT1, ITT0] */
static u32 sysmask;             /* bit n: procs[n] is a system process */
static u32 cmlo, cmhi;          /* 040/060: a page's cache mode below and above 16 MB (its kernel window's) */

/* D60 section 4: a tree per protected process, made at its first switch to user mode */
struct tree {
    u32 *root;                  /* 0: none made (yet) */
    u32 lent;                   /* bit n: procs[n]'s view is in this tree (this process holds that client's request) */
    u8 upd;                     /* the update bundle is in it (root's zup, D41 section 6) */
};
static struct tree *trees;      /* NPROC of them, allocated by mmu_setup */
static u32 lentany;             /* bit n: procs[n]'s view may be lent to some tree (set at a lend, cleared by
                                 * mmu_block_ after it took the stale lends back) */
static u32 *krt;                /* the kernel's tree: nothing valid (SRP; URP while no protected process ran) */
static u32 *loaded;             /* the root in URP / CRP now */

#define SLOT(p) (1u << ((p) - procs))
#define D_TABLE 0x0Au           /* a table descriptor: U, resident (DT/UDT 2; the same bits on 030 and 040/060) */
#define D_PAGE  0x18u           /* a page descriptor's fixed bits: U and M set (the table search never writes) */
#define D_VALID 1u              /* bit 0: resident (030 DT 1, 040/060 PDT 01) */
#define D_WP    4u              /* bit 2: write-protected (030 WP, 040/060 W) */
#define D_G     0x400u          /* 040/060 bit 10: global, kept by PFLUSHAN (never on the 030: an address bit there) */
#define CM_INH  0x40u           /* 040/060 CM 10: inhibited, serialized / precise */
#define CM_CB   0x20u           /* 040/060 CM 01: copyback; 00 write-through */
#define PG_SH   (MMU_PG == 8192u ? 13 : 12)
#define NPTE    (0x40000u / MMU_PG)     /* a page table's entries: 64 (4 KB pages) or 32 (8 KB) */
#define PTSZ    (NPTE * 4u)     /* its bytes, and its alignment */

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
    "    move.l  32(%a1),%d0\n"
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
    /* u32 mmu_fpu(void): 1 if an FRESTORE of a null frame runs (the CPU has an FPU: a full 68040; the FPU is left in
     * its reset state, so nothing AmigaOS left pending in it can raise an exception later), 0 if it traps (line F: a
     * 68LC040 or 68EC040, which nothing else tells apart; MC68040UM appendices A and B) */
    "mmu_fpu:\n"
    "    movem.l %d2/%a2-%a3,-(%sp)\n"
    "    moveq   #0,%d2\n"
    "    lea     1f(%pc),%a0\n"
    "    bsr     mt_on\n"
    "    clr.l   -(%sp)\n"
    "    .word   0xF35F\n"               /* frestore (sp)+: the null state (mt_trap drops the long on a trap) */
    "    moveq   #1,%d2\n"
    "1:  bsr     mt_off\n"
    "    move.l  %d2,%d0\n"
    "    movem.l (%sp)+,%d2/%a2-%a3\n"
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
    "mmu_flushan:\n"
    "    nop\n"
    "    .word   0xF510\n"               /* pflushan: the global entries (a world-readable segment's code) stay */
    "    nop\n"
    "    rts\n"
    /* void mmu_root(u32 root, u32 fam): the next tree. 040/060: the descriptors were pushed when written; the nop
     * drains the 060's store buffer, then URP and PFLUSHAN. 030: PMOVE to CRP with FD 0 flushes the ATC itself
     * (MC68030UM 9.7.5.1) */
    "mmu_root:\n"
    "    move.l  4(%sp),%d0\n"
    "    cmp.l   #1,8(%sp)\n"
    "    beq.s   1f\n"
    "    nop\n"
    "    .long   0x4E7B0806\n"           /* movec d0,urp */
    "    .word   0xF510\n"               /* pflushan */
    "    nop\n"
    "    rts\n"
    "1:  move.l  %d0,-(%sp)\n"
    "    move.l  #0x80000002,-(%sp)\n"   /* CRP's upper long: no limit, short table descriptors */
    "    move.l  %sp,%a0\n"
    "    .long   0xF0104C00\n"           /* pmove (a0),crp */
    "    addq.l  #8,%sp\n"
    "    rts\n"
    "    .pushsection .bss\n"
    "    .balign 4\n"
    "mt_pc: .space 4\n"            /* (after the release with protection off, stale: only mt_trap, in the span, reads it) */
    "mt_sp: .space 4\n"
    "    .popsection\n"
    "    .popsection\n");

#ifdef MMU_PFLUSH_PAGES
/* a variant for the measurements on a real CPU (make DIAG=-DMMU_PFLUSH_PAGES): up to 8 pages flushed one by one
 * instead of PFLUSHAN. Both emulators' MMU model makes PFLUSH (An) cost milliseconds (D60 "As built"). */
__asm__(
    "    .pushsection .zhwmmu.text,\"ax\",@progbits\n"
    /* void mmu_pflush(const u32 *a, u32 n): PFLUSH (An) for each page, user entries (DFC 1: MC68040UM 3.7.2,
     * MC68060UM 4.7.2); the global ones at those pages too */
    "mmu_pflush:\n"
    "    move.l  4(%sp),%a1\n"
    "    move.l  8(%sp),%d1\n"
    "    movec   %dfc,%d0\n"
    "    move.l  %d0,-(%sp)\n"
    "    moveq   #1,%d0\n"
    "    movec   %d0,%dfc\n"
    "    nop\n"
    "    bra.s   2f\n"
    "1:  move.l  (%a1)+,%a0\n"
    "    .word   0xF508\n"               /* pflush (a0) */
    "2:  dbra    %d1,1b\n"
    "    nop\n"
    "    move.l  (%sp)+,%d0\n"
    "    movec   %d0,%dfc\n"
    "    rts\n"
    "    .popsection\n");
#endif

/* The kernel's default options (D41 section 13): tools/kopts.py finds the marker in a kernel file and sets the long
 * after it. KOPT_MMU_AUTO in every build (D41 section 19, the partner 2026-11-17): protection on wherever the CPU is
 * known to have an MMU (a 68030 whose MMU passes the probe, a 68040 with an FPU, a 68060 whose PCR says 0x0430); an LC
 * or EC 040/060, which only the MMU's own instructions tell apart, only with MMU=ON. 0: off unless MMU=ON;
 * KOPT_MMU_ON: on wherever the probe passes, LC and EC parts tried too. */
__attribute__((used, aligned(4))) static volatile const u32 kopt_default[3] = { 0x5A4B4F50u, 0x54444546u,
                                                                               KOPT_MMU_AUTO };

/* ---- the table pool (D60 section 4): units of PTSZ and 512 bytes cut from whole pages of the kernel's memory (fast
 * RAM first), aligned to their size as the manuals ask. A tree's tables go back to the pool when their last valid
 * entry goes; the pool keeps its pages (its size is the most the trees needed at once) ---- */
static u32 *fl[2];              /* the free units of each size (0: page tables, 1: roots and pointer tables), linked */

KMMU static u32 usz(int k) { return k ? 512u : PTSZ; }

KMMU static u32 *unit(int k)    /* a zeroed unit, pushed, or 0 */
{
    if (!fl[k]) {
        u8 *pg = mem_alloc(MMU_PG, MEM_USER, 0);
        if (!pg)
            return 0;
        tbytes += MMU_PG;
        for (u32 o = 0; o < MMU_PG; o += usz(k)) {
            u32 *u = (u32 *)(pg + o);
            *u = (u32)fl[k];
            fl[k] = u;
        }
    }
    u32 *u = fl[k];
    fl[k] = (u32 *)*u;
    memset(u, 0, usz(k));
    if (dpush)
        cache_push_range((u32)u, (u32)u + usz(k));
    return u;
}

KMMU static void unit_free(u32 *u, int k)
{
    *u = (u32)fl[k];
    fl[k] = u;
}

/* ---- the trees ---- */
KMMU static inline void dpushd(u32 *a, u32 *b) { if (dpush) cache_push_range((u32)a, (u32)b); }

/* the pages to flush after a change (only changes to the loaded tree, and every global entry cleared anywhere) */
static u32 fl_n;
#ifdef MMU_PFLUSH_PAGES
static u32 fl_a[8];
#endif
static u8 fl_g;

KMMU static u32 *pt_of(u32 *r, u32 a, int mk)   /* a's page table in tree r (made if mk), or 0 */
{
    u32 *e = &r[a >> 25];
    if (!(*e & 2)) {
        u32 *t;
        if (!mk || !(t = unit(1)))
            return 0;
        *e = (u32)t | D_TABLE;
        dpushd(e, e + 1);
    }
    u32 *pt = (u32 *)(*e & ~0x1FFu);
    e = &pt[(a >> 18) & 127];
    if (!(*e & 2)) {
        u32 *t;
        if (!mk || !(t = unit(0)))
            return 0;
        *e = (u32)t | D_TABLE;
        dpushd(e, e + 1);
    }
    return (u32 *)(*e & ~(PTSZ - 1));
}

KMMU static int all0(const u32 *t, u32 n)
{
    while (n--)
        if (*t++)
            return 0;
    return 1;
}

/* the tables of a's 256 KB in tree r given back when empty (the page table, then its pointer table) */
KMMU static void tidy(u32 *r, u32 a)
{
    u32 *e = &r[a >> 25];
    u32 *pt = (u32 *)(*e & ~0x1FFu), *pe = &pt[(a >> 18) & 127];
    if (!all0((u32 *)(*pe & ~(PTSZ - 1)), NPTE))
        return;
    unit_free((u32 *)(*pe & ~(PTSZ - 1)), 0);
    *pe = 0;
    dpushd(pe, pe + 1);
    if (!all0(pt, 128))
        return;
    unit_free(pt, 1);
    *e = 0;
    dpushd(e, e + 1);
}

/* every page of [lo, hi) (rounded out to pages) in tree r: valid with bits how (D_VALID, | D_WP, | D_G), or invalid
 * (how 0; how TIDY: invalid and the emptied tables given back). -1: no memory for a table (the pages before it are
 * done). Interrupts masked. */
#define TIDY 0x80000000u
KMMU static int tflip(u32 *r, u32 lo, u32 hi, u32 how)
{
    lo &= ~(MMU_PG - 1);
    hi = (hi + MMU_PG - 1) & ~(MMU_PG - 1);
    int ld = r == loaded, td = how == TIDY;
    if (td)
        how = 0;
    while (lo < hi) {
        u32 end = (lo | 0x3FFFFu) + 1;              /* the end of this page table's 256 KB */
        if (end > hi || !end)
            end = hi;
        u32 *pg = pt_of(r, lo, how != 0);
        if (!pg) {
            if (how)
                return -1;
        } else {
            u32 *d = pg + ((lo >> PG_SH) & (NPTE - 1)), *d0 = d, ch = 0;
            u32 bits = how ? D_PAGE | how | (fam == 2 ? (lo < 0x01000000u ? cmlo : cmhi) : 0) : 0;
            for (u32 a = lo; a < end; a += MMU_PG, d++) {
                u32 v = how ? a | bits : 0;
                if (*d == v)
                    continue;
                if (*d & D_G)
                    fl_g = 1;                       /* a global entry survives every switch: flushed whatever is loaded */
                /* a page made valid from invalid needs no flush on 040/060 for data: the 060 makes no ATC entry from an
                 * invalid descriptor (MC68060UM 4.2.2.3), the 040's data ATC holds one only after a data access there,
                 * which killed the process; the 040's instruction ATC may hold one from a prefetch (MC68040UM 3.2.2.3),
                 * so a block the process may run code in is flushed (mmu_block_); the 030's one ATC may too */
                int was = *d & D_VALID;
                *d = v;
                ch = 1;
                if (ld && (was || fam == 1)) {
#ifdef MMU_PFLUSH_PAGES
                    if (fl_n < 8)
                        fl_a[fl_n] = a;
#endif
                    fl_n++;
                }
            }
            dpushd(d0, d);                          /* the 060's table search does not look in the data cache */
            if (td && ch)
                tidy(r, lo);
        }
        lo = end;
    }
    return 0;
}

/* after the changes: PFLUSH of up to 8 pages, else the entries that are not global; everything when a global entry
 * went (D60 section 4's ATC rules). The 030 has no global entries and 22 in all: always PFLUSHA. */
KMMU static void tflush(void)
{
    if (fl_g || (fam == 1 && fl_n))
        mmu_flush(fam);
#ifdef MMU_PFLUSH_PAGES
    else if (fl_n > 8)
        mmu_flushan();
    else if (fl_n)
        mmu_pflush(fl_a, fl_n);
#else
    else if (fl_n)
        mmu_flushan();
#endif
    fl_n = 0;
    fl_g = 0;
}

/* p's code's bits: read only; global on 040/060 for a shared segment of a file everyone may read (D60 section 4) */
KMMU static u32 codebits(const struct proc *p)
{
    return D_VALID | D_WP | (fam == 2 && p->seg && p->seg->world ? D_G : 0);
}

/* p's view (D41 section 6) in tree r: in (or out); its code only when code is set. Only page-aligned starts (a
 * protected process's blocks: what user_ok_wr/rd let it hand to a system call), the same both ways. */
KMMU static int view(u32 *r, const struct proc *p, int in, int code)
{
    u32 rw = in ? D_VALID : 0;
    int bad = 0;
    if (!(p->base & (MMU_PG - 1)))
        bad |= tflip(r, p->base, p->base + p->span, rw);
    for (int i = 0; i < NREG; i++)
        if (p->reg[i].base && !(p->reg[i].base & (MMU_PG - 1)))
            bad |= tflip(r, p->reg[i].base, p->reg[i].base + p->reg[i].size, rw);
    if (code && p->csize && !(p->code & (MMU_PG - 1)))
        bad |= tflip(r, p->code, p->code + p->csize, in ? codebits(p) : 0);
    return bad;
}

KMMU static int serves(const struct proc *c, const struct proc *s)
{
    return c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_SERVING && c->wait_msg->server == s->pid;
}

/* c's request waits in a port of s's (s takes it at its next receive) */
KMMU static int calling(const struct proc *c, const struct proc *s)
{
    u32 i = (u32)c->wait_port & 0xFF;
    return c->state == P_MSG && c->wait_msg && c->wait_msg->state == MSG_QUEUED && i < NPORT && ports[i].used &&
           ports[i].owner == s->pid;
}

/* the lends and the flips run with the serial port's receive (level 5: it only queues the byte) let in: a big view
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

/* the runtime calls from outside: every interrupt masked but the serial port's receive; at the end the flushes */
KMMU static u16 enter(void)
{
    u16 s = irq_save();
    sr_to4();
    return s;
}
KMMU static void leave(u16 s)
{
    tflush();
    irq_restore(s);
}

KMMU static void load(u32 *r)
{
    if (loaded != r) {
        loaded = r;
        mmu_root((u32)r, fam);
    }
}

/* c's view out of s's tree t; its code stays if s or another client s serves runs the same. The tables stay (the
 * next request of a client there needs them again; at most one page table per 256 KB of RAM in a tree, 0.1% of RAM,
 * freed with the tree or with the memory) */
KMMU static void unlend(struct tree *t, const struct proc *s, struct proc *c)
{
    t->lent &= ~SLOT(c);
    int keep = s->csize && s->code == c->code;
    for (int i = 1; i < NPROC && !keep; i++)
        if ((t->lent & (1u << i)) && procs[i].csize && procs[i].code == c->code)
            keep = 1;
    view(t->root, c, 0, !keep);
}

/* a part of a view could not be made valid: its first access there says "memory protection" */
KMMU static void nomem(const struct proc *p)
{
    kprintf("\n[kernel] mmu: no memory for a table of pid %d (%s)\n", p->pid, p->name);
}

KMMU static void lendin(struct tree *t, struct proc *c)
{
    t->lent |= SLOT(c);
    lentany |= SLOT(c);
    if (view(t->root, c, 1, 1)) {
        nomem(c);
        unlend(t, current, c);          /* not kept half made: the next request tries again */
    }
}

/* p's tree made (D60 section 4): its view, the bundle for root, the clients it serves */
KMMU static void build(struct proc *n, struct tree *t)
{
    if (!(t->root = unit(1)))
        return nomem(n);
    t->lent = 0;
    t->upd = 0;
    if (view(t->root, n, 1, 1))
        nomem(n);
    for (int i = 1; i < NPROC; i++)
        if (serves(&procs[i], n))
            lendin(t, &procs[i]);
}

/* the tree of slot i given back (the process ended, or is a system one now); its view out of every other tree (a slot
 * given to a new process: only the marks go, its old pages being out of every tree since they were freed) */
KMMU static void drop(int i, int others)
{
    struct tree *t = &trees[i];
    if (t->root) {
        if (loaded == t->root)
            load(krt);
        for (u32 a = 0; a < 128; a++)           /* every table, then the root: an empty tree needs no flips */
            if (t->root[a] & 2) {
                u32 *pt = (u32 *)(t->root[a] & ~0x1FFu);
                for (u32 b = 0; b < 128; b++)
                    if (pt[b] & 2)
                        unit_free((u32 *)(pt[b] & ~(PTSZ - 1)), 0);
                unit_free(pt, 1);
            }
        unit_free(t->root, 1);
        t->root = 0;
    }
    t->lent = 0;                                /* (the ATC entries of its global pages stay: they show a segment
                                                 * everyone may read; one freed flushes everything, mmu_freed_) */
    t->upd = 0;
    for (int k = 1; others && k < NPROC; k++)
        if (trees[k].lent & (1u << i))
            unlend(&trees[k], &procs[k], &procs[i]);
}

KMMU static void setmode(int m)
{
    if (mode != m) {
        mode = (u8)m;
        mmu_ttr(win[m], fam);
    }
}

/* the switch's slow part: the tree made, a stale lend taken back, the bundle in or out */
KMMU __attribute__((noinline)) static void fix(struct proc *n, struct tree *t)
{
    {
        u16 sr = sr_to4();
        if (!t->root)
            build(n, t);
        if (t->root) {
            for (int k = 1; t->lent && k < NPROC; k++)      /* lent clients it no longer serves, out */
                if ((t->lent & (1u << k)) && !serves(&procs[k], n) && !calling(&procs[k], n)) {
                    if (procs[k].state == P_FREE || procs[k].state == P_ZOMBIE)
                        t->lent &= ~(1u << k);  /* its pages went out of every tree when they were freed */
                    else
                        unlend(t, n, &procs[k]);
                }
            u8 u = upd_pa && n->uid == 0;   /* D15: zup reads the bundle in place, read only; out once not root */
            if (t->upd != u) {          /* (marked in when tried: a half-made map is taken out with the rest) */
                if (tflip(t->root, upd_pa, upd_pa + upd_size, u ? D_VALID | D_WP : TIDY))
                    nomem(n);
                t->upd = u;
            }
        }
        sr_back(sr);
    }
}

KMMU static void mmu_switch_(struct proc *n)
{
    if (n->pid == 0 || n->exiting)  /* idle and a process in proc_exit never run user code again */
        return;
    int i = (int)(n - procs);
    if (sysmask & (1u << i)) {
        setmode(0);
        return;
    }
    struct tree *t = &trees[i];
    if (__builtin_expect(!t->root || t->lent || t->upd != (upd_pa && n->uid == 0), 0))
        fix(n, t);
    u32 *r = t->root ? t->root : krt;   /* no tree (no memory): nothing valid, its first access says so */
    if (loaded != r) {
        loaded = r;
        mmu_root((u32)r, fam);          /* (the flush of every entry not global) */
        fl_n = 0;
    }
    if (__builtin_expect(fl_n | fl_g, 0))
        tflush();
    if (mode != 1) {
        mode = 1;
        mmu_ttr(win[1], fam);
    }
}

void mmu_sysmark(struct proc *p)
{
    if (p->dbgf & DBGF_TRACED)
        dbg_sysmark(p);                 /* D54 section 4: a system process has no debugger */
    if (sysmask & SLOT(p))
        return;
    sysmask |= SLOT(p);
    if (mmu_on)                         /* its tree goes (and from now on, also on its way back to user mode now) */
        mmu_ops->forget(p);
}
/* the slot is given to a new process: a protected one, nothing of the slot's last tree left */
void mmu_slot_new(struct proc *p)
{
    sysmask &= ~SLOT(p);
    if (mmu_on)
        mmu_ops->forget(p);
}
int mmu_is_sys(const struct proc *p) { return (sysmask & SLOT(p)) != 0; }

/* proc_exit, at its start (before seg_put drops the code), a process made a system one (mmu_sysmark), and a slot given to a new process: never seen again */
KMMU static void mmu_forget_(struct proc *p)
{
    u16 s = enter();
    int sys = (sysmask & SLOT(p)) && !p->exiting;
    drop((int)(p - procs), !sys);       /* made a system one: its lends stay (a server may be serving it) */
    if (p == current && sys)
        setmode(0);                     /* every access let through, also on its way back now */
    leave(s);
}

/* ipc_recv gave c's request to the current process: c's view in its tree until the request ends (D60 5.3, D41 8) */
KMMU static void mmu_lend_(struct proc *c)
{
    int i = (int)(current - procs);
    struct tree *t = &trees[i];
    if (!c || c == current || c->exiting || (sysmask & (1u << i)) || !t->root || (t->lent & SLOT(c)))
        return;                         /* (an ending client's calls: its memory is about to go) */
    u16 s = enter();
    lendin(t, c);
    leave(s);
}

/* seg_disown (D54): p's code is its own copy now, never global: its tree made again */
KMMU static void mmu_redo_(struct proc *p)
{
    u16 s = irq_save();
    int i = (int)(p - procs);
    fl_g = 1;                           /* the segment's global entries go, whoever made them */
    if (trees[i].root) {
        drop(i, 1);
        if (p == current && !(sysmask & (1u << i)))
            mmu_switch_(p);             /* it goes on running: its tree now */
    }
    tflush();
    irq_restore(s);
}

/* mem.c: a MEM_USER block was made for owner: valid in its tree at once (D41 9). A tree p's view is still lent to (a
 * server it called before: p runs, so it is served by nobody now) takes it back, so no lend ever lacks a block */
KMMU static int mmu_block_(u32 data, u32 len, u32 owner)
{
    if (!owner || owner >= 0x80000000u)
        return 0;
    struct proc *p = proc_by_pid((int)owner);
    if (!p || !trees[p - procs].root)
        return 0;                       /* no tree yet: made from p's view at its first switch */
    u16 s = enter();
    if (lentany & SLOT(p)) {
        lentany &= ~SLOT(p);
        for (int i = 1; i < NPROC; i++)
            if (trees[i].lent & SLOT(p))
                unlend(&trees[i], &procs[i], p);
    }
    int bad = tflip(trees[p - procs].root, data, data + len, D_VALID);
    if (bad) {
        nomem(p);
        tflip(trees[p - procs].root, data, data + len, TIDY);   /* the block is not given (mem_alloc frees it) */
    }
    if (trees[p - procs].root == loaded)
        fl_n |= 1;                      /* a 040's prefetch may have left an invalid instruction entry there */
    leave(s);
    return bad;
}

KMMU static void mmu_upd_gone_(void)     /* the bundle's pages (their partial ends too) out before it is freed */
{
    if (!upd_pa)
        return;
    u16 s = irq_save();
    for (int i = 1; i < NPROC; i++)
        if (trees[i].upd) {
            tflip(trees[i].root, upd_pa, upd_pa + upd_size, TIDY);
            trees[i].upd = 0;
        }
    tflush();
    irq_restore(s);
}

/* mem.c: a block is freed: its whole pages invalid in every tree (a shared segment's: every global entry flushed) */
KMMU static void mmu_freed_(u32 data, u32 len, u32 owner)
{
    u32 lo = (data + MMU_PG - 1) & ~(MMU_PG - 1), hi = (data + len) & ~(MMU_PG - 1);
    if (lo >= hi)
        return;
    u16 s = enter();
    for (int i = 1; i < NPROC; i++)
        if (trees[i].root)
            tflip(trees[i].root, lo, hi, TIDY);
    if (owner == SEG_OWNER)
        fl_g = 1;
    leave(s);
}

/* ZINE (D41 section 11), interrupts masked: off as D37 left the registers; back with the windows of the mode now and
 * the tree that was loaded */
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
    m.root = (u32)loaded;
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

KINIT void mmu_setup(const struct bootinfo *bi)
{
    const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
    u32 k = bi->kopts;
    u32 d = kopt_default[2];
    /* 0 not asked; 1 asked (MMU=ON, or a kernel file made so): every part tried; 2 the default: known MMUs only */
    int want = (k & KOPT_MMU_OFF) ? 0 : (k & KOPT_MMU_ON) || (d & KOPT_MMU_ON) ? 1 : (d & KOPT_MMU_AUTO) ? 2 : 0;
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
    if (fam == 2 && want == 2) {        /* the default on a 040/060: only a part known to have an MMU is touched */
        long pcr = cpu_pcr();
        if (pcr != -1 ? (u32)pcr >> 16 != 0x0430 : !mmu_fpu())
            return (void)off_why("an LC or EC CPU (MMU=ON tries it)");
    }
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
        return (void)off_why(k & KOPT_MMU_OFF ? "MMU=OFF" : "not asked for (the kernel file's default)");
    if (!(CIAA(0) & 0x40))
        return (void)off_why("mouse button held");
    if (miss)
        return (void)off_why("no MMU (a register is missing)");

    /* the kernel's windows (section 4). A page's cache mode is its window's: the first 16 MB inhibited, the fast
     * block D37's mode, so the kernel and a program never see one line two ways; with the data cache off everything is
     * write-through, cachable: only instruction fetches use it then, as they did before */
    cmhi = cache_dmode == CACHE_D_CB ? CM_CB : 0;
    cmlo = cache_dmode == CACHE_D_OFF ? 0 : CM_INH;
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
        on.tc = (bi->attn & 0x80 ? 0x0200u : 0) | 0x8000u | (MMU_PG == 8192u ? 0x4000u : 0);  /* E, P (8 KB pages);
                                                             * the 060's DCO as D37 */
        on.itt1 = 0;
        dpush = cache_dmode != CACHE_D_OFF;
    } else {
        win[0][0] = 0x00FF8107u;        /* TT0: everything, RWM, every function code */
        win[1][0] = 0x00FF8143u;        /* function codes 4-7: supervisor only */
        on.tc = MMU_PG == 8192u ? 0x80D07750u : 0x80C07760u;    /* E, PS 4 KB, IS 0, TIA 7, TIB 7, TIC 6 (8 KB:
                                                                 * PS 13, TIC 5) */
        on.crp_hi = 0x80000002u;        /* lower limit 0 (none), short table descriptors */
    }

    /* the kernel's tree (D60 section 4): a root with nothing valid, for SRP and for URP until a protected process
     * runs; the trees themselves are made from the table pool once protection is on */
    void *kblk = mem_alloc(1024, 0, 0);
    u8 *pb = mem_alloc(3 * MMU_PG, 0, 0), *tb = mem_alloc(512 + 512 + PTSZ, 0, 0);
    if (!kblk || !pb || !tb) {
        mem_free(kblk);
        mem_free(pb);
        mem_free(tb);
        return (void)off_why("out of memory for the tables");
    }
    krt = (u32 *)(((u32)kblk + 511) & ~511u);
    memset(krt, 0, 512);
    on.dtt0 = win[0][0];
    on.dtt1 = fam == 2 ? win[0][1] : 0;
    on.itt0 = fam == 2 ? win[0][2] : 0;
    on.root = on.srp = (u32)krt;

    /* the probe (D41 section 3): in the kernel's tree for the time of it, T mapped onto R, R invalid; the windows in
     * protected mode */
    u32 T = ((u32)pb + MMU_PG - 1) & ~(MMU_PG - 1), R = T + MMU_PG;
    u32 *ptp = (u32 *)(((u32)tb + 511) & ~511u), *pgp = ptp + 128;
    memset(ptp, 0, 512 + PTSZ);
    krt[T >> 25] = (u32)ptp | D_TABLE;
    ptp[(T >> 18) & 127] = (u32)pgp | D_TABLE;
    pgp[(T >> PG_SH) & (NPTE - 1)] = R | D_PAGE | (fam == 2 ? (T < 0x01000000u ? cmlo : cmhi) : 0) | D_VALID;
    *(volatile u32 *)T = 0x5A4D5531u;
    *(volatile u32 *)R = 0x5A4D5532u;
    if (dpush) {
        cache_push_range((u32)krt, (u32)krt + 512);
        cache_push_range((u32)ptp, (u32)ptp + 512 + PTSZ);
    }
    struct mregs m = on;
    m.dtt0 = win[1][0];
    if (fam == 2) {
        m.dtt1 = win[1][1];
        m.itt0 = win[1][2];
    }
    u32 res = mmu_probe_hw(&m, T, R);
    krt[T >> 25] = 0;
    if (dpush)
        cache_push_range((u32)krt, (u32)krt + 512);
    mem_free(pb);
    mem_free(tb);
    trees = res == 3 ? mem_alloc(NPROC * sizeof *trees, 0, 0) : 0;
    if (!trees) {
        mmu_regs_load(&off);
        mem_free(kblk);
        mem_free(trees);
        trees = 0;
        if (res == 3)
            return (void)off_why("out of memory for the tables");
        kprintf("mmu: memory protection off: no MMU (the probe found none: %u)\n", res);
        return;
    }
    memset(trees, 0, NPROC * sizeof *trees);
    if (mmu_regs_load(&on)) {
        mmu_regs_load(&off);
        mem_free(kblk);
        mem_free(trees);
        trees = 0;
        return (void)off_why("no MMU (a register is missing)");
    }
    loaded = krt;
    mode = 0;
    tbytes = 1024;
    mmu_arm(fam);                       /* the table to the span's functions, and mmu_on set: together, only here */
    kprintf("mmu: memory protection on: %s, %u KB pages, %u KB of tables (a tree per process)\n",
            bi->attn & 0x80 ? "68060" : bi->attn & 0x08 ? "68040" : "68030", MMU_PG / 1024, tbytes / 1024);
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
static int nop_b(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return 0; }
static void nop_i(int a) { (void)a; }
static int nop_f(const u8 *f, u32 pc) { (void)f; (void)pc; return 0; }
static const struct mmu_ops mmu_ops_off = { nop_p, nop_p, nop_p, nop_p, nop_b, nop_v, nop_3, nop_i, nop_v, nop_f };
const struct mmu_ops *mmu_ops = &mmu_ops_off;

/* the live table: read-only data in the span (.zhwmmu.rodata), the only place that holds span functions' addresses;
 * tools/check-init.py lets only mmu_arm refer to it, as move.l #mmu_ops_on,mmu_ops */
__attribute__((section(".zhwmmu.rodata"))) static const struct mmu_ops mmu_ops_on = {
    mmu_switch_, mmu_forget_, mmu_lend_, mmu_redo_, mmu_block_, mmu_upd_gone_, mmu_freed_,
    mmu_hand_, mmu_before_reset_, mmu_fault_ };

/* the only writer of mmu_on besides mmu_before_reset_ (tools/check-init.py): armed exactly while it is set */
KMMU __attribute__((noinline)) static void mmu_arm(u8 f)
{
    mmu_ops = &mmu_ops_on;
    mmu_on = f;
}

/* mem_boot_release, protection off, before the span is filled and freed: no slot may point into it afterwards */
void mmu_disarm(void) { mmu_ops = &mmu_ops_off; }
