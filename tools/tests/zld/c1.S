/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): merged constants (.rodata.cst4/cst8, with c2) and a merged string with an addend */
	.section .text.main,"ax"
	.globl main
main:	lea k1,%a0
	lea k2,%a0
	lea k3+2,%a0
	lea s1,%a0
	lea s2,%a0
	lea q1,%a0
	bsr.l f9
	rts
	.section .rodata.cst4,"aM",@progbits,4
	.balign 4
k1:	.long 0x11223344
k2:	.long 0x55667788
k3:	.long 0x11223344
	.section .rodata.cst8,"aM",@progbits,8
	.balign 8
	.quad 5
	.section .rodata.str1.4,"aMS",@progbits,1
	.balign 4
s1:	.string "abcdef"
s2:	.string "def"
	.string "ef"
	.section .rodata.str2.2,"aMS",@progbits,2
	.balign 2
q1:	.short 65,66,67,0
	.short 66,67,0
	.section .init_array.00200,"aw"
	.long main
	.section .init_array.00100,"aw"
	.long main
	.section .fini_array.00300,"aw"
	.long main
