/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob, the assembly part (DESIGN D29; blob.h): the header, the entry from the kernel, the CPU's vectors, the
 * way into the program and the ways out of it, and the exec functions that must keep every register (5.2).
 *
 * The blob is a ZEXE image (tools/mkzexe.py) that /bin/zine relocates into a block of its own; the kernel checks the
 * "ZINB" header at offset 32 (kernel/zine.h ZINB_OFF) and calls offset 0 in supervisor mode, interrupts masked, with
 * the struct zine_args on the stack. */
#define ZEXEC 0x0800
#define IDNEST (ZEXEC + 294)
#define TDNEST (ZEXEC + 295)

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   zb_entry
    .ascii  "ZEXE"                  /* filled in by tools/mkzexe.py */
    .long   0, 0, 0, 0, 0, 0
    .ascii  "ZINB"                  /* +32: kernel/zine.h ZINB_MAGIC, the layout, the file's CRC32 (tools/mkzblob.py) */
    .long   1
    .long   0

/* long entry(struct zine_args *): the plan first, on the kernel's stack (nothing is touched yet: a refusal returns
 * here and the kernel resumes as if nothing happened); then the ZINE block's own stack and the session */
zb_entry:
    move.l  4(%sp),-(%sp)
    jsr     zb_prepare
    addq.l  #4,%sp
    tst.l   %d0
    bne.s   1f
    move.l  zb_stack_top,%sp
    jsr     zb_run                  /* never returns */
1:  moveq   #0,%d1
    rts

    .text
/* ---- the CPU's vectors (8.2): one entry for every exception, one for the interrupts ---- */

/* every exception without its own meaning: the program crashed (9.1). The registers and the frame go to C. */
    .globl  zv_crash
zv_crash:
    move.w  #0x2700,%sr
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     zb_crash                /* never returns */

/* the spurious interrupt (vector 24) */
    .globl  zv_spurious
zv_spurious:
    rte

/* the autovectors 25-31: exec's dispatch (8.3), in C; d0/d1/a0/a1/a5/a6 saved (handlers may destroy them) */
    .globl  zv_irq
zv_irq:
    movem.l %d0-%d1/%a0-%a1/%a5-%a6,-(%sp)
    moveq   #0,%d0
    move.w  30(%sp),%d0             /* the format/vector word: 24 bytes of registers, SR, PC */
    and.w   #0x0FFF,%d0
    lsr.w   #2,%d0
    move.l  %d0,-(%sp)
    jsr     zb_irq
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d1/%a0-%a1/%a5-%a6
    rte

/* the privilege violation (vector 8): ZINE's own privileged steps taken from user mode (Supervisor, SetSR, the cache
 * flush, the way out); any other is a crash */
    .globl  zv_priv
zv_priv:
    cmp.l   #zs_sv,2(%sp)
    beq.s   1f
    cmp.l   #zs_catch,2(%sp)
    beq.w   zb_goback
    cmp.l   #zs_setsr,2(%sp)
    beq.s   3f
    cmp.l   #zs_flush,2(%sp)
    beq.s   4f
    bra.s   zv_crash
1:  move.l  %a0,-(%sp)              /* Supervisor: the frame's PC becomes the caller's return address (popped from */
    move.l  %usp,%a0                /* the user stack), and the function runs in supervisor mode; its rte returns */
    move.l  (%a0)+,6(%sp)           /* to the caller with the caller's SR. Every user register stays as it was. */
    move.l  %a0,%usp
    move.l  (%sp)+,%a0
    jmp     (%a5)
3:  move.l  %d0,%a0                 /* SetSR(new d0, mask d1) from user mode: the frame's SR is changed */
    moveq   #0,%d0
    move.w  (%sp),%d0
    move.l  %d0,-(%sp)              /* the old SR, the answer */
    move.l  %a0,%d0
    and.w   %d1,%d0
    not.w   %d1
    and.w   %d1,4(%sp)
    not.w   %d1
    or.w    %d0,4(%sp)
    move.l  (%sp)+,%d0
    move.l  #zs_rts,2(%sp)          /* back to the caller through an rts */
    rte
4:  move.l  %d0,-(%sp)              /* the cache flush */
    jsr     zb_flush
    move.l  (%sp)+,%d0
    move.l  #zs_rts,2(%sp)
    rte

