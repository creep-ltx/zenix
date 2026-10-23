/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv)
{
    char b[256]; int rc = 0;
    for (int i = 1; i < argc; i++) { int n = sys_readlink(argv[i], b, sizeof b - 1); if (n < 0) { rc = 1; continue; } b[n] = 0; printf("%s\n", b); }
    return rc;
}
