/* SPDX-License-Identifier: GPL-2.0-only */
/* zpkt: a cruncher's way of using the overlay mechanism, as Spaceballs' Peek does (docs/d29-zine.md 16.7), and dos
 * Execute. tools/mkhunktest.py writes it as an overlaid root with no overlay parts: its code hunk, HUNK_OVERLAY (a
 * table of two longwords: the data's length and its length - 1), HUNK_BREAK, then the data (section .ovldata). The
 * program talks to its overlay file's handler itself, with packets on the handle's fh_Type (dp_Arg1 its fh_Arg1),
 * waiting on a reply port of its own: ACTION_SEEK from the end back to the data, ACTION_READ of it, ACTION_FLUSH,
 * an action no handler knows (DOSFALSE, ERROR_ACTION_NOT_KNOWN), and ACTION_WRITE through Output()'s handle. Then
 * Execute("run >nil: setpatch") (DOSTRUE: not run, the output names it). Exit 0 when everything held; else the
 * return code names the step. With an argument starting with u it calls dos Rename("RAM:zpkt-gone", ...) first,
 * which ZINE lacks: the stop line names the call and the output its first argument. Position-independent code, one hunk. */
#include "zt.i"
#define LVO_FindTask    -294
#define LVO_SetSignal   -306
#define LVO_AllocSignal -330
#define LVO_FreeSignal  -336
#define LVO_GetMsg      -372
#define LVO_WaitPort    -384
#define LVO_Execute     -222
#define LVO_Rename      -78
#define ACTION_FLUSH    27
#define ACTION_READ     82
#define ACTION_WRITE    87
#define ACTION_SEEK     1008
#define OFFSET_END      1
#define DATALEN         16
/* the variables (at vars): */
#define V_dos   0
#define V_sig   4
#define V_buf   8               /* 16 bytes */
#define V_port  24              /* a MsgPort, 34 bytes */
#define V_msg   60              /* a StandardPacket: the Message (20 bytes), then the DosPacket (48) */
#define V_pkt   80
#define V_end   128

    .text
r0:
    bra.w   main                /* the overlay header LoadSeg fills in (the Guru Book's 19.3.2.2) */
    .long   0x0000ABCD
ovfh:   .long 0
ovtab:  .long 0
htab:   .long 0
gvec:   .long 0
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    move.b  (%a0),%d4           /* the argument's first letter */
    moveq   #-1,%d0
    move.l  %d0,V_sig(%a4)      /* no signal yet */
    move.l  4.w,%a6
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V_dos(%a4)
    beq     f20
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d7
    cmp.b   #0x75,%d4           /* "u": a dos call ZINE lacks, with a name in d1 */
    bne.s   1f
    lea     gone(%pc),%a0
    move.l  %a0,%d1
    move.l  %d1,%d2
    jsr     LVO_Rename(%a6)
    moveq   #30,%d0             /* not reached under ZINE: the call stops the program */
    bra     done
1:  move.l  4.w,%a6
    move.l  ovfh(%pc),%d0
    beq     f21
    move.l  ovtab(%pc),%a0      /* the table copy: the data's length first */
    cmp.l   #DATALEN,(%a0)
    bne     f21
    moveq   #-1,%d0             /* the reply port, built by hand as Peek does */
    jsr     LVO_AllocSignal(%a6)
    move.l  %d0,V_sig(%a4)
    bmi     f22
    lea     V_port(%a4),%a2
    move.b  #4,8(%a2)           /* NT_MSGPORT */
    clr.b   14(%a2)             /* PA_SIGNAL */
    move.b  %d0,15(%a2)
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,16(%a2)
    lea     20(%a2),%a0         /* the list: head -> &tail, tail 0, tailpred -> head */
    lea     4(%a0),%a1
    move.l  %a1,(%a0)
    clr.l   (%a1)
    move.l  %a0,8(%a0)
    lea     V_msg(%a4),%a0      /* the message's ln_Name is the packet, the packet's dp_Link the message */
    lea     V_pkt(%a4),%a1
    move.l  %a1,10(%a0)
    move.l  %a0,(%a1)
    move.l  ovfh(%pc),%d0       /* the overlay handle's handler port and its fh_Arg1 */
    lsl.l   #2,%d0
    move.l  %d0,%a0
    move.l  8(%a0),%d5
    beq     f23
    move.l  36(%a0),%d6
    move.l  #ACTION_SEEK,%d0    /* from the end back to the data: the old position is answered */
    move.l  %d6,%d1
    moveq   #-DATALEN,%d2
    moveq   #OFFSET_END,%d3
    bsr     packet
    tst.l   %d0
    ble     f24
    move.l  #ACTION_READ,%d0
    move.l  %d6,%d1
    lea     V_buf(%a4),%a0
    move.l  %a0,%d2
    moveq   #DATALEN,%d3
    bsr     packet
    cmp.l   #DATALEN,%d0
    bne     f25
    lea     V_buf(%a4),%a0
    lea     data(%pc),%a1
    moveq   #DATALEN/4-1,%d0
