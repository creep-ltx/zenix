/* SPDX-License-Identifier: GPL-2.0-only */
/* zt.library: a small disk library of ZINE's own (docs/d29-zine.md 16.8), embedded in zlibs, which writes it to RAM:
 * and opens it as a demo opens the library it carries. Written from the RKRM's description of a disk library: the
 * first code says "moveq #-1,d0; rts", then a RomTag with RTF_AUTOINIT whose rt_Init points to four longwords for
 * MakeLibrary (the base's size, the vectors as word displacements, an InitStruct table, the init code).
 *   -30 Double(d0): d0 * 2 + 1      -36 Sum(): the sum of the 16 bytes InitStruct wrote at base + 34
 *   -42 Seg(): the segment list init was given
 * Its Expunge removes it and frees its base, answering the segment list, or sets LIBF_DELEXP while it is open.
 * tools/mkhunktest.py -r writes it with its absolute addresses as HUNK_RELOC32. */
#define LVO_Remove  -252
#define LVO_FreeMem -210
#define LIB_FLAGS   14
#define LIB_NEGSIZE 16
#define LIB_POSSIZE 18
#define LIB_OPENCNT 32
#define LIBF_DELEXP 3
#define BASESIZE    56
#define B_SEG       52
#define B_EXEC      50              /* a word: 1 when init got a6 = ExecBase */

    .text
start:
    moveq   #-1,%d0                 /* run as a program, it does nothing */
    rts
rt: .word   0x4AFC                  /* RTC_MATCHWORD */
    .long   rt                      /* rt_MatchTag */
    .long   end                     /* rt_EndSkip */
    .byte   0x80, 3, 9, 0           /* RTF_AUTOINIT, version 3, NT_LIBRARY, priority 0 */
    .long   name
    .long   idstr
    .long   autoinit
autoinit:
    .long   BASESIZE
    .long   funcs
    .long   data
    .long   init
funcs:
    .word   -1                      /* word displacements from funcs */
    .word   l_open - funcs, l_close - funcs, l_expunge - funcs, l_null - funcs
    .word   l_double - funcs, l_sum - funcs, l_seg - funcs
    .word   -1
data:                               /* InitStruct, in the forms exec/initializers.i makes and three more */
    .byte   0xE0, 0                 /* INITBYTE LN_TYPE, NT_LIBRARY */
    .word   8
    .byte   9, 0
    .byte   0xC0, 0                 /* INITLONG LN_NAME */
    .word   10
    .long   name
    .byte   0xE0, 0                 /* INITBYTE LIB_FLAGS, LIBF_SUMUSED | LIBF_CHANGED */
    .word   LIB_FLAGS
    .byte   6, 0
    .byte   0xD0, 0                 /* INITWORD LIB_VERSION, 3 */
    .word   20
    .word   3
    .byte   0xD0, 0                 /* INITWORD LIB_REVISION, 7 */
    .word   22
    .word   7
    .byte   0xC0, 0                 /* INITLONG LIB_IDSTRING */
    .word   24
    .long   idstr
    .byte   0xA1, 34                /* bytes, a byte offset: 2 bytes at 34 */
    .byte   0x11, 0x22
    .byte   0x52, 0                 /* words repeated, the next destination (36): 3 times */
    .word   0x5A5A
    .byte   0x01, 0                 /* longs, the next destination (42): 2 of them */
    .long   0x01020304, 0x05060708
    .byte   0, 0
    .even

/* init: d0 the base, a0 the segment list, a6 ExecBase */
init:
    move.l  %d0,%a1
    move.l  %a0,B_SEG(%a1)
    cmp.l   4.w,%a6
    seq     %d1
    and.w   #1,%d1
    move.w  %d1,B_EXEC(%a1)
    rts                             /* d0: the base */

l_open:                             /* a6 the base, d0 the version */
    addq.w  #1,LIB_OPENCNT(%a6)
    bclr    #LIBF_DELEXP,LIB_FLAGS(%a6)
    move.l  %a6,%d0
    rts
l_close:
    moveq   #0,%d0
    subq.w  #1,LIB_OPENCNT(%a6)
    bne.s   1f
    btst    #LIBF_DELEXP,LIB_FLAGS(%a6)
    bne.s   l_expunge
1:  rts
l_expunge:
    tst.w   LIB_OPENCNT(%a6)
    beq.s   1f
    bset    #LIBF_DELEXP,LIB_FLAGS(%a6)
    moveq   #0,%d0
    rts
1:  movem.l %d2/%a5-%a6,-(%sp)
    move.l  B_SEG(%a6),%d2
    move.l  %a6,%a5
    move.l  4.w,%a6
    move.l  %a5,%a1
    jsr     LVO_Remove(%a6)
    move.l  %a5,%a1
    moveq   #0,%d0
    move.w  LIB_NEGSIZE(%a5),%d0
    sub.l   %d0,%a1
    add.w   LIB_POSSIZE(%a5),%d0
    jsr     LVO_FreeMem(%a6)
    move.l  %d2,%d0
    movem.l (%sp)+,%d2/%a5-%a6
    rts
l_null:
    moveq   #0,%d0
    rts
l_double:
    add.l   %d0,%d0
    addq.l  #1,%d0
    rts
l_sum:
    move.l  %d2,-(%sp)
    moveq   #0,%d0
    moveq   #0,%d1
    lea     34(%a6),%a0
    moveq   #15,%d2
1:  move.b  (%a0)+,%d1
    add.l   %d1,%d0
    dbra    %d2,1b
    move.l  (%sp)+,%d2
    rts
l_seg:
    move.l  B_SEG(%a6),%d0
    rts
name:   .asciz  "zt.library"
idstr:  .asciz  "zt 3.7 (ZINE's test library)\r\n"
    .even
end:
