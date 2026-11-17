/* SPDX-License-Identifier: GPL-2.0-only */
/* zwrite: a write to any path is kept in memory for the session (docs/d29-zine.md 16.9), never on a disk. Writes
 * T:zw.tmp, ENV:Zw/pref, PROGDIR:zw.new and DH0:zw/deep and reads each back by another form of its path (case, "zw.new"
 * for "PROGDIR:zw.new", "SYS:zw.new"); NIL: still takes and keeps nothing; zexit (a file of its own folder) shadowed by a
 * new PROGDIR:zexit (read back as zexit); zhello opened MODE_READWRITE, changed and read back (a copy in memory);
 * DeleteFile of the shadowed zexit, of zbadcall (never written) and of T:zw.tmp: each then not found, a second delete
 * ERROR_OBJECT_NOT_FOUND; SetProtection, SetComment, SetFileDate, FGetC, UnGetC, FGets (16.24). The shell checks afterwards that the disk has none of it. Exit 0 when everything held; else
 * the return code names the step (and a line says it). PC-relative code. */
#include "zt.i"
#define LVO_DeleteFile  -72
#define LVO_Open        -30
#define LVO_Close       -36
#define LVO_IoErr       -132
#define LVO_Read        -42
#define LVO_Seek        -66
#define LVO_SetComment  -180
#define LVO_SetProtection -186
#define LVO_SetFileDate -396
#define LVO_FGetC       -306
#define LVO_UnGetC      -318
#define LVO_FGets       -336
#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
#define MODE_READWRITE  1004
#define OFFSET_BEGINNING -1
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
    /* 2-5: four paths written, each read back by another form of its name */
    moveq   #2,%d6
    lea     f_t(%pc),%a0
    lea     f_t2(%pc),%a1
    bsr     roundtrip
    moveq   #3,%d6
    lea     f_env(%pc),%a0
    lea     f_env2(%pc),%a1
    bsr     roundtrip
    moveq   #4,%d6
    lea     f_new(%pc),%a0
    lea     f_new2(%pc),%a1
    bsr     roundtrip
    lea     f_new3(%pc),%a0
    bsr     readback
    moveq   #5,%d6
    lea     f_dh0(%pc),%a0
    lea     f_dh02(%pc),%a1
    bsr     roundtrip
    lea     m_paths(%pc),%a0
    bsr     puts
    /* 6: NIL: takes the bytes and keeps none */
    moveq   #6,%d6
    lea     f_nil(%pc),%a0
    move.l  #MODE_NEWFILE,%d2
    bsr     open
    bsr     write
    bsr     close
    lea     f_nil(%pc),%a0
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    bsr     read64
    bsr     close
    tst.l   %d5
    bne     fail
    /* 7: zexit (one of its own files) read, then shadowed by PROGDIR:zexit and read back as zexit */
    moveq   #7,%d6
    lea     f_zexit(%pc),%a0
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    bsr     read64
    bsr     close
    cmp.l   #0x3F3,BUF(%a4)     /* HUNK_HEADER: the real file */
    bne     fail
    lea     f_zexit2(%pc),%a0
    move.l  #MODE_NEWFILE,%d2
    bsr     open
    bsr     write
    bsr     close
    lea     f_zexit(%pc),%a0
    bsr     readback
    lea     m_shadow(%pc),%a0
    bsr     puts
    /* 8: zhello opened MODE_READWRITE, its first two bytes changed: the rest is the file's own */
    moveq   #8,%d6
    lea     f_zhello(%pc),%a0
    move.l  #MODE_READWRITE,%d2
    bsr     open
    bsr     read64
    moveq   #64,%d0
    cmp.l   %d0,%d5
    bne     fail
    cmp.l   #0x3F3,BUF(%a4)
    bne     fail
    move.l  %d4,%d1
    moveq   #0,%d2
    moveq   #OFFSET_BEGINNING,%d3
    jsr     LVO_Seek(%a6)
    move.l  %d4,%d1
    lea     t_xy(%pc),%a0
    move.l  %a0,%d2
    moveq   #2,%d3
    jsr     LVO_Write(%a6)
    moveq   #2,%d1
    cmp.l   %d1,%d0
    bne     fail
    bsr     close
    lea     f_zhello(%pc),%a0
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    bsr     read64
    bsr     close
    cmp.l   #0x585903F3,BUF(%a4)        /* "XY", then the file's own 03 F3 */
    bne     fail
    lea     m_rw(%pc),%a0
    bsr     puts
    /* 9: the shadowed zexit deleted: not found, a second delete ERROR_OBJECT_NOT_FOUND */
    moveq   #9,%d6
    lea     f_zexit(%pc),%a0
    bsr     delete
    /* 10: zbadcall, never written, deleted (hidden for the session only) */
    moveq   #10,%d6
    lea     f_zbad(%pc),%a0
    bsr     delete
    /* 11: T:zw.tmp deleted */
    moveq   #11,%d6
    lea     f_t2(%pc),%a0
    bsr     delete
    lea     m_del(%pc),%a0
    bsr     puts
    /* 12 (16.24): SetProtection of a written file (zw.new), SetComment of one of its own (zhello), SetFileDate of the
     * same by another name (SYS:zw.new): DOSTRUE each; SetProtection of the deleted zexit: 0, ERROR_OBJECT_NOT_FOUND */
    moveq   #12,%d6
    lea     f_new2(%pc),%a0
    move.l  %a0,%d1
    moveq   #0,%d2
    jsr     LVO_SetProtection(%a6)
    tst.l   %d0
    beq     fail
    lea     f_zhello(%pc),%a0
    move.l  %a0,%d1
    lea     text(%pc),%a0
    move.l  %a0,%d2
    jsr     LVO_SetComment(%a6)
    tst.l   %d0
    beq     fail
    lea     f_new3(%pc),%a0
    move.l  %a0,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_SetFileDate(%a6)
    tst.l   %d0
    beq     fail
    lea     f_zexit(%pc),%a0
    move.l  %a0,%d1
    moveq   #0,%d2
    jsr     LVO_SetProtection(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne     fail
    lea     m_attr(%pc),%a0
    bsr     puts
    /* 13 (16.24): zw.new read with FGetC ('z'), UnGetC (-1: the same byte again), FGets (the whole line with its
     * newline, buf answered), FGets again (0: the end) */
    moveq   #13,%d6
    lea     f_new2(%pc),%a0
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    move.l  %d4,%d1
    jsr     LVO_FGetC(%a6)
    moveq   #'z',%d1
    cmp.l   %d1,%d0
    bne     fail
    move.l  %d4,%d1
    moveq   #-1,%d2
    jsr     LVO_UnGetC(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d4,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_FGets(%a6)
    lea     BUF(%a4),%a0
    cmp.l   %a0,%d0
    bne     fail
    lea     text(%pc),%a1
    moveq   #textend-text-1,%d0
1:  cmpm.b  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne     fail
    tst.b   (%a0)
    bne     fail
    move.l  %d4,%d1
    lea     BUF(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_FGets(%a6)
    tst.l   %d0
    bne     fail
    bsr     close
    lea     m_fgets(%pc),%a0
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

/* roundtrip: a0 written with the text, then a1 read back: the same text */
roundtrip:
    move.l  %a1,-(%sp)
    move.l  #MODE_NEWFILE,%d2
    bsr     open
    bsr     write
    bsr     close
    move.l  (%sp)+,%a0
/* readback: a0 read: the text and nothing after it */
readback:
    move.l  #MODE_OLDFILE,%d2
    bsr     open
    bsr     read64
    bsr     close
    moveq   #textend-text,%d0
    cmp.l   %d0,%d5
    bne.s   9f
    lea     BUF(%a4),%a0
    lea     text(%pc),%a1
    subq.l  #1,%d0
1:  cmpm.b  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne.s   9f
    rts
9:  addq.l  #4,%sp
    bra     fail

/* delete: DeleteFile(a0) answers DOSTRUE, then Open(a0) and a second delete fail with ERROR_OBJECT_NOT_FOUND */
delete:
    move.l  %a0,%a2
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    beq.s   9f
    move.l  %a2,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    tst.l   %d0
    bne.s   9f
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne.s   9f
    move.l  %a2,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    bne.s   9f
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne.s   9f
    rts
9:  addq.l  #4,%sp
    bra     fail

/* open: Open(a0, d2) -> d4, fail from the caller when 0 */
open:
    move.l  %a0,%d1
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    beq.s   9f
    rts
9:  addq.l  #4,%sp
    bra     fail
/* write: the text to d4, all of it */
write:
    move.l  %d4,%d1
    lea     text(%pc),%a0
    move.l  %a0,%d2
    moveq   #textend-text,%d3
    jsr     LVO_Write(%a6)
    moveq   #textend-text,%d1
    cmp.l   %d1,%d0
    bne.s   9f
    rts
9:  addq.l  #4,%sp
    bra     fail
/* read64: up to 64 bytes of d4 into BUF (cleared first) -> d5 */
read64:
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
    rts
close:
    move.l  %d4,%d1
    jmp     LVO_Close(%a6)

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
f_t:      .asciz "T:zw.tmp"
f_t2:     .asciz "t:ZW.TMP"
f_env:    .asciz "ENV:Zw/pref"
f_env2:   .asciz "env:zw//Zw/pref"
f_new:    .asciz "PROGDIR:zw.new"
f_new2:   .asciz "zw.new"
f_new3:   .asciz "SYS:zw.new"
f_dh0:    .asciz "DH0:zw/deep"
f_dh02:   .asciz "dh0:ZW/deep/"
f_nil:    .asciz "NIL:"
f_zexit:  .asciz "zexit"
f_zexit2: .asciz "PROGDIR:zexit"
f_zhello: .asciz "zhello"
f_zbad:   .asciz "zbadcall"
t_xy:     .ascii "XY"
text:     .ascii "zwrite kept this in memory\n"
textend:
m_paths:  .asciz "zwrite: T:, ENV:, the program's folder and DH0: written and read back from memory\n"
m_shadow: .asciz "zwrite: its own zexit shadowed by what it wrote\n"
m_rw:     .asciz "zwrite: zhello changed through MODE_READWRITE, a copy in memory\n"
m_del:    .asciz "zwrite: the deleted files are gone for the session\n"
m_attr:   .asciz "zwrite: SetProtection, SetComment and SetFileDate answer for files that are there\n"
m_fgets:  .asciz "zwrite: FGetC, UnGetC and FGets read zw.new a line at a time\n"
m_fail:   .asciz "zwrite: failed (the return code names the step)\n"
    .even
vars:   .space  V_end
    .space  64
