/* SPDX-License-Identifier: GPL-2.0-only */
/* zexit: the smallest AmigaOS program: returns 7 to its caller (ZINE reports "return code 7"). */
#include "zt.i"
    .text
    moveq   #7,%d0
    rts
