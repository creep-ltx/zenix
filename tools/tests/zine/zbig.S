/* SPDX-License-Identifier: GPL-2.0-only */
/* zbig: a program whose file and hunks do not fit in memory together (docs/d29-zine.md 16.12; tools/mkbigtest.py
 * writes it: a 1.5 MB chip data hunk, a 1.25 MB fast data hunk, a 64 KB BSS hunk), so zine loads it from its file before
 * the hand-over. Checks both data hunks' first, middle and last longwords, that they are in chip and fast RAM, the
 * BSS clear; in its folder the small zbig.small is found; then (16.23) zbig.huge (400 KB, longword i 0x4E000000 | i)
 * stays on the disk and is read from there (its first longword, an aligned 1 KB, 8 bytes across a sector's end, the
 * last 8 bytes, then the end), zbig.mid1-20 (60 KB each, more than the volume's budget together) on the disk or in
 * the volume, read right, and the archive zbig-pack.lha left out (archives go first); with the argument m (zine -m: every
 * file in memory), too big for ZINE's volume as before 16.23, zbig.huge and zbig-pack.lha are left out and not found.
 * Exit 0 when everything held, else the step. PC-relative code. */
#include "zt.i"
#define LVO_TypeOfMem -534
#define LVO_Open -30
#define LVO_Close -36
#define LVO_Read -42
#define LVO_Seek -66
#define MODE_OLDFILE 1005
    .text
start:
    bra.w   main
p_chip: .long 0                 /* relocated to hunk 1 */
p_fast: .long 0                 /* relocated to hunk 2 */
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    moveq   #0,%d5              /* d5: 1 with the argument m */
    cmp.b   #'m',(%a0)
    bne.s   0f
    moveq   #1,%d5
0:  move.l  4.w,%a6
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
    tst.l   %d5
    beq.s   20f
    moveq   #6,%d7              /* zine -m: zbig.huge and zbig-pack.lha left out */
    lea     f_huge(%pc),%a0
    bsr     open
    bne     9f
    lea     f_lha(%pc),%a0
    bsr     open
    bne     9f
    lea     m_vol(%pc),%a0
    bsr     puts
    moveq   #0,%d7
    bra     9f
