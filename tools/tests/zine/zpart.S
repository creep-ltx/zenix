/* SPDX-License-Identifier: GPL-2.0-only */
/* zpart: a part of a demo made of separate programs (Spaceballs' HOG: docs/d29-zine.md 16.11), run by zexec through
 * dos Execute. Every run checks its start as a Shell gives it: d0 the argument line's length, (a0) the line ending in
 * "\n", 4(sp) its stack's size, the task's tc_SPLower/tc_SPUpper that size apart (12,000 bytes: the $STACK: cookie
 * below), pr_CLI's cli_Module its segment list (a3's link >> 2). The argument's first letter says what it does:
 *   o  ("one ..."): writes its argument line to RAM:zpart.one, writes a line to Output() (which zexec sends to NIL:)
 *      and returns 5
 *   t  ("two"): run as Parts/SECOND, from a subfolder: reads PROGDIR:here (its own folder's) and says its
 *      cli_CommandName; returns 0
 *   n  ("nest"): Executes "run >NIL: zpart deep", which writes RAM:zpart.deep and ends with dos Exit(7); answered
 *      DOSTRUE with IoErr() 7; returns 3
 *   l  ("loop"): Executes "zpart loop" again: ZINE runs 8 inside one another, the next is refused (DOSFALSE), said
 *   d  ("deep"): see n
 *   h  ("hand"): hands its segments over in cli_Module as a decruncher does (CrunchMania's first hunk, The Black
 *      Lotus' Tint: d29 16.21): a 64 KB block made a segment (its size, then the link to the old cli_Module's list)
 *      is cli_Module now; the Shell unloads what cli_Module holds when the command ends, so zexec's AvailMem is as
 *      at its start only when that list is unloaded, not the one loaded; returns 0
 * A failed check returns 40 + the step and says so. PC-relative code, one hunk. */
#include "zt.i"
#define LVO_Open        -30
#define LVO_Close       -36
#define LVO_Read        -42
#define LVO_IoErr       -132
#define LVO_Execute     -222
#define LVO_FindTask    -294
#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define V_dos   0
#define V_end   4
#define BUF     V_end           /* 64 bytes */

    .text
main:
    move.l  4(%sp),%d6          /* the stack's size */
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    move.l  %a0,%a2             /* the arguments */
    move.l  %d0,%d5
    move.l  %a3,%d4             /* the segment list's first link (BPTR << 2) */
    move.l  4.w,%a6
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V_dos(%a4)
    beq     f41
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d7
    moveq   #42,%d3             /* 42: the argument line */
    tst.l   %d5
    beq     fail
    cmp.b   #10,-1(%a2,%d5.l)
    bne     fail
    moveq   #43,%d3             /* 43: the stack */
    cmp.l   #12000,%d6
    bne     fail
    move.l  4.w,%a6
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,%a5
    move.l  62(%a5),%d0         /* tc_SPUpper - tc_SPLower */
    sub.l   58(%a5),%d0
    cmp.l   %d6,%d0
    bne     fail
    moveq   #44,%d3             /* 44: cli_Module */
    move.l  172(%a5),%d0        /* pr_CLI */
    lsl.l   #2,%d0
    move.l  %d0,%a3
    move.l  60(%a3),%d0         /* cli_Module */
    lsl.l   #2,%d0
    cmp.l   %d4,%d0
    bne     fail
    move.l  V_dos(%a4),%a6
    move.b  (%a2),%d0
    cmp.b   #0x6F,%d0           /* o */
    beq     one
    cmp.b   #0x74,%d0           /* t */
    beq     two
    cmp.b   #0x6E,%d0           /* n */
    beq     nest
    cmp.b   #0x6C,%d0           /* l */
    beq     loop
    cmp.b   #0x64,%d0           /* d */
    beq     deep
    cmp.b   #0x68,%d0           /* h */
    beq     hand
    moveq   #45,%d3
    bra     fail

one:
    lea     f_one(%pc),%a0      /* the argument line, as given */
    move.l  %a2,%a1
    move.l  %d5,%d0
    bsr     wfile
    lea     m_nil(%pc),%a0
    bsr     puts
    moveq   #5,%d0
    bra     done

