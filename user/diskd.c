/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* embedded in the kernel image: size first */
#ifdef ZHOST
#include "../tools/host/zhost.h"     /* host build for the tests (tools/disk-host-test.sh) */
#else
#include "libz.h"
#endif
#include "../kernel/diskproto.h"
#include "rdb.h"
#ifndef ZHOST
typedef unsigned char u8;
#endif

/* The disk server: owns the raw disk (IDE or SD card, through the kernel's raw-sector calls,
 * which only this process may use), reads its partition table - an Amiga RDB, else a PC MBR -
 * and serves the whole disk and each partition as a named device.
 * Rules (docs/man/diskd.txt):
 *  - a handle is a device's slot, and a slot keeps its device across rescans (matched by name and place on the
 *    disk), so a filesystem server's handle never points at another partition after fdisk or a new floppy;
 *  - devices that share sectors overlap: the whole disk overlaps every partition, a PC partition of type 0x76
 *    overlaps the RDB partitions inside it, and two RDB partitions may overlap on a damaged table;
 *  - DISK_MOUNT and DISK_RAW are refused while an overlapping device is mounted; a write is refused when it
 *    touches a device another process has mounted. A plain open may read anything;
 *  - a mount whose server has died is released (checked when it would block someone). */

#define MAXDEV 20
static struct diskinfo dev[MAXDEV];
static u8 used[MAXDEV];                 /* the slot holds a device */
static u8 phys[MAXDEV];                 /* which medium: 0 the disk, 1 the RAM disk, 2+N floppy dfN */
static u8 order[MAXDEV];                /* DISK_LIST order: slots in the order the last scan found them */
static int norder;
static int fd_probe = 1;                /* floppy drives looked at: bit 0 = df0; df1-3 only when a name asks for them (probing an absent drive takes a third of a second) */
static int fdrive[MAXDEV];              /* floppy drive number of a device, -1 for the others */
static u32 disk_sectors;
static u8 *ram_base;                    /* the boot ramdisk, if any */
static u32 ram_sectors;

/* the scan's result before it is merged into the slots */
static struct diskinfo cand[MAXDEV];
static int cfd[MAXDEV], ncand, dropped;
static u8 cphys[MAXDEV];

static void add(const char *name, const char *label, u32 start, u32 sectors, u32 dostype, u32 flags)
{
    if (ncand == MAXDEV) { dropped++; return; }
    cfd[ncand] = -1;
    cphys[ncand] = 0;
    struct diskinfo *d = &cand[ncand++];
    memset(d, 0, sizeof *d);
    strcpy(d->name, name);
    strncpy(d->label, label, sizeof d->label - 1);
    d->start = start; d->sectors = sectors; d->dostype = dostype; d->flags = flags;
    d->maxsect = 256;
    d->caps = DISKCAP_FLUSH;
    if (!strcmp(name, "ram0")) { d->caps = DISKCAP_RAM; cphys[ncand - 1] = 1; }
}

static u32 rd_base;                     /* first sector of the table being read (0, or an MBR partition) */
static int disk_rd(unsigned lba, void *buf) { return sys_blk_read(rd_base + lba, 1, buf); }

static void part_name(char *name, int n)
{
    strcpy(name, "ide0p");
    if (n >= 10) { name[5] = (char)('0' + n / 10); name[6] = (char)('0' + n % 10); name[7] = 0; }
    else { name[5] = (char)('0' + n); name[6] = 0; }
}

/* the partitions of an RDB that starts at sector "base" (inside "limit" sectors); returns 1 if there was one */
static int add_rdb(u32 base, u32 limit, int *count)
{
    static struct rdb r;
    rd_base = base;
    int rc = rdb_read(disk_rd, &r);
    rd_base = 0;
    if (rc != 1) return 0;
    for (int i = 0; i < r.nparts; i++) {
        char name[16];
        part_name(name, ++*count);
        const struct rdb_part *p = &r.part[i];
        if (!p->sectors || p->start + p->sectors > limit || p->start + p->sectors < p->start) {
            printf("diskd: %s (%s) has a geometry or size that does not fit the disk: not served\n", name, p->label);
            continue;
        }
        add(name, p->label, base + p->start, p->sectors, p->dostype, p->bootable);
    }
    if (r.noedit) printf("diskd: note: the partition table: %s\n", r.noedit);
    return 1;
}

