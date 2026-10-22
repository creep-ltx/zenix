/* SPDX-License-Identifier: GPL-2.0-only */
/* zenixfs-carve: get files back out of a ZenixFS image whose superblock, commit records, free-space map or upper tree are damaged.
 *
 * It does not trust any of the structure that ties the volume together. It reads the raw image, finds every block that is a valid
 * leaf of the tree (right magic, its own block number, a correct checksum), takes the items from them (inodes, directory entries,
 * file extents, small files) and rebuilds the directories and files from those. Every leaf carries the generation that wrote it,
 * so if two leaves hold the same key the newer one wins.
 *
 * If the newest commit record and the free-space map are still good, only blocks that the map says are in use are read: that
 * is exactly the current tree. If not, old copies of leaves (blocks the filesystem had already replaced but not yet reused) are
 * read too, and a file that was deleted shortly before the damage can come back. Files whose directory was lost are put in
 * lost+found/dir<N> and lost+found/file<N>.
 * Files are never overwritten: a name that already exists in the output is left alone (so this can fill the gaps of a normal
 * salvage). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "zenixfs-carve.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define CSUM_INIT 0x5A4E4653u
#define HDR 32
#define KEYLEN 12

static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u64 rd64(const u8 *p) { return (u64)rd32(p) << 32 | rd32(p + 4); }

static u32 csum_of(const u8 *b, u32 n)
{
    u32 s = CSUM_INIT;
    for (u32 i = 4; i < n; i += 4) { u32 w = rd32(b + i); s = ((s << 1) | (s >> 31)) + w; }
    return s;
}
static int valid_block(const u8 *b, u32 B, u32 self, int type)
{
    if (rd16(b + 4) != 0x5A42 || b[6] != type || rd32(b + 8) != self) return 0;
    return csum_of(b, B) == rd32(b);
}

struct item { u8 key[KEYLEN]; u32 gen, blk; u16 off, size; };
struct carve {
    int fd; u32 B, bshift, nblocks, nb, dstart;
    struct item *it; size_t n, cap;
    u8 *live;                                    /* bitmap of blocks in use, or NULL */
    struct carve_stats *st;
    int verbose, include_stale;
    u8 *visited; u32 maxino;                     /* per inode: 1 = written out */
    u8 *referenced;                              /* per inode: some directory entry names it */
    char *outroot;
    u8 *blkbuf;
};

static int rdblk(struct carve *c, u32 n, u8 *buf)
{
    ssize_t r = pread(c->fd, buf, c->B, (off_t)n * c->B);
    return r == (ssize_t)c->B ? 0 : -1;
}

int zenixfs_guess_bshift(const char *image)
{
    int fd = open(image, O_RDONLY);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    int best = -1; u32 bestn = 0;
    for (int bs = 10; bs <= 12; bs++) {
        u32 B = 1u << bs, nblocks = (u32)(size >> bs), cnt = 0;
        u8 *buf = malloc(B);
        for (u32 n = 9; n < nblocks && n < 40000; n++) {
            if (pread(fd, buf, B, (off_t)n * B) != (ssize_t)B) break;
            if (valid_block(buf, B, n, 4) || valid_block(buf, B, n, 5)) cnt++;
        }
        for (u32 s = 1; s <= 8 && s < nblocks; s++)
            if (pread(fd, buf, B, (off_t)s * B) == (ssize_t)B && valid_block(buf, B, s, 2)) cnt += 3;
        free(buf);
        if (cnt > bestn) { bestn = cnt; best = bs; }
    }
    close(fd);
    return bestn ? best : -1;
}

/* the bitmap of blocks in use, from the newest commit record and the pages it names; NULL if anything is missing or damaged */
static u8 *load_live(struct carve *c)
{
    u8 *buf = malloc(c->B), *best = malloc(c->B);
    u64 bg = 0; int have = 0;
    for (u32 s = 1; s <= 8; s++)
        if (rdblk(c, s, buf) == 0 && valid_block(buf, c->B, s, 2) && rd32(buf + 56) == c->nb) {
            u64 g = rd64(buf + 16);
            if (!have || g > bg) { bg = g; memcpy(best, buf, c->B); have = 1; }
        }
    u8 *live = NULL;
    if (have) {
        u32 bmbytes = c->B - HDR;
        live = calloc(c->nb, bmbytes);
        for (u32 i = 0; i < c->nb && live; i++) {
            u32 sel = (best[60 + i / 8] >> (i % 8)) & 1, blk = 9 + 2 * i + sel;
            if (rdblk(c, blk, buf) || !valid_block(buf, c->B, blk, 3)) { free(live); live = NULL; break; }
            memcpy(live + (size_t)i * bmbytes, buf + HDR, bmbytes);
        }
    }
    free(buf); free(best);
    return live;
}

