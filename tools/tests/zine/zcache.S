/* SPDX-License-Identifier: GPL-2.0-only */
/* zcache (d29 16.15): a program on a 68040/060 that switches the data cache on itself (MOVEC to CACR, as demos do)
 * and writes chip RAM with the CPU: the chips must see the write (the blitter copies it), because ZINE leaves Zenix's
 * transparent translation (DTT0: the first 16 MB, chip RAM and the chips, inhibited) in place. A control after it:
 * the same write with DTT0 made copyback for a moment, then inhibited again before the blit, shows whether the
 * machine models the data cache at all (said, not judged). Also: MOVEC to and from CAAR (020/030 only) taken as a
 * no-op that reads 0; AttnFlags say the FPU when the CPU package has it (AFF_68881, AFF_68882, AFF_FPU40);
 * CacheControl keeps the data cache off. Each check that fails is named; "zcache: all answered" and 0, else 1.
 * Run on the 68040 and 68060 machines only (fsuae/zine). Position-independent code. */
#include "zt.i"
#define LVO_CacheControl -648
#define AttnFlags       296
#define PATTERN         0x5A17C0DE
#define CONTROL         0x600DF00D
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    moveq   #0,%d7
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,%a3                 /* dos */
    tst.l   %d0
    beq.w   99f
    move.w  AttnFlags(%a6),%d0
    and.w   #0x0088,%d0             /* AFF_68040 or AFF_68060 */
    bne.s   1f
    lea     n_cpu(%pc),%a0
    bsr.w   wrong
    bra.w   8f
1:  move.w  AttnFlags(%a6),%d0
    and.w   #0x0070,%d0
    cmp.w   #0x0070,%d0
    beq.s   2f
    lea     n_attn(%pc),%a0
    bsr.w   wrong
2:  moveq   #64,%d0
    move.l  #0x10002,%d1            /* MEMF_CHIP | MEMF_CLEAR */
    jsr     LVO_AllocMem(%a6)
    tst.l   %d0
    beq.w   99f
    move.l  %d0,%d6                 /* to free */
    add.l   #15,%d0
    and.w   #0xFFF0,%d0
    move.l  %d0,%a4                 /* A: a 16-byte line of chip RAM; B: the next line */
    moveq   #-1,%d2
    moveq   #-1,%d3
    moveq   #-1,%d5
    lea     sv(%pc),%a5
    jsr     LVO_Supervisor(%a6)
    cmp.l   #PATTERN,%d2
    beq.s   3f
    lea     n_blit(%pc),%a0
    bsr.w   wrong
3:  lea     ctl_on(%pc),%a0         /* the control: what the blitter copied while the write was only in the cache */
    cmp.l   #PATTERN,%d3
    beq.s   4f
    lea     ctl_off(%pc),%a0
    cmp.l   #CONTROL,%d3
    beq.s   4f
    lea     ctl_odd(%pc),%a0
    addq.l  #1,%d7
4:  bsr.w   say
    tst.l   %d5
    beq.s   5f
    lea     n_caar(%pc),%a0
    bsr.w   wrong
5:  move.l  #0x100,%d0              /* CacheControl(CACRF_EnableD, CACRF_EnableD): the data cache stays off */
    move.l  %d0,%d1
    jsr     LVO_CacheControl(%a6)
    moveq   #0,%d0
    moveq   #0,%d1
    jsr     LVO_CacheControl(%a6)
    btst    #8,%d0
    beq.s   6f
    lea     n_cc(%pc),%a0
    bsr.w   wrong
6:  move.l  %d6,%a1
    moveq   #64,%d0
    jsr     LVO_FreeMem(%a6)
8:  tst.l   %d7
    bne.s   9f
    lea     allright(%pc),%a0
    bsr.w   say
9:  move.l  %a3,%a1
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    tst.l   %d7
    beq.s   10f
    moveq   #1,%d0
