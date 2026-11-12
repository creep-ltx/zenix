/* SPDX-License-Identifier: GPL-2.0-only */
/* ztask (d29 16.16): more than one task. Child processes from dos CreateNewProc and a plain task from exec AddTask,
 * each check named when it fails; "ztask: all 9 parts right" and 0, else 1. The parts:
 *  A  a child of higher priority runs at once when made; 100 signals ping-ponged, each Signal switching at once
 *  B  10 messages to the child's pr_MsgPort and back by ReplyMsg (a child of lower priority: it runs while we wait)
 *  C  preemption from an interrupt: a vertical blank server signals a child of higher priority while this task
 *     loops in its own code without a call
 *  D  time slicing: a child of the same priority runs while this task loops without a call (the quantum)
 *  E  Forbid holds this task: a Signal to a higher child runs it only at Permit
 *  F  a semaphore held here: the child waits in ObtainSemaphore until ReleaseSemaphore hands it over
 *  G  AddTask and RemTask of another task; FindTask finds it, then not
 *  H  a child that calls dos Exit(7): its NP_ExitCode hook gets 7 and it is gone; the session goes on
 *  I  with an FPU: FP0 kept for each task across 20 switches (the FPU's state saved and restored)
 * Every child is gone at the end (FindTask). Position-independent code; a4 the data. */
#include "zt.i"
#define LVO_AddIntServer -168
#define LVO_RemIntServer -174
#define LVO_AddTask     -282
#define LVO_RemTask     -288
#define LVO_FindTask    -294
#define LVO_SetSignal   -306
#define LVO_Wait        -318
#define LVO_Signal      -324
#define LVO_AllocSignal -330
#define LVO_GetMsg      -372
#define LVO_ReplyMsg    -378
#define LVO_WaitPort    -384
#define LVO_InitSemaphore -558
#define LVO_ReleaseSemaphore -570
#define LVO_CreateMsgPort -666
#define LVO_DeleteMsgPort -672
#define LVO_CreateNewProc -498
#define LVO_Delay       -198
#define LVO_DExit       -144
#define NP_Entry        (0x80000000 + 1000 + 3)
#define NP_Name         (0x80000000 + 1000 + 12)
#define NP_Priority     (0x80000000 + 1000 + 13)
#define NP_ExitCode     (0x80000000 + 1000 + 24)
#define LIMIT           40000000
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
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,V(vmain)
    moveq   #-1,%d0
    jsr     LVO_AllocSignal(%a6)
    moveq   #0,%d1
    bset    %d0,%d1
    move.l  %d1,V(vmsig)

/* ---- A: priority 1, ping-pong ---- */
    lea     childA(%pc),%a0
    lea     nameA(%pc),%a1
    moveq   #1,%d0
    bsr.w   spawn
    move.l  %d0,V(vkid)
    bne.s   1f
    lea     nA0(%pc),%a0
    bsr.w   wrong
    bra.w   partB
1:  tst.l   V(vstarted)             /* it ran inside CreateNewProc, up to its Wait */
    bne.s   1f
    lea     nA1(%pc),%a0
    bsr.w   wrong
1:  moveq   #0,%d5                  /* the round */
    moveq   #0,%d6                  /* rounds where Signal did not switch at once */
2:  move.l  V(vcnt),%d4
    move.l  V(vkid),%a1
    move.l  V(vksig),%d0
    jsr     LVO_Signal(%a6)
    addq.l  #1,%d4
    cmp.l   V(vcnt),%d4
    beq.s   3f
    addq.l  #1,%d6
3:  move.l  V(vmsig),%d0
    jsr     LVO_Wait(%a6)
    addq.l  #1,%d5
    cmp.l   #100,%d5
    bne.s   2b
    tst.l   %d6
    beq.s   1f
    lea     nA2(%pc),%a0
    bsr.w   wrong
