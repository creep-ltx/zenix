/* SPDX-License-Identifier: GPL-2.0-only */
/* zalloc: 1 MB of fast RAM and 256 KB of chip RAM from AllocMem, every long of both written and read back, both
 * freed, AvailMem the same as at the start. Returns 0, or 10 + the step that went wrong. */
#include "zt.i"
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    moveq   #0,%d1
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