10: movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
99: moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* the blitter copies A's first two words to B (A to D, one row of two words); a0 the chips; d1 counts the waits
 * (a timeout: B keeps what it had, which the check names). No stack is used. */
.macro BLIT
    lea     0xDFF000,%a0
    move.l  #4000000,%d1
91: subq.l  #1,%d1
    beq.s   93f
    btst    #6,2(%a0)               /* DMACONR BBUSY */
    bne.s   91b
    move.w  #0x8240,0x96(%a0)       /* DMACON: SET, DMAEN, BLTEN */
    move.l  #0x09F00000,0x40(%a0)   /* BLTCON0 A to D, BLTCON1 0 */
    moveq   #-1,%d0
    move.l  %d0,0x44(%a0)           /* BLTAFWM, BLTALWM */
    move.l  %a4,0x50(%a0)           /* BLTAPT */
    lea     16(%a4),%a1
    move.l  %a1,0x54(%a0)           /* BLTDPT */
    clr.l   0x64(%a0)               /* BLTAMOD, BLTDMOD */
    move.w  #0x0042,0x58(%a0)       /* BLTSIZE: one row (1 << 6), two words */
    tst.b   2(%a0)
    move.l  #4000000,%d1
92: subq.l  #1,%d1
    beq.s   93f
    btst    #6,2(%a0)
    bne.s   92b
93:
.endm

/* in supervisor mode (Supervisor): d2 what the blitter copied with ZINE's set-up, d3 the control's copy, d5 CAAR */
sv: ori.w   #0x0700,%sr
    movec   %cacr,%a2
    move.l  %a2,%d0
    or.l    #0x80000000,%d0         /* 040 DE, 060 EDC: the data cache on, as a demo switches it on */
    movec   %d0,%cacr
    move.l  #PATTERN,(%a4)
    BLIT
    move.l  16(%a4),%d2
    clr.l   16(%a4)
    /* the control: the first 16 MB copyback through DTT0, A written (and only in the cache), DTT0 back */
    .long   0x4E7A4006              /* movec dtt0,d4 */
    move.l  #0x0000C020,%d0         /* base 0, mask 0, enabled, both modes, copyback */
    .long   0x4E7B0006              /* movec d0,dtt0 */
    move.l  (%a4),%d0               /* A's line into the cache */
    move.l  #CONTROL,(%a4)
    .long   0x4E7B4006              /* movec d4,dtt0: inhibited again, the line not pushed */
    BLIT
    move.l  16(%a4),%d3
    .word   0xF478                  /* cpusha dc: the line written to A now */
    movec   %a2,%cacr
    .word   0xF4F8                  /* cpusha bc */
    /* MOVEC CAAR (020/030 only): a no-op that reads 0 */
    move.l  #0x12345678,%d0
    .long   0x4E7B0802              /* movec d0,caar */
    moveq   #-1,%d5
    .long   0x4E7A5802              /* movec caar,d5 */
    rte

wrong:                              /* a0: the line */
    addq.l  #1,%d7
say:                                /* a0: a 0-ended line */
    movem.l %d0-%d3/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    move.l  %a3,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3/%a6
    rts
dosname: .asciz "dos.library"
allright: .asciz "zcache: all answered\n"
n_cpu:   .asciz "zcache: wrong: AttnFlags say no 68040 or 68060\n"
n_attn:  .asciz "zcache: wrong: AttnFlags do not say the FPU\n"
n_blit:  .asciz "zcache: wrong: the blitter did not see a chip RAM write made with the data cache on\n"
n_caar:  .asciz "zcache: wrong: MOVEC CAAR did not read 0\n"
n_cc:    .asciz "zcache: wrong: CacheControl switched the data cache on\n"
ctl_on:  .asciz "zcache: control: a write kept in the cache was not seen by the blitter (the machine models the data cache)\n"
ctl_off: .asciz "zcache: control: the blitter saw a write meant to stay in the cache (the machine does not model the data cache)\n"
ctl_odd: .asciz "zcache: wrong: the control's copy is neither value\n"
