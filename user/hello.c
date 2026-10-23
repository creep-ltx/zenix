/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 3;
    for (int i = 0; i < n; i++) {
        printf("hello from pid %d, round %d\n", sys_getpid(), i);
        sys_sleep(20);
    }
    return 7;
}
