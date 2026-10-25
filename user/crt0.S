/* SPDX-License-Identifier: GPL-2.0-only */
/* Program entry. The kernel loads and relocates the ZEXE image, clears
 * bss, builds the stack and jumps to offset 0. At entry the stack holds
 * argc and argv.
 * Built with -DZEX2 for a program with shared code (DESIGN D13, link/pure.ld): the ZEX2 header (8 words after the
 * magic, filled in by tools/mkzexe.py) and a start with no absolute address in it: _zstart (libz) is reached
 * PC-relative and finds argc and argv where the C rules put a function's arguments. */
    .section .text.start
    .globl  _start
_start:
    bra.w   _entry
#ifdef ZEX2
    .ascii  "ZEX2"
    .long   0, 0, 0, 0, 0, 0, 0, 0
_entry:
    bsr.l   _zstart                 /* sets _zargv, calls main, then _zexit: never returns */
#else
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
_entry:
    move.l  (%sp),%d0               /* argc */
    move.l  4(%sp),%d1              /* argv */
    move.l  %d1,_zargv              /* for libz's messages (xmalloc) */
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    jsr     main
    move.l  %d0,-(%sp)
    jsr     _zexit                  /* flushes buffered output, then SYS_exit */
#endif
1:  bra.s   1b
