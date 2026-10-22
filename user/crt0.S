/* Program entry. The kernel loads and relocates the ZEXE image, clears
 * bss, builds the stack and jumps to offset 0. At entry the stack holds
 * argc and argv. */
    .section .text.start
    .globl  _start
_start:
    bra.w   _entry
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
_entry:
    move.l  (%sp),%d0               /* argc */
    move.l  4(%sp),%d1              /* argv */
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    jsr     main
    move.l  %d0,-(%sp)
    jsr     _zexit                  /* flushes buffered output, then SYS_exit */
1:  bra.s   1b
