/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"     /* host build for the tests (tools/disk-host-test.sh) */
#else
#include "libz.h"
#endif
#include "../kernel/diskproto.h"
#include "rdb.h"

/* fdisk: the Amiga partition table (RDB) of a disk.
 *   fdisk list [disk]                  show the disk(s) and partitions
 *   fdisk init DISK -y                 write an empty partition table (destroys the old one)
 *   fdisk add DISK LABEL SIZE [TYPE]   new partition; SIZE in MB or "rest"; TYPE ext2 (default) | zenixfs
 *   fdisk del DISK N                   delete partition N
 *   fdisk boot DISK N on|off           mark bootable
 * Changes are written at once and the disk server re-reads the table.
 * Editing keeps everything else exactly as it was: other partitions' PART blocks (their own geometry, DosEnvec,
 * flags) are not rewritten, only the "next" link of the one before a change; the FileSysHeader/LSEG chains (PFS3,
 * SFS handlers), the bad-block list and the drive-init code are never touched; a new PART block goes into a block
 * of the RDB area that no chain uses; a new partition uses the RDSK block's geometry, is placed in sectors where
 * nothing else is and is checked against every partition before it is written. A table fdisk cannot keep exactly
 * (see rdb.h: bad checksums, other block sizes, overlaps, more than 32 partitions, ...) is not edited at all. */

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

static int dlist(u32 i, struct diskinfo *d)
{
    struct umsg m;
    m.type = DISK_LISTSZ; m.a[0] = i; m.a[1] = (u32)d; m.a[2] = sizeof *d;
    return sys_call(port, &m);
}
static int dinfo(int hd, struct diskinfo *d)
{
    struct umsg m;
    m.type = DISK_INFOSZ; m.a[0] = (u32)hd; m.a[1] = (u32)d; m.a[2] = sizeof *d;
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
    dprintf(1, "%-8s %-10s %10s %9s  %s\n", "DEVICE", "LABEL", "SECTORS", "SIZE", "TYPE");
    for (u32 i = 0; dlist(i, &d) == 0; i++) {
        if (only && strncmp(d.name, only, strlen(only))) continue;
        /* KB below 1 MB (a floppy, the RAM disk), MB from there on: never "0" for a disk that has sectors */
        char size[16];
        if (d.sectors < 2048) snprintf(size, sizeof size, "%u KB", d.sectors / 2);
        else snprintf(size, sizeof size, "%u MB", d.sectors / 2048);
        dprintf(1, "%-8s %-10s %10u %9s  %s%s%s\n", d.name, d.label, d.sectors, size,
               d.dostype ? tname(d.dostype) : "", d.flags & 1 ? " boot" : "", d.mounted ? " mounted" : "");
    }
}

/* does sector 0 look like a PC partition table (MBR)? Such a disk (an Emu68 SD card, say) holds boot files
 * that init would wipe, so init needs a second -y there. */
static int has_mbr(void)
{
    static unsigned char sec[512];
    if (rd(0, sec) || sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++)
        if (sec[446 + 16 * i + 4] != 0 && (sec[446 + 16 * i] == 0 || sec[446 + 16 * i] == 0x80)) return 1;
    return 0;
}

/* about to write: the disk server gives a raw handle only when nothing on the disk is mounted */
static int may_write(const char *name)
{
    int r = open_disk(name, DISK_RAW);
    if (r == -EBUSY) { dprintf(1, "fdisk: %s is in use (a partition on it is mounted): fdisk can only list it\n", name); return 0; }
    if (r < 0) { dprintf(1, "fdisk: cannot open %s for writing\n", name); return 0; }
    h = r;
    return 1;
}

static unsigned b[128];

/* an empty table with fdisk's own geometry (16 x 63), the RDB in cylinder 0 */
static int write_empty(unsigned cylinders)
{
    for (unsigned i = 0; i < 128; i++) b[i] = 0;
    b[0] = RDB_ID_RDSK; b[1] = 64; b[3] = 7; b[4] = 512; b[5] = 0x12;
    for (int i = 6; i < 16; i++) b[i] = 0xFFFFFFFFu;
    b[16] = cylinders; b[17] = RDB_SECS; b[18] = RDB_HEADS;
    b[20] = cylinders; b[24] = b[25] = b[26] = cylinders;
    b[32] = 0; b[33] = RDB_CYL - 1; b[34] = 1; b[35] = cylinders - 1;
    b[36] = RDB_CYL; b[38] = 0;
    rdb_sum(b);
    return wr(0, b);
}

/* change one long of an RDB block in place (checksum recomputed); everything else in it stays */
static int poke(unsigned blk, unsigned idx, unsigned val, unsigned id)
{
    if (rd(blk, b) || b[0] != id || !rdb_sum_ok(b)) return -1;
    b[idx] = val;
    rdb_sum(b);
    return wr(blk, b);
}

static struct rdb r;

static int owned(unsigned blk)
{
    for (unsigned i = 0; i < r.nused; i++) if (r.used[i] == blk) return 1;
    return 0;
}

/* a new partition of "mb" megabytes (0 = the largest free space) in the RDSK geometry; returns 0 and the cylinders */
static int place(unsigned mb, unsigned disk_sectors, unsigned *lo, unsigned *hi)
{
    unsigned cyl = r.cylblocks;
    unsigned end = (r.hicyl + 1) * cyl;
    if (end > disk_sectors || end / cyl != r.hicyl + 1) end = disk_sectors - disk_sectors % cyl;
    unsigned want = mb ? (unsigned)(((unsigned long long)mb * 2048 + cyl - 1) / cyl) : 0;
    unsigned best_lo = 0, best_n = 0;
    /* free ranges between the partitions, in sectors, rounded to whole cylinders */
    for (unsigned c = r.locyl; c * cyl < end;) {
        unsigned s = c * cyl, next = end;
        int hit = 0;
        for (int i = 0; i < r.nparts; i++) {
            const struct rdb_part *p = &r.part[i];
            if (!p->sectors) continue;
            if (p->start <= s && s < p->start + p->sectors) { hit = 1; next = p->start + p->sectors; break; }
            if (p->start > s && p->start < next) next = p->start;
        }
        if (hit) { c = (next + cyl - 1) / cyl; continue; }
        unsigned n = next / cyl - c;                    /* whole free cylinders from c */
        if (mb && n >= want) { *lo = c; *hi = c + want - 1; return 0; }
        if (!mb && n > best_n) { best_n = n; best_lo = c; }
        c += n ? n : 1;
    }
    if (!mb && best_n) { *lo = best_lo; *hi = best_lo + best_n - 1; return 0; }
    return -1;
}

static unsigned free_mb(unsigned disk_sectors)
{
    unsigned lo, hi;
    if (place(0, disk_sectors, &lo, &hi)) return 0;
    return (unsigned)((unsigned long long)(hi - lo + 1) * r.cylblocks / 2048);
}

static const char USAGE[] = "usage: fdisk list|init|add|del|boot ...\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc < 2) zusage(USAGE);
    if (argv[1][0] == '-') zbadopt(argv[1], USAGE);
    port = sys_port_find("disk");
    if (port < 0) { dprintf(1, "fdisk: no disk server\n"); return 1; }
    if (!strcmp(argv[1], "list")) { list(argc > 2 ? argv[2] : 0); return 0; }
    if (argc < 3) { dprintf(1, "fdisk: which disk?\n"); return 2; }
    h = open_disk(argv[2], 0);                       /* reading first; writing needs DISK_RAW (see may_write) */
    struct diskinfo di;
    if (h < 0 || dinfo(h, &di) < 0 || di.start != 0 || di.dostype != 0 || strncmp(di.name, "ide", 3)) {
        dprintf(1, "fdisk: %s is not a whole disk\n", argv[2]);
        return 1;
    }
    int have = rdb_read(rd, &r);
    if (have < 0) { dprintf(1, "fdisk: cannot read %s\n", argv[2]); return 1; }
    unsigned cylinders = di.sectors / RDB_CYL;

    if (!strcmp(argv[1], "init")) {
        if (argc < 4 || strcmp(argv[3], "-y")) { dprintf(1, "fdisk: init destroys the partition table: add -y\n"); return 1; }
        if (have != 1 && has_mbr() && (argc < 5 || strcmp(argv[4], "-y"))) {
            dprintf(1, "fdisk: %s has a PC partition table (MBR) and boot files; init would destroy them.\n"
                   "       If you really mean it: fdisk init %s -y -y\n", argv[2], argv[2]);
            return 1;
        }
        if (cylinders < 3) { dprintf(1, "fdisk: disk too small\n"); return 1; }
        if (!may_write(argv[2])) return 1;
        if (write_empty(cylinders)) { dprintf(1, "fdisk: cannot write the partition table\n"); return 1; }
        rescan();
        list(argv[2]);
        return 0;
    }
    if (have != 1 && has_mbr()) {
        dprintf(1, "fdisk: %s has a PC partition table (MBR); fdisk edits Amiga tables (RDB) only.\n", argv[2]);
        return 1;
    }
    if (have != 1) {
        dprintf(1, "fdisk: no partition table on %s (use: fdisk init %s -y)\n", argv[2], argv[2]);
        return 1;
    }
    if (r.noedit) {
        dprintf(1, "fdisk: %s: %s.\n       fdisk will not change a table it cannot keep exactly (use HDToolBox).\n", argv[2], r.noedit);
        return 1;
    }

    if (strcmp(argv[1], "add") && strcmp(argv[1], "del") && strcmp(argv[1], "boot")) { dprintf(1, "fdisk: unknown command %s\n", argv[1]); return 2; }
    if (!may_write(argv[2])) return 1;
    int rc = 0;
    if (!strcmp(argv[1], "add")) {
        if (argc < 5) { dprintf(2, "usage: fdisk add DISK LABEL SIZE [ext2|zenixfs]\n"); return 2; }
        if (r.nparts >= RDB_MAXPART) { dprintf(1, "fdisk: too many partitions (%d)\n", RDB_MAXPART); return 1; }
        if (strlen(argv[3]) > 31 || !argv[3][0]) { dprintf(1, "fdisk: a label has 1 to 31 characters\n"); return 1; }
        unsigned mb = strcmp(argv[4], "rest") ? (unsigned)atoi(argv[4]) : 0, lo, hi;
        if (strcmp(argv[4], "rest") && mb == 0) { dprintf(1, "fdisk: size in MB, or \"rest\"\n"); return 2; }
        if (place(mb, di.sectors, &lo, &hi)) {
            dprintf(1, "fdisk: not enough free space (the largest free part is %u MB)\n", free_mb(di.sectors));
            return 1;
        }
        unsigned st = lo * r.cylblocks, n = (hi - lo + 1) * r.cylblocks;
        for (int i = 0; i < r.nparts; i++)               /* never over another partition */
            if (r.part[i].sectors && st < r.part[i].start + r.part[i].sectors && r.part[i].start < st + n) {
                dprintf(1, "fdisk: internal error: the new partition would overlap %s\n", r.part[i].label);
                return 1;
            }
        unsigned nb;                                     /* a block of the RDB area that no chain uses */
        for (nb = r.blk + 1; nb <= r.rdbhi && owned(nb); nb++) ;
        if (nb > r.rdbhi) { dprintf(1, "fdisk: no free block left in the RDB area for another partition\n"); return 1; }
        for (unsigned i = 0; i < 128; i++) b[i] = 0;
        b[0] = RDB_ID_PART; b[1] = 64; b[3] = 7;
        b[4] = 0xFFFFFFFFu;
        unsigned l = 0;
        while (argv[3][l] && l < 31) { rdb_setbyte(b, 37 + l, (unsigned char)argv[3][l]); l++; }
        rdb_setbyte(b, 36, l);                           /* DriveName: a BSTR at long 9 */
        unsigned *env = &b[32];
        env[0] = 16; env[1] = 128; env[3] = r.heads; env[4] = 1; env[5] = r.secs; env[6] = 2;
        env[9] = lo; env[10] = hi; env[11] = 30;
        env[13] = 0x7FFFFFFFu; env[14] = 0xFFFFFFFEu;
        env[16] = (argc > 5 && !strcmp(argv[5], "zenixfs")) ? DT_ZENIXFS : DT_EXT2;
        rdb_sum(b);
        if (wr(nb, b)) rc = -1;                          /* first the new block, then the link to it */
        else if (r.nparts) rc = poke(r.part[r.nparts - 1].blk, 4, nb, RDB_ID_PART);
        else rc = poke(r.blk, 7, nb, RDB_ID_RDSK);
        if (!rc && rd(r.blk, b) == 0 && b[38] < nb) rc = poke(r.blk, 38, nb, RDB_ID_RDSK);   /* HighRDSKBlock */
    } else if (!strcmp(argv[1], "del") || !strcmp(argv[1], "boot")) {
        if (argc < 4) { dprintf(2, "usage: fdisk %s DISK N\n", argv[1]); return 2; }
        int n = atoi(argv[3]) - 1;
        if (n < 0 || n >= r.nparts) { dprintf(1, "fdisk: no partition %s\n", argv[3]); return 1; }
        if (!strcmp(argv[1], "del")) {
            if (rd(r.part[n].blk, b) || b[0] != RDB_ID_PART) rc = -1;
            else {
                unsigned next = b[4];
                rc = n ? poke(r.part[n - 1].blk, 4, next, RDB_ID_PART) : poke(r.blk, 7, next, RDB_ID_RDSK);
            }
        } else {
            if (rd(r.part[n].blk, b) || b[0] != RDB_ID_PART) rc = -1;
            else rc = poke(r.part[n].blk, 5, (argc > 4 && !strcmp(argv[4], "off")) ? (b[5] & ~1u) : (b[5] | 1u), RDB_ID_PART);
        }
    } else { dprintf(1, "fdisk: unknown command %s\n", argv[1]); return 2; }

    if (rc) { dprintf(1, "fdisk: cannot write the partition table\n"); return 1; }
    rescan();
    list(argv[2]);
    return 0;
}
