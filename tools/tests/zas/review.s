/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test (tools/zas-host-test.sh): the review's cases (2026-11-13), GNU as's object: a branch over .skip and
 * .fill, a .set counter in .rept, an alias of an undefined symbol, .exitm inside .if, .lcomm after .bss, jsr
 * through the PLT, an empty .file name, .previous after .popsection */
	jbra L
	.skip 2
L:
	jbsr M
	.space 124
M:	nop
	.set i,0
	.rept 3
	.long i
	.set i,i+1
	.endr
	x = y + 4
	.long x
	.macro mm
	.if 1
	.exitm
	.endif
	.endm
	mm
	.lcomm b1,3
	.lcomm b2,9
	.bss
	.long 0
	.text
	jsr f@PLTPC
	.file ""
	jbra N
	.fill 2,1,0
N:	nop
	.section .text.a,"ax"
	nop
	.section .text.b,"ax"
	.pushsection .text.c,"ax"
	nop
	.popsection
	.previous
	nop
