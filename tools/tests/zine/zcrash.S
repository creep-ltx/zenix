/* SPDX-License-Identifier: GPL-2.0-only */
/* zcrash: runs an ILLEGAL instruction (vector 4); ZINE must say "crashed: illegal instruction" and give Zenix back. */
#include "zt.i"
    .text
    nop
    .word   0x4afc                  /* ILLEGAL */
    moveq   #0,%d0
    rts
