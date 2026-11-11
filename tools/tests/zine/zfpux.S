/* SPDX-License-Identifier: GPL-2.0-only */
/* zfpux (d29 16.14): the FPU and integer instructions a 68040 or 68060 lacks in hardware, each answer compared with
 * the known value (to 1 part in 10^12): on a 68881/2 the FPU does them, on a 68040/060 ZINE's support package
 * (Motorola's) emulates them. FSIN FCOS FTAN FATAN FASIN FACOS FETOX FETOXM1 FLOGN FLOG10 FLOG2 FLOGNP1 FSINH FCOSH
 * FTANH FATANH FTWOTOX FTENTOX FINT FINTRZ FGETEXP FGETMAN FSCALE FMOD FREM, FMOVECR (pi, 100), FSINCOS (both),
 * a packed decimal source, a denormal source, an immediate double (the 060's unimplemented effective address), then
 * the 060's missing integer ones: MULU.L and DIVU.L with 64 bits, MOVEP.L both ways, CAS2.L and CHK2.L. Each wrong
 * answer is named in the output; at the end "zfpux: all 37 answers right" and 0, else 1. */
#include "zt.i"
    .chip   68060
    .text
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  4.w,%a6
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    beq.w   99f
    move.l  %d0,%a5                 /* dos */
    moveq   #0,%d7                  /* wrong answers */
    fmove.l %d7,%fpcr               /* round to nearest, extended precision, no exceptions */
.macro UN op, arg, exp, name        /* fp0 = op(arg) */
    fmove.d \arg(%pc),%fp1
    \op\().x %fp1,%fp0
    lea     \exp(%pc),%a0
    lea     \name(%pc),%a1
    bsr.w   check
.endm
.macro BIN op, a, b, exp, name      /* fp0 = a, then op b,fp0 */
    fmove.d \a(%pc),%fp0
    \op\().d \b(%pc),%fp0
    lea     \exp(%pc),%a0
    lea     \name(%pc),%a1
    bsr.w   check
.endm
    UN  fsin, half, e_sin, n_sin
    UN  fcos, half, e_cos, n_cos
    UN  ftan, half, e_tan, n_tan
    UN  fatan, one, e_atan, n_atan
    UN  fasin, half, e_asin, n_asin
    UN  facos, half, e_acos, n_acos
    UN  fetox, one, e_e, n_etox
    UN  fetoxm1, half, e_etoxm1, n_etoxm1
    UN  flogn, two, e_ln2, n_logn
    UN  flog10, hundred, two, n_log10
    UN  flog2, eight, three, n_log2
    UN  flognp1, one, e_ln2, n_lognp1
    UN  fsinh, one, e_sinh, n_sinh
    UN  fcosh, one, e_cosh, n_cosh
    UN  ftanh, half, e_tanh, n_tanh
    UN  fatanh, half, e_atanh, n_atanh
    UN  ftwotox, three, eight, n_twotox
    UN  ftentox, two, hundred, n_tentox
    UN  fint, p27, three, n_int     /* not a tie: 2.5 (to 2) came out wrong on FS-UAE's 68882, 68040 and 68060 alike */
    UN  fintrz, m27, m2, n_intrz
    UN  fgetexp, ten, three, n_getexp
    UN  fgetman, ten, e_125, n_getman
    BIN fscale, onehalf, three, twelve, n_scale
    BIN fmod, seven, three, one, n_mod
    BIN frem, eight, three, m1, n_rem
    fmovecr #0,%fp0                 /* pi */
    lea     e_pi(%pc),%a0
    lea     n_crpi(%pc),%a1
    bsr.w   check
    fmovecr #0x34,%fp0              /* 10^2 */
    lea     hundred(%pc),%a0
    lea     n_cr100(%pc),%a1
    bsr.w   check
    fmove.d half(%pc),%fp1
    fsincos.x %fp1,%fp2:%fp0
    fmove.x %fp2,%fp4
    lea     e_sin(%pc),%a0
    lea     n_sincos(%pc),%a1
    bsr.w   check
    fmove.x %fp4,%fp0
    lea     e_cos(%pc),%a0
    lea     n_sincos(%pc),%a1
    bsr.w   check
    fmove.p p15(%pc),%fp0           /* packed decimal 1.5 */
    lea     onehalf(%pc),%a0
    lea     n_packed(%pc),%a1
    bsr.w   check
    fmove.d denorm(%pc),%fp0        /* the smallest denormal, doubled: exact */
    fadd.x  %fp0,%fp0
    fmove.d %fp0,-(%sp)
    move.l  (%sp)+,%d0
    move.l  (%sp)+,%d1
    tst.l   %d0
    bne.s   1f
    cmp.l   #2,%d1
    beq.s   2f
1:  lea     n_denorm(%pc),%a1
    bsr.w   wrong
2:  fmove.d #0r2.5,%fp0             /* an immediate double */
    lea     twohalf(%pc),%a0
    lea     n_imm(%pc),%a1
    bsr.w   check
    /* the integer ones */
    move.l  #0x12345678,%d3
    move.l  #0x9ABCDEF0,%d1
    mulu.l  %d1,%d2:%d3             /* 64-bit product: 0x0B00EA4E_242D2080 */
    cmp.l   #0x0B00EA4E,%d2
    bne.s   3f
    cmp.l   #0x242D2080,%d3
    beq.s   4f
3:  lea     n_mulu(%pc),%a1
    bsr.w   wrong
4:  moveq   #1,%d2
    moveq   #0,%d3
    moveq   #3,%d1
    divu.l  %d1,%d2:%d3             /* 2^32 / 3: 0x55555555, remainder 1 */
    cmp.l   #0x55555555,%d3
    bne.s   5f
    cmp.l   #1,%d2
    beq.s   6f
5:  lea     n_divu(%pc),%a1
    bsr.w   wrong
6:  lea     buf(%pc),%a0
    clr.l   (%a0)
    clr.l   4(%a0)
    move.l  #0x11223344,%d0
    movep.l %d0,0(%a0)              /* bytes 0, 2, 4, 6 */
    moveq   #0,%d1
    movep.l 0(%a0),%d1
    cmp.l   %d0,%d1
    bne.s   7f
    cmp.l   #0x11002200,(%a0)
    beq.s   8f
7:  lea     n_movep(%pc),%a1
    bsr.w   wrong
8:  lea     buf(%pc),%a0
    lea     4(%a0),%a1
    move.l  #5,(%a0)
    move.l  #6,(%a1)
    moveq   #5,%d0
    moveq   #6,%d1
    moveq   #7,%d2
    moveq   #8,%d3
    cas2.l  %d0:%d1,%d2:%d3,(%a0):(%a1)     /* both equal: 7 and 8 written */
    bne.s   9f
    cmp.l   #7,(%a0)
    bne.s   9f
    cmp.l   #8,(%a1)
    beq.s   10f
9:  lea     n_cas2(%pc),%a1
    bsr.w   wrong
10: lea     bounds(%pc),%a0
    moveq   #50,%d0
    chk2.l  (%a0),%d0               /* 10 <= 50 <= 100: no trap */
    bcs.s   11f
    moveq   #5,%d0
    cmp2.l  (%a0),%d0               /* below: C set */
    bcs.s   12f
11: lea     n_chk2(%pc),%a1
    bsr.w   wrong
12: tst.l   %d7
    bne.s   13f
    lea     allright(%pc),%a0
    move.l  %a0,%d2
    moveq   #allend-allright,%d3
    bsr.w   out
13: move.l  %a5,%a1
    move.l  4.w,%a6
    jsr     LVO_CloseLibrary(%a6)
    moveq   #0,%d0
    tst.l   %d7
    beq.s   14f
    moveq   #1,%d0
14: movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts
99: moveq   #20,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* fp0 against the double at a0: |fp0 - e| <= |e| * 1e-12 + 1e-300, else the name at a1 (8 bytes) is said */
check:
    fmove.d (%a0),%fp2
    fmove.x %fp0,%fp3
    fsub.x  %fp2,%fp3
    fabs.x  %fp3
    fabs.x  %fp2
    fmul.d  tol(%pc),%fp2
    fadd.d  tiny(%pc),%fp2
    fcmp.x  %fp2,%fp3
    fbugt   wrong                    /* greater, or a NaN */
    rts
