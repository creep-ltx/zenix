/* Floppy bootblock for Zenix (bootinfo v2).
 *
 * The Kickstart boot strap loads sectors 0-1 (1024 bytes), verifies the
 * checksum, and jumps to offset 12 with a1 = trackdisk IORequest and
 * a6 = ExecBase. We load the kernel (disk offset 1024) into a chip
 * buffer via trackdisk, harvest exec's memory list into a bootinfo block
 * at 0x1000, pick the largest fast-RAM region as the kernel's home
 * (256KB-aligned; chip 0x40000 as fallback), kill the OS, copy the
 * kernel there and jump to it. Never returns.
 *
 * bootinfo v3 at 0x1000:
 *   +0  u16 AttnFlags        +2  u16 nregions
 *   +4  u32 kernel_pa        +8  u32 kernel_size
 *   +12 u32 DefaultFont (ROM Topaz TextFont*, 0 if unavailable)
 *   +16 {u32 attrs, u32 lower, u32 upper}[8]
 *
 * hwinfo at 0x1100 (kernel/hwinfo.h): the Pi hardware addresses, filled in by stage0.
 *
 * The longs at bootblock offsets 8 and 1020 are patched by tools/mkadf.py with
 * the kernel size and the ramdisk size (Kickstart jumps to offset 12, so the
 * code starts there) (both multiples of 512; the ramdisk
 * image follows the kernel on the disk and is loaded with it; bootinfo
 * +112 = its address, +116 = its size, 0 if there is none).
 */
    .text
    .globl  _start
_start:
    move.l  %a1,%a5                 /* trackdisk ioreq */
    move.l  _start-4(%pc),%d7       /* kernel size (bootblock offset 8) */
    move.l  _start+1008(%pc),0x1074 /* ramdisk size (last long of the block, offset 1020; 0 = none) */
    clr.l   0x1070

    move.l  %d7,%d0
    add.l   0x1074,%d0              /* kernel + ramdisk are loaded together */
    moveq   #2,%d1                  /* MEMF_CHIP */
    jsr     -198(%a6)               /* AllocMem */
    tst.l   %d0
    beq     fail
    move.l  %d0,%a4                 /* load buffer */

    move.w  #2,28(%a5)              /* io_Command = CMD_READ */
    move.l  %d7,%d0
    add.l   0x1074,%d0
    move.l  %d0,36(%a5)             /* io_Length */
    move.l  %a4,40(%a5)             /* io_Data */
    move.l  #1024,44(%a5)           /* io_Offset */
    move.l  %a5,%a1
    jsr     -456(%a6)               /* DoIO */
    tst.l   %d0
    bne     fail

    /* the ramdisk stays in the load buffer, right behind the kernel image */
    move.l  %a4,%d0
    add.l   %d7,%d0
    move.l  %d0,0x1070

    /* ---- harvest bootinfo ---- */
    move.w  296(%a6),0x1000         /* AttnFlags */
    move.l  %d7,0x1008              /* kernel_size */

    /* GfxBase pointer, saved before the OS dies: the kernel walks its
     * TextFonts list for ROM Topaz 8 */
    lea     gfxname(%pc),%a1
    moveq   #0,%d0
    jsr     -552(%a6)               /* OpenLibrary */
    move.l  %d0,0x100C

    /* stage0: position-independent code inside the kernel image (ZEXE header +28, 0 = none).
     * It reads the Pi's hardware addresses from Emu68's device tree into the table at
     * HWINFO_ADDR (0x1100) while AmigaOS is alive. Clears the caches first: the image was
     * just DMA'd into chip RAM. Preserves everything but d0/d1/a0/a1. */
    move.w  296(%a6),%d0            /* AttnFlags: stage0 is 68020 code, a 68000 must reach the kernel's own message */
    and.w   #0x008E,%d0
    beq.s   nostage0
    move.l  28(%a4),%d0
    beq.s   nostage0
    jsr     -636(%a6)               /* CacheClearU */
    move.l  28(%a4),%d0             /* (d0 was clobbered) */
    lea     (%a4,%d0.l),%a0
    jsr     (%a0)
nostage0:
    lea     0x1010,%a3
    moveq   #0,%d6
    move.l  322(%a6),%a2            /* exec MemList lh_Head */
mloop:
    move.l  (%a2),%d0               /* ln_Succ */
    beq.s   mdone
    cmp.w   #8,%d6
    bge.s   mdone
    moveq   #0,%d1
    move.w  14(%a2),%d1             /* mh_Attributes */
    move.l  %d1,(%a3)+
    move.l  20(%a2),(%a3)+          /* mh_Lower */
    move.l  24(%a2),(%a3)+          /* mh_Upper */
    addq.w  #1,%d6
    move.l  %d0,%a2
    bra.s   mloop
mdone:
    move.w  %d6,0x1002

    /* ---- pick kernel home: largest fast region, 256K-aligned ---- */
    moveq   #0,%d5                  /* best usable size */
    move.l  #0x40000,%d4            /* fallback: chip 0x40000 */
    lea     0x1010,%a0
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
    move.l  %d4,0x1004              /* bootinfo kernel_pa */

    jsr     -150(%a6)               /* SuperState: supervisor from here on */

    lea     0xDFF000,%a0
    move.w  #0x7FFF,0x9A(%a0)       /* INTENA: everything off */
    move.w  #0x7FFF,0x9C(%a0)       /* INTREQ: everything clear */
    move.w  #0x7FFF,0x96(%a0)       /* DMACON: everything off */
    move.b  #0x7F,0xBFED01          /* CIA-A ICR: all sources off */
    move.b  #0x7F,0xBFDD00          /* CIA-B ICR: all sources off */

    /* relocate the copier clear of any possible overlap */
    lea     copier(%pc),%a0
    lea     0x500,%a1
    moveq   #(copier_end-copier)/2-1,%d0
1:  move.w  (%a0)+,(%a1)+
    dbf     %d0,1b

    move.l  %a4,%a0                 /* src: chip load buffer */
    move.l  %d4,%a1                 /* dst: kernel home */
    move.l  %d4,%a2                 /* jump target after copy */
    move.l  %d7,%d1                 /* bytes */
    move.l  %d7,%d0
    lsr.l   #2,%d0                  /* longwords */
    jmp     0x500

fail:
    move.w  #0xF00,0xDFF180         /* red background = boot failure */
    bra.s   fail

gfxname:
    .asciz  "graphics.library"
    .balign 2

/* Overlap-safe copier; position-independent, executes at 0x500.
 * In: a0 src, a1 dst, a2 jump target, d0 longwords, d1 bytes. */
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
    lea     0x10000,%sp             /* temp stack in chip, above bootinfo */
    jmp     (%a2)
copier_end:
