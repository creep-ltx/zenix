/* SPDX-License-Identifier: GPL-2.0-only */
/* zenixboot, the assembly part: the program's entry, one way to call any AmigaOS library function from C, and the
 * two steps of the hand-over that are shared with the floppy bootblock (boot/handoff.S). The C part
 * (boot/zenixboot.c) loads the files and checks everything first; docs/man/zenixboot.txt says what it does.
 *
 * The program is linked like a Zenix program (link/user.ld, ZEXE header first, tools/mkzexe.py) and then wrapped into
 * an AmigaOS hunk executable by tools/mkhunk.py: AmigaOS starts it at offset 0, the bra.w below. */
#include "handoff.S"

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   zb_entry
    .ascii  "ZEXE"                  /* the ZEXE header mkzexe.py fills in (mkhunk.py reads it) */
    .long   0, 0, 0, 0, 0, 0

/* Entered by AmigaOS: a0 = arguments, d0 = their length (ReadArgs reads them itself), return code in d0. Everything
 * up to the call of zb_main is plain 68000 code, so that a 68000 or Kickstart 1.3 hears why it is refused. */
zb_entry:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    move.l  %a6,SysBase
    sub.l   %a1,%a1
    jsr     -294(%a6)               /* FindTask(0) */
    move.l  %d0,%a2
    tst.l   172(%a2)                /* pr_CLI: 0 = started from Workbench */
    bne.s   1f
    lea     92(%a2),%a0             /* pr_MsgPort: take the Workbench message and give it back (no Workbench use) */
    jsr     -384(%a6)               /* WaitPort */
    lea     92(%a2),%a0
    jsr     -372(%a6)               /* GetMsg */
    move.l  %d0,%a2
    jsr     -132(%a6)               /* Forbid (the reply ends the program: it must not be unloaded before we leave) */
    move.l  %a2,%a1
    jsr     -378(%a6)               /* ReplyMsg */
    moveq   #0,%d0
    bra.s   9f
1:  lea     msg_kick(%pc),%a2
    cmp.w   #37,20(%a6)             /* exec lib_Version: Kickstart 2.04 is V37 */
    blo.s   2f
    lea     msg_cpu(%pc),%a2
    move.w  296(%a6),%d0            /* AttnFlags: 68020, 68030, 68040 or 68060 */
    and.w   #0x008E,%d0
    beq.s   2f
    jsr     zb_main
    bra.s   9f
2:  jsr     -504(%a6)               /* RawIOInit: the serial port at 9600 baud, for the copy of the message there */
    move.l  %a2,-(%sp)
4:  move.l  (%sp),%a0
    move.b  (%a0)+,%d0
    move.l  %a0,(%sp)
    tst.b   %d0
    beq.s   5f
    jsr     -516(%a6)               /* RawPutChar */
    bra.s   4b
5:  addq.l  #4,%sp
    lea     dosname(%pc),%a1        /* say why, through the oldest dos.library calls (Output, Write) */
    moveq   #0,%d0
    jsr     -552(%a6)               /* OpenLibrary */
    tst.l   %d0
    beq.s   8f
    move.l  %d0,%a6
    jsr     -60(%a6)                /* Output */
    move.l  %d0,%d1
    beq.s   7f
    move.l  %a2,%d2
    moveq   #-1,%d3
3:  addq.l  #1,%d3
    tst.b   (%a2)+
    bne.s   3b
    jsr     -48(%a6)                /* Write(fh, msg, len) */
7:  move.l  %a6,%a1
    move.l  4.w,%a6
    jsr     -414(%a6)               /* CloseLibrary */
8:  moveq   #20,%d0
9:  movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

dosname:
    .asciz  "dos.library"
msg_kick:
    .asciz  "zenixboot: needs Kickstart 2.04 or newer (Zenix does not boot from Kickstart 1.3)\n"
msg_cpu:
    .asciz  "zenixboot: Zenix needs a 68020 or better; this Amiga has a 68000 or 68010\n"
    .balign 2

    .text
