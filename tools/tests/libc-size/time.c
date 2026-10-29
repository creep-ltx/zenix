/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: the time functions (time, localtime with TZ, strftime) */
#include <stdio.h>
#include <time.h>
int main(void) { char b[64]; time_t t = time(0); strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S %Z", localtime(&t)); printf("%s\n", b); return 0; }
