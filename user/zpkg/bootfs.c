/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The boot partition (DESIGN D22 4.5, as built 2026-10-31): zpkg puts a system update where every zenixboot looks
 * for one, BOOT:Zenix/zenix-update.zup and zenix-update.zup.sig, as new files of the AmigaDOS Fast File System (DOS\1,
 * DOS\3). This is the strictest code in zpkg, written to the owner's AmigaOS partition, so:
 *   - the whole volume is walked and checked first, every time (nothing is kept between runs): geometry 512-byte
 *     blocks and 2 reserved blocks only, DOS\1 and DOS\3 only, a device that can flush; the root (type, checksum,
 *     hash table size, "bitmap valid", at most two bitmap extension blocks), the bitmap blocks' checksums; every directory,
 *     file and link from the root by its hash chains (type, own number, parent, checksum), every file's extension
 *     blocks and data pointers (inside the partition, the count matching the byte size); no block used twice, and
 *     every block in use marked in use in the bitmap. Anything else is refused before a byte is written;
 *   - a new file is written in AmigaOS's own safe order (the public ADF FAQ's block formats; the order learned with
 *     CFile's FFS writer, knowledge base amiga/disk-images.md): its blocks into blocks free in the bitmap and named by
 *     nothing, flushed; the bitmap blocks, flushed; then the ONE block write that links the header into its
 *     directory's hash chain (at its place in block order, as FFS keeps chains), flushed. The root's "bitmap valid"
 *     flag is never cleared. A cut leaves the volume valid: the file is there whole or not at all, at worst with
 *     blocks marked in use that no file names (a leak: AmigaOS never validates a volume whose bitmap is valid, so the
 *     next create gives them back in its own bitmap writes, the walk having proved that nothing names them);
 *   - nothing else on the volume is ever changed: no other file, no directory but the hash-chain link, no date;
 *   - an existing file's data blocks may be overwritten in place (zp_boot_pwrite): no metadata changes;
 *   - for zinstall's kept start-up partition (DESIGN D35 3b): a directory made in the root (zp_boot_mkdir, the same
 *     order as a file), files in the root (zp_boot_cd ""), a file replaced (zp_boot_replace: the new data and extension
 *     blocks in free blocks, the bitmap, then the ONE write of its header in its own place; the old blocks given back
 *     after) and a file deleted (zp_boot_delete: the ONE write that unlinks it, its blocks given back after). A cut
 *     leaves the old file or the new one, at worst with a leak as above.
 * zenixboot (under Kickstart's own dos) then reads, checks and installs the files as D15 built it.
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
#define ST_SOFTLINK 3u
#define ST_LINKDIR 4u
#define ST_FILE 0xFFFFFFFDu         /* -3 */
#define ST_LINKFILE 0xFFFFFFFCu     /* -4 */
#define L_HIGHSEQ 2u
#define L_FIRST 4u
#define L_CHECKSUM 5u
#define L_HASHSIZE 3u
#define L_BMFLAG 78u
#define L_BMPAGES 79u
#define L_PROTECT 80u
#define L_BYTESIZE 81u
#define L_BMEXT 104u
#define L_DAYS 105u
#define L_NEXTLINK 118u             /* the first hard link to this file */
#define L_CHAIN 124u
#define L_PARENT 125u
#define L_EXT 126u
#define L_SECTYPE 127u
#define NAMEMAX 30u
#define MAXDEPTH 8                  /* directories below the root (BOOT: has two levels) */
#define MAXHEADERS 4096u            /* headers on the volume (BOOT: has a few dozen) */

static zp_u32 L(const unsigned char *b, zp_u32 i)
{
    b += 4 * i;
    return (zp_u32)b[0] << 24 | (zp_u32)b[1] << 16 | (zp_u32)b[2] << 8 | b[3];
}

static void S(unsigned char *b, zp_u32 i, zp_u32 v)
{
    b += 4 * i;
    b[0] = (unsigned char)(v >> 24);
    b[1] = (unsigned char)(v >> 16);
    b[2] = (unsigned char)(v >> 8);
    b[3] = (unsigned char)v;
}

static int sum_ok(const unsigned char *b)
{
    zp_u32 s = 0, i;
    for (i = 0; i < LONGS; i++)
        s += L(b, i);
    return s == 0;
}

static void sum_set(unsigned char *b, zp_u32 at)        /* the long at "at" makes the block's sum zero */
{
    zp_u32 s = 0, i;
    S(b, at, 0);
    for (i = 0; i < LONGS; i++)
        s += L(b, i);
    S(b, at, (zp_u32)0 - s);
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

static int name_is(const unsigned char *hdr, const char *name, int intl)
{
    zp_u32 n = hdr[432], k;
    if (n != strlen(name))
        return 0;
    for (k = 0; k < n; k++)
        if (up(hdr[433 + k], intl) != up((unsigned char)name[k], intl))
            return 0;
    return 1;
}

static int refuse(const char **why, const char *text, int rc)
{
    *why = text;
    return rc;
}

static int rd(const struct zp_bootvol *v, zp_u32 b, unsigned char *buf, const char **why)
{
    if (b < v->f->reserved || b >= v->f->nblocks)
        return refuse(why, "a block pointer outside the partition", ZP_EINVAL);
    if (v->f->read(v->f->u, b, buf) < 0)
        return refuse(why, "a read error on the boot partition", ZP_EIO);
    return 0;
}

static int wr(const struct zp_bootvol *v, zp_u32 b, unsigned char *buf, const char **why)
{
    if (b < v->f->reserved || b >= v->f->nblocks)        /* (never: every number written was checked) */
        return refuse(why, "a block outside the partition", ZP_EINVAL);
    if (v->f->write(v->f->u, b, buf) < 0)
        return refuse(why, "a write error on the boot partition", ZP_EIO);
    return 0;
}

static int flush(const struct zp_bootvol *v, const char **why)
{
    if (!v->f->flush || v->f->flush(v->f->u) < 0)
        return refuse(why, "the boot partition's device did not flush", ZP_EIO);
    return 0;
}

static int owned(const struct zp_bootvol *v, zp_u32 b) { return v->own[b >> 3] >> (b & 7) & 1; }

/* a block the walk found in use: inside the partition, never twice */
static int own(struct zp_bootvol *v, zp_u32 b, const char **why)
{
    if (b < v->f->reserved || b >= v->f->nblocks)
        return refuse(why, "a block pointer outside the partition", ZP_EINVAL);
    if (owned(v, b))
        return refuse(why, "a block used twice (a damaged volume)", ZP_EINVAL);
    v->own[b >> 3] |= (unsigned char)(1u << (b & 7));
    return 0;
}

/* the bitmap: bit set = free; block reserved + 32 * long + bit, long 0 the checksum (as tools/mkffs.py) */
static int bm_free(const struct zp_bootvol *v, zp_u32 b)
{
    zp_u32 k = b - v->f->reserved, per = 32 * (LONGS - 1);
    return L(v->bm + BS * (k / per), 1 + (k % per) / 32) >> (k % 32) & 1;
}

static void bm_take(struct zp_bootvol *v, zp_u32 b, unsigned char *dirty)
{
    zp_u32 k = b - v->f->reserved, per = 32 * (LONGS - 1), pg = k / per, li = 1 + (k % per) / 32;
    unsigned char *page = v->bm + BS * pg;
    S(page, li, L(page, li) & ~(1u << (k % 32)));
    dirty[pg] = 1;
}

static void bm_give(struct zp_bootvol *v, zp_u32 b, unsigned char *dirty)
{
    zp_u32 k = b - v->f->reserved, per = 32 * (LONGS - 1), pg = k / per, li = 1 + (k % per) / 32;
    unsigned char *page = v->bm + BS * pg;
    S(page, li, L(page, li) | 1u << (k % 32));
    dirty[pg] = 1;
}

/* a header block: its type, own number, checksum and parent */
static int header(struct zp_bootvol *v, zp_u32 b, zp_u32 type, zp_u32 parent, unsigned char *buf, const char **why)
{
    int rc = rd(v, b, buf, why);
    if (rc)
        return rc;
    if (L(buf, 0) != type || L(buf, 1) != b || !sum_ok(buf) || L(buf, L_PARENT) != parent)
        return refuse(why, "a damaged header block (type, number, checksum or parent)", ZP_EINVAL);
    if (type == T_HEADER && buf[432] > NAMEMAX)
        return refuse(why, "a name longer than 30 characters", ZP_EINVAL);
    return 0;
}

/* a file's data pointers from its header (in buf) and extension blocks, in file order into out (0: only count and
 * own them); the count must be the byte size's */
static int file_blocks(struct zp_bootvol *v, zp_u32 fh, unsigned char *buf, zp_u32 *out, int take, zp_u32 *nblk,
                       const char **why)
{
    zp_u32 size = L(buf, L_BYTESIZE), want = size / BS + (size % BS != 0), got = 0, ext, i, hs, steps = 0;
    int rc;
    if (size > 0x7FFFFFFFu || want > v->f->nblocks)
        return refuse(why, "a file larger than its partition", ZP_EINVAL);
    if (want && L(buf, L_FIRST) != L(buf, 77))
        return refuse(why, "a file whose first data block is not its first pointer", ZP_EINVAL);
    for (;;) {
        hs = L(buf, L_HIGHSEQ);
        if (hs > HT || got + hs > want)
            return refuse(why, "a file's block count does not match its size", ZP_EINVAL);
        for (i = 0; i < hs; i++) {
            zp_u32 b = L(buf, 77 - i);
            if (take && (rc = own(v, b, why)) != 0)
                return rc;
            if (!take && (b < v->f->reserved || b >= v->f->nblocks))
                return refuse(why, "a block pointer outside the partition", ZP_EINVAL);
            if (out)
                out[got] = b;
            got++;
        }
        ext = L(buf, L_EXT);
        if (!ext)
            break;
        if (hs != HT || ++steps > v->f->nblocks)
            return refuse(why, "a damaged extension chain", ZP_EINVAL);
        if (take && (rc = own(v, ext, why)) != 0)
            return rc;
        if ((rc = header(v, ext, T_LIST, fh, buf, why)) != 0)
            return rc;
        if (L(buf, L_SECTYPE) != ST_FILE)
            return refuse(why, "a damaged extension block", ZP_EINVAL);
    }
    if (got != want)
        return refuse(why, "a file's block count does not match its size", ZP_EINVAL);
    *nblk = got;
    return 0;
}

/* every entry of directory d (its header already owned and checked), and below it */
static int walk_dir(struct zp_bootvol *v, zp_u32 d, int depth, const char *want, unsigned char *buf, const char **why)
{
    zp_u32 *ht, h, b, n;
    int rc = 0;
    if (depth > MAXDEPTH)
        return refuse(why, "directories nested too deep", ZP_EINVAL);
    ht = malloc(sizeof *ht * HT);
    if (!ht)
        return refuse(why, "out of memory", ZP_ENOMEM);
    if ((rc = rd(v, d, buf, why)) != 0)
        goto out;
    for (h = 0; h < HT; h++)
        ht[h] = L(buf, 6 + h);
    for (h = 0; h < HT && !rc; h++) {
        for (b = ht[h]; b && !rc; b = L(buf, L_CHAIN)) {
            if (++v->nheaders > MAXHEADERS) {
                rc = refuse(why, "more than 4096 files and directories", ZP_EINVAL);
                break;
            }
            if ((rc = own(v, b, why)) != 0 || (rc = header(v, b, T_HEADER, d, buf, why)) != 0)
                break;
            {
                char nm[NAMEMAX + 1];
                zp_u32 k, nl = buf[432];
                for (k = 0; k < nl && buf[433 + k]; k++)
                    nm[k] = (char)buf[433 + k];
                nm[k] = 0;
                if (!nl || k != nl || name_hash(nm, v->intl) != h) {
                    rc = refuse(why, "an entry in another name's hash chain (a damaged directory)", ZP_EINVAL);
                    break;
                }
            }
            switch (L(buf, L_SECTYPE)) {
            case ST_USERDIR:
                if (depth == 0 && want && name_is(buf, want, v->intl)) {
                    if (v->dir) {
                        rc = refuse(why, "two directories of the same name", ZP_EINVAL);
                        break;
                    }
                    v->dir = b;
                }
                if ((rc = walk_dir(v, b, depth + 1, 0, buf, why)) != 0)
                    break;
                rc = header(v, b, T_HEADER, d, buf, why);      /* buf again: this entry, for its chain */
                break;
            case ST_FILE:
                if ((rc = file_blocks(v, b, buf, 0, 1, &n, why)) != 0)
                    break;
                rc = header(v, b, T_HEADER, d, buf, why);
                break;
            case ST_LINKFILE: case ST_LINKDIR:
                v->links = 1;                                   /* see the give-back below */
                break;
            case ST_SOFTLINK:
                break;                                          /* a header only (the target is walked as itself) */
            default:
                rc = refuse(why, "an entry of an unknown kind (secondary type)", ZP_EINVAL);
            }
        }
    }
out:
    free(ht);
    return rc;
}

void zp_boot_close(struct zp_bootvol *v)
{
    free(v->bm);
    free(v->own);
    memset(v, 0, sizeof *v);
}

int zp_boot_open(const struct zp_ffs *f, const char *dir, struct zp_bootvol *v, const char **why)
{
    static const char *dummy;
    unsigned char *buf;
    zp_u32 i, b;
    int rc;

    if (!why)
        why = &dummy;
    memset(v, 0, sizeof *v);
    v->f = f;
    if (f->blocksize != BS || f->reserved != 2)
        return refuse(why, "only 512-byte blocks and 2 reserved blocks are supported", ZP_EINVAL);
    if (f->dostype != ZP_DOS1 && f->dostype != ZP_DOS3)
        return refuse(why, "only DOS\\1 and DOS\\3 (FFS) are supported", ZP_EINVAL);
    if (!f->canflush)
        return refuse(why, "the device cannot flush its cache", ZP_EINVAL);
    if (f->nblocks < 8 || f->nblocks > 0x7FFFFFFFu)
        return refuse(why, "a bad partition size", ZP_EINVAL);
    if (dir && (!dir[0] || strlen(dir) > NAMEMAX))
        return refuse(why, "a bad directory name", ZP_EINVAL);
    v->intl = f->dostype == ZP_DOS3;
    buf = malloc(BS);
    v->own = calloc(f->nblocks / 8 + 1, 1);
    if (!buf || !v->own) {
        rc = refuse(why, "out of memory", ZP_ENOMEM);
        goto out;
    }
    if (f->read(f->u, 0, buf) < 0) {
        rc = refuse(why, "a read error on the boot partition", ZP_EIO);
        goto out;
    }
    if (L(buf, 0) != f->dostype) {
        rc = refuse(why, "the boot block's dostype is not the partition's", ZP_EINVAL);
        goto out;
    }
    /* the root */
    v->root = (f->reserved + f->nblocks - 1) / 2;
    if ((rc = rd(v, v->root, buf, why)) != 0)
        goto out;
    if (L(buf, 0) != T_HEADER || L(buf, L_SECTYPE) != ST_ROOT || L(buf, L_HASHSIZE) != HT || !sum_ok(buf) ||
        L(buf, L_PARENT) != 0) {
        rc = refuse(why, "a damaged root block", ZP_EINVAL);
        goto out;
    }
    if (L(buf, L_BMFLAG) != 0xFFFFFFFFu) {
        rc = refuse(why, "the volume's bitmap is not valid (AmigaOS validates it at its next start)", ZP_EINVAL);
        goto out;
    }
    v->nbm = (f->nblocks - f->reserved + 32 * (LONGS - 1) - 1) / (32 * (LONGS - 1));
    if (v->nbm > ZP_BOOT_BMPAGES) {
        rc = refuse(why, "a boot partition over about 550 MB (more than two bitmap extension blocks) is not supported",
                    ZP_EINVAL);
        goto out;
    }
    if ((rc = own(v, v->root, why)) != 0)
        goto out;
    for (i = 0; i < 25; i++) {
        b = L(buf, L_BMPAGES + i);
        if ((i < v->nbm) != (b != 0)) {
            rc = refuse(why, "the root's bitmap pointers do not fit the partition", ZP_EINVAL);
            goto out;
        }
        if (i < v->nbm) {
            v->bmblk[i] = b;
            if ((rc = own(v, b, why)) != 0)
                goto out;
        }
    }
    /* the rest in bitmap extension blocks (a volume over about 50 MB): 127 pointers each, then the next one's */
    b = L(buf, L_BMEXT);
    if ((v->nbm > 25) != (b != 0)) {
        rc = refuse(why, "the root's bitmap extension pointer does not fit the partition", ZP_EINVAL);
        goto out;
    }
    while (b) {
        zp_u32 j;
        if ((rc = own(v, b, why)) != 0 || (rc = rd(v, b, buf, why)) != 0)
            goto out;
        for (j = 0; j < LONGS - 1; j++) {
            zp_u32 p = L(buf, j);
            if ((i < v->nbm) != (p != 0)) {
                rc = refuse(why, "a bitmap extension block's pointers do not fit the partition", ZP_EINVAL);
                goto out;
            }
            if (i < v->nbm) {
                v->bmblk[i++] = p;
                if ((rc = own(v, p, why)) != 0)
                    goto out;
            }
        }
        b = L(buf, LONGS - 1);
        if ((i < v->nbm) != (b != 0)) {
            rc = refuse(why, "a bitmap extension block's next pointer does not fit the partition", ZP_EINVAL);
            goto out;
        }
    }
    v->bm = malloc(BS * v->nbm);
    if (!v->bm) {
        rc = refuse(why, "out of memory", ZP_ENOMEM);
        goto out;
    }
    for (i = 0; i < v->nbm; i++) {
        if ((rc = rd(v, v->bmblk[i], v->bm + BS * i, why)) != 0)
            goto out;
        if (!sum_ok(v->bm + BS * i)) {
            rc = refuse(why, "a damaged bitmap block", ZP_EINVAL);
            goto out;
        }
    }
    /* everything below the root, then the bitmap against it */
    if ((rc = walk_dir(v, v->root, 0, dir, buf, why)) != 0)
        goto out;
    for (b = f->reserved; b < f->nblocks; b++) {
        int fr = bm_free(v, b);
        if (owned(v, b) && fr) {
            rc = refuse(why, "a block in use is marked free in the bitmap (a damaged volume)", ZP_EINVAL);
            goto out;
        }
        if (fr)
            v->nfree++;
        else if (!owned(v, b))
            v->nleak++;
    }
    rc = 0;
out:
    free(buf);
    if (rc)
        zp_boot_close(v);
    return rc;
}

void zp_boot_file_free(struct zp_bootfile *bf)
{
    free(bf->blk);
    memset(bf, 0, sizeof *bf);
}

int zp_boot_find(struct zp_bootvol *v, const char *name, struct zp_bootfile *bf, const char **why)
{
    static const char *dummy;
    unsigned char *buf;
    zp_u32 b, n;
    int rc = ZP_ENOENT;
    if (!why)
        why = &dummy;
    memset(bf, 0, sizeof *bf);
    if (!v->dir)
        return refuse(why, "there is no Zenix directory", ZP_ENOENT);
    buf = malloc(BS);
    if (!buf)
        return refuse(why, "out of memory", ZP_ENOMEM);
    if ((rc = rd(v, v->dir, buf, why)) != 0)
        goto out;
    rc = ZP_ENOENT;
    *why = "no such file";
    for (b = L(buf, 6 + name_hash(name, v->intl)); b; b = L(buf, L_CHAIN)) {
        if ((rc = header(v, b, T_HEADER, v->dir, buf, why)) != 0)
            goto out;
        rc = ZP_ENOENT;
        if (!name_is(buf, name, v->intl))
            continue;
        if (L(buf, L_SECTYPE) != ST_FILE || L(buf, L_NEXTLINK) != 0) {
            rc = refuse(why, "not a plain file (a directory, a link, or a file with a hard link)", ZP_EINVAL);
            goto out;
        }
        bf->header = b;
        bf->size = L(buf, L_BYTESIZE);
        n = bf->size / BS + (bf->size % BS != 0);
        bf->blk = malloc(sizeof *bf->blk * (n ? n : 1));
        if (!bf->blk) {
            rc = refuse(why, "out of memory", ZP_ENOMEM);
            goto out;
        }
        rc = file_blocks(v, b, buf, bf->blk, 0, &bf->nblk, why);
        goto out;
    }
out:
    free(buf);
    if (rc)
        zp_boot_file_free(bf);
    return rc;
}

int zp_boot_read(const struct zp_bootvol *v, const struct zp_bootfile *bf, zp_u32 off, unsigned char *out, zp_u32 n)
{
    unsigned char *buf;
    const char *why;
    if (off > bf->size || n > bf->size - off)
        return ZP_EINVAL;
    buf = malloc(BS);
    if (!buf)
        return ZP_ENOMEM;
    while (n) {
        zp_u32 bo = off % BS, k = BS - bo;
        if (k > n)
            k = n;
        if (rd(v, bf->blk[off / BS], buf, &why)) {
            free(buf);
            return ZP_EIO;
        }
        memcpy(out, buf + bo, k);
        off += k;
        out += k;
        n -= k;
    }
    free(buf);
    return 0;
}

int zp_boot_pwrite(const struct zp_bootvol *v, const struct zp_bootfile *bf, zp_u32 off, const unsigned char *data,
                   zp_u32 n)
{
    unsigned char *buf;
    const char *why;
    if (off > bf->size || n > bf->size - off)
        return ZP_EINVAL;
    buf = malloc(BS);
    if (!buf)
        return ZP_ENOMEM;
    while (n) {
        zp_u32 bo = off % BS, k = BS - bo, b = bf->blk[off / BS];
        if (k > n)
            k = n;
        if (rd(v, b, buf, &why) || (memcpy(buf + bo, data, k), wr(v, b, buf, &why))) {
            free(buf);
            return ZP_EIO;
        }
        off += k;
        data += k;
        n -= k;
    }
    free(buf);
    return flush(v, &why) ? ZP_EIO : 0;
}

zp_u32 zp_boot_blocks(zp_u32 size)
{
    zp_u32 n = size / BS + (size % BS != 0);
    return 1 + n + (n > HT ? (n - HT + HT - 1) / HT : 0);
}

/* the blocks of a new file's body (data and extension blocks, nb in order, as zp_boot_blocks counts them without
 * the header): its data from src, the extension blocks naming hdrblk their file. ptr: the data blocks in file order;
 * *first_ext: the first extension block (0: none) */
static int body(struct zp_bootvol *v, const zp_u32 *nb, zp_u32 hdrblk, zp_u32 size, zp_src src, void *u,
                unsigned char *buf, zp_u32 *ptr, zp_u32 *first_ext, const char **why)
{
    zp_u32 ndata = size / BS + (size % BS != 0), groups = ndata ? (ndata + HT - 1) / HT : 1, j, k = 0, next_ext = 0;
    zp_u32 *ext = malloc(sizeof *ext * groups);
    int rc = 0;
    if (!ext)
        return refuse(why, "out of memory", ZP_ENOMEM);
    for (j = 0; j < ndata; j++) {
        if (j && j % HT == 0)
            ext[j / HT] = nb[k++];
        ptr[j] = nb[k++];
    }
    for (j = 0; j < ndata && !rc; j++) {
        zp_u32 n = size - j * BS < BS ? size - j * BS : BS;
        memset(buf, 0, BS);
        if (src(u, j * BS, buf, n) < 0)
            rc = refuse(why, "a file for the boot partition could not be read", ZP_EIO);
        else
            rc = wr(v, ptr[j], buf, why);
    }
    for (j = groups; j-- > 1 && !rc;) {                     /* the extension blocks, last first */
        zp_u32 n = ndata - j * HT < HT ? ndata - j * HT : HT, t;
        memset(buf, 0, BS);
        S(buf, 0, T_LIST);
        S(buf, 1, ext[j]);
        S(buf, L_HIGHSEQ, n);
        for (t = 0; t < n; t++)
            S(buf, 77 - t, ptr[j * HT + t]);
        S(buf, L_PARENT, hdrblk);
        S(buf, L_EXT, next_ext);
        S(buf, L_SECTYPE, ST_FILE);
        sum_set(buf, L_CHECKSUM);
        rc = wr(v, ext[j], buf, why);
        next_ext = ext[j];
    }
    free(ext);
    *first_ext = next_ext;
    return rc;
}

/* a file header's table, size and date (the rest of hdr as the caller set it) */
static void file_header(unsigned char *hdr, zp_u32 size, const zp_u32 *ptr, zp_u32 first_ext, const zp_u32 date[3])
{
    zp_u32 ndata = size / BS + (size % BS != 0), n = ndata < HT ? ndata : HT, j;
    for (j = 6; j < 6 + HT; j++)
        S(hdr, j, 0);
    S(hdr, L_HIGHSEQ, n);
    S(hdr, L_FIRST, ndata ? ptr[0] : 0);
    for (j = 0; j < n; j++)
        S(hdr, 77 - j, ptr[j]);
    S(hdr, L_BYTESIZE, size);
    S(hdr, L_DAYS, date[0]);
    S(hdr, L_DAYS + 1, date[1]);
    S(hdr, L_DAYS + 2, date[2]);
    S(hdr, L_EXT, first_ext);
    S(hdr, L_SECTYPE, ST_FILE);
}

/* n blocks free in the bitmap and named by nothing, from the root upwards, then from the start (as FFS) */
static int pick(struct zp_bootvol *v, zp_u32 *nb, zp_u32 n, const char **why)
{
    zp_u32 i, b;
    if (v->nfree < n)
        return refuse(why, "not enough free space on the boot partition", ZP_ENOSPC);
    for (i = 0, b = v->root + 1; i < n; b++) {
        if (b == v->f->nblocks)
            b = v->f->reserved;
        if (b == v->root)                                   /* round once: nfree said there are enough (never) */
            return refuse(why, "not enough free space on the boot partition", ZP_ENOSPC);
        if (!owned(v, b) && bm_free(v, b))
            nb[i++] = b;
    }
    return 0;
}

/* the dirty bitmap blocks written, then a flush */
static int bm_write(struct zp_bootvol *v, const unsigned char *dirty, const char **why)
{
    zp_u32 i;
    int rc;
    for (i = 0; i < v->nbm; i++)
        if (dirty[i]) {
            sum_set(v->bm + BS * i, 0);
            if ((rc = wr(v, v->bmblk[i], v->bm + BS * i, why)) != 0)
                return rc;
        }
    return flush(v, why);
}

/* blocks that nothing names any more, given back: in the bitmap and in the walk's record */
static int give_back(struct zp_bootvol *v, const zp_u32 *blk, zp_u32 n, const char **why)
{
    unsigned char dirty[ZP_BOOT_BMPAGES];
    zp_u32 i;
    int rc;
    memset(dirty, 0, sizeof dirty);
    for (i = 0; i < n; i++) {
        bm_give(v, blk[i], dirty);
        v->own[blk[i] >> 3] &= (unsigned char)~(1u << (blk[i] & 7));
    }
    if ((rc = bm_write(v, dirty, why)) != 0)
        return rc;
    v->nfree += n;
    return 0;
}

/* a new entry NAME in directory parent: a file of size bytes from src, or (isdir) an empty directory. *hdrout: its header */
static int put_new(struct zp_bootvol *v, zp_u32 parent, const char *name, zp_u32 size, zp_src src, void *u,
                   const zp_u32 date[3], int isdir, zp_u32 *hdrout, const char **why)
{
    unsigned char *buf = 0, *hdr = 0, dirty[ZP_BOOT_BMPAGES];
    zp_u32 *nb = 0, *ptr = 0, total, ndata, i, b, h, prev = 0, next, nl = (zp_u32)strlen(name), first_ext = 0;
    int rc;

    if (!nl || nl > NAMEMAX || strchr(name, '/') || strchr(name, ':'))
        return refuse(why, "a bad file name", ZP_EINVAL);
    if (size > ZP_BUNDLE_MAX + BS)
        return refuse(why, "a file too large", ZP_EINVAL);
    if (!v->f->write)
        return refuse(why, "the boot partition is not open for writing", ZP_EINVAL);
    ndata = isdir ? 0 : size / BS + (size % BS != 0);
    total = isdir ? 1 : zp_boot_blocks(size);
    buf = malloc(BS);
    hdr = malloc(BS);
    nb = malloc(sizeof *nb * total);
    ptr = malloc(sizeof *ptr * (ndata ? ndata : 1));
    if (!buf || !hdr || !nb || !ptr) {
        rc = refuse(why, "out of memory", ZP_ENOMEM);
        goto out;
    }
    if ((rc = pick(v, nb, total, why)) != 0)
        goto out;
    /* the hash chain's place: before the first entry with a larger block number (FFS keeps chains in block order) */
    h = name_hash(name, v->intl);
    if ((rc = rd(v, parent, buf, why)) != 0)
        goto out;
    for (next = L(buf, 6 + h); next && next < nb[0]; next = L(buf, L_CHAIN)) {
        prev = next;
        if ((rc = header(v, next, T_HEADER, parent, buf, why)) != 0)
            goto out;
    }
    /* 1. the data, the extension blocks, the header: in free blocks, seen by nothing yet */
    if (!isdir && (rc = body(v, nb + 1, nb[0], size, src, u, buf, ptr, &first_ext, why)) != 0)
        goto out;
    memset(hdr, 0, BS);
    S(hdr, 0, T_HEADER);
    S(hdr, 1, nb[0]);
    if (isdir) {
        S(hdr, L_DAYS, date[0]);
        S(hdr, L_DAYS + 1, date[1]);
        S(hdr, L_DAYS + 2, date[2]);
        S(hdr, L_SECTYPE, ST_USERDIR);
    } else
        file_header(hdr, size, ptr, first_ext, date);
    S(hdr, L_PROTECT, 0);                                   /* 0: read, write, execute, delete for everyone */
    hdr[432] = (unsigned char)nl;
    memcpy(hdr + 433, name, nl);
    S(hdr, L_CHAIN, next);
    S(hdr, L_PARENT, parent);
    sum_set(hdr, L_CHECKSUM);
    if ((rc = wr(v, nb[0], hdr, why)) != 0 || (rc = flush(v, why)) != 0)
        goto out;
    /* 2. the bitmap: those blocks in use */
    memset(dirty, 0, sizeof dirty);
    for (i = 0; i < total; i++) {
        bm_take(v, nb[i], dirty);
        v->own[nb[i] >> 3] |= (unsigned char)(1u << (nb[i] & 7));
    }
    if (v->nleak && !v->links)                              /* blocks a cut write left in use that nothing names (the
                                                             * walk proved it): free again in the same writes. Not with
                                                             * hard links: a cut in ffsd's rename (d31 8) leaves a file
                                                             * whose header is reached only through its link */
        for (b = v->f->reserved; b < v->f->nblocks; b++)
            if (!owned(v, b) && !bm_free(v, b))
                bm_give(v, b, dirty);
    if ((rc = bm_write(v, dirty, why)) != 0)
        goto out;
    /* 3. the one write that makes the entry appear: the directory's hash slot, or the chain's previous entry */
    if ((rc = rd(v, prev ? prev : parent, buf, why)) != 0)
        goto out;
    if (L(buf, 0) != T_HEADER || L(buf, 1) != (prev ? prev : parent == v->root ? 0 : parent) || !sum_ok(buf) ||
        L(buf, prev ? L_CHAIN : 6 + h) != next) {           /* (the root block's own number is 0) */
        rc = refuse(why, "the directory changed while the file was written", ZP_EIO);
        goto out;
    }
    S(buf, prev ? L_CHAIN : 6 + h, nb[0]);
    sum_set(buf, L_CHECKSUM);
    if ((rc = wr(v, prev ? prev : parent, buf, why)) != 0 || (rc = flush(v, why)) != 0)
        goto out;
    if (v->links)
        v->nfree -= total;
    else {
        v->nfree = v->nfree - total + v->nleak;
        v->nleak = 0;
    }
    if (hdrout)
        *hdrout = nb[0];
    rc = 0;
out:
    free(ptr);
    free(nb);
    free(hdr);
    free(buf);
    return rc;
}

int zp_boot_create(struct zp_bootvol *v, const char *name, zp_u32 size, zp_src src, void *u, const zp_u32 date[3],
                   const char **why)
{
    static const char *dummy;
    struct zp_bootfile bf;
    int rc;
    if (!why)
        why = &dummy;
    if (!v->dir)
        return refuse(why, "there is no Zenix directory", ZP_ENOENT);
    rc = zp_boot_find(v, name, &bf, why);
    zp_boot_file_free(&bf);
    if (rc != ZP_ENOENT)
        return rc ? rc : refuse(why, "the file is there already", ZP_EEXIST);
    return put_new(v, v->dir, name, size, src, u, date, 0, 0, why);
}

int zp_boot_cd(struct zp_bootvol *v, const char *name, const char **why)
{
    static const char *dummy;
    unsigned char *buf;
    zp_u32 b;
    int rc;
    if (!why)
        why = &dummy;
    if (!name || !name[0]) {
        v->dir = v->root;
        return 0;
    }
    v->dir = 0;
    buf = malloc(BS);
    if (!buf)
        return refuse(why, "out of memory", ZP_ENOMEM);
    if ((rc = rd(v, v->root, buf, why)) != 0)
        goto out;
    rc = refuse(why, "no such directory", ZP_ENOENT);
    for (b = L(buf, 6 + name_hash(name, v->intl)); b; b = L(buf, L_CHAIN)) {
        if ((rc = header(v, b, T_HEADER, v->root, buf, why)) != 0)
            goto out;
        rc = refuse(why, "no such directory", ZP_ENOENT);
        if (!name_is(buf, name, v->intl))
            continue;
        if (L(buf, L_SECTYPE) != ST_USERDIR)
            rc = refuse(why, "a file where a directory is wanted", ZP_EINVAL);
        else {
            v->dir = b;
            rc = 0;
        }
        break;
    }
out:
    free(buf);
    return rc;
}

int zp_boot_mkdir(struct zp_bootvol *v, const char *name, const zp_u32 date[3], const char **why)
{
    static const char *dummy;
    struct zp_bootfile bf;
    zp_u32 blk = 0;
    int rc;
    if (!why)
        why = &dummy;
    if ((rc = zp_boot_cd(v, name, why)) != ZP_ENOENT)
        return rc ? rc : refuse(why, "the directory is there already", ZP_EEXIST);
    v->dir = v->root;                                       /* no file of that name either */
    rc = zp_boot_find(v, name, &bf, why);
    zp_boot_file_free(&bf);
    if (rc != ZP_ENOENT) {
        v->dir = 0;
        return rc ? rc : refuse(why, "a file where a directory is wanted", ZP_EINVAL);
    }
    rc = put_new(v, v->root, name, 0, 0, 0, date, 1, &blk, why);
    v->dir = rc ? 0 : blk;
    return rc;
}

/* a file's header and extension blocks (the caller has its data blocks): into out, *n of them */
static int meta_blocks(struct zp_bootvol *v, zp_u32 hdrblk, unsigned char *buf, zp_u32 *out, zp_u32 max, zp_u32 *n,
                       const char **why)
{
    zp_u32 ext, steps = 0;
    int rc;
    *n = 0;
    out[(*n)++] = hdrblk;
    if ((rc = header(v, hdrblk, T_HEADER, v->dir, buf, why)) != 0)
        return rc;
    for (ext = L(buf, L_EXT); ext; ext = L(buf, L_EXT)) {
        if (*n == max || ++steps > v->f->nblocks)
            return refuse(why, "a damaged extension chain", ZP_EINVAL);
        out[(*n)++] = ext;
        if ((rc = header(v, ext, T_LIST, hdrblk, buf, why)) != 0)
            return rc;
    }
    return 0;
}

int zp_boot_delete(struct zp_bootvol *v, const char *name, const char **why)
{
    static const char *dummy;
    struct zp_bootfile bf;
    unsigned char *buf = 0;
    zp_u32 *gone = 0, nmeta = 0, h, b, prev = 0, chain, at;
    int rc;
    if (!why)
        why = &dummy;
    if (!v->f->write)
        return refuse(why, "the boot partition is not open for writing", ZP_EINVAL);
    if ((rc = zp_boot_find(v, name, &bf, why)) != 0)
        return rc;
    buf = malloc(BS);
    gone = malloc(sizeof *gone * (bf.nblk + bf.nblk / HT + 2));
    if (!buf || !gone) {
        rc = refuse(why, "out of memory", ZP_ENOMEM);
        goto out;
    }
    if ((rc = meta_blocks(v, bf.header, buf, gone, bf.nblk / HT + 2, &nmeta, why)) != 0)
        goto out;
    if ((rc = header(v, bf.header, T_HEADER, v->dir, buf, why)) != 0)
        goto out;
    chain = L(buf, L_CHAIN);
    /* its place in the chain: the directory's slot, or the entry before it */
    h = name_hash(name, v->intl);
    if ((rc = rd(v, v->dir, buf, why)) != 0)
        goto out;
    for (b = L(buf, 6 + h); b && b != bf.header; b = L(buf, L_CHAIN)) {
        prev = b;
        if ((rc = header(v, b, T_HEADER, v->dir, buf, why)) != 0)
            goto out;
    }
    if (!b) {
        rc = refuse(why, "the file is not in its hash chain", ZP_EINVAL);
        goto out;
    }
    /* the one write that makes it disappear; then its blocks are named by nothing and given back */
    at = prev ? prev : v->dir;
    if ((rc = rd(v, at, buf, why)) != 0)
        goto out;
    if (L(buf, 0) != T_HEADER || L(buf, 1) != (at == v->root ? 0 : at) || !sum_ok(buf) || L(buf, prev ? L_CHAIN : 6 + h) != bf.header) {
        rc = refuse(why, "the directory changed while the file was deleted", ZP_EIO);
        goto out;
    }
    S(buf, prev ? L_CHAIN : 6 + h, chain);
    sum_set(buf, L_CHECKSUM);
    if ((rc = wr(v, at, buf, why)) != 0 || (rc = flush(v, why)) != 0)
        goto out;
    memcpy(gone + nmeta, bf.blk, sizeof *gone * bf.nblk);
    rc = give_back(v, gone, nmeta + bf.nblk, why);
out:
    zp_boot_file_free(&bf);
    free(gone);
    free(buf);
    return rc;
}

int zp_boot_replace(struct zp_bootvol *v, const char *name, zp_u32 size, zp_src src, void *u, const zp_u32 date[3],
                    const char **why)
{
    static const char *dummy;
    struct zp_bootfile bf;
    unsigned char *buf = 0, *hdr = 0, dirty[ZP_BOOT_BMPAGES];
    zp_u32 *nb = 0, *ptr = 0, *gone = 0, total, ndata, i, nmeta = 0, first_ext = 0;
    int rc;
    if (!why)
        why = &dummy;
    if (!v->dir)
        return refuse(why, "there is no Zenix directory", ZP_ENOENT);
    rc = zp_boot_find(v, name, &bf, why);
    if (rc == ZP_ENOENT)
        return zp_boot_create(v, name, size, src, u, date, why);
    if (rc)
        return rc;
    if (!v->f->write) {
        rc = refuse(why, "the boot partition is not open for writing", ZP_EINVAL);
        goto out;
    }
    if (size > ZP_BUNDLE_MAX + BS) {
        rc = refuse(why, "a file too large", ZP_EINVAL);
        goto out;
    }
    ndata = size / BS + (size % BS != 0);
    total = zp_boot_blocks(size) - 1;                       /* the header stays where it is */
    buf = malloc(BS);
    hdr = malloc(BS);
    nb = malloc(sizeof *nb * (total ? total : 1));
    ptr = malloc(sizeof *ptr * (ndata ? ndata : 1));
    gone = malloc(sizeof *gone * (bf.nblk + bf.nblk / HT + 2));
    if (!buf || !hdr || !nb || !ptr || !gone) {
        rc = refuse(why, "out of memory", ZP_ENOMEM);
        goto out;
    }
    if ((rc = meta_blocks(v, bf.header, buf, gone, bf.nblk / HT + 2, &nmeta, why)) != 0 ||
        (rc = pick(v, nb, total, why)) != 0)
        goto out;
    /* 1. the new data and extension blocks, in free blocks seen by nothing */
    if ((rc = body(v, nb, bf.header, size, src, u, buf, ptr, &first_ext, why)) != 0 || (rc = flush(v, why)) != 0)
        goto out;
    /* 2. the bitmap: those blocks in use */
    memset(dirty, 0, sizeof dirty);
    for (i = 0; i < total; i++) {
        bm_take(v, nb[i], dirty);
        v->own[nb[i] >> 3] |= (unsigned char)(1u << (nb[i] & 7));
    }
    if (v->nleak && !v->links)                              /* a cut write's leftovers, as put_new gives them back */
        for (i = v->f->reserved; i < v->f->nblocks; i++)
            if (!owned(v, i) && !bm_free(v, i))
                bm_give(v, i, dirty);
    if ((rc = bm_write(v, dirty, why)) != 0)
        goto out;
    v->nfree -= total;
    if (!v->links) {
        v->nfree += v->nleak;
        v->nleak = 0;
    }
    /* 3. the one write: the header anew in its own place (its name, chain, parent, protection and comment kept) */
    if ((rc = header(v, bf.header, T_HEADER, v->dir, hdr, why)) != 0)
        goto out;
    if (!name_is(hdr, name, v->intl) || L(hdr, L_SECTYPE) != ST_FILE) {
        rc = refuse(why, "the file changed while it was written", ZP_EIO);
        goto out;
    }
    file_header(hdr, size, ptr, first_ext, date);
    sum_set(hdr, L_CHECKSUM);
    if ((rc = wr(v, bf.header, hdr, why)) != 0 || (rc = flush(v, why)) != 0)
        goto out;
    /* 4. the old data and extension blocks, named by nothing now, given back (the header is the new file's) */
    memcpy(gone + nmeta, bf.blk, sizeof *gone * bf.nblk);
    rc = give_back(v, gone + 1, nmeta - 1 + bf.nblk, why);
out:
    zp_boot_file_free(&bf);
    free(gone);
    free(ptr);
    free(nb);
    free(hdr);
    free(buf);
    return rc;
}
