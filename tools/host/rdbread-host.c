/* SPDX-License-Identifier: GPL-2.0-only */
/* user/rdb.h as the disk server and fdisk build it (its own RDB_MAXUSED), on an image file: what diskd would say about the table.
 *   rdbread-host IMG [BASE]      prints "noedit REASON" or "noedit -", then a line per partition and the blocks owned
 * (tools/zinstall-host-test.py: a table zinstall wrote must read whole here, as AmigaOS reads it). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../../user/rdb.h"

static int fd;
static unsigned long base;
static int rd(unsigned lba, void *buf)
{
    unsigned char b[512];
    if (pread(fd, b, 512, (off_t)(base + lba) * 512) != 512) return -1;
    unsigned *l = buf;
    for (int i = 0; i < 128; i++) l[i] = (unsigned)b[i * 4] << 24 | (unsigned)b[i * 4 + 1] << 16 | (unsigned)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    return 0;
}

int main(int argc, char **argv)
{
    static struct rdb r;
    if (argc < 2) return 2;
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) { perror(argv[1]); return 2; }
    base = argc > 2 ? strtoul(argv[2], 0, 10) : 0;
    int rc = rdb_read(rd, &r);
    if (rc != 1) { printf("no rdb (%d)\n", rc); return 1; }
    printf("noedit %s\n", r.noedit ? r.noedit : "-");
    printf("rdbhi %u used %u max %u\n", r.rdbhi, r.nused, (unsigned)RDB_MAXUSED);
    for (int i = 0; i < r.nparts; i++) printf("part %s blk %u start %u sectors %u\n", r.part[i].label, r.part[i].blk, r.part[i].start, r.part[i].sectors);
    return 0;
}
