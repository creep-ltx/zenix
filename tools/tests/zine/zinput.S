/* SPDX-License-Identifier: GPL-2.0-only */
/* zinput (d29 16.17): the keys and the mouse for programs, each way AmigaOS gives them, while the test types on the
 * emulated keyboard (tools/zine-input-test.sh: a, b, c, F1, the fire button of a joystick in port 0 (the left mouse
 * button's line), then q). A window (IDCMP VANILLAKEY, RAWKEY, MOUSEBUTTONS) read through gadtools' GT_GetIMsg and
 * GT_ReplyIMsg, with a gadget list (CreateContext, CreateGadgetA, FreeGadgets) and menus (CreateMenusA, FreeMenus);
 * console.device's CMD_READ on the window; an input.device handler (priority 60) counting the key events;
 * keyboard.device's KBD_READEVENT; keymap.library's MapRawKey. Ends at the VANILLAKEY q and writes one line:
 *   zinput: console C vanilla V raw R keyboard K handler H buttons B map M gadtools G front F rows R
 * C the console's first characters (CSI as ^), V the VANILLAKEY characters, R the last RAWKEY key-down code, K the
 * first keyboard.device code, H the handler's key events, B the MOUSEBUTTONS codes, M MapRawKey of Shift+a, G 1 when
 * the gadget and menu calls answered as the autodocs say, F 1 when IntuitionBase's FirstScreen is the screen the
 * window is on, R the PAL monitor's rows from GetDisplayInfoData (312). Its return code is 0. */
#include "zt.i"
#define LVO_OpenDevice  -444
#define LVO_CloseDevice -450
#define LVO_SendIO      -462
#define LVO_DoIO        -456
#define LVO_WaitIO      -474
#define LVO_AbortIO     -480
#define LVO_Wait        -318
#define LVO_GetMsg      -372
#define LVO_ReplyMsg    -378
#define LVO_CreateMsgPort -666
#define LVO_DeleteMsgPort -672
#define LVO_RawDoFmt    -522
#define LVO_OpenWindowTagList -606
#define LVO_CloseWindow -72
#define LVO_CreateGadgetA -30
#define LVO_FreeGadgets -36
#define LVO_CreateMenusA -48
#define LVO_FreeMenus   -54
#define LVO_GT_GetIMsg  -72
#define LVO_GT_ReplyIMsg -78
#define LVO_CreateContext -114
#define LVO_MapRawKey   -42
#define LVO_GetDisplayInfoData -756
#define v_dos 0
#define v_int 4
#define v_gt 8
#define v_km 12
#define v_win 16
#define v_port 20
#define v_glist 24
#define v_raw 28
#define v_kbd 32
#define v_hcount 36
#define v_done 40
#define v_kdone 41
#define v_con 42
#define v_van 58
#define v_but 74
#define v_map 90
#define v_gtok 96
#define v_front 98
#define v_scr 140
#define v_rows 144
#define v_args 100
#define v_line 148
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4           /* a4: the variables */
    move.l  4.w,%a6
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,v_dos(%a4)
    beq.w   99f
    lea     intname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,v_int(%a4)
    beq.w   99f
    lea     gtname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,v_gt(%a4)
    beq.w   99f
    lea     kmname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,v_km(%a4)
    beq.w   99f
    /* gadtools: a context, a gadget behind it, menus; all freed */
    move.l  v_gt(%a4),%a6
    lea     v_glist(%a4),%a0
    jsr     LVO_CreateContext(%a6)
    move.l  %d0,%a0                 /* previous: the context */
    tst.l   %d0
    beq.s   1f
    moveq   #1,%d0                  /* BUTTON_KIND */
    lea     newgad(%pc),%a1
    sub.l   %a2,%a2
    jsr     LVO_CreateGadgetA(%a6)
    tst.l   %d0
    beq.s   1f
    move.l  %d0,%a0
    cmp.w   #77,38(%a0)             /* gg_GadgetID from ng_GadgetID */
    bne.s   1f
    move.l  v_glist(%a4),%a0
    cmp.l   (%a0),%d0               /* linked behind the context */
    bne.s   1f
    jsr     LVO_FreeGadgets(%a6)
    lea     newmenu(%pc),%a0
    sub.l   %a1,%a1
    jsr     LVO_CreateMenusA(%a6)
    tst.l   %d0
    beq.s   1f
    move.l  %d0,%a0
    move.l  18(%a0),%a1             /* mu_FirstItem */
    move.l  %a1,%d1
    beq.s   1f
    cmp.l   #0x1234,34(%a1)         /* GTMENUITEM_USERDATA */
    bne.s   1f
    jsr     LVO_FreeMenus(%a6)
    move.b  #'1',v_gtok(%a4)
1:  /* graphics: the display database's monitor record of PAL lores (TotalRows) */
    move.l  4.w,%a6
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq.s   2f
    move.l  %d0,%a6
    sub.l   %a0,%a0
    lea     v_line(%a4),%a1         /* a buffer for now */
    moveq   #96,%d0
    move.l  #0x80002000,%d1         /* DTAG_MNTR */
    move.l  #0x00021000,%d2         /* PAL_MONITOR_ID, lores */
    jsr     LVO_GetDisplayInfoData(%a6)
    cmp.l   #96,%d0
    bne.s   3f
    moveq   #0,%d0
    move.w  v_line+36(%a4),%d0      /* mtr_TotalRows */
    move.l  %d0,v_rows(%a4)
3:  move.l  %a6,%a1
    move.l  4.w,%a6
    jsr     LVO_CloseLibrary(%a6)
2:  /* keymap: MapRawKey of Shift+a */
    move.l  v_km(%a4),%a6
    lea     fakeev(%pc),%a0
    lea     v_map(%a4),%a1
    moveq   #4,%d1
    sub.l   %a2,%a2
    jsr     LVO_MapRawKey(%a6)
    /* a screen: in front, so IntuitionBase's FirstScreen is it; the window on it */
    move.l  v_int(%a4),%a6
    sub.l   %a0,%a0
    lea     stags(%pc),%a1
    jsr     LVO_OpenScreenTagList(%a6)
    move.l  %d0,v_scr(%a4)
    beq.w   99f
    lea     wtags(%pc),%a1
    move.l  %d0,wscr-wtags(%a1)
    cmp.l   60(%a6),%d0             /* ib_FirstScreen */
    bne.s   2f
    move.b  #'1',v_front(%a4)
2:  sub.l   %a0,%a0
    jsr     LVO_OpenWindowTagList(%a6)
    move.l  %d0,v_win(%a4)
    beq.w   99f
    move.l  4.w,%a6
    jsr     LVO_CreateMsgPort(%a6)
    move.l  %d0,v_port(%a4)
    beq.w   99f
    /* console.device on the window, a CMD_READ of one byte */
    lea     conio(%pc),%a3
    move.l  v_win(%a4),40(%a3)
    move.l  #136,36(%a3)
    move.l  %d0,14(%a3)
    lea     conname(%pc),%a0
    moveq   #0,%d0
    move.l  %a3,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne.w   99f
    bsr.w   conread
    /* input.device: the handler */
    lea     inpio(%pc),%a3
    move.l  v_port(%a4),14(%a3)
    lea     inpname(%pc),%a0
    moveq   #0,%d0
    move.l  %a3,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne.w   99f
    move.w  #9,28(%a3)              /* IND_ADDHANDLER */
    lea     hint(%pc),%a0
    lea     handler(%pc),%a1
    move.l  %a1,18(%a0)             /* is_Code */
    move.l  %a4,14(%a0)             /* is_Data: the variables */
    move.l  %a0,40(%a3)
    move.l  %a3,%a1
    jsr     LVO_DoIO(%a6)
    /* keyboard.device: one KBD_READEVENT */
    lea     kbdio(%pc),%a3
    move.l  v_port(%a4),14(%a3)
    lea     kbdname(%pc),%a0
    moveq   #0,%d0
    move.l  %a3,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne.w   99f
    move.w  #9,28(%a3)
    lea     kev(%pc),%a0
    move.l  %a0,40(%a3)
    moveq   #22,%d0
    move.l  %d0,36(%a3)
    move.l  %a3,%a1
    jsr     LVO_SendIO(%a6)
    lea     ready(%pc),%a0
    bsr.w   say
    /* the loop: until the VANILLAKEY q */
10: move.l  v_win(%a4),%a0
    move.l  86(%a0),%a0             /* wd_UserPort */
    moveq   #0,%d0
    move.b  15(%a0),%d1             /* mp_SigBit */
    bset    %d1,%d0
    move.l  v_port(%a4),%a0
    move.b  15(%a0),%d1
    bset    %d1,%d0
    move.l  4.w,%a6
    jsr     LVO_Wait(%a6)
11: move.l  v_gt(%a4),%a6
    move.l  v_win(%a4),%a0
    move.l  86(%a0),%a0
    jsr     LVO_GT_GetIMsg(%a6)
    tst.l   %d0
    beq.s   20f
    move.l  %d0,%a1
    move.l  20(%a1),%d2             /* im_Class */
    moveq   #0,%d3
    move.w  24(%a1),%d3             /* im_Code */
    jsr     LVO_GT_ReplyIMsg(%a6)
    cmp.l   #0x200000,%d2           /* VANILLAKEY */
    bne.s   12f
    lea     v_van(%a4),%a0
    bsr.w   append
    cmp.b   #'q',%d3
    bne.w   11b
    st      v_done(%a4)
    bra.w   11b
12: cmp.l   #0x400,%d2              /* RAWKEY: the key-downs */
    bne.s   13f
    tst.b   %d3
    bmi.s   11b
    move.l  %d3,v_raw(%a4)
    bra.w   11b
13: cmp.l   #8,%d2                  /* MOUSEBUTTONS: SELECTDOWN as 'D', SELECTUP as 'U' */
    bne.w   11b
    moveq   #'D',%d0
    cmp.b   #0x68,%d3
    beq.s   14f
    moveq   #'U',%d0
    cmp.b   #0xE8,%d3
    beq.s   14f
    moveq   #'?',%d0
14: move.b  %d0,%d3
    lea     v_but(%a4),%a0
    bsr.w   append
    bra.w   11b
20: move.l  4.w,%a6                 /* the device replies */
    move.l  v_port(%a4),%a0
    jsr     LVO_GetMsg(%a6)
    tst.l   %d0
    beq.w   30f
    lea     conio(%pc),%a0
    cmp.l   %a0,%d0
    bne.s   21f
    tst.l   32(%a0)                 /* io_Actual */
    beq.s   22f
    move.b  cbuf(%pc),%d3
    cmp.b   #0x9B,%d3
    bne.s   23f
    moveq   #'^',%d3
23: lea     v_con(%a4),%a0
    bsr.w   append
22: tst.b   v_done(%a4)
    bne.w   20b
    bsr.w   conread
    bra.w   20b
21: lea     kbdio(%pc),%a0
    cmp.l   %a0,%d0
    bne.w   20b
    lea     kev(%pc),%a0
    moveq   #0,%d0
    move.w  6(%a0),%d0              /* ie_Code */
    move.l  %d0,v_kbd(%a4)
    st      v_kdone(%a4)
    bra.w   20b
30: tst.b   v_done(%a4)
    beq.w   10b
    /* the end: everything closed */
    move.l  4.w,%a6
    lea     conio(%pc),%a1
    jsr     LVO_AbortIO(%a6)
    lea     conio(%pc),%a1
    jsr     LVO_WaitIO(%a6)
    lea     conio(%pc),%a1
    jsr     LVO_CloseDevice(%a6)
    tst.b   v_kdone(%a4)
    bne.s   31f
    lea     kbdio(%pc),%a1
    jsr     LVO_AbortIO(%a6)
    lea     kbdio(%pc),%a1
    jsr     LVO_WaitIO(%a6)
31: lea     kbdio(%pc),%a1
    jsr     LVO_CloseDevice(%a6)
    lea     inpio(%pc),%a1
    move.w  #10,28(%a1)             /* IND_REMHANDLER */
    jsr     LVO_DoIO(%a6)
    lea     inpio(%pc),%a1
    jsr     LVO_CloseDevice(%a6)
    move.l  v_int(%a4),%a6
    move.l  v_win(%a4),%a0
    jsr     LVO_CloseWindow(%a6)
    move.l  v_scr(%a4),%a0
    jsr     LVO_CloseScreen(%a6)
    move.l  4.w,%a6
    move.l  v_port(%a4),%a0
    jsr     LVO_DeleteMsgPort(%a6)
    /* the line */
    lea     v_con(%a4),%a0
    move.l  %a0,v_args(%a4)
    lea     v_van(%a4),%a0
    move.l  %a0,v_args+4(%a4)
    move.l  v_raw(%a4),v_args+8(%a4)
    move.l  v_kbd(%a4),v_args+12(%a4)
    move.l  v_hcount(%a4),v_args+16(%a4)
    lea     v_but(%a4),%a0
    move.l  %a0,v_args+20(%a4)
    lea     v_map(%a4),%a0
    move.l  %a0,v_args+24(%a4)
    lea     v_gtok(%a4),%a0
    move.l  %a0,v_args+28(%a4)
    lea     v_front(%a4),%a0
    move.l  %a0,v_args+32(%a4)
    move.l  v_rows(%a4),v_args+36(%a4)
    lea     fmt(%pc),%a0
    lea     v_args(%a4),%a1
    lea     putch(%pc),%a2
    lea     v_line(%a4),%a3
    jsr     LVO_RawDoFmt(%a6)
    lea     v_line(%a4),%a0
    bsr.s   say
    moveq   #0,%d0
    bra.s   98f
99: lea     failed(%pc),%a0
    bsr.s   say
    moveq   #20,%d0
98: movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

conread:                            /* a CMD_READ of one byte (a6 exec) */
    lea     conio(%pc),%a1
    move.w  #2,28(%a1)
    lea     cbuf(%pc),%a0
    move.l  %a0,40(%a1)
    moveq   #1,%d0
    move.l  %d0,36(%a1)
    jmp     LVO_SendIO(%a6)

append:                             /* d3's byte onto the 0-ended string at a0 (at most 15) */
    moveq   #14,%d0
1:  tst.b   (%a0)
    beq.s   2f
    addq.l  #1,%a0
    dbra    %d0,1b
    rts
2:  move.b  %d3,(%a0)+
    clr.b   (%a0)
    rts

handler:                            /* a0 the events, a1 the variables: the key events counted; the chain passed on */
    move.l  %a0,%d0
    move.l  %a0,%d1
1:  tst.l   %d1
    beq.s   2f
    move.l  %d1,%a0
    cmp.b   #1,4(%a0)               /* IECLASS_RAWKEY */
    bne.s   3f
    addq.l  #1,v_hcount(%a1)
3:  move.l  (%a0),%d1
    bra.s   1b
2:  rts

putch:
    move.b  %d0,(%a3)+
    rts

say:                                /* a0: a 0-ended line */
    movem.l %d2-%d3/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    move.l  vars+v_dos(%pc),%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d2-%d3/%a6
    rts

dosname: .asciz "dos.library"
intname: .asciz "intuition.library"
gtname:  .asciz "gadtools.library"
kmname:  .asciz "keymap.library"
gfxname: .asciz "graphics.library"
conname: .asciz "console.device"
inpname: .asciz "input.device"
kbdname: .asciz "keyboard.device"
ready:   .asciz "zinput: ready\n"
failed:  .asciz "zinput: a library, the window or a device could not be opened\n"
fmt:     .asciz "zinput: console %s vanilla %s raw %lx keyboard %lx handler %ld buttons %s map %s gadtools %s front %s rows %ld\n"
gtitle:  .asciz "Project"
gitem:   .asciz "Quit"
    .balign 2
newgad:  .word 10, 20, 80, 12          /* ng_LeftEdge, TopEdge, Width, Height */
         .long 0, 0                    /* ng_GadgetText, ng_TextAttr */
         .word 77                      /* ng_GadgetID */
         .long 0, 0, 0                 /* ng_Flags, ng_VisualInfo, ng_UserData */
newmenu: .byte 1, 0
         .long 0, 0                    /* nm_Label, nm_CommKey */
         .word 0
         .long 0, 0
         .byte 2, 0
         .long 0, 0
         .word 0
         .long 0, 0x1234
         .byte 0, 0
         .long 0, 0
         .word 0
         .long 0, 0
fakeev:  .long 0                       /* an InputEvent: RAWKEY 0x20 ('a') with Left Shift */
         .byte 1, 0
         .word 0x20, 0x0001
         .long 0, 0, 0
stags:   .long 0x80000023, 320, 0x80000024, 200, 0x80000025, 2, 0    /* SA_Width, SA_Height, SA_Depth */
wtags:   .long 0x80000066, 320         /* WA_Width */
         .long 0x80000067, 200         /* WA_Height */
         .long 0x8000006A, 0x00200408  /* WA_IDCMP: VANILLAKEY, RAWKEY, MOUSEBUTTONS */
         .long 0x8000008A, 1           /* WA_RMBTrap */
         .long 0x80000070              /* WA_CustomScreen: the screen */
wscr:    .long 0
         .long 0
    .balign 4
hint:    .long 0, 0                    /* the handler's Interrupt: ln_Succ, ln_Pred */
         .byte 2, 60                   /* NT_INTERRUPT, priority 60 (before intuition) */
         .long 0, 0, 0                 /* ln_Name, is_Data, is_Code */
conio:   .space 48
inpio:   .space 48
kbdio:   .space 48
kev:     .space 24
cbuf:    .space 4
vars:    .space 300
