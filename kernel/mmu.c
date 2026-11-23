/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D41 (docs/d41-memory-protection.md): optional memory protection with the MMU of a 68030, 68040 or 68060.
 * The MMU only protects, never translates: every page is mapped to itself. DESIGN D60 stage 1
 * (docs/d60-full-protection.md section 4): a translation tree per protected process, valid only for its view and the
 * views of the clients whose requests it holds (lent at the request, taken back at its end); the switch is a root
 * pointer write and a flush of the non-global entries; a world-readable shared segment's pages are global on 040/060.
 * The kernel never uses the trees: the transparent translation registers match its supervisor accesses, as D37 set them
 * up. DESIGN D60 stage 3: there are no system processes (D41 section 7 is gone): every process, the servers too, sees
 * only its own memory, the windows of the requests it holds and its grants. Off, the gates in mmu.h call nothing and
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
#include "netproto.h"
#include "mmu.h"

#ifdef MMU_PROF
static u32 pf_g, pf_e, pf_s, pf_n, pf_ns, pf_fa, pf_fan, pf_r, pf_bn, pf_w, pf_ft, pf_wm, pf_wd, pf_cp, pf_ncp;
#define PF(x) x
#else
#define PF(x)
#endif
#ifndef MMU_PROF                        /* (the profiled build wraps these: DIAG=-DMMU_PROF) */
#define mmu_switch_i mmu_switch_
#define mmu_give_i mmu_give_
#define mmu_end_i mmu_end_
#define win_make_i win_make
#define win_drop_i win_drop
#endif
KMMU static void mmu_switch_i(struct proc *n);
KMMU static void mmu_switch_(struct proc *n);

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
static u8 mode;                 /* the windows now: 0 every access (the boot, before the first process), 1 supervisor only */
static u8 dpush;                /* the table writes must be pushed (a 040/060 with the data cache on) */
static u32 tbytes;              /* the pool's bytes */
static struct mregs on, off;    /* translation on (the windows in system mode), and as it was before (D37's) */
static u32 win[2][3];           /* the windows' values: [mode][DTT0/TT0, DTT1, ITT0] */
static u32 cmlo, cmhi;          /* 040/060: a page's cache mode below and above 16 MB (its kernel window's) */

/* D60 section 4: a tree per protected process, made at its first switch to user mode */
struct tree {
    u32 *root;                  /* 0: none made (yet) */
    u32 lent;                   /* bit n: procs[n]'s view is in this tree (this process holds that client's request) */
    u8 upd;                     /* the update bundle is in it (a root process that asked UPD_INFO, D60 7.2) */
};
static struct tree *trees;      /* NPROC of them, allocated by mmu_setup */
static u32 lentany;             /* bit n: procs[n]'s view may be lent to some tree (set at a lend, cleared by
                                 * mmu_block_ after it took the stale lends back) */
static u32 *krt;                /* the kernel's tree: nothing valid (SRP; URP while no protected process ran) */
static u32 *loaded;             /* the root in URP / CRP now */

#define SLOT(p) (1u << ((p) - procs))
static u32 sysm;                /* MMU=D41 (D60 12): bit n, procs[n] is D41's system process (every access let through) */
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
static u32 *ppg;                /* the pool's pages (bit 0: cut into 512-byte units), for ZINE's rebuild (7.3) */
static u32 npg, cpg;

KMMU static u32 usz(int k) { return k ? 512u : PTSZ; }

KMMU static u32 *unit(int k)    /* a zeroed unit, pushed, or 0 */
{
    if (!fl[k]) {
        if (npg == cpg) {               /* the list of the pool's pages grows by 32 */
            u32 *n = kmalloc((cpg + 32) * 4);
            if (!n)
                return 0;
            if (ppg)
                memcpy(n, ppg, npg * 4);
            kfree(ppg);
            ppg = n;
            cpg += 32;
        }
        u8 *pg = mem_alloc(MMU_PG, MEM_USER, 0);
        if (!pg)
            return 0;
        ppg[npg++] = (u32)pg | (u32)k;
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
KMMU static inline void dpushd(u32 *a, u32 *b) { if (dpush) { PF(u32 t0 = hr_time(); pf_ncp++;) cache_push_range((u32)a, (u32)b); PF(pf_cp += hr_time() - t0;) } }

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
 * done). Interrupts masked. D60 stage 2: these are the identity descriptors; one that maps a window's copy page (a
 * bounce page: another address) is the windows' own (win_* below), and taking pages out leaves it alone */
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
            u32 *d = pg + ((lo >> PG_SH) & (NPTE - 1)), *d0 = d;
            u32 bits = how ? D_PAGE | how | (fam == 2 && !(how & G_IO) ? (lo < 0x01000000u ? cmlo : cmhi) : 0) : 0;
            for (u32 a = lo; a < end; a += MMU_PG, d++) {
                u32 v = how ? a | bits : 0;
                if (*d == v || ((*d & D_VALID) && (*d & ~(MMU_PG - 1)) != a))
                    continue;           /* (a window's bounce descriptor: never taken over, never cleared here) */
                if (*d & D_G)
                    fl_g = 1;                       /* a global entry survives every switch: flushed whatever is loaded */
                /* a page made valid from invalid needs no flush on 040/060 for data: the 060 makes no ATC entry from an
                 * invalid descriptor (MC68060UM 4.2.2.3), the 040's data ATC holds one only after a data access there,
                 * which killed the process; the 040's instruction ATC may hold one from a prefetch (MC68040UM 3.2.2.3),
                 * so a block the process may run code in is flushed (mmu_block_); the 030's one ATC may too */
                int was = *d & D_VALID;
                *d = v;
                if (ld && (was || fam == 1)) {
#ifdef MMU_PFLUSH_PAGES
                    if (fl_n < 8)
                        fl_a[fl_n] = a;
#endif
                    fl_n++;
                }
            }
            dpushd(d0, d);                          /* the 060's table search does not look in the data cache */
            if (td)
                tidy(r, lo);            /* (also unchanged: the tables a window kept, dkeep, go with the memory) */
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
        { PF(pf_fa++;) mmu_flush(fam); }
#ifdef MMU_PFLUSH_PAGES
    else if (fl_n > 8)
        mmu_flushan();
    else if (fl_n)
        mmu_pflush(fl_a, fl_n);
#else
    else if (fl_n)
        { PF(pf_fan++; u32 t0 = hr_time();) mmu_flushan(); PF(pf_ft += hr_time() - t0;) }
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

/* ======== D60 stage 2 (docs/d60-full-protection.md section 5): the kernel's records, the inboxes, windows ========
 * Every call is recorded in the kernel (call(): the record is the message the kernel keeps). A server receives a copy
 * of it in its inbox, a slot of 1 KB in pages of its own memory; for the kernel's own requests (KP_FS, KP_NET) the
 * small buffers the kernel declares are copied after it, the argument pointing at each rewritten to the copy (and
 * back at the reply), and every other buffer is lent to a protected server as a window: its whole pages mapped as
 * they are, its part pages (at most two) mapped onto a bounce page holding exactly the window's bytes. A system
 * process (D41 section 7, still in stage 2) sees all memory: it gets the copies, no windows. A program's own request
 * to a protected server gets D41's lend of the whole client, but only when the server could debug the client (5.8).
 * Interrupts masked (enter/leave) wherever a tree changes. */
#define SLOTSZ   1024u
#define SLOTHDR  72u                    /* struct umsg and struct umsg_groups (52 + 18), then the copies */
#define SPP      (MMU_PG / SLOTSZ)      /* slots a page */
#define NIBPG    (NPROC / SPP)          /* an inbox's pages at most: NPROC slots */
#define PGM      (MMU_PG - 1)
#define NWIN     32                     /* windows in use at once, every server together */
#define WIN_REQ  8                      /* windows a request (5.3) */
#define WIN_BNC  8                      /* bounce pages a window holds */
#define NBNC     64                     /* bounce pages in use at once */
#define BNC_SRV  32                     /* a server's bounce pages (5.3: 128 KB) */
#define NBPOOL   1                      /* bounce pages kept (5.3 says 4; stage 3: services map their clients' part pages
                                         * as they are, so few are made, and every page kept is RAM a 4 MB machine misses) */
#define KHOT     __attribute__((optimize("O2")))    /* every request runs these */

_Static_assert(__builtin_offsetof(struct krec, g) == sizeof(struct umsg), "the groups follow the record's message");
u32 mmu_syncing;                        /* (resident: mmu.h's gate at every system call's end) */
static u32 mmu_updask;                  /* D60 7.2: bit n, procs[n] asked UPD_INFO */
u8 mmu_strict, mmu_d41;                 /* (resident) MMU=STRICT, MMU=D41 (D60 12), set by mmu_setup */
u32 mmu_winners;                        /* (resident) bit n: procs[n] holds a window (SYS_call's gate) */
struct inbox { u32 pg[NIBPG]; u32 busy; u8 rr; u8 used[NPROC]; struct krec *rec[NPROC]; };   /* used: a slot's
                                         * copies' end, in longs; rec: the request whose copy it holds */
struct win { struct krec *k; u32 a, n; u8 srv, wr, sync, nb; u8 b[WIN_BNC]; u8 idp, pown; };    /* b: its bounce pages
                                         * (bncs[]); idp: its part pages that are wholly the client's own memory are
                                         * mapped as they are (a service's window, D60 stage 3); pown: bit 1 its first
                                         * page, bit 2 its last, mapped so by it */
struct bnc { u32 lp, pp; u8 srv, refs, wr, pad; };     /* lp in server srv's tree maps the page pp */
static struct krec *recs;               /* NPROC: a process's record for its call */
static struct inbox **ibx;              /* NPROC: a server's inbox (0: it never received with protection on) */
static u8 nwins[NPROC];                 /* windows each server holds (mmu_winners' bit while not 0) */
static u8 nwin_all, nbnc_all;           /* windows and bounce pages in use, every server together */
#define WIN_USER 8                      /* windows a server that is not the system's may hold at once, */
#define RES_WIN  12                     /* ... and windows and bounce pages kept for the system's servers (a user's */
#define RES_BNC  16                     /* servers and clients can never take them all: the kernel's own requests) */
/* current is the system's own: a service the service manager started, or a driver of the network service */
KMMU static int sysown(void) { return svc_any() || (current->ppid && current->ppid == svc_pid("net")); }
static struct bnc *bncs;
static u32 bpool[NBPOOL];
static u8 nbpool;
static u8 eacces_said;
/* D60 stage 3: a process's grants (beside its view) and the shared regions (7.4) */
#define NGRANT 32                       /* grants, every process together */
#define NSHMT  24                       /* offers and shares, every process together */
struct grant { u32 lo, hi; u8 slot, how, pad[2]; };            /* hi 0: free */
struct shmt { u32 base, size; int owner, to; u8 ro, taken, claim, pad; };   /* size 0: free; one per (region, receiver) */
static struct grant *grants;
static struct shmt *shms;

KMMU static u32 cmode(u32 a) { return fam == 2 ? (a < 0x01000000u ? cmlo : cmhi) : 0; }

