/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): ZEX2: an absolute address in the shared part (must be refused) */
	.section .text.main,"ax"
	.globl main
main:	lea d1,%a0
	rts
	.section .data.d1,"aw"
d1:	.long 1
