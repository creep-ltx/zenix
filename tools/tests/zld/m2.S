/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): the other half of m0 (not the representative section) */
	.section .text.f,"ax"
	.globl f
f:	lea t1,%a0
	lea t2+1,%a0
	lea tend,%a0
	rts
	.section .rodata.str1.1,"aMS",@progbits,1
t1:	.string "abc"
t2:	.string "world"
	.string "xyz"
tend:
	.section .text.main2,"ax"