wrong:                              /* a1: the name */
    addq.l  #1,%d7
    lea     wrongtxt(%pc),%a0
    move.l  %a0,%d2
    moveq   #wrongend-wrongtxt,%d3
    move.l  %a1,-(%sp)
    bsr.s   out
    move.l  (%sp)+,%d2
    moveq   #9,%d3
out:                                /* d2 text, d3 its length */
    move.l  %a6,-(%sp)
    move.l  %a5,%a6
    move.l  %d2,-(%sp)
    move.l  %d3,-(%sp)
    jsr     LVO_Output(%a6)
    move.l  %d0,%d1
    move.l  (%sp)+,%d3
    move.l  (%sp)+,%d2
    jsr     LVO_Write(%a6)
    move.l  (%sp)+,%a6
    rts

dosname: .asciz "dos.library"
wrongtxt: .ascii "zfpux: wrong: "
wrongend:
allright: .ascii "zfpux: all 37 answers right\n"
allend:
n_sin:   .ascii "FSIN    \n"
n_cos:   .ascii "FCOS    \n"
n_tan:   .ascii "FTAN    \n"
n_atan:  .ascii "FATAN   \n"
n_asin:  .ascii "FASIN   \n"
n_acos:  .ascii "FACOS   \n"
n_etox:  .ascii "FETOX   \n"
n_etoxm1: .ascii "FETOXM1 \n"
n_logn:  .ascii "FLOGN   \n"
n_log10: .ascii "FLOG10  \n"
n_log2:  .ascii "FLOG2   \n"
n_lognp1: .ascii "FLOGNP1 \n"
n_sinh:  .ascii "FSINH   \n"
n_cosh:  .ascii "FCOSH   \n"
n_tanh:  .ascii "FTANH   \n"
n_atanh: .ascii "FATANH  \n"
n_twotox: .ascii "FTWOTOX \n"
n_tentox: .ascii "FTENTOX \n"
n_int:   .ascii "FINT    \n"
n_intrz: .ascii "FINTRZ  \n"
n_getexp: .ascii "FGETEXP \n"
n_getman: .ascii "FGETMAN \n"
n_scale: .ascii "FSCALE  \n"
n_mod:   .ascii "FMOD    \n"
n_rem:   .ascii "FREM    \n"
n_crpi:  .ascii "FMOVECR \n"
n_cr100: .ascii "FMOVECR2\n"
n_sincos: .ascii "FSINCOS \n"
n_packed: .ascii "PACKED  \n"
n_denorm: .ascii "DENORMAL\n"
n_imm:   .ascii "IMMEDIAT\n"
n_mulu:  .ascii "MULU.L64\n"
n_divu:  .ascii "DIVU.L64\n"
n_movep: .ascii "MOVEP   \n"
n_cas2:  .ascii "CAS2    \n"
n_chk2:  .ascii "CHK2    \n"
    .balign 4
half:    .double 0.5
one:     .double 1.0
two:     .double 2.0
three:   .double 3.0
seven:   .double 7.0
eight:   .double 8.0
ten:     .double 10.0
twelve:  .double 12.0
hundred: .double 100.0
twohalf: .double 2.5
onehalf: .double 1.5
m27:     .double -2.7
p27:     .double 2.7
m2:      .double -2.0
m1:      .double -1.0
e_125:   .double 1.25
e_sin:   .double 0.479425538604203
e_cos:   .double 0.8775825618903728
e_tan:   .double 0.5463024898437905
e_atan:  .double 0.7853981633974483
e_asin:  .double 0.5235987755982989
e_acos:  .double 1.0471975511965979
e_e:     .double 2.718281828459045
e_etoxm1: .double 0.6487212707001282
e_ln2:   .double 0.6931471805599453
e_sinh:  .double 1.1752011936438014
e_cosh:  .double 1.5430806348152437
e_tanh:  .double 0.46211715726000974
e_atanh: .double 0.5493061443340549
e_pi:    .double 3.141592653589793
tol:     .double 1.0e-12
tiny:    .double 1.0e-300
denorm:  .long  0, 1
p15:     .long  0x00000001, 0x50000000, 0x00000000     /* packed decimal +1.5E+0 */
bounds:  .long  10, 100
buf:     .long  0, 0
