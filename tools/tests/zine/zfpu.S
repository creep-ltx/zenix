/* SPDX-License-Identifier: GPL-2.0-only */
/* zfpu: FSIN, which a 68040's or 68060's FPU lacks (AmigaOS's 68040/68060.library emulates it; ZINE does not), as
 * does a machine with no FPU: there zine's crash line must name it. On a CPU that has it (a 68881/68882; Emu68) it
 * runs and the program returns 0. */
#include "zt.i"
    .text
    .word   0xF200,0x000E           /* fsin.x fp0 */
    moveq   #0,%d0
    rts
