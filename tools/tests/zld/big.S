/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): a program past 4 GB by alignment (must be refused) */
	.section .text.main,"ax"
	.globl main
main:	lea b1,%a0
	lea b2,%a0
	rts
	.section .bss.b1,"aw",@nobits
b1:	.skip 0xffffff6d
	.section .bss.b2,"aw",@nobits
	.balign 16
b2:	.skip 0x100
