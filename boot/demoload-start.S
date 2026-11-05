/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's stage 2, the assembly part (DESIGN D25 revision 3, section 6): the entry the bootblock
 * (boot/demoboot.S) jumps to, and the shared helpers of the C boot programs (boot/handoff-c.S: the library calls,
 * the hand-over). Linked like a Zenix program (link/user.ld, a ZEXE: the bootblock relocates it); the C part is
 * boot/demoload.c, the screen boot/demoscreen.c.
 *
 * Entered with a1 = the boot drive's trackdisk request, a6 = ExecBase, d7 = the stage 2's size on the disk (so the
 * table is at byte 1024 + d7). Up to the call of dl_main everything is 68000 code (and the screen's, built for the
 * 68000), so that a 68000 Amiga or Kickstart 1.3 is told why it cannot start this disk. Never returns to Kickstart:
 * when something fails, the reason stays on the screen. */
#include "handoff.S"

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   dl_entry
    .ascii  "ZEXE"                  /* the ZEXE header mkzexe.py fills in */
    .long   0, 0, 0, 0, 0, 0

dl_entry:
    move.l  %a6,SysBase
    lea     msg_kick(%pc),%a2
    cmp.w   #37,20(%a6)             /* exec lib_Version: Kickstart 2.04 is V37 */
    blo.s   1f
    lea     msg_cpu(%pc),%a2
    move.w  296(%a6),%d0            /* AttnFlags: 68020, 68030, 68040 or 68060 */
    and.w   #0x008E,%d0
    beq.s   1f
    move.l  %d7,%d0
    add.l   #1024,%d0
    move.l  %d0,-(%sp)              /* the table's place on the disk */
    move.l  %a1,-(%sp)              /* the request */
    jsr     dl_main                 /* returns only when it failed and said why */
    bra.s   9f
1:  move.w  #9,28(%a1)              /* TD_MOTOR, io_Length 0: the drive's motor off */
    clr.l   36(%a1)
    jsr     -456(%a6)               /* DoIO */
    clr.l   -(%sp)                  /* lines: the reason, then the end */
    move.l  %a2,-(%sp)
    move.l  %sp,-(%sp)
    pea     1.w                     /* failed */
    jsr     demo_screen
9:  move.l  SysBase,%a6
    moveq   #0,%d0
    jsr     -318(%a6)               /* Wait(0): the boot task sleeps for good; the screen says why */
    bra.s   9b

msg_kick:
    .asciz  "This disk needs Kickstart 2.04 or newer and a 68020 or better."
msg_cpu:
    .asciz  "This disk needs a 68020 or better (this Amiga has a 68000 or 68010)."
    .balign 2

#include "handoff-c.S"
