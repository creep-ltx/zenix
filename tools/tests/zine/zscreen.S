/* SPDX-License-Identifier: GPL-2.0-only */
/* zscreen: intuition's OpenScreenTagList (320x256, 2 planes), plane 0 filled with vertical stripes, colour 1 set
 * yellow with graphics' LoadRGB32, 100 frames (2 s on PAL) by the beam, CloseScreen, returns 0 (20 if it failed). */
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
    move.w  #99,%d6
2:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    bne.s   2b
3:  move.l  4(%a2),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    beq.s   3b
    dbra    %d6,2b
    move.l  %a5,%a6
    move.l  %a3,%a0
    jsr     LVO_CloseScreen(%a6)
    moveq   #0,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
9:  moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
    .even
tags:   .long   0x80000023,320, 0x80000024,256, 0x80000025,8, 0
rgb:    .long   2<<16, 0,0,0, 0xffffffff,0xffffffff,0, 0     /* (no '|': a comment in gas) */
intname: .asciz "intuition.library"
gfxname: .asciz "graphics.library"
    .even