static int live_bit(struct carve *c, u32 b) { return (c->live[b >> 3] >> (b & 7)) & 1; }

static int keycmp(const void *a, const void *b)
{
    const struct item *x = a, *y = b;
    int r = memcmp(x->key, y->key, KEYLEN);
    if (r) return r;
    return x->gen < y->gen ? 1 : x->gen > y->gen ? -1 : 0;      /* newest first */
}

static void scan(struct carve *c)
{
    u8 *buf = malloc(c->B);
    for (u32 n = c->dstart; n < c->nblocks; n++) {
        if (rdblk(c, n, buf)) break;
        if (!valid_block(buf, c->B, n, 4)) continue;
        c->st->leaves++;
        if (c->live && !live_bit(c, n)) { c->st->leaves_stale++; if (!c->include_stale) continue; }
        u32 cnt = rd16(buf + 24), d = rd16(buf + 26), gen = (u32)rd64(buf + 16);
        if (HDR + 16 * cnt > d || d > c->B) continue;
        for (u32 i = 0; i < cnt; i++) {
            const u8 *ih = buf + HDR + 16 * i;
            u32 off = rd16(ih + 12), sz = rd16(ih + 14);
            if (off < d || off + sz > c->B) continue;
            if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 4096; c->it = realloc(c->it, c->cap * sizeof *c->it); }
            struct item *t = &c->it[c->n++];
            memcpy(t->key, ih, KEYLEN); t->gen = gen; t->blk = n; t->off = (u16)off; t->size = (u16)sz;
        }
    }
    free(buf);
    qsort(c->it, c->n, sizeof *c->it, keycmp);
    size_t w = 0;                                                   /* one item per key: the newest */
    for (size_t i = 0; i < c->n; i++)
        if (w == 0 || memcmp(c->it[w - 1].key, c->it[i].key, KEYLEN)) c->it[w++] = c->it[i];
    c->n = w;
    c->st->items = (u32)w;
}

/* items with this object and kind: [lo, hi) in the sorted array */
static void range(struct carve *c, u32 obj, u8 kind, size_t *lo, size_t *hi)
{
    u8 k[KEYLEN] = { obj >> 24, obj >> 16, obj >> 8, obj, kind, 0, 0, 0, 0, 0, 0, 0 };
    size_t a = 0, b = c->n;
    while (a < b) { size_t m = (a + b) / 2; if (memcmp(c->it[m].key, k, KEYLEN) < 0) a = m + 1; else b = m; }
    *lo = a;
    size_t e = a;
    while (e < c->n && rd32(c->it[e].key) == obj && c->it[e].key[4] == kind) e++;
    *hi = e;
}
static int value(struct carve *c, const struct item *t, u8 *out)    /* reads the value of an item (its leaf must still be readable) */
{
    if (rdblk(c, t->blk, c->blkbuf)) return -1;
    memcpy(out, c->blkbuf + t->off, t->size);
    return t->size;
}

struct cinode { u32 mode, uid, gid, nlink, flags, parent; u64 size, mtime; int have; };
static int get_inode(struct carve *c, u32 ino, struct cinode *o)
{
    size_t lo, hi;
    memset(o, 0, sizeof *o);
    range(c, ino, 1, &lo, &hi);
    u8 v[1100];
    if (lo == hi || value(c, &c->it[lo], v) != 60) return 0;
    o->mode = rd32(v); o->uid = rd32(v + 4); o->gid = rd32(v + 8); o->nlink = rd32(v + 12);
    o->size = rd64(v + 16); o->mtime = rd64(v + 24); o->flags = rd32(v + 40); o->parent = rd32(v + 44);
    o->have = 1;
    return 1;
}

static void mark(u8 *a, u32 n, u32 ino) { if (ino <= n) a[ino] = 1; }

static char *join(const char *dir, const char *name)
{
    size_t l = strlen(dir) + strlen(name) + 2;
    char *p = malloc(l);
    snprintf(p, l, "%s/%s", dir, name);
    return p;
}

