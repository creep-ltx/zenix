/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): the other half of g2, the same symbols in other classes */
	.section .text.f2,"ax"
	.globl f2
f2:	move.l (v3@GOT.b,%a5,%d0.l),%a0
	move.l (v1@GOT.l,%a5),%a0
	move.l (w@GOT.w,%a5),%a0
	move.l (lw@GOT.b,%a5,%d0.l),%a0
	rts
	.section .data.w,"aw"
	.globl w
w:	.long 1
lw:	.long 1
