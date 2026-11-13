/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): calls fa and fb from an archive with two members of one long name (dup1, dup2) */
	.section .text.main,"ax"
	.globl main
main:	bsr.l fb
	bsr.l fa
	rts