1:  cmp.l   #100,V(vcnt)
    beq.s   1f
    lea     nA3(%pc),%a0
    bsr.w   wrong
1:  lea     nameA(%pc),%a0
    bsr.w   gone

/* ---- B: priority -1, messages ---- */
partB:
    jsr     LVO_CreateMsgPort(%a6)
    move.l  %d0,V(vport)
    lea     childB(%pc),%a0
    lea     nameB(%pc),%a1
    moveq   #-1,%d0
    bsr.w   spawn
    move.l  %d0,V(vkid)
    beq.w   9f
    lea     V(vmsg),%a0
    move.l  V(vport),14(%a0)        /* mn_ReplyPort */
    move.w  #24,18(%a0)             /* mn_Length */
    moveq   #0,%d5
2:  lea     V(vmsg),%a1
    move.l  %d5,20(%a1)             /* the payload */
    move.l  V(vkid),%a0
    lea     92(%a0),%a0             /* pr_MsgPort */
    jsr     LVO_PutMsg(%a6)
    move.l  V(vport),%a0
    jsr     LVO_WaitPort(%a6)
    move.l  V(vport),%a0
    jsr     LVO_GetMsg(%a6)
    lea     V(vmsg),%a0
    cmp.l   %a0,%d0
    bne.s   8f
    move.l  %d5,%d1
    add.l   #1000,%d1
    cmp.l   20(%a0),%d1
    bne.s   8f
    addq.l  #1,%d5
    cmp.l   #10,%d5
    bne.s   2b
    bra.s   7f
8:  lea     nB1(%pc),%a0
    bsr.w   wrong
7:  move.l  V(vdos),%a6
    moveq   #3,%d1
    jsr     LVO_Delay(%a6)          /* the child returns while this task sleeps */
    move.l  4.w,%a6
    move.l  V(vport),%a0
    jsr     LVO_DeleteMsgPort(%a6)
    lea     nameB(%pc),%a0
    bsr.w   gone
    bra.s   partC
9:  lea     nB0(%pc),%a0
    bsr.w   wrong

/* ---- C: priority 5, woken by a vertical blank server while this task loops ---- */
partC:
    clr.l   V(vwakes)
    lea     childC(%pc),%a0
    lea     nameC(%pc),%a1
    moveq   #5,%d0
    bsr.w   spawn
    move.l  %d0,V(vkid)
    beq.w   9f
    lea     V(vint),%a1
    move.b  #2,8(%a1)               /* NT_INTERRUPT */
    lea     nameC(%pc),%a0
    move.l  %a0,10(%a1)
    lea     V(vkid),%a0
    move.l  %a0,14(%a1)             /* is_Data: the child and its signal */
    lea     server(%pc),%a0
    move.l  %a0,18(%a1)             /* is_Code */
    moveq   #5,%d0                  /* INTB_VERTB */
    jsr     LVO_AddIntServer(%a6)
    move.l  #LIMIT,%d3
2:  cmp.l   #5,V(vwakes)            /* this task's own code only: no call */
    beq.s   3f
    subq.l  #1,%d3
    bne.s   2b
3:  lea     V(vint),%a1
    moveq   #5,%d0
    jsr     LVO_RemIntServer(%a6)
    cmp.l   #5,V(vwakes)
    beq.s   1f
    lea     nC1(%pc),%a0
    bsr.w   wrong
1:  bsr.w   nap
    lea     nameC(%pc),%a0
    bsr.w   gone
    bra.s   partD
9:  lea     nC0(%pc),%a0
    bsr.w   wrong

/* ---- D: priority 0, the quantum ---- */
partD:
    clr.l   V(vspin)
    clr.l   V(vstop)
    lea     childD(%pc),%a0
    lea     nameD(%pc),%a1
    moveq   #0,%d0
    bsr.w   spawn
    tst.l   %d0
    beq.s   9f
    tst.l   V(vspin)                /* the same priority: it did not run at once */
    beq.s   1f
    lea     nD1(%pc),%a0
    bsr.w   wrong
