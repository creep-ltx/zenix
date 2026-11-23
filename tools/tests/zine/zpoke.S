/* SPDX-License-Identifier: GPL-2.0-only */
/* zpoke A B: changes one byte at A + B (two numbers in hex, "0x" allowed) and returns 0: a stray write into Zenix's
 * memory during the session (DESIGN D60 7.3's test, tools/zine-emu-test.sh: the kernel's check after the session must
 * find it). Without a number it changes nothing. */
#include "zt.i"
    .text
    movem.l %d2-%d3,-(%sp)
    moveq   #0,%d1                  /* the number being read */
    moveq   #0,%d3                  /* the sum of those read */
1:  tst.l   %d0
    beq.s   3f
    subq.l  #1,%d0
    move.b  (%a0)+,%d2
    cmp.b   #'0',%d2
    blt.s   2f
    cmp.b   #'9',%d2
    bgt.s   4f
    sub.b   #'0',%d2
    bra.s   5f
4:  or.b    #0x20,%d2
    cmp.b   #'a',%d2
    blt.s   2f
    cmp.b   #'f',%d2
    bgt.s   2f
    sub.b   #'a'-10,%d2
5:  lsl.l   #4,%d1
    and.l   #15,%d2
    or.l    %d2,%d1
    bra.s   1b
2:  add.l   %d1,%d3                 /* a number ended (a leading 0 and the x of 0x add nothing) */
    moveq   #0,%d1
    bra.s   1b
3:  add.l   %d1,%d3
    beq.s   9f
    move.l  %d3,%a1
    eori.b  #0x20,(%a1)
9:  movem.l (%sp)+,%d2-%d3
    moveq   #0,%d0
    rts
