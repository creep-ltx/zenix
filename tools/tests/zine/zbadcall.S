/* SPDX-License-Identifier: GPL-2.0-only */
/* zbadcall: calls exec.library's PutMsg (offset -366), which ZINE does not have yet; ZINE must stop the program,
 * name the call and give Zenix back. */
#include "zt.i"
    .text
    move.l  4.w,%a6
    sub.l   %a0,%a0
    sub.l   %a1,%a1
    jsr     LVO_PutMsg(%a6)
    moveq   #0,%d0
    rts
