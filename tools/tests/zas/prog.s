/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test (tools/zas-host-test.sh, tools/zas-test.sh): a small program in assembly. Zenix assembles it with zas
 * and links it with zld (crt0.o, libz); it prints sums, products, a bit field, a table lookup and a string it builds,
 * and returns 3. The PC assembles it with zas and GNU as: the same object. */
	.include "progmac.i"
	.section .text.main,"ax",@progbits
	.globl	main
	.type	main, @function
main:	movem.l	%d2-%d4/%a2,-(%sp)
	/* 1 + 2 + ... + 100 with dbra */
	moveq	#0,%d0
	moveq	#100,%d1
	moveq	#99,%d2
1:	add.l	%d1,%d0
	subq.l	#1,%d1
	dbra	%d2,1b
	PRINT	fmt_sum, %d0
	/* 12345 * 6789 and the quotient and remainder of 1000000 / 7 */
	move.l	#12345,%d0
	mulu.l	#6789,%d0
	PRINT	fmt_mul, %d0
	move.l	#1000000,%d1
	divul.l	#7,%d2:%d1
	PRINT2	fmt_div, %d1, %d2
	/* a bit field: bits 4-11 of 0x12345678 */
	move.l	#0x12345678,%d3
	bfextu	%d3{20:8},%d0
	PRINT	fmt_bf, %d0
	/* a table through PC-relative index */
	moveq	#0,%d4
	lea	squares(%pc),%a2
	moveq	#SQUARES - 1,%d2
2:	add.w	(%a2)+,%d4
	dbra	%d2,2b
	PRINT	fmt_sq, %d4
	/* a string built backwards in the bss */
	lea	buf+16,%a0
	clr.b	-(%a0)
	lea	word(%pc),%a1
3:	move.b	(%a1)+,%d0
	jeq	4f
	move.b	%d0,-(%a0)
	jra	3b
4:	move.l	%a0,-(%sp)
	pea	fmt_str
	jsr	oprintf
	addq.l	#8,%sp
	moveq	#3,%d0
	movem.l	(%sp)+,%d2-%d4/%a2
	rts
	.size	main, .-main
squares:
	.irp	n, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
	.word	\n * \n
	.endr
	.set	SQUARES, (. - squares) / 2
word:	.asciz	"!saz morf olleh"
	.section .rodata.fmt,"a"
fmt_sum: .asciz	"sum %d\n"
fmt_mul: .asciz	"product %d\n"
fmt_div: .asciz	"quotient %d remainder %d\n"
fmt_bf:	.asciz	"field %x\n"
fmt_sq:	.asciz	"squares %d\n"
fmt_str: .asciz	"%s\n"
	.section .bss.buf,"aw",@nobits
buf:	.zero	16
