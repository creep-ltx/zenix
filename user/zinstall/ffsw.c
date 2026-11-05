/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include <string.h>
#else
#include "../libz.h"
#endif
#include "../sha256.h"
#include "ffsw.h"

#define LONGS 128
#define HT 72                           /* LONGS - 56: hash table entries, data pointers per block */
#define T_HEADER 2u
#define T_LIST 16u
#define ST_ROOT 1u
#define ST_DIR 2u
#define ST_FILE 0xFFFFFFFDu
#define BMPER (32u * 127u)              /* blocks one bitmap block covers */

static const struct ffsw_io *io;
static unsigned nblocks, root, nextb, nbm, nbx;     /* nbx: bitmap extension blocks, after the bitmap blocks */
static unsigned day, minute;
static const char *err;
static unsigned char *bm;               /* the bitmap blocks (nbm * 512) as bytes, set bit = free */
static unsigned bmcap;
struct ent {
    unsigned blk, dir;                  /* dir: its directory's header block */
    unsigned char hash, used;
    unsigned L[LONGS];
};
static struct ent ents[FFSW_MAXFILES];
static unsigned rootL[LONGS];
static unsigned char out[512];
static int writing;

static void put(unsigned char *b, const unsigned *L) { for (int i = 0; i < LONGS; i++) { b[i * 4] = (unsigned char)(L[i] >> 24); b[i * 4 + 1] = (unsigned char)(L[i] >> 16); b[i * 4 + 2] = (unsigned char)(L[i] >> 8); b[i * 4 + 3] = (unsigned char)L[i]; } }
static int wlongs(unsigned blk, const unsigned *L) { put(out, L); return io->wr(io->ctx, blk, 1, out); }
static void fail(const char *why) { if (!err) err = why; }

static void bit_clear(unsigned blk)     /* in use */
{
    unsigned rel = blk - 2;
    unsigned k = rel / BMPER, j = rel % BMPER;
    unsigned at = k * 512 + 4 + (j / 32) * 4;
    /* big-endian longs; bit j % 32 of the long, bit 0 the lowest */
    bm[at + 3 - (j % 32) / 8] &= (unsigned char)~(1u << ((j % 32) % 8));
}
static unsigned alloc(void)
{
    if (nextb >= nblocks) { fail("the boot partition is full"); return 0; }
    bit_clear(nextb);
    return nextb++;
}
static unsigned nhash(const char *name)
{
    unsigned l = (unsigned)strlen(name), h = l;
    for (unsigned i = 0; i < l; i++) {
        unsigned c = (unsigned char)name[i];
        if (c >= 97 && c <= 122) c -= 32;
        h = (h * 13 + c) & 0x7FF;
    }
    return h % HT;
}
static void set_name(unsigned *L, const char *name)
{
    unsigned l = (unsigned)strlen(name);
    unsigned char b[32];
    memset(b, 0, sizeof b);
    b[0] = (unsigned char)l;
    memcpy(b + 1, name, l);
    for (int i = 0; i < 8; i++)
        L[LONGS - 20 + i] = (unsigned)b[i * 4] << 24 | (unsigned)b[i * 4 + 1] << 16 | (unsigned)b[i * 4 + 2] << 8 | b[i * 4 + 3];
}
static void set_date(unsigned *L, int at) { L[at] = day; L[at + 1] = minute; L[at + 2] = 0; }

int ffsw_begin(const struct ffsw_io *in, unsigned blocks, const char *name, unsigned days, unsigned mins)
{
    static unsigned char *buf;
    io = in; err = 0; nblocks = blocks; day = days; minute = mins; writing = 1;
    memset(ents, 0, sizeof ents);
    if (blocks < 64 || blocks > FFSW_MAXBLOCKS) { fail("the boot partition's size is outside what this writer makes"); return -1; }
    nbm = (blocks - 2 + BMPER - 1) / BMPER;
    root = (2 + blocks - 1) / 2;
    if (nbm * 512 > bmcap) {
#ifdef ZHOST
        extern void *malloc(unsigned long);
#endif
        buf = malloc(nbm * 512);
        if (!buf) { fail("not enough memory for the boot partition's bitmap"); return -1; }
        bmcap = nbm * 512;
    }
    bm = buf;
    memset(bm, 0, nbm * 512);
    for (unsigned b = 2; b < blocks; b++) {      /* free = 1; past the end stays 0 */
        unsigned rel = b - 2, k = rel / BMPER, j = rel % BMPER, at = k * 512 + 4 + (j / 32) * 4;
        bm[at + 3 - (j % 32) / 8] |= (unsigned char)(1u << ((j % 32) % 8));
    }
    nbx = nbm > 25 ? (nbm - 25 + 126) / 127 : 0;      /* the root names 25 bitmap blocks, an extension block 127 more */
    bit_clear(root);
    for (unsigned i = 0; i < nbm + nbx; i++) bit_clear(root + 1 + i);
    nextb = root + 1 + nbm + nbx;
    memset(rootL, 0, sizeof rootL);
    rootL[0] = T_HEADER;
    rootL[3] = HT;
    rootL[LONGS - 50] = 0xFFFFFFFFu;
    for (unsigned i = 0; i < nbm && i < 25; i++) rootL[LONGS - 49 + i] = root + 1 + i;
    if (nbx) rootL[LONGS - 24] = root + 1 + nbm;
    set_date(rootL, LONGS - 23);
    set_name(rootL, name);
    set_date(rootL, LONGS - 10);
    set_date(rootL, LONGS - 7);
    rootL[LONGS - 1] = ST_ROOT;
    /* block 0 first: an empty volume's mark. The volume is whole only after ffsw_end's root write */
    memset(out, 0, 512);
    memcpy(out, "DOS\1", 4);
    if (io->wr(io->ctx, 0, 1, out)) { fail("cannot write the boot partition"); return -1; }
    memset(out, 0, 512);
    if (io->wr(io->ctx, 1, 1, out) || io->wr(io->ctx, root, 1, out)) { fail("cannot write the boot partition"); return -1; }
    return 0;
}

static struct ent *new_ent(const char *name, unsigned dirblk)
{
    if (!name[0] || strlen(name) > 30) { fail("a bad file name"); return 0; }
    for (int i = 0; i < FFSW_MAXFILES; i++) {
        struct ent *e = &ents[i];
        if (e->used) continue;
        e->used = 1;
        e->blk = alloc();
        if (err) return 0;
        e->dir = dirblk;
        e->hash = (unsigned char)nhash(name);
        memset(e->L, 0, sizeof e->L);
        e->L[0] = T_HEADER;
        e->L[1] = e->blk;
        set_name(e->L, name);
        set_date(e->L, LONGS - 23);
        e->L[LONGS - 3] = dirblk;
        return e;
    }
    fail("too many files for this writer");
    return 0;
}
static unsigned dir_block(const char *dir)
{
    if (!dir || !dir[0]) return root;
    for (int i = 0; i < FFSW_MAXFILES; i++) {
        const struct ent *e = &ents[i];
        if (!e->used || e->L[LONGS - 1] != ST_DIR) continue;
        unsigned l = e->L[LONGS - 20] >> 24;
        char n[32];
        for (unsigned k = 0; k < l && k < 31; k++) n[k] = (char)(e->L[LONGS - 20 + (k + 1) / 4] >> (24 - 8 * ((k + 1) % 4)));
        n[l < 31 ? l : 31] = 0;
        if (!strcmp(n, dir)) return e->blk;
    }
    fail("no such directory on the boot partition");
    return 0;
}

int ffsw_mkdir(const char *name)
{
    struct ent *e = new_ent(name, root);
    if (!e) return -1;
    e->L[LONGS - 1] = ST_DIR;
    return 0;
}

int ffsw_file(const char *dir, const char *name, unsigned size, ffsw_src src, void *ctx, unsigned char sha32[32])
{
    static unsigned ext[LONGS];
    static unsigned char data[512 * 22];
    unsigned parent = dir_block(dir);
    if (err) return -1;
    struct ent *e = new_ent(name, parent);
    if (!e) return -1;
    e->L[LONGS - 1] = ST_FILE;
    e->L[LONGS - 47] = size;
    unsigned total = (size + 511) / 512;
    struct sha256 c;
    sha256_init(&c);
    /* the header's table, then extension blocks, each for 72 data blocks, the data written as we go */
    unsigned *tab = e->L;
    unsigned tabblk = 0;                /* 0: the header itself, else the extension block's number */
    unsigned intab = 0, done = 0, left = size;
    memset(ext, 0, sizeof ext);
    unsigned first = 0;
    while (done < total) {
        unsigned room = HT - intab;
        unsigned k = total - done < room ? total - done : room;
        if (k > 22) k = 22;
        /* k consecutive blocks */
        unsigned lo = nextb;
        for (unsigned i = 0; i < k; i++) {
            unsigned b = alloc();
            if (err) return -1;
            if (!first) first = b;
            tab[6 + HT - 1 - intab] = b;
            intab++;
        }
        unsigned bytes = k * 512 < left ? k * 512 : left;
        memset(data, 0, k * 512);
        if (src(ctx, data, bytes)) { fail("cannot read a file for the boot partition"); return -1; }
        sha256_update(&c, data, bytes);
        if (io->wr(io->ctx, lo, k, data)) { fail("cannot write the boot partition"); return -1; }
        left -= bytes;
        done += k;
        tab[2] = intab;
        if (intab == HT && done < total) {   /* the next extension block */
            unsigned nx = alloc();
            if (err) return -1;
            if (tabblk) {
                ext[LONGS - 2] = nx;
                ext[5] = 0;
                unsigned s = 0;
                for (int i = 0; i < LONGS; i++) s += ext[i];
                ext[5] = 0u - s;
                if (wlongs(tabblk, ext)) { fail("cannot write the boot partition"); return -1; }
            } else e->L[LONGS - 2] = nx;
            memset(ext, 0, sizeof ext);
            ext[0] = T_LIST; ext[1] = nx; ext[LONGS - 3] = e->blk; ext[LONGS - 1] = ST_FILE;
            tab = ext; tabblk = nx; intab = 0;
        }
    }
    if (tabblk) {
        ext[5] = 0;
        unsigned s = 0;
        for (int i = 0; i < LONGS; i++) s += ext[i];
        ext[5] = 0u - s;
        if (wlongs(tabblk, ext)) { fail("cannot write the boot partition"); return -1; }
    }
    e->L[4] = first;
    sha256_final(&c, sha32);
    return 0;
}

int ffsw_end(void)
{
    if (err || !writing) return -1;
    writing = 0;
    /* hash chains in ascending block order: the tables and chain links */
    for (int i = 0; i < FFSW_MAXFILES; i++) {
        struct ent *e = &ents[i];
        if (!e->used) continue;
        unsigned *table = e->dir == root ? rootL : 0;
        if (!table) for (int k = 0; k < FFSW_MAXFILES; k++) if (ents[k].used && ents[k].blk == e->dir) table = ents[k].L;
        if (!table) { fail("internal: a file in no directory"); return -1; }
        unsigned *slot = &table[6 + e->hash];
        /* ents is in ascending block order: go to the chain's end */
        struct ent *p = 0;
        for (unsigned at = *slot; at;) {
            p = 0;
            for (int k = 0; k < FFSW_MAXFILES; k++) if (ents[k].used && ents[k].blk == at) p = &ents[k];
            if (!p) { fail("internal: a broken chain"); return -1; }
            at = p->L[LONGS - 4];
            if (!at) break;
        }
        if (p) p->L[LONGS - 4] = e->blk; else *slot = e->blk;
    }
    for (int i = 0; i < FFSW_MAXFILES; i++) {
        struct ent *e = &ents[i];
        if (!e->used) continue;
        e->L[5] = 0;
        unsigned s = 0;
        for (int k = 0; k < LONGS; k++) s += e->L[k];
        e->L[5] = 0u - s;
        if (wlongs(e->blk, e->L)) { fail("cannot write the boot partition"); return -1; }
    }
    for (unsigned i = 0; i < nbm; i++) {
        unsigned char *b = bm + i * 512;
        unsigned s = 0;
        for (int k = 1; k < LONGS; k++) s += (unsigned)b[k * 4] << 24 | (unsigned)b[k * 4 + 1] << 16 | (unsigned)b[k * 4 + 2] << 8 | b[k * 4 + 3];
        s = 0u - s;
        b[0] = (unsigned char)(s >> 24); b[1] = (unsigned char)(s >> 16); b[2] = (unsigned char)(s >> 8); b[3] = (unsigned char)s;
        if (io->wr(io->ctx, root + 1 + i, 1, b)) { fail("cannot write the boot partition"); return -1; }
    }
    for (unsigned x = 0; x < nbx; x++) {                 /* the extension blocks: 127 bitmap blocks' numbers, then the next one's */
        static unsigned XL[LONGS];
        memset(XL, 0, sizeof XL);
        for (unsigned j = 0; j < 127 && 25 + x * 127 + j < nbm; j++) XL[j] = root + 1 + 25 + x * 127 + j;
        XL[LONGS - 1] = x + 1 < nbx ? root + 1 + nbm + x + 1 : 0;
        if (wlongs(root + 1 + nbm + x, XL)) { fail("cannot write the boot partition"); return -1; }
    }
    rootL[5] = 0;
    unsigned s = 0;
    for (int k = 0; k < LONGS; k++) s += rootL[k];
    rootL[5] = 0u - s;
    if (wlongs(root, rootL)) { fail("cannot write the boot partition"); return -1; }
    return 0;
}

