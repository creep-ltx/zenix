/* SPDX-License-Identifier: GPL-2.0-only */
/* Program entry. The kernel loads and relocates the program, clears bss, builds the stack and jumps to offset 0. At
 * entry the stack holds argc and argv, just where the C rules put a function's two arguments after its return
 * address: so the start is one PC-relative call to _zstart (libz), which sets _zargv, calls main and then _zexit.
 * No hand-written code reaches a global and nothing here needs relocating (DESIGN D13 step 3a).
 * Built with -DZEX2 for a program with shared code (link/pure.ld): the ZEX2 header (8 words after the magic, filled
 * in by tools/mkzexe.py) instead of the ZEXE one (6 words). */
    .section .text.start
    .globl  _start
_start:
    bra.w   _entry
#ifdef ZEX2
    .ascii  "ZEX2"
    .long   0, 0, 0, 0, 0, 0, 0, 0
#else
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
#endif
_entry:
    bsr.l   _zstart                 /* never returns */
1:  bra.s   1b
