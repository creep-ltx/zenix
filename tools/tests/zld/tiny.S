/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): a crt0 whose .text.start is shorter than the ZEXE header (must be refused) */
	.section .text.start,"ax"
	.globl _start
_start:	bra.w _start
	.ascii "ZEXE"
