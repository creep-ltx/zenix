/* Zenix user program entry. The kernel builds the initial stack as
 * [argc][argv0..argvN][NULL][strings...]; pop argc, the array address
 * is argv, call main(argc, argv), exit with its return value. */
    .section .text.ustart
    .globl  _ustart
_ustart:
    move.l  (%sp)+,%d0              /* argc */
    move.l  %sp,%d1                 /* argv */
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    jsr     main
    addq.l  #8,%sp
    move.l  %d0,%d1                 /* exit code */
    moveq   #1,%d0                  /* SYS_exit */
    trap    #0
1:  bra.s   1b
