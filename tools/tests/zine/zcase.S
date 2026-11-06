/* SPDX-License-Identifier: GPL-2.0-only */
/* zcase: names found without case, the AmigaOS way (docs/d29-zine.md 16.10), in subdirectories of the program's
 * folder. The shell makes the folder (tools/tests/zine-null-test.txt): Data/XYL.TP3 ("upper"), Data/Deep/Er/Mod.X
 * ("deeper"), Data/clash ("low") and Data/CLASH ("UP"), Data/Deep/Er/zexit, LIBS/ZT.Library (zt.library). zcase opens
 * DATA/Xyl.Tp3, PROGDIR:data/xyl.tp3 and SYS:data/deep/er/mod.x; Data/clash and Data/CLASH (each the exact one) and
 * Data/Clash (the first found: clash); LoadSegs data/DEEP/er/ZEXIT; OpenLibrary("zt.library") from LIBS: (the folder's
 * LIBS dir); writes data/NEW.txt and reads DATA/new.TXT; writes DATA/xyl.TP3 (shadowing the file) and reads
 * Data/XYL.TP3 back as written; deletes data/Xyl.tp3: not found after. Exit 0 when everything held; else the return
 * code names the step (and a line says it). PC-relative code. */
#include "zt.i"
#define LVO_DeleteFile  -72
#define LVO_Open        -30
#define LVO_Close       -36
#define LVO_IoErr       -132
#define LVO_Read        -42
#define LVO_LoadSeg     -150
#define LVO_UnLoadSeg   -156
#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define V_dos   0
#define V_end   4
#define BUF     V_end           /* the read-back scratch (64 bytes) after the variables */

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
    /* 2-4: a file in a subdirectory by names of other case, two and three levels down */
    moveq   #2,%d6
    lea     f_x1(%pc),%a0
    lea     t_upper(%pc),%a1
    bsr     expect
    moveq   #3,%d6
    lea     f_x2(%pc),%a0
    lea     t_upper(%pc),%a1
    bsr     expect
    moveq   #4,%d6
    lea     f_mod(%pc),%a0
    lea     t_deeper(%pc),%a1
    bsr     expect
    lea     m_found(%pc),%a0
    bsr     puts
    /* 5-7: two names the same but for case: the exact one, else the first found */
    moveq   #5,%d6
    lea     f_cl(%pc),%a0
    lea     t_low(%pc),%a1
    bsr     expect
    moveq   #6,%d6
    lea     f_CL(%pc),%a0
    lea     t_UP(%pc),%a1
    bsr     expect
    moveq   #7,%d6
    lea     f_Cl(%pc),%a0
    lea     t_low(%pc),%a1
    bsr     expect
    lea     m_twins(%pc),%a0
    bsr     puts
    /* 8: LoadSeg by another case */
    moveq   #8,%d6
    lea     f_seg(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_LoadSeg(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%d1
    jsr     LVO_UnLoadSeg(%a6)
    /* 9: OpenLibrary from LIBS:, the folder's LIBS/ZT.Library */
    moveq   #9,%d6
    move.l  4.w,%a6
    lea     libname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%a1
    jsr     LVO_CloseLibrary(%a6)
    move.l  V_dos(%a4),%a6
    lea     m_load(%pc),%a0
    bsr     puts
    /* 10: a written file read back by another case */
    moveq   #10,%d6
    lea     f_new(%pc),%a0
    bsr     create
    lea     f_new2(%pc),%a0
    lea     text(%pc),%a1
    bsr     expect
    /* 11: the volume's file shadowed by a write of another case, read back by a third */
    moveq   #11,%d6
    lea     f_x3(%pc),%a0
    bsr     create
    lea     f_x4(%pc),%a0
    lea     text(%pc),%a1
    bsr     expect
    /* 12: deleted by a fourth: not found by any */
    moveq   #12,%d6
    lea     f_x5(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    beq     fail
    lea     f_x1(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne     fail
    lea     m_write(%pc),%a0
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

/* expect: a0 read whole (up to 64 bytes): the text at a1 (0-ended) and nothing more */
expect:
    move.l  %a1,%a3
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    lea     BUF(%a4),%a0
    moveq   #15,%d0
1:  clr.l   (%a0)+
    dbra    %d0,1b
    move.l  %d4,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_Read(%a6)
    move.l  %d0,%d5
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    lea     BUF(%a4),%a0
    moveq   #0,%d0
2:  tst.b   (%a3)
    beq.s   3f
    cmpm.b  (%a3)+,(%a0)+
    bne.s   9f
    addq.l  #1,%d0
    bra.s   2b
3:  cmp.l   %d0,%d5
    bne.s   9f
    rts
9:  addq.l  #4,%sp
    bra     fail

/* create: a0 opened MODE_NEWFILE, the text written, closed */
create:
    move.l  #MODE_NEWFILE,%d2
    bsr     open
    move.l  %d4,%d1
    lea     text(%pc),%a0
    move.l  %a0,%d2
    moveq   #textend-text,%d3
    jsr     LVO_Write(%a6)
    moveq   #textend-text,%d1
    cmp.l   %d1,%d0
    bne.s   9f
    move.l  %d4,%d1
    jmp     LVO_Close(%a6)
9:  addq.l  #4,%sp
    bra     fail

/* open: Open(a0, d2) -> d4, fail from the caller's caller when 0 */
open:
    move.l  %a0,%d1
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    beq.s   9f
    rts
9:  addq.l  #8,%sp
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
libname:  .asciz "zt.library"
f_x1:     .asciz "DATA/Xyl.Tp3"
f_x2:     .asciz "PROGDIR:data/xyl.tp3"
f_x3:     .asciz "DATA/xyl.TP3"
f_x4:     .asciz "Data/XYL.TP3"
f_x5:     .asciz "data/Xyl.tp3"
f_mod:    .asciz "SYS:data/deep/er/mod.x"
f_cl:     .asciz "Data/clash"
f_CL:     .asciz "Data/CLASH"
f_Cl:     .asciz "Data/Clash"
f_seg:    .asciz "data/DEEP/er/ZEXIT"
f_new:    .asciz "data/NEW.txt"
f_new2:   .asciz "DATA/new.TXT"
t_upper:  .asciz "upper\n"
t_deeper: .asciz "deeper\n"
t_low:    .asciz "low\n"
t_UP:     .asciz "UP\n"
text:     .ascii "zcase wrote this\n"
textend:
          .byte 0
m_found:  .asciz "zcase: files in subdirectories found by names of another case\n"
m_twins:  .asciz "zcase: two names the same but for case: the exact one, else the first found\n"
m_load:   .asciz "zcase: LoadSeg and OpenLibrary from LIBS: by names of another case\n"
m_write:  .asciz "zcase: written, shadowed and deleted by names of another case\n"
m_fail:   .asciz "zcase: failed (the return code names the step)\n"
    .even
vars:   .space  V_end
    .space  64
