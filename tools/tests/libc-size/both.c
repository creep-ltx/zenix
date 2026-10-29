/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: a program that links both oputs and stdout (tools/libc-size.sh must warn about it) */
#include <stdio.h>
#include <zenix.h>
int main(void) { oputs("one\n"); printf("two\n"); return 0; }
