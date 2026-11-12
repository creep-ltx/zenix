/* SPDX-License-Identifier: GPL-2.0-only */
/* Floppy bootblock for Zenix (bootinfo v3).
 *
 * The Kickstart boot strap loads sectors 0-1 (1024 bytes), verifies the
 * checksum, and jumps to offset 12 with a1 = trackdisk IORequest and
 * a6 = ExecBase. We load the kernel (disk offset 1024) into a chip
 * buffer via trackdisk, harvest exec's memory list into a bootinfo block,
 * pick the largest fast-RAM region as the kernel's home (256KB-aligned;
 * chip 0x40000 as fallback), kill the OS, copy the kernel there and jump
 * to it. Never returns.
 *
 * The bootinfo/hwinfo table and what is handed to the kernel: boot/handoff.S.
 *
 * The longs at bootblock offsets 8 and 1020 are patched by tools/mkadf.py with
 * the kernel size and the ramdisk size (Kickstart jumps to offset 12, so the
 * code starts there) (both multiples of 512; the ramdisk
 * image follows the kernel on the disk and is loaded with it).
 */
/* The stage colours, the klog-area size and the hand-over to the kernel (from the memory list to the jump) are shared
 * with zenixboot: boot/handoff.S. */
#include "handoff.S"

    .text
    .globl  _start
_start:
    move.w  #0x0180,0xDFF096        /* DMACON: copper and bitplane DMA off: COLOR00 fills the whole screen */
    move.w  #0x0200,0xDFF100        /* BPLCON0: no bitplanes, colour on */
    move.w  #0,0xDFF106             /* BPLCON3 (ECS/AGA): bank 0, no border blanking (not clr: a 68000 reads first) */
    move.w  #DIAG_BLUE,0xDFF180
    DIAGSTOP(1)
    move.l  %a1,%a5                 /* trackdisk ioreq */
    move.l  _start-4(%pc),%d7       /* kernel size (bootblock offset 8) */

    /* keep the kernel log's area (the top 16.5 KB of chip RAM, below MaxLocMem) out of every buffer below: the kernel reads
     * the previous boot's log from there. If exec already uses it AllocAbs fails; harmless (the kernel checks). */
    move.l  62(%a6),%a1             /* ExecBase->MaxLocMem: top of chip RAM */
    lea     -KLOG_AREA_SIZE(%a1),%a1
    move.l  #KLOG_AREA_SIZE,%d0
    jsr     -204(%a6)               /* AllocAbs (never freed: the OS dies) */

    /* the bootinfo + hwinfo table: 1 KB of cleared chip RAM (bootinfo 120 bytes, hwinfo 440 at +0x100), moved to
     * 0x1000 by the copier */
    move.l  #0x400,%d0
    move.l  #0x10002,%d1            /* MEMF_CHIP | MEMF_CLEAR */
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a3                 /* a3 = table, kept across every OS call (a2-a6 are preserved) */
    move.l  _start+1008(%pc),0x74(%a3)  /* ramdisk size (last long of the block, offset 1020; 0 = none) */

    move.l  %d7,%d6
    add.l   0x74(%a3),%d6           /* kernel + ramdisk are loaded together: d6 their size */
    /* With a RAM disk: MEMF_CHIP | MEMF_REVERSE, the top of chip RAM, away from the kernel's home at 0x40000 on a machine with no fast
     * RAM; the lowest fit would lie across it, and the copy would overwrite the RAM disk behind the image (audit A7). But never over
     * the kernel log and the boot note at the very top (KLOG_AREA_SIZE below MaxLocMem, kept across a reset for dmesg -p): where the
     * AllocAbs above did not get that area and the buffer reaches into it, it is given back and taken low, as without a RAM disk. */
    tst.l   0x74(%a3)
    beq.s   lowbuf
    /* KLOG_AREA_SIZE more than needed: exec may hold part of the log's area (then the AllocAbs failed) and leave the rest free; the
     * buffer is the block's lower part, so it ends at least that far below the top. Not given back: the OS dies */
    move.l  %d6,%d0
    add.l   #KLOG_AREA_SIZE,%d0
    move.l  #0x40002,%d1
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq.s   lowbuf
    move.l  %d0,%a4
    add.l   %d6,%d0                 /* the buffer's end */
    move.l  62(%a6),%d1             /* MaxLocMem */
    sub.l   #KLOG_AREA_SIZE,%d1
    cmp.l   %d1,%d0
    bls.s   havebuf
    move.l  %a4,%a1
    move.l  %d6,%d0
    add.l   #KLOG_AREA_SIZE,%d0
    jsr     -210(%a6)               /* FreeMem */
lowbuf:
    move.l  %d6,%d0
    moveq   #2,%d1                  /* MEMF_CHIP */
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a4                 /* load buffer */
havebuf:

    move.w  #2,28(%a5)              /* io_Command = CMD_READ */
    move.l  %d7,%d0
    add.l   0x74(%a3),%d0
    move.l  %d0,36(%a5)             /* io_Length */
    move.l  %a4,40(%a5)             /* io_Data */
    move.l  #1024,44(%a5)           /* io_Offset */
    move.l  %a5,%a1
    jsr     -456(%a6)               /* DoIO */
    tst.l   %d0
    bne     fail
    move.w  #DIAG_CYAN,0xDFF180
    DIAGSTOP(2)

    /* the drive is not used again: motor off (TD_MOTOR with io_Length 0), or it would run until Zenix's own
     * floppy driver happens to use the drive (it only stops motors it started) */
    move.w  #9,28(%a5)              /* io_Command = TD_MOTOR */
    clr.l   36(%a5)                 /* io_Length = 0: off */
    move.l  %a5,%a1
    jsr     -456(%a6)               /* DoIO (the result does not matter) */

    handoff_tables                  /* bootinfo, stage0, memory list, the kernel's home (d4) */
    /* a RAM disk below the end of the kernel's memory at its home (a chip-only machine with too little of it): the screen goes red,
     * never a kernel over its own root (audit A7) */
    tst.l   0x74(%a3)
    beq.s   ramok
    move.l  12(%a4),%d1             /* the kernel's memory (ZEXE +12) */
    add.l   %d4,%d1
    cmp.l   0x70(%a3),%d1
    bhi     ramlow
ramok:
    handoff_kill                    /* AmigaOS off */
    handoff_jump                    /* copier to 0x500, copy, jump: never returns */

ramlow:
    cmp.l   0x70(%a3),%d4           /* (a home above the RAM disk, in fast RAM, is no overlap) */
    bhs.s   ramok
fail:
    move.w  #0xF00,0xDFF180         /* the whole screen red = the bootblock failed (no memory, or a floppy read error) */
    bra.s   fail

    handoff_subs
