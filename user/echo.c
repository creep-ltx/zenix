/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* a write that fails (a full disk) says so and gives status 1, never a silent success */
static int put(const char *s, u32 n)
{
    int r = sys_write(1, s, n);
    if (r == (int)n)
        return 0;
    const char *t = r < 0 ? zwhy("write error") : "short write";
    sys_write(2, "echo: write error: ", 19); sys_write(2, t, strlen(t)); sys_write(2, "\n", 1);
    return 1;
}
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1 && put(" ", 1)) return 1;
        if (put(argv[i], strlen(argv[i]))) return 1;
    }
    return put("\n", 1);
}
