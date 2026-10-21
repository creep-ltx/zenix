/* Program entry. The kernel loads and relocates the ZEXE image, clears
 * bss, sets the stack and jumps to offset 0. */
    .section .text.start
    .globl  _start
_start:
    bra.w   _entry
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
_entry:
    jsr     main
    move.l  %d0,%d1
    moveq   #1,%d0                  /* SYS_exit */
    trap    #0
1:  bra.s   1b
