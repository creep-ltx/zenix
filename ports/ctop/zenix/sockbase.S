/* SPDX-License-Identifier: GPL-2.0-only */
/* bsdsocket.library's base for ctop on Zenix (ports/ctop/README): ctop calls the library itself, through a6 and the
 * call's offset below the base (SocketBaseTagList -294, ReleaseInterfaceList -456, ObtainInterfaceList -462,
 * QueryInterfaceTagList -468), its arguments in a0 and a1. The table lives in the code (built, never written at run
 * time: nothing to flush) and every entry branches to a stub that hands the registers to cu.c's C function
 * (d0, d1, a0 and a1 are the callee's to change, as on AmigaOS; a5, the shared code's data, is left alone). Every
 * other offset answers -1. The base itself is a struct Library: version 4 (ctop asks for 4 or more). */
    .text
    .even
    .globl zenix_sockbase
sock_tab:                       /* entry k is at the base - (78 - k) * 6: bra.w and a nop, 6 bytes each */
    bra.w   sk_qitl                 /* -468 QueryInterfaceTagList */
    nop
    bra.w   sk_oil                  /* -462 ObtainInterfaceList */
    nop
    bra.w   sk_ril                  /* -456 ReleaseInterfaceList */
    nop
    .rept 26
    bra.w   sk_bad
    nop
    .endr
    bra.w   sk_sbtl                 /* -294 SocketBaseTagList */
    nop
    .rept 48
    bra.w   sk_bad
    nop
    .endr
zenix_sockbase:
    .word   4, 0
    .even
sk_bad:
    jmp     zenix_sockbad(%pc)
sk_sbtl:
    move.l  %a0,-(%sp)
    jsr     zenix_sbtags(%pc)
    addq.l  #4,%sp
    rts
sk_ril:
    move.l  %a0,-(%sp)
    jsr     zenix_releaseifs(%pc)
    addq.l  #4,%sp
    rts
sk_oil:
    jmp     zenix_obtainifs(%pc)
sk_qitl:
    move.l  %a1,-(%sp)
    move.l  %a0,-(%sp)
    jsr     zenix_queryif(%pc)
    addq.l  #8,%sp
    rts
