/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* sleep SECONDS (whole seconds; a fraction like 0.5 is rounded to hundredths) */
int main(int argc, char **argv)
{
    if (argc < 2) { dprintf(2, "usage: sleep seconds\n"); return 2; }
    u32 s = (u32)strtoul(argv[1], 0, 10), frac = 0; const char *d = strchr(argv[1], '.');
    if (d) { d++; for (int k = 0; k < 2; k++) frac = frac * 10 + (isdigit((unsigned char)*d) ? (u32)(*d++ - '0') : 0); }
    sys_sleep(s * 100 + frac);
    return 0;
}
