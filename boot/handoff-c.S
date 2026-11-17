/* SPDX-License-Identifier: GPL-2.0-only */
/* The C boot programs' assembly helpers, shared by zenixboot (boot/zenixboot-start.S) and the demo floppy's stage 2
 * (boot/demoload-start.S, DESIGN D25 revision 3 section 6): one way to call any AmigaOS library function from C, the
 * serial copy of the messages, a stack of one's own, and the two steps of the hand-over of boot/handoff.S
 * (zb_tables, zb_go). Included with #include after boot/handoff.S; puts its code in .text and SysBase in .data. */

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

    /* The MMU off: the kernel starts with translation off (D41 builds its own tables later). 68040.library and
     * 68060.library (SetPatch) switch a 040/060's on with tables in fast RAM that the kernel would overwrite; on a
     * 68030 Enforcer, MMULib or CPU FASTROM do the same (audit 2026-11-12 A6-14: the kernel switched that off only
     * after copying itself and building its memory lists under the old tables). Probed with the illegal-instruction
     * and line-F vectors caught, because an EC part may not have the registers: on a 68040/060 the rest is then
     * skipped (it had no MMU to switch off), on a 68030 each step goes on by itself (below). A 68020 has none. */
    move.w  296(%a6),%d0
    and.w   #0x008C,%d0             /* AFF_68030, AFF_68040, AFF_68060 */
    beq.s   mmuskip
    movec   %vbr,%a1
    move.l  0x10(%a1),%d5           /* vector 4, illegal instruction */
    move.l  0x2C(%a1),%d6           /* vector 11, line F */
    lea     mmutrap(%pc),%a2
    move.l  %a2,0x10(%a1)
    move.l  %a2,0x2C(%a1)
    move.l  %sp,%a5
    and.w   #0x0088,%d0
    bne.s   mmu040
    /* 68030: each step resumes at its own label if it traps (a2), so a missing TC (a 68EC030: Motorola documents only
     * AC0/AC1 there) does not leave TT0/TT1 (its AC0/AC1) on; the transparent windows first, TC (E off, ATC flushed)
     * last. Then both caches cleared: the 030's caches hold logical addresses, and a tool that remapped pages (CPU
     * FASTROM) leaves lines that now name the wrong RAM (its data cache is write-through: nothing dirty is lost). */
    clr.l   -(%sp)
    move.l  %sp,%a5                 /* a trap comes back with the zero long still on the stack */
    lea     1f(%pc),%a2
    .long   0xF0170800              /* pmove (sp),tt0 */
1:  lea     2f(%pc),%a2
    .long   0xF0170C00              /* pmove (sp),tt1 */
2:  lea     3f(%pc),%a2
    .long   0xF0174000              /* pmove (sp),tc: translation off, the ATC flushed */
3:  addq.l  #4,%sp                 /* the zero long dropped (a5 is not needed after this) */
    movec   %cacr,%d0
    or.w    #0x0808,%d0             /* CD, CI: clear the data and instruction caches */
    movec   %d0,%cacr
    bra.s   mmuback
mmu040:
    lea     mmuback(%pc),%a2
    .word   0xF4F8                  /* cpusha bc: nothing dirty may be lost when the mapping goes */
    moveq   #0,%d0
    .long   0x4E7B0003              /* movec d0,tc: translation off (with E clear the ATC is not used: no PFLUSHA,
                                     * which an EC040 must not be given, MC68040UM B.1) */
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

mmutrap:                            /* one of the MMU instructions does not exist here: drop the frame, go on at a2 */
    move.l  %a5,%sp
    jmp     (%a2)

    handoff_subs

    .data
    .globl  SysBase
SysBase:
    .long   0
