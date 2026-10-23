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
 * bootinfo v3, at 0x1000 when the kernel runs:
 *   +0  u16 AttnFlags        +2  u16 nregions
 *   +4  u32 kernel_pa        +8  u32 kernel_size
 *   +12 u32 GfxBase (the kernel finds ROM Topaz 8 through it, 0 if unavailable)
 *   +16 {u32 attrs, u32 lower, u32 upper}[8]
 *   +112 u32 ramdisk address, +116 u32 ramdisk size (0 if there is none)
 *
 * hwinfo (kernel/hwinfo.h, 408 bytes) follows at +0x100 (0x1100 for the kernel): the Pi hardware addresses, filled
 * in by stage0. Together they take 0x1000-0x1297; the kernel keeps 0-0x1FFF out of its memory pools.
 *
 * While AmigaOS is alive, 0x1000-0x13FF belongs to exec (its chip-RAM free list starts above it, so it can neither
 * be allocated nor assumed free): the table is built in a 1 KB block from AllocMem (a3) and copied to 0x1000 by
 * the copier, after interrupts, DMA and the OS are gone. The kernel's fixed address stays.
 *
 * The longs at bootblock offsets 8 and 1020 are patched by tools/mkadf.py with
 * the kernel size and the ramdisk size (Kickstart jumps to offset 12, so the
 * code starts there) (both multiples of 512; the ramdisk
 * image follows the kernel on the disk and is loaded with it).
 */
/* Boot stage colours (audit A11; the table is in docs/pi-first-boot.md): the whole screen shows COLOR00 from the
 * first instruction on (copper and bitplane DMA off, 0 bitplanes), so a freeze is named by its colour. Same values
 * as kernel/diag.h. A test build may stop at stage N (make DIAG=-DDIAG_STOP=N) to check the colour in FS-UAE. */
#define DIAG_BLUE       0x00F       /* 1: the bootblock runs, loading the kernel from the floppy */
#define DIAG_CYAN       0x0FF       /* 2: loaded; stage0 (Pi tables) and the memory list, AmigaOS still alive */
#define DIAG_MAGENTA    0xF0F       /* 3: AmigaOS switched off, the copier moves the kernel to its home */
#ifdef DIAG_STOP
#define DIAGSTOP(n)     .if DIAG_STOP == n ; 99: bra.s 99b ; .endif
#else
#define DIAGSTOP(n)
#endif
/* The previous-boot log (audit A12): the top 16.5 KB of chip RAM (KLOG_AREA_SIZE in kernel/kernel.h, the same value) */
#define KLOG_AREA_SIZE  0x4200

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

    /* the bootinfo + hwinfo table: 1 KB of cleared chip RAM (bootinfo 120 bytes, hwinfo 408 at +0x100), moved to
     * 0x1000 by the copier */
    move.l  #0x400,%d0
    move.l  #0x10002,%d1            /* MEMF_CHIP | MEMF_CLEAR */
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a3                 /* a3 = table, kept across every OS call (a2-a6 are preserved) */
    move.l  _start+1008(%pc),0x74(%a3)  /* ramdisk size (last long of the block, offset 1020; 0 = none) */

    move.l  %d7,%d0
    add.l   0x74(%a3),%d0           /* kernel + ramdisk are loaded together */
    moveq   #2,%d1                  /* MEMF_CHIP */
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a4                 /* load buffer */

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

    /* the ramdisk stays in the load buffer, right behind the kernel image */
    move.l  %a4,%d0
    add.l   %d7,%d0
    move.l  %d0,0x70(%a3)

    /* ---- harvest bootinfo ---- */
    move.w  296(%a6),(%a3)          /* AttnFlags */
    move.l  %d7,8(%a3)              /* kernel_size */

    /* GfxBase pointer, saved before the OS dies: the kernel walks its
     * TextFonts list for ROM Topaz 8 */
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     -552(%a6)               /* OpenLibrary */
    move.l  %d0,12(%a3)

    /* stage0: position-independent code inside the kernel image (ZEXE header +28, 0 = none).
     * It reads the Pi's hardware addresses from Emu68's device tree into the hwinfo table (a1)
     * while AmigaOS is alive. Clears the caches first: the image was
     * just DMA'd into chip RAM. Preserves everything but d0/d1/a0/a1. */
    move.w  296(%a6),%d0            /* AttnFlags: stage0 is 68020 code, a 68000 must reach the kernel's own message */
    and.w   #0x008E,%d0
    beq.s   nostage0
    move.l  28(%a4),%d0
    beq.s   nostage0
    jsr     -636(%a6)               /* CacheClearU */
    move.l  28(%a4),%d0             /* (d0 was clobbered) */
    lea     (%a4,%d0.l),%a0
    lea     0x100(%a3),%a1          /* the hwinfo table */
    jsr     (%a0)
nostage0:
    lea     0x10(%a3),%a0           /* region list */
    moveq   #0,%d6
    move.l  322(%a6),%a2            /* exec MemList lh_Head */
mloop:
    move.l  (%a2),%d0               /* ln_Succ */
    beq.s   mdone
    cmp.w   #8,%d6
    bge.s   mdone
    moveq   #0,%d1
    move.w  14(%a2),%d1             /* mh_Attributes */
    move.l  %d1,(%a0)+
    move.l  20(%a2),(%a0)+          /* mh_Lower */
    move.l  24(%a2),(%a0)+          /* mh_Upper */
    addq.w  #1,%d6
    move.l  %d0,%a2
    bra.s   mloop
