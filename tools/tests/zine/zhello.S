/* SPDX-License-Identifier: GPL-2.0-only */
/* zhello: opens dos.library, writes a line to Output() and returns 0 (ZINE keeps the output and zine prints it). */
#include "zt.i"
    .text
    movem.l %d2-%d3/%a6,-(%sp)
    move.l  4.w,%a6                 /* ExecBase */
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
    move.l  %a6,%a1
    move.l  4.w,%a6
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    movem.l (%sp)+,%d2-%d3/%a6
    rts
9:  moveq   #20,%d0
    movem.l (%sp)+,%d2-%d3/%a6
    rts
dosname: .asciz "dos.library"
text:   .ascii "hello from an AmigaOS program\n"
textend:
    .even
