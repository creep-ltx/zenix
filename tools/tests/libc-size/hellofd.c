/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: hello without streams, C's formatter straight to the descriptor (Zenix's own programs' way) */
#include <stdio.h>
#include <unistd.h>
int main(void) { dprintf(1, "hello %d %s\n", getpid(), "world"); return 0; }
