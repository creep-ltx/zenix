/* SPDX-License-Identifier: GPL-2.0-only */
/* zsound (d29 16.16): a process made with dos CreateNewProc (priority 10, as a demo's sound process) plays a sound
 * through audio.device while the first task draws (a colour each frame, WaitTOF). The child opens audio.device for
 * channel 0, then keeps two CMD_WRITEs queued (ADIOF_PERVOL, period 400, volume 64, 2,000 bytes of a square wave in
 * chip RAM, one cycle each): eight writes, each requeued when WaitIO says it has played, the other still in progress
 * then (double buffering without a gap). Checked: the writes come back in order with no error, the other one still
 * playing each time; the eight took about their sound's length (8 x 2,000 x 400 x 0.2794 us = 1.79 s, as 50 Hz ticks
 * of DateStamp: 75-105); the channel's DMA was seen on (DMACONR bit 0) from the drawing task and is off after
 * CloseDevice; the drawing task drew meanwhile. "zsound: played and drew" and 0, else 1. Position-independent; a4
 * the data. */
#include "zt.i"
#define LVO_FindTask    -294
#define LVO_Wait        -318
#define LVO_Signal      -324
#define LVO_AllocSignal -330
#define LVO_OpenDevice  -444
#define LVO_CloseDevice -450
#define LVO_CheckIO     -468
#define LVO_WaitIO      -474
#define LVO_CreateMsgPort -666
#define LVO_DeleteMsgPort -672
#define LVO_CreateNewProc -498
#define LVO_DateStamp   -192
#define LVO_Delay       -198
#define NP_Entry        (0x80000000 + 1000 + 3)
#define NP_Name         (0x80000000 + 1000 + 12)
#define NP_Priority     (0x80000000 + 1000 + 13)
#define NP_StackSize    (0x80000000 + 1000 + 11)
#define BYTES           2000
#define V(x)            (x - data)(%a4)
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     data(%pc),%a4
    move.l  4.w,%a6
    moveq   #0,%d7
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V(vdos)
    beq.w   99f
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V(vgfx)
    beq.w   99f
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,V(vmain)
    moveq   #-1,%d0
    jsr     LVO_AllocSignal(%a6)
    moveq   #0,%d1
    bset    %d0,%d1
    move.l  %d1,V(vmsig)
    lea     V(vtags),%a0
    move.l  #NP_Entry,(%a0)+
    lea     child(%pc),%a1
    move.l  %a1,(%a0)+
    move.l  #NP_Name,(%a0)+
    lea     cname(%pc),%a1
    move.l  %a1,(%a0)+
    move.l  #NP_Priority,(%a0)+
    move.l  #10,(%a0)+
    move.l  #NP_StackSize,(%a0)+
    move.l  #8000,(%a0)+
    clr.l   (%a0)
    lea     V(vtags),%a0
    move.l  %a0,%d1
    move.l  V(vdos),%a6
    jsr     LVO_CreateNewProc(%a6)
    move.l  4.w,%a6
    tst.l   %d0
    bne.s   1f
    lea     n0(%pc),%a0
    bsr.w   wrong
    bra.w   done
/* the first task draws until the child says it is done: COLOR00 each frame; Paula's channel 0 watched */
1:  moveq   #0,%d5                  /* frames drawn */
    moveq   #0,%d6                  /* frames with channel 0's DMA on */
2:  move.l  V(vgfx),%a6
    jsr     LVO_WaitTOF(%a6)
    move.w  %d5,0xDFF180
    addq.l  #1,%d5
    move.w  0xDFF002,%d0            /* DMACONR */
    btst    #0,%d0
    beq.s   3f
    addq.l  #1,%d6
3:  tst.l   V(vdone)
    bne.s   4f
    cmp.l   #1500,%d5               /* 30 s: the child never finished */
    bne.s   2b
    lea     n1(%pc),%a0
    bsr.w   wrong
    bra.w   done
