// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
int main(void) { char b[1]; printf("%s\n", pw_name((u32)sys_geteuid(), b)); return 0; }
