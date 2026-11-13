/* SPDX-License-Identifier: GPL-2.0-only */
/* zas test (tools/zas-host-test.sh): directives, symbols, expressions, macros, conditionals; zas must give gas's
 * object */
	.file	"dirs.s"
	.include "tools/tests/zas/inc.i"
	.text
	.globl	main, data1
	.type	main, @function
main:	moveq	#VALUE,%d0
	move.l	#(1 << 4) + 3,%d1
	move.l	#-(3 * 5 + 2) / 4,%d2
	move.l	#((7 % 3) << 2) ^ 0xf0,%d3
	move.l	#(5 > 3) + (2 <= 1) + (4 != 4) + (1 && 2) + (0 && 1),%d4
	move.w	#'A',%d5
	move.b	#'\n',%d6
	move.l	#~0x0f & 0xff,%d7
	move.l	#end_main - main,%d0
	lea	1f(%pc),%a0
1:	lea	1b(%pc),%a1
	bra.s	1f
	nop
1:	nop
	PUSH	%d0, %d1
	POP	%d1, %d0
	TWICE	nop
	SUM	1, 2, 3, 4
	.rept	3
	addq.l	#1,%d0
	.endr
	.irp	r, %d1, %d2, %d3
	clr.l	\r
	.endr
	.irpc	c, 123
	moveq	#\c,%d0
	.endr
	.if	VALUE > 3
	moveq	#1,%d1
	.elseif	VALUE > 1
	moveq	#2,%d1
	.else
	moveq	#3,%d1
	.endif
	.ifdef	VALUE
	nop
	.endif
	.ifndef	NOPE
	nop
	.endif
	.ifc	abc,abc
	nop
	.endif
	.ifnb	x
	nop
	.endif
	.ifeq	VALUE - 5
	nop
	.endif
	move.l	str@GOT.w(%a5),%a0
	move.l	data1@GOT(%a5),%a0
	bsr.l	puts@PLTPC
	bsr.l	local_fn@PLTPC
	jsr	local_fn
	rts
end_main:
	.size	main, .-main
	.type	local_fn, @function
local_fn:
	rts
	.size	local_fn, .-local_fn
	.section .rodata.str1.1,"aMS",@progbits,1
str:	.string	"hello\tworld\n"
str2:	.asciz	"x", "yz"
.LC9:	.string	"merged"
	.section .data.data1,"aw"
	.align	2
	.type	data1, @object
	.size	data1, 64
data1:	.long	main, local_fn + 2, .LC9, .LC9 + 3, str2 - str, 0x12345678
	.word	-1, 65535, end_main - main
	.byte	1, 255, -128, 'a'
	.8byte	0x123456789abcdef0
	.short	2
	.2byte	3
	.4byte	4
	.int	5
	.ascii	"abc" "def"
	.base64	"aGVsbG8="
	.even
	.float	1.5, -0.1
	.single	3.0e10
	.double	2.718281828459045, 1e-310, 0r6.25
	.extend	1.0, -2.5
	.zero	3
	.space	2, 0x55
	.skip	1
	.fill	2, 3, 0x11223344
	.balign	8
	.p2align 2, 0x77
	.long	ext_dat - .
	.uleb128 300, 0
	.sleb128 -129, 64
	.org	.+4
	.pushsection .data.more,"aw"
more:	.long	buf, lbuf
	.popsection
	.long	more
	.section .bss.buf,"aw",@nobits
	.align	4
buf:	.zero	100
	.lcomm	lbuf, 8
	.previous
	.weak	maybe
	.long	maybe
	.globl	hid
	.hidden	hid
	.internal intern
hid:	.long	1
intern:	.long	2
	.set	alias, main
	.set	alias2, data1 + 4
	.equ	EQ, 99
	SYM = EQ + 1
	.long	alias, alias2, SYM
	.section .init_array,"a"
	.long	main
	.section .text.unlikely,"ax"
	.incbin	"tools/tests/zas/blob.bin", 2, 5
	.ident	"zas test"
