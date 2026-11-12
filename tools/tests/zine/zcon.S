/* SPDX-License-Identifier: GPL-2.0-only */
/* zcon (d29 16.14): console.device (a CMD_WRITE with a control sequence, a CMD_READ that stays in progress until
 * AbortIO, the ConUnit's grid), input.device (IND_ADDHANDLER and IND_REMHANDLER done), intuition's ViewAddress, and a
 * large AllocMem that fails (ZINE says it once). Each check that fails is named; "zcon: all answered" and 0, else 1. */
#include "zt.i"
#define LVO_OpenDevice  -444
#define LVO_CloseDevice -450
#define LVO_DoIO        -456
#define LVO_SendIO      -462
#define LVO_CheckIO     -468
#define LVO_WaitIO      -474
#define LVO_AbortIO     -480
#define LVO_ViewAddress -294
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    moveq   #0,%d7
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,%a5                 /* dos */
    tst.l   %d0
    beq.w   99f
    lea     io(%pc),%a3
    /* console.device, unit 0, no window: the 640 x 200 grid */
    lea     conname(%pc),%a0
    moveq   #0,%d0
    move.l  %a3,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne.w   90f
    move.l  24(%a3),%a0             /* io_Unit: the ConUnit */
    cmp.w   #79,42(%a0)             /* cu_XMax */
    bne.w   90f
    cmp.w   #24,44(%a0)             /* cu_YMax */
    bne.w   90f
    move.w  #3,28(%a3)              /* CMD_WRITE */
    lea     text(%pc),%a0
    move.l  %a0,40(%a3)
    moveq   #-1,%d0
    move.l  %d0,36(%a3)             /* up to its 0 byte */
    move.l  %a3,%a1
    jsr     LVO_DoIO(%a6)
    tst.l   %d0
    bne.w   90f
    cmp.l   #textend-text-1,32(%a3) /* io_Actual: every byte */
    bne.w   90f
    move.w  #2,28(%a3)              /* CMD_READ: in progress */
    lea     buf(%pc),%a0
    move.l  %a0,40(%a3)
    moveq   #8,%d0
    move.l  %d0,36(%a3)
    move.l  %a3,%a1
    jsr     LVO_SendIO(%a6)
    move.l  %a3,%a1
    jsr     LVO_CheckIO(%a6)
    tst.l   %d0
    bne.w   90f                     /* done already: wrong */
    move.l  %a3,%a1
    jsr     LVO_AbortIO(%a6)
    move.l  %a3,%a1
    jsr     LVO_WaitIO(%a6)
    cmp.l   #-2,%d0                 /* IOERR_ABORTED */
    bne.w   90f
    move.l  %a3,%a1
    jsr     LVO_CloseDevice(%a6)
    bra.s   1f
90: lea     n_con(%pc),%a0
    bsr.w   wrong
    /* input.device: a handler added and removed */
1:  lea     io(%pc),%a3
    lea     inpname(%pc),%a0
    moveq   #0,%d0
    move.l  %a3,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne.s   91f
    move.w  #9,28(%a3)              /* IND_ADDHANDLER: io_Data the handler's Interrupt */
    lea     hint(%pc),%a0
    lea     handler(%pc),%a1
    move.l  %a1,18(%a0)             /* is_Code */
    move.l  %a0,40(%a3)
    move.l  %a3,%a1
    jsr     LVO_DoIO(%a6)
    tst.l   %d0
    bne.s   91f
    move.w  #10,28(%a3)             /* IND_REMHANDLER */
    move.l  %a3,%a1
    jsr     LVO_DoIO(%a6)
    tst.l   %d0
    bne.s   91f
    move.l  %a3,%a1
    jsr     LVO_CloseDevice(%a6)
    bra.s   2f
91: lea     n_inp(%pc),%a0
    bsr.w   wrong
    /* intuition ViewAddress */
2:  lea     intname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq.s   92f
    move.l  %d0,%a4
    exg     %a4,%a6
    jsr     LVO_ViewAddress(%a6)
    exg     %a4,%a6
    move.l  %d0,%d2
    move.l  %a4,%a1
    jsr     LVO_CloseLibrary(%a6)
    tst.l   %d2
    bne.s   3f
92: lea     n_view(%pc),%a0
    bsr.w   wrong
    /* AllocMem of 1 GB: refused (ZINE's line) */
3:  move.l  #0x40000000,%d0
    moveq   #0,%d1
    jsr     LVO_AllocMem(%a6)
    tst.l   %d0
    beq.s   4f
    lea     n_alloc(%pc),%a0
    bsr.w   wrong
4:  tst.l   %d7
    bne.s   5f
    lea     allright(%pc),%a0
    bsr.w   say
5:  move.l  %a5,%a1
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
handler:
    move.l  %a0,%d0
    rts
wrong:                              /* a0: the line */
    addq.l  #1,%d7
say:                                /* a0: a 0-ended line */
    movem.l %d2-%d3/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    move.l  %a5,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d2-%d3/%a6
    rts
dosname: .asciz "dos.library"
intname: .asciz "intuition.library"
conname: .asciz "console.device"
inpname: .asciz "input.device"
text:    .ascii "zcon: written through \x9b", "1mconsole.device\x9b", "0m\n"
         .byte 0
textend:
allright: .asciz "zcon: all answered\n"
n_con:   .asciz "zcon: wrong: console.device\n"
n_inp:   .asciz "zcon: wrong: input.device\n"
n_view:  .asciz "zcon: wrong: ViewAddress\n"
n_alloc: .asciz "zcon: wrong: AllocMem of 1 GB given\n"
    .balign 4
hint:    .long 0, 0                 /* ln_Succ, ln_Pred */
         .byte 2, 0                 /* NT_INTERRUPT, priority 0 */
         .long 0, 0, 0              /* ln_Name, is_Data, is_Code */
io:      .space 64
buf:     .space 8
