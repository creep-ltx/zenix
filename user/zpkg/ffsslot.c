/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The update slot (DESIGN D22 4.5): a file AmigaOS made once on BOOT: (Zenix/zenix-update.slot), whose data blocks
 * Zenix overwrites in place, never anything else there. FFS (DOS\1, DOS\3) keeps no header or checksum in its data
 * blocks, so writing them changes no bitmap, header, hash chain, date or directory cache. This is the strictest code
 * in zpkg: the walk is read-only and done every time (no block list is kept between runs), and every check of 4.5
 * must pass before one byte is written:
 *   - geometry 512-byte blocks and 2 reserved blocks only; DOS\1 and DOS\3 only; a device that can flush;
 *   - the root block (type, checksum, hash table size, "bitmap valid", no bitmap extension block), the bitmap blocks'
 *     checksums;
 *   - the directory and the file by their hash chains (the international upper case for DOS\3), each block's type,
 *     own number, parent and checksum; the directory exactly ST_USERDIR, the file exactly ST_FILE with no hard link
 *     to it; extension blocks T_LIST/ST_FILE with their parent and checksum;
 *   - every data-block pointer inside the partition, none twice, none a block read as metadata, none a bitmap block,
 *     every one marked in use in the bitmap; the block count matching the byte size.
 * The block format is the AmigaDOS one as the public ADF FAQ describes it (and tools/mkffs.py writes it).
 */
#include <string.h>
#include <stdlib.h>
#include "zpkg.h"

#define BS 512u
#define LONGS 128u
#define HT 72u
#define T_HEADER 2u
#define T_LIST 16u
#define ST_ROOT 1u
#define ST_USERDIR 2u
#define ST_FILE 0xFFFFFFFDu         /* -3 */
#define L_HIGHSEQ 2u
#define L_FIRST 4u
#define L_CHECKSUM 5u
#define L_HASHSIZE 3u
#define L_BMFLAG 78u
#define L_BMPAGES 79u
#define L_BMEXT 104u
#define L_BYTESIZE 81u
#define L_NEXTLINK 118u             /* byte 472: the first hard link to this file */
#define L_CHAIN 124u
#define L_PARENT 125u
#define L_EXT 126u
#define L_SECTYPE 127u
#define BMPAGES 25u
#define NAMEMAX 30u

static zp_u32 L(const unsigned char *b, zp_u32 i)
{
    b += 4 * i;
    return (zp_u32)b[0] << 24 | (zp_u32)b[1] << 16 | (zp_u32)b[2] << 8 | b[3];
}

static int sum_ok(const unsigned char *b)
{
    zp_u32 s = 0, i;
    for (i = 0; i < LONGS; i++)
        s += L(b, i);
    return s == 0;
}

static unsigned up(unsigned c, int intl)
{
    if ((c >= 'a' && c <= 'z') || (intl && c >= 224 && c <= 254 && c != 247))
        return c - 32;
    return c;
}

static zp_u32 name_hash(const char *name, int intl)
{
    zp_u32 h = (zp_u32)strlen(name);
    for (; *name; name++)
        h = (h * 13 + up((unsigned char)*name, intl)) & 0x7FF;
    return h % HT;
}

struct walk {
    const struct zp_ffs *f;
    const char **why;
    unsigned char *buf;             /* one block */
    zp_u32 *meta;                   /* every block read as metadata */
    zp_u32 nmeta, capmeta;
    zp_u32 root;
    int intl;
};

static int refuse(struct walk *w, const char *why)
{
    *w->why = why;
    return ZP_EINVAL;
}

static int meta_add(struct walk *w, zp_u32 b)
{
    if (w->nmeta == w->capmeta) {
        zp_u32 cap = w->capmeta ? w->capmeta * 2 : 64;
        zp_u32 *n = realloc(w->meta, sizeof *n * cap);
        if (!n)
            return refuse(w, "out of memory"), ZP_ENOMEM;
        w->meta = n;
        w->capmeta = cap;
    }
    w->meta[w->nmeta++] = b;
    return 0;
}

static int rd(struct walk *w, zp_u32 b, unsigned char *buf)
{
    if (b < w->f->reserved || b >= w->f->nblocks)
        return refuse(w, "a block pointer outside the partition");
    if (w->f->read(w->f->u, b, buf) < 0)
        return refuse(w, "a read error"), ZP_EIO;
    return 0;
}

/* a header block in a chain: read it, check type, own number, checksum, parent; record it as metadata */
static int header(struct walk *w, zp_u32 b, zp_u32 type, zp_u32 parent)
{
    int rc = rd(w, b, w->buf);
    if (rc)
        return rc;
    if (L(w->buf, 0) != type || L(w->buf, 1) != b || !sum_ok(w->buf) || L(w->buf, L_PARENT) != parent)
        return refuse(w, "a damaged header block (type, number, checksum or parent)");
    return meta_add(w, b);
}

