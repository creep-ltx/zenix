/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(int argc, char **argv) { znoargs(argc, argv, "usage: clear\n"); dprintf(1, "\033[2J\033[H"); return 0; }
