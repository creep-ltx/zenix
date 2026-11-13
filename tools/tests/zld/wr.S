/* SPDX-License-Identifier: GPL-2.0-only */
/* zld test (tools/zld-host-test.sh): --wrap=foo: foo goes to __wrap_foo, __real_foo to foo */
	.section .text.main,"ax"
	.globl main
main:	bsr.l foo
	rts
	.section .text.w,"ax"
	.globl __wrap_foo
__wrap_foo: bsr.l __real_foo
	rts
	.section .text.foo,"ax"
	.globl foo
foo:	rts
