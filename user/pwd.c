/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(void)
{
    char b[160];                                    /* FS_PATHMAX: the longest working directory text (D26) */
    if (sys_getcwd(b, sizeof b) < 0) { fprintf(2, "pwd: %s\n", zwhy("cannot read the working directory")); return 1; }
    printf("%s\n", b);
    return 0;
}
