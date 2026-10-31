/* SPDX-License-Identifier: GPL-2.0-only */
/* zmmu: what old demos do with a 68030's MMU (switch it off, flush it, read its registers to put them back later), on
 * a CPU without that MMU (a 68020, a 68040/060, Emu68): ZINE's line-F vector takes each as a no-op (D29 16.4). Reads
 * give 0, (An)+ and -(An) move An, the program goes on after each. Returns 0 when every check held, else the number
 * of the first that failed. The instructions as words: the assembler's -m68020-60 has no 68030 MMU. */
#include "zt.i"
    .text
    lea     buf(%pc),%a0
    moveq   #-1,%d0
    move.l  %d0,(%a0)
    move.l  %d0,4(%a0)
    move.l  %d0,8(%a0)
    move.l  %d0,12(%a0)
    .word   0xF010,0x4200           /* pmove tc,(a0) */
    moveq   #1,%d7
    tst.l   (%a0)
    bne.s   fail
    .word   0xF010,0x4000           /* pmove (a0),tc */
    .word   0xF000,0x2400           /* pflusha */
    move.l  %d0,(%a0)
    .word   0xF010,0x0A00           /* pmove tt0,(a0) */
    moveq   #2,%d7
    tst.l   (%a0)
    bne.s   fail
    clr.l   (%a0)
    move.l  %a0,%a1
    .word   0xF019,0x4000           /* pmove (a1)+,tc */
    moveq   #3,%d7
    lea     4(%a0),%a2
    cmp.l   %a1,%a2
    bne.s   fail
    move.l  %sp,%a3
    .word   0xF027,0x4E00           /* pmove crp,-(sp) */
    moveq   #4,%d7
    lea     -8(%a3),%a2
    cmp.l   %sp,%a2
    bne.s   fail
    tst.l   (%sp)+
    bne.s   fail
    tst.l   (%sp)+
    bne.s   fail
    .word   0xF028,0x4A00,0x0008    /* pmove srp,8(a0) */
    moveq   #5,%d7
    tst.l   8(%a0)
    bne.s   fail
    tst.l   12(%a0)
    bne.s   fail
    .word   0xF03C,0x4000,0x0000,0x0000     /* pmove #0,tc */
    move.l  4.w,%a6                 /* and in supervisor mode, as programs do them (they are privileged there) */
    lea     sv(%pc),%a5
    jsr     LVO_Supervisor(%a6)
    moveq   #6,%d7
    tst.l   %d0
    bne.s   fail
    moveq   #0,%d7
fail:
    move.l  %d7,%d0
    rts
sv: lea     buf(%pc),%a0            /* supervisor mode: the 68040's PFLUSHA, and PMOVE TC, read (0) and written */
    moveq   #-1,%d0
    move.l  %d0,(%a0)
    .word   0xF518                  /* pflusha (68040/060) */
    .word   0xF010,0x4200           /* pmove tc,(a0) */
    move.l  (%a0),%d0
    .word   0xF010,0x4000           /* pmove (a0),tc */
    rte
    .balign 4
buf:
    .long   0, 0, 0, 0
