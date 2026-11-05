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

#include "handoff-c.S"
