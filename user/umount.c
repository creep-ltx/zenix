/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

static const char USAGE[] = "usage: umount DIR\n";
/* umount DIR   sync the filesystem mounted on DIR, stop its server and let go of the device */
int main(int argc, char **argv)
{
    zoperands(&argc, &argv, USAGE);
    if (argc != 2) zusage(USAGE);
    int r = sys_umount(argv[1]);
    if (r == -EBUSY) { dprintf(2, "umount: %s is busy (a program has it as its working directory or has a file open there)\n", argv[1]); return 1; }
    if (r == -EINVAL) { dprintf(2, "umount: %s is not a mount point\n", argv[1]); return 1; }
    if (r < 0) { dprintf(2, "umount: %s: %s\n", argv[1], zwhy("cannot unmount")); return 1; }
    return 0;
}
