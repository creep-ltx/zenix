/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"

/* dd if=SRC of=DST [bs=N] [count=N] [skip=N] [seek=N]: copy blocks. SRC and DST are files, or disks and partitions
 * of the disk server (df0, ide0, ide0p1, ...; a name without a slash that the disk server knows). On a disk the
 * block size must be a multiple of 512. */

static int port;
static u32 bs = 512;

struct ep { int dev, h, fd; u32 sectors; };

static int open_ep(struct ep *e, const char *name, int write, u32 seek)
{
    e->dev = 0; e->h = e->fd = -1;
    if (!strchr(name, '/') && port >= 0) {
        struct umsg m;
        m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = DISK_RAW;
        int h = sys_call(port, &m);
        if (h >= 0) {
            struct diskinfo di;
            m.type = DISK_INFOSZ; m.a[0] = (u32)h; m.a[1] = (u32)&di; m.a[2] = sizeof di;
            if (sys_call(port, &m) < 0) return -1;
            e->dev = 1; e->h = h; e->sectors = di.sectors;
            if (bs % 512) { dprintf(2, "dd: the block size for a disk must be a multiple of 512\n"); return -1; }
            return 0;
        }
    }
    e->fd = write ? sys_open(name, O_WRONLY | O_CREAT | (seek ? 0 : O_TRUNC)) : sys_open(name, O_RDONLY);
    if (e->fd < 0) { dprintf(2, "dd: cannot open %s\n", name); return -1; }
    return 0;
}

/* read or write n bytes at block number blk; returns the bytes done (0 at the end) or -1 */
static int xfer(struct ep *e, int write, u32 blk, void *buf, u32 n)
{
    if (e->dev) {
        u32 sec = blk * (bs / 512), cnt = n / 512;
        if (sec >= e->sectors) return 0;
        if (sec + cnt > e->sectors) cnt = e->sectors - sec;
        struct umsg m;
        m.type = write ? DISK_WRITE : DISK_READ; m.a[0] = (u32)e->h; m.a[1] = sec; m.a[2] = cnt; m.a[3] = (u32)buf;
        if (sys_call(port, &m) < 0) return -1;
        return (int)(cnt * 512);
    }
    return write ? sys_write(e->fd, buf, n) : sys_read(e->fd, buf, n);
}

static const char USAGE[] = "usage: dd if=SRC of=DST [bs=N] [count=N] [skip=N] [seek=N]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *in = 0, *out = 0;
    u32 count = 0xFFFFFFFFu, skip = 0, seek = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "if=", 3)) in = argv[i] + 3;
        else if (!strncmp(argv[i], "of=", 3)) out = argv[i] + 3;
        else if (!strncmp(argv[i], "bs=", 3)) bs = (u32)atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "count=", 6)) count = (u32)atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "skip=", 5)) skip = (u32)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "seek=", 5)) seek = (u32)atoi(argv[i] + 5);
        else if (argv[i][0] == '-') zbadopt(argv[i], USAGE);
        else zusage(USAGE);
    }
    if (!in || !out || bs == 0 || bs > 65536) zusage(USAGE);
    port = sys_port_find("disk");
    struct ep a, b;
    if (open_ep(&a, in, 0, 0) || open_ep(&b, out, 1, seek)) return 1;
    static unsigned char buf[65536];
    if (!a.dev && skip) sys_lseek(a.fd, (int)(skip * bs), 0);
    if (!b.dev && seek) sys_lseek(b.fd, (int)(seek * bs), 0);
    u32 recs = 0, partial = 0;
    while (recs + partial < count) {
        int n = xfer(&a, 0, skip + recs + partial, buf, bs);
        if (n < 0) { dprintf(2, "dd: read error\n"); return 1; }
        if (n == 0) break;
        int w = xfer(&b, 1, seek + recs + partial, buf, (u32)n);
        if (w != n) { dprintf(2, "dd: write error\n"); return 1; }
        if ((u32)n == bs) recs++; else partial++;
    }
    dprintf(2, "%u+%u records in\n%u+%u records out\n", recs, partial, recs, partial);   /* on stderr, as GNU dd */
    return 0;
}