/* ---- exec functions that keep every register (5.2) ---- */
    .globl  zx_Disable, zx_Enable, zx_Forbid, zx_Permit, zx_GetCC, zx_Supervisor, zx_SetSR, zx_CacheClearU
zx_Disable:
    move.w  #0x4000,0xDFF09A
    addq.b  #1,IDNEST
    rts
zx_Enable:
    subq.b  #1,IDNEST
    bge.s   1f
    move.w  #0xC000,0xDFF09A
1:  rts
zx_Forbid:
    addq.b  #1,TDNEST
    rts
zx_Permit:
    subq.b  #1,TDNEST
    rts
zx_GetCC:
    move.w  %ccr,-(%sp)
    moveq   #0,%d0
    move.w  (%sp)+,%d0
    rts
/* Supervisor(a5 = function): from user mode by the privilege violation (above); called in supervisor mode, the
 * frame is built here: SR, the return address, format 0 */
zx_Supervisor:
zs_sv:
    ori.w   #0,%sr                  /* privileged: traps in user mode, does nothing in supervisor mode */
    subq.l  #4,%sp
    move.l  4(%sp),2(%sp)
    move.w  #0,6(%sp)
    move.w  %sr,(%sp)
    jmp     (%a5)
/* SetSR(newSR d0, mask d1) -> the old SR */
zx_SetSR:
zs_setsr:
    ori.w   #0,%sr
    move.l  %d2,-(%sp)              /* supervisor mode: done here */
    move.w  %sr,%d2
    and.w   %d1,%d0
    not.w   %d1
    and.w   %d2,%d1
    or.w    %d1,%d0
    move.w  %d0,%sr
    moveq   #0,%d0
    move.w  %d2,%d0
    move.l  (%sp)+,%d2
    .globl  zs_rts, zx_chain, zs_catch
zs_rts:
zx_chain:                               /* the server chains' handler in IntVects (blob.c runs the chain itself) */
    rts
/* CacheClearU(), CacheClearE() */
zx_CacheClearU:
zs_flush:
    ori.w   #0,%sr
    move.l  %d0,-(%sp)
    jsr     zb_flush
    move.l  (%sp)+,%d0
    rts

/* ---- the ways out (9.1) ----
 * The program's return address (its rts), dos Exit, exec ColdReboot and every vector ZINE does not have reach
 * zs_catch with zr_how set: from user mode its privileged instruction traps (zv_priv sends it to zb_goback), in
 * supervisor mode it falls through. */
    .globl  zx_rts, zx_Exit, zx_ColdReboot, zx_unsup
zx_rts:
    move.l  %d0,zr_code
    move.l  #1,zr_how               /* ZH_RETURN */
    bra.s   zs_catch
zx_Exit:
    move.l  %d1,zr_code
    move.l  #2,zr_how               /* ZH_EXIT */
    bra.s   zs_catch
zx_ColdReboot:
    move.l  #3,zr_how               /* ZH_REBOOT */
    bra.s   zs_catch
/* a vector ZINE does not have: its slot is "jsr zx_unsup", so the return address is the slot + 6 */
zx_unsup:
    move.l  (%sp),zr_ret6
    move.l  4(%sp),zr_callpc
    move.l  #4,zr_how               /* ZH_UNSUP */
zs_catch:
    ori.w   #0,%sr
    .globl  zb_goback
zb_goback:
    move.w  #0x2700,%sr
    move.l  zb_stack_top,%sp
    jsr     zb_back                 /* never returns */

/* ---- helpers for C ---- */
    .globl  zb_setvbr, zb_flush, zb_go, zb_leave, zb_call, zb_putch
zb_setvbr:
    move.l  4(%sp),%d0
    movec   %d0,%vbr
    rts
/* the caches flushed after writing code (CLAUDE.md): CACR's clear bit on 020/030, cpusha bc on 040/060 and Emu68
 * (never cinva); the CPU from ExecBase's AttnFlags as AmigaOS reported them (bootinfo). Supervisor mode only. */
zb_flush:
    move.w  zb_attn+2,%d0          /* the CPU flags zb_prepare kept: the low page may be Zenix's again (review 2026-10-31) */
    and.w   #0x0088,%d0
    bne.s   1f
    movec   %cacr,%d0
    or.w    #0x0008,%d0
    movec   %d0,%cacr
    rts
1:  .word   0xF4F8                  /* cpusha bc */
    rts
