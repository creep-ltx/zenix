/* SPDX-License-Identifier: GPL-2.0-only */
/* znext: the calls the Top 100 productions asked for in ZINE's third round (docs/d29-zine.md 16.13), each called and
 * its answer checked: exec Allocate/Deallocate on a MemHeader of its own, FindSemaphore; dos Lock, Examine, ExNext,
 * NameFromLock, ParentDir, CurrentDir (a name relative to it opened), DateStamp, a disk's name taken as its folder; graphics GetVPModeID,
 * ModeNotAvailable, VideoControl, ScrollVPort, BltClear, GetSprite/FreeSprite, Draw, PolyDraw; intuition
 * QueryOverscan, WBenchToFront, ActivateWindow, RethinkDisplay, MakeScreen, MoveScreen, AllocRemember/FreeRemember,
 * DrawBorder, EasyRequestArgs, DisplayAlert; diskfont OpenDiskFont (a font of /usr/share/fonts/amiga, one of the
 * program's own Fonts folder, a missing one, a damaged one, a size it does not have) and AvailFonts. The test runs it
 * in a folder with Data/Sub/f.txt ("hello") and Fonts/zfixed.font (tools/tests/zine/zfix17). A line per group; exit 0
 * when everything held, else the step's number. PC-relative code. */
#include "zt.i"
#define LVO_Allocate -186
#define LVO_Deallocate -192
#define LVO_FindSemaphore -594
#define LVO_Lock -84
#define LVO_CompareDates -738
#define LVO_DoPkt -240
#define LVO_SetDrMd -354
#define LVO_RectFill -306
#define LVO_CopyMem -624
#define LVO_GetDeviceProc -642
#define LVO_FreeDeviceProc -648
#define LVO_LockDosList -654
#define LVO_UnLockDosList -660
#define LVO_NextDosEntry -690
#define LVO_FindDosEntry -684
#define LVO_UnLock -90
#define LVO_Examine -102
#define LVO_ExNext -108
#define LVO_CurrentDir -126
#define LVO_IoErr -132
#define LVO_ParentDir -210
#define LVO_DateStamp -192
#define LVO_NameFromLock -402
#define LVO_Open -30
#define LVO_Close -36
#define LVO_Read -42
#define LVO_GetVPModeID -792
#define LVO_ModeNotAvailable -798
#define LVO_VideoControl -708
#define LVO_ScrollVPort -588
#define LVO_BltClear -300
#define LVO_GetSprite -408
#define LVO_FreeSprite -414
#define LVO_InitBitMap -390
#define LVO_AllocRaster -492
#define LVO_FreeRaster -498
#define LVO_InitRastPort -198
#define LVO_SetAPen -342
#define LVO_Move -240
#define LVO_Draw -246
#define LVO_PolyDraw -336
#define LVO_SetFont -66
#define LVO_TextLength -54
#define LVO_OpenFont -72
#define LVO_CloseFont -78
#define LVO_QueryOverscan -474
#define LVO_WBenchToFront -342
#define LVO_ActivateWindow -450
#define LVO_AllocRemember -396
#define LVO_FreeRemember -408
#define LVO_DrawBorder -108
#define LVO_EasyRequestArgs -588
#define LVO_DisplayAlert -90
#define LVO_RethinkDisplay -390
#define LVO_MakeScreen -378
#define LVO_MoveScreen -162
#define LVO_OpenDiskFont -30
#define LVO_AvailFonts -36
#define LVO_GetColorMap -570
#define LVO_FreeColorMap -576
#define LVO_GetRGB4 -582
#define LVO_SetRGB4CM -630
#define LVO_GetRGB32 -900
#define LVO_InitVPort -204
#define LVO_InitView -360
#define LVO_MrgCop -210
#define LVO_WaitBOVP -402
#define LVO_AllocDBufInfo -966
#define LVO_FreeDBufInfo -972
#define LVO_ChangeVPBitMap -942
#define LVO_WritePixel -324
#define LVO_ReadPixel -318
#define LVO_FilePart -870
#define LVO_PathPart -876
#define LVO_AddPart -882
#define LVO_Fault -468
#define LVO_PrintFault -474
#define LVO_SumKickData -612
#define LVO_CreateMsgPort -666
#define LVO_DeleteMsgPort -672
#define LVO_CreateIORequest -654
#define LVO_DeleteIORequest -660
#define LVO_OpenDevice -444
#define LVO_CloseDevice -450
#define LVO_DoIO -456
#define LVO_ReadEClock -60
#define LVO_Delay -198
#define MODE_OLDFILE 1005
#define SHARED_LOCK -2
/* the variables (at vars) */
#define V_dos   0
#define V_gfx   4
#define V_int   8
#define V_out   12
#define V_df    16
#define V_sc    20
#define V_l1    24
#define V_l2    28
#define V_key   32
#define V_blk   36
#define V_tmp   40
#define V_fib   44              /* a FileInfoBlock, 260 bytes */
#define V_buf   304             /* 64 bytes */
#define V_rect  368
#define V_bm    376             /* a BitMap, 40 bytes */
#define V_rp    416             /* a RastPort, 100 bytes */
#define V_ds    516             /* a DateStamp */
#define V_tags  528             /* 64 bytes */
#define V_flag  592
#define V_mh    596             /* a MemHeader, 32 bytes */
#define V_es    628             /* an EasyStruct, 20 bytes */
#define V_bord  648             /* a Border, 16 bytes */
#define V_ss    664             /* a SimpleSprite, 12 bytes */
#define V_ta    676             /* a TextAttr, 8 bytes */
#define V_tf    684
#define V_av    688             /* AvailFonts' buffer, 512 bytes */
#define V_sp    1200
#define V_view  1204            /* a View, 18 bytes */
#define V_vp    1224            /* a ViewPort, 40 bytes */
#define V_ri    1264            /* a RasInfo, 12 bytes */
#define V_cm    1276
#define V_dbi   1280
#define V_rgb   1284            /* GetRGB32's three longs */
#define V_io    1296
#define V_ev    1300            /* two EClockVals */
#define V_end   1316

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
/* a TextAttr of name (PC-relative) and size, opened with OpenDiskFont: d0 the TextFont */
.macro DFONT name, size, flags=0
    lea     \name(%pc),%a0
    move.l  %a0,V_ta(%a4)
    move.w  #\size,V_ta+4(%a4)
    move.b  #0,V_ta+6(%a4)
    move.b  #\flags,V_ta+7(%a4)
    lea     V_ta(%a4),%a0
    CALL    V_df, LVO_OpenDiskFont
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
    STEP    2                   /* Allocate, Deallocate on a MemHeader over 1 KB of its own */
    move.l  4.w,%a6
    move.l  #1024,%d0
    move.l  #0x10001,%d1
    jsr     LVO_AllocMem(%a6)
    move.l  %d0,V_blk(%a4)
    beq     fail
    move.l  %d0,%a0
    clr.l   (%a0)               /* the one chunk: mc_Next 0, mc_Bytes 1024 */
    move.l  #1024,4(%a0)
    lea     V_mh(%a4),%a1
    move.b  #10,8(%a1)          /* NT_MEMORY */
    move.l  %d0,16(%a1)         /* mh_First */
    move.l  %d0,20(%a1)         /* mh_Lower */
    add.l   #1024,%d0
    move.l  %d0,24(%a1)         /* mh_Upper */
    move.l  #1024,28(%a1)       /* mh_Free */
    lea     V_mh(%a4),%a0
    moveq   #100,%d0
    jsr     LVO_Allocate(%a6)
    cmp.l   V_blk(%a4),%d0
    bne     fail
    lea     V_mh(%a4),%a0
    moveq   #7,%d0
    jsr     LVO_Allocate(%a6)
    move.l  V_blk(%a4),%d1
    add.l   #104,%d1            /* 100 rounded up to 8 */
    cmp.l   %d1,%d0
    bne     fail
    cmp.l   #1024-112,V_mh+28(%a4)
    bne     fail
    lea     V_mh(%a4),%a0
    move.l  V_blk(%a4),%a1
    moveq   #100,%d0
    jsr     LVO_Deallocate(%a6)
    lea     V_mh(%a4),%a0
    moveq   #96,%d0
    jsr     LVO_Allocate(%a6)
    cmp.l   V_blk(%a4),%d0      /* the freed block again */
    bne     fail
    lea     V_mh(%a4),%a0
    move.l  #2000,%d0
    jsr     LVO_Allocate(%a6)
    tst.l   %d0                 /* more than it has: 0 */
    bne     fail
    move.l  V_blk(%a4),%a1
    move.l  #1024,%d0
    jsr     LVO_FreeMem(%a6)
    STEP    3                   /* FindSemaphore of a name nobody added: 0 */
    lea     semname(%pc),%a1
    jsr     LVO_FindSemaphore(%a6)
    tst.l   %d0
    bne     fail
    SAY     m_exec
    /* ---- dos ---- */
    STEP    4                   /* Lock and Examine a file two folders down */
    lea     p_file(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    CALL    V_dos, LVO_Lock
    move.l  %d0,V_l1(%a4)
    beq     fail
    move.l  %d0,%d1
    lea     V_fib(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_Examine(%a6)
    tst.l   %d0
    beq     fail
    cmp.l   #-3,V_fib+4(%a4)    /* fib_DirEntryType ST_FILE */
    bne     fail
    cmp.l   #6,V_fib+124(%a4)   /* fib_Size */
    bne     fail
    lea     V_fib+8(%a4),%a0
    lea     s_ftxt(%pc),%a1
    bsr     streq
    bne     fail
    move.l  V_l1(%a4),%d1
    jsr     LVO_UnLock(%a6)
    STEP    5                   /* a folder by another case: Examine, ExNext lists Sub, then the end (232) */
    lea     p_data(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    jsr     LVO_Lock(%a6)
    move.l  %d0,V_l1(%a4)
    beq     fail
    move.l  %d0,%d1
    lea     V_fib(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_Examine(%a6)
    cmp.l   #2,V_fib+4(%a4)     /* ST_USERDIR */
    bne     fail
    move.l  V_l1(%a4),%d1
    lea     V_fib(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_ExNext(%a6)
    tst.l   %d0
    beq     fail
    lea     V_fib+8(%a4),%a0
    lea     s_sub(%pc),%a1
    bsr     streq
    bne     fail
    move.l  V_l1(%a4),%d1
    lea     V_fib(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_ExNext(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #232,%d0            /* ERROR_NO_MORE_ENTRIES */
    bne     fail
    STEP    6                   /* NameFromLock: "ZINE:Data"; ParentDir: the volume's top (ST_ROOT) */
    move.l  V_l1(%a4),%d1
    lea     V_buf(%a4),%a0
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_NameFromLock(%a6)
    tst.l   %d0
    beq     fail
    lea     V_buf(%a4),%a0
    lea     s_zdata(%pc),%a1
    bsr     streq
    bne     fail
    move.l  V_l1(%a4),%d1
    jsr     LVO_ParentDir(%a6)
    move.l  %d0,V_l2(%a4)
    beq     fail
    move.l  %d0,%d1
    lea     V_fib(%a4),%a0
    move.l  %a0,%d2
    jsr     LVO_Examine(%a6)
    cmp.l   #1,V_fib+4(%a4)     /* ST_ROOT */
    bne     fail
    move.l  V_l2(%a4),%d1
    jsr     LVO_UnLock(%a6)
    STEP    7                   /* CurrentDir(Data): "Sub/f.txt" opened and read, "/znext" found; back */
    move.l  V_l1(%a4),%d1
    jsr     LVO_CurrentDir(%a6)
    move.l  %d0,V_tmp(%a4)
    lea     p_rel(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%d1
    lea     V_buf(%a4),%a0
    move.l  %a0,%d2
    moveq   #5,%d3
    jsr     LVO_Read(%a6)
    cmp.l   #5,%d0
    bne     fail
    clr.b   V_buf+5(%a4)
    move.l  %d5,%d1
    jsr     LVO_Close(%a6)
    lea     V_buf(%a4),%a0
    lea     s_hello(%pc),%a1
    bsr     streq
    bne     fail
    lea     p_up(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    jsr     LVO_Lock(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%d1
    jsr     LVO_UnLock(%a6)
    move.l  V_tmp(%a4),%d1
    jsr     LVO_CurrentDir(%a6)
    cmp.l   V_l1(%a4),%d0
    bne     fail
    move.l  V_l1(%a4),%d1
    jsr     LVO_UnLock(%a6)
    STEP    8                   /* a missing name: 0 and 205; DateStamp: minutes and ticks in range */
    lea     p_none(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    jsr     LVO_Lock(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne     fail
    lea     V_ds(%a4),%a0
    move.l  %a0,%d1
    jsr     LVO_DateStamp(%a6)
    lea     V_ds(%a4),%a0
    cmp.l   %a0,%d0
    bne     fail
    cmp.l   #1440,V_ds+4(%a4)
    bcc     fail
    cmp.l   #3000,V_ds+8(%a4)
    bcc     fail
    STEP    23                  /* a disk's name ZINE lacks: the program's own folder; T: never */
    lea     p_disk(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    CALL    V_dos, LVO_Lock
    tst.l   %d0
    beq     fail
    move.l  %d0,%d1
    jsr     LVO_UnLock(%a6)
    lea     p_t(%pc),%a0
    move.l  %a0,%d1
    moveq   #SHARED_LOCK,%d2
    jsr     LVO_Lock(%a6)
    tst.l   %d0
    bne     fail
    SAY     m_dos
    /* ---- graphics ---- */
    STEP    9                   /* a PAL hires screen: GetVPModeID, ModeNotAvailable */
    lea     scrtags(%pc),%a1
    sub.l   %a0,%a0
    CALL    V_int, LVO_OpenScreenTagList
    move.l  %d0,V_sc(%a4)
    beq     fail
    move.l  %d0,%a0
    lea     sc_ViewPort(%a0),%a0
    CALL    V_gfx, LVO_GetVPModeID
    cmp.l   #0x29000,%d0
    bne     fail
    move.l  #0x29000,%d0
    jsr     LVO_ModeNotAvailable(%a6)
    tst.l   %d0
    bne     fail
    move.l  #0x99000,%d0        /* DblPAL: not ZINE's */
    jsr     LVO_ModeNotAvailable(%a6)
    tst.l   %d0
    beq     fail
    STEP    10                  /* VideoControl: border blank on and read back, the ViewPort, the mode, IMMEDIATE */
    lea     V_tags(%a4),%a1
    move.l  #0x80000005,(%a1)+  /* VTAG_BORDERBLANK_SET */
    clr.l   (%a1)+
    move.l  #0x80000017,(%a1)+  /* VTAG_BORDERBLANK_GET */
    clr.l   (%a1)+
    move.l  #0x8000001B,(%a1)+  /* VTAG_ATTACH_CM_GET */
    clr.l   (%a1)+
    move.l  #0x80000020,(%a1)+  /* VTAG_VPMODEID_GET */
    clr.l   (%a1)+
    move.l  #0x80000037,(%a1)+  /* VTAG_IMMEDIATE */
    lea     V_flag(%a4),%a0
    move.l  %a0,(%a1)+
    clr.l   (%a1)
    moveq   #1,%d0
    move.l  %d0,V_flag(%a4)
    move.l  V_sc(%a4),%a0
    move.l  sc_ViewPort+4(%a0),%a0  /* vp_ColorMap */
    lea     V_tags(%a4),%a1
    jsr     LVO_VideoControl(%a6)
    tst.l   %d0
    bne     fail
    cmp.l   #1,V_tags+12(%a4)
    bne     fail
    move.l  V_sc(%a4),%d0
    add.l   #sc_ViewPort,%d0
    cmp.l   V_tags+20(%a4),%d0
    bne     fail
    cmp.l   #0x29000,V_tags+28(%a4)
    bne     fail
    tst.l   V_flag(%a4)
    bne     fail
    STEP    11                  /* ScrollVPort, RethinkDisplay, MakeScreen, MoveScreen: taken */
    move.l  V_sc(%a4),%a0
    move.l  sc_ViewPort+36(%a0),%a1     /* vp_RasInfo */
    move.w  #1,10(%a1)          /* RyOffset */
    lea     sc_ViewPort(%a0),%a0
    jsr     LVO_ScrollVPort(%a6)
    CALL    V_int, LVO_RethinkDisplay
    move.l  V_sc(%a4),%a0
    jsr     LVO_MakeScreen(%a6)
    move.l  V_sc(%a4),%a0
    moveq   #0,%d0
    moveq   #10,%d1
    jsr     LVO_MoveScreen(%a6)
    STEP    12                  /* QueryOverscan: text 0,0-639,255 in PAL hires; standard from -32,-14; invalid 0 */
    move.l  #0x29000,%a0
    lea     V_rect(%a4),%a1
    moveq   #1,%d0
    jsr     LVO_QueryOverscan(%a6)
    tst.l   %d0
    beq     fail
    tst.l   V_rect(%a4)
    bne     fail
    cmp.l   #(639 << 16) + 255,V_rect+4(%a4)
    bne     fail
    move.l  #0x29000,%a0
    lea     V_rect(%a4),%a1
    moveq   #2,%d0
    jsr     LVO_QueryOverscan(%a6)
    cmp.l   #0xFFE0FFF2,V_rect(%a4)    /* -32, -14 */
    bne     fail
    moveq   #-1,%d0
    move.l  %d0,%a0
    lea     V_rect(%a4),%a1
    moveq   #1,%d0
    jsr     LVO_QueryOverscan(%a6)
    tst.l   %d0
    bne     fail
    STEP    13                  /* BltClear: 2 rows of 16 bytes cleared, the byte after kept */
    move.l  4.w,%a6
    moveq   #64,%d0
    moveq   #MEMF_CHIP,%d1
    jsr     LVO_AllocMem(%a6)
    move.l  %d0,V_blk(%a4)
    beq     fail
    move.l  %d0,%a0
    moveq   #15,%d1
1:  move.l  #-1,(%a0)+
    dbf     %d1,1b
    move.l  V_blk(%a4),%a1
    move.l  #(2 << 16) + 16,%d0
    moveq   #2,%d1
    CALL    V_gfx, LVO_BltClear
    move.l  V_blk(%a4),%a0
    moveq   #7,%d1
2:  tst.l   (%a0)+
    bne     fail
    dbf     %d1,2b
    cmp.b   #0xFF,(%a0)
    bne     fail
    move.l  4.w,%a6
    move.l  V_blk(%a4),%a1
    moveq   #64,%d0
    jsr     LVO_FreeMem(%a6)
    STEP    14                  /* GetSprite: any (1), the same one again (-1), freed, then by number */
    lea     V_ss(%a4),%a0
    moveq   #-1,%d0
    CALL    V_gfx, LVO_GetSprite
    cmp.l   #1,%d0
    bne     fail
    cmp.w   #1,V_ss+10(%a4)
    bne     fail
    lea     V_ss(%a4),%a0
    moveq   #1,%d0
    jsr     LVO_GetSprite(%a6)
    cmp.l   #-1,%d0
    bne     fail
    moveq   #1,%d0
    jsr     LVO_FreeSprite(%a6)
    lea     V_ss(%a4),%a0
    moveq   #1,%d0
    jsr     LVO_GetSprite(%a6)
    cmp.l   #1,%d0
    bne     fail
    moveq   #1,%d0
    jsr     LVO_FreeSprite(%a6)
    STEP    15                  /* Draw, DrawBorder, PolyDraw into a BitMap of its own (64 x 8, one plane) */
    lea     V_bm(%a4),%a0
    moveq   #1,%d0
    moveq   #64,%d1
    moveq   #8,%d2
    jsr     LVO_InitBitMap(%a6)
    moveq   #64,%d0
    moveq   #8,%d1
    jsr     LVO_AllocRaster(%a6)
    move.l  %d0,V_bm+bm_Planes(%a4)
    beq     fail
    lea     V_rp(%a4),%a1
    jsr     LVO_InitRastPort(%a6)
    lea     V_bm(%a4),%a0
    move.l  %a0,V_rp+4(%a4)     /* rp_BitMap */
    lea     V_rp(%a4),%a1
    moveq   #1,%d0
    jsr     LVO_SetAPen(%a6)
    lea     V_rp(%a4),%a1
    moveq   #0,%d0
    moveq   #0,%d1
    jsr     LVO_Move(%a6)
    lea     V_rp(%a4),%a1
    moveq   #15,%d0
    moveq   #0,%d1
    jsr     LVO_Draw(%a6)
    lea     V_bord(%a4),%a1     /* a Border: 0,0, pen 1, JAM1, two points (0,0) (7,0) */
    clr.l   (%a1)
    move.l  #0x01000002,4(%a1)
    lea     bxy(%pc),%a0
    move.l  %a0,8(%a1)
    clr.l   12(%a1)
    lea     V_rp(%a4),%a0
    moveq   #0,%d0
    moveq   #2,%d1
    CALL    V_int, LVO_DrawBorder
    lea     V_rp(%a4),%a1
    moveq   #0,%d0
    moveq   #4,%d1
    CALL    V_gfx, LVO_Move
    lea     V_rp(%a4),%a1
    moveq   #1,%d0
    lea     pxy(%pc),%a0
    jsr     LVO_PolyDraw(%a6)
    move.l  V_bm+bm_Planes(%a4),%a0
    cmp.l   #0xFFFF0000,(%a0)   /* row 0: 16 pixels */
    bne     fail
    cmp.w   #0xFF00,16(%a0)     /* row 2: the border's 8 */
    bne     fail
    cmp.w   #0xF000,32(%a0)     /* row 4: 4 */
    bne     fail
    SAY     m_gfx
    /* ---- the second batch (16.13: what the reruns asked for next) ---- */
    STEP    24                  /* WritePixel, ReadPixel */
    lea     V_rp(%a4),%a1
    moveq   #20,%d0
    moveq   #6,%d1
    jsr     LVO_WritePixel(%a6)
    tst.l   %d0
    bne     fail
    lea     V_rp(%a4),%a1
    moveq   #20,%d0
    moveq   #6,%d1
    jsr     LVO_ReadPixel(%a6)
    cmp.l   #1,%d0
    bne     fail
    lea     V_rp(%a4),%a1
    moveq   #21,%d0
    moveq   #6,%d1
    jsr     LVO_ReadPixel(%a6)
    tst.l   %d0
    bne     fail
    STEP    25                  /* a ColorMap: SetRGB4CM, GetRGB4, GetRGB32 */
    moveq   #32,%d0
    jsr     LVO_GetColorMap(%a6)
    move.l  %d0,V_cm(%a4)
    beq     fail
    move.l  %d0,%a0
    moveq   #1,%d0
    moveq   #15,%d1
    moveq   #8,%d2
    moveq   #0,%d3
    jsr     LVO_SetRGB4CM(%a6)
    move.l  V_cm(%a4),%a0
    moveq   #1,%d0
    jsr     LVO_GetRGB4(%a6)
    cmp.l   #0xF80,%d0
    bne     fail
    move.l  V_cm(%a4),%a0
    moveq   #1,%d0
    moveq   #1,%d1
    lea     V_rgb(%a4),%a1
    jsr     LVO_GetRGB32(%a6)
    cmp.l   #0xFFFFFFFF,V_rgb(%a4)
    bne     fail
    cmp.l   #0x88888888,V_rgb+4(%a4)
    bne     fail
    tst.l   V_rgb+8(%a4)
    bne     fail
    STEP    26                  /* a View of its own: LoadView, MrgCop, WaitBOVP, a DBufInfo's ChangeVPBitMap */
    lea     V_view(%a4),%a1
    jsr     LVO_InitView(%a6)
    lea     V_vp(%a4),%a0
    jsr     LVO_InitVPort(%a6)
    lea     V_vp(%a4),%a0
    move.l  V_cm(%a4),4(%a0)    /* vp_ColorMap */
    move.w  #64,24(%a0)         /* DWidth, DHeight */
    move.w  #8,26(%a0)
    lea     V_ri(%a4),%a1
    move.l  %a1,36(%a0)         /* vp_RasInfo */
    clr.l   (%a1)
    clr.l   8(%a1)
    lea     V_bm(%a4),%a2
    move.l  %a2,4(%a1)          /* ri_BitMap */
    move.l  %a0,V_view(%a4)     /* v_ViewPort */
    lea     V_view(%a4),%a1
    jsr     LVO_LoadView(%a6)
    lea     V_view(%a4),%a0
    cmp.l   gb_ActiView(%a6),%a0
    bne     fail
    lea     V_view(%a4),%a1
    jsr     LVO_MrgCop(%a6)
    lea     V_vp(%a4),%a0
    jsr     LVO_WaitBOVP(%a6)
    lea     V_vp(%a4),%a0
    jsr     LVO_AllocDBufInfo(%a6)
    move.l  %d0,V_dbi(%a4)
    beq     fail
    clr.l   V_ri+4(%a4)
    lea     V_vp(%a4),%a0
    lea     V_bm(%a4),%a1
    move.l  V_dbi(%a4),%a2
    jsr     LVO_ChangeVPBitMap(%a6)
    lea     V_bm(%a4),%a0
    cmp.l   V_ri+4(%a4),%a0
    bne     fail
    move.l  V_dbi(%a4),%a1
    jsr     LVO_FreeDBufInfo(%a6)
    sub.l   %a1,%a1
    jsr     LVO_LoadView(%a6)
    move.l  V_cm(%a4),%a0
    jsr     LVO_FreeColorMap(%a6)
    move.l  V_bm+bm_Planes(%a4),%a0
    moveq   #64,%d0
    moveq   #8,%d1
    jsr     LVO_FreeRaster(%a6)
    STEP    27                  /* FilePart, PathPart, AddPart, Fault, PrintFault; exec's SumKickData */
    lea     p_part(%pc),%a0
    move.l  %a0,%d1
    CALL    V_dos, LVO_FilePart
    lea     p_part+4(%pc),%a0
    cmp.l   %a0,%d0
    bne     fail
    lea     p_part(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_PathPart(%a6)
    lea     p_part+3(%pc),%a0
    cmp.l   %a0,%d0
    bne     fail
    lea     V_buf(%a4),%a0
    move.l  #0x44617461,(%a0)   /* "Data" */
    clr.b   4(%a0)
    move.l  %a0,%d1
    lea     s_sub(%pc),%a1
    move.l  %a1,%d2
    moveq   #64,%d3
    jsr     LVO_AddPart(%a6)
    tst.l   %d0
    beq     fail
    lea     V_buf(%a4),%a0
    lea     s_datasub(%pc),%a1
    bsr     streq
    bne     fail
    move.l  #205,%d1
    lea     s_x(%pc),%a0
    move.l  %a0,%d2
    lea     V_buf(%a4),%a0
    move.l  %a0,%d3
    moveq   #64,%d4
    jsr     LVO_Fault(%a6)
    cmp.l   #19,%d0
    bne     fail
    lea     V_buf(%a4),%a0
    lea     s_fault(%pc),%a1
    bsr     streq
    bne     fail
    move.l  #232,%d1
    lea     myname(%pc),%a0
    move.l  %a0,%d2
    jsr     LVO_PrintFault(%a6)
    move.l  4.w,%a6
    jsr     LVO_SumKickData(%a6)
    tst.l   %d0
    bne     fail
    STEP    28                  /* timer.device UNIT_WAITECLOCK: an E-clock time 0.1 s ahead, 2 s into the session */
    move.l  #100,%d1
    CALL    V_dos, LVO_Delay
    move.l  4.w,%a6
    jsr     LVO_CreateMsgPort(%a6)
    move.l  %d0,%d5
    beq     fail
    move.l  %d0,%a0
    moveq   #40,%d0
    jsr     LVO_CreateIORequest(%a6)
    move.l  %d0,V_io(%a4)
    beq     fail
    lea     tdname(%pc),%a0
    moveq   #4,%d0              /* UNIT_WAITECLOCK */
    move.l  V_io(%a4),%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne     fail
    move.l  V_io(%a4),%a1
    move.l  20(%a1),%a6         /* io_Device: timer's base */
    lea     V_ev(%a4),%a0
    jsr     LVO_ReadEClock(%a6)
    move.l  %d0,%d4             /* E-clock ticks a second */
    divu.l  #10,%d0
    move.l  V_io(%a4),%a1
    move.l  V_ev(%a4),32(%a1)
    move.l  V_ev+4(%a4),%d1
    add.l   %d0,%d1
    move.l  %d1,36(%a1)
    bcc.s   1f
    addq.l  #1,32(%a1)
1:  move.w  #9,28(%a1)          /* TR_ADDREQUEST */
    move.l  4.w,%a6
    jsr     LVO_DoIO(%a6)
    move.l  V_io(%a4),%a1
    move.l  20(%a1),%a6
    lea     V_ev+8(%a4),%a0
    jsr     LVO_ReadEClock(%a6)
    move.l  V_ev+12(%a4),%d0
    sub.l   V_ev+4(%a4),%d0     /* the ticks it took */
    move.l  %d4,%d1
    divu.l  #20,%d1
    cmp.l   %d1,%d0             /* at least 0.05 s (a frame's grain) */
    bcs     fail
    cmp.l   %d4,%d0             /* under a second */
    bcc     fail
    move.l  4.w,%a6
    move.l  V_io(%a4),%a1
    jsr     LVO_CloseDevice(%a6)
    move.l  V_io(%a4),%a0
    jsr     LVO_DeleteIORequest(%a6)
    move.l  %d5,%a0
    jsr     LVO_DeleteMsgPort(%a6)
    SAY     m_more
    /* ---- intuition ---- */
    STEP    16                  /* WBenchToFront (none: FALSE), ActivateWindow, Remember lists freed whole */
    CALL    V_int, LVO_WBenchToFront
    tst.l   %d0
    bne     fail
    sub.l   %a0,%a0
    jsr     LVO_ActivateWindow(%a6)
    move.l  4.w,%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  %d0,V_tmp(%a4)
    clr.l   V_key(%a4)
    lea     V_key(%a4),%a0
    moveq   #100,%d0
    move.l  #0x10000,%d1
    CALL    V_int, LVO_AllocRemember
    tst.l   %d0
    beq     fail
    lea     V_key(%a4),%a0
    move.l  #300,%d0
    moveq   #0,%d1
    jsr     LVO_AllocRemember(%a6)
    tst.l   %d0
    beq     fail
    lea     V_key(%a4),%a0
    moveq   #1,%d0
    jsr     LVO_FreeRemember(%a6)
    tst.l   V_key(%a4)
    bne     fail
    move.l  4.w,%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    cmp.l   V_tmp(%a4),%d0
    bne     fail
    STEP    17                  /* EasyRequestArgs: the leftmost of two gadgets is 1; one gadget 0 */
    lea     V_es(%a4),%a1
    moveq   #20,%d0
    move.l  %d0,(%a1)
    clr.l   4(%a1)
    lea     es_title(%pc),%a0
    move.l  %a0,8(%a1)
    lea     es_text(%pc),%a0
    move.l  %a0,12(%a1)
    lea     es_gads(%pc),%a0
    move.l  %a0,16(%a1)
    lea     s_one(%pc),%a0
    move.l  %a0,V_tmp(%a4)      /* the argument array: one string */
    lea     V_tmp(%a4),%a3
    sub.l   %a0,%a0
    sub.l   %a2,%a2
    CALL    V_int, LVO_EasyRequestArgs
    cmp.l   #1,%d0
    bne     fail
    lea     es_ok(%pc),%a0
    move.l  %a0,V_es+16(%a4)
    lea     V_es(%a4),%a1
    lea     V_tmp(%a4),%a3
    sub.l   %a0,%a0
    jsr     LVO_EasyRequestArgs(%a6)
    tst.l   %d0
    bne     fail
    STEP    18                  /* DisplayAlert: a recovery alert, FALSE */
    moveq   #0,%d0
    lea     alert(%pc),%a0
    moveq   #30,%d1
    jsr     LVO_DisplayAlert(%a6)
    tst.l   %d0
    bne     fail
    move.l  V_sc(%a4),%a0
    jsr     LVO_CloseScreen(%a6)
    SAY     m_int
    /* ---- diskfont ---- */
    STEP    19                  /* ztest 11 from FONTS: (/usr/share/fonts/amiga): proportional, A 4 + B 5 */
    OPENLIB dfname, V_df
    DFONT   ztestname, 11
    move.l  %d0,V_tf(%a4)
    beq     fail
    move.l  %d0,%a0
    cmp.w   #11,20(%a0)         /* tf_YSize */
    bne     fail
    btst    #1,23(%a0)          /* tf_Flags FPF_DISKFONT */
    beq     fail
    lea     V_bm(%a4),%a0
    move.l  %a0,V_rp+4(%a4)
    lea     V_rp(%a4),%a1
    move.l  V_tf(%a4),%a0
    CALL    V_gfx, LVO_SetFont
    lea     V_rp(%a4),%a1
    lea     s_AB(%pc),%a0
    moveq   #2,%d0
    jsr     LVO_TextLength(%a6)
    cmp.l   #9,%d0
    bne     fail
    lea     V_ta(%a4),%a0       /* in memory now: OpenFont finds the same one */
    jsr     LVO_OpenFont(%a6)
    cmp.l   V_tf(%a4),%d0
    bne     fail
    move.l  %d0,%a1
    jsr     LVO_CloseFont(%a6)
    move.l  V_tf(%a4),%a1
    jsr     LVO_CloseFont(%a6)
    STEP    20                  /* zfixed 9 from the program's own Fonts folder: 7 wide */
    DFONT   zfixname, 9
    move.l  %d0,V_tf(%a4)
    beq     fail
    move.l  %d0,%a0
    cmp.w   #9,20(%a0)
    bne     fail
    cmp.w   #7,24(%a0)          /* tf_XSize */
    bne     fail
    lea     V_rp(%a4),%a1
    move.l  V_tf(%a4),%a0
    CALL    V_gfx, LVO_SetFont
    lea     V_rp(%a4),%a1
    lea     s_AB(%pc),%a0
    moveq   #2,%d0
    jsr     LVO_TextLength(%a6)
    cmp.l   #14,%d0
    bne     fail
    STEP    21                  /* missing (0, said), damaged (0, said), a size it lacks (the nearest, said) */
    DFONT   nonename, 8
    tst.l   %d0
    bne     fail
    DFONT   zbadname, 11
    tst.l   %d0
    bne     fail
    DFONT   ztestname, 12
    tst.l   %d0
    beq     fail
    move.l  %d0,%a0
    cmp.w   #11,20(%a0)
    bne     fail
    DFONT   ztestname, 12, 0x40 /* FPF_DESIGNED: only the size itself */
    tst.l   %d0
    bne     fail
    STEP    22                  /* AvailFonts: memory and disk fonts listed, at least topaz, ztest, zfixed */
    lea     V_av(%a4),%a0
    move.l  #512,%d0
    moveq   #3,%d1
    CALL    V_df, LVO_AvailFonts
    tst.l   %d0
    bne     fail
    cmp.w   #3,V_av(%a4)
    bcs     fail
    lea     V_av(%a4),%a0
    moveq   #4,%d0
    moveq   #3,%d1
    jsr     LVO_AvailFonts(%a6)
    tst.l   %d0                 /* a buffer too small: the bytes more it needs */
    beq     fail
    STEP    23                  /* 16.18: the DOS list (the program's volume ZINE, the RAM device) and CompareDates */
    move.l  #13,%d1             /* LDF_READ, LDF_DEVICES, LDF_VOLUMES */
    CALL    V_dos, LVO_LockDosList
    move.l  %d0,%d5
    beq     fail
    move.l  %d5,%d1
    moveq   #8,%d2              /* LDF_VOLUMES: the program's volume first */
    jsr     LVO_NextDosEntry(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%a0
    cmp.l   #2,4(%a0)           /* DLT_VOLUME */
    bne     fail
    move.l  40(%a0),%d0         /* dol_Name, a BSTR: "ZINE" */
    lsl.l   #2,%d0
    move.l  %d0,%a1
    cmp.l   #0x045A494E,(%a1)   /* length 4, "ZIN" */
    bne     fail
    cmp.b   #'E',4(%a1)
    bne     fail
    move.l  %a0,%d1
    moveq   #4,%d2              /* LDF_DEVICES: then the RAM device */
    jsr     LVO_NextDosEntry(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%a0
    tst.l   4(%a0)              /* DLT_DEVICE */
    bne     fail
    move.l  %d5,%d1
    lea     p_ramdisk(%pc),%a0
    move.l  %a0,%d2
    moveq   #8,%d3
    jsr     LVO_FindDosEntry(%a6)
    tst.l   %d0                 /* "ram disk" found without case */
    beq     fail
    move.l  %d5,%d1
    lea     p_none(%pc),%a0
    move.l  %a0,%d2
    moveq   #12,%d3
    jsr     LVO_FindDosEntry(%a6)
    tst.l   %d0
    bne     fail
    move.l  #13,%d1
    jsr     LVO_UnLockDosList(%a6)
    lea     p_disk(%pc),%a0     /* GetDeviceProc: ZINE's handler port, the volume's node; again: 0 */
    move.l  %a0,%d1
    moveq   #0,%d2
    jsr     LVO_GetDeviceProc(%a6)
    tst.l   %d0
    beq     fail
    move.l  %d0,%a2
    tst.l   (%a2)               /* dvp_Port */
    beq     fail
    tst.l   12(%a2)             /* dvp_DevNode */
    beq     fail
    move.l  (%a2),%d1           /* 16.19: DoPkt to that port: ACTION_IS_FILESYSTEM yes, ACTION_DISK_INFO 512-byte */
    move.l  #1027,%d2           /* blocks; to a port that is not a handler: DOSFALSE, ERROR_ACTION_NOT_KNOWN */
    jsr     LVO_DoPkt(%a6)
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne     fail
    move.l  (%a2),%d1
    moveq   #25,%d2
    lea     infodata(%pc),%a0
    move.l  %a0,%d3
    lsr.l   #2,%d3
    jsr     LVO_DoPkt(%a6)
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne     fail
    lea     infodata(%pc),%a0
    cmp.l   #512,20(%a0)        /* id_BytesPerBlock */
    bne     fail
    lea     infodata(%pc),%a0
    move.l  %a0,%d1             /* not a port at all */
    moveq   #25,%d2
    jsr     LVO_DoPkt(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #209,%d0
    bne     fail
    lea     handles(%pc),%a3        /* 16.19: 40 files open at once (ZINE's most was 16; Hexel keeps more) */
    moveq   #39,%d4
1:  lea     p_disk(%pc),%a0
    move.l  %a0,%d1
    move.l  #1005,%d2               /* MODE_OLDFILE */
    jsr     LVO_Open(%a6)
    move.l  %d0,(%a3)+
    beq     fail
    dbra    %d4,1b
    moveq   #39,%d4
2:  move.l  -(%a3),%d1
    jsr     LVO_Close(%a6)
    dbra    %d4,2b
    lea     p_disk(%pc),%a0
    move.l  %a0,%d1
    move.l  %a2,%d2
    jsr     LVO_GetDeviceProc(%a6)
    tst.l   %d0
    bne     fail
    move.l  %a2,%d1
    jsr     LVO_FreeDeviceProc(%a6)
    lea     ds_late(%pc),%a0
    move.l  %a0,%d1
    lea     ds_early(%pc),%a0
    move.l  %a0,%d2
    jsr     LVO_CompareDates(%a6)
    tst.l   %d0                 /* date1 the later: below 0 */
    bpl     fail
    lea     ds_early(%pc),%a0
    move.l  %a0,%d1
    lea     ds_late(%pc),%a0
    move.l  %a0,%d2
    jsr     LVO_CompareDates(%a6)
    tst.l   %d0                 /* date2 the later: above 0 */
    ble     fail
    move.l  %d2,%d1
    jsr     LVO_CompareDates(%a6)
    tst.l   %d0
    bne     fail
    moveq   #3,%d4              /* CopyMem at odd alignments (16.18's copy: longs once the destination is aligned) */
    moveq   #1,%d7
50: lea     V_av(%a4),%a0
    moveq   #15,%d0
52: clr.l   (%a0)+
    dbra    %d0,52b
    lea     cm_src(%pc),%a0
    add.l   %d7,%a0
    lea     V_av(%a4),%a1
    add.l   %d4,%a1
    move.l  #45,%d0
    move.l  4.w,%a6
    jsr     LVO_CopyMem(%a6)
    lea     cm_src(%pc),%a0
    add.l   %d7,%a0
    lea     V_av(%a4),%a1
    add.l   %d4,%a1
    moveq   #44,%d0
51: cmpm.b  (%a0)+,(%a1)+
    dbne    %d0,51b
    bne     fail
    tst.b   (%a1)               /* the byte after: untouched (cleared before) */
    bne     fail
    subq.l  #1,%d4
    bpl.s   50b
    lea     V_bm(%a4),%a0       /* RectFill (16.18: a byte at a time): 64 x 4, one plane */
    moveq   #1,%d0
    moveq   #64,%d1
    moveq   #4,%d2
    CALL    V_gfx, LVO_InitBitMap
    moveq   #64,%d0
    moveq   #4,%d1
    jsr     LVO_AllocRaster(%a6)
    move.l  %d0,V_bm+bm_Planes(%a4)
    beq     fail
    lea     V_rp(%a4),%a1
    jsr     LVO_InitRastPort(%a6)
    lea     V_bm(%a4),%a0
    move.l  %a0,V_rp+4(%a4)
    lea     V_rp(%a4),%a1
    moveq   #1,%d0
    jsr     LVO_SetAPen(%a6)
    lea     V_rp(%a4),%a1
    moveq   #3,%d0
    moveq   #1,%d1
    moveq   #40,%d2
    moveq   #2,%d3
    jsr     LVO_RectFill(%a6)
    lea     V_rp(%a4),%a1
    moveq   #2,%d0              /* COMPLEMENT */
    jsr     LVO_SetDrMd(%a6)
    lea     V_rp(%a4),%a1
    moveq   #4,%d0
    moveq   #1,%d1
    moveq   #7,%d2
    moveq   #1,%d3
    jsr     LVO_RectFill(%a6)
    move.l  V_bm+bm_Planes(%a4),%a0
    tst.l   (%a0)               /* row 0 untouched */
    bne     fail
    cmp.l   #0x10FFFFFF,8(%a0)  /* row 1: 3-40 set, then 4-7 complemented */
    bne     fail
    cmp.l   #0xFF800000,12(%a0)
    bne     fail
    cmp.l   #0x1FFFFFFF,16(%a0) /* row 2: 3-40 */
    bne     fail
    cmp.l   #0xFF800000,20(%a0)
    bne     fail
    tst.l   24(%a0)             /* row 3 untouched */
    bne     fail
    moveq   #64,%d0
    moveq   #4,%d1
    jsr     LVO_FreeRaster(%a6)
    SAY     m_dosl
    SAY     m_font
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
dfname:   .asciz "diskfont.library"
semname:  .asciz "znext.nosuch"
p_file:   .asciz "Data/Sub/f.txt"
p_data:   .asciz "DATA"
p_rel:    .asciz "Sub/f.txt"
p_up:     .asciz "/znext"
p_none:   .asciz "nosuch.file"
p_disk:   .asciz "ZDisk:Data/Sub/f.txt"
    .balign 4
infodata: .space 36
handles:  .space 160
p_t:      .asciz "T:znext"
p_ramdisk: .asciz "ram disk"
m_dosl:   .asciz "znext: the DOS list, GetDeviceProc, DoPkt, 40 open files, CompareDates, CopyMem and RectFill answered\n"
    .balign 4
cm_src:   .ascii "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJ"
          .byte 0x5A
    .balign 4
ds_late:  .long 17000, 600, 10
ds_early: .long 17000, 599, 2000
s_ftxt:   .asciz "f.txt"
s_sub:    .asciz "Sub"
s_zdata:  .asciz "ZINE:Data"
s_hello:  .asciz "hello"
s_one:    .asciz "one"
s_AB:     .asciz "AB"
es_title: .asciz "ZNext"
es_text:  .asciz "Pick %s"
es_gads:  .asciz "Yes|No"
es_ok:    .asciz "OK"
ztestname: .asciz "ztest.font"
zfixname: .asciz "zfixed.font"
zbadname: .asciz "zbad.font"
nonename: .asciz "nosuch.font"
alert:    .byte 0, 20, 15
          .asciz "ZNEXT ALERT"
          .byte 0
m_exec:   .asciz "znext: exec's Allocate, Deallocate and FindSemaphore answered\n"
m_dos:    .asciz "znext: Lock, Examine, ExNext, NameFromLock, ParentDir, CurrentDir and DateStamp answered\n"
m_gfx:    .asciz "znext: GetVPModeID, ModeNotAvailable, VideoControl, QueryOverscan, BltClear, sprites, Draw, DrawBorder answered\n"
m_int:    .asciz "znext: intuition's Remember lists, requester and alert answered\n"
m_font:   .asciz "znext: diskfont opened fonts from FONTS: and the program's own folder; AvailFonts listed them\n"
m_more:   .asciz "znext: the second batch (pixels, ColorMaps, a View of its own, DBufInfo, FilePart, Fault, timer units) answered\n"
m_fail:   .asciz "znext: a step failed\n"
p_part:   .asciz "a/b/c.txt"
tdname:   .asciz "timer.device"
s_datasub: .asciz "Data/Sub"
s_x:      .asciz "x"
s_fault:  .asciz "x: object not found"
myname:   .asciz "znext"
    .even
scrtags:  .long 0x80000025, 2, 0x80000032, 0x29000, 0      /* SA_Depth 2, SA_DisplayID PAL HIRES */
bxy:      .word 0, 0, 7, 0
pxy:      .word 3, 4
vars:     .space V_end