/* the descriptor of page l in tree r (0: none) */
KMMU static u32 dget(u32 *r, u32 l)
{
    u32 *pg = pt_of(r, l, 0);
    return pg ? pg[(l >> PG_SH) & (NPTE - 1)] : 0;
}

/* page l's descriptor in tree r set to v (0: invalid, its emptied tables given back); the same flush rules as tflip.
 * -1: no memory for a table */
static u8 dkeep;                        /* dset: an emptied table stays (a window's page: the next request needs it) */
KMMU static int dset(u32 *r, u32 l, u32 v)
{
    u32 *pg = pt_of(r, l, v != 0);
    if (!pg)
        return v ? -1 : 0;
    u32 *d = pg + ((l >> PG_SH) & (NPTE - 1));
    if (*d == v)
        return 0;
    if (*d & D_G)
        fl_g = 1;
    int was = *d & D_VALID;
    *d = v;
    dpushd(d, d + 1);
    if (r == loaded && (was || fam == 1)) {
#ifdef MMU_PFLUSH_PAGES
        if (fl_n < 8)
            fl_a[fl_n] = l;
#endif
        fl_n++;
    }
    if (!v && !dkeep)
        tidy(r, l);
    return 0;
}

KMMU static int ovl(u32 p, u32 lo, u32 n) { return n && p < lo + n && p + MMU_PG > lo; }

/* page p is part of server s's own view (its memory, its code, its inbox): a window never changes it */
KMMU static int own_page(const struct proc *s, u32 p)
{
    if (ovl(p, s->base, s->span) || (s->csize && ovl(p, s->code, s->csize)))
        return 1;
    for (int i = 0; i < NREG; i++)
        if (s->reg[i].base && ovl(p, s->reg[i].base, s->reg[i].size))
            return 1;
    struct inbox *ib = ibx[s - procs];
    for (u32 i = 0; ib && i < NIBPG; i++)
        if (ib->pg[i] == p)
            return 1;
    return 0;
}

/* ---- the records (5.1) ---- */
KMMU KHOT static struct umsg *mmu_rec_(struct umsg *um, u32 kp)
{
    struct krec *k = &recs[current - procs];
    u8 heap = 0;
    if (k->um) {                        /* a call inside a call (a service's load, D11): a record of its own */
        if (!(k = kmalloc(sizeof *k)))
            return 0;
        heap = 1;
    }
    k->m.type = um->type;
    for (int i = 0; i < 4; i++)
        k->m.a[i] = um->a[i];
    k->um = um;
    k->m.state = 0;                     /* (nothing of the record's last call is left to be read as this one's) */
    k->m.result = um->result;
    k->slot = 0;
    k->rw = k->ncopy = k->nwin = k->done = 0;
    k->heap = heap;
    k->kp = (u8)kp;
    k->ndv = k->dvw = 0;
    const struct ucalldecl *dc = mmu_decl;      /* D60 5.5: SYS_callv's buffers (checked by syscall.c) */
    if (dc) {
        mmu_decl = 0;
        for (u32 i = 0; i < dc->n; i++)
            if (dc->b[i].len) {
                k->dva[k->ndv] = dc->b[i].addr;
                k->dvn[k->ndv] = dc->b[i].len;
                k->dvw |= (u8)((dc->b[i].flags & CALLB_WR ? 1u : 0u) << k->ndv);
                k->ndv++;
            }
    }
    return &k->m;
}

/* the call is over (the caller runs again): the answer into the caller's message, the record free */
KMMU KHOT static void mmu_unrec_(struct umsg *m)
{
    struct krec *k = (struct krec *)m;
    struct umsg *um = k->um;
    um->result = m->result;
    for (int i = 0; i < 4; i++)
        um->a[i] = m->a[i];
    if (m->kflags & UMSG_KERNEL)
        um->kflags = m->kflags;         /* the kernel's own message: UMSG_NETREPLY (sock.c, drv.c) */
    k->um = 0;
    if (k->heap)
        kfree(k);
}


/* the bounce pages' copies and clears (D60 stage 3: the hot part of a window on an unaligned buffer): 32 bytes a
 * MOVEM pair when both ends share their alignment (a bounce page's offset is the buffer's: they always do) */
void mmu_fcpy(void *d, const void *s, u32 n);
void mmu_fzero(void *d, u32 n);
__asm__(
    "    .pushsection .zhwmmu.text,\"ax\",@progbits\n"
    "mmu_fcpy:\n"
    "    move.l  4(%sp),%a1\n"
    "    move.l  8(%sp),%a0\n"
    "    move.l  12(%sp),%d0\n"
    "    move.l  %a0,%d1\n"
    "    move.l  %a1,-(%sp)\n"
    "    eor.l   %d1,(%sp)\n"
    "    move.l  (%sp)+,%d1\n"
    "    and.w   #3,%d1\n"
    "    bne.s   8f\n"                   /* never the same alignment: bytes */
    "1:  move.l  %a0,%d1\n"
    "    and.w   #3,%d1\n"
    "    beq.s   2f\n"
    "    subq.l  #1,%d0\n"
    "    bcs.s   9f\n"
    "    move.b  (%a0)+,(%a1)+\n"
    "    bra.s   1b\n"
    "2:  move.l  %d0,%d1\n"
    "    lsr.l   #5,%d1\n"
    "    beq.s   4f\n"
    "    movem.l %d2-%d7/%a2-%a3,-(%sp)\n"
    "3:  movem.l (%a0)+,%d2-%d7/%a2-%a3\n"
    "    movem.l %d2-%d7/%a2-%a3,(%a1)\n"
    "    lea     32(%a1),%a1\n"
    "    subq.l  #1,%d1\n"
    "    bne.s   3b\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a3\n"
    "4:  and.l   #31,%d0\n"
    "    bra.s   6f\n"
    "5:  move.l  (%a0)+,(%a1)+\n"
    "    subq.l  #4,%d0\n"
    "6:  cmp.l   #4,%d0\n"
    "    bcc.s   5b\n"
    "8:  subq.l  #1,%d0\n"
    "    bcs.s   9f\n"
    "    move.b  (%a0)+,(%a1)+\n"
    "    bra.s   8b\n"
    "9:  rts\n"
    "mmu_fzero:\n"
    "    move.l  4(%sp),%a1\n"
    "    move.l  8(%sp),%d0\n"
    "1:  move.l  %a1,%d1\n"
    "    and.w   #3,%d1\n"
    "    beq.s   2f\n"
    "    subq.l  #1,%d0\n"
    "    bcs.s   9f\n"
    "    clr.b   (%a1)+\n"
    "    bra.s   1b\n"
    "2:  move.l  %d0,%d1\n"
    "    lsr.l   #5,%d1\n"
    "    beq.s   4f\n"
    "    movem.l %d2-%d7/%a2-%a3,-(%sp)\n"
    "    moveq   #0,%d2\n"
    "    moveq   #0,%d3\n"
    "    moveq   #0,%d4\n"
    "    moveq   #0,%d5\n"
    "    moveq   #0,%d6\n"
    "    moveq   #0,%d7\n"
    "    move.l  %d2,%a2\n"
    "    move.l  %d2,%a3\n"
    "3:  movem.l %d2-%d7/%a2-%a3,(%a1)\n"
    "    lea     32(%a1),%a1\n"
    "    subq.l  #1,%d1\n"
    "    bne.s   3b\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a3\n"
    "4:  and.l   #31,%d0\n"
    "    bra.s   6f\n"
    "5:  clr.l   (%a1)+\n"
    "    subq.l  #4,%d0\n"
    "6:  cmp.l   #4,%d0\n"
    "    bcc.s   5b\n"
    "8:  subq.l  #1,%d0\n"
    "    bcs.s   9f\n"
    "    clr.b   (%a1)+\n"
    "    bra.s   8b\n"
    "9:  rts\n"
    "    .popsection\n");
/* ---- bounce pages (5.3): one per (server, page), shared by that server's windows there. A page in the pool (4 kept)
 * may hold an earlier window's bytes: a new one is cleared around its window's bytes when it is taken, and only
 * then mapped; one freed to the kernel is cleared first ---- */
KMMU static void bnc_unmap(struct bnc *b)
{
    struct tree *t = &trees[b->srv];
    if (t->root && dget(t->root, b->lp) == (b->pp | D_PAGE | cmode(b->pp) | D_VALID | (b->wr ? 0 : D_WP))) {
        dkeep = 1;                      /* (the tables stay: the server's next window there needs them again) */
        dset(t->root, b->lp, 0);
        dkeep = 0;
    }
}

KMMU static int bnc_map(struct bnc *b)
{
    struct tree *t = &trees[b->srv];
    return dset(t->root, b->lp, b->pp | D_PAGE | cmode(b->pp) | D_VALID | (b->wr ? 0 : D_WP));
}

/* the client's bytes [lo, hi) into bounce page b, but for those another live window of server si covers there (the
 * window being made is not in the table's count yet: w->nb is 0 and it has no bounce page of this page) */
static struct win *wins;
KMMU static void bnc_in(const struct bnc *b, int si, u32 lo, u32 hi)
{
    while (lo < hi) {
        u32 e = hi;
        for (int i = 0; i < NWIN; i++) {
            const struct win *w = &wins[i];
            int has = 0;
            for (int j = 0; j < w->nb; j++)
                has |= &bncs[w->b[j]] == b;
            if (!w->k || w->srv != si || !has)
                continue;
            if (w->a <= lo && w->a + w->n > lo) {
                lo = w->a + w->n;       /* covered: skip to its end */
                e = 0;
                break;
            }
            if (w->a > lo && w->a < e)
                e = w->a;
        }
        if (!e)
            continue;
        mmu_fcpy((void *)(b->pp + (lo & PGM)), (const void *)lo, e - lo);
        lo = e;
    }
}

/* the window's bytes [lo, hi) of page lp in server si's bounce page: shared or made; its index, or -1 */
KMMU static int bnc_get(int si, u32 lp, u32 lo, u32 hi, int wr)
{
    struct tree *t = &trees[si];
    int fr = -1, mine = 0;
    for (int i = 0; i < NBNC; i++) {
        struct bnc *b = &bncs[i];
        if (b->refs && b->srv == si && b->lp == lp) {
            if (wr && !b->wr) {
                b->wr = 1;
                bnc_map(b);             /* valid to valid in place: no table made, nothing can fail */
            }
            bnc_in(b, si, lo, hi);      /* (not over bytes another window of the server holds: its answer, unsent) */
            b->refs++;
            return i;
        }
        if (!b->refs && fr < 0)
            fr = i;
        if (b->refs && b->srv == si)
            mine++;
    }
    u32 d = dget(t->root, lp);
    if (fr < 0 || mine >= BNC_SRV || (d & D_VALID) || (!svc_any() && nbnc_all >= NBNC - RES_BNC))
        return -1;                      /* (a page valid in the tree for another reason: refused, never covered) */
    u32 pp = nbpool ? bpool[--nbpool] : 0;
    if (!pp) {
        PF(pf_bn++;)
        void *n = mem_alloc(MMU_PG, MEM_USER, 0);   /* page-aligned (protection is on), fast RAM first; the kernel's */
        if (!n)
            return -1;
        pp = (u32)n;
    }
    struct bnc *b = &bncs[fr];
    b->lp = lp;
    b->pp = pp;
    b->srv = (u8)si;
    b->wr = (u8)wr;
    b->refs = 1;
    mmu_fzero((void *)pp, lo & PGM);                                /* nothing but the window's bytes */
    mmu_fcpy((void *)(pp + (lo & PGM)), (const void *)lo, hi - lo);
    mmu_fzero((void *)(pp + (hi - lp)), MMU_PG - (hi - lp));
    nbnc_all++;
    if (bnc_map(b)) {
        nbnc_all--;
        b->refs = 0;
        bpool[nbpool++] = pp;           /* (nbpool < NBPOOL: one was just taken or none kept) */
        return -1;
    }
    return fr;
}

/* a window lets go of bounce page i: its bytes [lo, hi) back to the client first when back, then cleared while
 * another window of the server still maps the page; the last one unmaps it (into the pool as it is: cleared when it is
 * taken again, or before it goes back to the kernel) */
KMMU static void bnc_put(int i, u32 lo, u32 hi, int back)
{
    struct bnc *b = &bncs[i];
    void *in = (void *)(b->pp + (lo & PGM));
    if (back)
        mmu_fcpy((void *)lo, in, hi - lo);
    if (--b->refs)
        return;                         /* (its bytes stay until the page's last window ends: the same client's, the
                                         * same server; another window's may lie among them) */
    bnc_unmap(b);
    nbnc_all--;
    if (nbpool < NBPOOL)
        bpool[nbpool++] = b->pp;
    else {
        mmu_fzero((void *)b->pp, MMU_PG);   /* the server could write all of it: nothing of it shown again */
        mem_free((void *)b->pp);
    }
}

/* ---- windows (5.3) ---- */
/* the part of page p that window w covers */
KMMU static void wpart(const struct win *w, u32 p, u32 *lo, u32 *hi)
{
    u32 e = w->a + w->n;
    *lo = p < w->a ? w->a : p;
    *hi = p + MMU_PG - 1 >= e - 1 ? e : p + MMU_PG;
}

/* a whole page of the window that is mapped as it is (not the client's code: that is always copied) */
KMMU static int wwhole(const struct win *w, u32 p)
{
    const struct proc *c = w->k->m.client;
    u32 lo, hi;
    wpart(w, p, &lo, &hi);
    return lo == p && hi - lo == MMU_PG && !(c && c->csize && ovl(p, c->code, c->csize));
}

/* a window's part page mapped as it is (pown): its first page (1) or its last (2) */
KMMU static u32 wbit(const struct win *w, u32 p) { return p == (w->a & ~PGM) ? 1u : 2u; }

/* window w's pages out of its server's tree; its bounce pages' bytes back to the client when back (an RW window the
 * server answered, not handed back to the client already: sync) */
KMMU static void win_drop(struct win *w, int back);
KMMU static void win_drop_i(struct win *w, int back)
{
    struct tree *t = &trees[w->srv];
    const struct proc *s = &procs[w->srv];
    int nb = 0;
    if (w->n)
        for (u32 p = w->a & ~PGM, last = (w->a + w->n - 1) & ~PGM;; p += MMU_PG) {
            u32 lo, hi;
            wpart(w, p, &lo, &hi);
            if (nb < w->nb && bncs[w->b[nb]].lp == p)
                bnc_put(w->b[nb++], lo, hi, back && w->wr && !w->sync);
            else if (t->root && !own_page(s, p) && (wwhole(w, p) || (w->pown & wbit(w, p))) &&
                     dget(t->root, p) == (p | D_PAGE | cmode(p) | D_VALID | (w->wr ? 0 : D_WP))) {
                dkeep = 1;              /* D60 stage 3: a server's tree keeps the tables of its clients' memory (as
                                         * stage 1 did for lends): made and freed at every request they cost more
                                         * than the request; at most a page table per 256 KB, freed with the tree */
                dset(t->root, p, 0);
                dkeep = 0;
            }
            if (p == last)
                break;
        }
    w->k->nwin--;
    w->k = 0;
    nwin_all--;
    if (!--nwins[w->srv])
        mmu_winners &= ~(1u << w->srv);
}

/* [a, a + n) of request k's client lent to server s (current, protected) for the request, read and write if wr */
KMMU __attribute__((optimize("no-function-cse"))) static int win_make_i(struct krec *k, struct proc *s, u32 a, u32 n, int wr)
{
    int si = (int)(s - procs);
    struct tree *t = &trees[si];
    if (!n || (sysm & SLOT(s)))
        return 0;                       /* (MMU=D41: a system process reaches the buffer itself, in place) */
    if (a + n <= a)
        return -EFAULT;                 /* (n > 0 here: a range that wraps or ends at 4 GB) */
    if (!t->root || k->nwin >= WIN_REQ)
        return -ENOMEM;
    struct win *w = 0;
    int own = svc_any();
    if (!own && (nwins[si] >= WIN_USER || nwin_all >= NWIN - RES_WIN))
        return -ENOMEM;                 /* (review 2: a user's servers cannot use up the windows) */
    for (int i = 0; i < NWIN && !w; i++)
        if (!wins[i].k)
            w = &wins[i];
    if (!w)
        return -ENOMEM;
    PF(pf_w++;)
    w->k = k;
    w->a = a;
    w->n = n;
    w->srv = (u8)si;
    w->wr = (u8)wr;
    w->sync = 0;
    w->nb = 0;
    w->idp = (u8)own;                   /* (s is current: give and peer run in the server) */
    w->pown = 0;
    k->nwin++;
    nwins[si]++;
    nwin_all++;
    mmu_winners |= SLOT(s);
    int e = 0;
    u32 p = a & ~PGM, last = (a + n - 1) & ~PGM;
    const struct proc *c = k->m.client;
    for (;; p += MMU_PG) {
        u32 lo, hi;
        wpart(w, p, &lo, &hi);
        int code = c && c->csize && ovl(p, c->code, c->csize);
        int whole = lo == p && hi - lo == MMU_PG && !code;
        /* wholly the client's own memory (never the server's then): a service's part page is mapped as it is too
         * (D60 stage 3, as built: it could ask for those bytes anyway; the copies were most of a confined fsd's
         * cost); a page of the kernel's or another process's is bounced */
        int cw = (whole || w->idp) && !code && c && user_ok_wr(c, p, MMU_PG);
        if (!cw && own_page(s, p)) {
            if (wr) {
                e = -EFAULT;            /* (a buffer to write in the server's own memory: not the client's) */
                break;
            }
        } else if (whole) {
            if (dget(t->root, p) & D_VALID) {
                e = -EBUSY;
                break;
            }
            if (dset(t->root, p, p | D_PAGE | cmode(p) | D_VALID | (wr ? 0 : D_WP))) {
                e = -ENOMEM;
                break;
            }
        } else if (cw) {
            u32 d = dget(t->root, p);
            if (d & D_VALID) {          /* a sibling window of the request maps it already (they end together) */
                if ((d & ~PGM) != p || (wr && (d & D_WP))) {
                    e = -EBUSY;
                    break;
                }
            } else if (dset(t->root, p, p | D_PAGE | cmode(p) | D_VALID | (wr ? 0 : D_WP))) {
                e = -ENOMEM;
                break;
            } else
                w->pown |= (u8)wbit(w, p);
        } else {
            int b = w->nb < WIN_BNC ? bnc_get(si, p, lo, hi, wr) : -1;
            if (b < 0) {
                e = -ENOMEM;
                break;
            }
            w->b[w->nb++] = (u8)b;
        }
        if (p == last)
            break;
    }
    if (e) {
        w->n = p > a ? p - a : 0;       /* undo only what was made: the pages before p */
        win_drop(w, 0);
    }
    return e;
}

#ifdef MMU_PROF
KMMU static void win_drop(struct win *w, int back)
{
    PF(u32 t0 = hr_time();)
    win_drop_i(w, back);
    PF(pf_wd += hr_time() - t0;)
}
KMMU static int win_make(struct krec *k, struct proc *s, u32 a, u32 n, int wr)
{
    PF(u32 t0 = hr_time();)
    int e = win_make_i(k, s, a, n, wr);
    PF(pf_wm += hr_time() - t0;)
    return e;
}
#endif

/* ---- the sync (5.7): a window's bounce bytes and the client's page ---- */
KMMU static void wsync(struct win *w, int out)
{
    for (int j = 0; j < w->nb; j++) {
        struct bnc *b = &bncs[w->b[j]];
        u32 lo, hi;
        wpart(w, b->lp, &lo, &hi);
        void *in = (void *)(b->pp + (lo & PGM));
        if (out)
            mmu_fcpy((void *)lo, in, hi - lo);
        else
            mmu_fcpy(in, (const void *)lo, hi - lo);
    }
    w->sync = (u8)out;
}

/* user_ok_call_*'s second half: [a, a + n) lies in a window p holds, read and write if wr (wr 2: no range, every
 * window of current's: SYS_call, whose message may name any). For current, its bounce bytes go to the client's page
 * until the system call ends (mmu_unsync_): the kernel works on the client's address, and so does any server the
 * call reaches, this server's tree aside */
KMMU static int mmu_callok_(struct proc *p, u32 a, u32 n, int wr)
{
    /* (the inbox is not accepted: a page of it passed on would show every slot to another server, 5.5) */
    int pi = (int)(p - procs), ok = 0;
    u16 s = enter();                    /* (the serial port's receive let in: no interrupt touches the windows) */
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        if (!w->k || w->srv != pi)
            continue;
        int in = wr == 2 || (a >= w->a && a + n >= a && a + n <= w->a + w->n);
        if (in && (wr == 2 || !wr || w->wr))
            ok = 1;
        if (in && p == current && w->wr && w->nb && !w->sync) {   /* every RW window it lies in (they may overlap) */
            wsync(w, 1);
            mmu_syncing |= SLOT(p);
        }
    }
    for (int i = 1; wr != 2 && !ok && i < NPROC; i++)      /* D60 stage 5: an old server's lent client (5.8) */
        if ((trees[pi].lent & (1u << i)) && (wr ? user_ok_wr(&procs[i], a, n) : user_ok_rd(&procs[i], a, n)))
            ok = 1;
    for (int i = 0; wr != 2 && !ok && i < NWIN; i++)        /* ... and a window that client holds (lent on, 5.6) */
        if (wins[i].k && wins[i].srv < NPROC && (trees[pi].lent & (1u << wins[i].srv)) && a >= wins[i].a &&
            a + n >= a && a + n <= wins[i].a + wins[i].n && (!wr || wins[i].wr))
            ok = 1;
    if (wr != 2 && !ok && (sysm & SLOT(p))) {               /* MMU=D41: a system process, any process's memory */
        int u = 0;
        u32 o = mem_owner_of(a, &u);
        const struct proc *q = u && o && o < 0x80000000u ? proc_by_pid((int)o) : 0;
        ok = q && (wr ? user_ok_wr(q, a, n) : user_ok_rd(q, a, n));
    }
    for (int i = 0; wr != 2 && !ok && i < NSHMT; i++) {     /* D60 stage 3: a shared region it took */
        const struct shmt *e = &shms[i];
        if (e->size && e->taken && e->to == p->pid && a >= e->base && a + n >= a && a + n <= e->base + e->size &&
            !(wr && e->ro))
            ok = 1;
    }
    irq_restore(s);
    return wr == 2 ? 0 : ok;
}

/* the system call ends: current's windows given back to the client are its bounce pages' again */
KMMU static void mmu_unsync_(void)
{
    int pi = (int)(current - procs);
    if (!(mmu_syncing & SLOT(current)))
        return;
    u16 s = enter();
    for (int i = 0; i < NWIN; i++)
        if (wins[i].k && wins[i].srv == pi && wins[i].sync)
            wsync(&wins[i], 0);
    mmu_syncing &= ~SLOT(current);
    irq_restore(s);
}

/* server slot i's windows: dropped (its tree is gone: it ended, or the slot is new) */
KMMU static void wins_forget(int i)
{
    for (int j = 0; j < NWIN; j++)
        if (wins[j].k && wins[j].srv == i)
            win_drop(&wins[j], 0);
    mmu_syncing &= ~(1u << i);
    if (ibx[i]) {                       /* its pages are freed with its memory */
        kfree(ibx[i]);
        ibx[i] = 0;
    }
}

/* ---- the inbox (5.1) ---- */
KMMU KHOT static u8 *slot_get(struct proc *s, int *k)
{
    int si = (int)(s - procs);
    struct inbox *ib = ibx[si];
    if (!ib) {
        if (!(ib = kmalloc(sizeof *ib)))
            return 0;
        memset(ib, 0, sizeof *ib);
        ibx[si] = ib;
    }
    u32 have = 0;                       /* the slots of the pages it has */
    while (have < NIBPG && ib->pg[have])
        have++;
    have *= SPP;
    u32 j = ib->rr;
    for (u32 n = 0; n <= have && n < NPROC; n++, j++) {
        /* round robin among the slots it has (5.1: a slot is given again only after the others); a page more only when
         * every one is busy */
        if (j >= have)
            j = 0;
        if (n == have)
            j = have;
        if (ib->busy & (1u << j))
            continue;
        u32 *pg = &ib->pg[j / SPP];
        if (!*pg) {
            void *np = mem_alloc(MMU_PG, MEM_USER, (u32)s->pid);    /* its own memory, valid in its tree at once */
            if (!np)
                return 0;
            memset(np, 0, MMU_PG);
            *pg = (u32)np;
        }
        ib->rr = (u8)(j + 1);
        ib->busy |= 1u << j;
        ib->rec[j] = 0;
        *k = j;
        u8 *sl = (u8 *)(*pg + (u32)(j % SPP) * SLOTSZ);
        if (ib->used[j])                /* the copies of the slot's last request: nothing of it shows */
            mmu_fzero(sl + SLOTHDR, (u32)ib->used[j] * 4u);
        ib->used[j] = 0;
        return sl;
    }
    return 0;
}

KMMU KHOT static void slot_put(struct krec *k, u32 used)
{
    struct inbox *ib = ibx[k->srv];
    u32 j = k->sj;
    if (ib && ib->pg[j / SPP] && k->slot == ib->pg[j / SPP] + (j % SPP) * SLOTSZ) {   /* (gone with its server: no) */
        ib->busy &= ~(1u << j);
        ib->rec[j] = 0;
        ib->used[j] = (u8)((used + 3) / 4);
    }
    k->slot = 0;
}

/* the end of the copies in a slot (the last one's end) */
KMMU KHOT static u32 slot_used(const struct krec *k)
{
    u32 u = 0;
    for (int i = 0; i < k->ncopy; i++)
        if (k->c[i].off + k->c[i].len > u)
            u = k->c[i].off + k->c[i].len;
    return u > SLOTHDR ? u - SLOTHDR : 0;
}

/* a small buffer [at, at + n) copied into the slot; *ptr (in the slot) pointed at the copy */
KMMU KHOT static int kcopy(struct krec *k, u8 *sl, u32 *used, u32 *ptr, u32 at, u32 n, int wr)
{
    u32 off = (*used + 3) & ~3u;
    if (k->ncopy >= NKCOPY || n > SLOTSZ || off + n > SLOTSZ)
        return -ENOMEM;
    mmu_fcpy(sl + off, (const void *)at, n);
    struct kcopy *c = &k->c[k->ncopy++];
    c->at = at;
    c->len = (u16)n;
    c->off = (u16)off;
    c->wr = (u8)wr;
    *ptr = (u32)sl + off;
    *used = off + n;
    return 0;
}

/* the buffers of the kernel's requests, per FS_* type (fsproto.h's comments): a kind for each of a[0..3] */
enum { K_NONE, K_PATH, K_STAT, K_BUFW, K_BUFR, K_DIRENT, K_U32, K_STATFS, K_FSINFO, K_RDX, K_RDXBUF, K_BMAP };
#define FK(a0, a1, a2, a3) ((a0) | (a1) << 4 | (a2) << 8 | (a3) << 12)
#define NFSDECL 29
static const u16 fsdecl[NFSDECL] = {      /* (resident: the span's data holds only mmu_ops_on) */
    [FS_OPEN] = FK(K_PATH, 0, 0, K_STAT), [FS_READ] = FK(0, 0, K_BUFW, 0), [FS_WRITE] = FK(0, 0, K_BUFR, 0),
    [FS_STAT] = FK(K_PATH, 0, K_STAT, 0), [FS_FSTAT] = FK(0, K_STAT, 0, 0), [FS_READDIR] = FK(0, 0, K_DIRENT, K_U32),
    [FS_CHDIR] = K_PATH, [FS_MKDIR] = K_PATH, [FS_UNLINK] = K_PATH, [FS_RMDIR] = K_PATH,
    [FS_RENAME] = FK(K_PATH, K_PATH, 0, 0), [FS_STATFS] = K_STATFS, [FS_CHMOD] = K_PATH,
    [FS_LINK] = FK(K_PATH, K_PATH, 0, 0), [FS_SYMLINK] = FK(K_PATH, K_PATH, 0, 0),
    [FS_READLINK] = FK(K_PATH, 0, K_BUFW, 0), [FS_UTIMES] = K_PATH, [FS_LSTAT] = FK(K_PATH, 0, K_STAT, 0),
    [FS_CHOWN] = K_PATH, [FS_ROOT] = K_FSINFO, [FS_READDIRS] = FK(0, 0, K_RDXBUF, K_RDX),
    [FS_BMAP] = FK(0, 0, K_BMAP, 0),
};
static const u8 ksize[] = {
    [K_STAT] = USTAT_FIRST, [K_DIRENT] = UDIRENT_FIRST, [K_U32] = 4, [K_STATFS] = USTATFS_FIRST,
    [K_FSINFO] = sizeof(struct fsinfo), [K_RDX] = sizeof(struct fsrdx),
};

/* request k's buffers (the kernel's: KP_FS, KP_NET) for server s: copies into slot sl, windows when win */
KMMU __attribute__((optimize("O2", "no-function-cse"))) static int declare(struct krec *k, struct proc *s, u8 *sl, u32 *used,
                                                                    int win)
{
    struct umsg *d = (struct umsg *)sl;
    u32 type = k->m.type;
    int e = 0;
    if (k->kp == KP_FS) {
        u32 spec = type < NFSDECL ? fsdecl[type] : 0;
        for (int i = 0; i < 4 && !e; i++, spec >>= 4) {
            u32 kind = spec & 15, v = k->orig[i];
            if (!kind || !v)
                continue;
            if (kind == K_PATH) {
                int rw = (k->m.kflags & UMSG_LINKS) && fsl_ok(type, i == 1 ? FSL_A1 : 0);
                if (rw) {
                    e = kcopy(k, sl, used, &d->a[i], v, FS_PATHMAX, 1);   /* D26: a link's text may come back */
                } else {                /* up to its 0 (copied while it is looked for) */
                    u32 off = (*used + 3) & ~3u, n = 0;
                    const char *f = (const char *)v;
                    char *t = (char *)sl + off;
                    if (k->ncopy >= NKCOPY || off + FS_PATHMAX > SLOTSZ)
                        return -ENOMEM;
                    while (n < FS_PATHMAX && (t[n] = f[n]) != 0)
                        n++;
                    n += n < FS_PATHMAX;
                    struct kcopy *c = &k->c[k->ncopy++];
                    c->at = v;
                    c->len = (u16)n;
                    c->off = (u16)off;
                    c->wr = 0;
                    d->a[i] = (u32)t;
                    *used = off + n;
                }
                k->rw |= (u8)(1u << i);
            } else if (kind == K_BUFW || kind == K_BUFR || kind == K_BMAP || kind == K_RDXBUF) {
                u32 n = kind == K_BMAP ? (k->orig[3] < 0x10000000u ? k->orig[3] * 8 : 0xFFFFFFFFu) :
                        kind == K_RDXBUF ? (k->orig[3] ? ((const struct fsrdx *)k->orig[3])->size : 0) : k->orig[3];
                if (win)
                    e = win_make(k, s, v, n, kind != K_BUFR);
            } else {
                e = kcopy(k, sl, used, &d->a[i], v, ksize[kind], 1);
                k->rw |= (u8)(1u << i);
            }
        }
        return e;
    }
    if (type >= NETDRV_ATTACH)          /* a driver's block (drv.c), in the driver's memory: netd reads and writes it */
        return win ? win_make(k, s, k->orig[1], k->orig[2], 1) : 0;
    const struct netreq *q = (const struct netreq *)k->orig[1];
    if (!q)
        return 0;
    if ((e = kcopy(k, sl, used, &d->a[1], (u32)q, sizeof *q, 0)))
        return e;
    k->rw |= 2;
    struct netreq *c = (struct netreq *)d->a[1];
    u8 dir = type <= SOCK_OPS ? sock_dirs[type] : 0;
    if ((dir & (D_ADDRR | D_ADDRW)) && q->r.addr && q->alen)
        e = kcopy(k, sl, used, &c->r.addr, q->r.addr, q->alen, (dir & D_ADDRW) != 0);
    if (!e && (dir & D_ADDRW) && q->r.alen)
        e = kcopy(k, sl, used, &c->r.alen, q->r.alen, 4, 1);
    if (!e && (dir & (D_VALR | D_VALW)) && q->r.val && q->vlen)
        e = kcopy(k, sl, used, &c->r.val, q->r.val, q->vlen, (dir & D_VALW) != 0);
    if (!e && (dir & D_VALW) && q->r.vlen && type != SOCK_IOCTL)
        e = kcopy(k, sl, used, &c->r.vlen, q->r.vlen, 4, 1);
    if (!e && win && (dir & (D_BUFR | D_BUFW)) && q->r.buf && q->len)
        e = win_make(k, s, q->r.buf, q->len, (dir & D_BUFW) != 0);
    return e;
}