two:
    moveq   #46,%d3
    lea     f_here(%pc),%a0     /* PROGDIR: is the part's own folder */
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    beq     fail
    move.l  %d4,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_Read(%a6)
    move.l  %d0,%d2
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    moveq   #46,%d3
    cmp.l   #4,%d2              /* "sub\n" */
    bne     fail
    cmp.l   #0x7375620A,BUF(%a4)
    bne     fail
    lea     m_two(%pc),%a0
    bsr     puts
    move.l  16(%a3),%d0         /* cli_CommandName, a BSTR */
    lsl.l   #2,%d0
    move.l  %d0,%a0
    moveq   #0,%d3
    move.b  (%a0)+,%d3
    move.l  %a0,%d2
    move.l  %d7,%d1
    jsr     LVO_Write(%a6)
    lea     m_nl(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra     done

nest:
    lea     c_deep(%pc),%a0
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    jsr     LVO_Execute(%a6)
    moveq   #47,%d3
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    moveq   #48,%d3
    moveq   #7,%d1
    cmp.l   %d1,%d0
    bne     fail
    lea     m_nest(%pc),%a0
    bsr     puts
    moveq   #3,%d0
    bra     done

deep:
    lea     f_deep(%pc),%a0
    lea     m_deepf(%pc),%a1
    moveq   #m_deepfe-m_deepf,%d0
    bsr     wfile
    moveq   #7,%d1              /* dos Exit: back to the Execute that ran it */
    jsr     LVO_Exit(%a6)
    moveq   #49,%d3             /* not reached */
    bra     fail

loop:
    lea     c_loop(%pc),%a0
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    jsr     LVO_Execute(%a6)
    tst.l   %d0
    bne.s   1f
    jsr     LVO_IoErr(%a6)
    lea     m_limit(%pc),%a0
    bsr     puts
1:  moveq   #0,%d0
    bra     done

hand:
    moveq   #51,%d3
    move.l  4.w,%a6
    move.l  #65536,%d0
    moveq   #1,%d1              /* MEMF_PUBLIC */
    jsr     LVO_AllocMem(%a6)
    move.l  V_dos(%a4),%a6
    tst.l   %d0
    beq     fail
    move.l  %d0,%a0
    move.l  #65536,(%a0)        /* a segment as LoadSeg makes it: the size, then the link (BPTR) */
    move.l  60(%a3),4(%a0)      /* to zpart's own list: unloading the new list frees both */
    addq.l  #4,%d0
    lsr.l   #2,%d0
    move.l  %d0,60(%a3)         /* cli_Module */
    lea     m_hand(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra     done

f41: moveq  #41,%d3
fail:
    lea     m_fail(%pc),%a0
    bsr     puts
    move.l  %d3,%d0
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

/* wfile: a0 the name, a1 the bytes, d0 their count: written to a new file (fail from the caller when not) */
wfile:
    movem.l %d0/%a1,-(%sp)
    move.l  %a0,%d1
    move.l  #MODE_NEWFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    movem.l (%sp)+,%d3/%a1
    beq.s   9f
    move.l  %d4,%d1
    move.l  %a1,%d2
    jsr     LVO_Write(%a6)
    move.l  %d0,%d2
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    rts
9:  addq.l  #4,%sp
    moveq   #50,%d3
    bra     fail

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
cookie:   .asciz "$STACK:12000"
f_one:    .asciz "RAM:zpart.one"
f_deep:   .asciz "RAM:zpart.deep"
f_here:   .asciz "PROGDIR:here"
c_deep:   .asciz "run >NIL: zpart deep"
c_loop:   .asciz "zpart loop"
m_deepf:  .ascii "deep\n"
m_deepfe:
m_nil:    .asciz "zpart: this line goes to NIL: and must not be seen\n"
m_two:    .asciz "zpart: two ran from its subfolder, PROGDIR:here its own; its command name: "
m_nl:     .asciz "\n"
m_nest:   .asciz "zpart: the nested part ended with Exit(7), its return code from IoErr()\n"
m_limit:  .asciz "zpart: the ninth loop refused at ZINE's limit\n"
m_hand:   .asciz "zpart: its segments handed over in cli_Module, a 64 KB segment first\n"
m_fail:   .asciz "zpart: failed (the return code names the step)\n"
    .even
vars:   .space  V_end
    .space  64
