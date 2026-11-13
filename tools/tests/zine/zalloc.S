/* SPDX-License-Identifier: GPL-2.0-only */
/* zalloc: 1 MB of fast RAM and 256 KB of chip RAM from AllocMem, every long of both written and read back, both
 * freed, AvailMem the same as at the start. Returns 0, or 10 + the step that went wrong. "zalloc b" (16.22) frees
 * 64 bytes at $F80000 (the ROM: in no memory list): ZINE names the block, then exec's AN_BadFreeAddr ends it.
 * Then a pool (16.22): CreatePool(MEMF_CLEAR, 4096, 1024); 316 and 100 bytes from it lie next to each other in one
 * puddle (the second 320 bytes after the first: no bookkeeping between them), cleared; a long written 4 bytes before
 * the second (Signals' write) breaks nothing; 2000 bytes (above the threshold) come from elsewhere; all three freed,
 * a fourth taken, DeletePool; AvailMem as at the start. Steps 17-21. */
#include "zt.i"
#define LVO_CreatePool  -696
#define LVO_DeletePool  -702
#define LVO_AllocPooled -708
#define LVO_FreePooled  -714
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    cmp.b   #0x62,(%a0)             /* "b": a block in no memory list freed */
    bne.s   0f
    move.l  #0xF80000,%a1
    moveq   #64,%d0
    jsr     LVO_FreeMem(%a6)
0:  moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  %d0,%d7                 /* free at the start */
    move.l  #0x100000,%d0
    moveq   #MEMF_FAST,%d1
    jsr     LVO_AllocMem(%a6)
    moveq   #11,%d6
    tst.l   %d0
    beq     9f
    move.l  %d0,%a4
    move.l  #0x40000,%d0
    moveq   #MEMF_CHIP,%d1
    jsr     LVO_AllocMem(%a6)
    moveq   #12,%d6
    tst.l   %d0
    beq     8f
    move.l  %d0,%a5
    moveq   #13,%d6
    cmp.l   #0x200000,%a5           /* chip RAM is below 2 MB */
    bcc     7f
    move.l  %a4,%a0                 /* the fast block: its address as the pattern */
    move.l  #0x100000/4-1,%d0
1:  move.l  %a0,(%a0)+
    subq.l  #1,%d0
    bpl.s   1b
    move.l  %a5,%a0
    move.l  #0x40000/4-1,%d0
2:  move.l  %a0,(%a0)+
    subq.l  #1,%d0
    bpl.s   2b
    moveq   #14,%d6
    move.l  %a4,%a0
    move.l  #0x100000/4-1,%d0
3:  cmp.l   (%a0),%a0
    bne.s   7f
    addq.l  #4,%a0
    subq.l  #1,%d0
    bpl.s   3b
    moveq   #15,%d6
    move.l  %a5,%a0
    move.l  #0x40000/4-1,%d0
4:  cmp.l   (%a0),%a0
    bne.s   7f
    addq.l  #4,%a0
    subq.l  #1,%d0
    bpl.s   4b
    moveq   #0,%d6
7:  move.l  %a5,%a1
    move.l  #0x40000,%d0
    jsr     LVO_FreeMem(%a6)
8:  move.l  %a4,%a1
    move.l  #0x100000,%d0
    jsr     LVO_FreeMem(%a6)
    tst.l   %d6
    bne     9f
    move.l  #0x10000,%d0            /* MEMF_CLEAR */
    move.l  #4096,%d1
    move.l  #1024,%d2
    jsr     LVO_CreatePool(%a6)
    moveq   #17,%d6
    tst.l   %d0
    beq     9f
    move.l  %d0,%a5                 /* the pool */
    move.l  %a5,%a0
    move.l  #316,%d0
    jsr     LVO_AllocPooled(%a6)
    move.l  %d0,%d3
    move.l  %a5,%a0
    moveq   #100,%d0
    jsr     LVO_AllocPooled(%a6)
    move.l  %d0,%d4
    moveq   #18,%d6
    tst.l   %d3
    beq     5f
    tst.l   %d4
    beq     5f
    move.l  %d3,%d0
    add.l   #320,%d0
    cmp.l   %d0,%d4                 /* next to each other in the puddle */
    bne     5f
    moveq   #19,%d6
    move.l  %d4,%a0
    moveq   #24,%d0
6:  tst.l   (%a0)+                  /* MEMF_CLEAR: cleared */
    bne     5f
    subq.l  #1,%d0
    bne.s   6b
    move.l  %d4,%a0
    move.l  #0xDEADBEEF,-4(%a0)     /* Signals' write before its block */
    move.l  %a5,%a0
    move.l  #2000,%d0
    jsr     LVO_AllocPooled(%a6)
    move.l  %d0,%d5
    moveq   #20,%d6
    tst.l   %d5
    beq     5f
    move.l  %a5,%a0
    move.l  %d4,%a1
    moveq   #100,%d0
    jsr     LVO_FreePooled(%a6)
    move.l  %a5,%a0
    move.l  %d3,%a1
    move.l  #316,%d0
    jsr     LVO_FreePooled(%a6)
    move.l  %a5,%a0
    move.l  %d5,%a1
    move.l  #2000,%d0
    jsr     LVO_FreePooled(%a6)
    move.l  %a5,%a0
    moveq   #40,%d0
    jsr     LVO_AllocPooled(%a6)
    moveq   #21,%d6
    tst.l   %d0
    beq.s   5f
    moveq   #0,%d6
5:  move.l  %a5,%a0
    jsr     LVO_DeletePool(%a6)
    tst.l   %d6
    bne.s   9f
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    moveq   #16,%d6
    cmp.l   %d0,%d7
    bne.s   9f
    moveq   #0,%d6
9:  move.l  %d6,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
