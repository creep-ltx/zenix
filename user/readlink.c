/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* the whole target, however long (audit F15: a link made elsewhere may hold more than 255 bytes); ext2 keeps a
 * target in one block, so 4 KB holds any */
#define TMAX 4096
int main(int argc, char **argv)
{
    char *b = malloc(TMAX + 1); int rc = 0;
    if (!b) { dprintf(2, "readlink: out of memory\n"); return 1; }
    for (int i = 1; i < argc; i++) {
        int n = sys_readlink(argv[i], b, TMAX);
        if (n < 0) { rc = 1; continue; }
        if (n == TMAX) { dprintf(2, "readlink: %s: target longer than %d bytes\n", argv[i], TMAX - 1); rc = 1; continue; }
        b[n] = 0; dprintf(1, "%s\n", b);
    }
    return rc;
}
