/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's 68040 support package (docs/d29-zine.md 16.14): Motorola's M68040 Floating Point Software Package (msp/040,
 * the GNU assembler form the Linux kernel carries, under Motorola's licence in msp/040/README) with ZINE's call-outs
 * in place of the package's sample skeleton. A real 68040's FPU lacks the transcendental and other instructions
 * AmigaOS's 68040.library emulates (FSIN, FCOS, FTAN, FETOX, FLOGN, FATAN, FSINH, FINT, FINTRZ, FMOD, FREM, FMOVECR,
 * ...), packed decimal and the denormalised and unnormalised data types. /bin/zine loads this file (a ZEXE image,
 * tools/mkzexe.py, relocated) on a 68040/060 machine and the blob points the CPU's vectors at its entry points for the
 * session when the CPU is a 68040 with an FPU (blob.c zb_sp_install).
 *
 * The header after the ZEXE one (offset 32) is zsp060.S's: "ZSPK", the CPU (40), the hooks' address, the number of
 * vector entries, then each: the vector number (bit 8: only with an FPU) and the entry's address. The call-outs below
 * follow what the package's sample skeleton does with the FPU's state (its E1 and E3 flags, the 68040 bug 1238's
 * fix for inexact) and then go to ZINE's handlers instead of an operating system's trap code. */
#include "msp/040/fpsp.h"
#define NEEDFPU 0x100

    .section .text.start,"ax"
    .globl  _start
_start:
    bra.w   _start                      /* not called: the blob takes the vectors from the header */
    .ascii  "ZEXE"
    .long   0, 0, 0, 0, 0, 0
    .ascii  "ZSPK"                      /* +32 */
    .long   40                          /* +36: the CPU */
    .long   hooks                       /* +40 */
    .long   (vend - vtab) / 8           /* +44 */
vtab:                                   /* +48: the vectors (the 68040 User's Manual's numbers) */
    .long   NEEDFPU + 11, fpsp_fline    /* line F: unimplemented FPU instructions (format 2 frames), FMOVECR */
    .long   NEEDFPU + 55, fpsp_unsupp   /* unimplemented data type */
    .long   NEEDFPU + 48, fpsp_bsun
    .long   NEEDFPU + 49, z_inex
    .long   NEEDFPU + 50, z_dz
    .long   NEEDFPU + 51, fpsp_unfl
    .long   NEEDFPU + 52, fpsp_operr
    .long   NEEDFPU + 53, fpsp_ovfl
    .long   NEEDFPU + 54, fpsp_snan
vend:
hooks:
h_fline: .long  0                       /* ZINE's line-F vector (zv_linef): MMU no-ops, else a crash */
h_crash: .long  0                       /* ZINE's crash vector (zv_crash) */

    .text
.macro HOOK h
    move.l  \h(%pc),-(%sp)
    rts
.endm

/* the exception taken off the FPU's state, then ZINE's crash handler names it: a program that enabled an FP exception
 * in FPCR has no task trap handler in ZINE (under AmigaOS the default one shows an alert) */
.macro E1ONLY
    link    %a6,#-LOCAL_SIZE
    fsave   -(%sp)
    bclrb   #E1,E_BYTE(%a6)
    frestore (%sp)+
    unlk    %a6
.endm
.macro E3E1
    link    %a6,#-LOCAL_SIZE
    fsave   -(%sp)
    bclrb   #E3,E_BYTE(%a6)
    bnes    1f
    bclrb   #E1,E_BYTE(%a6)
1:  frestore (%sp)+
    unlk    %a6
.endm

    .globl  real_dz, real_inex, real_ovfl, real_unfl, real_snan, real_operr, real_bsun, real_fline, real_trace
    .globl  fpsp_done, fpsp_fmt_error, mem_read, mem_write
z_dz:
real_dz:
    E1ONLY
    HOOK    h_crash
real_snan:
real_operr:
    E1ONLY
    HOOK    h_crash
real_ovfl:
real_unfl:
    E3E1
    HOOK    h_crash
real_bsun:
    link    %a6,#-LOCAL_SIZE
    fsave   -(%sp)
    bclrb   #E1,E_BYTE(%a6)
    fmovel  %fpsr,-(%sp)
    bclrb   #nan_bit,(%sp)
    fmovel  (%sp)+,%fpsr
    frestore (%sp)+
    unlk    %a6
    HOOK    h_crash

/* inexact: on the original 68040 mask set (frame version $40) an inexact may stand for an SNAN, overflow or
 * underflow taken with it (bug 1232): those go to their own handlers first */
z_inex:
    link    %a6,#-LOCAL_SIZE
    fsave   -(%sp)
    cmpib   #VER_40,(%sp)
    bnes    2f
    fmovel  %fpsr,-(%sp)
    btstb   #E1,E_BYTE(%a6)
    beqs    1f
    btstb   #snan_bit,2(%sp)
    beqs    3f
    addql   #4,%sp
    frestore (%sp)+
    unlk    %a6
    jmp     fpsp_snan
3:  btstb   #ovfl_bit,2(%sp)
    beqs    4f
    addql   #4,%sp
    frestore (%sp)+
    unlk    %a6
    jmp     fpsp_ovfl
4:  btstb   #unfl_bit,2(%sp)
    beqs    1f
    addql   #4,%sp
    frestore (%sp)+
    unlk    %a6
    jmp     fpsp_unfl
1:  addql   #4,%sp
    frestore (%sp)+
    unlk    %a6
real_inex:
    link    %a6,#-LOCAL_SIZE
    fsave   -(%sp)
2:  bclrb   #E3,E_BYTE(%a6)
    beqs    5f
    moveml  %d0/%d1,USER_DA(%a6)
    bfextu  CMDREG1B(%a6){#6:#3},%d0    /* the destination register: its dirty bit cleared */
    bclrb   %d0,FPR_DIRTY_BITS(%a6)
    bsrl    b1238_fix
    moveml  USER_DA(%a6),%d0/%d1
    bras    6f
5:  bclrb   #E1,E_BYTE(%a6)
6:  frestore (%sp)+
    unlk    %a6
    HOOK    h_crash

real_fline:                             /* an F-line that is no FPU instruction: ZINE's (MMU no-ops, else a crash) */
    HOOK    h_fline
real_trace:                             /* the trace vector as it is now */
    subq.l  #4,%sp
    move.l  %a0,-(%sp)
    movec   %vbr,%a0
    move.l  0x24(%a0),4(%sp)
    move.l  (%sp)+,%a0
    rts
fpsp_fmt_error:                         /* an FPU state frame the package does not know */
    HOOK    h_crash
fpsp_done:
    rte

/* a0 source, a1 destination, d0 bytes (more than 0); the MMU is off, so user and supervisor memory are the same */
mem_read:
mem_write:
1:  moveb   (%a0)+,(%a1)+
    subql   #1,%d0
    bnes    1b
    rts