mdone:
    move.w  %d6,2(%a3)

    /* ---- pick kernel home: largest fast region, 256K-aligned ---- */
    moveq   #0,%d5                  /* best usable size */
    move.l  #0x40000,%d4            /* fallback: chip 0x40000 */
    lea     0x10(%a3),%a0
    move.w  %d6,%d3
    subq.w  #1,%d3
    bmi.s   nofast
rloop:
    move.l  (%a0)+,%d0              /* attrs */
    move.l  (%a0)+,%d1              /* lower */
    move.l  (%a0)+,%d2              /* upper */
    btst    #2,%d0                  /* MEMF_FAST? */
    beq.s   rnext
    add.l   #0x3FFFF,%d1
    and.l   #0xFFFC0000,%d1         /* align lower up to 256KB */
    cmp.l   %d1,%d2
    bls.s   rnext                   /* nothing usable after align */
    move.l  %d2,%d0
    sub.l   %d1,%d0                 /* usable bytes */
    cmp.l   %d5,%d0
    bls.s   rnext
    move.l  %d0,%d5
    move.l  %d1,%d4                 /* new best home */
rnext:
    dbf     %d3,rloop
nofast:
    move.l  %d4,4(%a3)              /* bootinfo kernel_pa */

    jsr     -150(%a6)               /* SuperState: supervisor from here on */

    lea     0xDFF000,%a0
    move.w  #0x7FFF,0x9A(%a0)       /* INTENA: everything off */
    move.w  #0x7FFF,0x9C(%a0)       /* INTREQ: everything clear */
    move.w  #0x7FFF,0x96(%a0)       /* DMACON: everything off */
    move.b  #0x7F,0xBFED01          /* CIA-A ICR: all sources off */
    move.b  #0x7F,0xBFDD00          /* CIA-B ICR: all sources off */
    move.w  #DIAG_MAGENTA,0x180(%a0)
    DIAGSTOP(3)

    /* relocate the copier clear of any possible overlap */
    lea     copier(%pc),%a0
    lea     0x500,%a1
    moveq   #(copier_end-copier)/2-1,%d0
1:  move.w  (%a0)+,(%a1)+
    dbf     %d0,1b
    bsr.s   iflush                  /* code was just written at 0x500 */

    move.l  %a4,%a0                 /* src: chip load buffer */
    move.l  %d4,%a1                 /* dst: kernel home */
    move.l  %d4,%a2                 /* jump target after copy */
    move.l  %d7,%d1                 /* bytes */
    move.l  %d7,%d0
    lsr.l   #2,%d0                  /* longwords */
    jmp     0x500

fail:
    move.w  #0xF00,0xDFF180         /* the whole screen red = the bootblock failed (no memory, or a floppy read error) */
    bra.s   fail

/* Instruction-cache flush after code was written (supervisor mode): the CPU
 * (or Emu68's JIT) must not run stale copies of what was there before.
 * 020/030: CACR bit 3 clears the instruction cache (the enable bits stay as
 * the OS left them); 040/060 and Emu68: CPUSHA BC (0xF4F8; never CINVA, which
 * drops dirty data on Emu68). A 68000 has no cache and no CACR. In: nothing;
 * clobbers d0. */
iflush:
    move.w  (%a3),%d0               /* AttnFlags */
    and.w   #0x008E,%d0
    beq.s   1f                      /* 68000/010 */
    and.w   #0x0088,%d0
    bne.s   2f
    movec   %cacr,%d0
    or.w    #0x0008,%d0
    movec   %d0,%cacr
1:  rts
2:  .word   0xF4F8                  /* cpusha bc */
    rts

gfxname:
    .asciz  "graphics.library"
    .balign 2

/* Overlap-safe copier; position-independent, executes at 0x500.
 * In: a0 src, a1 dst, a2 jump target, a3 bootinfo table, d0 longwords, d1 bytes. */
copier:
    cmp.l   %a1,%a0
    bhi.s   cfwd
    add.l   %d1,%a0                 /* src below dst: copy backwards */
    add.l   %d1,%a1
cbwd:
    move.l  -(%a0),-(%a1)
    subq.l  #1,%d0
    bne.s   cbwd
    bra.s   cgo
cfwd:
    move.l  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   cfwd
cgo:
    lea     0x1000,%a1              /* bootinfo + hwinfo to their fixed home: the OS is dead, the memory is ours */
    move.w  #0x400/4-1,%d0
1:  move.l  (%a3)+,(%a1)+
    dbf     %d0,1b
    /* the kernel was just written: flush the instruction cache / Emu68's translations (same as iflush) */
    move.w  0x1000,%d0
    and.w   #0x008E,%d0
    beq.s   3f
    and.w   #0x0088,%d0
    bne.s   2f
    movec   %cacr,%d0
    or.w    #0x0008,%d0
    movec   %d0,%cacr
    bra.s   3f
2:  .word   0xF4F8                  /* cpusha bc */
3:  lea     0x10000,%sp             /* temp stack in chip, above bootinfo */
    jmp     (%a2)
copier_end:
