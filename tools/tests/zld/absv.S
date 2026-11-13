/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): a plain program's R_68K_32 to absolute symbols (--defsym, .set): written as the
 * numbers, not relocated, by zld and by tools/mkzexe.py alike */
	.section .text.main,"ax"
	.globl main
main:	lea vals,%a0
	rts
	.section .data.vals,"aw"
vals:	.long hwreg, ABSV, main
	.set ABSV, 0x1234
