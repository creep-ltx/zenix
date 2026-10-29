/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: hello through Zenix's buffered output (zenix.h's oputs) */
#include <zenix.h>
int main(void) { oputs("hello world\n"); return 0; }
