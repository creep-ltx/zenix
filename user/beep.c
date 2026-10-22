/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* beep: ring the terminal bell (Paula plays a short tone) */
int main(void)
{
    sys_write(1, "\a", 1);
    return 0;
}
