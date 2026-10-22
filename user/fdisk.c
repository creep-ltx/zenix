#include "libz.h"
#include "../kernel/diskproto.h"
#include "rdb.h"

/* fdisk: the Amiga partition table (RDB) of a disk.
 *   fdisk list [disk]                  show the disk(s) and partitions
 *   fdisk init DISK -y                 write an empty partition table (destroys the old one)
 *   fdisk add DISK LABEL SIZE [TYPE]   new partition; SIZE in MB or "rest"; TYPE ext2 (default) | zenixfs
 *   fdisk del DISK N                   delete partition N
 *   fdisk boot DISK N on|off           mark bootable
 * Changes are written at once and the disk server re-reads the table. */

static int port, h;

static int wr(unsigned lba, void *buf)
{
    struct umsg m;
    m.type = DISK_WRITE; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = 1; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}
static int rd(unsigned lba, void *buf)
{
    struct umsg m;
    m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = 1; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}

static int dinfo(u32 i, struct diskinfo *d)
{
    struct umsg m;
    m.type = DISK_LIST; m.a[0] = i; m.a[1] = (u32)d;
    return sys_call(port, &m);
}

static int open_disk(const char *name, u32 flags)
{
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = flags;
    return sys_call(port, &m);
}

static void rescan(void)
{
    struct umsg m;
    m.type = DISK_RESCAN;
    sys_call(port, &m);
}

static const char *tname(unsigned t)
{
    return t == DT_EXT2 ? "ext2" : t == DT_ZENIXFS ? "zenixfs" : "other";
}

static void list(const char *only)
{
    struct diskinfo d;
    printf("%-8s %-10s %10s %8s  %s\n", "DEVICE", "LABEL", "SECTORS", "SIZE(MB)", "TYPE");
    for (u32 i = 0; dinfo(i, &d) == 0; i++) {
        if (only && strncmp(d.name, only, strlen(only))) continue;
        printf("%-8s %-10s %10u %8u  %s%s%s\n", d.name, d.label, d.sectors, d.sectors / 2048,
               d.dostype ? tname(d.dostype) : "", d.flags & 1 ? " boot" : "", d.mounted ? " mounted" : "");
    }
}

static unsigned total_cyls(void)
{
    struct diskinfo d;
    if (dinfo(0, &d) < 0) return 0;
    return d.sectors / RDB_CYL;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: fdisk list|init|add|del|boot ...\n"); return 2; }
    port = sys_port_find("disk");
    if (port < 0) { printf("fdisk: no disk server\n"); return 1; }
    if (!strcmp(argv[1], "list")) { list(argc > 2 ? argv[2] : 0); return 0; }
    if (argc < 3) { printf("fdisk: which disk?\n"); return 2; }
    h = open_disk(argv[2], DISK_RAW);
    struct diskinfo di;
    if (h < 0 || dinfo((u32)h, &di) < 0 || di.start != 0 || di.dostype != 0) {
        printf("fdisk: %s is not a whole disk (or is mounted)\n", argv[2]);
        return 1;
    }
    struct rdb r;
    int have = rdb_read(rd, &r);
    r.cylinders = total_cyls();
    if (r.cylinders < 3) { printf("fdisk: disk too small\n"); return 1; }

    if (!strcmp(argv[1], "init")) {
        if (argc < 4 || strcmp(argv[3], "-y")) { printf("fdisk: init destroys the partition table: add -y\n"); return 1; }
        r.nparts = 0;
    } else if (have != 1) {
        printf("fdisk: no partition table on %s (use: fdisk init %s -y)\n", argv[2], argv[2]);
        return 1;
    } else if (!strcmp(argv[1], "add")) {
        if (argc < 5) { printf("usage: fdisk add DISK LABEL SIZE [ext2|zenixfs]\n"); return 2; }
        if (r.nparts >= RDB_MAXPART) { printf("fdisk: too many partitions\n"); return 1; }
        unsigned first = 1;
        for (int i = 0; i < r.nparts; i++) if (r.part[i].highcyl + 1 > first) first = r.part[i].highcyl + 1;
        unsigned avail = r.cylinders - first;
        unsigned cyls;
        if (!strcmp(argv[4], "rest")) cyls = avail;
        else cyls = ((unsigned)atoi(argv[4]) * 2048 + RDB_CYL - 1) / RDB_CYL;
        if (cyls == 0 || cyls > avail) { printf("fdisk: not enough free space (%u MB left)\n", avail * RDB_CYL / 2048); return 1; }
        struct rdb_part *p = &r.part[r.nparts++];
        memset(p, 0, sizeof *p);
        strcpy(p->label, argv[3]);
        p->lowcyl = first; p->highcyl = first + cyls - 1;
        p->dostype = (argc > 5 && !strcmp(argv[5], "zenixfs")) ? DT_ZENIXFS : DT_EXT2;
    } else if (!strcmp(argv[1], "del") || !strcmp(argv[1], "boot")) {
        if (argc < 4) { printf("usage: fdisk %s DISK N\n", argv[1]); return 2; }
        int n = atoi(argv[3]) - 1;
        if (n < 0 || n >= r.nparts) { printf("fdisk: no partition %s\n", argv[3]); return 1; }
        if (!strcmp(argv[1], "del")) { for (int i = n; i + 1 < r.nparts; i++) r.part[i] = r.part[i + 1]; r.nparts--; }
        else r.part[n].bootable = (argc > 4 && !strcmp(argv[4], "off")) ? 0 : 1;
    } else { printf("fdisk: unknown command %s\n", argv[1]); return 2; }

    if (rdb_write(wr, &r)) { printf("fdisk: cannot write the partition table\n"); return 1; }
    rescan();
    list(argv[2]);
    return 0;
}
