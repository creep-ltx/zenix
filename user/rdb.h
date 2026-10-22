#pragma once
/* Amiga Rigid Disk Block (RDB) partition table: read and write.
 * Shared by the disk server and fdisk so there is one implementation.
 * Geometry is fixed at 16 heads x 63 sectors (1008 sectors per cylinder);
 * cylinder 0 holds the RDB itself, partitions start at cylinder 1. */
#define RDB_HEADS 16
#define RDB_SECS  63
#define RDB_CYL   (RDB_HEADS * RDB_SECS)
#define RDB_MAXPART 12

struct rdb_part {
    char label[32];
    unsigned lowcyl, highcyl;       /* inclusive */
    unsigned dostype;
    unsigned bootable;
    unsigned heads, secs;           /* the partition's own geometry (from the table) */
};
struct rdb {
    unsigned cylinders;             /* of the whole disk */
    int nparts;
    struct rdb_part part[RDB_MAXPART];
};

#define DT_EXT2  0x4C4E5800u        /* 'LNX\0': ext2 */
#define DT_ZENIXFS 0x5A4E4601u      /* 'ZNF\1': ZenixFS */

/* rd/wr: read or write one 512-byte sector; return 0 on success */
typedef int (*rdb_io)(unsigned lba, void *buf);

/* returns 1 if an RDB was found (and fills *r), 0 if the disk has none, -1 on error */
static int rdb_read(rdb_io rd, struct rdb *r)
{
    static unsigned sec[128];
    r->nparts = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (rd(i, sec)) return -1;
        if (sec[0] != 0x5244534b) continue;                  /* 'RDSK' */
        r->cylinders = sec[16];
        unsigned next = sec[7];
        for (int guard = 0; next != 0xFFFFFFFFu && guard < RDB_MAXPART; guard++) {
            if (rd(next, sec)) return -1;
            if (sec[0] != 0x50415254) break;                 /* 'PART' */
            struct rdb_part *p = &r->part[r->nparts++];
            unsigned char *nm = (unsigned char *)&sec[9];
            unsigned l = nm[0] < 31 ? nm[0] : 31;
            for (unsigned k = 0; k < l; k++) p->label[k] = (char)nm[1 + k];
            p->label[l] = 0;
            unsigned *env = &sec[32];
            p->lowcyl = env[9]; p->highcyl = env[10]; p->dostype = env[16];
            p->heads = env[3]; p->secs = env[5];
            p->bootable = sec[5] & 1;
            next = sec[4];
        }
        return 1;
    }
    return 0;
}

static void rdb_sum(unsigned *b)
{
    b[2] = 0;
    unsigned s = 0;
    for (unsigned i = 0; i < b[1]; i++) s += b[i];
    b[2] = -s;
}

/* write the RDB (block 0) and one PART block per partition (blocks 1..n) */
static __attribute__((unused)) int rdb_write(rdb_io wr, const struct rdb *r)
{
    static unsigned b[128];
    for (unsigned i = 0; i < 128; i++) b[i] = 0;
    b[0] = 0x5244534b; b[1] = 64; b[3] = 7; b[4] = 512; b[5] = 0x12;
    for (int i = 6; i < 16; i++) b[i] = 0xFFFFFFFFu;
    b[7] = r->nparts ? 1 : 0xFFFFFFFFu;
    b[16] = r->cylinders; b[17] = RDB_SECS; b[18] = RDB_HEADS;
    b[20] = r->cylinders; b[24] = b[25] = b[26] = r->cylinders;
    b[32] = 0; b[33] = RDB_CYL - 1; b[34] = 1; b[35] = r->cylinders - 1;
    b[36] = RDB_CYL; b[38] = (unsigned)r->nparts;
    rdb_sum(b);
    if (wr(0, b)) return -1;
    for (int n = 0; n < r->nparts; n++) {
        const struct rdb_part *p = &r->part[n];
        for (unsigned i = 0; i < 128; i++) b[i] = 0;
        b[0] = 0x50415254; b[1] = 64; b[3] = 7;
        b[4] = (n + 1 < r->nparts) ? (unsigned)(n + 2) : 0xFFFFFFFFu;
        b[5] = p->bootable ? 1 : 0;
        unsigned char *nm = (unsigned char *)&b[9];
        unsigned l = 0;
        while (p->label[l] && l < 31) { nm[1 + l] = (unsigned char)p->label[l]; l++; }
        nm[0] = (unsigned char)l;
        unsigned *env = &b[32];
        env[0] = 16; env[1] = 128; env[3] = RDB_HEADS; env[4] = 1; env[5] = RDB_SECS; env[6] = 2;
        env[9] = p->lowcyl; env[10] = p->highcyl; env[11] = 30;
        env[13] = 0x7FFFFFFFu; env[14] = 0xFFFFFFFEu; env[16] = p->dostype;
        rdb_sum(b);
        if (wr((unsigned)(n + 1), b)) return -1;
    }
    return 0;
}
