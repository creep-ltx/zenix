/* SPDX-License-Identifier: GPL-2.0-only */
/* zscreen: intuition's OpenScreenTagList (320x256, 8 planes), plane 0 filled with vertical stripes, colour 1 set
 * yellow with graphics' LoadRGB32, a user Copper list on its ViewPort (16.18: UCopperListInit, CWait, CMove, CBump; a
 * MOVE to DMACON at line 10 that sets the disk DMA enable bit, seen in DMACONR; then FreeVPortCopLists), 100 frames
 * (2 s on PAL) by the beam, CloseScreen, returns 0 (20 if a call failed, 21 if DSKEN was on before the list, 22 if the
 * Copper's MOVE did not set it, 23 if it was still set after the list was freed and DSKEN cleared; 24-29 the sprite,
 * below). */
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
    bsr     sprite
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
/* 16.19: a 16x16 sprite from a BitMap (plane 0 all set: colour 1) made by AllocSpriteDataA, taken as sprite 2 by
 * GetExtSpriteA, moved to 16,16 by MoveSprite (its control words checked: the AHRM's SPRxPOS/SPRxCTL for lines $3C-$4C,
 * the ExtSprite's pixel $91); seen by the hardware's collision with plane 1 (CLXCON: plane 1 set; CLXDAT bit 2,
 * plane 1 against sprite 2): none while it is at 0,0 (no lines), one once moved, none after FreeSprite. d7: 0, or 24
 * AllocSpriteDataA failed, 25 GetExtSpriteA did not give 2, 26 the control words wrong, 27 a collision while hidden,
 * 28 none when shown, 29 one after FreeSprite; a4 GfxBase, a3 the screen, a2 the chips (kept); d0-d1, d5, a0-a1, a6 */
sprite:
    moveq   #0,%d5
    move.l  %a4,%a6
    lea     sbm(%pc),%a0
    lea     splane(%pc),%a1
    move.l  %a1,8(%a0)              /* bm_Planes[0] */
    move.l  %a0,%a2
    lea     notags(%pc),%a1
    jsr     -1020(%a6)              /* AllocSpriteDataA */
    lea     0xdff000,%a2
    moveq   #24,%d7
    tst.l   %d0
    beq     7f
    move.l  %d0,%d5                 /* the ExtSprite */
    move.l  %d5,%a2
    lea     numtags(%pc),%a1
    jsr     -930(%a6)               /* GetExtSpriteA, sprite 2 */
    lea     0xdff000,%a2
    moveq   #25,%d7
    cmp.l   #2,%d0
    bne     7f
    move.w  #0x0041,0x98(%a2)       /* CLXCON: plane 1 enabled, matched when set */
    bsr     frames
    move.w  0x0e(%a2),%d0           /* CLXDAT read: cleared */
    bsr     frames
    moveq   #27,%d7
    btst    #2,0x0f(%a2)
    bne     7f
    lea     sc_ViewPort(%a3),%a0
    move.l  %d5,%a1
    moveq   #16,%d0
    moveq   #16,%d1
    jsr     -426(%a6)               /* MoveSprite */
    moveq   #26,%d7
    move.l  %d5,%a0
    move.l  (%a0),%a0
    cmp.l   #0x3c484c01,(%a0)
    bne     7f
    bsr     frames
    move.w  0x0e(%a2),%d0
    bsr     frames
    moveq   #28,%d7
    btst    #2,0x0f(%a2)
    beq     7f
    moveq   #2,%d0
    jsr     -414(%a6)               /* FreeSprite */
    bsr     frames
    move.w  0x0e(%a2),%d0
    bsr     frames
    moveq   #29,%d7
    btst    #2,0x0f(%a2)
    bne     7f
    moveq   #0,%d7
7:  tst.l   %d5
    beq.s   6f
    move.l  %d5,%a2
    jsr     -1032(%a6)              /* FreeSpriteData */
6:  lea     0xdff000,%a2
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
sbm:    .word   2, 16               /* a BitMap: BytesPerRow, Rows, */
    .byte   0, 1                    /* Flags, Depth, */
    .word   0                       /* pad, */
    .long   0, 0, 0, 0, 0, 0, 0, 0  /* Planes */
splane: .rept 16
    .word   0xffff
    .endr
notags: .long   0
numtags: .long  0x82000020, 2, 0
intname: .asciz "intuition.library"
gfxname: .asciz "graphics.library"
    .even
