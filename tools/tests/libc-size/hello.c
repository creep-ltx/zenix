/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: hello with printf and stdout's FILE stream */
#include <stdio.h>
#include <unistd.h>
int main(void) { printf("hello %d %s\n", getpid(), "world"); return 0; }
