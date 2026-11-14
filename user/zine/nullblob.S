/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's null blob (DESIGN D29 section 16 step 2, tests only: /tests/zine/null.blob): the kernel's pause and resume
 * with nothing in between. Entered like the real blob (supervisor mode, interrupts masked, struct zine_args * on the
 * stack); it touches no hardware and goes straight back through the kernel's resume routine with the answer 0 and a
 * session of 0 seconds. zine then reads the ZINE block as it left it (how 0: "ended in an unknown way (0)"); the test
 * checks the kernel's "zine: back after 0 s (0)" line and that everything works afterwards. */
    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   1f
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
    .ascii  "ZINB"
    .long   2                       /* kernel/zine.h ZINE_LAYOUT */
    .long   0
1:  move.l  4(%sp),%a0              /* struct zine_args * */
    move.l  4(%a0),%a1              /* its resume routine */
    moveq   #0,%d0
    moveq   #0,%d1
    jmp     (%a1)