1:  move.l  #LIMIT,%d3
2:  tst.l   V(vspin)
    bne.s   3f
    subq.l  #1,%d3
    bne.s   2b
    lea     nD2(%pc),%a0
    bsr.w   wrong
3:  move.l  #1,V(vstop)
    bsr.w   nap
    lea     nameD(%pc),%a0
    bsr.w   gone
    bra.s   partE
9:  lea     nD0(%pc),%a0
    bsr.w   wrong

/* ---- E: Forbid ---- */
partE:
    clr.l   V(vflag)
    clr.l   V(vstarted)
    lea     childE(%pc),%a0
    lea     nameE(%pc),%a1
    moveq   #5,%d0
    bsr.w   spawn
    move.l  %d0,V(vkid)
    beq.s   9f
    jsr     LVO_Forbid(%a6)
    move.l  V(vkid),%a1
    move.l  V(vksig),%d0
    jsr     LVO_Signal(%a6)
    move.l  V(vflag),%d4            /* still 0: held */
    jsr     LVO_Permit(%a6)
    tst.l   %d4
    beq.s   1f
    lea     nE1(%pc),%a0
    bsr.w   wrong
1:  tst.l   V(vflag)
    bne.s   1f
    lea     nE2(%pc),%a0
    bsr.w   wrong
1:  lea     nameE(%pc),%a0
    bsr.w   gone
    bra.s   partF
9:  lea     nE0(%pc),%a0
    bsr.w   wrong

/* ---- F: a semaphore ---- */
partF:
    clr.l   V(vflag)
    lea     V(vsem),%a0
    jsr     LVO_InitSemaphore(%a6)
    lea     V(vsem),%a0
    jsr     LVO_ObtainSemaphore(%a6)
    lea     childF(%pc),%a0
    lea     nameF(%pc),%a1
    moveq   #5,%d0
    bsr.w   spawn
    tst.l   %d0
    beq.s   9f
    move.l  V(vflag),%d4            /* 0: it waits for the semaphore */
    lea     V(vsem),%a0
    jsr     LVO_ReleaseSemaphore(%a6)
    tst.l   %d4
    beq.s   1f
    lea     nF1(%pc),%a0
    bsr.w   wrong
1:  cmp.l   #1,V(vflag)
    beq.s   1f
    lea     nF2(%pc),%a0
    bsr.w   wrong
1:  lea     nameF(%pc),%a0
    bsr.w   gone
    bra.s   partG
9:  lea     nF0(%pc),%a0
    bsr.w   wrong

/* ---- G: AddTask, RemTask ---- */
partG:
    lea     V(vtask),%a1
    move.b  #1,8(%a1)               /* NT_TASK */
    move.b  #-5,9(%a1)
    lea     nameG(%pc),%a0
    move.l  %a0,10(%a1)
    lea     V(vstack),%a0
    move.l  %a0,58(%a1)             /* tc_SPLower */
    lea     1024(%a0),%a0
    move.l  %a0,62(%a1)             /* tc_SPUpper */
    move.l  %a0,54(%a1)             /* tc_SPReg */
    lea     childG(%pc),%a2
    sub.l   %a3,%a3
    jsr     LVO_AddTask(%a6)
    lea     V(vtask),%a0
    cmp.l   %a0,%d0
    bne.s   9f
    bsr.w   nap                     /* it runs to its Wait */
    lea     nameG(%pc),%a1
    jsr     LVO_FindTask(%a6)
    lea     V(vtask),%a0
    cmp.l   %a0,%d0
    beq.s   1f
    lea     nG1(%pc),%a0
    bsr.w   wrong
1:  tst.l   V(vgran)
    bne.s   1f
    lea     nG2(%pc),%a0
    bsr.w   wrong
1:  lea     V(vtask),%a1
    jsr     LVO_RemTask(%a6)
    lea     nameG(%pc),%a0
    bsr.w   gone
    bra.s   partH
9:  lea     nG0(%pc),%a0
    bsr.w   wrong

/* ---- H: dos Exit in a child ---- */
partH:
    clr.l   V(vexit)
    lea     tags(%pc),%a0
    lea     V(vtags),%a1
    moveq   #9,%d0
1:  move.l  (%a0)+,(%a1)+
    dbra    %d0,1b
    lea     V(vtags),%a0
    lea     childH(%pc),%a1
    move.l  %a1,4(%a0)
    lea     nameH(%pc),%a1
    move.l  %a1,12(%a0)
    move.l  #1,20(%a0)
    lea     hook(%pc),%a1
    move.l  %a1,28(%a0)             /* NP_ExitCode */
    move.l  %a0,%d1
    move.l  V(vdos),%a6
    jsr     LVO_CreateNewProc(%a6)
    move.l  4.w,%a6
    tst.l   %d0
    beq.s   9f
    cmp.l   #7,V(vexit)
    beq.s   1f
    lea     nH1(%pc),%a0
    bsr.w   wrong
1:  lea     nameH(%pc),%a0
    bsr.w   gone
    bra.s   partI
9:  lea     nH0(%pc),%a0
    bsr.w   wrong

/* ---- I: the FPU's state, with an FPU ---- */
partI:
    move.w  296(%a6),%d0            /* AttnFlags: AFF_68881, AFF_68882, AFF_FPU40 */
    and.w   #0x70,%d0
    beq.w   done
    clr.l   V(vstarted)
    clr.l   V(vcnt)
    clr.l   V(vfbad)
    lea     childI(%pc),%a0
    lea     nameI(%pc),%a1
    moveq   #1,%d0
    bsr.w   spawn
    move.l  %d0,V(vkid)
    beq.s   9f
    .word   0xF23C,0x4000           /* fmove.l #1234,fp0 */
    .long   1234
    moveq   #0,%d5
2:  move.l  V(vkid),%a1
    move.l  V(vksig),%d0
    jsr     LVO_Signal(%a6)
    move.l  V(vmsig),%d0
    jsr     LVO_Wait(%a6)
    .word   0xF200,0x6000           /* fmove.l fp0,d0 */
    cmp.l   #1234,%d0
    beq.s   3f
    addq.l  #1,V(vfbad)
3:  addq.l  #1,%d5
    cmp.l   #10,%d5
    bne.s   2b
    tst.l   V(vfbad)
    beq.s   1f
    lea     nI1(%pc),%a0
    bsr.w   wrong
1:  lea     nameI(%pc),%a0
    bsr.w   gone
    lea     sayfpu(%pc),%a0
    bsr.w   say
    bra.s   done
9:  lea     nI0(%pc),%a0
    bsr.w   wrong

done:
    tst.l   %d7
    bne.s   5f
    lea     allright(%pc),%a0
    bsr.w   say
5:  move.l  V(vdos),%a1
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

/* spawn(a0 entry, a1 name, d0 priority) -> d0 the process (CreateNewProc); a6 exec again */
spawn:
    lea     V(vtags),%a5
    move.l  #NP_Entry,(%a5)
    move.l  %a0,4(%a5)
    move.l  #NP_Name,8(%a5)
    move.l  %a1,12(%a5)
    move.l  #NP_Priority,16(%a5)
    move.l  %d0,20(%a5)
    clr.l   24(%a5)
    move.l  %a5,%d1
    move.l  V(vdos),%a6
    jsr     LVO_CreateNewProc(%a6)
    move.l  4.w,%a6
    rts
/* nap: Delay(3), the others run meanwhile */
nap:
    move.l  V(vdos),%a6
    moveq   #3,%d1
    jsr     LVO_Delay(%a6)
    move.l  4.w,%a6
    rts
