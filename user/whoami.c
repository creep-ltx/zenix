// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
int main(int argc, char **argv) { znoargs(argc, argv, "usage: whoami\n"); char b[1]; dprintf(1, "%s\n", pw_name((u32)sys_geteuid(), b)); return 0; }