static void apply_meta(const char *path, const struct cinode *in)
{
    if (!in->have) return;
    chmod(path, in->mode & 0777);
}

static void emit_file(struct carve *c, u32 ino, const char *path)
{
    struct cinode in;
    int have = get_inode(c, ino, &in);
    if (access(path, F_OK) == 0) { c->st->skipped_existing++; return; }
    mark(c->visited, c->maxino, ino);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { c->st->failed++; return; }
    u64 size = have ? in.size : 0, maxend = 0, covered = 0;
    int incomplete = 0;
    u8 v[1100];
    if (have && (in.flags & 1)) {                                    /* small file stored in the tree */
        size_t lo, hi;
        range(c, ino, 4, &lo, &hi);
        if (lo < hi) { int n = value(c, &c->it[lo], v); if (n > 0) { if (write(fd, v, n) != n) c->st->failed++; maxend = n; } }
        if (maxend < size) incomplete = 1;                          /* the data item is lost */
    } else {
        size_t lo, hi;
        range(c, ino, 3, &lo, &hi);
        u8 *blk = malloc(c->B);
        for (size_t i = lo; i < hi; i++) {
            if (value(c, &c->it[i], v) != 8) continue;
            u32 lbn = rd32(c->it[i].key + 8), phys = rd32(v), len = rd32(v + 4);
            for (u32 k = 0; k < len; k++) {
                if (phys + k >= c->nblocks || rdblk(c, phys + k, blk)) { c->st->data_blocks_lost++; incomplete = 1; continue; }
                u64 off = (u64)(lbn + k) * c->B;
                u64 n = c->B;
                if (have && off >= size) break;
                if (have && off + n > size) n = size - off;
                if (pwrite(fd, blk, n, off) != (ssize_t)n) c->st->failed++;
                if (off + n > maxend) maxend = off + n;
                covered++;
            }
        }
        free(blk);
        if (have && covered < (size + c->B - 1) / c->B) incomplete = 1;       /* extents missing (or a sparse file: the report says which) */
    }
    if (have && ftruncate(fd, size) != 0) c->st->failed++;
    close(fd);
    apply_meta(path, &in);
    /* a file that is not whole must not look whole: give it a name that says so */
    const char *suffix = !have ? ".nosize" : incomplete ? ".partial" : NULL;
    if (suffix) {
        char np[4200]; snprintf(np, sizeof np, "%s%s", path, suffix);
        if (rename(path, np) == 0) { if (!have) c->st->no_inode++; else c->st->partial++; }
    }
    c->st->files++;
}

static void emit_dir(struct carve *c, u32 ino, const char *path, int depth);

static void emit_entries(struct carve *c, u32 dir, const char *path, int depth)
{
    size_t lo, hi;
    range(c, dir, 2, &lo, &hi);
    u8 v[300];
    for (size_t i = lo; i < hi; i++) {
        int n = value(c, &c->it[i], v);
        if (n < 7) continue;
        u32 ino = rd32(v); u8 type = v[4]; int nl = v[5];
        if (nl == 0 || 6 + nl > n) continue;
        char name[260];
        memcpy(name, v + 6, nl); name[nl] = 0;
        if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) continue;
        mark(c->referenced, c->maxino, ino);
        char *p = join(path, name);
        if (type == 4) {
            if (ino <= c->maxino && c->visited[ino]) { free(p); continue; }   /* loop or second link */
            mkdir(p, 0755);
            emit_dir(c, ino, p, depth + 1);
        } else if (type == 10) {                                       /* symbolic link: the target is the inline data */
            size_t a, b;
            range(c, ino, 4, &a, &b);
            u8 t[1100];
            int tl = a < b ? value(c, &c->it[a], t) : 0;
            if (tl > 0 && access(p, F_OK) != 0) { t[tl] = 0; if (symlink((char *)t, p) == 0) c->st->links++; }
            mark(c->visited, c->maxino, ino);
        } else {
            if (ino <= c->maxino && c->visited[ino]) {                 /* a hard link to a file already written: link it */
                free(p); continue;
            }
            emit_file(c, ino, p);
        }
        free(p);
    }
}

static void emit_dir(struct carve *c, u32 ino, const char *path, int depth)
{
    if (depth > 200 || (ino <= c->maxino && c->visited[ino] == 2)) return;
    mark(c->visited, c->maxino, ino);
    if (ino <= c->maxino) c->visited[ino] = 2;
    c->st->dirs++;
    emit_entries(c, ino, path, depth);
    struct cinode in;
    if (get_inode(c, ino, &in)) apply_meta(path, &in);
}

int zenixfs_carve(const char *image, const char *outdir, struct carve_stats *st, int verbose, int include_stale)
{
    struct carve c; memset(&c, 0, sizeof c);
    memset(st, 0, sizeof *st);
    c.st = st; c.verbose = verbose; c.include_stale = include_stale;
    c.fd = open(image, O_RDONLY);
    if (c.fd < 0) return -1;
    int bs = zenixfs_guess_bshift(image);
    if (bs < 0) { close(c.fd); return -2; }                                  /* nothing in it looks like ZenixFS */
    off_t size = lseek(c.fd, 0, SEEK_END);
    c.bshift = bs; c.B = 1u << bs; c.nblocks = (u32)(size >> bs);
    u32 bpb = 8 * (c.B - HDR);
    c.nb = (c.nblocks + bpb - 1) / bpb; c.dstart = 9 + 2 * c.nb;
    st->bshift = bs;
    c.blkbuf = malloc(c.B);
    c.live = load_live(&c);
    st->live_filter = c.live != NULL;
    scan(&c);
    if (c.n == 0) { close(c.fd); free(c.blkbuf); return -3; }
    /* the largest inode number mentioned anywhere */
    for (size_t i = 0; i < c.n; i++) {
        u32 o = rd32(c.it[i].key);
        if (o > c.maxino && o < 100000000) c.maxino = o;
        if (c.it[i].key[4] == 2 && c.it[i].size >= 6) {
            u8 v[300];
            if (value(&c, &c.it[i], v) >= 6) { u32 ino = rd32(v); if (ino > c.maxino && ino < 100000000) c.maxino = ino; }
        }
    }
    c.visited = calloc(c.maxino + 2, 1);
    c.referenced = calloc(c.maxino + 2, 1);
    mkdir(outdir, 0755);
    c.outroot = (char *)outdir;
    emit_dir(&c, 1, outdir, 0);                                              /* from the root directory */
    /* directories that nobody reaches: lost+found/dir<N>, then files nobody names: lost+found/file<N> */
    char *lf = join(outdir, "lost+found");
    for (size_t i = 0; i < c.n; i++) {
        if (c.it[i].key[4] != 2) continue;
        u32 d = rd32(c.it[i].key);
        if (d > c.maxino || c.visited[d]) continue;
        if (c.referenced[d]) continue;                                       /* named by an entry: reached when its parent is emitted */
        mkdir(lf, 0755);
        char nm[40]; snprintf(nm, sizeof nm, "dir%u", d);
        char *p = join(lf, nm);
        mkdir(p, 0755);
        emit_dir(&c, d, p, 0);
        st->lost_dirs++;
        free(p);
    }
    for (size_t i = 0; i < c.n; i++) {                                       /* a directory only named by another lost directory that was never reached */
        if (c.it[i].key[4] != 2) continue;
        u32 d = rd32(c.it[i].key);
        if (d > c.maxino || c.visited[d]) continue;
        mkdir(lf, 0755);
        char nm[40]; snprintf(nm, sizeof nm, "dir%u", d);
        char *p = join(lf, nm);
        mkdir(p, 0755);
        emit_dir(&c, d, p, 0);
        st->lost_dirs++;
        free(p);
    }
    for (size_t i = 0; i < c.n; i++) {
        if (c.it[i].key[4] != 1) continue;                                   /* an inode nobody has written out and nobody names */
        u32 ino = rd32(c.it[i].key);
        if (ino > c.maxino || c.visited[ino] || c.referenced[ino] || ino == 1) continue;
        struct cinode in;
        if (!get_inode(&c, ino, &in) || (in.mode & 0170000) == 0040000) continue;
        mkdir(lf, 0755);
        char nm[40]; snprintf(nm, sizeof nm, "file%u", ino);
        char *p = join(lf, nm);
        emit_file(&c, ino, p);
        st->lost_files++;
        free(p);
    }
    free(lf);
    free(c.it); free(c.live); free(c.visited); free(c.referenced); free(c.blkbuf);
    close(c.fd);
    return 0;
}
