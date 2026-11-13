/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): ZEX2: a PC-relative reference from code to data (must be refused) */
	.section .text.main,"ax"
	.globl main
main:	lea (d2,%pc),%a0
	rts
	.section .data.d2,"aw"
	.globl d2
d2:	.long 2
