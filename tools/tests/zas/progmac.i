/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test: the macros of tools/tests/zas/prog.s */
	.macro	PRINT fmt, value
	move.l	\value,-(%sp)
	pea	\fmt
	jsr	oprintf
	addq.l	#8,%sp
	.endm
	.macro	PRINT2 fmt, v1, v2
	move.l	\v2,-(%sp)
	move.l	\v1,-(%sp)
	pea	\fmt
	jsr	oprintf
	lea	12(%sp),%sp
	.endm
