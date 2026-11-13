/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): an empty, referenced, 64-aligned section */
	.section .text.main,"ax"
	.globl main
main:	lea e,%a0
	rts
	.section .rodata.e,"a"
	.balign 64
e:
