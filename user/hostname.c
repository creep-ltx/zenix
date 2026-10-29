/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
int main(void) { const char *u = getenv("HOSTNAME"); dprintf(1, "%s\n", u ? u : "zenix"); return 0; }
