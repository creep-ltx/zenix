/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): an empty .rodata section aligned to 64 */
	.section .text.main,"ax"
	.globl main
main:	moveq #0,%d0
	rts
	.section .rodata.empty,"a"
	.balign 64
	.section .data.x,"aw"
	.globl xx
xx:	.long 5
