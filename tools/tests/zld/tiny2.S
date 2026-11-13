/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): the same for ZEX2 */
	.section .text.start,"ax"
	.globl _start
_start:	bra.w _start
	.ascii "ZEX2"
