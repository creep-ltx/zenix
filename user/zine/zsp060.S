/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's 68060 support package (docs/d29-zine.md 16.14): Motorola's M68060 Software Package (msp/060: fpsp.sa, the
 * floating-point kernel module, and isp.sa, the unimplemented integer module, as released, under Motorola's licence
 * in msp/060/README) with ZINE's call-outs. A real 68060 lacks in hardware what AmigaOS's 68060.library emulates: the
 * FPU's transcendental and other instructions (FSIN, FCOS, FETOX, FLOGN, FMOD, FMOVECR, ...), packed decimal, the
 * unimplemented data types and effective addresses, and the integer MULU.L/MULS.L/DIVU.L/DIVS.L with 64 bits, MOVEP,
 * CAS2, CHK2/CMP2 and a misaligned CAS. /bin/zine loads this file (a ZEXE image, tools/mkzexe.py; position-independent,
 * no relocations) on a 68040/060 machine and the blob points the CPU's vectors at its entry points for the session
 * (blob.c zb_sp_install), as the call-out sections of the package's documentation (msp/060/fpsp.doc, isp.doc) say.
 *
 * The header after the ZEXE one (offset 32): "ZSPK", the CPU (60), the address of the two hooks the blob fills
 * (ZINE's line-F handler for an F-line that is no FPU instruction, and its crash handler), the number of vector
 * entries, then each: the vector number (bit 8: only with an FPU) and the entry's address (relocated by zine).
 * Every call-out keeps every register; ZINE runs with the MMU off, so the memory call-outs are plain copies in either
 * mode. */
#define NEEDFPU 0x100

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   _start                      /* not called: the blob takes the vectors from the header */
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
    .ascii  "ZSPK"                      /* +32 */
    .long   60                          /* +36: the CPU */
    .long   hooks                       /* +40 (relocated by zine: an address, as the vectors below) */
    .long   (vend - vtab) / 8           /* +44 */
vtab:                                   /* +48: the vectors (the 68060 User's Manual's numbers) */
    .long   NEEDFPU + 11, fp_top + 0x80 + 0x30         /* line F: _060_fpsp_fline */
    .long   NEEDFPU + 54, fp_top + 0x80 + 0x00         /* SNAN: _060_fpsp_snan */
    .long   NEEDFPU + 52, fp_top + 0x80 + 0x08         /* OPERR */
    .long   NEEDFPU + 53, fp_top + 0x80 + 0x10         /* OVFL */
    .long   NEEDFPU + 51, fp_top + 0x80 + 0x18         /* UNFL */
    .long   NEEDFPU + 50, fp_top + 0x80 + 0x20         /* DZ */
    .long   NEEDFPU + 49, fp_top + 0x80 + 0x28         /* INEX */
    .long   NEEDFPU + 55, fp_top + 0x80 + 0x38         /* unimplemented data type: _060_fpsp_unsupp */
    .long   NEEDFPU + 60, fp_top + 0x80 + 0x40         /* unimplemented effective address: _effadd */
    .long   61, i_top + 0x80 + 0x00                    /* unimplemented integer: _060_isp_unimp */
vend:
hooks:
h_fline: .long  0                       /* ZINE's line-F vector (zv_linef): MMU no-ops, else a crash */
h_crash: .long  0                       /* ZINE's crash vector (zv_crash) */

    /* (one section: the header's offsets are differences inside it) */
/* jump to a hook / to what the vector table holds now, every register kept */
.macro HOOK h
    move.l  \h(%pc),-(%sp)
    rts
.endm
.macro VEC n
    subq.l  #4,%sp
    move.l  %a0,-(%sp)
    movec   %vbr,%a0
    move.l  4*\n(%a0),4(%sp)
    move.l  (%sp)+,%a0
    rts
.endm

/* the FP exceptions a program enabled in FPCR, after the package's IEEE handling: the exception taken off the FPU's
 * state (the frame made a null one's status, as the package's sample call-outs do), then ZINE's crash handler names
 * it (no task trap handler in ZINE: under AmigaOS the default one shows an alert) */
r_fpex:
    fsave   -(%sp)
    move.w  #0x6000,2(%sp)
    frestore (%sp)+
    HOOK    h_crash
r_bsun:
    fmove.l %fpsr,-(%sp)
    andi.b  #0xFE,(%sp)
    fmove.l (%sp)+,%fpsr
    HOOK    h_crash
r_fline:
    HOOK    h_fline
r_fpu_disabled:                         /* the FPU switched off in PCR: on again, the instruction tried again */
    move.l  %d0,-(%sp)
    .long   0x4E7A0808                  /* movec pcr,d0 */
    bclr    #1,%d0
    .long   0x4E7B0808                  /* movec d0,pcr */
    move.l  (%sp)+,%d0
    move.l  0xC(%sp),2(%sp)             /* the frame's "current PC" */
    rte
r_trap:                                 /* FTRAPcc taken: the TRAPcc vector */
    VEC     7
r_trace:
    VEC     9
r_access:
    VEC     2
r_chk:                                  /* CHK2 out of bounds */
    VEC     6
r_divzero:                              /* a 64-bit divide by zero */
    VEC     5
r_done:
    rte
r_lock:                                 /* _060_real_lock_page / unlock: no MMU, nothing to lock */
r_unlock:
    clr.l   %d0
    rts

/* memory: a0/a1 the addresses, d0 the count or the data; d1 = 0 (success) */
m_read:                                 /* imem_read, dmem_read: a0 source, a1 destination, d0 bytes */
m_write:                                /* dmem_write: the same */
    tst.l   %d0
    beq.s   2f
1:  move.b  (%a0)+,(%a1)+
    subq.l  #1,%d0
    bne.s   1b
2:  clr.l   %d1
    rts
m_rw:                                   /* imem_read_word, dmem_read_word */
    clr.l   %d0
    move.w  (%a0),%d0
    clr.l   %d1
    rts
m_rl:                                   /* imem_read_long, dmem_read_long */
    move.l  (%a0),%d0
    clr.l   %d1
    rts
m_rb:
    clr.l   %d0
    move.b  (%a0),%d0
    clr.l   %d1
    rts
m_wb:
    move.b  %d0,(%a0)
    clr.l   %d1
    rts
m_ww:
    move.w  %d0,(%a0)
    clr.l   %d1
    rts
m_wl:
    move.l  %d0,(%a0)
    clr.l   %d1
    rts

/* the FPSP's call-out section, then the package (fpsp.doc: "68060FPSP call-outs") */
    .balign 4
fp_top:
    .long   r_bsun - fp_top, r_fpex - fp_top, r_fpex - fp_top, r_fpex - fp_top     /* bsun snan operr ovfl */
    .long   r_fpex - fp_top, r_fpex - fp_top, r_fpex - fp_top, r_fline - fp_top    /* unfl dz inex fline */
    .long   r_fpu_disabled - fp_top, r_trap - fp_top, r_trace - fp_top, r_access - fp_top
    .long   r_done - fp_top, 0, 0, 0
    .long   m_read - fp_top, m_read - fp_top, m_write - fp_top, m_rw - fp_top
    .long   m_rl - fp_top, m_rb - fp_top, m_rw - fp_top, m_rl - fp_top
    .long   m_wb - fp_top, m_ww - fp_top, m_wl - fp_top, 0
    .long   0, 0, 0, 0
#include "msp/060/fpsp.sa"

/* the ISP's call-out section, then the package (isp.doc: "68060ISP call-outs"); cas and cas2 go back into the
 * package's own emulation (its entry points +0x08 and +0x10), as isp.doc says when it is sufficient */
    .balign 4
i_top:
    .long   r_chk - i_top, r_divzero - i_top, r_trace - i_top, r_access - i_top
    .long   r_done - i_top, i_top + 0x80 + 0x08 - i_top, i_top + 0x80 + 0x10 - i_top, r_lock - i_top
    .long   r_unlock - i_top, 0, 0, 0
    .long   0, 0, 0, 0
    .long   m_read - i_top, m_read - i_top, m_write - i_top, m_rw - i_top
    .long   m_rl - i_top, m_rb - i_top, m_rw - i_top, m_rl - i_top
    .long   m_wb - i_top, m_ww - i_top, m_wl - i_top, 0
    .long   0, 0, 0, 0
#include "msp/060/isp.sa"