/* zb_go(pc, usp, d0, a0, a6, a3): into the program in user mode, interrupts on (6); a3 the address of its first
 * segment's link longword (seglist << 2), as programs started from a Shell find it: some decrunchers' first
 * instructions walk the segment list from there */
zb_go:
    move.l  4(%sp),%a1
    move.l  8(%sp),%a2
    move.l  %a2,%usp
    move.l  12(%sp),%d0
    move.l  16(%sp),%a0
    move.l  20(%sp),%a6
    move.l  24(%sp),%a3
    move.l  zb_stack_top,%sp
    clr.w   -(%sp)                  /* format 0 */
    move.l  %a1,-(%sp)
    clr.w   -(%sp)                  /* SR: user mode, no interrupt mask */
    moveq   #0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    moveq   #0,%d4
    moveq   #0,%d5
    moveq   #0,%d6
    moveq   #0,%d7
    sub.l   %a1,%a1
    sub.l   %a2,%a2
    sub.l   %a4,%a4
    sub.l   %a5,%a5
    rte
/* zb_leave(resume, d0, d1): to the kernel's resume routine */
zb_leave:
    move.l  4(%sp),%a0
    move.l  8(%sp),%d0
    move.l  12(%sp),%d1
    jmp     (%a0)
/* zb_call(code, data, d1): an interrupt handler or server as exec calls it (8.3) -> 1 when it returned Z clear */
zb_call:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  48(%sp),%a5
    move.l  52(%sp),%a1
    move.l  56(%sp),%d1
    lea     0xDFF000,%a0
    move.l  #ZEXEC,%a6
    jsr     (%a5)
    sne     %d0
    and.l   #1,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
/* zb_putch(proc, &a3, c): RawDoFmt's PutChProc with d0 the character and a3 its data (which it may move on) */
zb_putch:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  48(%sp),%a0
    move.l  52(%sp),%a1
    move.l  (%a1),%a3
    move.l  56(%sp),%d0
    move.l  %a1,-(%sp)
    jsr     (%a0)
    move.l  (%sp)+,%a1
    move.l  %a3,(%a1)
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* ---- the register-to-C stubs of the other functions ----
 * CF name, cfunc, n, r1..r4: the arguments pushed as C wants them (r1 first), the C function called; d2-d7/a2-a6 are
 * kept by C as exec's functions must keep them, d0 is the answer. */
.macro CF name, fn, n, r1=%d0, r2=%d0, r3=%d0, r4=%d0
    .globl  \name
\name:
    .if \n >= 4
    move.l  \r4,-(%sp)
    .endif
    .if \n >= 3
    move.l  \r3,-(%sp)
    .endif
    .if \n >= 2
    move.l  \r2,-(%sp)
    .endif
    .if \n >= 1
    move.l  \r1,-(%sp)
    .endif
    jsr     \fn
    .if \n
    lea     4*\n(%sp),%sp
    .endif
    rts
