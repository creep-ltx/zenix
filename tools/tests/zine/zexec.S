/* SPDX-License-Identifier: GPL-2.0-only */
/* zexec: a demo made of separate programs, as Spaceballs' HOG (docs/d29-zine.md 16.11): the main program runs its
 * parts (zpart, tools/tests/zine/zpart.S, in its folder and as Parts/Second) with dos Execute, each run for real:
 *   2  "zpart >NIL: one  "two words"": DOSTRUE, IoErr() 5 (its return code), cli_ReturnCode 5; RAM:zpart.one holds
 *      its argument line as given ("one  "two words"\n"); the line it wrote to Output() went to NIL:
 *   3  "parts/SECOND two": a part in a subfolder, its name in another case: DOSTRUE, IoErr() 0 (it says the rest)
 *   4  "zpart nest": a part that Executes "run >NIL: zpart deep" (which ends with Exit(7)): IoErr() 3,
 *      RAM:zpart.deep written
 *   5  "zpart loop": parts inside parts, the ninth refused: DOSTRUE
 *   6  "nosuchpart x": DOSFALSE, IoErr() ERROR_OBJECT_NOT_FOUND (205)
 *   7  "stack 20000": not run, DOSTRUE
 *   8  the marks deleted, AvailMem as at the start (every part's segments and stack freed)
 * Exit 0 when everything held; else the return code names the step (and a line says it). PC-relative code. */
#include "zt.i"
#define LVO_Open        -30
#define LVO_Close       -36
#define LVO_Read        -42
#define LVO_IoErr       -132
#define LVO_Execute     -222
#define LVO_DeleteFile  -72
#define LVO_FindTask    -294
#define MODE_OLDFILE    1005
#define V_dos   0
#define V_mem   4
#define V_end   8
#define BUF     V_end           /* 64 bytes */

    .text
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    moveq   #1,%d6
    move.l  4.w,%a6
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V_dos(%a4)
    beq     fail
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d7
    move.l  4.w,%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  %d0,V_mem(%a4)
    move.l  V_dos(%a4),%a6
    /* 2: a part with arguments, its output to NIL:, its return code */
    moveq   #2,%d6
    lea     c_one(%pc),%a0
    bsr     exec
    bne     fail
    moveq   #5,%d1
    cmp.l   %d1,%d0
    bne     fail
    move.l  4.w,%a6
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,%a0
    move.l  172(%a0),%d0        /* pr_CLI -> cli_ReturnCode */
    lsl.l   #2,%d0
    move.l  %d0,%a0
    moveq   #5,%d1
    cmp.l   12(%a0),%d1
    bne     fail
    move.l  V_dos(%a4),%a6
    lea     f_one(%pc),%a0
    bsr     rfile
    moveq   #a_oneend-a_one,%d1
    cmp.l   %d1,%d0
    bne     fail
    lea     BUF(%a4),%a0
    lea     a_one(%pc),%a1
    subq.l  #1,%d0
1:  cmpm.b  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne     fail
    lea     m_one(%pc),%a0
    bsr     puts
    /* 3: a part in a subfolder, named in another case */
    moveq   #3,%d6
    lea     c_two(%pc),%a0
    bsr     exec
    bne     fail
    tst.l   %d0
    bne     fail
    /* 4: a part that runs a part */
    moveq   #4,%d6
    lea     c_nest(%pc),%a0
    bsr     exec
    bne     fail
    moveq   #3,%d1
    cmp.l   %d1,%d0
    bne     fail
    lea     f_deep(%pc),%a0
    bsr     rfile
    moveq   #5,%d1
    cmp.l   %d1,%d0
    bne     fail
    lea     m_nest(%pc),%a0
    bsr     puts
    /* 5: parts inside parts up to ZINE's limit */
    moveq   #5,%d6
    lea     c_loop(%pc),%a0
    bsr     exec
    bne     fail
    /* 6: a name that is no program */
    moveq   #6,%d6
    lea     c_none(%pc),%a0
    bsr     exec
    beq     fail
    cmp.l   #205,%d0
    bne     fail
    lea     m_none(%pc),%a0
    bsr     puts
    /* 7: a command of the Shell's: not run */
    moveq   #7,%d6
    lea     c_stack(%pc),%a0
    bsr     exec
    bne     fail
    /* 8: the marks deleted, the memory as at the start */
    moveq   #8,%d6
    lea     f_one(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    beq     fail
    lea     f_deep(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    beq     fail
    move.l  4.w,%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  V_dos(%a4),%a6
    cmp.l   V_mem(%a4),%d0
    bne     fail
    lea     m_mem(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra.s   done
fail:
    lea     m_fail(%pc),%a0
    bsr     puts
    move.l  %d6,%d0
done:
    move.l  %d0,%d2
    move.l  4.w,%a6
    move.l  V_dos(%a4),%d0
    beq.s   1f
    move.l  %d0,%a1
    jsr     LVO_CloseLibrary(%a6)
1:  move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* exec: Execute(a0, 0, 0): Z set when it answered DOSTRUE; d0 IoErr() */
exec:
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    jsr     LVO_Execute(%a6)
    move.l  %d0,%d2
    jsr     LVO_IoErr(%a6)
    moveq   #-1,%d1
    cmp.l   %d1,%d2
    rts

/* rfile: a0's first 64 bytes into BUF -> d0 the count (-1 when it cannot be opened) */
rfile:
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    bne.s   1f
    moveq   #-1,%d0
    rts
1:  move.l  %d4,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_Read(%a6)
    move.l  %d0,%d5
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    move.l  %d5,%d0
    rts

/* puts: a0 a text ending in 0, to d7 */
puts:
    movem.l %d0-%d3/%a0-%a1/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   0(%a0,%d3.l)
    bne.s   1b
    move.l  %d7,%d1
    move.l  V_dos(%a4),%a6
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3/%a0-%a1/%a6
    rts

dosname:  .asciz "dos.library"
c_one:    .asciz "zpart >NIL: one  \"two words\"  "
c_two:    .asciz "parts/SECOND two"
c_nest:   .asciz "zpart nest"
c_loop:   .asciz "zpart loop"
c_none:   .asciz "nosuchpart x"
c_stack:  .asciz "stack 20000"
f_one:    .asciz "RAM:zpart.one"
f_deep:   .asciz "RAM:zpart.deep"
a_one:    .ascii "one  \"two words\"\n"
a_oneend:
m_one:    .asciz "zexec: part one ran with its arguments, its output to NIL:, return code 5\n"
m_nest:   .asciz "zexec: the nested parts ran, return code 3\n"
m_none:   .asciz "zexec: a missing part answered DOSFALSE, ERROR_OBJECT_NOT_FOUND\n"
m_mem:    .asciz "zexec: memory as at the start\n"
m_fail:   .asciz "zexec: failed (the return code names the step)\n"
    .even
vars:   .space  V_end
    .space  64
