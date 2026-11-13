/* SPDX-License-Identifier: GPL-2.0-only */
/* zcalls: the calls the Top 100 productions asked for (docs/d29-zine.md 16.12), each called and its answer checked:
 * exec CreateIORequest/DeleteIORequest, AddPort/FindPort/RemPort, RawIOInit/RawMayGetChar/RawPutChar (a line on the
 * serial port); dos IsInteractive, FWrite, GetProgramName, ReadArgs/FreeArgs on its own arguments (the test runs
 * "zcalls one two TO=dest NUM 42 QUIET") and on a CSource buffer (a required argument missing: 116), AllocDosObject;
 * graphics OpenFont (topaz.font; another name 0), SetFont, Move, SetAPen, Text into a BitMap of its own (the 'A'
 * glyph's rows read back), TextLength, SetChipRev, GetBitMapAttr, BestModeIDA; intuition LockPubScreen (640 wide),
 * AllocScreenBuffer, ChangeScreenBuffer, FreeScreenBuffer; utility.library's tags, UDivMod32, UMult64, Stricmp,
 * Amiga2Date/CheckDate; the six math libraries' arithmetic and a function of each transcendental one. A line per
 * group; exit 0 when everything held, else the step's number (and a line saying it). PC-relative code. */
#include "zt.i"
#define LVO_FindPort -390
#define LVO_AddPort -354
#define LVO_RemPort -360
#define LVO_CreateIORequest -654
#define LVO_DeleteIORequest -660
#define LVO_RawIOInit -504
#define LVO_RawMayGetChar -510
#define LVO_RawPutChar -516
#define LVO_CreateMsgPort -666
#define LVO_DeleteMsgPort -672
#define LVO_IsInteractive -216
#define LVO_FWrite -330
#define LVO_GetProgramName -576
#define LVO_ReadArgs -798
#define LVO_FreeArgs -858
#define LVO_AllocDosObject -228
#define LVO_FreeDosObject -234
#define LVO_IoErr -132
#define LVO_Open -30
#define LVO_Close -36
#define LVO_OpenFont -72
#define LVO_CloseFont -78
#define LVO_SetFont -66
#define LVO_Text -60
#define LVO_TextLength -54
#define LVO_Move -240
#define LVO_SetAPen -342
#define LVO_InitRastPort -198
#define LVO_AllocBitMap -918
#define LVO_FreeBitMap -924
#define LVO_SetChipRev -888
#define LVO_GetBitMapAttr -960
#define LVO_BestModeIDA -1050
#define LVO_LockPubScreen -510
#define LVO_UnlockPubScreen -516
#define LVO_AllocScreenBuffer -768
#define LVO_FreeScreenBuffer -774
#define LVO_ChangeScreenBuffer -780
#define LVO_GetTagData -36
#define LVO_UDivMod32 -156
#define LVO_UMult64 -204
#define LVO_Stricmp -162
#define LVO_Amiga2Date -120
#define LVO_CheckDate -132
#define LVO_DPFlt -36
#define LVO_DPFix -30
#define LVO_DPMul -78
#define LVO_DPSqrt -96
#define LVO_DPSin -36
#define LVO_DPTst -48
#define LVO_SPFlt -36
#define LVO_SPFix -30
#define LVO_SPCmp -42
#define LVO_SPDiv -84
#define LVO_SPSqrt -96
#define LVO_SPAdd -66
#define LVO_SPExp -78
#define MODE_NEWFILE 1006
/* the variables (at vars) */
#define V_dos   0
#define V_gfx   4
#define V_int   8
#define V_out   12
#define V_bm    16
#define V_tmp   20
#define V_arr   24              /* ReadArgs' array, 6 longwords */
#define V_rp    48              /* a RastPort, 100 bytes */
#define V_buf   148             /* 64 bytes */
#define V_cd    212             /* a ClockData, 14 bytes */
#define V_port  228             /* a MsgPort, 34 bytes */
#define V_sp    264             /* the stack at the start (fail goes back to it) */
#define V_end   268

.macro CALL lib, lvo
    move.l  \lib(%a4),%a6
    jsr     \lvo(%a6)
.endm
.macro STEP n
    moveq   #\n,%d6
.endm
.macro SAY label
    lea     \label(%pc),%a0
    bsr     puts
.endm
.macro OPENLIB name, slot
    move.l  4.w,%a6
    lea     \name(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,\slot(%a4)
    beq     fail
.endm

    .text
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    move.l  %sp,V_sp(%a4)
    STEP    1
    OPENLIB dosname, V_dos
    OPENLIB gfxname, V_gfx
    OPENLIB intname, V_int
    CALL    V_dos, LVO_Output
    move.l  %d0,V_out(%a4)
    /* ---- exec ---- */
    STEP    2                   /* CreateIORequest: its reply port and length; DeleteIORequest */
    move.l  4.w,%a6
    jsr     LVO_CreateMsgPort(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%a0
    moveq   #48,%d0
    jsr     LVO_CreateIORequest(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%a1
    cmp.l   14(%a1),%d5
    bne     fail
    cmp.w   #48,18(%a1)
    bne     fail
    move.l  %a1,%a0
    jsr     LVO_DeleteIORequest(%a6)
    move.l  %d5,%a0
    jsr     LVO_DeleteMsgPort(%a6)
    STEP    3                   /* AddPort, FindPort finds it, RemPort, then it is gone */
    lea     V_port(%a4),%a1
    lea     portname(%pc),%a0
    move.l  %a0,10(%a1)
    move.l  4.w,%a6
    jsr     LVO_AddPort(%a6)
    lea     portname(%pc),%a1
    jsr     LVO_FindPort(%a6)
    lea     V_port(%a4),%a1
    cmp.l   %a1,%d0
    bne     fail
    jsr     LVO_RemPort(%a6)
    lea     portname(%pc),%a1
    jsr     LVO_FindPort(%a6)
    tst.l   %d0
    bne     fail
    STEP    4                   /* the debug serial port: a line out, a byte maybe in */
    jsr     LVO_RawIOInit(%a6)
    jsr     LVO_RawMayGetChar(%a6)
    lea     rawline(%pc),%a2
1:  moveq   #0,%d0
    move.b  (%a2)+,%d0
    beq.s   2f
    jsr     LVO_RawPutChar(%a6)
    bra.s   1b
2:  SAY     m_exec
    /* ---- dos ---- */
    STEP    5                   /* IsInteractive: the console's Output() yes, NIL: no */
    move.l  V_out(%a4),%d1
    CALL    V_dos, LVO_IsInteractive
    tst.l   %d0
    beq     fail
    lea     nil(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_NEWFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%d1
    jsr     LVO_IsInteractive(%a6)
    tst.l   %d0
    bne     fail
    move.l  %d5,%d1
    jsr     LVO_Close(%a6)
    STEP    6                   /* FWrite: two blocks of 15 bytes, the answer 2 */
    move.l  V_out(%a4),%d1
    lea     fw(%pc),%a0
    move.l  %a0,%d2
    moveq   #15,%d3
    moveq   #2,%d4
    jsr     LVO_FWrite(%a6)
    cmp.l   #2,%d0
    bne     fail
    STEP    7                   /* GetProgramName: "zcalls" */
    lea     V_buf(%a4),%a0
    move.l  %a0,%d1
    moveq   #64,%d2
    jsr     LVO_GetProgramName(%a6)
    tst.l   %d0
    beq     fail
    lea     V_buf(%a4),%a0
    lea     myname(%pc),%a1
    bsr     streq
    bne     fail
    STEP    8                   /* ReadArgs on "one two TO=dest NUM 42 QUIET" */
    moveq   #5,%d0
    lea     V_arr(%a4),%a0
1:  clr.l   (%a0)+
    dbra    %d0,1b
    lea     tmpl(%pc),%a0
    move.l  %a0,%d1
    lea     V_arr(%a4),%a0
    move.l  %a0,%d2
    moveq   #0,%d3
    jsr     LVO_ReadArgs(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  V_arr(%a4),%a0      /* FROM */
    lea     s_one(%pc),%a1
    bsr     streq
    bne     fail
    move.l  V_arr+4(%a4),%a0    /* TO */
    lea     s_dest(%pc),%a1
    bsr     streq
    bne     fail
    move.l  V_arr+8(%a4),%a0    /* NUM: a pointer to 42 */
    cmp.l   #42,(%a0)
    bne     fail
    tst.l   V_arr+12(%a4)       /* QUIET */
    beq     fail
    move.l  V_arr+16(%a4),%a2   /* REST/M: { "two", 0 } */
    move.l  (%a2),%a0
    lea     s_two(%pc),%a1
    bsr     streq
    bne     fail
    tst.l   4(%a2)
    bne     fail
    move.l  %d5,%d1
    jsr     LVO_FreeArgs(%a6)
    STEP    9                   /* a CSource of one word against X/A,Y/A: 0, IoErr 116 */
    moveq   #5,%d1              /* DOS_RDARGS */
    moveq   #0,%d2
    jsr     LVO_AllocDosObject(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%a0
    lea     csrc(%pc),%a1
    move.l  %a1,(%a0)
    move.l  #5,4(%a0)
    lea     tmpl2(%pc),%a0
    move.l  %a0,%d1
    lea     V_arr(%a4),%a0
    move.l  %a0,%d2
    move.l  %d5,%d3
    jsr     LVO_ReadArgs(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #116,%d0
    bne     fail
    moveq   #5,%d1
    move.l  %d5,%d2
    jsr     LVO_FreeDosObject(%a6)
    SAY     m_dos
    /* ---- graphics ---- */
    STEP    10                  /* OpenFont: topaz.font 8 found, another name not */
    lea     ta_topaz(%pc),%a0
    lea     topazname(%pc),%a1
    move.l  %a1,(%a0)
    CALL    V_gfx, LVO_OpenFont
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%a0
    cmp.w   #8,20(%a0)          /* tf_YSize */
    bne     fail
    lea     ta_none(%pc),%a0
    lea     nonename(%pc),%a1
    move.l  %a1,(%a0)
    jsr     LVO_OpenFont(%a6)
    tst.l   %d0
    bne     fail
    STEP    11                  /* "A" drawn at (0,6) into a 64x16x1 BitMap: its rows are the glyph's */
    moveq   #64,%d0
    moveq   #16,%d1
    moveq   #1,%d2
    moveq   #1,%d3              /* BMF_CLEAR */
    sub.l   %a0,%a0
    jsr     LVO_AllocBitMap(%a6)
    move.l  %d0,V_bm(%a4)
    beq     fail
    lea     V_rp(%a4),%a1
    jsr     LVO_InitRastPort(%a6)
    lea     V_rp(%a4),%a1
    move.l  V_bm(%a4),4(%a1)
    move.l  %d5,%a0
    jsr     LVO_SetFont(%a6)
    lea     V_rp(%a4),%a1
    moveq   #1,%d0
    jsr     LVO_SetAPen(%a6)
    lea     V_rp(%a4),%a1
    moveq   #0,%d0
    moveq   #6,%d1
    jsr     LVO_Move(%a6)
    lea     V_rp(%a4),%a1
    lea     s_A(%pc),%a0
    moveq   #1,%d0
    jsr     LVO_Text(%a6)
    move.l  V_bm(%a4),%a0
    move.l  8(%a0),%a0          /* plane 0; 8 bytes a row */
    lea     glyphA(%pc),%a1
    moveq   #6,%d0
1:  cmpm.b  (%a1)+,(%a0)+
    bne     fail
    addq.l  #7,%a0
    dbra    %d0,1b
    lea     V_rp(%a4),%a1
    lea     s_A(%pc),%a0
    moveq   #2,%d0
    jsr     LVO_TextLength(%a6)
    cmp.l   #16,%d0
    bne     fail
    move.l  %d5,%a1
    jsr     LVO_CloseFont(%a6)
    STEP    12                  /* GetBitMapAttr: width 64, depth 1, height 16 */
    move.l  V_bm(%a4),%a0
    moveq   #8,%d1
    jsr     LVO_GetBitMapAttr(%a6)
    cmp.l   #64,%d0
    bne     fail
    move.l  V_bm(%a4),%a0
    moveq   #4,%d1
    jsr     LVO_GetBitMapAttr(%a6)
    cmp.l   #1,%d0
    bne     fail
    move.l  V_bm(%a4),%a0
    moveq   #0,%d1
    jsr     LVO_GetBitMapAttr(%a6)
    cmp.l   #16,%d0
    bne     fail
    move.l  V_bm(%a4),%a0
    jsr     LVO_FreeBitMap(%a6)
    STEP    13                  /* SetChipRev answers ($1F on the AGA test machine: ALICE, LISA and MLISA as a
                                 * real A1200's, d29 16.20); BestModeIDA of 640 wide, 2 planes: a HIRES id */
    moveq   #-1,%d0
    jsr     LVO_SetChipRev(%a6)
    cmp.l   #0x1F,%d0
    bne     fail
    lea     bidtags(%pc),%a0
    jsr     LVO_BestModeIDA(%a6)
    cmp.l   #-1,%d0
    beq     fail
    btst    #15,%d0
    beq     fail
    SAY     m_gfx
    /* ---- intuition ---- */
    STEP    14                  /* LockPubScreen(NULL): 640 wide; two screen buffers changed between */
    sub.l   %a0,%a0
    CALL    V_int, LVO_LockPubScreen
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%a0
    cmp.w   #640,12(%a0)        /* sc_Width */
    bne     fail
    move.l  %d5,%a0
    sub.l   %a1,%a1
    moveq   #1,%d0              /* SB_SCREEN_BITMAP */
    jsr     LVO_AllocScreenBuffer(%a6)
    move.l  %d0,%d3
    beq     fail
    move.l  %d5,%a0
    sub.l   %a1,%a1
    moveq   #2,%d0              /* SB_COPY_BITMAP */
    jsr     LVO_AllocScreenBuffer(%a6)
    move.l  %d0,%d4
    beq     fail
    move.l  %d4,%a1
    tst.l   (%a1)
    beq     fail
    move.l  %d5,%a0
    jsr     LVO_ChangeScreenBuffer(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d5,%a0
    move.l  %d3,%a1
    jsr     LVO_ChangeScreenBuffer(%a6)
    move.l  %d5,%a0
    move.l  %d4,%a1
    jsr     LVO_FreeScreenBuffer(%a6)
    move.l  %d5,%a0
    move.l  %d3,%a1
    jsr     LVO_FreeScreenBuffer(%a6)
    sub.l   %a0,%a0
    move.l  %d5,%a1
    jsr     LVO_UnlockPubScreen(%a6)
    SAY     m_int
    /* ---- utility.library ---- */
    STEP    15
    OPENLIB utilname, V_tmp
    STEP    16                  /* GetTagData through TAG_MORE */
    move.l  #0x80000002,%d0
    moveq   #0,%d1
    lea     tags1(%pc),%a0
    lea     tags2(%pc),%a1
    move.l  %a1,12(%a0)         /* the TAG_MORE's data: tags2 (PC-relative code has no absolute pointers) */
    CALL    V_tmp, LVO_GetTagData
    cmp.l   #7,%d0
    bne     fail
    STEP    17                  /* UDivMod32(100, 7): 14 rest 2; UMult64(0x10000, 0x10000): 1 high, 0 low */
    moveq   #100,%d0
    moveq   #7,%d1
    jsr     LVO_UDivMod32(%a6)
    cmp.l   #14,%d0
    bne     fail
    cmp.l   #2,%d1
    bne     fail
    move.l  #0x10000,%d0
    move.l  %d0,%d1
    jsr     LVO_UMult64(%a6)
    cmp.l   #1,%d0
    bne     fail
    tst.l   %d1
    bne     fail
    STEP    18                  /* Stricmp("Zine", "zINE") 0 */
    lea     s_zine1(%pc),%a0
    lea     s_zine2(%pc),%a1
    jsr     LVO_Stricmp(%a6)
    tst.l   %d0
    bne     fail
    STEP    19                  /* Amiga2Date(365 days + 1 h): 1979-01-01 01:00, a Monday; CheckDate gives it back */
    move.l  #31539600,%d0
    lea     V_cd(%a4),%a0
    jsr     LVO_Amiga2Date(%a6)
    lea     V_cd(%a4),%a0
    cmp.w   #1,4(%a0)           /* hour */
    bne     fail
    cmp.w   #1,6(%a0)           /* mday */
    bne     fail
    cmp.w   #1,8(%a0)           /* month */
    bne     fail
    cmp.w   #1979,10(%a0)
    bne     fail
    cmp.w   #1,12(%a0)          /* wday */
    bne     fail
    jsr     LVO_CheckDate(%a6)
    cmp.l   #31539600,%d0
    bne     fail
    SAY     m_util
    /* ---- the math libraries ---- */
    STEP    20                  /* IEEE double: 3 * 3 = 9 */
    OPENLIB dbasname, V_tmp
    moveq   #3,%d0
    CALL    V_tmp, LVO_DPFlt
    move.l  %d0,%d2
    move.l  %d1,%d3
    jsr     LVO_DPMul(%a6)
    move.l  %d0,%d4             /* 9.0 kept in d4/d5 */
    move.l  %d1,%d5
    jsr     LVO_DPFix(%a6)
    cmp.l   #9,%d0
    bne     fail
    STEP    21                  /* sqrt(9.0) = 3, sin(0) tests 0 */
    OPENLIB dtrname, V_tmp
    move.l  %d4,%d0
    move.l  %d5,%d1
    CALL    V_tmp, LVO_DPSqrt
    move.l  %d0,%d4
    move.l  %d1,%d5
    moveq   #0,%d0
    moveq   #0,%d1
    jsr     LVO_DPSin(%a6)
    move.l  %d0,%d2
    move.l  %d1,%d3
    OPENLIB dbasname, V_tmp
    move.l  %d4,%d0
    move.l  %d5,%d1
    CALL    V_tmp, LVO_DPFix
    cmp.l   #3,%d0
    bne     fail
    move.l  %d2,%d0
    move.l  %d3,%d1
    jsr     LVO_DPTst(%a6)
    bne     fail
    STEP    22                  /* FFP: 10 / 2 = 5 (SPDiv d0 / d1); SPCmp(d1 2, d0 10) is GT */
    OPENLIB ffpname, V_tmp
    moveq   #2,%d0
    CALL    V_tmp, LVO_SPFlt
    move.l  %d0,%d4
    moveq   #10,%d0
    jsr     LVO_SPFlt(%a6)
    move.l  %d0,%d5
    move.l  %d4,%d1
    jsr     LVO_SPCmp(%a6)
    ble     fail
    move.l  %d5,%d0
    move.l  %d4,%d1
    jsr     LVO_SPDiv(%a6)
    jsr     LVO_SPFix(%a6)
    cmp.l   #5,%d0
    bne     fail
    STEP    23                  /* mathtrans: sqrt(16) = 4 */
    moveq   #16,%d0
    jsr     LVO_SPFlt(%a6)
    move.l  %d0,%d5
    OPENLIB trname, V_tmp
    move.l  %d5,%d0
    CALL    V_tmp, LVO_SPSqrt
    move.l  %d0,%d5
    OPENLIB ffpname, V_tmp
    move.l  %d5,%d0
    CALL    V_tmp, LVO_SPFix
    cmp.l   #4,%d0
    bne     fail
    STEP    24                  /* IEEE single: 7 + 7 = 14; exp(0) = 1 */
    OPENLIB sbasname, V_tmp
    moveq   #7,%d0
    CALL    V_tmp, LVO_SPFlt
    move.l  %d0,%d1
    jsr     LVO_SPAdd(%a6)
    jsr     LVO_SPFix(%a6)
    cmp.l   #14,%d0
    bne     fail
    OPENLIB strname, V_tmp
    moveq   #0,%d0
    CALL    V_tmp, LVO_SPExp
    move.l  %d0,%d5
    OPENLIB sbasname, V_tmp
    move.l  %d5,%d0
    CALL    V_tmp, LVO_SPFix
    cmp.l   #1,%d0
    bne     fail
    SAY     m_math
    moveq   #0,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
fail:
    move.l  V_sp(%a4),%sp
    lea     m_fail(%pc),%a0
    bsr     puts
    move.l  %d6,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* puts(a0): the 0-ended line to Output() (d0-d3/a0-a1/a6 used) */
puts:
    movem.l %d2-%d3/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    move.l  V_out(%a4),%d1
    move.l  V_dos(%a4),%a6
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d2-%d3/%a6
    rts
/* streq(a0, a1): Z set when the two 0-ended strings are the same */
streq:
1:  move.b  (%a0)+,%d0
    cmp.b   (%a1)+,%d0
    bne.s   2f
    tst.b   %d0
    bne.s   1b
2:  rts

dosname:  .asciz "dos.library"
gfxname:  .asciz "graphics.library"
intname:  .asciz "intuition.library"
utilname: .asciz "utility.library"
dbasname: .asciz "mathieeedoubbas.library"
dtrname:  .asciz "mathieeedoubtrans.library"
ffpname:  .asciz "mathffp.library"
trname:   .asciz "mathtrans.library"
sbasname: .asciz "mathieeesingbas.library"
strname:  .asciz "mathieeesingtrans.library"
portname: .asciz "zcalls.port"
rawline:  .asciz "zcalls-raw: RawPutChar on the serial port\r\n"
nil:      .asciz "NIL:"
fw:       .ascii "zcalls: FWrite "
          .ascii "in two blocks.\n"
          .byte 0
myname:   .asciz "zcalls"
tmpl:     .asciz "FROM/A,TO/K,N=NUM/N,Q=QUIET/S,REST/M"
tmpl2:    .asciz "X/A,Y/A"
csrc:     .ascii "only\n"
s_one:    .asciz "one"
s_two:    .asciz "two"
s_dest:   .asciz "dest"
topazname: .asciz "topaz.font"
nonename: .asciz "nosuch.font"
s_A:      .asciz "AB"
s_zine1:  .asciz "Zine"
s_zine2:  .asciz "zINE"
glyphA:   .byte 0x38, 0x6c, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6
m_exec:   .asciz "zcalls: exec's IO requests, public ports and debug serial port answered\n"
m_dos:    .asciz "zcalls: IsInteractive, FWrite, GetProgramName, ReadArgs answered\n"
m_gfx:    .asciz "zcalls: topaz 8 drew an A; GetBitMapAttr, SetChipRev, BestModeIDA answered\n"
m_int:    .asciz "zcalls: the public screen and screen buffers answered\n"
m_util:   .asciz "zcalls: utility.library's tags, division, multiply, Stricmp and dates answered\n"
m_math:   .asciz "zcalls: the six math libraries computed\n"
m_fail:   .asciz "zcalls: a step failed\n"
    .even
ta_topaz: .long 0
          .word 8, 0
ta_none:  .long 0
          .word 8, 0
bidtags:  .long 0x80000004, 640, 0x80000008, 2, 0
tags1:    .long 0x80000001, 5, 2, 0
tags2:    .long 0x80000002, 7, 0
vars:     .space V_end