1:  cmpm.l  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne     f25
    moveq   #ACTION_FLUSH,%d0
    move.l  %d6,%d1
    bsr     packet
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne     f26
    move.l  #9999,%d0           /* an action no handler knows */
    move.l  %d6,%d1
    bsr     packet
    tst.l   %d0
    bne     f27
    cmp.l   #209,%d1            /* ERROR_ACTION_NOT_KNOWN */
    bne     f27
    lea     m_read(%pc),%a0
    bsr     puts
    move.l  %d7,%d0             /* Output()'s handle through its handler port */
    lsl.l   #2,%d0
    move.l  %d0,%a0
    move.l  8(%a0),%d5
    beq     f28
    moveq   #ACTION_WRITE,%d0
    move.l  36(%a0),%d1
    lea     m_wr(%pc),%a0
    move.l  %a0,%d2
    moveq   #m_wrend-m_wr,%d3
    bsr     packet
    cmp.l   #m_wrend-m_wr,%d0
    bne     f28
    move.l  V_dos(%a4),%a6      /* Execute: not run, answered DOSTRUE */
    lea     cmd(%pc),%a0
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    jsr     LVO_Execute(%a6)
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne     f29
    lea     m_exec(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra     done
f20: moveq  #20,%d0
    bra     done
f21: moveq  #21,%d0
    bra.s   fail
f22: moveq  #22,%d0
    bra.s   fail
f23: moveq  #23,%d0
    bra.s   fail
f24: moveq  #24,%d0
    bra.s   fail
f25: moveq  #25,%d0
    bra.s   fail
f26: moveq  #26,%d0
    bra.s   fail
f27: moveq  #27,%d0
    bra.s   fail
f28: moveq  #28,%d0
    bra.s   fail
f29: moveq  #29,%d0
fail:
    move.l  %d0,%d2
    lea     m_fail(%pc),%a0
    bsr     puts
    move.l  %d2,%d0
done:
    move.l  %d0,%d2
    move.l  4.w,%a6
    move.l  V_sig(%a4),%d0
    bmi.s   1f
    jsr     LVO_FreeSignal(%a6)
1:  move.l  V_dos(%a4),%d0
    beq.s   2f
    move.l  %d0,%a1
    jsr     LVO_CloseLibrary(%a6)
2:  move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* packet: d0 the action, d1-d3 dp_Arg1-3, d5 the handler's port: sent with PutMsg, waited for on the reply port
 * (WaitPort, GetMsg, the signal cleared, as Peek does). d0 dp_Res1, d1 dp_Res2; d0 0x7777 and d1 0 when the reply
 * did not come back as the handler should send it (the message, dp_Port the handler's port) */
packet:
    movem.l %a2/%a6,-(%sp)
    move.l  4.w,%a6
    lea     V_pkt(%a4),%a2
    move.l  %d0,8(%a2)
    move.l  %d1,20(%a2)
    move.l  %d2,24(%a2)
    move.l  %d3,28(%a2)
    lea     V_port(%a4),%a0
    move.l  %a0,4(%a2)          /* dp_Port: where the reply goes */
    move.l  %d5,%a0
    lea     V_msg(%a4),%a1
    jsr     LVO_PutMsg(%a6)
    lea     V_port(%a4),%a0
    jsr     LVO_WaitPort(%a6)
    lea     V_port(%a4),%a0
    jsr     LVO_GetMsg(%a6)
    lea     V_msg(%a4),%a0
    cmp.l   %a0,%d0
    bne.s   9f
    moveq   #0,%d0
    moveq   #1,%d1
    move.l  V_sig(%a4),%d2
    lsl.l   %d2,%d1
    jsr     LVO_SetSignal(%a6)
    cmp.l   4(%a2),%d5
    bne.s   9f
    move.l  12(%a2),%d0
    move.l  16(%a2),%d1
    movem.l (%sp)+,%a2/%a6
    rts
9:  move.l  #0x7777,%d0
    moveq   #0,%d1
    movem.l (%sp)+,%a2/%a6
    rts

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

dosname: .asciz "dos.library"
cmd:     .asciz "run >nil: setpatch"
gone:    .asciz "RAM:zpkt-gone"
m_read:  .asciz "zpkt: the data after the overlay table read with packets to the overlay handle's handler port\n"
m_wr:    .ascii "zpkt: written with a packet to Output()'s handler port\n"
m_wrend:
m_exec:  .asciz "zpkt: Execute answered done\n"
m_fail:  .asciz "zpkt: failed (the return code names the step)\n"
data:    .ascii "ZPKT-PACKED-DATA"
    .even
vars:   .space  V_end

/* the data after the overlay table (mkhunktest.py: HUNK_OVERLAY, HUNK_BREAK, then these bytes) */
    .section .ovldata,"a"
    .ascii  "ZPKT-PACKED-DATA"
