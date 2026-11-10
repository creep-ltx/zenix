/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the math libraries on the FPU when the machine has one (docs/d29-zine.md 16.13). bmath.c computes with
 * gcc's soft float (libgcc's __adddf3 and friends); the blob is linked with ld's --wrap for the arithmetic and the
 * conversions, so each call lands here first: with an FPU (AttnFlags AFF_68881, AFF_68882 or AFF_FPU40, zm_fpu set by
 * zb_more_init) the 68881/2's or 68040/060's own instruction does it, else libgcc's code does as before. Only the
 * instructions every FPU has (FADD, FSUB, FMUL, FDIV, FMOVE; no FINTRZ, which the 68040 lacks: FMOVE.L under
 * round-to-zero instead). FP0 is kept for the caller (a program that uses the FPU itself may hold a value there).
 * The m68k ABI as gcc uses it: arguments on the stack, a double's high long first; a double answered in d0:d1, a
 * float or an int in d0. */
    .text
    .globl  zm_fpu
.macro DF2 name, op                     /* double op double */
    .globl  __wrap___\name
__wrap___\name:
    tst.b   zm_fpu
    beq.w   __real___\name
    fmovem.x %fp0,-(%sp)
    fmove.d 16(%sp),%fp0
    \op\().d 24(%sp),%fp0
    fmove.d %fp0,-(%sp)
    move.l  (%sp)+,%d0
    move.l  (%sp)+,%d1
    fmovem.x (%sp)+,%fp0
    rts
.endm
.macro SF2 name, op                     /* float op float */
    .globl  __wrap___\name
__wrap___\name:
    tst.b   zm_fpu
    beq.w   __real___\name
    fmovem.x %fp0,-(%sp)
    fmove.s 16(%sp),%fp0
    \op\().s 20(%sp),%fp0
    fmove.s %fp0,%d0
    fmovem.x (%sp)+,%fp0
    rts
.endm
    DF2 adddf3, fadd
    DF2 subdf3, fsub
    DF2 muldf3, fmul
    DF2 divdf3, fdiv
    SF2 addsf3, fadd
    SF2 subsf3, fsub
    SF2 mulsf3, fmul
    SF2 divsf3, fdiv
/* int -> double, int -> float, float -> double, double -> float */
    .globl  __wrap___floatsidf, __wrap___floatsisf, __wrap___extendsfdf2, __wrap___truncdfsf2
__wrap___floatsidf:
    tst.b   zm_fpu
    beq.w   __real___floatsidf
    fmovem.x %fp0,-(%sp)
    fmove.l 16(%sp),%fp0
    bra.s   1f
__wrap___extendsfdf2:
    tst.b   zm_fpu
    beq.w   __real___extendsfdf2
    fmovem.x %fp0,-(%sp)
    fmove.s 16(%sp),%fp0
1:  fmove.d %fp0,-(%sp)
    move.l  (%sp)+,%d0
    move.l  (%sp)+,%d1
    fmovem.x (%sp)+,%fp0
    rts
__wrap___floatsisf:
    tst.b   zm_fpu
    beq.w   __real___floatsisf
    fmovem.x %fp0,-(%sp)
    fmove.l 16(%sp),%fp0
    bra.s   2f
__wrap___truncdfsf2:
    tst.b   zm_fpu
    beq.w   __real___truncdfsf2
    fmovem.x %fp0,-(%sp)
    fmove.d 16(%sp),%fp0
2:  fmove.s %fp0,%d0
    fmovem.x (%sp)+,%fp0
    rts
/* double -> int, float -> int: toward zero (FPCR's mode RZ for the move, the program's FPCR put back); out of range
 * the FPU answers the largest int of the sign, as libgcc's saturates */
    .globl  __wrap___fixdfsi, __wrap___fixsfsi
__wrap___fixdfsi:
    tst.b   zm_fpu
    beq.w   __real___fixdfsi
    fmovem.x %fp0,-(%sp)
    fmove.d 16(%sp),%fp0
    bra.s   3f
__wrap___fixsfsi:
    tst.b   zm_fpu
    beq.w   __real___fixsfsi
    fmovem.x %fp0,-(%sp)
    fmove.s 16(%sp),%fp0
3:  fmove.l %fpcr,%d1
    fmove.l #0x10,%fpcr             /* round toward zero, no exceptions enabled */
    fmove.l %fp0,%d0
    fmove.l %d1,%fpcr
    fmovem.x (%sp)+,%fp0
    rts

    .data
zm_fpu: .byte   0
    .even