4:  move.w  #0,0xDFF180
    move.l  4.w,%a6
    cmp.l   #20,%d5
    bcc.s   1f
    lea     n2(%pc),%a0
    bsr.w   wrong
1:  cmp.l   #20,%d6
    bcc.s   1f
    lea     n3(%pc),%a0
    bsr.w   wrong
1:  move.w  0xDFF002,%d0
    btst    #0,%d0
    beq.s   1f
    lea     n4(%pc),%a0
    bsr.w   wrong
1:  tst.l   V(vbad)
    beq.s   1f
    move.l  V(vbad),%a0
    bsr.w   wrong
1:  move.l  V(vticks),%d0
    cmp.l   #75,%d0
    bcs.s   8f
    cmp.l   #106,%d0
    bcs.s   1f
8:  lea     n5(%pc),%a0
    bsr.w   wrong
1:  move.l  V(vdos),%a6
    moveq   #3,%d1
    jsr     LVO_Delay(%a6)          /* the child ends meanwhile */
    move.l  4.w,%a6
    lea     cname(%pc),%a1
    jsr     LVO_FindTask(%a6)
    tst.l   %d0
    beq.s   done
    lea     n6(%pc),%a0
    bsr.w   wrong
done:
    tst.l   %d7
    bne.s   5f
    lea     allright(%pc),%a0
    bsr.w   say
5:  move.l  4.w,%a6
    move.l  V(vgfx),%a1
    jsr     LVO_CloseLibrary(%a6)
    move.l  V(vdos),%a1
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    tst.l   %d7
    beq.s   6f
    moveq   #1,%d0
6:  movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
99: moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* the sound process: a2, a3 the two requests (IOAudio, 68 bytes) */
child:
    lea     data(%pc),%a4
    move.l  4.w,%a6
    jsr     LVO_CreateMsgPort(%a6)
    move.l  %d0,V(vport)
    beq.w   90f
    move.l  #BYTES,%d0
    moveq   #MEMF_CHIP,%d1
    jsr     LVO_AllocMem(%a6)
    move.l  %d0,V(vwave)
    beq.w   90f
    move.l  %d0,%a0                 /* a square wave: 20 bytes +100, 20 bytes -100 */
    move.l  #BYTES/40-1,%d1
1:  moveq   #19,%d2
2:  move.b  #100,(%a0)+
    dbra    %d2,2b
    moveq   #19,%d2
3:  move.b  #-100,(%a0)+
    dbra    %d2,3b
    dbra    %d1,1b
    lea     V(vio1),%a2
    lea     V(vio2),%a3
    move.l  V(vport),14(%a2)        /* mn_ReplyPort */
    move.b  #127,9(%a2)             /* ln_Pri: the allocation's precedence */
    lea     masks(%pc),%a0
    move.l  %a0,34(%a2)             /* ioa_Data: the masks wanted */
    move.l  #1,38(%a2)              /* ioa_Length: their count */
    lea     audname(%pc),%a0
    moveq   #0,%d0
    move.l  %a2,%a1
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    beq.s   1f
    lea     n7(%pc),%a0
    move.l  %a0,V(vbad)
    bra.w   90f
1:  move.w  #3,28(%a2)              /* CMD_WRITE */
    move.b  #0x10,30(%a2)           /* ADIOF_PERVOL */
    move.l  V(vwave),34(%a2)
    move.l  #BYTES,38(%a2)
    move.w  #400,42(%a2)
    move.w  #64,44(%a2)
    move.w  #1,46(%a2)
    moveq   #16,%d0                 /* the second one: a copy (68 bytes) */
    move.l  %a2,%a0
    move.l  %a3,%a1
2:  move.l  (%a0)+,(%a1)+
    dbra    %d0,2b
    lea     V(vds),%a0
    move.l  %a0,%d1
    move.l  V(vdos),%a6
    jsr     LVO_DateStamp(%a6)
    move.l  4.w,%a6
    move.l  %a2,%a1
    bsr.w   begin
    move.l  %a3,%a1
    bsr.w   begin
    moveq   #8,%d5                  /* writes still to wait for; the next one to requeue: 6 more */
    moveq   #6,%d6
3:  move.l  %a2,%a1                 /* the older one */
    jsr     LVO_WaitIO(%a6)
    tst.l   %d0
    beq.s   4f
    lea     n8(%pc),%a0
    move.l  %a0,V(vbad)
4:  subq.l  #1,%d5
    beq.s   5f
    move.l  %a3,%a1                 /* the other: still playing (double buffering) */
    jsr     LVO_CheckIO(%a6)
    tst.l   %d0
    beq.s   4f
    lea     n9(%pc),%a0
    move.l  %a0,V(vbad)
4:  tst.l   %d6
    beq.s   4f
    subq.l  #1,%d6
    move.l  %a2,%a1
    bsr.w   begin
4:  exg     %a2,%a3
    bra.s   3b
5:  lea     V(vds2),%a0
    move.l  %a0,%d1
    move.l  V(vdos),%a6
    jsr     LVO_DateStamp(%a6)
    move.l  4.w,%a6
    lea     V(vds),%a0              /* the ticks taken: minutes x 3000 + ticks, both stamps */
    move.l  V(vds2+4),%d0
    sub.l   4(%a0),%d0
    mulu.l  #3000,%d0
    add.l   V(vds2+8),%d0
    sub.l   8(%a0),%d0
    move.l  %d0,V(vticks)
    move.l  %a2,%a1
    jsr     LVO_CloseDevice(%a6)
    move.l  V(vwave),%a1
    move.l  #BYTES,%d0
    jsr     LVO_FreeMem(%a6)
90: move.l  V(vport),%d0
    beq.s   91f
    move.l  %d0,%a0
    jsr     LVO_DeleteMsgPort(%a6)
91: move.l  #1,V(vdone)
    move.l  V(vmain),%a1
    move.l  V(vmsig),%d0
    jsr     LVO_Signal(%a6)
    moveq   #0,%d0
    rts
begin:                                  /* a1 the request: the device's BeginIO (-30), flags kept */
    move.l  %a6,-(%sp)
    move.l  20(%a1),%a6
    jsr     -30(%a6)
    move.l  (%sp)+,%a6
    rts

wrong:                              /* a0: the line */
    addq.l  #1,%d7
say:                                /* a0: a 0-ended line */
    movem.l %d0-%d3/%a0-%a1/%a6,-(%sp)
    move.l  V(vdos),%a6
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   (%a0)+
    bne.s   1b
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3/%a0-%a1/%a6
    rts
dosname: .asciz "dos.library"
gfxname: .asciz "graphics.library"
audname: .asciz "audio.device"
cname:  .asciz "zsnd"
masks:  .byte 1
allright: .asciz "zsound: played and drew\n"
n0:     .asciz "zsound: wrong: CreateNewProc gave no process\n"
n1:     .asciz "zsound: wrong: the sound process never finished\n"
n2:     .asciz "zsound: wrong: the first task did not draw while the sound played\n"
n3:     .asciz "zsound: wrong: channel 0's DMA was not seen on\n"
n4:     .asciz "zsound: wrong: channel 0's DMA still on after CloseDevice\n"
n5:     .asciz "zsound: wrong: the eight writes did not take about their sound's length\n"
n6:     .asciz "zsound: wrong: the sound process is still there\n"
n7:     .asciz "zsound: wrong: audio.device did not open with channel 0\n"
n8:     .asciz "zsound: wrong: a write came back with an error\n"
n9:     .asciz "zsound: wrong: the other write was not still playing\n"
    .balign 4
data:
vdos:   .long 0
vgfx:   .long 0
vmain:  .long 0
vmsig:  .long 0
vport:  .long 0
vwave:  .long 0
vdone:  .long 0
vbad:   .long 0
vticks: .long 0
vds:    .space 12
vds2:   .space 12
vtags:  .space 40
vio1:   .space 68
vio2:   .space 68
