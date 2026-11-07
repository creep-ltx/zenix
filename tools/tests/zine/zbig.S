/* SPDX-License-Identifier: GPL-2.0-only */
/* zbig: a program whose file and hunks do not fit in memory together (docs/d29-zine.md 16.12; tools/mkbigtest.py
 * writes it: a 1.5 MB chip data hunk, a 1.25 MB fast data hunk, a 64 KB BSS hunk), so zine loads it from its file before
 * the hand-over. Checks both data hunks' first, middle and last longwords, that they are in chip and fast RAM, the
 * BSS clear; in its folder, too big for ZINE's volume, the small zbig.small is found and the large zbig.huge and
 * the archive zbig-pack.lha are not (left out). Exit 0 when everything held, else the step. PC-relative code. */
#include "zt.i"
#define LVO_TypeOfMem -534
#define LVO_Open -30
#define LVO_Close -36
#define LVO_Read -42
#define MODE_OLDFILE 1005
    .text
start:
    bra.w   main
p_chip: .long 0                 /* relocated to hunk 1 */
p_fast: .long 0                 /* relocated to hunk 2 */
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    moveq   #1,%d7
    move.l  p_chip(%pc),%a0
    cmp.l   #0xC0000000,(%a0)
    bne     9f
    cmp.l   #0xC0000000+192000,4*192000(%a0)
    bne     9f
    cmp.l   #0xC0000000+383999,4*383999(%a0)
    bne     9f
    moveq   #2,%d7
    move.l  p_fast(%pc),%a0
    cmp.l   #0xFA000000,(%a0)
    bne     9f
    cmp.l   #0xFA000000+160000,4*160000(%a0)
    bne     9f
    cmp.l   #0xFA000000+319999,4*319999(%a0)
    bne     9f
    moveq   #3,%d7
    move.l  p_chip(%pc),%a1
    jsr     LVO_TypeOfMem(%a6)
    btst    #1,%d0              /* MEMF_CHIP */
    beq     9f
    move.l  p_fast(%pc),%a1
    jsr     LVO_TypeOfMem(%a6)
    btst    #2,%d0              /* MEMF_FAST */
    beq     9f
    moveq   #4,%d7              /* the BSS hunk: the third segment after the code's, cleared */
    lea     start-4(%pc),%a0    /* the code segment's link longword */
    moveq   #2,%d1
1:  move.l  (%a0),%d0
    lsl.l   #2,%d0
    move.l  %d0,%a0
    dbra    %d1,1b              /* a0: the BSS segment's link longword, its data after it */
    tst.l   4(%a0)
    bne     9f
    tst.l   4+65532(%a0)
    bne     9f
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,%a6
    lea     m_load(%pc),%a0
    bsr     puts
    moveq   #5,%d7              /* zbig.small found and read */
    lea     f_small(%pc),%a0
    bsr     open
    beq     9f
    move.l  %d0,%d4
    move.l  %d4,%d1
    lea     buf(%pc),%a0
    move.l  %a0,%d2
    moveq   #5,%d3
    jsr     LVO_Read(%a6)
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    move.l  buf(%pc),%d0
    cmp.l   #0x534D414C,%d0     /* "SMAL" */
    bne     9f
    moveq   #6,%d7              /* zbig.huge and zbig-pack.lha left out */
    lea     f_huge(%pc),%a0
    bsr     open
    bne     9f
    lea     f_lha(%pc),%a0
    bsr     open
    bne     9f
    lea     m_vol(%pc),%a0
    bsr     puts
    moveq   #0,%d7
9:  move.l  %d7,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
/* open(a0 name) -> d0 the handle (Z clear) or 0 (Z set); a6 dos */
open:
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    tst.l   %d0
    rts
/* puts(a0): the line to Output() */
puts:
    movem.l %d2-%d3,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d2-%d3
    rts
dosname: .asciz "dos.library"
f_small: .asciz "zbig.small"
f_huge:  .asciz "zbig.huge"
f_lha:   .asciz "zbig-pack.lha"
m_load:  .asciz "zbig: loaded from its file before the hand-over: its chip, fast and BSS hunks right\n"
m_vol:   .asciz "zbig: zbig.small found; zbig.huge and zbig-pack.lha left out of the volume\n"
    .even
buf:     .long 0, 0
