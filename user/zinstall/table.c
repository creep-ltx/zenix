/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D35: see table.h. Everything read from the disk is untrusted: every number is range-checked before use, a table
 * that does not add up is reported and never changed. */
#ifdef ZHOST
#include <string.h>
#include <stdio.h>
#else
#include "../libz.h"
#endif
#include "../sha256.h"
#include "table.h"
#ifndef RDB_MAXUSED
#define RDB_MAXUSED 2048                /* blocks of chains in one RDB, for this program (a PFS3 handler is 121) */
#endif
#include "../rdb.h"

#define BOOTNEW 1                       /* a new RDB's own cylinder */
#define FLG_BOOT 1u
#define FLG_NOMOUNT 2u
#define MAXTRANSFER_DOS 0x1FE00u        /* what is safe with every IDE drive under AmigaOS */

static t_u32 be32(const t_u8 *p) { return (t_u32)p[0] << 24 | (t_u32)p[1] << 16 | (t_u32)p[2] << 8 | p[3]; }
static t_u32 le32(const t_u8 *p) { return (t_u32)p[3] << 24 | (t_u32)p[2] << 16 | (t_u32)p[1] << 8 | p[0]; }
static void put_le32(t_u8 *p, t_u32 v) { p[0] = (t_u8)v; p[1] = (t_u8)(v >> 8); p[2] = (t_u8)(v >> 16); p[3] = (t_u8)(v >> 24); }
static void to_longs(t_u32 *l, const t_u8 *b) { for (int i = 0; i < 128; i++) l[i] = be32(b + i * 4); }
static void to_bytes(t_u8 *b, const t_u32 *l)
{
    for (int i = 0; i < 128; i++) { b[i * 4] = (t_u8)(l[i] >> 24); b[i * 4 + 1] = (t_u8)(l[i] >> 16); b[i * 4 + 2] = (t_u8)(l[i] >> 8); b[i * 4 + 3] = (t_u8)l[i]; }
}
static int all_zero(const t_u8 *b, int n) { for (int i = 0; i < n; i++) if (b[i]) return 0; return 1; }

/* ---- reading, with the digest ---- */
static const struct tio *cio;
static struct sha256 hctx;
static t_u32 rd_base, rd_limit;
static t_u8 sec[512];

static int hrd(t_u32 lba, t_u8 *buf)    /* one sector of the whole disk, into the digest */
{
    if (cio->rd(cio->ctx, lba, 1, buf)) return -1;
    t_u8 at[4] = { (t_u8)(lba >> 24), (t_u8)(lba >> 16), (t_u8)(lba >> 8), (t_u8)lba };
    sha256_update(&hctx, at, 4);
    sha256_update(&hctx, buf, 512);
    return 0;
}
/* rdb.h's reader: sectors of the RDB's own range, as longs */
static int rdb_rd(unsigned lba, void *buf)
{
    if (lba >= rd_limit) return -1;
    t_u8 b[512];
    if (hrd(rd_base + lba, b)) return -1;
    to_longs((t_u32 *)buf, b);
    return 0;
}
static struct rdb rw;                   /* the reader's work space: rdb.h's struct with this program's RDB_MAXUSED */

static void dev_name(char *out, int n)
{
    strcpy(out, "ide0p");
    if (n >= 10) { out[5] = (char)('0' + (n / 10) % 10); out[6] = (char)('0' + n % 10); out[7] = 0; }
    else { out[5] = (char)('0' + n); out[6] = 0; }
}

static int overlap(t_u32 a, t_u32 an, t_u32 b, t_u32 bn)
{
    return (unsigned long long)a < (unsigned long long)b + bn && (unsigned long long)b < (unsigned long long)a + an;
}

/* the volume name in an AmigaDOS volume's root block: "what is on it" (best effort; not part of the digest) */
static void read_vol(const struct tio *io, struct t_part *p)
{
    t_u8 b[512];
    if ((p->dostype >> 8) != 0x444F53u || p->sectors < 8) return;
    t_u32 root = (2 + p->sectors - 1) / 2;
    if (io->rd(io->ctx, p->start + root, 1, b)) return;
    if (be32(b) != 2 || be32(b + 508) != 1) return;
    unsigned l = b[512 - 80];
    if (l == 0 || l > 30) return;
    for (unsigned k = 0; k < l; k++) {
        t_u8 c = b[512 - 79 + k];
        p->vol[k] = (c >= 32 && c < 127) || c >= 160 ? (char)c : '?';
    }
    p->vol[l] = 0;
}

/* the partitions of the RDB at base, into d: 1 there is one, 0 there is none, -1 a read failed. */
static int read_rdb(struct tdisk *d, t_u32 base, t_u32 limit, int *count)
{
    if (d->nregions == T_MAXREGION) return 0;
    rd_base = base; rd_limit = limit;
    int rc = rdb_read(rdb_rd, &rw);
    if (rc != 1) return rc;
    int ri = d->nregions++;
    struct t_region *g = &d->region[ri];
    g->base = base; g->limit = limit;
    g->blk = rw.blk; g->cyl = rw.cylblocks; g->heads = rw.heads; g->secs = rw.secs;
    g->locyl = rw.locyl; g->hicyl = rw.hicyl;
    g->noedit = rw.noedit;
    g->rdbhi = rw.rdbhi;
    g->nused = rw.nused;
    g->uselo = 0xFFFFFFFFu; g->usehi = 0; g->lseglo = 0xFFFFFFFFu; g->lseghi = 0; g->nlseg = 0;
    for (unsigned i = 0; i < rw.nused; i++) {
        t_u32 b = rw.used[i];
        if (b < g->uselo) g->uselo = b;
        if (b > g->usehi) g->usehi = b;
        int part = b == rw.blk;
        for (int k = 0; k < rw.nparts; k++) if (rw.part[k].blk == b) part = 1;
        if (!part) { g->nlseg++; if (b < g->lseglo) g->lseglo = b; if (b > g->lseghi) g->lseghi = b; }
    }
    {
        t_u8 rb[512];
        if (hrd(base + rw.blk, rb) == 0) { g->rdblo = be32(rb + 32 * 4); g->high = be32(rb + 38 * 4); }
    }
    if (!g->noedit && rw.nused == RDB_MAXUSED) g->noedit = "its block chains are longer than this program handles";
    if (!g->noedit && (g->heads > 0xFFFF || g->secs > 0xFFFF)) g->noedit = "the RDSK block's geometry does not add up";
    {   /* a bad-block list maps sectors to spare blocks that no chain names: those look free and are not */
        t_u8 rb[512];
        if (base + rw.blk < base || hrd(base + rw.blk, rb)) return -1;
        if (be32(rb + 6 * 4) != 0xFFFFFFFFu && !g->noedit) g->noedit = "it has a bad-block list, whose spare blocks this program cannot tell from free ones";
    }
    for (int i = 0; i < rw.nparts; i++) {
        const struct rdb_part *q = &rw.part[i];
        ++*count;
        if (d->nparts == T_MAXPART) { d->full = 1; if (!g->noedit) g->noedit = "more partitions than this program handles"; continue; }
        struct t_part *p = &d->part[d->nparts++];
        memset(p, 0, sizeof *p);
        dev_name(p->dev, *count);
        strncpy(p->label, q->label, sizeof p->label - 1);
        p->how = TP_RDB; p->region = (signed char)ri; p->holds = -1;
        p->dostype = q->dostype; p->blk = base + q->blk;
        p->lowcyl = q->lowcyl; p->highcyl = q->highcyl;
        if (q->sectors) {
            unsigned long long end = (unsigned long long)q->start + q->sectors;
            if (end > limit) { if (!g->noedit) g->noedit = "a partition reaches past the end of the disk"; }
            else { p->start = base + q->start; p->sectors = q->sectors; }
        }
        if (!p->sectors && !g->noedit) g->noedit = "a partition has a geometry this program does not understand";
        p->bootable = (t_u8)q->bootable;
    }
    /* each PART block once more: flags and boot priority */
    for (int i = 0; i < d->nparts; i++) {
        struct t_part *p = &d->part[i];
        if (p->region != ri) continue;
        t_u8 b[512];
        if (p->blk < base || p->blk - base >= limit) continue;
        if (hrd(p->blk, b)) return -1;
        t_u32 fl = be32(b + 5 * 4), pri = be32(b + (32 + 15) * 4);
        p->nomount = (fl & FLG_NOMOUNT) != 0;
        p->pri = (int)pri > 127 ? 127 : (int)pri < -128 ? -128 : (signed char)(int)pri;
    }
    return 1;
}