.endm
    /* exec */
    CF  zs_Alert, c_Alert, 1, %d7
    CF  zs_SetIntVector, c_SetIntVector, 2, %d0, %a1
    CF  zs_AddIntServer, c_AddIntServer, 2, %d0, %a1
    CF  zs_RemIntServer, c_RemIntServer, 2, %d0, %a1
    CF  zs_AllocMem, zx_AllocMem, 2, %d0, %d1
    CF  zs_AllocAbs, c_AllocAbs, 2, %d0, %a1
    CF  zs_FreeMem, zx_FreeMem, 2, %a1, %d0
    CF  zs_AvailMem, c_AvailMem, 1, %d1
    CF  zs_Insert, c_Insert, 3, %a0, %a1, %a2
    CF  zs_AddHead, c_AddHead, 2, %a0, %a1
    CF  zs_AddTail, c_AddTail, 2, %a0, %a1
    CF  zs_Remove, c_Remove, 1, %a1
    CF  zs_RemHead, c_RemHead, 1, %a0
    CF  zs_RemTail, c_RemTail, 1, %a0
    CF  zs_Enqueue, c_Enqueue, 2, %a0, %a1
    CF  zs_FindName, c_FindName, 2, %a0, %a1
    CF  zs_FindTask, c_FindTask, 1, %a1
    CF  zs_SetTaskPri, c_SetTaskPri, 2, %a1, %d0
    CF  zs_SetSignal, c_SetSignal, 2, %d0, %d1
    CF  zs_Wait, c_Wait, 1, %d0
    CF  zs_Signal, c_Signal, 2, %a1, %d0
    CF  zs_AllocSignal, c_AllocSignal, 1, %d0
    CF  zs_FreeSignal, c_FreeSignal, 1, %d0
    CF  zs_OldOpenLibrary, c_OldOpenLibrary, 1, %a1
    CF  zs_OpenLibrary, c_OpenLibrary, 2, %a1, %d0
    CF  zs_TaggedOpenLibrary, c_TaggedOpenLibrary, 1, %d0
    CF  zs_CloseLibrary, c_CloseLibrary, 1, %a1
    CF  zs_OpenResource, c_OpenResource, 1, %a1
    CF  zs_OpenDevice, c_OpenDevice, 4, %a0, %d0, %a1, %d1
    CF  zs_CloseDevice, c_CloseDevice, 1, %a1
    CF  zs_SetFunction, c_SetFunction, 3, %a1, %a0, %d0
    CF  zs_RawDoFmt, c_RawDoFmt, 4, %a0, %a1, %a2, %a3
    CF  zs_TypeOfMem, c_TypeOfMem, 1, %a1
    CF  zs_CopyMem, c_CopyMem, 3, %a0, %a1, %d0
    CF  zs_CacheControl, c_CacheControl, 2, %d0, %d1
    CF  zs_AllocVec, c_AllocVec, 2, %d0, %d1
    CF  zs_FreeVec, c_FreeVec, 1, %a1
    CF  zs_CreatePool, c_CreatePool, 3, %d0, %d1, %d2
    CF  zs_DeletePool, c_DeletePool, 1, %a0
    CF  zs_AllocPooled, c_AllocPooled, 2, %a0, %d0
    CF  zs_FreePooled, c_FreePooled, 3, %a0, %a1, %d0
    /* graphics */
    CF  zs_LoadView, c_LoadView, 1, %a1
    CF  zs_WaitTOF, c_WaitTOF, 0
    CF  zs_WaitBlit, c_WaitBlit, 0
    CF  zs_OwnBlitter, c_OwnBlitter, 0
    CF  zs_DisownBlitter, c_DisownBlitter, 0
    CF  zs_VBeamPos, c_VBeamPos, 0
    CF  zs_SetRGB4, c_SetRGB4, 4, %d0, %d1, %d2, %d3
    CF  zs_LoadRGB4, c_LoadRGB4, 2, %a1, %d0
    CF  zs_InitBitMap, c_InitBitMap, 4, %a0, %d0, %d1, %d2
    CF  zs_AllocRaster, c_AllocRaster, 2, %d0, %d1
    CF  zs_FreeRaster, c_FreeRaster, 3, %a0, %d0, %d1
    CF  zs_FreeBitMap, c_FreeBitMap, 1, %a0
    CF  zs_InitRastPort, c_InitRastPort, 1, %a1
    CF  zs_LoadRGB32, c_LoadRGB32, 2, %a0, %a1
    CF  zs_OpenScreenTagList, c_OpenScreenTagList, 2, %a0, %a1
    CF  zs_OpenScreen, c_OpenScreen, 1, %a0
    CF  zs_CloseScreen, c_CloseScreen, 1, %a0
    CF  zs_ScreenToFront, c_ScreenToFront, 1, %a0
    CF  zs_GetMsg, c_GetMsg, 1, %a0
    CF  zs_Open, c_Open, 2, %d1, %d2
    CF  zs_Close, c_Close, 1, %d1
    CF  zs_Seek, c_Seek, 3, %d1, %d2, %d3
    CF  zs_Cause, c_Cause, 1, %a1
    CF  zs_AddICRVector, c_AddICRVector, 3, %a6, %d0, %a1
    CF  zs_RemICRVector, c_RemICRVector, 3, %a6, %d0, %a1
    CF  zs_AbleICR, c_AbleICR, 2, %a6, %d0
    CF  zs_SetICR, c_SetICR, 2, %a6, %d0
    CF  zs_DoIO, c_DoIO, 1, %a1
    CF  zs_SendIO, c_SendIO, 1, %a1
    CF  zs_CheckIO, c_CheckIO, 1, %a1
    CF  zs_WaitIO, c_WaitIO, 1, %a1
    CF  zs_AbortIO, c_AbortIO, 1, %a1
    CF  zs_TimerBeginIO, c_TimerBeginIO, 1, %a1
    CF  zs_TimerAbortIO, c_TimerAbortIO, 1, %a1
    CF  zs_AddTime, c_AddTime, 2, %a0, %a1
    CF  zs_SubTime, c_SubTime, 2, %a0, %a1
    CF  zs_CmpTime, c_CmpTime, 2, %a0, %a1
    CF  zs_ReadEClock, c_ReadEClock, 1, %a0
    CF  zs_GetSysTime, c_GetSysTime, 1, %a0
    CF  zs_PutMsg, c_PutMsg, 2, %a0, %a1
    CF  zs_ReplyMsg, c_ReplyMsg, 1, %a1
    CF  zs_WaitPort, c_WaitPort, 1, %a0
    CF  zs_CreateMsgPort, c_CreateMsgPort, 0
    CF  zs_DeleteMsgPort, c_DeleteMsgPort, 1, %a0
    CF  zs_OpenWindowTagList, c_OpenWindowTagList, 2, %a0, %a1
    CF  zs_OpenWindow, c_OpenWindow, 1, %a0
    CF  zs_CloseWindow, c_CloseWindow, 1, %a0
    CF  zs_ModifyIDCMP, c_ModifyIDCMP, 2, %a0, %d0
    CF  zs_SetPointer, c_SetPointer, 1, %a0
    CF  zs_ViewPortAddress, c_ViewPortAddress, 1, %a0
    .globl  zs_SetRGB32             /* a0 the ViewPort, d0 the pen, d1-d3 red, green, blue */
