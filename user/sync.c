/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* sync: flush everything to disk (writes are write-through today; kept so scripts and habits work) */
int main(void) { return sys_sync() < 0 ? 1 : 0; }
