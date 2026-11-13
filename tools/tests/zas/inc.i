/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test: an included file (tools/tests/zas/dirs.s) */
	.set	VALUE, 5
	.macro	PUSH a, b
	move.l	\a,-(%sp)
	move.l	\b,-(%sp)
	.endm
	.macro	POP a, b=%d7
	move.l	(%sp)+,\a
	move.l	(%sp)+,\b
	.endm
	.macro	TWICE insn:vararg
	\insn
	\insn
	.endm
	.macro	SUM first, rest:vararg
	moveq	#\first,%d0
	.ifnb	\rest
	SUM	\rest
	.endif
	.endm
