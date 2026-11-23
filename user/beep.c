/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* beep: ring the terminal bell (Paula plays a short tone) */
int main(int argc, char **argv)
{
    znoargs(argc, argv, "usage: beep\n");
    sys_write(1, "\a", 1);
    return 0;
}
