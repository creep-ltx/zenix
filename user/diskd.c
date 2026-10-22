#include "libz.h"
#include "../kernel/diskproto.h"
#include "rdb.h"
typedef unsigned char u8;

/* The disk server: owns the raw disk (IDE or SD card, through the kernel's raw-sector calls,
 * which only this process may use), reads its partition table - an Amiga RDB, else a PC MBR -
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

static u32 rd_base;                     /* first sector of the table being read (0, or an MBR partition) */
static int disk_rd(unsigned lba, void *buf) { return sys_blk_read(rd_base + lba, 1, buf); }

static void part_name(char *name, int n)
{
    strcpy(name, "ide0p");
    if (n >= 10) { name[5] = (char)('0' + n / 10); name[6] = (char)('0' + n % 10); name[7] = 0; }
    else { name[5] = (char)('0' + n); name[6] = 0; }
}

/* the partitions of an RDB that starts at sector "base"; returns 1 if there was one */
static int add_rdb(u32 base, int *count)
{
    struct rdb r;
    rd_base = base;
    int rc = rdb_read(disk_rd, &r);
    rd_base = 0;
    if (rc != 1) return 0;
    for (int i = 0; i < r.nparts; i++) {
        char name[16];
        part_name(name, ++*count);
        u32 cyl = r.part[i].heads * r.part[i].secs;          /* sectors per cylinder, as the table says */
        u32 start = base + r.part[i].lowcyl * cyl;
        add(name, r.part[i].label, start, (r.part[i].highcyl - r.part[i].lowcyl + 1) * cyl,
            r.part[i].dostype, r.part[i].bootable);
    }
    return 1;
}

#define DT_RDB_IN_MBR 0x52444200u       /* 'RDB\0': MBR type 0x76, an Amiga RDB inside (Emu68's convention) */
#define DT_MBR        0x4D425200u       /* 'MBR' + type byte: a partition of a kind we do not use */
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* A PC partition table (MBR) at sector 0: four primary entries. Type 0x83 (Linux) is ext2 for us; type 0x76
 * holds an Amiga RDB of its own (the partitions inside are listed too); the rest (FAT boot partitions, ...) are
 * listed but never mounted. Returns 1 if sector 0 really is a partition table: every entry has a plausible
 * status and lies inside the disk (a bare FAT volume also ends in 55 AA but has boot code where the table is). */
static int add_mbr(int *count)
{
    static u8 sec[512];
    if (disk_rd(0, sec) || sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    u32 st[4], len[4], ty[4], used = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = sec + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) return 0;
        ty[i] = e[4]; st[i] = le32(e + 8); len[i] = le32(e + 12);
        if (ty[i] == 0 || len[i] == 0) continue;
        if (st[i] == 0 || st[i] + len[i] < st[i] || st[i] + len[i] > disk_sectors) return 0;
        used++;
    }
    if (!used) return 0;
    for (int i = 0; i < 4; i++) {
        if (ty[i] == 0 || len[i] == 0) continue;
        char name[16];
        part_name(name, ++*count);
        u32 dt = ty[i] == 0x83 ? DT_EXT2 : ty[i] == 0x76 ? DT_RDB_IN_MBR : DT_MBR | ty[i];
        add(name, "", st[i], len[i], dt, sec[446 + 16 * i] == 0x80);
        if (ty[i] == 0x76) add_rdb(st[i], count);
    }
    return 1;
}

static void scan(void)
{
    u32 keep[MAXDEV];
    for (int i = 0; i < ndev; i++) keep[i] = dev[i].mounted;
    int oldn = ndev;
    ndev = 0;
    if (disk_sectors) add("ide0", "", 0, disk_sectors, 0, 0);
    if (ram_sectors) add("ram0", "", 0, ram_sectors, 0, 0);
    int count = 0;
    if (disk_sectors && !add_rdb(0, &count)) add_mbr(&count);
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