#define DT_RDB_IN_MBR 0x52444200u       /* 'RDB\0': MBR type 0x76, an Amiga RDB inside (Emu68's convention) */
#define DT_MBR        0x4D425200u       /* 'MBR' + type byte: a partition of a kind we do not use */
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* A PC partition table (MBR) at sector 0: four primary entries. Type 0x83 (Linux) is ext2 for us (fsd checks the
 * filesystem's features before it uses one; ext3/ext4 are refused or read-only); type 0x76 holds an Amiga RDB of
 * its own (the partitions inside are listed too); the rest (FAT boot partitions, ...) are listed but never mounted.
 * Returns 1 if sector 0 really is a partition table: every entry has a plausible status and lies inside the disk
 * (a bare FAT volume also ends in 55 AA but has boot code where the table is). */
static int add_mbr(int *count)
{
    static u8 sec[512];
    if (disk_rd(0, sec) || sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    u32 st[4], len[4], ty[4], nused = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = sec + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) return 0;
        ty[i] = e[4]; st[i] = le32(e + 8); len[i] = le32(e + 12);
        if (ty[i] == 0 || len[i] == 0) continue;
        if (st[i] == 0 || st[i] + len[i] < st[i] || st[i] + len[i] > disk_sectors) return 0;
        nused++;
    }
    if (!nused) return 0;
    for (int i = 0; i < 4; i++) {
        if (ty[i] == 0 || len[i] == 0) continue;
        char name[16];
        part_name(name, ++*count);
        u32 dt = ty[i] == 0x83 ? DT_EXT2 : ty[i] == 0x76 ? DT_RDB_IN_MBR : DT_MBR | ty[i];
        add(name, "", st[i], len[i], dt, sec[446 + 16 * i] == 0x80);
        if (ty[i] == 0x76) add_rdb(st[i], len[i], count);
    }
    return 1;
}

/* is slot s the device the scan found as candidate c? The same sectors of the same medium: a partition keeps its
 * slot when a change before it in the table renames it (ide0p2 becomes ide0p1) */
static int same(int s, int c)
{
    return phys[s] == cphys[c] && dev[s].start == cand[c].start && dev[s].sectors == cand[c].sectors &&
           (!strcmp(dev[s].name, cand[c].name) || (!strncmp(dev[s].name, "ide0p", 5) && !strncmp(cand[c].name, "ide0p", 5)));
}

/* read the tables again and merge: a device found again keeps its slot (and its mount), a mounted device that
 * is gone keeps its slot until it is unmounted, the rest of the slots are freed and new devices take free ones */
static void scan(void)
{
    ncand = 0; dropped = 0;
    if (disk_sectors) add("ide0", "", 0, disk_sectors, 0, 0);
    if (ram_sectors) add("ram0", "", 0, ram_sectors, 0, 0);
    int count = 0;
    if (disk_sectors && !add_rdb(0, disk_sectors, &count)) add_mbr(&count);
    for (int f = 0; f < 4; f++) {                         /* floppy drives with a disk in them: df0 ... df3 */
        if (!(fd_probe & (1 << f))) continue;
        int r = sys_fd_info((u32)f);
        if ((r & 3) == 3) {
            char name[4] = { 'd', 'f', (char)('0' + f), 0 };
            int before = ncand;
            add(name, "", 0, (u32)r >> 8, 0, 0);
            if (ncand > before) {
                cfd[ncand - 1] = f;
                cphys[ncand - 1] = (u8)(2 + f);
                cand[ncand - 1].caps = 0;                 /* write-through: nothing to flush */
                cand[ncand - 1].maxsect = 22;
            }
        }
    }
    if (dropped) printf("diskd: more than %d devices: %d not served\n", MAXDEV, dropped);
    u8 taken[MAXDEV];
    memset(taken, 0, sizeof taken);
    norder = 0;
    for (int s = 0; s < MAXDEV; s++) {                    /* old slots: found again, or kept while mounted */
        if (!used[s]) continue;
        int c;
        for (c = 0; c < ncand; c++) if (!taken[c] && same(s, c)) break;
        if (c < ncand) {
            u32 m = dev[s].mounted;
            dev[s] = cand[c]; dev[s].mounted = m; fdrive[s] = cfd[c]; phys[s] = cphys[c];
            taken[c] = 1;
        } else if (!dev[s].mounted) {
            used[s] = 0;
        }
    }
    for (int c = 0; c < ncand; c++) {                     /* new devices into free slots */
        if (taken[c]) continue;
        int s;
        for (s = 0; s < MAXDEV && used[s]; s++) ;
        if (s == MAXDEV) { printf("diskd: no slot for %s: not served\n", cand[c].name); continue; }
        used[s] = 1; dev[s] = cand[c]; fdrive[s] = cfd[c]; phys[s] = cphys[c];
        taken[c] = 1;
    }
    for (int c = 0; c < ncand; c++)                       /* the listing order: as found */
        for (int s = 0; s < MAXDEV; s++)
            if (used[s] && same(s, c) && norder < MAXDEV) { order[norder++] = (u8)s; break; }
    for (int s = 0; s < MAXDEV; s++) {                    /* then mounted devices that are gone from the table */
        int in = 0;
        for (int k = 0; k < norder; k++) if (order[k] == s) in = 1;
        if (used[s] && !in && norder < MAXDEV) order[norder++] = (u8)s;
    }
}

static int find(const char *name)
{
    for (int k = 0; k < norder; k++)
        if (!strcmp(dev[order[k]].name, name)) return order[k];
    return -1;
}

static int alive(u32 pid)               /* is the process that mounted a device still there? */
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++) {
        if (sys_procinfo(i, &p) < 0) break;
        if (p.pid == pid && p.state != 0 && p.state != 5) return 1;     /* not free, not a zombie */
    }
    return 0;
}

/* the process that has device s mounted, if it still exists (a mount of a dead server is released here) */
static u32 holder(int s)
{
    if (dev[s].mounted && !alive(dev[s].mounted)) {
        printf("diskd: %s: the filesystem server (pid %u) is gone: mount released\n", dev[s].name, dev[s].mounted);
        dev[s].mounted = 0;
    }
    return dev[s].mounted;
}

/* does device s (sectors [lba, lba+n) of it) share sectors with device t? */
static int overlaps(int s, u32 lba, u32 n, int t)
{
    if (phys[s] != phys[t]) return 0;
    u32 a = dev[s].start + lba, b = dev[t].start;
    return a < b + dev[t].sectors && b < a + n;
}

/* is there a device other than "self" overlapping [lba, lba+n) of s that someone other than pid has mounted? */
static int blocked(int s, u32 lba, u32 n, u32 pid)
{
    for (int t = 0; t < MAXDEV; t++)
        if (used[t] && dev[t].mounted && overlaps(s, lba, n, t) && holder(t) && dev[t].mounted != pid)
            return 1;
    return 0;
}

static int handle(struct umsg *m)
{
    switch (m->type) {
    case DISK_OPEN: {
        const char *nm = (const char *)m->a[0];
        if (nm[0] == 'd' && nm[1] == 'f' && nm[2] >= '0' && nm[2] <= '3' && !nm[3]) {
            fd_probe |= 1 << (nm[2] - '0');
            scan();                                                  /* a disk may have been put in since */
        }
        int h = find(nm);
        if (h < 0) return -1;
        if (m->a[1] & (DISK_MOUNT | DISK_RAW)) {
            /* a mount, or a tool that may write the device: nothing overlapping it may be mounted (not even
             * by the caller: a server mounts one device) */
            if (blocked(h, 0, dev[h].sectors, 0)) return -16;       /* EBUSY */
            if (m->a[1] & DISK_MOUNT) dev[h].mounted = (u32)m->from;
        }
        return h;
    }
    case DISK_UNMOUNT:
        if (m->a[0] < MAXDEV && used[m->a[0]] && dev[m->a[0]].mounted == (u32)m->from) { dev[m->a[0]].mounted = 0; return 0; }
        return -1;
    case DISK_READ: case DISK_WRITE: {
        u32 h = m->a[0], lba = m->a[1], n = m->a[2];
        if (h >= MAXDEV || !used[h] || n == 0 || lba + n < lba || lba + n > dev[h].sectors) return -1;
        if (m->type == DISK_WRITE && blocked((int)h, lba, n, (u32)m->from)) return -16;
        if (phys[h] == 1) {                                         /* the RAM disk: plain memory */
            if (m->type == DISK_READ) memcpy((void *)m->a[3], ram_base + lba * 512, n * 512);
            else memcpy(ram_base + lba * 512, (const void *)m->a[3], n * 512);
            return 0;
        }
        if (fdrive[h] >= 0)
            return sys_fd_rw((u32)fdrive[h], lba, n, (void *)m->a[3], m->type == DISK_WRITE);
        lba += dev[h].start;
        return m->type == DISK_READ ? sys_blk_read(lba, n, (void *)m->a[3])
                                    : sys_blk_write(lba, n, (const void *)m->a[3]);
    }
    case DISK_INFO:
        if (m->a[0] >= MAXDEV || !used[m->a[0]]) return -1;
        holder((int)m->a[0]);
        memcpy((void *)m->a[1], &dev[m->a[0]], sizeof(struct diskinfo));
        return 0;
    case DISK_LIST:
        if (m->a[0] >= (u32)norder) return -1;
        holder(order[m->a[0]]);
        memcpy((void *)m->a[1], &dev[order[m->a[0]]], sizeof(struct diskinfo));
        return 0;
    case DISK_FLUSH:
        if (m->a[0] >= MAXDEV || !used[m->a[0]]) return -1;
        if (phys[m->a[0]] != 0) return 0;                           /* RAM disk, floppy: nothing to flush */
        return sys_blk_flush();
    case DISK_RESCAN:
        scan();
        return norder;
    }
    return -1;
}

int main(void)
{
    int n = sys_blk_size();
    disk_sectors = n > 0 ? (u32)n : 0;
    u32 ram[2];
    if (sys_blk_ram(ram) == 0 && ram[1]) { ram_base = (u8 *)ram[0]; ram_sectors = ram[1] / 512; }
    int any_fd = 0;
    if (sys_fd_info(0) & 1) any_fd = 1;
    if (!disk_sectors && !ram_sectors && !any_fd) { printf("diskd: no disk\n"); return 1; }
    scan();
    int port = sys_port_create("disk");
    if (port < 0) return 1;
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1)
        m = sys_replyrecv(m, handle(m), port);
    return 1;
}
