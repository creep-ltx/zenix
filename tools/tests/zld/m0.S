/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): merged strings: a tail merged into a longer string, sym+addend, a label at the end */
	.section .text.main,"ax"
	.globl main
main:	jsr f
	lea s2+3,%a0
	lea endl,%a1
	rts
	.section .rodata.str1.1,"aMS",@progbits,1
s1:	.string "hello world"
s2:	.string "world"
endl:
