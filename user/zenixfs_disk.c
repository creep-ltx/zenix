/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "zenixfs_disk.h"

static int dport = -1, handle = -1;
static unsigned nsect, dtype, shift_, caps;

int zdisk_open(const char *name, unsigned flags)
{
    struct umsg m;
    struct diskinfo di;
    if (dport < 0 && (dport = sys_port_find("disk")) < 0) return -1;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = flags;
    handle = sys_call(dport, &m);
    if (handle < 0) return -1;
    m.type = DISK_INFO; m.a[0] = (u32)handle; m.a[1] = (u32)&di;
    if (sys_call(dport, &m) < 0) { handle = -1; return -1; }
    nsect = di.sectors; dtype = di.dostype; caps = di.caps;
    return 0;
}

int zdisk_probe(const char *name)
{
    static unsigned char sec[512];
    struct umsg m;
    if (dport < 0 && (dport = sys_port_find("disk")) < 0) return 0;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = 0;
    int h = sys_call(dport, &m);
    if (h < 0) return 0;
    m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = 0; m.a[2] = 1; m.a[3] = (u32)sec;
    if (sys_call(dport, &m) < 0) return 0;
    return sec[4] == 0x5A && sec[5] == 0x42 && sec[6] == 1 && sec[32] == 'Z' && sec[33] == 'N' && sec[34] == 'F' && sec[35] == 'S';
}

void zdisk_close(void)
{
    struct umsg m;
    if (handle < 0) return;
    m.type = DISK_UNMOUNT; m.a[0] = (u32)handle;
    sys_call(dport, &m);                               /* refused if it was a raw handle; harmless */
    handle = -1;
}

unsigned zdisk_sectors(void) { return nsect; }
int zdisk_can_flush(void) { return (caps & (DISKCAP_FLUSH | DISKCAP_RAM)) != 0; }
unsigned zdisk_dostype(void) { return dtype; }

static int zd_rw(uint32_t op, uint32_t blk, void *buf)
{
    struct umsg m;
    unsigned spb = 1u << (shift_ - 9);
    m.type = op; m.a[0] = (u32)handle; m.a[1] = blk * spb; m.a[2] = spb; m.a[3] = (u32)buf;
    return sys_call(dport, &m) < 0 ? -1 : 0;
}
static int zd_read(void *ctx, uint32_t blk, void *buf) { (void)ctx; return zd_rw(DISK_READ, blk, buf); }
static int zd_write(void *ctx, uint32_t blk, const void *buf) { (void)ctx; return zd_rw(DISK_WRITE, blk, (void *)buf); }
static int zd_flush(void *ctx)
{
    struct umsg m;
    (void)ctx;
    m.type = DISK_FLUSH; m.a[0] = (u32)handle;
    int r = sys_call(dport, &m);
    return r == -95 ? 0 : r < 0 ? -1 : 0;           /* -95: the disk cannot flush its cache (mount.zenixfs said so) */
}

void zdisk_dev(struct zenixfs_dev *dev, unsigned bshift)
{
    shift_ = bshift;
    dev->ctx = 0; dev->read = zd_read; dev->write = zd_write; dev->flush = zd_flush;
    dev->bshift = bshift; dev->nblocks = nsect >> (bshift - 9);
}
