/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): ZEX2: a writable section in the shared part (must be refused) */
	.section .text.main,"ax"
	.globl main
main:	move.l (cnt@GOT.w,%a5),%a0
	addq.l #1,(%a0)
	rts
	.section .rodata.cnt,"aw"
cnt:	.long 0
