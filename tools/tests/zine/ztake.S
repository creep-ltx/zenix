/* SPDX-License-Identifier: GPL-2.0-only */
/* ztake: takes the display over as a demo does and gives it back. Opens graphics.library, keeps the active view,
 * LoadView(0), puts its own Copper list in chip RAM (the background red at the top, blue from line 0x80 on, white
 * from 0xC0) with only Copper DMA on, waits 150 frames (3 s on PAL) by the beam, then puts the system's Copper list
 * and view back and returns 0. Every frame it also writes the frame count into its own table, which ZINE ignores. */
#include "zt.i"
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq     9f
    move.l  %d0,%a5                 /* GfxBase */
    move.l  gb_ActiView(%a5),%d7    /* the view to give back */
    moveq   #32,%d0
    moveq   #MEMF_CHIP,%d1
    jsr     LVO_AllocMem(%a6)
    tst.l   %d0
    beq     8f
    move.l  %d0,%a4                 /* the Copper list, in chip RAM */
    lea     coplist(%pc),%a0
    move.l  %a4,%a1
    moveq   #7,%d0
1:  move.l  (%a0)+,(%a1)+
    dbra    %d0,1b
    exg     %a5,%a6
    sub.l   %a1,%a1
    jsr     LVO_LoadView(%a6)
    jsr     LVO_WaitTOF(%a6)
    jsr     LVO_WaitTOF(%a6)
    exg     %a5,%a6
    jsr     LVO_Forbid(%a6)
    lea     0xdff000,%a3
    move.w  #0x7fff,0x96(%a3)       /* DMACON: all off */
    move.l  %a4,0x80(%a3)           /* COP1LC: ours */
    move.w  %d0,0x88(%a3)           /* COPJMP1 */
    move.w  #0x8280,0x96(%a3)       /* DMACON: master and Copper on */
    move.w  #149,%d6
2:  move.l  4(%a3),%d0              /* VPOSR/VHPOSR: wait for line 256, then for its end */
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    bne.s   2b
3:  move.l  4(%a3),%d0
    and.l   #0x1ff00,%d0
    cmp.l   #0x10000,%d0
    beq.s   3b
    dbra    %d6,2b
    move.l  gb_copinit(%a5),0x80(%a3)   /* the system's Copper list back */
    move.w  %d0,0x88(%a3)
    move.w  #0x83f0,0x96(%a3)       /* DMACON: master, bitplanes, Copper, blitter, sprites on */
    jsr     LVO_Permit(%a6)
    exg     %a5,%a6
    move.l  %d7,%a1
    jsr     LVO_LoadView(%a6)
    jsr     LVO_WaitTOF(%a6)
    exg     %a5,%a6
    move.l  %a4,%a1
    moveq   #32,%d0
    jsr     LVO_FreeMem(%a6)
    move.l  %a5,%a1
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
8:  move.l  %a5,%a1
    jsr     LVO_CloseLibrary(%a6)
9:  moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
    .even
coplist:
    .word   0x0180,0x0f00           /* COLOR00 red */
    .word   0x8001,0xfffe           /* wait for line 0x80 */
    .word   0x0180,0x000f           /* blue */
    .word   0xc001,0xfffe           /* wait for line 0xC0 */
    .word   0x0180,0x0fff           /* white */
    .word   0xffff,0xfffe           /* the end */
    .word   0xffff,0xfffe
    .word   0xffff,0xfffe
gfxname: .asciz "graphics.library"
    .even