/* ---- reading a file back as any reader would find it: the root's table, the chain, the header, the data ---- */
static unsigned rdbuf[LONGS];
static int rd_longs(unsigned blk)
{
    static unsigned char b[512];
    if (blk >= nblocks || io->rd(io->ctx, blk, 1, b)) return -1;
    for (int i = 0; i < LONGS; i++) rdbuf[i] = (unsigned)b[i * 4] << 24 | (unsigned)b[i * 4 + 1] << 16 | (unsigned)b[i * 4 + 2] << 8 | b[i * 4 + 3];
    return 0;
}
static int same_name(const unsigned *L, const char *name)
{
    unsigned l = L[LONGS - 20] >> 24;
    if (l != strlen(name)) return 0;
    for (unsigned k = 0; k < l; k++)
        if ((char)(L[LONGS - 20 + (k + 1) / 4] >> (24 - 8 * ((k + 1) % 4))) != name[k]) return 0;
    return 1;
}
static unsigned lookup(unsigned dir, const char *name)
{
    if (rd_longs(dir)) return 0;
    unsigned at = rdbuf[6 + nhash(name)], guard = 0;
    while (at && guard++ < 64) {
        if (rd_longs(at)) return 0;
        if (same_name(rdbuf, name)) return at;
        at = rdbuf[LONGS - 4];
    }
    return 0;
}
int ffsw_read_sha(const char *dir, const char *name, unsigned size, unsigned char sha32[32])
{
    static unsigned char data[512];
    static unsigned tab[HT];
    unsigned d = root;
    if (dir && dir[0] && !(d = lookup(root, dir))) return -1;
    unsigned hdr = lookup(d, name);
    if (!hdr || rd_longs(hdr) || rdbuf[LONGS - 1] != ST_FILE || rdbuf[LONGS - 47] != size) return -1;
    struct sha256 c;
    sha256_init(&c);
    unsigned left = size, guard = 0;
    for (;;) {
        unsigned cnt = rdbuf[2], next = rdbuf[LONGS - 2];
        if (cnt > HT) return -1;
        for (unsigned i = 0; i < cnt; i++) tab[i] = rdbuf[6 + HT - 1 - i];
        for (unsigned i = 0; i < cnt && left; i++) {
            if (tab[i] >= nblocks || io->rd(io->ctx, tab[i], 1, data)) return -1;
            unsigned k = left < 512 ? left : 512;
            sha256_update(&c, data, k);
            left -= k;
        }
        if (!next) break;
        if (guard++ > nblocks || rd_longs(next)) return -1;
    }
    if (left) return -1;
    sha256_final(&c, sha32);
    return 0;
}

const char *ffsw_error(void) { return err ? err : "unknown error"; }
