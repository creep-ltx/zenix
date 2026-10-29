/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: a raw copy, read and write only (tools/libc-size.sh) */
#include <unistd.h>
int main(void) { char b[512]; ssize_t n; while ((n = read(0, b, sizeof b)) > 0) write(1, b, (size_t)n); return 0; }
