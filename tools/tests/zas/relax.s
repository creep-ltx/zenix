/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test (tools/zas-host-test.sh): gas's sizes for branches and PC-relative operands, near and far, across
 * sections and alignments; zas must give the same object */
	.text
	.globl	start
start:	jra	near1
	jbsr	near1
	jeq	far1
	jne	ext_fn
	jbsr	ext_fn
	bra	near1
	bsr.w	near1
	bra.s	start
	beq.l	far1
	dbra	%d0,start
	dbne	%d1,far1
	dbeq	%d2,ext_fn
	fbeq	near1
	fbne.l	far1
	lea	near1(%pc),%a0
	lea	far1(%pc),%a0
	lea	ext_dat(%pc),%a0
	move.l	(near1,%pc,%d0.w*2),%d1
	move.l	(mid1,%pc,%d0.l),%d1
	move.l	(far1,%pc,%d0.l),%d1
	move.l	near1,%d0
	move.l	far1,%d0
	move.l	%d0,near1
	jsr	near1
	jsr	far1
	jmp	ext_fn
	pea	near1
	lea	other,%a1
	jra	next
next:	jra	2f
	nop
2:	jra	1f
	.balignw 4,0x4e71
1:	jra	3f
	.balignw 2,0x284c
3:	bra	1b
	.p2align 3
near1:	rts
	.skip	200
mid1:	nop
	.skip	40000
far1:	rts
	jra	start
	jbsr	start
	lea	start(%pc),%a0
	move.l	start,%d1
	.set	FWD, 6
	.set	BACK, 8
	move.l	FWD(%a0),%d0
	move.l	BACK(%a0),%d0
	.section .text.other,"ax",@progbits
other:	jra	start
	jbsr	start
	bra	start
	lea	start,%a0
	lea	start(%pc),%a0
	rts
