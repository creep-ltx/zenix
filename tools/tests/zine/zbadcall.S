/* SPDX-License-Identifier: GPL-2.0-only */
/* zbadcall: calls exec.library's ObtainSemaphore (offset -564), which ZINE does not have yet; ZINE must stop the program,
 * name the call and give Zenix back. */
#include "zt.i"
    .text
    move.l  4.w,%a6
    sub.l   %a0,%a0
    sub.l   %a1,%a1
    jsr     LVO_ObtainSemaphore(%a6)
    moveq   #0,%d0
    rts
