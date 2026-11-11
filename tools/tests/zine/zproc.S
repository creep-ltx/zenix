/* SPDX-License-Identifier: GPL-2.0-only */
/* zproc (d29 16.15): dos CreateNewProc and CreateProc ask for a second process; ZINE runs one task and answers as
 * AmigaOS does when it cannot make one: 0, IoErr ERROR_NO_FREE_STORE (103), said once with the name. The child's
 * entry must never run. Each check that fails is named; "zproc: refused as asked" and 0, else 1.
 * Position-independent code. */
#include "zt.i"
#define LVO_IoErr       -132
#define LVO_CreateProc  -138
#define LVO_CreateNewProc -498
#define NP_Entry        (0x80000000 + 1000 + 3)
#define NP_Name         (0x80000000 + 1000 + 12)
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
    lea     ran(%pc),%a0
    clr.l   (%a0)
    lea     tags(%pc),%a0           /* the tag list, filled in here (position-independent) */
    move.l  #NP_Entry,(%a0)+
    lea     child(%pc),%a1
    move.l  %a1,(%a0)+
    move.l  #NP_Name,(%a0)+
    lea     cname(%pc),%a1
    move.l  %a1,(%a0)+
    clr.l   (%a0)+
    clr.l   (%a0)
    move.l  %a3,%a6
    lea     tags(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_CreateNewProc(%a6)
    tst.l   %d0
    beq.s   1f
    lea     n_new(%pc),%a0
    bsr.w   wrong
1:  jsr     LVO_IoErr(%a6)
    cmp.l   #103,%d0
    beq.s   2f
    lea     n_err(%pc),%a0
    bsr.w   wrong
2:  lea     cname(%pc),%a0          /* CreateProc(name, pri, seglist, stack): 0 too */
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    move.l  #4096,%d4
    jsr     LVO_CreateProc(%a6)
    tst.l   %d0
    beq.s   3f
    lea     n_old(%pc),%a0
    bsr.w   wrong
3:  lea     ran(%pc),%a0
    tst.l   (%a0)
    beq.s   4f
    lea     n_ran(%pc),%a0
    bsr.w   wrong
4:  tst.l   %d7
    bne.s   5f
    lea     allright(%pc),%a0
    bsr.w   say
5:  move.l  %a3,%a1
    move.l  4.w,%a6
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    tst.l   %d7
    beq.s   6f
    moveq   #1,%d0
6:  movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
99: moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
child:                              /* the second process's entry: must not run */
    lea     ran(%pc),%a0
    addq.l  #1,(%a0)
    moveq   #0,%d0
    rts
wrong:                              /* a0: the line */
    addq.l  #1,%d7
say:                                /* a0: a 0-ended line; a6 is dos */
    movem.l %d0-%d3,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3
    rts
dosname: .asciz "dos.library"
cname:   .asciz "zchild"
allright: .asciz "zproc: refused as asked\n"
n_new:   .asciz "zproc: wrong: CreateNewProc gave a process\n"
n_err:   .asciz "zproc: wrong: IoErr is not 103\n"
n_old:   .asciz "zproc: wrong: CreateProc gave a process\n"
n_ran:   .asciz "zproc: wrong: the child's entry ran\n"
    .balign 4
ran:     .long 0
tags:    .space 24