/* an MBR at sector 0, by the disk server's rule (user/diskd.c): 1 a table, 0 not */
static int mbr_plausible(const t_u8 *s, t_u32 sectors)
{
    if (s[510] != 0x55 || s[511] != 0xAA) return 0;
    int nused = 0;
    for (int i = 0; i < 4; i++) {
        const t_u8 *e = s + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) return 0;
        t_u32 st = le32(e + 8), n = le32(e + 12);
        if (!e[4] || !n) continue;
        if (!st || (unsigned long long)st + n > sectors) return 0;
        nused++;
    }
    return nused != 0;                  /* (no entries: a FAT volume's boot sector also ends in 55 AA) */
}
/* a PC volume's boot sector: a jump, and a plausible sector size and cluster size where a FAT volume has them */
static int looks_vbr(const t_u8 *s)
{
    if (s[510] != 0x55 || s[511] != 0xAA || (s[0] != 0xEB && s[0] != 0xE9)) return 0;
    unsigned bps = s[11] | (unsigned)s[12] << 8, spc = s[13];
    if (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096) return spc && !(spc & (spc - 1));
    return 0;
}
static int is_fat(t_u8 t)
{
    switch (t) {
    case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:
    case 0x07: case 0x17:
    case 0x11: case 0x14: case 0x16: case 0x1B: case 0x1C: case 0x1E: case 0xEF: return 1;
    }
    return 0;
}
static int is_ext(t_u8 t) { return t == 0x05 || t == 0x0F || t == 0x85 || t == 0xEE || t == 0xEF; }

static void digest_size(t_u32 s)
{
    t_u8 at[4] = { (t_u8)(s >> 24), (t_u8)(s >> 16), (t_u8)(s >> 8), (t_u8)s };
    sha256_update(&hctx, at, 4);
}

int t_read(struct tdisk *d, const struct tio *io, t_u32 sectors, const struct t_busy *busy, int nbusy)
{
    static t_u8 s1[512];
    memset(d, 0, sizeof *d);
    d->sectors = sectors;
    cio = io;
    sha256_init(&hctx);
    digest_size(sectors);
    if (sectors < 64) { d->kind = TD_UNKNOWN; d->what = "too small to hold anything"; sha256_final(&hctx, d->digest); return 0; }
    if (hrd(0, d->sec0) || hrd(1, s1)) return -1;
    int count = 0;
    rd_base = 0; rd_limit = sectors;
    /* is there an RDB? rdb.h looks in blocks 0-15 */
    int rdbat = -1;
    for (int i = 0; i < 16; i++) {
        t_u8 b[512];
        if (hrd((t_u32)i, b)) return -1;
        if (be32(b) == RDB_ID_RDSK) { rdbat = i; break; }
    }
    int mbr = rdbat != 0 && mbr_plausible(d->sec0, sectors) && !looks_vbr(d->sec0);
    int gpt = !memcmp(s1, "EFI PART", 8);
    if (mbr) for (int i = 0; i < 4; i++) if (d->sec0[446 + 16 * i + 4] == 0xEE) gpt = 1;
    if (gpt) {
        d->kind = TD_GPT; d->what = "a GPT partition table, which this program does not change";
    } else if (rdbat >= 0 && mbr) {
        d->kind = TD_HYBRID; d->what = "both an RDB and a PC partition table, which this program does not change";
    } else if (rdbat >= 0) {
        d->kind = TD_RDB; d->what = "an Amiga partition table (RDB)";
        int rc = read_rdb(d, 0, sectors, &count);
        if (rc < 0) return -1;
        d->noedit = d->region[0].noedit;
        d->oldrdb = !d->noedit;
    } else if (mbr) {
        d->kind = TD_MBR; d->what = "a PC partition table (MBR)";
        for (int i = 0; i < 4; i++) {
            const t_u8 *e = d->sec0 + 446 + 16 * i;
            t_u32 st = le32(e + 8), n = le32(e + 12);
            if (e[4] == 0) continue;
            d->nslotsused++;
            if (is_fat(e[4])) d->hasfat = 1;
            if (n == 0) continue;
            count++;
            if (d->nparts == T_MAXPART) { d->full = 1; continue; }
            int pi = d->nparts++;
            struct t_part *p = &d->part[pi];
            memset(p, 0, sizeof *p);
            dev_name(p->dev, count);
            p->how = TP_MBR; p->mbrtype = e[4]; p->slot = (t_u8)i; p->region = -1; p->holds = -1;
            p->start = st; p->sectors = n; p->bootable = e[0] == 0x80;
            if (e[4] == 0x76) {
                int before = d->nregions;
                int rc = read_rdb(d, st, n, &count);
                if (rc < 0) return -1;
                if (rc == 1) {
                    d->part[pi].holds = (signed char)before;
                    if (d->region[before].noedit && !d->noedit) d->noedit = d->region[before].noedit;   /* its real extent is not known: nothing beside it is sure free */
                }
            }
        }
        for (int a = 0; a < d->nparts; a++)         /* the entries must not overlap each other */
            for (int b = a + 1; b < d->nparts; b++)
                if (d->part[a].how == TP_MBR && d->part[b].how == TP_MBR &&
                    overlap(d->part[a].start, d->part[a].sectors, d->part[b].start, d->part[b].sectors))
                    d->noedit = "two of its partitions overlap";
        if (d->full && !d->noedit) d->noedit = "more partitions than this program handles";
    } else {
        /* no table. What is there? */
        const t_u8 *s = d->sec0;
        for (int i = 0; i < 4; i++) {           /* the shape of a FAT entry, whatever else is wrong with it: the erase is not for this disk */
            const t_u8 *e = s + 446 + 16 * i;
            if ((e[0] == 0 || e[0] == 0x80) && is_fat(e[4]) && le32(e + 12)) d->hasfat = 1;
        }
        static t_u8 s2[512];
        if (hrd(2, s2)) return -1;
        if (be32(s) >> 8 == 0x444F53u) { d->kind = TD_BARE; d->what = "an AmigaDOS volume, no partition table"; }
        else if (be32(s) >> 8 == 0x504653u || be32(s) >> 8 == 0x504453u || be32(s) >> 8 == 0x534653u) { d->kind = TD_BARE; d->what = "a PFS or SFS volume, no partition table"; }
        else if (s2[56] == 0x53 && s2[57] == 0xEF) { d->kind = TD_BARE; d->what = "an ext2 volume, no partition table"; }
        else if (!memcmp(s + 32, "ZNFS", 4)) { d->kind = TD_BARE; d->what = "a ZenixFS volume, no partition table"; }
        else if (s[510] == 0x55 && s[511] == 0xAA && (s[0] == 0xEB || s[0] == 0xE9)) { d->kind = TD_BARE; d->what = "a PC volume (FAT?), no partition table"; d->hasfat = 1; }
        else if (s[510] == 0x55 && s[511] == 0xAA) {
            d->kind = TD_UNKNOWN; d->what = "a PC partition table that makes no sense (it reaches outside the disk)";
        }
        else {
            /* zeros at the start and at 16 places across the disk? Otherwise: data of some kind */
            int blank = all_zero(s, 512) && all_zero(s1, 512) && all_zero(s2, 512);
            for (t_u32 i = 3; blank && i < 64; i++) { t_u8 b[512]; if (hrd(i, b)) return -1; if (!all_zero(b, 512)) blank = 0; }
            for (t_u32 i = 1; blank && i < 16; i++) { t_u8 b[512]; if (hrd((t_u32)(((unsigned long long)sectors * i) / 16), b)) return -1; if (!all_zero(b, 512)) blank = 0; }
            if (blank) { d->kind = TD_BLANK; d->what = "nothing (it looks empty)"; }
            else { d->kind = TD_UNKNOWN; d->what = "data, but no partition table this program knows"; }
        }
    }
    sha256_final(&hctx, d->digest);
    for (int i = 0; i < d->nparts; i++) read_vol(io, &d->part[i]);
    /* a Zenix an earlier zinstall made: ZBn (FFS), ZRn (ext2 or ZenixFS), perhaps ZHn, the same n, in one Amiga table */
    d->zb = d->zr = d->zh = -1;
    for (int i = 0; i < d->nparts && d->zr < 0; i++) {
        const struct t_part *r = &d->part[i];
        if (r->how != TP_RDB || strncmp(r->label, "ZR", 2) || !r->label[2] || r->label[3] || (r->dostype != DT_EXT2 && r->dostype != DT_ZENIXFS)) continue;
        int b = -1, h = -1;
        for (int k = 0; k < d->nparts; k++) {
            const struct t_part *q = &d->part[k];
            if (q->how != TP_RDB || q->region != r->region || q->label[2] != r->label[2] || q->label[3]) continue;
            if (!strncmp(q->label, "ZB", 2) && q->dostype == 0x444F5301u) b = k;
            if (!strncmp(q->label, "ZH", 2) && (q->dostype == DT_EXT2 || q->dostype == DT_ZENIXFS)) h = k;
        }
        if (b >= 0) { d->zb = b; d->zr = i; d->zh = h; }
    }
    for (int i = 0; i < nbusy; i++) {
        if (!busy[i].sectors) continue;
        d->anybusy = 1;
        for (int k = 0; k < d->nparts; k++)
            if (overlap(busy[i].start, busy[i].sectors, d->part[k].start, d->part[k].sectors)) d->part[k].busy = 1;
    }
    return 0;
}

int t_same(const struct tdisk *d, const struct tio *io, t_u32 sectors, const struct t_busy *busy, int nbusy)
{
    static struct tdisk again;
    if (sectors != d->sectors) return 0;
    if (t_read(&again, io, sectors, busy, nbusy)) return -1;
    if (memcmp(again.digest, d->digest, 32) || again.anybusy != d->anybusy || again.nparts != d->nparts) return 0;
    for (int i = 0; i < d->nparts; i++) if (again.part[i].busy != d->part[i].busy) return 0;
    return 1;
}

const char *t_typename(const struct t_part *p)
{
    if (p->how == TP_MBR) {
        if (p->mbrtype == 0x76) return "Amiga container (0x76)";
        if (p->mbrtype == 0x83) return "Linux (0x83)";
        if (p->mbrtype == 0x0C || p->mbrtype == 0x0B) return "FAT32";
        if (is_fat(p->mbrtype)) return "FAT";
        if (is_ext(p->mbrtype)) return "extended";
        return "PC type";
    }
    switch (p->dostype) {
    case DT_EXT2: return "ext2";
    case DT_ZENIXFS: return "ZenixFS";
    case 0x444F5300: return "AmigaDOS OFS";
    case 0x444F5301: return "AmigaDOS FFS";
    case 0x444F5302: return "AmigaDOS OFS Intl";
    case 0x444F5303: return "AmigaDOS FFS Intl";
    case 0x444F5304: return "AmigaDOS OFS DC";
    case 0x444F5305: return "AmigaDOS FFS DC";
    case 0x444F5307: return "AmigaDOS FFS LNFS";
    case 0x50465301: case 0x50465302: case 0x50465303: return "PFS3";
    case 0x50445301: case 0x50445302: case 0x50445303: return "PDS3";
    case 0x53465300: case 0x53465302: return "SFS";
    }
    return "other";
}

/* ---- what may be done ---- */
static const char *table_refused(const struct tdisk *d)
{
    switch (d->kind) {
    case TD_GPT: return "the disk has a GPT table, which this program does not change";
    case TD_HYBRID: return "the disk has both an RDB and a PC table, which this program does not change";
    default: break;
    }
    return 0;
}

const char *t_part_refused(const struct tdisk *d, int i)
{
    const struct t_part *p = &d->part[i];
    const char *t = table_refused(d);
    if (t) return t;
    if (p->busy) return "it is in use now (mounted)";
    if (!p->sectors) return "the table does not say where it is";
    if (d->noedit) return d->noedit;
    if (p->how == TP_MBR) {
        if (is_fat(p->mbrtype)) return "a FAT partition: it may hold Emu68, the Kickstart, a PC's files; never touched";
        if (p->mbrtype == 0x76) return "it holds an Amiga partition table with its own partitions; never touched as a whole";
        if (is_ext(p->mbrtype)) return "an extended partition; never touched";
        if (p->mbrtype != 0x83) return "a PC partition of a type this program does not know; never touched";
    } else {
        const char *n = d->region[(int)p->region].noedit;
        if (n) return n;
    }
    if (p->sectors < T_MINSECT) return "too small for Zenix (at least 28 MB)";
    return 0;
}

const char *t_whole_refused(const struct tdisk *d)
{
    const char *t = table_refused(d);
    if (t) return t;
    if (d->anybusy) return "something on this disk is in use (mounted)";
    if (d->hasfat) return "it has a FAT partition (Emu68, the Kickstart, a PC's files): never touched";
    if (d->sectors / RDB_CYL < 3 || d->sectors < T_MINSECT + RDB_CYL) return "the disk is too small (at least 28 MB)";
    return 0;
}

/* the largest run of free whole cylinders of an RDB, which is parsed into rw for this: 0 and lo, n; -1 none */
static int region_parse(const struct tdisk *d, const struct tio *io, int ri)
{
    const struct t_region *g = &d->region[ri];
    cio = io;
    sha256_init(&hctx);
    rd_base = g->base; rd_limit = g->limit;
    return rdb_read(rdb_rd, &rw) == 1 ? 0 : -1;
}
/* ---- the partition step: partitions marked to delete count as free space, and are left out of the table the plan writes ---- */
static const struct tdisk *cur;
static int deleted_blk(t_u32 absblk)
{
    if (!cur) return 0;
    for (int k = 0; k < cur->nparts; k++) if (cur->part[k].how == TP_RDB && cur->part[k].blk == absblk && cur->part[k].del) return 1;
    return 0;
}
int t_ndel(const struct tdisk *d)
{
    int n = 0;
    for (int i = 0; i < d->nparts; i++) n += d->part[i].del != 0;
    return n;
}
const char *t_del_refused(const struct tdisk *d, int i)
{
    const struct t_part *p = &d->part[i];
    const char *t = table_refused(d);
    if (t) return t;
    if (d->noedit) return d->noedit;
    if (p->busy) return "it is in use now (mounted)";
    if (p->how == TP_MBR) {
        if (is_fat(p->mbrtype)) return "a FAT partition (it may hold Emu68, the Kickstart, a PC's files): never deleted here";
        if (p->mbrtype == 0x76) return "it holds an Amiga partition table with its own partitions: delete those instead";
        if (is_ext(p->mbrtype)) return "an extended partition: never deleted here";
    } else {
        const char *n = d->region[(int)p->region].noedit;
        if (n) return n;
    }
    /* one table at a time: the deletions and the new partitions go into it with one write */
    for (int k = 0; k < d->nparts; k++)
        if (d->part[k].del && k != i && (d->part[k].how != p->how || d->part[k].region != p->region))
            return "partitions in another table are marked: one table at a time";
    return 0;
}

static void largest_free(const struct t_region *g, t_u32 *best_lo, t_u32 *best_n)
{
    t_u32 cyl = g->cyl;
    unsigned long long endl = ((unsigned long long)g->hicyl + 1) * cyl;
    t_u32 end = endl > g->limit ? g->limit - g->limit % cyl : (t_u32)endl;
    *best_lo = *best_n = 0;
    for (t_u32 c = g->locyl; (unsigned long long)c * cyl < end;) {
        t_u32 s = c * cyl, next = end;
        int hit = 0;
        for (int i = 0; i < rw.nparts; i++) {
            const struct rdb_part *p = &rw.part[i];
            if (!p->sectors || deleted_blk(g->base + p->blk)) continue;
            if (p->start <= s && s - p->start < p->sectors) { hit = 1; next = p->start + p->sectors; break; }
            if (p->start > s && p->start < next) next = p->start;
        }
        if (hit) { c = (t_u32)(((unsigned long long)next + cyl - 1) / cyl); continue; }
        t_u32 n = next / cyl - c;
        if (n > *best_n) { *best_n = n; *best_lo = c; }
        c += n ? n : 1;
    }
}
/* a free slot of the MBR and the largest gap that lines up on 1 MB: 0 and start, n, slot; -1 none */
static int mbr_gap(const struct tdisk *d, t_u32 *st, t_u32 *num, int *slot)
{
    *slot = -1;
    for (int i = 3; i >= 0 && *slot < 0; i--) {
        int gone = 0;
        for (int k = 0; k < d->nparts; k++) if (d->part[k].how == TP_MBR && d->part[k].slot == i && d->part[k].del) gone = 1;
        if (d->sec0[446 + 16 * i + 4] == 0 || gone) *slot = i;
    }
    if (*slot < 0) return -1;
    t_u32 best = 0, best_st = 0;
    t_u32 at = 2048;                            /* the first MB is left alone */
    for (;;) {
        if (at >= d->sectors) break;
        t_u32 next = d->sectors, jump = 0;
        for (int i = 0; i < d->nparts; i++) {
            const struct t_part *p = &d->part[i];
            if (p->how != TP_MBR || !p->sectors || p->del) continue;
            if (p->start <= at && at - p->start < p->sectors) jump = p->start + p->sectors;
            else if (p->start > at && p->start < next) next = p->start;
        }
        if (jump) { at = jump; if (at % 2048) at += 2048 - at % 2048; if (at >= d->sectors) break; continue; }
        t_u32 n = next - at;
        if (n > best) { best = n; best_st = at; }
        if (next >= d->sectors) break;
        at = next;
    }
    best -= best % 2048;
    *st = best_st; *num = best;
    return best ? 0 : -1;
}

int t_options(const struct tdisk *d, const struct tio *io, struct t_opt *o, int max)
{
    int n = 0, nd = t_ndel(d), delreg = -2;
    if (table_refused(d)) return 0;
    cur = d;
    for (int i = 0; i < d->nparts; i++) if (d->part[i].del) delreg = d->part[i].how == TP_MBR ? -1 : d->part[i].region;
    if (d->kind == TD_MBR || d->kind == TD_RDB) {
        for (int r = 0; r < d->nregions && n < max; r++) {
            const struct t_region *g = &d->region[r];
            if (g->noedit || d->noedit) continue;
            if (nd && delreg != r) continue;
            if (region_parse(d, io, r)) continue;
            t_u32 lo, cnt;
            largest_free(g, &lo, &cnt);
            if ((unsigned long long)cnt * g->cyl >= T_MINSECT)
                o[n++] = (struct t_opt){ TO_FREE, r, -1, g->base + lo * g->cyl, cnt * g->cyl };
        }
        /* after the free cylinders of a table that is there: a second 0x76 has not been seen working under Emu68 */
        if (d->kind == TD_MBR && !d->noedit && (!nd || delreg == -1)) {
            t_u32 st, num; int slot;
            if (mbr_gap(d, &st, &num, &slot) == 0 && num >= T_MINSECT && n < max)
                o[n++] = (struct t_opt){ TO_NEWCONT, -1, -1, st, num };
        }
    }
    for (int i = 0; i < d->nparts && n < max && !nd; i++)
        if (!t_part_refused(d, i))
            o[n++] = (struct t_opt){ TO_PART, d->part[i].region, i, d->part[i].start, d->part[i].sectors };
    if (!nd && !t_whole_refused(d) && n < max)
        o[n++] = (struct t_opt){ TO_WHOLE, -1, -1, 0, d->sectors };
    return n;
}

/* ---- the plan ---- */
static void labels(const struct tdisk *d, struct t_plan *p)
{
    for (int k = 0; k < 10; k++) {
        const char c[3] = { 'B', 'R', 'H' };
        int clash = 0;
        for (int r = 0; r < 3; r++) {
            p->nw[r].label[0] = 'Z'; p->nw[r].label[1] = c[r]; p->nw[r].label[2] = (char)('0' + k); p->nw[r].label[3] = 0;
            for (int i = 0; i < d->nparts; i++) if (!d->part[i].del && !strcmp(d->part[i].label, p->nw[r].label)) clash = 1;   /* the chosen one too: its name may stay */
        }
        if (!clash) return;
    }
}

/* sizes in cylinders, the note's rules (section 3): the boot partition by the space; then the layout asked for. Returns the cylinders
 * used (all of avail but for a custom layout), 0 with *why when it does not fit */
static const struct t_want *want;
static t_u32 sizes(struct t_plan *p, t_u32 avail, t_u32 cyl, const char **why)
{
    unsigned long long tot = (unsigned long long)avail * cyl;
    t_u32 mb = (t_u32)(tot / 2048);
    t_u32 bootmb = mb >= 256 ? 16 : mb >= 64 ? 8 : 4;
    t_u32 bootc = (t_u32)(((unsigned long long)bootmb * 2048 + cyl - 1) / cyl);
    if (bootc == 0) bootc = 1;
    if (bootc >= avail) { *why = "less than 28 MB of room"; return 0; }
    t_u32 rest = avail - bootc;
    t_u32 rootc, homec = 0;
    t_u32 restmb = (t_u32)((unsigned long long)rest * cyl / 2048);
    if (want->layout == TL_CUSTOM) {
        rootc = (t_u32)(((unsigned long long)want->root + cyl - 1) / cyl);
        int all = want->home == T_HOME_REST;
        homec = all ? 0 : want->home ? (t_u32)(((unsigned long long)want->home + cyl - 1) / cyl) : 0;
        if (want->root < T_MINROOT || (want->home && !all && want->home < T_MINHOME)) { *why = "a root of at least 32 MB, and a /home of at least 4 MB or none"; return 0; }
        if (rootc < rest && (all || (homec && rootc + homec == rest + 1))) homec = rest - rootc;    /* the rest, or a cylinder's rounding */
        if ((unsigned long long)homec * cyl < T_MINHOME) homec = 0;
        if ((unsigned long long)rootc + homec > rest) { *why = "the sizes asked for do not fit the space"; return 0; }
    } else if (want->layout == TL_ONE || mb < 64) {
        rootc = rest;
    } else {
        /* recommended: a 1 GB root from 3 GB of space up, else a third of it (at least 128 MB, two thirds of a small space at
         * most); /home the rest (none under 4 MB) */
        t_u32 rmb = restmb >= 3072 ? 1024 : restmb / 3;
        if (rmb < 128) rmb = 128;
        if (rmb > restmb * 2 / 3 && restmb < 192) rmb = restmb * 2 / 3;
        rootc = (t_u32)(((unsigned long long)rmb * 2048 + cyl - 1) / cyl);
        if (rootc > rest) rootc = rest;
        homec = rest - rootc;
        if ((unsigned long long)homec * cyl < T_MINHOME) { homec = 0; rootc = rest; }
    }
    p->nw[R_BOOT].sectors = bootc * cyl;
    p->nw[R_ROOT].sectors = rootc * cyl;
    p->nw[R_HOME].sectors = homec * cyl;
    p->nnew = homec ? 3 : 2;
    return bootc + rootc + homec;
}
/* the sizes make a Zenix: a table's cylinders may be so big that they do not */
static const char *sizes_bad(const struct t_plan *p)
{
    if (p->nw[R_ROOT].sectors < 16u * 2048) return "this table's cylinders are too big for a root of at least 16 MB";
    if (p->nw[R_BOOT].sectors > 25u * 4064) return "this table's cylinders are too big for an AmigaDOS boot partition (at most 50 MB here)";
    return 0;
}

static void set_label(t_u32 *b, const char *name)
{
    unsigned l = 0;
    while (name[l] && l < 31) { rdb_setbyte(b, 37 + l, (unsigned char)name[l]); l++; }
    rdb_setbyte(b, 36, l);
}
/* a PART block in the given geometry */
static void part_block(t_u32 *b, const struct t_newp *n, t_u32 heads, t_u32 secs, t_u32 next, t_u32 flags, int pri)
{
    for (int i = 0; i < 128; i++) b[i] = 0;
    b[0] = RDB_ID_PART; b[1] = 64; b[3] = 7;
    b[4] = next; b[5] = flags;
    set_label(b, n->label);
    t_u32 *env = &b[32];
    env[0] = 16; env[1] = 128; env[3] = heads; env[4] = 1; env[5] = secs; env[6] = 2;
    env[9] = n->lo; env[10] = n->hi; env[11] = 30;
    env[13] = (n->dostype >> 8) == 0x444F53u ? MAXTRANSFER_DOS : 0x7FFFFFFFu;
    env[14] = 0xFFFFFFFEu;
    env[15] = (t_u32)pri;
    env[16] = n->dostype;
    rdb_sum(b);
}
static int add_write(struct t_plan *p, t_u32 lba, const t_u32 *longs, const t_u8 *bytes, int flush, const t_u8 *old)
{
    if (p->nw1 == T_MAXW) return -1;
    struct t_write *w = &p->w1[p->nw1++];
    w->lba = lba; w->flush = flush;
    if (longs) to_bytes(w->data, longs); else memcpy(w->data, bytes, 512);
    memcpy(w->old, old, 512);
    return 0;
}
/* read one sector for a plan (what it holds must still be there when it is written) */
static int plan_write(struct t_plan *p, const struct tio *io, t_u32 lba, const t_u32 *longs, const t_u8 *bytes, int flush)
{
    t_u8 old[512];
    if (io->rd(io->ctx, lba, 1, old)) return -1;
    return add_write(p, lba, longs, bytes, flush, old);
}

/* a fresh RDB at base: RDSK at block rdsk, the PARTs at blocks[], all of cylinder 0 ours */
static int fresh_rdb(struct t_plan *p, const struct tio *io, t_u32 base, t_u32 len, const t_u32 *blocks, t_u32 rdsk)
{
    static t_u32 b[128];
    t_u32 cyls = len / RDB_CYL;
    t_u32 high = 0;
    for (int i = 0; i < p->nnew; i++) {
        if (blocks[i] > high) high = blocks[i];
        part_block(b, &p->nw[i], RDB_HEADS, RDB_SECS, i + 1 < p->nnew ? blocks[i + 1] : 0xFFFFFFFFu, FLG_NOMOUNT, 0);
        if (plan_write(p, io, base + blocks[i], b, 0, i + 1 == p->nnew)) return -1;     /* whole on the disk before the RDSK block names them */
    }
    for (int i = 0; i < 128; i++) b[i] = 0;
    b[0] = RDB_ID_RDSK; b[1] = 64; b[3] = 7; b[4] = 512; b[5] = 0x12;
    for (int i = 6; i < 16; i++) b[i] = 0xFFFFFFFFu;
    b[7] = blocks[0];
    b[16] = cyls; b[17] = RDB_SECS; b[18] = RDB_HEADS;
    b[20] = cyls; b[24] = b[25] = b[26] = cyls;
    b[32] = 0; b[33] = RDB_CYL - 1; b[34] = 1; b[35] = cyls - 1;
    b[36] = RDB_CYL; b[38] = high;
    rdb_sum(b);
    return plan_write(p, io, base + rdsk, b, 0, 1);
}

static void place(struct t_plan *p, t_u32 base, t_u32 lo, t_u32 cyl, int zenixfs, int homefs)
{
    t_u32 c = lo;
    for (int i = 0; i < p->nnew; i++) {
        struct t_newp *n = &p->nw[i];
        t_u32 cnt = n->sectors / cyl;
        n->lo = c; n->hi = c + cnt - 1; n->start = base + c * cyl;
        n->dostype = i == R_BOOT ? 0x444F5301u : ((i == R_ROOT && zenixfs) || (i == R_HOME && homefs)) ? DT_ZENIXFS : DT_EXT2;
        c += cnt;
    }
}

/* can bootmenu name this partition? It trims a line's ends and stops a name at a colon (boot/bootmenu.c); a name it cannot find would
 * boot the menu again and again */
static int menu_name_ok(const char *n)
{
    size_t l = strlen(n);
    if (!l || n[l - 1] == ' ' || n[0] == ' ') return 0;
    for (size_t i = 0; i < l; i++) if ((t_u8)n[i] < 32 || n[i] == ':' || n[i] == '=' || n[i] == '#') return 0;
    return 1;
}

/* the boot priority, and the AmigaOS partition the menu offers */
static void find_aos(const struct tdisk *d, struct t_plan *p)
{
    int best = -129, maxpri = -129;
    for (int i = 0; i < d->nparts; i++) {
        const struct t_part *q = &d->part[i];
        if (q->how != TP_RDB || !q->bootable || q->nomount || i == p->opt.part || q->pri == -128) continue;
        if (i == d->zb || i == d->zr || i == d->zh || q->del) continue;     /* an earlier Zenix, or going: not what this disk "starts by itself" */
        if (q->pri > maxpri) maxpri = q->pri;
        if (q->dostype == DT_EXT2 || q->dostype == DT_ZENIXFS) continue;
        /* whatever this disk starts by itself today is what the menu's default goes on starting (it may be a menu of its own) */
        if (q->pri > best) { best = q->pri; strncpy(p->aos, q->label, sizeof p->aos - 1); }
    }
    if (p->opt.kind == TO_WHOLE) { p->aos[0] = 0; maxpri = -129; }
    p->pri = maxpri == -129 ? 0 : maxpri >= 127 ? 127 : maxpri + 1;
}

/* whole disk: blocks of the first cylinder that neither the old table nor old partitions use. 1 all found. */
static int whole_blocks(const struct tdisk *d, const struct tio *io, int want, t_u32 *out)
{
    int have_rdb = d->kind == TD_RDB && !region_parse(d, io, 0);
    if (d->kind != TD_RDB && d->kind != TD_MBR && d->kind != TD_BLANK) return 0;
    if (d->kind == TD_RDB && (!have_rdb || d->region[0].noedit)) return 0;
    int n = 0;
    for (t_u32 b = 1; b < RDB_CYL && n < want; b++) {
        int bad = 0;
        if (have_rdb) for (unsigned i = 0; i < rw.nused; i++) if (rw.used[i] == b) bad = 1;
        for (int i = 0; i < d->nparts; i++)
            if (d->part[i].sectors && overlap(b, 1, d->part[i].start, d->part[i].sectors)) bad = 1;
        if (!bad) out[n++] = b;
    }
    return n == want;
}

/* Partitions deleted and Zenix's added in one table: every kept PART block copied as it is (only its next changes) and the new ones,
 * into blocks of the RDB area nothing uses; then one write, the RDSK block's list (and HighRDSKBlock, RDBBlocksHi) names the new chain.
 * Before it the old chain is whole and untouched; after it the old blocks are unused. rw holds the region (parsed by the caller). */
static int rewrite_chain(const struct tdisk *d, const struct tio *io, const struct t_region *g, struct t_plan *p, const char **why)
{
    (void)d;
    static t_u32 q[128], b[128];
    static t_u8 old[512];
    int keep[RDB_MAXPART], nk = 0;
    for (int i = 0; i < rw.nparts; i++) if (!deleted_blk(g->base + rw.part[i].blk)) keep[nk++] = i;
    int need = nk + p->nnew, got = 0;
    static t_u32 fresh[RDB_MAXPART + 3];
    if (need > RDB_MAXPART + 3 || need + 1 > T_MAXW) { *why = "too many partitions in this table to write it anew here"; return -1; }
    for (t_u32 nb = rw.blk + 1; nb <= rw.rdbhi && got < need; nb++) {
        int used = 0;
        for (unsigned i = 0; i < rw.nused; i++) if (rw.used[i] == nb) used = 1;
        if (!used && nb < g->limit) fresh[got++] = nb;
    }
    t_u32 newhi = rw.rdbhi;
    if (got < need) {
        unsigned long long lim = (unsigned long long)g->locyl * g->cyl;
        for (int i = 0; i < rw.nparts; i++) if (rw.part[i].sectors && rw.part[i].start < lim) lim = rw.part[i].start;
        if (lim > g->limit) lim = g->limit;
        for (t_u32 nb = rw.rdbhi + 1; nb < lim && got < need; nb++) { fresh[got++] = nb; newhi = nb; }
    }
    if (got < need) { *why = "no free blocks left in the RDB area for the new table"; return -1; }
    t_u32 high = 0;
    for (int i = 0; i < need; i++) if (fresh[i] > high) high = fresh[i];
    for (int i = 0; i < need; i++) {
        t_u32 next = i + 1 < need ? fresh[i + 1] : 0xFFFFFFFFu;
        if (i < nk) {                                   /* a kept partition: its block as it is, but where the chain goes on */
            t_u8 pb[512];
            if (io->rd(io->ctx, g->base + rw.part[keep[i]].blk, 1, pb)) { *why = "cannot read the table"; return -1; }
            to_longs(q, pb);
            if (q[0] != RDB_ID_PART || !rdb_sum_ok(q)) { *why = "a block of the table does not add up"; return -1; }
            q[4] = next;
            rdb_sum(q);
        } else {
            int k = i - nk;
            part_block(q, &p->nw[k], g->heads, g->secs, next, FLG_NOMOUNT, 0);
            p->nw[k].blk = g->base + fresh[i];
        }
        if (plan_write(p, io, g->base + fresh[i], q, 0, i + 1 == need)) { *why = "cannot read the table"; return -1; }
    }
    if (io->rd(io->ctx, g->base + rw.blk, 1, old)) { *why = "cannot read the table"; return -1; }
    to_longs(b, old);
    b[7] = need ? g->base * 0 + fresh[0] : 0xFFFFFFFFu;
    if (high > b[38]) b[38] = high;
    b[33] = newhi;
    rdb_sum(b);
    p->commit1 = p->nw1;
    if (add_write(p, g->base + rw.blk, b, 0, 1, old)) { *why = "too many writes"; return -1; }
    return 0;
}

int t_plan_make(const struct tdisk *d, const struct tio *io, const struct t_opt *o, t_u32 maxsect, const struct t_want *w,
                struct t_plan *p, const char **why)
{
    static t_u32 b[128];
    static t_u8 old[512];
    memset(p, 0, sizeof *p);
    p->opt = *o;
    int zenixfs = w->rootfs, homefs = w->homefs;
    want = w;
    cur = d;
    p->zenixfs = zenixfs; p->homefs = homefs;
    p->atomic = 1;
    *why = 0;
    if (table_refused(d)) { *why = table_refused(d); return -1; }
    t_u32 space = o->sectors;
    if (maxsect && maxsect < space) space = maxsect;
    if (space < T_MINSECT) { *why = "less than 28 MB of room"; return -1; }
    /* Zenix takes the first root marked bootable (user/rootpick.h): another one marked would stay the machine's root */
    if (o->kind != TO_WHOLE)
        for (int i = 0; i < d->nparts; i++) {
            const struct t_part *q = &d->part[i];
            int root = q->how == TP_MBR ? q->mbrtype == 0x83 : q->dostype == DT_EXT2 || q->dostype == DT_ZENIXFS;
            if (root && q->bootable && i != o->part && i != d->zr && !q->del) {      /* (the old Zenix's root: its mark is taken off at the end) */
                *why = "another partition is marked as the root Zenix starts from, and would stay the root: take its mark off first (fdisk boot N off), or choose that partition";
                return -1;
            }
        }
    labels(d, p);
    find_aos(d, p);
    if (p->aos[0] && !menu_name_ok(p->aos)) { *why = "the partition this disk starts from has a name the boot menu cannot use (a space at its end, a colon): rename it first"; return -1; }
    if (o->kind != TO_WHOLE && d->nparts + 3 > 14) { *why = "too many partitions on this disk: Zenix looks at the first 14 when it picks its root"; return -1; }
    t_u32 blocks[3];
    switch (o->kind) {
    case TO_NEWCONT: case TO_PART:
        if (o->kind == TO_PART && d->part[o->part].how == TP_RDB) break;
        {
            if (o->kind == TO_NEWCONT && d->noedit) { *why = d->noedit; return -1; }
            if (o->kind == TO_PART && t_part_refused(d, o->part)) { *why = t_part_refused(d, o->part); return -1; }
            t_u32 base = o->start, len = o->kind == TO_NEWCONT ? space - space % 2048 : o->sectors;
            t_u32 avail = (space < len ? space : len) / RDB_CYL - BOOTNEW;
            t_u32 used = sizes(p, avail, RDB_CYL, why);
            if (!used) return -1;
            if (sizes_bad(p)) { *why = sizes_bad(p); return -1; }
            if (o->kind == TO_NEWCONT && used < avail) {        /* a custom layout: the container as big as what it holds, the rest stays free */
                unsigned long long l = ((unsigned long long)used + BOOTNEW) * RDB_CYL;
                l = (l + 2047) / 2048 * 2048;
                if (l < len) len = (t_u32)l;
            }
            place(p, base, BOOTNEW, RDB_CYL, zenixfs, homefs);
            p->cyl = RDB_CYL;
            for (int i = 0; i < p->nnew; i++) { p->nw[i].blk = base + 1 + (t_u32)i; blocks[i] = 1 + (t_u32)i; }
            if (fresh_rdb(p, io, base, len, blocks, 0)) { *why = "cannot read the disk"; return -1; }
            /* the one write: the MBR sector */
            static t_u8 m[512];
            memcpy(m, d->sec0, 512);
            for (int k = 0; k < d->nparts; k++) if (d->part[k].how == TP_MBR && d->part[k].del) memset(m + 446 + 16 * d->part[k].slot, 0, 16);
            int slot = o->kind == TO_PART ? d->part[o->part].slot : -1;
            if (o->kind == TO_NEWCONT) for (int i = 3; i >= 0 && slot < 0; i--) if (m[446 + 16 * i + 4] == 0) slot = i;   /* the last: the others keep their numbers, also under a driver that counts */
            if (slot < 0) { *why = "no free entry in the PC partition table"; return -1; }
            t_u8 *e = m + 446 + 16 * slot;
            if (o->kind == TO_NEWCONT) {
                e[0] = 0; e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
                put_le32(e + 8, base); put_le32(e + 12, len);
                if (m[510] != 0x55 || m[511] != 0xAA) { *why = "no table"; return -1; }
            }
            e[4] = 0x76;
            p->commit1 = p->nw1;
            if (plan_write(p, io, 0, 0, m, 1)) { *why = "cannot read the disk"; return -1; }
        }
        goto stage2;
    case TO_WHOLE:
        if (t_whole_refused(d)) { *why = t_whole_refused(d); return -1; }
        {
            t_u32 len = space - space % RDB_CYL;
            t_u32 avail = len / RDB_CYL - BOOTNEW;
            if (!sizes(p, avail, RDB_CYL, why)) return -1;
            if (sizes_bad(p)) { *why = sizes_bad(p); return -1; }
            place(p, 0, BOOTNEW, RDB_CYL, zenixfs, homefs);
            p->cyl = RDB_CYL;
            if (!whole_blocks(d, io, p->nnew, blocks)) {
                p->atomic = 0;
                for (int i = 0; i < p->nnew; i++) blocks[i] = 1 + (t_u32)i;
            }
            for (int i = 0; i < p->nnew; i++) p->nw[i].blk = blocks[i];
            if (fresh_rdb(p, io, 0, d->sectors - d->sectors % RDB_CYL, blocks, 0)) { *why = "cannot read the disk"; return -1; }
            p->commit1 = p->nw1 - 1;
        }
        goto stage2;
    case TO_FREE:
        break;
    default:
        *why = "not a kind of place this plans";
        return -1;
    }
    /* an existing RDB: free cylinders, or a chosen partition in it */
    {
        int ri = o->region;
        if (ri < 0 || ri >= d->nregions) { *why = "no such table"; return -1; }
        const struct t_region *g = &d->region[ri];
        if (g->noedit) { *why = g->noedit; return -1; }
        if (d->noedit) { *why = d->noedit; return -1; }
        if (o->kind == TO_PART && t_part_refused(d, o->part)) { *why = t_part_refused(d, o->part); return -1; }
        if (region_parse(d, io, ri)) { *why = "cannot read the table"; return -1; }
        t_u32 lo, cnt;
        if (o->kind == TO_FREE) {
            if ((o->start - g->base) % g->cyl) { *why = "the space is not on a cylinder"; return -1; }
            lo = (o->start - g->base) / g->cyl;
            cnt = o->sectors / g->cyl;
            /* still free? */
            for (int i = 0; i < rw.nparts; i++)
                if (rw.part[i].sectors && !deleted_blk(g->base + rw.part[i].blk) && overlap(rw.part[i].start, rw.part[i].sectors, lo * g->cyl, cnt * g->cyl)) {
                    *why = "the free space is not free any more"; return -1;
                }
        } else {
            const struct t_part *q = &d->part[o->part];
            t_u32 st = q->start - g->base;
            lo = (st + g->cyl - 1) / g->cyl;
            t_u32 hi = (st + q->sectors) / g->cyl;
            if (lo < g->locyl) lo = g->locyl;
            if (hi > g->hicyl + 1) hi = g->hicyl + 1;
            if (hi <= lo) { *why = "the partition does not hold a whole cylinder of this table"; return -1; }
            cnt = hi - lo;
        }
        unsigned long long sp = (unsigned long long)cnt * g->cyl;
        t_u32 pend = 0, full = cnt;                         /* a split: the chosen partition's end, in sectors of the region */
        if (sp > space) cnt = space / g->cyl;
        if ((unsigned long long)cnt * g->cyl < T_MINSECT) { *why = "less than 28 MB of room"; return -1; }
        t_u32 used = sizes(p, cnt, g->cyl, why);
        if (!used) return -1;
        if (sizes_bad(p)) { *why = sizes_bad(p); return -1; }
        if (o->kind == TO_PART && used < full) {            /* Zenix takes less than all of it: the rest stays a partition of the old name */
            p->keep = 1; pend = d->part[o->part].start - g->base + d->part[o->part].sectors;
        }
        cnt = used;
        place(p, g->base, lo, g->cyl, zenixfs, homefs);
        /* Kickstart 3.1's own disk code does not reach past 4 GB; with the highest boot priority a partition there could be read at wrapped places. Under Emu68 (an RDB in a 0x76) it is the driver's */
        if (g->base == 0 && d->kind == TD_RDB && (unsigned long long)p->nw[R_BOOT].start + p->nw[R_BOOT].sectors > 8388608ull) {
            *why = "the free space lies past 4 GB, where Kickstart 3.1 cannot start from: leave room for the boot partition below 4 GB";
            return -1;
        }
        p->cyl = g->cyl;
        if (t_ndel(d)) {                                    /* the partition step: the whole chain anew, one write names it */
            if (rewrite_chain(d, io, g, p, why)) return -1;
            goto stage2;
        }
        /* the blocks for the new PARTs: unused blocks of the RDB area */
        int need = (o->kind == TO_PART ? p->nnew - 1 : p->nnew) + p->keep, got = 0;
        t_u32 fresh[4];
        for (t_u32 nb = rw.blk + 1; nb <= rw.rdbhi && got < need; nb++) {
            int used = 0;
            for (unsigned i = 0; i < rw.nused; i++) if (rw.used[i] == nb) used = 1;
            if (!used && nb < g->limit) fresh[got++] = nb;
        }
        /* an RDB area kept tight (tools that wrote just enough): it may grow into the rest of the cylinders before the first partition,
         * which no table refers to and no partition holds; the RDSK block says so first, harmless alone */
        t_u32 newhi = rw.rdbhi;
        if (got < need) {
            unsigned long long lim = (unsigned long long)g->locyl * g->cyl;
            for (int i = 0; i < rw.nparts; i++) if (rw.part[i].sectors && rw.part[i].start < lim) lim = rw.part[i].start;
            if (lim > g->limit) lim = g->limit;
            for (t_u32 nb = rw.rdbhi + 1; nb < lim && got < need; nb++) { fresh[got++] = nb; newhi = nb; }
        }
        if (got < need) { *why = "no free block left in the RDB area for the new partitions"; return -1; }
        t_u32 oldnext = 0xFFFFFFFFu, high;
        /* the RDSK block as it is */
        if (io->rd(io->ctx, g->base + rw.blk, 1, old)) { *why = "cannot read the table"; return -1; }
        to_longs(b, old);
        high = b[38];
        t_u32 newhigh = high;
        for (int i = 0; i < got; i++) if (fresh[i] > newhigh) newhigh = fresh[i];
        static t_u32 keepb[128];
        if (o->kind == TO_PART) {
            const struct t_part *q = &d->part[o->part];
            t_u8 pb[512];
            if (io->rd(io->ctx, q->blk, 1, pb)) { *why = "cannot read the table"; return -1; }
            oldnext = be32(pb + 16);
            if (p->keep) {          /* the rest: the old PART block as it is (name, dostype, environment, its filesystem's chain untouched), its own cylinders */
                to_longs(keepb, pb);
                if (keepb[0] != RDB_ID_PART || !rdb_sum_ok(keepb)) { *why = "the chosen partition's block does not add up"; return -1; }
                t_u32 *env = &keepb[32];
                unsigned long long pcyl = (unsigned long long)env[3] * env[5] * (env[1] / 128);
                t_u32 zend = (lo + cnt) * g->cyl;
                if (!pcyl || env[1] % 128) { *why = "the chosen partition's geometry cannot be split"; return -1; }
                t_u32 rlo = (t_u32)((zend + pcyl - 1) / pcyl), rhi = env[10];
                if ((unsigned long long)rlo * pcyl + 16 * 2048 > pend || rhi < rlo) { *why = "what would be left of the partition is under 16 MB: give Zenix all of it, or less"; return -1; }
                keepb[5] &= ~(FLG_BOOT | FLG_NOMOUNT);   /* mountable, not bootable */
                env[9] = rlo;
                strncpy(p->rest.label, q->label, sizeof p->rest.label - 1);
                p->rest.lo = rlo; p->rest.hi = rhi;
                p->rest.start = g->base + (t_u32)(rlo * pcyl);
                p->rest.sectors = (t_u32)(((unsigned long long)rhi - rlo + 1) * pcyl);
                p->rest.dostype = env[16];
            }
            t_u32 ours = q->blk - g->base;
            blocks[0] = ours;
            for (int i = 1; i < p->nnew; i++) blocks[i] = fresh[i - 1];
        } else {
            for (int i = 0; i < p->nnew; i++) blocks[i] = fresh[i];
        }
        for (int i = 0; i < p->nnew; i++) p->nw[i].blk = g->base + blocks[i];
        t_u32 keepblk = p->keep ? fresh[got - 1] : 0;
        if (p->keep) {
            p->rest.blk = g->base + keepblk;
            keepb[4] = oldnext;                 /* the chain: Zenix's, then the rest, then what followed the chosen one */
            oldnext = keepblk;
            rdb_sum(keepb);
            if (plan_write(p, io, g->base + keepblk, keepb, 0, 0)) { *why = "cannot read the table"; return -1; }
        }
        int link_in_rdsk = o->kind == TO_FREE && rw.nparts == 0;
        if ((newhigh != high || newhi != rw.rdbhi) && !link_in_rdsk) {             /* harmless alone, and before anything needs it */
            b[38] = newhigh;
            b[33] = newhi;
            rdb_sum(b);
            if (add_write(p, g->base + rw.blk, b, 0, 1, old)) { *why = "too many writes"; return -1; }
        }
        for (int i = p->nnew - 1; i >= (o->kind == TO_PART ? 1 : 0); i--) {
            static t_u32 q[128];
            part_block(q, &p->nw[i], g->heads, g->secs, i + 1 < p->nnew ? blocks[i + 1] : oldnext, FLG_NOMOUNT, 0);
            if (plan_write(p, io, g->base + blocks[i], q, 0, 0)) { *why = "cannot read the table"; return -1; }
        }
        if (p->nw1) p->w1[p->nw1 - 1].flush = 1;
        p->commit1 = p->nw1;
        if (o->kind == TO_PART) {                           /* the chosen PART block becomes the first new one */
            static t_u32 q[128];
            part_block(q, &p->nw[0], g->heads, g->secs, p->nnew > 1 ? blocks[1] : oldnext, FLG_NOMOUNT, 0);
            if (plan_write(p, io, g->base + blocks[0], q, 0, 1)) { *why = "cannot read the table"; return -1; }
        } else if (rw.nparts) {                             /* the last PART's "next" */
            const struct rdb_part *last = &rw.part[rw.nparts - 1];
            t_u8 pb[512];
            if (io->rd(io->ctx, g->base + last->blk, 1, pb)) { *why = "cannot read the table"; return -1; }
            static t_u32 q[128];
            to_longs(q, pb);
            if (q[0] != RDB_ID_PART || !rdb_sum_ok(q) || q[4] != 0xFFFFFFFFu) { *why = "the table's last partition does not end the list"; return -1; }
            q[4] = blocks[0];
            rdb_sum(q);
            if (add_write(p, g->base + last->blk, q, 0, 1, pb)) { *why = "too many writes"; return -1; }
        } else {                                            /* an empty table: the RDSK block's list */
            b[7] = blocks[0];
            b[38] = newhigh;
            b[33] = newhi;
            rdb_sum(b);
            if (add_write(p, g->base + rw.blk, b, 0, 1, old)) { *why = "too many writes"; return -1; }
        }
    }
stage2:
    /* stage 2: only blocks that are ours. First the root (Zenix's rule for the root: bootable wins), the boot partition last */
    {
        static t_u32 q[128];
        /* the new blocks as stage 1 wrote them are in w1 */
        for (int r = 0; r < 2; r++) {
            int who = r == 0 ? R_ROOT : R_BOOT;
            if (r == 1 && d->zr >= 0 && p->opt.kind != TO_REINSTALL)     /* elsewhere: the old Zenix's marks off, so the new one is taken */
                for (int k = 0; k < 2; k++) {
                    int i = k == 0 ? d->zr : d->zb;
                    if (i < 0 || i == p->opt.part || !d->part[i].bootable) continue;
                    struct t_write *w = &p->w2[p->nw2++];
                    w->lba = d->part[i].blk; w->flush = 1;
                    if (io->rd(io->ctx, w->lba, 1, w->old)) { *why = "cannot read the table"; return -1; }
                    to_longs(q, w->old);
                    if (q[0] != RDB_ID_PART || !rdb_sum_ok(q)) { *why = "the old Zenix's table block does not add up"; return -1; }
                    q[5] &= ~FLG_BOOT;
                    rdb_sum(q);
                    to_bytes(w->data, q);
                }
            struct t_write *w = &p->w2[p->nw2++];
            w->lba = p->nw[who].blk; w->flush = 1;
            const struct t_write *src = 0;
            for (int i = p->nw1 - 1; i >= 0; i--) if (p->w1[i].lba == w->lba) { src = &p->w1[i]; break; }
            if (!src) { *why = "internal: a new block is not in the plan"; return -1; }
            memcpy(w->old, src->data, 512);
            to_longs(q, src->data);
            if (r == 0) q[5] = FLG_BOOT | FLG_NOMOUNT;
            else { q[5] = FLG_BOOT; q[32 + 15] = (t_u32)p->pri; }
            rdb_sum(q);
            to_bytes(w->data, q);
        }
    }
    return 0;
}

int t_commit(const struct tio *io, struct t_write *w, int n, const char **why)
{
    *why = 0;
    for (int i = 0; i < n; i++) {
        if (io->rd(io->ctx, w[i].lba, 1, sec)) { *why = "cannot read a sector of the table"; return -1; }
        if (memcmp(sec, w[i].old, 512)) { *why = "a sector of the table has changed since the plan: stopped"; return -1; }
        if (io->wr(io->ctx, w[i].lba, 1, w[i].data)) { *why = "cannot write a sector of the table"; return -1; }
        if (w[i].flush && io->fl(io->ctx)) { *why = "the disk cannot make the write durable"; return -1; }
        if (io->rd(io->ctx, w[i].lba, 1, sec) || memcmp(sec, w[i].data, 512)) { *why = "a sector of the table does not read back as written"; return -1; }
    }
    return 0;
}

/* ---- a Zenix that is there already ---- */
static int zset_ok(const struct tdisk *d, const char **why)
{
    if (d->zr < 0 || d->zb < 0) { *why = "no Zenix of zinstall's found on this disk"; return 0; }
    const struct t_region *g = &d->region[(int)d->part[d->zr].region];
    if (g->noedit) { *why = g->noedit; return 0; }
    if (d->noedit) { *why = d->noedit; return 0; }
    for (int i = 0; i < 3; i++) {
        int k = i == 0 ? d->zb : i == 1 ? d->zr : d->zh;
        if (k >= 0 && d->part[k].busy) { *why = "the Zenix on this disk is in use now (mounted): start the installer from the floppy or from AmigaOS"; return 0; }
    }
    return 1;
}