/* gone(a0 name): FindTask must not find it */
gone:
    move.l  %a0,-(%sp)
    move.l  %a0,%a1
    jsr     LVO_FindTask(%a6)
    move.l  (%sp)+,%a1
    tst.l   %d0
    beq.s   1f
    lea     ngone(%pc),%a0
    bsr.w   wrong
1:  rts

/* the children (a4 found again: each runs on its own) */
kidsig:                                 /* their signal: V(vksig), the task V(vkid) */
    lea     data(%pc),%a4
    move.l  4.w,%a6
    moveq   #-1,%d0
    jsr     LVO_AllocSignal(%a6)
    moveq   #0,%d1
    bset    %d0,%d1
    move.l  %d1,V(vksig)
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,V(vkid)
    move.l  #1,V(vstarted)
    rts
childA:
    bsr.s   kidsig
1:  move.l  V(vksig),%d0
    jsr     LVO_Wait(%a6)
    addq.l  #1,V(vcnt)
    move.l  V(vmain),%a1
    move.l  V(vmsig),%d0
    jsr     LVO_Signal(%a6)
    cmp.l   #100,V(vcnt)
    bne.s   1b
    moveq   #0,%d0
    rts
childB:
    lea     data(%pc),%a4
    move.l  4.w,%a6
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    move.l  %d0,%a2
    lea     92(%a2),%a2             /* its pr_MsgPort */
    moveq   #9,%d5
1:  move.l  %a2,%a0
    jsr     LVO_WaitPort(%a6)
    move.l  %a2,%a0
    jsr     LVO_GetMsg(%a6)
    move.l  %d0,%a1
    add.l   #1000,20(%a1)
    jsr     LVO_ReplyMsg(%a6)
    dbra    %d5,1b
    moveq   #0,%d0
    rts
childC:
    bsr.w   kidsig
1:  move.l  V(vksig),%d0
    jsr     LVO_Wait(%a6)
    addq.l  #1,V(vwakes)
    cmp.l   #5,V(vwakes)
    bne.s   1b
    moveq   #0,%d0
    rts
server:                                 /* a1: &vkid (the task, then its signal); a6 exec */
    move.l  4(%a1),%d0
    move.l  (%a1),%a1
    jsr     LVO_Signal(%a6)
    moveq   #0,%d0
    rts
childD:
    lea     data(%pc),%a4
1:  addq.l  #1,V(vspin)
    tst.l   V(vstop)
    beq.s   1b
    moveq   #0,%d0
    rts
childE:
    bsr.w   kidsig
    move.l  V(vksig),%d0
    jsr     LVO_Wait(%a6)
    move.l  #1,V(vflag)
    moveq   #0,%d0
    rts
childF:
    lea     data(%pc),%a4
    move.l  4.w,%a6
    lea     V(vsem),%a0
    jsr     LVO_ObtainSemaphore(%a6)
    addq.l  #1,V(vflag)
    lea     V(vsem),%a0
    jsr     LVO_ReleaseSemaphore(%a6)
    moveq   #0,%d0
    rts
childG:                                 /* a plain task: waits for ever (removed by RemTask) */
    lea     data(%pc),%a4
    move.l  4.w,%a6
    move.l  #1,V(vgran)
    move.l  #0x80000000,%d0
    jsr     LVO_Wait(%a6)
    rts
childH:
    lea     data(%pc),%a4
    move.l  V(vdos),%a6
    moveq   #7,%d1
    jsr     LVO_DExit(%a6)
    moveq   #99,%d0                 /* never */
    move.l  %d0,V(vexit)
    rts
hook:                                   /* NP_ExitCode: d0 the return code */
    lea     data(%pc),%a0
    move.l  %d0,vexit-data(%a0)
    rts
childI:
    bsr.w   kidsig
    .word   0xF23C,0x4000           /* fmove.l #5678,fp0 */
    .long   5678
