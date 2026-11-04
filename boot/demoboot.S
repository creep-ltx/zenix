/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's bootblock (DESIGN D25 revision 3, section 6): reads the stage 2 (boot/demoload*.c, a ZEXE image
 * at disk byte 1024), relocates it, clears its BSS and jumps to it. Kickstart loads these 1024 bytes, checks the
 * checksum and enters at offset 12 with a1 = the trackdisk request and a6 = ExecBase. Only 68000 code: the stage 2
 * itself tells a 68000 Amiga that it cannot run Zenix.
 *
 * tools/mkdemo.py patches the long at offset 8 with the stage 2's size on the disk (a multiple of 512) and the
 * long at offset 1020 with the memory it needs (the ZEXE's mem_size, at least its file size). The plain RAM floppy's
 * bootblock (boot/bootblock.S) stays as it is.
 *
 * The stage 2 is read from this disk only, and checked: its header, the relocation table inside the file, every
 * relocated long inside the image. A failure turns the whole screen red (the bootblock colours of boot/handoff.S). */
#define DIAG_BLUE       0x00F
#define DIAG_RED        0xF00

    .text
    .globl  _start
_start:
    move.w  #0x0180,0xDFF096        /* DMACON: copper and bitplane DMA off: COLOR00 fills the whole screen */
    move.w  #0x0200,0xDFF100        /* BPLCON0: no bitplanes, colour on */
    move.w  #DIAG_BLUE,0xDFF180     /* stage 1: loading */
    move.l  %a1,%a5                 /* the trackdisk request */
    move.l  _start-4(%pc),%d7       /* the stage 2's size on the disk (offset 8) */
    move.l  _start+1008(%pc),%d6    /* the memory it needs (offset 1020) */
    cmp.l   %d7,%d6
    bhs.s   1f
    move.l  %d7,%d6                 /* never less than the file */
1:  move.l  %d6,%d0
    move.l  #0x10001,%d1            /* MEMF_PUBLIC | MEMF_CLEAR */
    cmp.w   #36,20(%a6)             /* before V36 trackdisk reads only into chip RAM (RKM Devices): there chip, so */
    bhs.s   2f                      /* a Kickstart 1.3 machine gets the stage 2's "needs 2.04" screen, not red */
    move.l  #0x10002,%d1            /* MEMF_CHIP | MEMF_CLEAR */
2:
    jsr     -198(%a6)               /* AllocMem (never freed: the OS dies) */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a4                 /* a4 = the stage 2 */

    move.w  #2,28(%a5)              /* io_Command = CMD_READ */
    move.l  %d7,36(%a5)             /* io_Length */
    move.l  %a4,40(%a5)             /* io_Data */
    move.l  #1024,44(%a5)           /* io_Offset */
    move.l  %a5,%a1
    jsr     -456(%a6)               /* DoIO */
    tst.b   %d0
    bne     fail

    /* the ZEXE header (tools/mkzexe.py): 'ZEXE' at +4, image size +8, mem size +12, nreloc +16, reloc_off +20 */
    cmp.l   #0x5A455845,4(%a4)
    bne     fail
    move.l  8(%a4),%d5              /* image size */
    cmp.l   %d7,%d5
    bhi     fail
    cmp.l   12(%a4),%d6             /* the memory taken holds the image and its BSS */
    blo     fail
    move.l  16(%a4),%d4             /* nreloc */
    move.l  20(%a4),%d3             /* reloc_off */
    cmp.l   %d5,%d3                 /* the table lies after the image and inside the file */
    blo     fail
    cmp.l   #0x10000,%d4            /* far more than any stage 2 has: refused before the multiply */
    bhs     fail
    move.l  %d4,%d0
    lsl.l   #2,%d0
    add.l   %d3,%d0
    cmp.l   %d7,%d0
    bhi     fail
    lea     (%a4,%d3.l),%a0         /* a0 = the relocation table */
    move.l  %d5,%d2
    subq.l  #4,%d2                  /* the last place a long fits in the image */
    bra.s   3f
2:  move.l  (%a0)+,%d0              /* a long that holds a link-time address */
    cmp.l   %d2,%d0
    bhi     fail
    btst    #0,%d0                  /* a 68000 reads longs only at even addresses */
    bne     fail
    move.l  %a4,%d1
    add.l   %d1,(%a4,%d0.l)
3:  dbf     %d4,2b

    /* the BSS: from the image's end to the memory's end (the relocation table was read there) */
    lea     (%a4,%d5.l),%a0
    move.l  %d6,%d0
    sub.l   %d5,%d0
    lsr.l   #1,%d0                  /* words (the sizes are even) */
    bra.s   5f
4:  clr.w   (%a0)+
5:  subq.l  #1,%d0
    bpl.s   4b

    cmp.w   #37,20(%a6)             /* CacheClearU is Kickstart 2.0's (on 1.3 the stage 2 only says it needs 2.04) */
    blo.s   6f
    jsr     -636(%a6)               /* CacheClearU: the stage 2 was just written */
6:
    move.l  %a5,%a1
    jmp     (%a4)                   /* d7 = its size on the disk: the table follows it */

fail:
    move.w  #DIAG_RED,0xDFF180      /* the whole screen red = the bootblock failed (no memory, a read error, a bad stage 2) */
    move.w  #9,28(%a5)              /* TD_MOTOR, io_Length 0: the drive's motor off */
    clr.l   36(%a5)
    move.l  %a5,%a1
    jsr     -456(%a6)               /* DoIO */
7:  moveq   #0,%d0
    jsr     -318(%a6)               /* Wait(0): the boot task sleeps for good, the screen stays red */
    bra.s   7b