int t_plan_reinstall(const struct tdisk *d, const struct tio *io, const struct t_want *w, int erase_home, struct t_plan *p, const char **why)
{
    static t_u32 q[128];
    memset(p, 0, sizeof *p);
    p->opt = (struct t_opt){ TO_REINSTALL, d->zr >= 0 ? d->part[d->zr].region : -1, d->zr, 0, 0 };
    *why = 0;
    if (!zset_ok(d, why)) return -1;
    cio = io;
    p->atomic = 1;
    p->zenixfs = w->rootfs;
    p->erase_home = erase_home && d->zh >= 0;
    p->homefs = d->zh >= 0 ? (p->erase_home ? w->homefs : d->part[d->zh].dostype == DT_ZENIXFS) : 0;
    int idx[3] = { d->zb, d->zr, d->zh };
    p->nnew = d->zh >= 0 ? 3 : 2;
    find_aos(d, p);
    p->pri = d->part[d->zb].pri;
    if (p->aos[0] && !menu_name_ok(p->aos)) { *why = "the partition this disk starts from has a name the boot menu cannot use (a space at its end, a colon): rename it first"; return -1; }
    for (int i = 0; i < p->nnew; i++) {
        const struct t_part *x = &d->part[idx[i]];
        struct t_newp *n = &p->nw[i];
        strncpy(n->label, x->label, sizeof n->label - 1);
        n->start = x->start; n->sectors = x->sectors; n->blk = x->blk;
        n->lo = x->lowcyl; n->hi = x->highcyl;
        n->dostype = i == R_BOOT ? 0x444F5301u : i == R_ROOT ? (p->zenixfs ? DT_ZENIXFS : DT_EXT2) : (p->homefs ? DT_ZENIXFS : DT_EXT2);
    }
    p->cyl = d->region[(int)d->part[d->zr].region].cyl;
    /* stage 1: the boot partition unbootable first, then the root, and /home if its type changes; their own blocks only */
    for (int i = 0; i < p->nnew; i++) {
        t_u8 old[512];
        if (io->rd(io->ctx, p->nw[i].blk, 1, old)) { *why = "cannot read the table"; return -1; }
        to_longs(q, old);
        if (q[0] != RDB_ID_PART || !rdb_sum_ok(q)) { *why = "a block of the Zenix's table does not add up"; return -1; }
        if (i == R_HOME && q[32 + 16] == p->nw[i].dostype) continue;            /* /home as it is */
        if (i != R_HOME) q[5] = FLG_NOMOUNT; else q[5] |= FLG_NOMOUNT;
        q[32 + 16] = p->nw[i].dostype;
        rdb_sum(q);
        if (add_write(p, p->nw[i].blk, q, 0, 1, old)) { *why = "too many writes"; return -1; }
    }
    p->commit1 = 0;
    /* stage 2: the same rule as an install's (root marked, the boot partition last) */
    for (int r = 0; r < 2; r++) {
        int who = r == 0 ? R_ROOT : R_BOOT;
        struct t_write *wr2 = &p->w2[p->nw2++];
        wr2->lba = p->nw[who].blk; wr2->flush = 1;
        const struct t_write *src = 0;
        for (int i = p->nw1 - 1; i >= 0; i--) if (p->w1[i].lba == wr2->lba) { src = &p->w1[i]; break; }
        if (!src) { *why = "internal: a block is not in the plan"; return -1; }
        memcpy(wr2->old, src->data, 512);
        to_longs(q, src->data);
        if (r == 0) q[5] = FLG_BOOT | FLG_NOMOUNT;
        else { q[5] = FLG_BOOT; q[32 + 15] = (t_u32)p->pri; }
        rdb_sum(q);
        to_bytes(wr2->data, q);
    }
    return 0;
}

int t_plan_remove(const struct tdisk *d, const struct tio *io, struct t_plan *p, const char **why)
{
    static t_u32 q[128], b[128];
    static t_u8 old[512];
    memset(p, 0, sizeof *p);
    p->opt = (struct t_opt){ TO_REMOVE, d->zr >= 0 ? d->part[d->zr].region : -1, d->zr, 0, 0 };
    *why = 0;
    if (!zset_ok(d, why)) return -1;
    int ri = d->part[d->zr].region;
    const struct t_region *g = &d->region[ri];
    if (region_parse(d, io, ri)) { *why = "cannot read the table"; return -1; }
    /* the Zenix's partitions must be one run of the chain */
    int first = -1, last = -1, n = d->zh >= 0 ? 3 : 2;
    for (int i = 0; i < rw.nparts; i++) {
        t_u32 blk = g->base + rw.part[i].blk;
        int z = blk == d->part[d->zb].blk || blk == d->part[d->zr].blk || (d->zh >= 0 && blk == d->part[d->zh].blk);
        if (z) { if (first < 0) first = i; last = i; }
    }
    if (first < 0 || last - first + 1 != n) { *why = "Zenix's partitions are not next to each other in the table: remove them with fdisk del or HDToolBox"; return -1; }
    p->atomic = 1;
    /* an RDB inside a 0x76 that holds nothing else: the MBR's entry goes, the space is free on the disk */
    if (g->base && rw.nparts == n) {
        int slot = -1;
        for (int i = 0; i < d->nparts; i++) if (d->part[i].how == TP_MBR && d->part[i].holds == ri) slot = d->part[i].slot;
        if (slot < 0 || d->noedit) { *why = "cannot find the PC table's entry for it"; return -1; }
        static t_u8 m[512];
        memcpy(m, d->sec0, 512);
        memset(m + 446 + 16 * slot, 0, 16);
        if (plan_write(p, io, 0, 0, m, 1)) { *why = "cannot read the disk"; return -1; }
        return 0;
    }
    t_u8 pb[512];
    if (io->rd(io->ctx, g->base + rw.part[last].blk, 1, pb)) { *why = "cannot read the table"; return -1; }
    t_u32 after = be32(pb + 16);
    if (first == 0) {
        if (io->rd(io->ctx, g->base + rw.blk, 1, old)) { *why = "cannot read the table"; return -1; }
        to_longs(b, old);
        b[7] = after;
        rdb_sum(b);
        if (add_write(p, g->base + rw.blk, b, 0, 1, old)) { *why = "too many writes"; return -1; }
    } else {
        if (io->rd(io->ctx, g->base + rw.part[first - 1].blk, 1, old)) { *why = "cannot read the table"; return -1; }
        to_longs(q, old);
        if (q[0] != RDB_ID_PART || !rdb_sum_ok(q)) { *why = "the table's block before Zenix's does not add up"; return -1; }
        q[4] = after;
        rdb_sum(q);
        if (add_write(p, g->base + rw.part[first - 1].blk, q, 0, 1, old)) { *why = "too many writes"; return -1; }
    }
    return 0;
}