1:  move.l  V(vksig),%d0
    jsr     LVO_Wait(%a6)
    .word   0xF200,0x6000           /* fmove.l fp0,d0 */
    cmp.l   #5678,%d0
    beq.s   2f
    addq.l  #1,V(vfbad)
2:  addq.l  #1,V(vcnt)
    move.l  V(vmain),%a1
    move.l  V(vmsig),%d0
    jsr     LVO_Signal(%a6)
    cmp.l   #10,V(vcnt)
    bne.s   1b
    moveq   #0,%d0
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
nameA:  .asciz "zta"
nameB:  .asciz "ztb"
nameC:  .asciz "ztc"
nameD:  .asciz "ztd"
nameE:  .asciz "zte"
nameF:  .asciz "ztf"
nameG:  .asciz "ztg"
nameH:  .asciz "zth"
nameI:  .asciz "zti"
allright: .asciz "ztask: all 9 parts right\n"
sayfpu: .asciz "ztask: FP0 kept for each task\n"
nA0:    .asciz "ztask: wrong: A: CreateNewProc gave no process\n"
nA1:    .asciz "ztask: wrong: A: the child of higher priority did not run at once\n"
nA2:    .asciz "ztask: wrong: A: a Signal to the child did not switch at once\n"
nA3:    .asciz "ztask: wrong: A: not 100 rounds\n"
nB0:    .asciz "ztask: wrong: B: CreateNewProc gave no process\n"
nB1:    .asciz "ztask: wrong: B: a message did not come back changed\n"
nC0:    .asciz "ztask: wrong: C: CreateNewProc gave no process\n"
nC1:    .asciz "ztask: wrong: C: the interrupt's signals did not preempt this task\n"
nD0:    .asciz "ztask: wrong: D: CreateNewProc gave no process\n"
nD1:    .asciz "ztask: wrong: D: a child of the same priority ran at once\n"
nD2:    .asciz "ztask: wrong: D: a child of the same priority never ran (no quantum)\n"
nE0:    .asciz "ztask: wrong: E: CreateNewProc gave no process\n"
nE1:    .asciz "ztask: wrong: E: the child ran inside Forbid\n"
nE2:    .asciz "ztask: wrong: E: the child did not run at Permit\n"
nF0:    .asciz "ztask: wrong: F: CreateNewProc gave no process\n"
nF1:    .asciz "ztask: wrong: F: the child got a semaphore this task held\n"
nF2:    .asciz "ztask: wrong: F: the child did not get the semaphore when it was released\n"
nG0:    .asciz "ztask: wrong: G: AddTask did not answer the task\n"
nG1:    .asciz "ztask: wrong: G: FindTask did not find the task\n"
nG2:    .asciz "ztask: wrong: G: the task did not run\n"
nH0:    .asciz "ztask: wrong: H: CreateNewProc gave no process\n"
nH1:    .asciz "ztask: wrong: H: the exit hook did not get Exit's code 7\n"
nI0:    .asciz "ztask: wrong: I: CreateNewProc gave no process\n"
nI1:    .asciz "ztask: wrong: I: FP0 not kept across the switches\n"
ngone:  .asciz "ztask: wrong: a finished task is still there\n"
    .balign 4
tags:   .long NP_Entry, 0, NP_Name, 0, NP_Priority, 0, NP_ExitCode, 0, 0, 0
data:
vdos:   .long 0
vmain:  .long 0
vmsig:  .long 0
vkid:   .long 0
vksig:  .long 0
vstarted: .long 0
vcnt:   .long 0
vport:  .long 0
vwakes: .long 0
vspin:  .long 0
vstop:  .long 0
vflag:  .long 0
vgran:  .long 0
vexit:  .long 0
vfbad:  .long 0
vtags:  .space 40
vmsg:   .space 24
vint:   .space 24
vsem:   .space 48
vtask:  .space 92
vstack: .space 1024
