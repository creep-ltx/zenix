/* SPDX-License-Identifier: GPL-2.0-only */
/* zscreen: intuition's OpenScreenTagList (320x256, 2 planes), plane 0 filled with vertical stripes, colour 1 set
 * yellow with graphics' LoadRGB32, a user Copper list on its ViewPort (16.18: UCopperListInit, CWait, CMove, CBump; a
 * MOVE to DMACON at line 10 that sets the disk DMA enable bit, seen in DMACONR; then FreeVPortCopLists), 100 frames
 * (2 s on PAL) by the beam, CloseScreen, returns 0 (20 if a call failed, 21 if DSKEN was on before the list, 22 if the
 * Copper's MOVE did not set it, 23 if it was still set after the list was freed and DSKEN cleared). */
#include "zt.i"
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    lea     intname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,%a5                 /* IntuitionBase */
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,%a4                 /* GfxBase */
    move.l  %a5,%d0
    beq     9f
    move.l  %a4,%d0
    beq     9f
    move.l  %a5,%a6
    sub.l   %a0,%a0
    lea     tags(%pc),%a1
    jsr     LVO_OpenScreenTagList(%a6)
    tst.l   %d0
    beq     9f
    move.l  %d0,%a3                 /* the Screen */
    move.l  sc_BitMap+bm_Planes(%a3),%a0
    move.w  #40*256/4-1,%d0
1:  move.l  #0xff00ff00,(%a0)+
    dbra    %d0,1b
    move.l  %a4,%a6
    lea     sc_ViewPort(%a3),%a0
    lea     rgb(%pc),%a1
    jsr     LVO_LoadRGB32(%a6)
    lea     0xdff000,%a2
    moveq   #21,%d7
    btst    #4,3(%a2)               /* DMACONR: DSKEN off at the start */
    bne     8f
    lea     ucl(%pc),%a0            /* the user Copper list: line 10, DMACON = $8010; the end */
    moveq   #4,%d0
    jsr     -594(%a6)               /* UCopperListInit */
    moveq   #20,%d7
    tst.l   %d0
    beq     8f
    lea     ucl(%pc),%a1
    moveq   #10,%d0
    moveq   #0,%d1
    jsr     -378(%a6)               /* CWait */
    lea     ucl(%pc),%a1
    jsr     -366(%a6)               /* CBump */
    lea     ucl(%pc),%a1
    move.l  #0x096,%d0
    move.l  #0x8010,%d1
    jsr     -372(%a6)               /* CMove */
    lea     ucl(%pc),%a1
    jsr     -366(%a6)
    lea     ucl(%pc),%a1
    move.l  #10000,%d0
    move.l  #255,%d1
    jsr     -378(%a6)               /* CEND */
    lea     ucl(%pc),%a1
    jsr     -366(%a6)
    lea     ucl(%pc),%a0
    move.l  %a0,sc_ViewPort+20(%a3) /* vp_UCopIns */
    move.l  %a5,%a6
    jsr     -390(%a6)               /* RethinkDisplay */
    bsr     frames
    moveq   #22,%d7
    btst    #4,3(%a2)               /* DSKEN set by the Copper */
    beq     8f
    move.l  %a4,%a6
    lea     sc_ViewPort(%a3),%a0
    jsr     -540(%a6)               /* FreeVPortCopLists */
    move.l  %a5,%a6
    jsr     -390(%a6)
    bsr     frames
    move.w  #0x0010,0x96(%a2)       /* DSKEN off again: nothing sets it now */
    bsr     frames
    moveq   #23,%d7
    btst    #4,3(%a2)
    bne     8f
    moveq   #0,%d7
8:  move.w  #99,%d6
2:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    bne.s   2b
3:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    beq.s   3b
    dbra    %d6,2b
    move.w  #0x0010,0x96(%a2)
    move.l  %a5,%a6
    move.l  %a3,%a0
    jsr     LVO_CloseScreen(%a6)
    move.l  %d7,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
frames:                                 /* three frames by the beam (a2 the chips; d0-d1 used) */
    moveq   #2,%d1
4:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    bne.s   4b
5:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    beq.s   5b
    dbra    %d1,4b
    rts
9:  moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
    .even
tags:   .long   0x80000023,320, 0x80000024,256, 0x80000025,8, 0
rgb:    .long   2<<16, 0,0,0, 0xffffffff,0xffffffff,0, 0     /* (no '|': a comment in gas) */
    .balign 4
ucl:    .long   0, 0, 0                 /* a UCopList: Next, FirstCopList, CopList */
intname: .asciz "intuition.library"
gfxname: .asciz "graphics.library"
    .even
