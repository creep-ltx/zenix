#include "libz.h"
#include "../kernel/diskproto.h"
#include "rdb.h"
typedef unsigned char u8;

/* The disk server: owns the IDE disk (through the kernel's raw-sector calls,
 * which only this process may use), reads the Amiga partition table (RDB)
 * and serves the whole disk and each partition as a named device.
 * Rules: a device a filesystem server has mounted can only be written by that
 * server; tools open devices with DISK_RAW, which is refused while mounted;
 * writes through the whole-disk handle may not touch a mounted partition. */

#define MAXDEV 14
static struct diskinfo dev[MAXDEV];
static int ndev;
static u32 disk_sectors;
static u8 *ram_base;                    /* the boot ramdisk, if any */
static u32 ram_sectors;

static void add(const char *name, const char *label, u32 start, u32 sectors, u32 dostype, u32 flags)
{
    if (ndev == MAXDEV) return;
    struct diskinfo *d = &dev[ndev++];
    memset(d, 0, sizeof *d);
    strcpy(d->name, name);
    strcpy(d->label, label);
    d->start = start; d->sectors = sectors; d->dostype = dostype; d->flags = flags;
}

static int disk_rd(unsigned lba, void *buf) { return sys_blk_read(lba, 1, buf); }

static void scan(void)
{
    struct rdb r;
    u32 keep[MAXDEV];
    for (int i = 0; i < ndev; i++) keep[i] = dev[i].mounted;
    int oldn = ndev;
    ndev = 0;
    if (disk_sectors) add("ide0", "", 0, disk_sectors, 0, 0);
    if (ram_sectors) add("ram0", "", 0, ram_sectors, 0, 0);
    if (disk_sectors && rdb_read(disk_rd, &r) == 1)
        for (int i = 0; i < r.nparts; i++) {
            char name[16] = "ide0p";
            name[5] = (char)('1' + i); name[6] = 0;
            u32 cyl = r.part[i].heads * r.part[i].secs;          /* sectors per cylinder, as the table says */
            u32 start = r.part[i].lowcyl * cyl;
            add(name, r.part[i].label, start, (r.part[i].highcyl - r.part[i].lowcyl + 1) * cyl,
                r.part[i].dostype, r.part[i].bootable);
        }
    for (int i = 0; i < ndev && i < oldn; i++) dev[i].mounted = keep[i];   /* mounts survive a rescan */
}

static int find(const char *name)
{
    for (int i = 0; i < ndev; i++)
        if (!strcmp(dev[i].name, name)) return i;
    return -1;
}

/* does [lba, lba+n) on the whole disk touch a mounted partition? */
static int hits_mounted(u32 lba, u32 n)
{
    for (int i = 1; i < ndev; i++)
        if (dev[i].mounted && strcmp(dev[i].name, "ram0") &&
            lba < dev[i].start + dev[i].sectors && lba + n > dev[i].start)
            return 1;
    return 0;
}

static int handle(struct umsg *m)
{
    switch (m->type) {
    case DISK_OPEN: {
        int h = find((const char *)m->a[0]);
        if (h < 0) return -1;
        if (m->a[1] & DISK_MOUNT) {
            if (dev[h].mounted) return -1;
            dev[h].mounted = (u32)m->from;
        } else if ((m->a[1] & DISK_RAW) && h > 0 && dev[h].mounted) {
            return -1;                                   /* tools stay off mounted devices */
        }
        return h;
    }
    case DISK_UNMOUNT:
        if (m->a[0] < (u32)ndev && dev[m->a[0]].mounted == (u32)m->from) { dev[m->a[0]].mounted = 0; return 0; }
        return -1;
    case DISK_READ: case DISK_WRITE: {
        u32 h = m->a[0], lba = m->a[1], n = m->a[2];
        if (h >= (u32)ndev || n == 0 || lba + n < lba || lba + n > dev[h].sectors) return -1;
        if (m->type == DISK_WRITE) {
            if (dev[h].mounted && dev[h].mounted != (u32)m->from) return -1;
            if (h == 0 && !strcmp(dev[0].name, "ide0") && hits_mounted(lba, n)) return -1;
        }
        if (!strcmp(dev[h].name, "ram0")) {              /* plain memory */
            if (m->type == DISK_READ) memcpy((void *)m->a[3], ram_base + lba * 512, n * 512);
            else memcpy(ram_base + lba * 512, (const void *)m->a[3], n * 512);
            return 0;
        }
        lba += dev[h].start;
        return m->type == DISK_READ ? sys_blk_read(lba, n, (void *)m->a[3])
                                    : sys_blk_write(lba, n, (const void *)m->a[3]);
    }
    case DISK_INFO: case DISK_LIST:
        if (m->a[0] >= (u32)ndev) return -1;
        memcpy((void *)m->a[1], &dev[m->a[0]], sizeof(struct diskinfo));
        return 0;
    case DISK_RESCAN:
        scan();
        return ndev;
    }
    return -1;
}

int main(void)
{
    int n = sys_blk_size();
    disk_sectors = n > 0 ? (u32)n : 0;
    u32 ram[2];
    if (sys_blk_ram(ram) == 0 && ram[1]) { ram_base = (u8 *)ram[0]; ram_sectors = ram[1] / 512; }
    if (!disk_sectors && !ram_sectors) { printf("diskd: no disk\n"); return 1; }
    scan();
    int port = sys_port_create("disk");
    if (port < 0) return 1;
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1)
        m = sys_replyrecv(m, handle(m), port);
    return 1;
}