20: moveq   #7,%d7              /* 16.23: zbig.huge from the disk */
    lea     f_huge(%pc),%a0
    bsr     open
    beq     9f
    move.l  %d0,%d4
    lea     buf(%pc),%a2
    moveq   #4,%d3              /* its first longword */
    bsr     rd
    cmp.l   #0x4E000000,(%a2)
    bne     8f
    moveq   #8,%d7              /* 1 KB from byte 4096: sectors straight into the buffer */
    move.l  #4096,%d2
    moveq   #-1,%d3
    bsr     seek
    lea     big(%pc),%a2
    move.l  #1024,%d3
    bsr     rd
    cmp.l   #0x4E000400,(%a2)
    bne     8f
    cmp.l   #0x4E0004FF,1020(%a2)
    bne     8f
    moveq   #9,%d7              /* 8 bytes from byte 300002 (in sector 585, which ends at 300032: no sector's end inside
                                 * them; the read through the 8-sector cache) */
    move.l  #300002,%d2
    moveq   #-1,%d3
    bsr     seek
    lea     buf(%pc),%a2
    moveq   #8,%d3
    bsr     rd
    cmp.l   #0x24F84E01,(%a2)
    bne     8f
    cmp.l   #0x24F94E01,4(%a2)
    bne     8f
    moveq   #10,%d7             /* 8 bytes across the end of sector 1 (bytes 1020-1027): two sectors in the cache */
    move.l  #1020,%d2
    moveq   #-1,%d3
    bsr     seek
    moveq   #8,%d3
    bsr     rd
    cmp.l   #0x4E0000FF,(%a2)
    bne     8f
    cmp.l   #0x4E000100,4(%a2)
    bne     8f
    moveq   #11,%d7             /* the last 8 bytes, then the end */
    moveq   #-8,%d2
    moveq   #1,%d3              /* OFFSET_END */
    bsr     seek
    moveq   #8,%d3
    bsr     rd
    cmp.l   #0x4E018FFE,(%a2)
    bne     8f
    cmp.l   #0x4E018FFF,4(%a2)
    bne     8f
    moveq   #4,%d3
    bsr     rd0
    tst.l   %d0
    bne     8f
    moveq   #13,%d7             /* 100 KB in one Read from byte 10240: past the file's 12th block (where its indirect
                                 * block breaks the run on the disk), straight into memory of its own */
    move.l  %a6,%a5
    move.l  4.w,%a6
    move.l  #102400,%d0
    moveq   #0,%d1
    jsr     LVO_AllocMem(%a6)
    move.l  %a5,%a6
    tst.l   %d0
    beq     8f
    move.l  %d0,%a2
    move.l  #10240,%d2
    moveq   #-1,%d3
    bsr     seek
    move.l  #102400,%d3
    bsr     rd
    moveq   #0,%d6              /* d6: 0 when the three longwords are right */
    cmp.l   #0x4E000A00,(%a2)
    sne     %d6
    cmp.l   #0x4E000A00+12800,51200(%a2)
    sne     %d0
    or.b    %d0,%d6
    cmp.l   #0x4E000A00+25599,102396(%a2)
    sne     %d0
    or.b    %d0,%d6
    move.l  %a2,%a1
    move.l  #102400,%d0
    move.l  4.w,%a6
    jsr     LVO_FreeMem(%a6)
    move.l  %a5,%a6
    lea     buf(%pc),%a2
    tst.b   %d6
    bne     8f
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    moveq   #14,%d7             /* zbig.mid1 and zbig.mid6, 60 KB each: in the volume or (the budget's largest) on
                                 * the disk, their last longwords right either way */
    lea     f_mid1(%pc),%a0
    move.l  #0x4D100000+15359,%d6
    bsr     midchk
    bne     9f
    lea     f_mid6(%pc),%a0
    move.l  #0x4D600000+15359,%d6
    bsr     midchk
    bne     9f
    moveq   #12,%d7             /* the archive: left out (the folder is over the volume's budget: archives first) */
    lea     f_lha(%pc),%a0
    bsr     open
    bne     9f
    lea     m_disk(%pc),%a0
    bsr     puts
    moveq   #0,%d7
    bra.s   9f
8:  move.l  %d4,%d1
    jsr     LVO_Close(%a6)
9:  move.l  %d7,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
/* rd: d3 bytes of d4 into a2, all of them or d0 -1 (then the caller's compare fails: a2 is checked); rd0: d0 the
 * bytes read. seek: d4 to d2 from d3 (-1 the start, 1 the end). a6 dos */
rd: bsr.s   rd0
    cmp.l   %d3,%d0
    beq.s   1f
    clr.l   (%a2)
1:  rts
rd0:
    move.l  %d4,%d1
    move.l  %a2,%d2
    jsr     LVO_Read(%a6)
    rts
seek:
    move.l  %d4,%d1
    jsr     LVO_Seek(%a6)
    rts
/* midchk(a0 name, d6 its last longword): Z set when it opened and its last longword is d6; a6 dos */
midchk:
    bsr.s   open
    beq.s   1f
    move.l  %d0,%d4
    moveq   #-4,%d2
    moveq   #1,%d3
    bsr.s   seek
    lea     buf(%pc),%a2
    moveq   #4,%d3
    bsr.s   rd
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    cmp.l   (%a2),%d6
    rts
1:  moveq   #1,%d0              /* Z clear: not found */
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
f_mid1:  .asciz "zbig.mid1"
f_mid6:  .asciz "zbig.mid6"
m_load:  .asciz "zbig: loaded from its file before the hand-over: its chip, fast and BSS hunks right\n"
m_vol:   .asciz "zbig: zbig.small found; zbig.huge and zbig-pack.lha left out of the volume\n"
m_disk:  .asciz "zbig: zbig.small found; zbig.huge read from the disk (its first longword, 1 KB, across sectors, the last bytes, the end, 100 KB across runs) right; zbig.mid1 and .mid6 right; zbig-pack.lha left out\n"
    .even
buf:     .long 0, 0
big:     .space 1024
