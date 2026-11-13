/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): an empty .init_array aligned to 64 */
	.section .text.main,"ax"
	.globl main
main:	moveq #0,%d0
	rts
	.section .init_array,"aw"
	.balign 64
