/* SPDX-License-Identifier: GPL-2.0-only */
/* bootmenu, the assembly part: the program's entry, one way to call any AmigaOS library function from C, and the
 * "boot this partition next" helper (kt_blob) that the C part copies into chip RAM before a reboot.
 * docs/man/bootmenu.txt says what the program does; boot/bootmenu.c is the rest.
 *
 * Linked like a Zenix program (link/user.ld, ZEXE header first, tools/mkzexe.py) and wrapped into an AmigaOS hunk
 * executable by tools/mkhunk.py, as zenixboot is: AmigaOS starts it at offset 0, the bra.w below. */

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   bm_entry
    .ascii  "ZEXE"                  /* the ZEXE header mkzexe.py fills in (mkhunk.py reads it) */
    .long   0, 0, 0, 0, 0, 0

/* Entered by AmigaOS from a Shell (the boot partition's S:Startup-Sequence): return code in d0. Kickstart 2.04 or
 * newer (V37) is needed for the calls the C part uses; older ones get return code 20 and nothing else. */
bm_entry:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    move.l  %a6,SysBase
    moveq   #20,%d0
    cmp.w   #37,20(%a6)             /* exec lib_Version */
    blo.s   9f
    jsr     bm_main
9:  movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

    .text
/* long bm_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1): call the library function
 * at offset lvo of base with these registers; returns d0. */
    .globl  bm_os
bm_os:
    movem.l %d2-%d3/%a2/%a6,-(%sp)
    move.l  20(%sp),%a6
    move.l  24(%sp),%a2
    movem.l 28(%sp),%d0-%d3/%a0-%a1
    jsr     0(%a6,%a2.l)
    movem.l (%sp)+,%d2-%d3/%a2/%a6
    rts

/* ---- kt_blob: boot one chosen partition at the next reset ----
 * The C part copies kt_blob..kt_blob_end into chip RAM, fills in the addresses marked "C:", puts the block on
 * ExecBase's KickMemPtr / KickTagPtr lists (ROM Kernel Manual, exec.library/SumKickData) and reboots. At the reset,
 * exec keeps the block (KickMemPtr) and runs the resident below (KickTagPtr) with the cold-start modules, after the
 * disk drivers have listed their bootable partitions on expansion.library's eb_MountList and before the strap module
 * boots the first one. The resident puts the old KickMemPtr/KickTagPtr back first (so it runs once, and a tool that
 * was there before stays), then moves the chosen partition's BootNode to priority 127, the head of the list.
 * Position independent: it uses only pc-relative addresses and the addresses the C part filled in. */
    .globl  kt_blob, kt_blob_end, kt_memlist, kt_tags, kt_res, kt_name, kt_code, kt_target, kt_oldmem, kt_oldtag
    .balign 4
kt_blob:
kt_memlist:                         /* struct MemList: Node, ml_NumEntries, one MemEntry */
    .long   0, 0                    /* ln_Succ, ln_Pred (C: ln_Succ = the old KickMemPtr) */
    .byte   0, 0                    /* ln_Type, ln_Pri */
    .long   0                       /* ln_Name */
    .word   1                       /* ml_NumEntries */
    .long   0                       /* C: me_Addr = the block */
    .long   0                       /* C: me_Length = its size */
    .balign 4
kt_tags:
    .long   0                       /* C: the resident */
    .long   0                       /* C: the old KickTagPtr with bit 31 set (a link), or 0 (the end) */
kt_oldmem:
    .long   0                       /* C: the old KickMemPtr */
kt_oldtag:
    .long   0                       /* C: the old KickTagPtr */
kt_res:                             /* struct Resident */
    .word   0x4AFC                  /* rt_MatchWord */
    .long   0                       /* C: rt_MatchTag = kt_res */
    .long   0                       /* C: rt_EndSkip = the end of the block */
    .byte   1                       /* rt_Flags: RTF_COLDSTART */
    .byte   1                       /* rt_Version */
    .byte   0                       /* rt_Type: NT_UNKNOWN */
    .byte   -59                     /* rt_Pri: after the drivers, before strap (-60) */
    .long   0                       /* C: rt_Name */
    .long   0                       /* C: rt_IdString */
    .long   0                       /* C: rt_Init = kt_code */
kt_name:
    .asciz  "bootmenu next boot"
kt_expname:
    .asciz  "expansion.library"
kt_target:
    .space  32                      /* C: the partition's device name, without the colon, NUL-terminated */
    .balign 2

/* rt_Init: called by exec (InitResident, not RTF_AUTOINIT) with a6 = SysBase */
kt_code:
    movem.l %d2-%d3/%a2-%a4/%a6,-(%sp)
    move.l  4.w,%a6
    move.l  kt_oldmem(%pc),0x222(%a6)   /* KickMemPtr */
    move.l  kt_oldtag(%pc),0x226(%a6)   /* KickTagPtr */
    moveq   #0,%d0
    move.l  0x222(%a6),%d1
    or.l    0x226(%a6),%d1
    beq.s   1f
    jsr     -612(%a6)               /* SumKickData */
1:  move.l  %d0,0x22A(%a6)          /* KickCheckSum */
    lea     kt_expname(%pc),%a1
    moveq   #0,%d0
    jsr     -552(%a6)               /* OpenLibrary("expansion.library", 0) */
    tst.l   %d0
    beq.s   8f
    move.l  %d0,%a4
    lea     0x4A(%a4),%a2           /* eb_MountList */
    move.l  (%a2),%a3               /* lh_Head */
2:  tst.l   (%a3)                   /* ln_Succ 0: past the last node */
    beq.s   7f
    move.l  16(%a3),%d0             /* bn_DeviceNode */
    beq.s   6f
    move.l  %d0,%a0
    move.l  40(%a0),%d0             /* dn_Name, a BSTR */
    beq.s   6f
    lsl.l   #2,%d0
    move.l  %d0,%a0
    moveq   #0,%d1
    move.b  (%a0)+,%d1              /* its length */
    lea     kt_target(%pc),%a1
3:  move.b  (%a1)+,%d2              /* the target ends: a match if the name ends here too */
    beq.s   4f
    subq.w  #1,%d1
    bmi.s   6f                      /* the name is shorter */
    move.b  (%a0)+,%d3
    or.b    #0x20,%d2               /* compare ignoring case (letters and digits only in drive names) */
    or.b    #0x20,%d3
    cmp.b   %d2,%d3
    beq.s   3b
    bra.s   6f
4:  tst.w   %d1
    bne.s   6f                      /* the name is longer */
    move.l  %a3,%a1
    jsr     -252(%a6)               /* Remove(node) */
    move.b  #127,9(%a3)             /* ln_Pri */
    move.l  %a2,%a0
    move.l  %a3,%a1
    jsr     -270(%a6)               /* Enqueue(eb_MountList, node) */
    bra.s   7f
6:  move.l  (%a3),%a3               /* the next node */
    bra.s   2b
7:  move.l  %a4,%a1
    jsr     -414(%a6)               /* CloseLibrary */
8:  moveq   #0,%d0
    movem.l (%sp)+,%d2-%d3/%a2-%a4/%a6
    rts
    .balign 4
kt_blob_end:
