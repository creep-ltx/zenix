#pragma once
/* Amiga Rigid Disk Block (RDB) partition table: reading, shared by the disk server and fdisk so there is one
 * implementation. Every PART block keeps its own geometry (DosEnvec Surfaces, BlocksPerTrack, SizeBlock, as
 * HDToolBox or any other tool wrote it): partition starts and sizes are computed from it, in 512-byte sectors.
 * rdb_read also finds every block the RDB owns (RDSK, the PART chain, the FileSysHeader chain with each LSEG chain,
 * the bad-block list, the drive-init code) and tells whether the table can be edited without changing anything it
 * does not understand (fdisk refuses to write otherwise). */
#define RDB_MAXPART 32              /* partitions read (a longer chain is "not editable") */
#define RDB_MAXUSED 128             /* blocks of the RDB area that belong to a chain */
#define RDB_HEADS 16                /* geometry of a NEW table (fdisk init): 16 heads x 63 sectors */
#define RDB_SECS  63
#define RDB_CYL   (RDB_HEADS * RDB_SECS)

struct rdb_part {
    char label[32];
    unsigned lowcyl, highcyl;       /* inclusive, in this partition's own cylinders */
    unsigned dostype;
    unsigned bootable;
    unsigned heads, secs;           /* the partition's own geometry (DosEnvec Surfaces, BlocksPerTrack) */
    unsigned blk;                   /* block number of its PART block */
    unsigned start, sectors;        /* in 512-byte sectors from the start of the RDB's disk; sectors 0 = unusable */
};
struct rdb {
    unsigned blk;                   /* block of the RDSK block (0-15) */
    unsigned cylinders, heads, secs, cylblocks;     /* the RDSK block's own geometry */
    unsigned locyl, hicyl;          /* cylinders available for partitions */
    unsigned rdbhi;                 /* last block of the RDB area (RDBBlocksHi) */
    int nparts;
    struct rdb_part part[RDB_MAXPART];
    unsigned nused, used[RDB_MAXUSED];  /* blocks owned by chains (RDSK, PART, FSHD, LSEG, BADB, drive init) */
    const char *noedit;             /* 0, or why fdisk must not write this table */
};

#define DT_EXT2  0x4C4E5800u        /* 'LNX\0': ext2 */
#define DT_ZENIXFS 0x5A4E4601u      /* 'ZNF\1': ZenixFS */

#define RDB_ID_RDSK 0x5244534bu
#define RDB_ID_PART 0x50415254u
#define RDB_ID_FSHD 0x46534844u
#define RDB_ID_LSEG 0x4c534547u
#define RDB_ID_BADB 0x42414442u

/* byte k of a block seen as big-endian longs (the label BSTR at long 9): long arithmetic only, so the same code
 * runs on the Amiga and in the host test (which swaps longs on I/O) */
static __attribute__((unused)) unsigned rdb_byte(const unsigned *b, unsigned k) { return (b[k / 4] >> (24 - 8 * (k % 4))) & 0xFF; }
static __attribute__((unused)) void rdb_setbyte(unsigned *b, unsigned k, unsigned v)
{
    unsigned sh = 24 - 8 * (k % 4);
    b[k / 4] = (b[k / 4] & ~(0xFFu << sh)) | ((v & 0xFF) << sh);
}

/* rd: read one 512-byte sector; return 0 on success */
typedef int (*rdb_io)(unsigned lba, void *buf);

static __attribute__((unused)) int rdb_sum_ok(const unsigned *b)
{
    unsigned s = 0;
    if (b[1] < 3 || b[1] > 128) return 0;
    for (unsigned i = 0; i < b[1]; i++) s += b[i];
    return s == 0;
}

static __attribute__((unused)) void rdb_sum(unsigned *b)
{
    b[2] = 0;
    unsigned s = 0;
    for (unsigned i = 0; i < b[1]; i++) s += b[i];
    b[2] = -s;
}

static __attribute__((unused)) int rdb_own(struct rdb *r, unsigned blk)   /* 0, or -1: a loop or too many blocks */
{
    for (unsigned i = 0; i < r->nused; i++) if (r->used[i] == blk) return -1;
    if (r->nused == RDB_MAXUSED) return -1;
    r->used[r->nused++] = blk;
    return 0;
}

/* follow a chain of blocks with ID id linked through long 4 (next); every block is recorded as owned */
static __attribute__((unused)) int rdb_chain(rdb_io rd, struct rdb *r, unsigned first, unsigned id, unsigned *sec)
{
    for (unsigned b = first; b != 0xFFFFFFFFu;) {
        if (b > r->rdbhi || rdb_own(r, b)) { r->noedit = "a block chain leaves the RDB area or loops"; return 0; }
        if (rd(b, sec)) return -1;
        if (sec[0] != id || !rdb_sum_ok(sec)) { r->noedit = "a block of the RDB has a bad ID or checksum"; return 0; }
        b = sec[4];
    }
    return 0;
}

/* returns 1 if an RDB was found (and fills *r), 0 if the disk has none, -1 on error */
static __attribute__((unused)) int rdb_read(rdb_io rd, struct rdb *r)
{
    static unsigned sec[128];
    r->nparts = 0;
    r->nused = 0;
    r->noedit = 0;
    for (unsigned i = 0; i < 16; i++) {
        if (rd(i, sec)) return -1;
        if (sec[0] != RDB_ID_RDSK) continue;
        r->blk = i;
        r->cylinders = sec[16]; r->secs = sec[17]; r->heads = sec[18]; r->cylblocks = sec[36];
        r->locyl = sec[34]; r->hicyl = sec[35]; r->rdbhi = sec[33];
        unsigned plist = sec[7], fslist = sec[8], badlist = sec[6], init = sec[9];
        if (!rdb_sum_ok(sec)) r->noedit = "the RDSK block has a bad checksum";
        else if (sec[4] != 512) r->noedit = "the RDB uses blocks other than 512 bytes";
        else if (r->cylblocks == 0 || r->cylblocks != r->heads * r->secs || r->locyl > r->hicyl ||
                 r->rdbhi < i || r->rdbhi >= r->locyl * r->cylblocks)
            r->noedit = "the RDSK block's geometry does not add up";
        if (r->rdbhi < i) r->rdbhi = i;
        rdb_own(r, i);
        unsigned guard = 0;
        for (unsigned next = plist; next != 0xFFFFFFFFu; guard++) {
            if (r->nparts == RDB_MAXPART || guard > RDB_MAXPART) { if (!r->noedit) r->noedit = "more partitions than fdisk handles"; break; }
            if (rdb_own(r, next)) { if (!r->noedit) r->noedit = "the partition chain loops"; break; }
            if (next > r->rdbhi) {               /* outside the RDB area: still read (AmigaOS follows it), never edited */
                if (!r->noedit) r->noedit = "the partition chain leaves the RDB area";
                if (next > r->rdbhi + 4096) break;
            }
            if (rd(next, sec)) { if (!r->noedit) r->noedit = "a partition block cannot be read"; break; }
            if (sec[0] != RDB_ID_PART) { if (!r->noedit) r->noedit = "the partition chain holds a block that is not a PART"; break; }
            if (!rdb_sum_ok(sec) && !r->noedit) r->noedit = "a PART block has a bad checksum";
            struct rdb_part *p = &r->part[r->nparts++];
            unsigned l = rdb_byte(sec, 36) < 31 ? rdb_byte(sec, 36) : 31;     /* DriveName: a BSTR at long 9 */
            for (unsigned k = 0; k < l; k++) p->label[k] = (char)rdb_byte(sec, 37 + k);
            p->label[l] = 0;
            unsigned *env = &sec[32];
            p->lowcyl = env[9]; p->highcyl = env[10]; p->dostype = env[16];
            p->heads = env[3]; p->secs = env[5];
            p->bootable = sec[5] & 1;
            p->blk = next;
            /* sectors per cylinder of this partition: Surfaces x BlocksPerTrack sectors of SizeBlock longs */
            unsigned sb = env[1], per = sb && (sb * 4) % 512 == 0 ? sb * 4 / 512 : 0;
            unsigned long long cyl = (unsigned long long)p->heads * p->secs * per;
            unsigned long long st = cyl * p->lowcyl, n = cyl * ((unsigned long long)p->highcyl - p->lowcyl + 1);
            if (env[0] < 10 || !cyl || p->highcyl < p->lowcyl || st + n > 0xFFFFFFFFull) {
                p->start = p->sectors = 0;              /* not a partition we can place: never served */
                if (!r->noedit) r->noedit = "a partition has a geometry fdisk does not understand";
            } else {
                p->start = (unsigned)st; p->sectors = (unsigned)n;
            }
            next = sec[4];
        }
        /* the other chains: never touched, but their blocks are not free */
        if (rdb_chain(rd, r, badlist, RDB_ID_BADB, sec)) return -1;
        if (rdb_chain(rd, r, init, RDB_ID_LSEG, sec)) return -1;
        for (unsigned b = fslist, g = 0; b != 0xFFFFFFFFu; g++) {
            if (g > 64 || b > r->rdbhi || rdb_own(r, b)) { if (!r->noedit) r->noedit = "the filesystem chain leaves the RDB area or loops"; break; }
            if (rd(b, sec)) return -1;
            if (sec[0] != RDB_ID_FSHD || !rdb_sum_ok(sec)) { if (!r->noedit) r->noedit = "a FileSysHeader block has a bad ID or checksum"; break; }
            unsigned nextfs = sec[4], seg = sec[18];
            if (rdb_chain(rd, r, seg, RDB_ID_LSEG, sec)) return -1;
            b = nextfs;
        }
        /* partitions must not overlap each other or the RDB area */
        for (int a = 0; a < r->nparts; a++) {
            struct rdb_part *p = &r->part[a];
            if (!p->sectors) continue;
            if (p->start <= r->rdbhi && !r->noedit) r->noedit = "a partition overlaps the RDB area";
            for (int b2 = a + 1; b2 < r->nparts; b2++) {
                struct rdb_part *q = &r->part[b2];
                if (q->sectors && p->start < q->start + q->sectors && q->start < p->start + p->sectors && !r->noedit)
                    r->noedit = "two partitions overlap";
            }
        }
        return 1;
    }
    return 0;
}