/* long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1): call the library function
 * at offset lvo of base with these registers; returns d0. Every AmigaOS call of the C part goes through here, so the
 * compiler never has to know about a6. */
    .globl  zb_os
zb_os:
    movem.l %d2-%d7/%a2-%a6,-(%sp)  /* 44 bytes; the arguments start at 48(%sp) */
    move.l  48(%sp),%a6
    move.l  52(%sp),%a5
    movem.l 56(%sp),%d0-%d3
    movem.l 72(%sp),%a0-%a1
    jsr     0(%a6,%a5.l)
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* long zb_dopkt(void *dosbase, void *port, long action): dos.library DoPkt (-240) with all five arguments 0 (d3-d7);
 * waits for the handler's reply. Used for ACTION_FLUSH (boot/zenixboot.c, D15). */
    .globl  zb_dopkt
zb_dopkt:
    movem.l %d2-%d7/%a2-%a6,-(%sp)  /* 44 bytes; the arguments start at 48(%sp) */
    move.l  48(%sp),%a6
    move.l  52(%sp),%d1
    move.l  56(%sp),%d2
    moveq   #0,%d3
    moveq   #0,%d4
    moveq   #0,%d5
    moveq   #0,%d6
    moveq   #0,%d7
    jsr     -240(%a6)
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* void zb_serial(const char *fmt, long *args): the same text on the serial port (exec's RawDoFmt and RawPutChar,
 * after RawIOInit in zb_main), so that a boot can be followed with a cable, or by a test in the emulator */
    .globl  zb_serial
zb_serial:
    movem.l %a2-%a3/%a6,-(%sp)
    move.l  4.w,%a6
    move.l  16(%sp),%a0
    move.l  20(%sp),%a1
    lea     zb_putch(%pc),%a2
    sub.l   %a3,%a3
    jsr     -522(%a6)               /* RawDoFmt */
    movem.l (%sp)+,%a2-%a3/%a6
    rts
zb_putch:                           /* d0 = the character (RawDoFmt ends with a 0: not sent) */
    tst.b   %d0
    beq.s   1f
    movem.l %d0-%d1/%a0-%a1/%a6,-(%sp)
    move.l  4.w,%a6
    jsr     -516(%a6)               /* RawPutChar */
    movem.l (%sp)+,%d0-%d1/%a0-%a1/%a6
1:  rts

/* long zb_onstack(long (*fn)(void *), void *arg, void *lower, unsigned long size): fn(arg) on a stack of its own
 * (exec StackSwap, V37), and back; returns fn's result. For the signature check (DESIGN D21 6.4, 6.6): its frames
 * are more than the boot shell's default stack can be trusted to hold. The StackSwapStruct lies on the old stack
 * and a4 points to it on both (StackSwap swaps its contents with the task's stack bounds, so the second call puts
 * the old ones back); size is a multiple of 4. */
    .globl  zb_onstack
zb_onstack:
    movem.l %d2-%d7/%a2-%a6,-(%sp)  /* 44 bytes; the arguments start at 48(%sp) */
    move.l  48(%sp),%a2             /* fn */
    move.l  52(%sp),%a3             /* arg */
    move.l  56(%sp),%d2             /* lower */
    move.l  60(%sp),%d3             /* size */
    lea     -12(%sp),%sp
    move.l  %sp,%a4
    move.l  %d2,(%a4)               /* stk_Lower */
    add.l   %d2,%d3
    move.l  %d3,4(%a4)              /* stk_Upper */
    move.l  %d3,8(%a4)              /* stk_Pointer: the new stack starts at its top */
    move.l  4.w,%a6
    move.l  %a4,%a0
    jsr     -732(%a6)               /* StackSwap: on the new stack from here */
    move.l  %a3,-(%sp)
    jsr     (%a2)
    addq.l  #4,%sp
    move.l  %d0,%d4
    move.l  4.w,%a6
    move.l  %a4,%a0
    jsr     -732(%a6)               /* StackSwap: the old stack again */
    move.l  %d4,%d0
    lea     12(%sp),%sp
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* unsigned long zb_tables(void *table, void *buffer, unsigned long ksize): AmigaOS still alive. Fills the bootinfo
 * and hwinfo table, runs stage0, picks the kernel's home and returns it (handoff_tables, as the bootblock does). */
    .globl  zb_tables
zb_tables:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    move.l  48(%sp),%a3
    move.l  52(%sp),%a4
    move.l  56(%sp),%d7
    handoff_tables
    move.l  %d4,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* void zb_go(void *table, void *buffer, unsigned long ksize, unsigned long home): never returns. AmigaOS off
 * (handoff_kill), then what a running AmigaOS may have switched on that a fresh boot from the floppy does not have,
 * then the copier and the jump (handoff_jump). */
    .globl  zb_go
zb_go:
    move.l  4.w,%a6
    move.l  4(%sp),%a3
    move.l  8(%sp),%a4
    move.l  12(%sp),%d7
    move.l  16(%sp),%d4
    handoff_kill                    /* leaves a0 = 0xDFF000 */

    /* every floppy motor off (df0-df3): the motor bit is latched by each drive when its select line goes low */
    lea     0xBFD100,%a1            /* CIA-B PRB: bit 7 MTR (low = on), bits 3-6 SEL0-SEL3 (low = selected) */
    or.b    #0xF8,(%a1)             /* MTR high (off), no drive selected */
    moveq   #3,%d1
    moveq   #0x08,%d2
1:  move.b  (%a1),%d0
    eor.b   %d2,%d0
    move.b  %d0,(%a1)               /* select the drive: it latches "motor off" */
    or.b    %d2,(%a1)               /* and deselect it */
    add.b   %d2,%d2
    dbf     %d1,1b

    /* the display registers a Workbench screen may have changed (the kernel's display expects reset values) */
    move.w  #0,0x1FC(%a0)           /* FMODE (AGA; nothing on OCS/ECS): 16-bit fetches, as after a reset */
    move.w  #0x0200,0x100(%a0)      /* BPLCON0: no bitplanes, colour on */
    move.w  #0,0x106(%a0)           /* BPLCON3: bank 0, no border blanking */

    /* 68040/68060: the MMU off. 68040.library/68060.library (SetPatch) switch it on with tables in fast RAM that the
     * kernel would overwrite; Zenix runs with no MMU. Probed with the illegal-instruction and line-F vectors caught,
     * because an EC/LC part may not have the registers: then the rest is skipped (it had no MMU to switch off). */
    move.w  296(%a6),%d0
    and.w   #0x0088,%d0
    beq.s   mmuskip
    movec   %vbr,%a1
    move.l  0x10(%a1),%d5           /* vector 4, illegal instruction */
    move.l  0x2C(%a1),%d6           /* vector 11, line F */
    lea     mmutrap(%pc),%a2
    move.l  %a2,0x10(%a1)
    move.l  %a2,0x2C(%a1)
    move.l  %sp,%a5
    .word   0xF4F8                  /* cpusha bc: nothing dirty may be lost when the mapping goes */
    moveq   #0,%d0
    .long   0x4E7B0003              /* movec d0,tc: translation off */
    .word   0xF518                  /* pflusha */
    .long   0x4E7B0004              /* movec d0,itt0 */
    .long   0x4E7B0005              /* movec d0,itt1 */
    .long   0x4E7B0006              /* movec d0,dtt0 */
    .long   0x4E7B0007              /* movec d0,dtt1 */
mmuback:
    move.l  %d5,0x10(%a1)
    move.l  %d6,0x2C(%a1)
mmuskip:
    lea     0xDFF000,%a0
    handoff_jump                    /* never returns */

mmutrap:                            /* one of the MMU instructions does not exist here: drop the frame, go on */
    move.l  %a5,%sp
    bra.s   mmuback

    handoff_subs

    .data
    .globl  SysBase
SysBase:
    .long   0
