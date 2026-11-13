/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): the other half of c1: constants and strings alike, merged across objects */
	.section .text.f9,"ax"
	.globl f9
f9:	lea t1,%a0
	lea t2,%a0
	lea c2,%a0
	lea tq,%a0
	rts
	.section .rodata.cst4,"aM",@progbits,4
	.balign 4
c2:	.long 0x55667788
	.long 0x99
	.section .rodata.str1.4,"aMS",@progbits,1
	.balign 4
t1:	.string "cdef"
t2:	.string "xyzdef"
	.section .rodata.str1.1,"aMS",@progbits,1
	.string "def"
	.section .rodata.str2.2,"aMS",@progbits,2
	.balign 2
tq:	.short 67,0
	.section .init_array.00100,"aw"
	.long f9
	.section .init_array,"aw"
	.long f9