zs_SetRGB32:
    move.l  %d3,-(%sp)
    move.l  %d2,-(%sp)
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    move.l  %a0,-(%sp)
    jsr     c_SetRGB32
    lea     20(%sp),%sp
    rts
    .globl  zs_AllocBitMap          /* five arguments: d0 width, d1 height, d2 depth, d3 flags, a0 friend */
zs_AllocBitMap:
    move.l  %a0,-(%sp)
    move.l  %d3,-(%sp)
    move.l  %d2,-(%sp)
    move.l  %d1,-(%sp)
    move.l  %d0,-(%sp)
    jsr     c_AllocBitMap
    lea     20(%sp),%sp
    rts
    /* dos */
    CF  zs_Output, c_Output, 0
    CF  zs_Input, c_Input, 0
    CF  zs_Write, c_Write, 3, %d1, %d2, %d3
    CF  zs_Read, c_Read, 3, %d1, %d2, %d3
    CF  zs_PutStr, c_PutStr, 1, %d1
    CF  zs_VPrintf, c_VPrintf, 2, %d1, %d2
    CF  zs_VFPrintf, c_VFPrintf, 3, %d1, %d2, %d3
    CF  zs_FPuts, c_FPuts, 2, %d1, %d2
    CF  zs_FPutC, c_FPutC, 2, %d1, %d2
    CF  zs_Flush, c_Flush, 1, %d1
    CF  zs_IoErr, c_IoErr, 0
    CF  zs_SetIoErr, c_SetIoErr, 1, %d1
    CF  zs_Delay, c_Delay, 1, %d1
    /* intuition */
    CF  zs_CloseWorkBench, c_CloseWorkBench, 0
    CF  zs_OpenWorkBench, c_OpenWorkBench, 0

/* exec's StackSwap (V37, -732): a0 the StackSwapStruct {stk_Lower, stk_Upper, stk_Pointer}: the task's stack bounds
 * and the stack pointer exchanged with the struct's; the caller returns on the new stack (amigaofs.h: ThisTask 276,
 * tc_SPLower 58, tc_SPUpper 62). Every register but a1 and d1 kept (d0 too) */
    .globl  zs_StackSwap
zs_StackSwap:
    move.l  (%sp)+,%a1              /* the return address, from the old stack */
    move.l  %a2,-(%sp)
    move.l  ZEXEC+276,%a2           /* ThisTask */
    move.l  58(%a2),%d1
    move.l  (%a0),58(%a2)
    move.l  %d1,(%a0)
    move.l  62(%a2),%d1
    move.l  4(%a0),62(%a2)
    move.l  %d1,4(%a0)
    move.l  (%sp)+,%a2
    move.l  %sp,%d1
    move.l  8(%a0),%sp
    move.l  %d1,8(%a0)
    jmp     (%a1)

/* zb_jumptable(base, slots, lvo[], fn[], n): every slot "jsr zx_unsup", then each of the n functions "jmp fn" at
 * base - lvo */
    .globl  zb_jumptable
zb_jumptable:
    movem.l %d2-%d3/%a2-%a3,-(%sp)
    move.l  20(%sp),%a0             /* base */
    move.l  24(%sp),%d0             /* slots */
    move.l  %a0,%a1
    bra.s   2f
1:  move.l  #zx_unsup,-(%a1)
    move.w  #0x4EB9,-(%a1)          /* jsr abs.l */
2:  dbra    %d0,1b
    move.l  28(%sp),%a2             /* lvo[] (words) */
    move.l  32(%sp),%a3             /* fn[] */
    move.l  36(%sp),%d0
    bra.s   4f
3:  moveq   #0,%d1
    move.w  (%a2)+,%d1
    move.l  %a0,%a1
    sub.l   %d1,%a1
#ifdef ZINE_TRACE
    move.l  ztr_n,%d1               /* the trace build: the slot calls zx_trace, which names it and goes on */
    lsl.l   #3,%d1
    lea     ztr_tab,%a3
    add.l   %d1,%a3
    move.l  %a1,(%a3)+              /* the slot */
    move.l  32(%sp),%d1
    move.l  36(%sp),%d2
    sub.l   %d0,%d2                 /* this function's index */
    subq.l  #1,%d2
    lsl.l   #2,%d2
    move.l  %d1,%a3
    move.l  0(%a3,%d2.l),%d1
    lea     ztr_tab,%a3
    move.l  ztr_n,%d2
    lsl.l   #3,%d2
    move.l  %d1,4(%a3,%d2.l)        /* its function */
    addq.l  #1,ztr_n
    move.w  #0x4EB9,(%a1)+          /* jsr abs.l */
    move.l  #zx_trace,(%a1)
#else
    move.w  #0x4EF9,(%a1)+          /* jmp abs.l */
    move.l  (%a3)+,(%a1)
#endif
4:  dbra    %d0,3b
    movem.l (%sp)+,%d2-%d3/%a2-%a3
    rts

    .data
    .globl  zb_attn                 /* AttnFlags (a long, the low word used), set by zb_prepare */
zb_attn:
    .long   0
    .globl  zb_tracing              /* 1 in the trace build: zb_back also writes the code before a bad call */
zb_tracing:
#ifdef ZINE_TRACE
    .long   1
#else
    .long   0
#endif
    .text
#ifdef ZINE_TRACE
/* the trace build (tests only, /tests/zine/trace.blob): every supported library call is written to the serial port
 * as "Z" and the slot's address in hex, then the function runs with every register as the caller left it */
    .bss
    .even
ztr_n:   .space 4
ztr_tab: .space 8 * 256
    .text
zx_trace:
    movem.l %d0-%d3/%a0-%a1,-(%sp)
    move.l  24(%sp),%d1             /* our return address: the slot + 6 */
    subq.l  #6,%d1
    move.l  %d1,%a1                 /* the slot */
    moveq   #'Z',%d0
    bsr.s   ztr_put
    moveq   #7,%d2
1:  rol.l   #4,%d1
    move.l  %d1,%d0
    and.w   #15,%d0
    move.b  ztr_hex(%pc,%d0.w),%d0
    bsr.s   ztr_put
    dbra    %d2,1b
    moveq   #10,%d0
    bsr.s   ztr_put
    lea     ztr_tab,%a0             /* find the slot's function */
    move.l  ztr_n,%d2
2:  subq.l  #1,%d2
    bmi.s   3f
    cmp.l   (%a0),%a1
    beq.s   4f
    addq.l  #8,%a0
    bra.s   2b
3:  move.l  #zx_unsup,%a0
    bra.s   5f
4:  move.l  4(%a0),%a0
5:  move.l  %a0,24(%sp)             /* our return address becomes the function: rts below jumps there */
    movem.l (%sp)+,%d0-%d3/%a0-%a1
    rts
ztr_put:                            /* d0: a byte to the serial port, waiting for the transmit buffer (d3 used) */
    move.w  0xdff018,%d3
    btst    #13,%d3
    beq.s   ztr_put
    and.w   #0xff,%d0
    or.w    #0x100,%d0
    move.w  %d0,0xdff030
    rts
ztr_hex: .ascii "0123456789abcdef"
    .even
#endif