/* find name in directory block dir (its hash table in w->buf on entry is not assumed: read again) */
static int lookup(struct walk *w, zp_u32 dir, const char *name, zp_u32 sectype, zp_u32 *out)
{
    zp_u32 b, steps = 0, h = name_hash(name, w->intl), nlen = (zp_u32)strlen(name);
    int rc = rd(w, dir, w->buf);
    if (rc)
        return rc;
    b = L(w->buf, 6 + h);
    while (b) {
        zp_u32 k, n;
        if (++steps > w->f->nblocks)
            return refuse(w, "a hash chain that goes round");
        if ((rc = header(w, b, T_HEADER, dir)) != 0)
            return rc;
        n = w->buf[432];
        if (n > NAMEMAX)
            return refuse(w, "a name longer than 30 characters");
        if (n == nlen) {
            for (k = 0; k < n; k++)
                if (up(w->buf[433 + k], w->intl) != up((unsigned char)name[k], w->intl))
                    break;
            if (k == n) {
                if (L(w->buf, L_SECTYPE) != sectype)
                    return refuse(w, sectype == ST_FILE ? "the slot is not a plain file (a link?)"
                                                        : "the slot's directory is not a plain directory (a link?)");
                *out = b;
                return 0;
            }
        }
        b = L(w->buf, L_CHAIN);
    }
    return refuse(w, sectype == ST_FILE ? "there is no slot file" : "there is no slot directory"), ZP_ENOENT;
}

static int cmp_u32(const void *a, const void *b)
{
    zp_u32 x = *(const zp_u32 *)a, y = *(const zp_u32 *)b;
    return x < y ? -1 : x > y;
}

static int in_sorted(const zp_u32 *v, zp_u32 n, zp_u32 x)
{
    zp_u32 lo = 0, hi = n;
    while (lo < hi) {
        zp_u32 m = lo + (hi - lo) / 2;
        if (v[m] == x)
            return 1;
        if (v[m] < x)
            lo = m + 1;
        else
            hi = m;
    }
    return 0;
}

void zp_slot_free(struct zp_slot *s)
{
    free(s->blk);
    memset(s, 0, sizeof *s);
}

