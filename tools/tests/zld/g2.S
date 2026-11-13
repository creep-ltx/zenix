/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): ZEX2: GOT8O, GOT16O and GOT32O entries for globals and locals (with g3) */
	.section .text.main,"ax"
	.globl main
main:	move.l (v1@GOT.b,%a5,%d0.l),%a0
	move.l (v2@GOT.w,%a5),%a0
	move.l (v3@GOT.l,%a5),%a0
	move.l (lv@GOT.b,%a5,%d0.l),%a0
	move.l (v2@GOT.b,%a5,%d0.l),%a0
	move.l (v4@GOT.w,%a5),%a0
	bsr.l f2
	rts
	.section .data.v,"aw"
	.globl v1, v2, v3
v1:	.long 1
v2:	.long 2
v3:	.long 3
lv:	.long 4
v4:	.long 5