KMMU static void rec_undo(struct krec *k)
{
    for (int j = 0; k->nwin && j < NWIN; j++)
        if (wins[j].k == k)
            win_drop(&wins[j], k->done);
    slot_put(k, slot_used(k));
}

/* D60 5.8: may server s see client c's whole view (D41's lend)? SYS_debug's rule (dbg.c's may()): never init, a
 * process without a parent, a priv one (the disk service) or one ending; root may see the others; any other user only
 * a client of the same real and effective user that is not set-user-id and not NODEBUG */
KMMU static int may_see(const struct proc *s, const struct proc *c)
{
    if (c->pid <= 1 || !c->ppid || c->priv || c->exiting || c->state == P_ZOMBIE)
        return 0;
    return s->uid == 0 || (s->uid == c->uid && s->ruid == c->ruid && c->uid == c->ruid && !(c->dbgf & DBGF_NODEBUG));
}

/* D41 8's lend, for a program's request to a protected server (D60 5.8): c's whole view in s's tree */
KMMU static void lend_old(struct tree *t, struct proc *c)
{
    if (c->exiting || (t->lent & SLOT(c)))
        return;                         /* (an ending client's calls: its memory is about to go) */
    lendin(t, c);
}

/* ipc_recv gave request k to current: its copy in current's inbox (*slot), the buffers copied and lent. 0, or the
 * error the request is answered with (-ENOMEM: no slot, copy or window; -EACCES: a program's request to a protected
 * server that could not debug its client) */
KMMU KHOT static int mmu_give_i(struct krec *k, u32 *out)
{
    struct proc *s = current, *c = k->m.client;
    int si = (int)(s - procs), j;
    u8 *sl = slot_get(s, &j);
    if (!sl)
        return -ENOMEM;
    struct umsg *d = (struct umsg *)sl;
    *d = k->m;                          /* the public fields; the kernel's own written 0 (5.1) */
    d->next = 0;
    d->client = 0;
    d->server = 0;
    if (k->m.kflags & UMSG_GROUPS)
        *(struct umsg_groups *)(d + 1) = k->g;
    for (int i = 0; i < 4; i++)
        k->orig[i] = k->m.a[i];
    k->slot = (u32)sl;
    k->srv = (u8)si;
    k->sj = (u8)j;
    ibx[si]->rec[j] = k;
    k->ncopy = k->nwin = k->rw = k->done = 0;
    int e = 0;
    struct tree *t = &trees[si];
    int ex = k->ex && !(mmu_d41 && c && may_see(s, c));    /* (MMU=D41: D41's whole-client lend where the debug
                                         * rule allows it; a marked port's declared buffers otherwise) */
    int may = !c || ex || (!mmu_strict && may_see(s, c));      /* (MMU=STRICT: no old server's lend, 5.8) */
    if (!k->kp && !ex && c && may && (t->lent & SLOT(c))) {  /* (an old server's common case: the client's view still
                                                         * lent; the rule asked again, the ids may have changed since) */
        *out = (u32)sl;
        return 0;
    }
    u16 sr = enter();
    if (!k->kp) {
        if (sysm & SLOT(s))
            ;                           /* (MMU=D41: a system server sees all memory) */
        else if (c && !t->root)
            e = -ENOMEM;                /* (no tree: never given unchecked, build() would lend it) */
        else if (c && ex) {
            /* D60 5.5 (a server that asks for what it touches): exactly the buffers the client declared */
            if (t->lent & SLOT(c))
                unlend(t, s, c);        /* (a lend kept from before the port was marked) */
            for (u32 i = 0; i < k->ndv && !e; i++)
                e = win_make(k, s, k->dva[i], k->dvn[i], (k->dvw >> i) & 1);
        } else if (c) {
            /* D60 5.8 (an old server: its port is not PEER_EXACT): the whole client only when SYS_debug's rule would
             * let the server debug it: root, or the same user with a client that is not set-user-id and not NODEBUG */
            if (may)
                lend_old(t, c);
            else {
                if (t->lent & SLOT(c))
                    unlend(t, s, c);    /* (a lend kept from before the client's ids changed) */
                e = -EACCES;
            }
        }
    } else {
        u32 used = SLOTHDR;
        if (c && (t->lent & SLOT(c)))
            unlend(t, s, c);            /* a kernel request: exactly its buffers, not the client's last lend */
        e = declare(k, s, sl, &used, 1);
    }
    if (e)
        rec_undo(k);
    leave(sr);
    if (e == -EACCES && !eacces_said) {
        eacces_said = 1;
        kprintf("[kernel] mmu: %s (pid %d) may not see the memory of pid %d (%s): its request refused (D60 5.8%s; said "
                "once)\n", s->name, s->pid, c->pid, c->name, mmu_strict ? ", MMU=STRICT" : "");
    }
    *out = (u32)sl;
    return e;
}

/* the server's reply to k (ipc_reply, before ipc_finish): a[] from its copy (an argument the kernel rewrote gets its
 * value back), the copies it may write back to the client */
KMMU KHOT static void mmu_take_(struct krec *k)
{
    const struct umsg *d = (const struct umsg *)k->slot;
    for (int i = 0; i < 4; i++)
        k->m.a[i] = (k->rw & (1u << i)) ? k->orig[i] : d->a[i];
    for (int i = 0; i < k->ncopy; i++)
        if (k->c[i].wr)
            mmu_fcpy((void *)k->c[i].at, (const u8 *)k->slot + k->c[i].off, k->c[i].len);
    k->done = 1;
}

/* ipc_rec: the record whose copy m is, if m is a slot of current's inbox in use (0 for anything else: a stray or forged
 * pointer, a slot answered already) */
KMMU KHOT static struct umsg *mmu_held_(struct umsg *m)
{
    struct inbox *ib = ibx[current - procs];
    u32 a = (u32)m;
    for (u32 i = 0; ib && i < NIBPG && ib->pg[i]; i++)
        if (a - ib->pg[i] < MMU_PG) {
            u32 j = i * SPP + (a - ib->pg[i]) / SLOTSZ;
            struct krec *k = ib->rec[j];
            return (a & (SLOTSZ - 1)) || !k || k->slot != a ? 0 : &k->m;
        }
    return 0;
}

/* k ends (ipc_finish: the reply, the server's end, a refusal): its windows go (an answered RW window's bounce bytes
 * back to the client first), its slot is free */
KMMU KHOT static void mmu_end_i(struct krec *k)
{
    if (!k->nwin)
        return slot_put(k, slot_used(k));
    u16 sr = enter();
    rec_undo(k);
    leave(sr);
}

/* ======== D60 stage 3 (docs/d60-full-protection.md 3.1, 5.5, 6.2, 7.4): every process confined ========
 * Beside its view and the windows of the requests it holds, a process's tree has its grants: the register pages of the
 * device it claimed (inhibited, whatever the address), its claim's DMA memory, and the shared regions it took. A
 * program's request lends nothing but what its client declared (SYS_callv) and what a server trusted to ask asks for
 * (SYS_peer: a service the service manager started, or a server that could debug the client); a port not marked
 * PEER_EXACT is an old server's, which gets D41's lend by SYS_debug's rule (5.8). */
const struct ucalldecl *mmu_decl;       /* (resident: set by syscall.c's SYS_callv for the call it makes) */

KMMU static int gflip(const struct grant *g, int in)
{
    struct tree *t = &trees[g->slot];
    return t->root ? tflip(t->root, g->lo, g->hi, in ? D_VALID | g->how : TIDY) : 0;
}

KMMU static int mmu_grant_(struct proc *p, u32 lo, u32 hi, u32 how)
{
    int pi = (int)(p - procs);
    struct grant *g = 0;
    lo &= ~PGM;
    hi = (hi + PGM) & ~PGM;
    for (int i = 0; i < NGRANT && !g; i++)  /* (a second grant of the same pages, a Paula claim and a grab: an entry
                                             * each, the pages out only with the last) */
        if (!grants[i].hi)
            g = &grants[i];
    if (!g || hi <= lo)
        return -ENOMEM;
    g->lo = lo;
    g->hi = hi;
    g->slot = (u8)pi;
    g->how = (u8)how;
    u16 s = enter();
    int e = gflip(g, 1);
    if (e) {
        gflip(g, 0);                    /* not kept half made */
        g->hi = 0;
    }
    if (trees[pi].root == loaded)
        fl_n |= 1;                      /* (a 040's prefetch may have left an invalid instruction entry: as mmu_block_) */
    leave(s);
    return e ? -ENOMEM : 0;
}

KMMU static void mmu_ungrant_(struct proc *p, u32 lo)
{
    int pi = (int)(p - procs);
    lo &= ~PGM;
    u16 s = enter();
    struct grant *g = 0;
    int more = 0;
    for (int i = 0; i < NGRANT; i++)
        if (grants[i].hi && grants[i].slot == pi && grants[i].lo == lo) {
            if (g)
                more = 1;
            else
                g = &grants[i];
        }
    if (g) {
        if (!more)
            gflip(g, 0);
        g->hi = 0;
    }
    leave(s);
}

/* SYS_peer PEER_RD / PEER_WR by current, serving request k, for [a, a + n) of its client (ipc.c checked that the range
 * is the client's memory or a window it holds): 0 if current may touch it now. Within a window of the request (the
 * client's declaration, an earlier ask) or an old server's lend: at once. Else only a server trusted to ask (a service,
 * or one that could debug the client) gets it lent, until the request ends; any other server -EFAULT (5.5) */
KMMU static int wover(const struct win *w, u32 a, u32 n)       /* w and [a, a + n) share a page */
{
    return (a & ~PGM) <= ((w->a + w->n - 1) & ~PGM) && (w->a & ~PGM) <= ((a + n - 1) & ~PGM);
}
KMMU static long mmu_peer_(struct krec *k, u32 a, u32 n, int wr)
{
    struct proc *s = current, *c = k->m.client;
    int si = (int)(s - procs);
    if (!n || (sysm & SLOT(s)))
        return 0;                       /* (MMU=D41: a system process sees it already) */
    if (!c || (trees[si].lent & SLOT(c)))
        return 0;                       /* (an old server's whole-client lend) */
    for (int i = 0; i < NWIN; i++) {
        const struct win *w = &wins[i];
        if (w->k == k && w->srv == si && a >= w->a && a + n <= w->a + w->n && (w->wr || !wr))
            return 0;
    }
    if (!svc_any() && !may_see(s, c))
        return -EFAULT;                 /* a server any user runs: only what its client declared */
    if ((u32)k->um < a + n && a < (u32)k->um + sizeof(struct umsg))
        return -EFAULT;                 /* never the client's message (5.5: the server has its copy) */
    u16 sr = enter();
    int e = win_make(k, s, a, n, wr);
    if (e == -EBUSY || e == -ENOMEM) {
        /* a page of it is already the request's (an earlier window): the windows sharing a page with it and it made
         * one (their bytes back to the client first) */
        u32 lo = a, hi = a + n;
        int any = 0, more = 1, w2 = wr;
        while (more) {                  /* the union first, nothing dropped yet */
            more = 0;
            for (int i = 0; i < NWIN; i++) {
                const struct win *w = &wins[i];
                if (w->k == k && w->srv == si && wover(w, lo, hi - lo) && (w->a < lo || w->a + w->n > hi || (w->wr && !w2))) {
                    if (w->a < lo)
                        lo = w->a;
                    if (w->a + w->n > hi)
                        hi = w->a + w->n;
                    w2 |= w->wr;
                    more = 1;
                }
                if (w->k == k && w->srv == si && wover(w, lo, hi - lo))
                    any = 1;
            }
        }
        /* the union too must be the client's (never a gap of the kernel's between two of its buffers), and written
         * only where it may be */
        if (any && (w2 ? user_ok_call_wr(c, lo, hi - lo) : user_ok_call_rd(c, lo, hi - lo))) {
            for (int i = 0; i < NWIN; i++)
                if (wins[i].k == k && wins[i].srv == si && wover(&wins[i], lo, hi - lo))
                    win_drop(&wins[i], 1);
            e = win_make(k, s, lo, hi - lo, w2);
        } else if (any)
            e = -EFAULT;
    }
    leave(sr);
    return e;
}

/* ---- SYS_shm (7.4) ---- */
KMMU static struct shmt *shm_find(u32 a, int pid, int as_to)
{
    for (int i = 0; i < NSHMT; i++) {
        struct shmt *e = &shms[i];
        if (e->size && a - e->base < e->size && (as_to ? e->to == pid : e->owner == pid))
            return e;
    }
    return 0;
}

/* the region at base goes from its owner to a holder (the owner freed it or ended): 1, or 0 when nobody holds it (its
 * offers withdrawn: the caller frees it) */
KMMU static int shm_handover(u32 base, int from)
{
    struct shmt *h = 0;
    for (int i = 0; i < NSHMT; i++) {
        struct shmt *e = &shms[i];
        if (e->size && e->base == base && e->owner == from) {
            if (!e->taken || e->to == from)
                e->size = 0;            /* an offer not taken is withdrawn */
            else if (!h)
                h = e;
        }
    }
    if (!h)
        return 0;
    for (int i = 0; i < NSHMT; i++)
        if (shms[i].size && shms[i].base == base)
            shms[i].owner = h->to;
    if (!h->claim)
        mem_set_owner((void *)base, (u32)h->to);    /* counted to the holder now, freed at its end */
    return 1;
}

/* a process ends (mmu_forget_): its shares go, the regions it owned go to a holder */
KMMU static void shm_exit(int pid)
{
    for (int i = 0; i < NSHMT; i++)     /* (a region handed to it: on to another holder below, or freed with its memory) */
        if (shms[i].size && shms[i].to == pid)
            shms[i].size = 0;
    for (int i = 0; i < NSHMT; i++)
        if (shms[i].size && shms[i].owner == pid) {
            u32 b = shms[i].base;
            if (!shm_handover(b, pid))
                for (int j = 0; j < NSHMT; j++)
                    if (shms[j].base == b)
                        shms[j].size = 0;       /* nobody holds it: freed with the process's memory */
        }
}

/* SYS_memfree of a region the caller owns: 1 if a holder keeps it (the caller's view loses it, the memory stays) */
KMMU static int mmu_shmfree_(u32 base)
{
    int pid = current->pid;
    if (!shm_find(base, pid, 0))
        return 0;
    u16 s = enter();
    int kept = shm_handover(base, pid);
    if (kept && (lentany & SLOT(current))) {    /* an old server's lend of our view: back first (it lists the region) */
        lentany &= ~SLOT(current);
        for (int i = 1; i < NPROC; i++)
            if (trees[i].lent & SLOT(current))
                unlend(&trees[i], &procs[i], current);
    }
    if (kept) {
        struct tree *t = &trees[current - procs];
        for (int i = 0; i < NREG; i++)
            if (current->reg[i].base == base && t->root)
                tflip(t->root, base, base + current->reg[i].size, TIDY);
    }
    leave(s);
    return kept;
}

KMMU static long mmu_shm_(u32 op, u32 a, u32 arg)
{
    struct proc *p = current;
    int pid = p->pid;
    struct shmt *e = 0;
    if (op == SHM_OFFER) {
        u32 size = 0;
        int claim = 0, n = 0, nt = 0, all = 0, to = (int)(arg & ~SHMF_RO);
        for (int i = 0; i < NREG && a; i++)
            if (p->reg[i].base == a)
                size = p->reg[i].size;
        if (!size && drv_dmamem(a, &size))
            claim = 1;
        if (!size)
            return -EINVAL;             /* not a region of the caller's, nor its claim's DMA memory */
        if (gcon_uses(a, size))
            return -EBUSY;              /* (review 2: the grab's Copper list or sprite: as SYS_memfree refuses it) */
        struct proc *q = proc_by_pid(to);
        if (!q || q == p || q->exiting || q->state == P_ZOMBIE)
            return -ESRCH;
        for (int i = 0; i < NSHMT; i++) {
            struct shmt *x = &shms[i];
            if (!x->size) {
                if (!e)
                    e = x;
            } else {
                n += x->owner == pid;
                nt += x->to == to;
                all++;
                if (x->owner == pid && x->base == a && x->to == to)
                    return -EEXIST;
            }
        }
        if (!e || n >= NSHM || nt >= NSHM || (!sysown() && all >= NSHMT - 8))
            return -ENOMEM;             /* (8 offers made and 8 received a process; 8 entries kept for the system's) */
        e->base = a;
        e->size = size;
        e->owner = pid;
        e->to = to;
        e->ro = (arg & SHMF_RO) != 0;
        e->taken = 0;
        e->claim = (u8)claim;
        return 0;
    }
    if (op == SHM_TAKE) {
        int n = 0;
        for (int i = 0; i < NSHMT; i++)
            if (shms[i].size && shms[i].to == pid) {
                n += shms[i].taken;
                if (!shms[i].taken && a - shms[i].base < shms[i].size)
                    e = &shms[i];
            }
        if (!e)
            return -ENOENT;
        int used = 0;
        for (int i = 0; i < NGRANT; i++)
            used += grants[i].hi != 0;
        if (n >= NSHM || (!sysown() && used >= NGRANT - 12))
            return -ENOMEM;             /* (12 grants kept for the system's: claims, the display's grab, netd's pool) */
        if (mmu_grant_(p, e->base, e->base + e->size, e->ro ? G_RO : 0))
            return -ENOMEM;
        e->taken = 1;
        return 0;
    }
    if (op == SHM_DROP) {
        if ((e = shm_find(a, pid, 1)) != 0) {           /* a holder lets go */
            u32 b = e->base;
            if (e->taken)
                mmu_ungrant_(p, b);
            e->size = 0;
            if (e->owner == pid) {                       /* it was handed the region: on to another, or freed */
                int claim = e->claim;
                if (!shm_handover(b, pid)) {
                    for (int j = 0; j < NSHMT; j++)
                        if (shms[j].base == b)
                            shms[j].size = 0;
                    if (!claim)
                        mem_free((void *)b);    /* (a claim's DMA memory is drv.c's to free, at the driver's reap) */
                }
            }
            return 0;
        }
        if (!shm_find(a, pid, 0))
            return -EINVAL;
        for (int i = 0; i < NSHMT; i++)                 /* the owner: its offers not taken withdrawn */
            if (shms[i].size && shms[i].owner == pid && a - shms[i].base < shms[i].size && !shms[i].taken)
                shms[i].size = 0;
        return 0;
    }
    if (op == SHM_INFO) {
        if (!(e = shm_find(a, pid, 1)) && !(e = shm_find(a, pid, 0)))
            return -EINVAL;
        if (!user_ok_wr(p, arg, sizeof(struct ushm)))
            return -EFAULT;
        struct ushm *u = (struct ushm *)arg;
        u->size = e->size;
        u->owner = (u32)e->owner;
        u->holders = 0;
        for (int i = 0; i < NSHMT; i++)
            if (shms[i].size && shms[i].base == e->base && shms[i].taken)
                u->holders++;
        u->flags = e->ro ? SHMF_RO : 0;
        return 0;
    }
    return -EINVAL;
}

/* SYS_callv (5.5) with protection on: the list checked (each buffer the caller's memory or a window it holds, never
 * its message), then SYS_call with the list recorded (mmu_rec_) */
KMMU static long mmu_callv_(u32 port, u32 msg, u32 list)
{
    struct ucalldecl d;
    struct proc *p = current;
    if (!user_ok_wr(p, msg, sizeof(struct umsg)))
        return -EFAULT;
    if (mmu_winners & SLOT(p))
        mmu_callok_(p, 0, 0, 2);        /* (as SYS_call: its windows' copy pages are the clients' again) */
    if (!user_ok_call_rd(p, list, 4))
        return -EFAULT;
    d.n = *(const u32 *)list;
    if (d.n > NCALLB)
        return -EINVAL;
    if (d.n && !user_ok_call_rd(p, list + 4, d.n * sizeof(struct ucallbuf)))
        return -EFAULT;
    memcpy(d.b, (const void *)(list + 4), d.n * sizeof(struct ucallbuf));
    for (u32 i = 0; i < d.n; i++) {
        const struct ucallbuf *b = &d.b[i];
        if (!b->len)
            continue;
        if ((b->flags & ~(u32)(CALLB_RD | CALLB_WR)) || (b->addr < msg + sizeof(struct umsg) && msg < b->addr + b->len))
            return -EINVAL;             /* (an unknown flag; the message itself is never lent: the server has its copy) */
        if (!(b->flags & CALLB_WR ? user_ok_call_wr(p, b->addr, b->len) : user_ok_call_rd(p, b->addr, b->len)))
            return -EFAULT;             /* its own memory, or a window it holds (a buffer passed on, 5.6) */
    }
    mmu_decl = &d;
    long r = ipc_call((int)port, (struct umsg *)msg);
    mmu_decl = 0;
    return r;
}

/* SYS_blk_ram's copy (stage 3): n sectors from sector a (bit 31: write) to or from the disk server's buffer b (its
 * memory, or a window it holds): the RAM disk is never in its view (its ends share pages with the kernel's image) */
KMMU static long mmu_ramdisk_(u32 a, u32 n, u32 b)
{
    u32 lba = a & 0x7FFFFFFFu, nb = boot_ram_size / 512;
    if (n > nb || lba > nb - n)
        return -EINVAL;
    if (!(a >> 31 ? user_ok_call_rd(current, b, n * 512) : user_ok_call_wr(current, b, n * 512)))
        return -EFAULT;
    if (a >> 31)
        mmu_fcpy((void *)(boot_ram_pa + lba * 512), (const void *)b, n * 512);
    else
        mmu_fcpy((void *)b, (const void *)(boot_ram_pa + lba * 512), n * 512);
    return 0;
}

/* POST_BUF by current (D60 5.8, review 2): an old poster (its port not PEER_EXACT; a displayd built before D60 reads
 * its client's ring tail in place between requests) that could debug current gets the window's pages read only, as
 * its old lend would show them; the old window's grant goes */
KMMU static void mmu_postbuf_(struct port *of, u32 old, struct port *nf, u32 a, u32 n)
{
    if (of && of->op && old)
        mmu_ungrant_(of->op, old);
    if (nf && nf->op && !nf->exact && a && n && !mmu_strict && may_see(nf->op, current))
        mmu_grant_(nf->op, a, a + n, G_RO);
}

/* ---- the update bundle (D60 7.2): read only, for a root process that asked UPD_INFO; its whole pages as they are, its
 * part pages at the two ends onto copies holding only the bundle's bytes (no kernel byte around it shows). The copies
 * are made once, shared by every tree that has the bundle (its bytes never change), freed with it ---- */
static u32 upc[2];                      /* the copies of the first and the last part page (0: none made) */
KMMU static int upd_part(int i, u32 *pp, int mk)   /* part page i (0 the first, 1 the last): 1 (*pp), 0 none, -1 no copy */
{
    u32 lo = upd_pa, hi = upd_pa + upd_size, p = i ? (hi - 1) & ~PGM : lo & ~PGM;
    if ((i && p == (lo & ~PGM)) || (p >= lo && p + MMU_PG <= hi))
        return 0;                       /* (one page: the first is it; a whole page) */
    if (!upc[i]) {
        u8 *c = mk ? mem_alloc(MMU_PG, MEM_USER, 0) : 0;     /* page-aligned (protection is on), the kernel's */
        if (!c)
            return -1;
        u32 a = p < lo ? lo : p, e = p + MMU_PG > hi ? hi : p + MMU_PG;
        mmu_fzero(c, MMU_PG);
        mmu_fcpy(c + (a - p), (const void *)a, e - a);
        upc[i] = (u32)c;
    }
    *pp = p;
    return 1;
}
KMMU static int upd_map(u32 *r, int in)
{
    u32 lo = (upd_pa + PGM) & ~PGM, hi = (upd_pa + upd_size) & ~PGM, p = 0;
    int bad = lo < hi ? tflip(r, lo, hi, in ? D_VALID | D_WP : TIDY) : 0;
    for (int i = 0; i < 2; i++) {
        int k = upd_part(i, &p, in);
        u32 v = upc[i] | D_PAGE | cmode(upc[i]) | D_VALID | D_WP;
        if (k < 0)
            bad |= in;                  /* (taking out: no copy was made, nothing of it mapped) */
        else if (!k)
            ;
        else if (in)
            bad |= dset(r, p, v);
        else if (dget(r, p) == v)
            dset(r, p, 0);
    }
    return bad;
}
KMMU static int upd_want(const struct proc *n) { return upd_pa && n->uid == 0 && (mmu_updask & SLOT(n)); }

/* p's tree made (D60 section 4): its view, the bundle for root, the clients it serves */
KMMU static void build(struct proc *n, struct tree *t)
{
    if (!(t->root = unit(1)))
        return nomem(n);
    t->lent = 0;
    t->upd = 0;
    if (view(t->root, n, 1, 1))
        nomem(n);
    int ni = (int)(n - procs);
    struct inbox *ib = ibx[ni];         /* D60 stage 2: its inbox, and the windows of the requests it holds */
    for (u32 i = 0; ib && i < NIBPG; i++)
        if (ib->pg[i] && tflip(t->root, ib->pg[i], ib->pg[i] + MMU_PG, D_VALID))
            nomem(n);
    for (int i = 0; i < NBNC; i++)
        if (bncs[i].refs && bncs[i].srv == ni && bnc_map(&bncs[i]))
            nomem(n);
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        if (!w->k || w->srv != ni)
            continue;
        for (u32 p = w->a & ~PGM, last = (w->a + w->n - 1) & ~PGM;; p += MMU_PG) {
            int b = 0;                  /* (a page made a bounce at win_make stays one: its kind is not worked out again) */
            for (int j = 0; j < w->nb; j++)
                b |= bncs[w->b[j]].lp == p;
            if (!b && !own_page(n, p) && (wwhole(w, p) || (w->pown & wbit(w, p))) &&
                dset(t->root, p, p | D_PAGE | cmode(p) | D_VALID | (w->wr ? 0 : D_WP)))
                nomem(n);
            if (p == last)
                break;
        }
    }
    for (int i = 0; i < NGRANT; i++)    /* D60 stage 3: its grants */
        if (grants[i].hi && grants[i].slot == ni && gflip(&grants[i], 1))
            nomem(n);
    for (int i = 1; i < NPROC; i++)
        if (serves(&procs[i], n) && !((struct krec *)procs[i].wait_msg)->kp &&
            (!((struct krec *)procs[i].wait_msg)->ex || mmu_d41) && !mmu_strict && may_see(n, &procs[i]))
            lendin(t, &procs[i]);       /* (D41's lend: only a program's request to an old server, D60 5.8) */
}

/* the tree of slot i given back (the process ended); its view out of every other tree (a slot given to a new process:
 * only the marks go, its old pages being out of every tree since they were freed) */
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
            u8 u = (u8)upd_want(n);     /* D15: zup reads the bundle in place, read only; out once not root */
            if (t->upd != u) {          /* (marked in when tried: a half-made map is taken out with the rest) */
                if (upd_map(t->root, u)) {
                    nomem(n);
                    upd_map(t->root, 0);    /* (not kept half made: the next switch tries again) */
                    u = 0;
                }
                t->upd = u;
            }
        }
        sr_back(sr);
    }
}

KMMU static void mmu_switch_i(struct proc *n)
{
    if (n->pid == 0 || n->exiting)  /* idle and a process in proc_exit never run user code again */
        return;
    int i = (int)(n - procs);
    struct tree *t = &trees[i];
    if (__builtin_expect(sysm & SLOT(n), 0)) {
        if (mode != 0) {                /* MMU=D41: a system process, the windows to every access (D41 7) */
            mode = 0;
            mmu_ttr(win[0], fam);
        }
        return;
    }
    if (__builtin_expect(!t->root || t->lent || t->upd != upd_want(n), 0))
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

/* the slot is given to a new process: nothing of the slot's last tree left */
void mmu_slot_new(struct proc *p)
{
    if (mmu_on)
        mmu_ops->forget(p);
}

static void shm_exit(int pid);

/* proc_exit, at its start (before seg_put drops the code, before its memory is freed), and a slot given to a new
 * process: never seen again */
KMMU static void mmu_forget_(struct proc *p)
{
    u16 s = enter();
    int i = (int)(p - procs);
    wins_forget(i);                     /* D60: its windows */
    drop(i, 1);
    mmu_updask &= ~SLOT(p);             /* (stage 5: the bundle asked for, D41's system mark) */
    sysm &= ~SLOT(p);
    for (int j = 0; j < NGRANT; j++)    /* D60 stage 3: its grants (the tree is gone) */
        if (grants[j].slot == i)
            grants[j].hi = 0;
    if (p->exiting)
        shm_exit(p->pid);               /* what it held of others' goes, what it owned goes to a holder (7.4) */
    leave(s);
}

/* MMU=D41 (D60 12): p is D41 section 7's system process from now on: every access let through while it runs; the
 * windows it holds go (their bytes back to the client first: it reaches the buffers in place from now on) */
KMMU static void mmu_mark_(struct proc *p, int upd)
{
    u16 s = enter();
    if (upd) {                          /* D60 7.2: p (root) asked UPD_INFO: the bundle in its view at once */
        mmu_updask |= SLOT(p);
        mmu_switch_i(p);
        return leave(s);
    }
    for (int i = 0; i < NWIN; i++)
        if (wins[i].k && wins[i].srv == (u8)(p - procs))
            win_drop(&wins[i], 1);
    sysm |= SLOT(p);
    drop((int)(p - procs), 0);          /* (never loaded again: its tables back to the pool) */
    if (p == current)
        mmu_switch_i(p);
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
        if (p == current)
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
            upd_map(trees[i].root, 0);
            trees[i].upd = 0;
        }
    tflush();
    mmu_updask = 0;
    for (int i = 0; i < 2; i++)
        if (upc[i]) {
            mem_free((void *)upc[i]);
            upc[i] = 0;
        }
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
    for (int i = 0; i < NGRANT; i++)    /* D60 stage 3: grants and shares of the memory are gone with it */
        if (grants[i].hi && grants[i].lo >= lo && grants[i].hi <= hi)
            grants[i].hi = 0;
    for (int i = 0; i < NSHMT; i++)
        if (shms[i].size && shms[i].base - data < len)
            shms[i].size = 0;
    leave(s);
}

/* ---- after ZINE (D60 7.3): the kernel's code and read-only data against their sum taken at the boot, the records the
 * trees are made from checked, then every tree made again from them (the tables in RAM are not trusted) ---- */
extern const u32 _start[], __kro_end[], __zhwmmu_start[], __zhwmmu_end[];
static u32 ksum0, ksr[4];               /* the sum at the boot; its two ranges (resident, and this span past its header) */
KMMU static u32 ksum(void)
{
    u32 s = 0;
    for (int r = 0; r < 4; r += 2)
        for (const u32 *a = (const u32 *)(ksr[r] + (r ? 32 : 0)); a < (const u32 *)ksr[r + 1]; a++)
            s = (s << 1 | s >> 31) ^ *a;
    return s;
}
KMMU static int used_blk(u32 a, u32 owner)     /* a lies in a block in use (of owner, unless ~0) */
{
    int u = 0;
    u32 o = mem_owner_of(a, &u);
    return u && (owner == ~0u || o == owner);
}
KMMU static const char *zine_bad(void)
{
    if (ksum() != ksum0)
        return "code";
    if (mem_sane())
        return "blocks";
    for (int i = 1; i < NPROC; i++) {
        const struct proc *p = &procs[i];
        if (p->state == P_FREE || p->state == P_ZOMBIE || p->exiting)
            continue;                   /* (an ended one's memory is freed) */
        if (p->state > P_EV || p->pid <= 0 || (p->span && !used_blk(p->base, ~0u)) || (p->csize && !used_blk(p->code, ~0u)))
            return "slots";
        for (int r = 0; r < NREG; r++)
            if (p->reg[r].base && !used_blk(p->reg[r].base, ~0u))
                return "slots";
        for (u32 j = 0; ibx[i] && j < NIBPG; j++)
            if (ibx[i]->pg[j] && !used_blk(ibx[i]->pg[j], (u32)p->pid))
                return "slots";
    }
    for (int i = 0; i < NWIN; i++) {
        const struct win *w = &wins[i];
        if (w->k && (w->srv >= NPROC || procs[w->srv].state == P_FREE || w->nb > WIN_BNC || w->a + w->n < w->a ||
                     (((u32)w->k - (u32)recs) >= NPROC * sizeof *recs && !used_blk((u32)w->k, 0))))
            return "windows";               /* (a record: one of the slots', or a nested call's from the heap) */
        for (int j = 0; w->k && j < w->nb; j++)
            if (w->b[j] >= NBNC || !bncs[w->b[j]].refs)
                return "windows";
    }
    for (int i = 0; i < NBNC; i++)
        if (bncs[i].refs && (bncs[i].srv >= NPROC || (bncs[i].pp & PGM) || !used_blk(bncs[i].pp, 0)))
            return "windows";
    for (int i = 0; i < NGRANT; i++)    /* (a claim's registers and DMA memory, the display's grab, shared regions) */
        if (grants[i].hi && (grants[i].slot >= NPROC || grants[i].lo >= grants[i].hi))
            return "grants";
    if (npg > cpg || nbpool > NBPOOL)
        return "tables";
    for (u32 i = 0; i < npg; i++)       /* (rebuild writes into them: a page of the kernel's that is no pool page never) */
        if (((ppg[i] & ~1u) & PGM) || !used_blk(ppg[i] & ~1u, 0) || ((ppg[i] & ~1u) - (u32)krt) < 512)
            return "tables";
    for (u32 i = 0; i < nbpool; i++)
        if ((bpool[i] & PGM) || !used_blk(bpool[i], 0))
            return "windows";
    return 0;
}
/* every tree out (the pool's pages cut into free units again, the kernel's tree emptied): each is made again from the
 * records at its process's next switch (build) */
KMMU static void rebuild(void)
{
    for (int i = 0; i < NPROC; i++) {
        trees[i].root = 0;
        trees[i].lent = 0;
        trees[i].upd = 0;
    }
    lentany = 0;
    fl[0] = fl[1] = 0;
    for (u32 i = 0; i < npg; i++) {
        int k = (int)(ppg[i] & 1);
        for (u32 o = 0; o < MMU_PG; o += usz(k)) {
            u32 *u = (u32 *)((ppg[i] & ~1u) + o);
            *u = (u32)fl[k];
            fl[k] = u;
        }
    }
    memset(krt, 0, 512);
    if (dpush)
        cache_push_range((u32)krt, (u32)krt + 512);
    loaded = krt;
    fl_n = 0;
    fl_g = 0;
}

/* ZINE (D41 section 11), interrupts masked: off as D37 left the registers; back (after D60 7.3's checks and the rebuild:
 * 1 if damaged, translation left off) with the windows of the mode now and the kernel's tree, then current's */
KMMU static int mmu_hand_(int back)
{
    if (!back) {
        mmu_regs_load(&off);
        return 0;
    }
    const char *bad = zine_bad();
    if (bad) {
        kprintf("mmu: after ZINE: Zenix's memory is damaged (%s)\n", bad);
        return 1;
    }
    rebuild();
    struct mregs m = on;
    m.dtt0 = win[mode][0];
    if (fam == 2) {
        m.dtt1 = win[mode][1];
        m.itt0 = win[mode][2];
    }
    m.root = (u32)loaded;
    mmu_regs_load(&m);
    mmu_switch_i(current);              /* (its tree now: it returns to user mode without a switch) */
    if (fl_n | fl_g)
        tflush();
    return 0;
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
    const struct hwinfo *hw = HWINFO_K;
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
    /* D60 stage 2: the records, the inboxes' list, the windows and the bounce pages' tables (protection on only) */
    u32 rb = NPROC * sizeof *recs, ib = NPROC * sizeof *ibx, wb = NWIN * sizeof *wins, bb = NBNC * sizeof *bncs;
    u32 gb = NGRANT * sizeof *grants, sb = NSHMT * sizeof *shms;    /* D60 stage 3: grants and shares */
    u8 *s2 = res == 3 ? mem_alloc(rb + ib + wb + bb + gb + sb, 0, 0) : 0;
    if (!s2) {
        mmu_regs_load(&off);
        mem_free(kblk);
        mem_free(trees);
        trees = 0;
        return (void)off_why("out of memory for the tables");
    }
    memset(s2, 0, rb + ib + wb + bb + gb + sb);
    grants = (struct grant *)(s2 + rb + ib + wb + bb);
    shms = (struct shmt *)(s2 + rb + ib + wb + bb + gb);
    recs = (struct krec *)s2;
    ibx = (struct inbox **)(s2 + rb);
    wins = (struct win *)(s2 + rb + ib);
    bncs = (struct bnc *)(s2 + rb + ib + wb);
    if (mmu_regs_load(&on)) {
        mmu_regs_load(&off);
        mem_free(kblk);
        mem_free(s2);
        mem_free(trees);
        trees = 0;
        return (void)off_why("no MMU (a register is missing)");
    }
    loaded = krt;
    mode = 0;
    tbytes = 1024;
    ksr[0] = (u32)_start;               /* D60 7.3: ZINE's check of the kernel's code (the span's first 32 bytes are */
    ksr[1] = (u32)__kro_end;            /* its block header in the pool) */
    ksr[2] = (u32)__zhwmmu_start;
    ksr[3] = (u32)__zhwmmu_end;
    ksum0 = ksum();
    mmu_d41 = ((k | d) & KOPT_MMU_D41) != 0;        /* D60 12: the boot's word, or the kernel file's default */
    mmu_strict = !mmu_d41 && ((k | d) & KOPT_MMU_STRICT);
    mmu_arm(fam);                       /* the table to the span's functions, and mmu_on set: together, only here */
    kprintf("mmu: memory protection on: %s, %u KB pages, %u KB of tables (a tree per process%s)\n",
            bi->attn & 0x80 ? "68060" : bi->attn & 0x08 ? "68040" : "68030", MMU_PG / 1024, tbytes / 1024,
            mmu_d41 ? "; MMU=D41" : mmu_strict ? "; MMU=STRICT" : "");
}


#ifdef MMU_PROF
KMMU static void mmu_switch_(struct proc *n)
{
#ifdef MMU_PROF
    u32 t = hr_time(); u32 *ld = loaded;
    mmu_switch_i(n);
    pf_s += hr_time() - t; pf_ns++; if (ld != loaded) pf_r++;
#else
    mmu_switch_i(n);
#endif
}
KMMU KHOT static int mmu_give_(struct krec *k, u32 *out)
{
#ifdef MMU_PROF
    u32 t = hr_time();
    int r = mmu_give_i(k, out);
    pf_g += hr_time() - t;
    if (++pf_n % 400 == 0) {
        kprintf("prof: 400 gives %u ticks, ends %u, switches %u (%u, %u roots), flusha %u flushan %u (%u), bnc new %u, wins %u (make %u, drop %u), pushes %u (%u)\n",
                pf_g, pf_e, pf_s, pf_ns, pf_r, pf_fa, pf_fan, pf_ft, pf_bn, pf_w, pf_wm, pf_wd, pf_ncp, pf_cp);
        pf_ncp = pf_cp = pf_wm = pf_wd = pf_ft = pf_g = pf_e = pf_s = pf_ns = pf_r = pf_fa = pf_fan = pf_bn = pf_w = 0;
    }
    return r;
#else
    return mmu_give_i(k, out);
#endif
}
KMMU KHOT static void mmu_end_(struct krec *k)
{
#ifdef MMU_PROF
    u32 t = hr_time();
    mmu_end_i(k);
    pf_e += hr_time() - t;
#else
    mmu_end_i(k);
#endif
}
#endif
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
        const struct bootinfo *bi = BOOTINFO_K;
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
static int nop_f(const u8 *f, u32 pc) { (void)f; (void)pc; return 0; }
static int nop_h0(int a) { (void)a; return 0; }
static void nop_mk(struct proc *p, int a) { (void)p; (void)a; }
static struct umsg *nop_r(struct umsg *m, u32 kp) { (void)kp; return m; }
static void nop_m(struct umsg *m) { (void)m; }
static int nop_g(struct krec *k, u32 *s) { (void)k; *s = (u32)k; return 0; }
static void nop_k(struct krec *k) { (void)k; }
static int nop_c(struct proc *p, u32 a, u32 n, int w) { (void)p; (void)a; (void)n; (void)w; return 0; }
static struct umsg *nop_h(struct umsg *m) { (void)m; return 0; }
static int nop_gr(struct proc *p, u32 a, u32 b, u32 c) { (void)p; (void)a; (void)b; (void)c; return 0; }
static void nop_ug(struct proc *p, u32 a) { (void)p; (void)a; }
static long nop_pe(struct krec *k, u32 a, u32 n, int w) { (void)k; (void)a; (void)n; (void)w; return 0; }
static long nop_sh(u32 op, u32 a, u32 b) { (void)a; (void)b; return op - 1 < 3 ? 0 : -EINVAL; }   /* D60 7.4: without
                                         * protection SHM_OFFER, TAKE and DROP change nothing (all memory is visible) */
static int nop_sf(u32 a) { (void)a; return 0; }
static long nop_cv(u32 a, u32 b, u32 c) { (void)a; (void)b; (void)c; return -ENOSYS; }
static void nop_pb(struct port *a, u32 b, struct port *c, u32 d, u32 e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
static long nop_dv(u32 op, void *c, u32 a) { (void)op; (void)c; (void)a; return -EOPNOTSUPP; }
static const struct mmu_ops mmu_ops_off = { nop_p, nop_p, nop_p, nop_b, nop_v, nop_3, nop_h0, nop_v, nop_f,
                                            nop_r, nop_m, nop_g, nop_k, nop_k, nop_c, nop_v, nop_h,
                                            nop_gr, nop_ug, nop_pe, nop_sh, nop_sf, nop_cv, nop_cv, nop_pb, nop_dv, nop_mk };
const struct mmu_ops *mmu_ops = &mmu_ops_off;

/* the live table: read-only data in the span (.zhwmmu.rodata), the only place that holds span functions' addresses;
 * tools/check-init.py lets only mmu_arm refer to it, as move.l #mmu_ops_on,mmu_ops */
__attribute__((section(".zhwmmu.rodata"))) static const struct mmu_ops mmu_ops_on = {
    mmu_switch_, mmu_forget_, mmu_redo_, mmu_block_, mmu_upd_gone_, mmu_freed_,
    mmu_hand_, mmu_before_reset_, mmu_fault_,
    mmu_rec_, mmu_unrec_, mmu_give_, mmu_take_, mmu_end_, mmu_callok_, mmu_unsync_, mmu_held_,
    mmu_grant_, mmu_ungrant_, mmu_peer_, mmu_shm_, mmu_shmfree_, mmu_callv_, mmu_ramdisk_,
    mmu_postbuf_, drv_span_, mmu_mark_ };

/* the only writer of mmu_on besides mmu_before_reset_ (tools/check-init.py): armed exactly while it is set */
KMMU __attribute__((noinline)) static void mmu_arm(u8 f)
{
    mmu_ops = &mmu_ops_on;
    mmu_on = f;
}

/* mem_boot_release, protection off, before the span is filled and freed: no slot may point into it afterwards */
void mmu_disarm(void) { mmu_ops = &mmu_ops_off; }