int zp_slot_find(const struct zp_ffs *f, const char *dir, const char *name, struct zp_slot *s, const char **why)
{
    static const char *dummy;
    struct walk w;
    unsigned char *bm = 0;
    zp_u32 nbm, i, d, fh, size, nblk, got = 0, ext, hs, steps = 0, *sorted = 0;
    int rc;

    memset(s, 0, sizeof *s);
    memset(&w, 0, sizeof w);
    w.f = f;
    w.why = why ? why : &dummy;
    if (f->blocksize != BS || f->reserved != 2)
        return refuse(&w, "only 512-byte blocks and 2 reserved blocks are supported");
    if (f->dostype != ZP_DOS1 && f->dostype != ZP_DOS3)
        return refuse(&w, "only DOS\\1 and DOS\\3 (FFS) are supported");
    if (!f->canflush)
        return refuse(&w, "the device cannot flush its cache");
    if (f->nblocks < 8 || f->nblocks > 0x7FFFFFFFu)
        return refuse(&w, "a bad partition size");
    if (!dir[0] || !name[0] || strlen(dir) > NAMEMAX || strlen(name) > NAMEMAX)
        return refuse(&w, "a bad slot name");
    w.intl = f->dostype == ZP_DOS3;
    w.buf = malloc(BS);
    if (!w.buf)
        return refuse(&w, "out of memory"), ZP_ENOMEM;
    if (f->read(f->u, 0, w.buf) < 0) {
        rc = refuse(&w, "a read error");
        rc = ZP_EIO;
        goto out;
    }
    if (L(w.buf, 0) != f->dostype) {
        rc = refuse(&w, "the boot block's dostype is not the partition's");
        goto out;
    }
    /* the root */
    w.root = (f->reserved + f->nblocks - 1) / 2;
    if ((rc = rd(&w, w.root, w.buf)) != 0)
        goto out;
    if (L(w.buf, 0) != T_HEADER || L(w.buf, L_SECTYPE) != ST_ROOT || L(w.buf, L_HASHSIZE) != HT || !sum_ok(w.buf)) {
        rc = refuse(&w, "a damaged root block");
        goto out;
    }
    if (L(w.buf, L_BMFLAG) != 0xFFFFFFFFu) {
        rc = refuse(&w, "the volume's bitmap is not valid (let AmigaOS validate it)");
        goto out;
    }
    if (L(w.buf, L_BMEXT) != 0) {
        rc = refuse(&w, "a bitmap extension block (a volume over about 50 MB) is not supported");
        goto out;
    }
    nbm = (f->nblocks - f->reserved + 32 * (LONGS - 1) - 1) / (32 * (LONGS - 1));
    if (nbm > BMPAGES) {
        rc = refuse(&w, "a bitmap extension block (a volume over about 50 MB) is not supported");
        goto out;
    }
    if ((rc = meta_add(&w, w.root)) != 0)
        goto out;
    bm = malloc(BS * nbm);
    if (!bm) {
        rc = refuse(&w, "out of memory");
        rc = ZP_ENOMEM;
        goto out;
    }
    {
        zp_u32 pages[BMPAGES];
        for (i = 0; i < BMPAGES; i++) {
            pages[i] = L(w.buf, L_BMPAGES + i);
            if ((i < nbm) != (pages[i] != 0)) {
                rc = refuse(&w, "the root's bitmap pointers do not fit the partition");
                goto out;
            }
        }
        for (i = 0; i < nbm; i++) {
            if ((rc = rd(&w, pages[i], bm + BS * i)) != 0)
                goto out;
            if (!sum_ok(bm + BS * i) || pages[i] == w.root) {
                rc = refuse(&w, "a damaged bitmap block");
                goto out;
            }
            if ((rc = meta_add(&w, pages[i])) != 0)
                goto out;
        }
    }
    /* the directory, the file */
    if ((rc = lookup(&w, w.root, dir, ST_USERDIR, &d)) != 0)
        goto out;
    if ((rc = lookup(&w, d, name, ST_FILE, &fh)) != 0)
        goto out;
    /* w.buf holds the file header (lookup's last read) */
    if (L(w.buf, L_NEXTLINK) != 0) {
        rc = refuse(&w, "the slot has a hard link");
        goto out;
    }
    size = L(w.buf, L_BYTESIZE);
    if (size < 2 * BS || size % BS || size > ZP_BUNDLE_MAX + BS) {
        rc = refuse(&w, "the slot's size is not whole blocks between 1 KB and 16 MB");
        goto out;
    }
    nblk = size / BS;
    s->blk = malloc(sizeof *s->blk * nblk);
    sorted = malloc(sizeof *sorted * nblk);
    if (!s->blk || !sorted) {
        rc = refuse(&w, "out of memory");
        rc = ZP_ENOMEM;
        goto out;
    }
    ext = fh;
    for (;;) {
        hs = L(w.buf, L_HIGHSEQ);
        if (hs > HT || got + hs > nblk) {
            rc = refuse(&w, "the slot's block count does not match its size");
            goto out;
        }
        for (i = 0; i < hs; i++)
            s->blk[got++] = L(w.buf, 77 - i);
        if (ext == fh && hs && L(w.buf, L_FIRST) != s->blk[0]) {
            rc = refuse(&w, "the slot's first data block is not its first pointer");
            goto out;
        }
        ext = L(w.buf, L_EXT);
        if (!ext)
            break;
        if (hs != HT || ++steps > nblk) {
            rc = refuse(&w, "a damaged extension chain");
            goto out;
        }
        if ((rc = header(&w, ext, T_LIST, fh)) != 0)
            goto out;
        if (L(w.buf, L_SECTYPE) != ST_FILE) {
            rc = refuse(&w, "a damaged extension block");
            goto out;
        }
    }
    if (got != nblk) {
        rc = refuse(&w, "the slot's block count does not match its size");
        goto out;
    }
    /* the data pointers */
    qsort(w.meta, w.nmeta, sizeof *w.meta, cmp_u32);
    memcpy(sorted, s->blk, sizeof *sorted * nblk);
    qsort(sorted, nblk, sizeof *sorted, cmp_u32);
    for (i = 0; i < nblk; i++) {
        zp_u32 b = sorted[i], k;
        if (b < f->reserved || b >= f->nblocks) {
            rc = refuse(&w, "a data pointer outside the partition");
            goto out;
        }
        if (i && sorted[i - 1] == b) {
            rc = refuse(&w, "a data block named twice");
            goto out;
        }
        if (in_sorted(w.meta, w.nmeta, b)) {
            rc = refuse(&w, "a data pointer to a metadata or bitmap block");
            goto out;
        }
        k = b - f->reserved;
        {
            const unsigned char *page = bm + BS * (k / (32 * (LONGS - 1)));
            zp_u32 j = k % (32 * (LONGS - 1));
            if (L(page, 1 + j / 32) & (1u << (j % 32))) {
                rc = refuse(&w, "a data block of the slot is marked free in the bitmap");
                goto out;
            }
        }
    }
    s->size = size;
    s->nblk = nblk;
    s->header = fh;
    rc = 0;
out:
    free(sorted);
    free(bm);
    free(w.meta);
    free(w.buf);
    if (rc)
        zp_slot_free(s);
    return rc;
}

int zp_slot_pwrite(const struct zp_ffs *f, const struct zp_slot *s, zp_u32 off, const unsigned char *data, zp_u32 n)
{
    unsigned char *buf;
    if (!f->write || off > s->size || n > s->size - off)
        return ZP_EINVAL;
    buf = malloc(BS);
    if (!buf)
        return ZP_ENOMEM;
    while (n) {
        zp_u32 bi = off / BS, bo = off % BS, k = BS - bo, b;
        if (k > n)
            k = n;
        b = s->blk[bi];
        if (b < f->reserved || b >= f->nblocks) {
            free(buf);
            return ZP_EINVAL;
        }
        if (k < BS && f->read(f->u, b, buf) < 0) {
            free(buf);
            return ZP_EIO;
        }
        memcpy(buf + bo, data, k);
        if (f->write(f->u, b, buf) < 0) {
            free(buf);
            return ZP_EIO;
        }
        off += k;
        data += k;
        n -= k;
    }
    free(buf);
    return 0;
}
