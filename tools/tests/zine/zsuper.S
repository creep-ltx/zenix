/* SPDX-License-Identifier: GPL-2.0-only */
/* zsuper: exec SuperState and UserState (docs/d29-zine.md 16.12; the RKRM autodocs). From user mode SuperState
 * gives supervisor mode on the program's own stack (the stack pointer as before the call; SR's S bit set; d0 the
 * system stack, not 0); called again in supervisor mode it does nothing and answers 0; UserState(d0) goes back to
 * user mode on the same stack (a third SuperState traps again: not 0). Writes "zsuper: ..." and returns 0, else the
 * number of the step that failed. Position-independent code. */
#include "zt.i"
#define LVO_SuperState  -150
#define LVO_UserState   -156
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    move.l  %sp,%d5                 /* the stack pointer before */
    jsr     LVO_SuperState(%a6)
    moveq   #1,%d7
    tst.l   %d0
    beq.s   9f
    move.l  %d0,%d6                 /* the system stack */
    moveq   #2,%d7
    cmp.l   %sp,%d5
    bne.s   9f
    move.w  %sr,%d1                 /* privileged: only in supervisor mode does this not trap */
    moveq   #3,%d7
    btst    #13,%d1
    beq.s   9f
    jsr     LVO_SuperState(%a6)     /* in supervisor mode: nothing, 0 */
    moveq   #4,%d7
    tst.l   %d0
    bne.s   9f
    move.l  %d6,%d0
    jsr     LVO_UserState(%a6)
    moveq   #5,%d7
    cmp.l   %sp,%d5
    bne.s   9f
    jsr     LVO_SuperState(%a6)     /* from user mode again: the system stack, not 0 */
    moveq   #6,%d7
    tst.l   %d0
    beq.s   9f
    jsr     LVO_UserState(%a6)
    jsr     LVO_UserState(%a6)      /* from user mode: nothing */
    moveq   #7,%d7
    cmp.l   %sp,%d5
    bne.s   9f
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq.s   9f
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    lea     text(%pc),%a0
    move.l  %a0,%d2
    moveq   #textend-text,%d3
    jsr     LVO_Write(%a6)
    moveq   #0,%d7
9:  move.l  %d7,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
dosname: .asciz "dos.library"
text:   .ascii "zsuper: SuperState and UserState on the program's own stack\n"
textend:
    .even
