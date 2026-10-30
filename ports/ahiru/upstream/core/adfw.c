/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* adfw - see adfw.h. No Amiga headers here, on purpose.
 *
 * THE ORDER OF WRITES. The Amiga has no journal, so the order is the
 * design: after any block write the volume must be valid, at worst with
 * blocks marked used that nothing points at. Two rules make that:
 *   - a block is written before anything points at it, and marked used
 *     in the bitmap (written) before the one write that links it in;
 *   - a block is unlinked (one write) before it is marked free.
 * The bitmap lives in memory while a change is made; nothing of it goes
 * out before its moment, so an error or Esc half-way just reloads it.
 *
 *   mkdir      allocate (memory) - bitmap - the header - link into the
 *              parent's hash chain (one write: the parent's table or
 *              the previous entry's next field) - the parent's date - the
 *              root's "disk altered" date (one write with the parent's
 *              when the parent is the root)
 *   putfile    allocate - data blocks (FFS: contiguous runs in one write
 *              each) - extension blocks - bitmap - header - link - dates.
 *              Replacing: the new file goes in under a scratch name that
 *              hashes into the SAME chain, then the old one is unlinked,
 *              then the new header is written with the real name (one
 *              write, no relinking), then the old blocks are freed in the
 *              bitmap. A cut between gives: old only / both (new under the
 *              scratch name) / new under the scratch name / new.
 *   delete     unlink - (a hard link: out of its target's link chain) -
 *              bitmap (freed) - dates. The blocks to free are checked
 *              (and freed in memory) first, so a damaged file is refused
 *              before anything is written.
 *   rename     same chain: the header rewritten with the new name (one
 *              write). Otherwise unlink from the old chain - the header
 *              (name, parent, next) - link into the new chain - dates.
 *              Between the unlink and the link the entry is in no
 *              directory: its blocks stay marked used (a valid volume
 *              that lost it, as AmigaDOS's own Rename would), never one
 *              entry in two directories. There is no single-write move:
 *              the header's next field cannot serve two chains.
 *   prot, comment, date: one write of the header, then the root's date.
 * Hash chains are kept in ascending block order, as the Fast File
 * System needs for ExNext (the Guru Book, 15.3.4.8); a new entry goes in
 * at its place, so one write still links it.
 *
 * Allocation: a file looks for a free run long enough for all its
 * blocks from its directory's block upwards (then from the start), and
 * takes header, data and extension blocks in order, so FFS reads them
 * in long runs later; without such a run it takes the first free blocks
 * from there. Blocks read for a change are checked (type, own number,
 * parent, checksum) and a bad one refuses the change: a damaged volume
 * is never made worse.
 */
#include "adfw.h"

#define T_HEADER 2
#define T_DATA 8
#define T_LIST 16
#define ST_ROOT 1
#define ST_DIR 2
#define ST_SLINK 3
#define ST_DLINK 4
#define ST_FILE 0xfffffffdUL    /* -3 */
#define ST_FLINK 0xfffffffcUL   /* -4 */
#define M32 0xffffffffUL
#define BMMAX 65536UL           /* bytes of bitmap kept in memory, at most */

struct adfw {
    adf_wrfn wr;
    void *wctx;
    unsigned long *bmno;        /* the bitmap blocks' numbers */
    unsigned char *pb, *qb, *hb, *xb;   /* parent, chain/scratch, entry, ext */
    unsigned char *bm;          /* the bitmap blocks as on disk */
    unsigned char *dirty;       /* one flag a bitmap block */
    unsigned long nbm, bpb;     /* bitmap blocks; bits in one */
    unsigned long nfree;
    unsigned long cur;          /* where take() looks next */
    unsigned long st1, st2;     /* directories already dated in this change */
    long now[3];
    int rootok;                 /* the root's disk date written in this change */
    int broken;                 /* memory and disk may disagree: no more changes */
};

/* ---- bytes ----------------------------------------------------------- */

static unsigned long g32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

static void p32(unsigned char *p, unsigned long x)
{
    p[0] = (unsigned char)(x >> 24);
    p[1] = (unsigned char)(x >> 16);
    p[2] = (unsigned char)(x >> 8);
    p[3] = (unsigned char)x;
}

static void mset(unsigned char *d, unsigned long n)
{
    while (n--) *d++ = 0;
}

static void mcopy(unsigned char *d, const unsigned char *s, unsigned long n)
{
    while (n--) *d++ = *s++;
}

static int slen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* the sum of a block's longs, 32 bits */
static unsigned long bsum(const unsigned char *b, unsigned long B)
{
    unsigned long s = 0, i;
    for (i = 0; i < B; i += 4) s = (s + g32(b + i)) & M32;
    return s;
}

/* the checksum long at off made right */
static void setck(unsigned char *b, unsigned long B, unsigned long off)
{
    p32(b + off, 0);
    p32(b + off, (0 - bsum(b, B)) & M32);
}

/* ---- blocks ---------------------------------------------------------- */

/* cnt blocks from n out of buf */
static int wblk(struct adfvol *v, unsigned long n, const unsigned char *buf, unsigned long cnt)
{
    struct adfw *w = v->w;
    long len = (long)(cnt * v->bsize);
    if (n < v->lo || n >= v->nblocks || cnt > v->nblocks - n) return ARC_ECORRUPT;
    if (w->wr(w->wctx, v->base + n * v->bsize, buf, len) != len) return ADF_EWRITE;
    return ARC_OK;
}

/* a header-type block (root, header, extension, OFS data): checksum at 20 */
static int whdr(struct adfvol *v, unsigned long n, unsigned char *b)
{
    setck(b, v->bsize, 20);
    return wblk(v, n, b, 1);
}

/* a header block n (root, dir, file, link) read and checked */
static int rhdr(struct adfvol *v, unsigned long n, unsigned char *b)
{
    int r = adf_blk(v, n, b);
    if (r) return r;
    if (g32(b) != T_HEADER || (g32(b + 4) != n && n != v->root) || bsum(b, v->bsize))
        return ARC_ECORRUPT;
    return ARC_OK;
}

#define SEC(b) g32((b) + v->bsize - 4)
#define NEXT(b) ((b) + v->bsize - 16)
#define PARENT(b) ((b) + v->bsize - 12)

/* ---- the bitmap ------------------------------------------------------ */

/* the byte and bit of block k (lo <= k < nblocks): set = free */
static unsigned char *bmbit(struct adfvol *v, unsigned long k, int *mask)
{
    struct adfw *w = v->w;
    unsigned long i = k - v->lo, b = i / w->bpb, r = i % w->bpb;
    *mask = 1 << (r & 7);
    return w->bm + b * v->bsize + 4 + (r >> 5) * 4 + 3 - ((r >> 3) & 3);
}

static int isfree(struct adfvol *v, unsigned long k)
{
    int m;
    return (*bmbit(v, k, &m) & m) != 0;
}

static void setused(struct adfvol *v, unsigned long k)
{
    int m;
    unsigned char *p = bmbit(v, k, &m);
    *p &= ~m;
    v->w->dirty[(k - v->lo) / v->w->bpb] = 1;
    v->w->nfree--;
}

static void setfree(struct adfvol *v, unsigned long k)
{
    int m;
    unsigned char *p = bmbit(v, k, &m);
    *p |= m;
    v->w->dirty[(k - v->lo) / v->w->bpb] = 1;
    v->w->nfree++;
}

/* a walk along the bitmap without a division a block (a 32-bit divide is
   a library call on the 68000): block k is bit r of the bitmap block
   whose bits start at base */
struct bmpos { unsigned long k, r; unsigned char *base; };

#define BMBYTE(p) ((p)->base[((p)->r >> 5) * 4 + 3 - (((p)->r >> 3) & 3)])
#define BMFREE(p) (BMBYTE(p) >> ((p)->r & 7) & 1)

static void bmseek(struct adfvol *v, struct bmpos *p, unsigned long k)
{
    struct adfw *w = v->w;
    unsigned long i = k - v->lo, b = i / w->bpb;
    p->k = k;
    p->r = i - b * w->bpb;
    p->base = w->bm + b * v->bsize + 4;
}

/* n blocks on (n = 1, or 8 from a byte's first bit), wrapping */
static void bmstep(struct adfvol *v, struct bmpos *p, unsigned long n)
{
    p->k += n;
    if (p->k >= v->nblocks) bmseek(v, p, v->lo);
    else if ((p->r += n) == v->w->bpb) {
        p->r = 0;
        p->base += v->bsize;
    }
}

/* every bitmap block from the disk (checksums checked), free blocks counted */
static int bmload(struct adfvol *v)
{
    struct adfw *w = v->w;
    struct bmpos p;
    unsigned long i, k;
    int r;
    for (i = 0; i < w->nbm; i++) {
        unsigned char *b = w->bm + i * v->bsize;
        if ((r = adf_blk(v, w->bmno[i], b))) return r;
        if (bsum(b, v->bsize)) return ARC_ECORRUPT;
        w->dirty[i] = 0;
    }
    w->nfree = 0;
    bmseek(v, &p, v->lo);
    for (k = v->lo; k < v->nblocks; k++, bmstep(v, &p, 1)) w->nfree += BMFREE(&p);
    return ARC_OK;
}

/* memory = disk again after a change that stopped; if even that fails,
   no more changes until the volume is opened again */
static int undo(struct adfvol *v, int r)
{
    if (bmload(v)) v->w->broken = 1;
    return r;
}

static int bmflush(struct adfvol *v)
{
    struct adfw *w = v->w;
    unsigned long i;
    int r;
    for (i = 0; i < w->nbm; i++)
        if (w->dirty[i]) {
            unsigned char *b = w->bm + i * v->bsize;
            setck(b, v->bsize, 0);
            if ((r = wblk(v, w->bmno[i], b, 1))) return r;
            w->dirty[i] = 0;
        }
    return ARC_OK;
}

/* the first free block from k on (wrapping), 0 when there is none */
static unsigned long nextfree(struct adfvol *v, unsigned long k)
{
    struct bmpos p;
    unsigned long n = v->nblocks - v->lo;
    if (k < v->lo || k >= v->nblocks) k = v->lo;
    bmseek(v, &p, k);
    while (n) {
        if (!(p.r & 7) && !BMBYTE(&p) && n >= 8 && p.k + 8 <= v->nblocks) {
            /* eight used blocks at once */
            bmstep(v, &p, 8);
            n -= 8;
        } else {
            if (BMFREE(&p)) return p.k;
            bmstep(v, &p, 1);
            n--;
        }
    }
    return 0;
}

/* the start of need free blocks in a row, from goal up, then from the
   start; 0 when there is no such run */
static unsigned long runat(struct adfvol *v, unsigned long goal, unsigned long need)
{
    struct bmpos p;
    unsigned long start = 0, got, pass, end;
    if (goal < v->lo || goal >= v->nblocks) goal = v->lo;
    for (pass = 0; pass < 2; pass++) {
        end = pass ? goal + need : v->nblocks;
        if (end > v->nblocks) end = v->nblocks;
        got = 0;
        for (bmseek(v, &p, pass ? v->lo : goal); p.k < end; bmstep(v, &p, 1)) {
            if (!BMFREE(&p)) got = 0;
            else {
                if (!got++) start = p.k;
                if (got == need) return start;
            }
            if (p.k == end - 1) break;
        }
    }
    return 0;
}

/* the next block for the change, marked used (in memory) */
static unsigned long take(struct adfvol *v)
{
    struct adfw *w = v->w;
    unsigned long k = w->nfree ? nextfree(v, w->cur) : 0;
    if (!k) return 0;
    setused(v, k);
    w->cur = k + 1;
    return k;
}

/* block n given back (in memory): it must be a used block that is not
   the root or a bitmap block, else the volume is damaged */
static int release(struct adfvol *v, unsigned long n)
{
    struct adfw *w = v->w;
    unsigned long i;
    if (n < v->lo || n >= v->nblocks || n == v->root || isfree(v, n)) return ARC_ECORRUPT;
    for (i = 0; i < w->nbm; i++)
        if (w->bmno[i] == n) return ARC_ECORRUPT;
    setfree(v, n);
    return ARC_OK;
}

/* every block of the entry whose header (block k) is in h given back:
   a file's data and extension blocks, then the header. Extension blocks
   are read into xb and checked. */
static int freeall(struct adfvol *v, unsigned char *h, unsigned long k)
{
    unsigned long B = v->bsize, steps = 0;
    unsigned char *cur = h;
    int r;
    if (SEC(h) == ST_FILE)
        for (;;) {
            unsigned long cnt = g32(cur + 8), j, ext;
            if (cnt > v->tsize) return ARC_ECORRUPT;
            for (j = 0; j < cnt; j++)
                if ((r = release(v, g32(cur + 24 + 4 * (v->tsize - 1 - j))))) return r;
            if (!(ext = g32(cur + B - 8))) break;
            if (++steps > v->nblocks) return ARC_ECORRUPT;
            cur = v->w->xb;
            if ((r = adf_blk(v, ext, cur))) return r;
            if (g32(cur) != T_LIST || g32(cur + 4) != ext || g32(cur + B - 12) != k ||
                bsum(cur, B)) return ARC_ECORRUPT;
            if ((r = release(v, ext))) return r;
        }
    return release(v, k);
}

/* ---- names and dates ------------------------------------------------- */

/* a name AmigaDOS takes: 1..30 characters, no '/' ':' or control ones */
static int okname(const char *s, int n)
{
    int i;
    if (n < 1 || n > 30) return 0;
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        if (c < 0x20 || c == '/' || c == ':' || (c >= 0x7f && c < 0xa0)) return 0;
    }
    return 1;
}

/* the entry's stored name equal to s (n bytes), without case */
static int samename(struct adfvol *v, const unsigned char *b, const char *s, int n)
{
    const unsigned char *p = b + v->bsize - 80;
    int i;
    if (*p != n) return 0;
    for (i = 0; i < n; i++)
        if (adf_up(p[1 + i], v->intl) != adf_up((unsigned char)s[i], v->intl)) return 0;
    return 1;
}

static void setname(struct adfvol *v, unsigned char *b, const char *s, int n)
{
    unsigned char *p = b + v->bsize - 80;
    mset(p, 32);
    *p = (unsigned char)n;
    mcopy(p + 1, (const unsigned char *)s, n);
}

static void setcomment(struct adfvol *v, unsigned char *b, const char *s)
{
    unsigned char *p = b + v->bsize - 184;
    int n = slen(s);
    mset(p, 80);
    *p = (unsigned char)n;
    mcopy(p + 1, (const unsigned char *)s, n);
}

static void setdate(unsigned char *p, long d, long m, long t)
{
    p32(p, (unsigned long)d & M32);
    p32(p + 4, (unsigned long)m & M32);
    p32(p + 8, (unsigned long)t & M32);
}

/* the directory header dir in b gets the time of the change (and the
   volume's altered date when it is the root) */
static void stamp(struct adfvol *v, unsigned char *b, unsigned long dir)
{
    struct adfw *w = v->w;
    setdate(b + v->bsize - 92, w->now[0], w->now[1], w->now[2]);
    if (dir == v->root) {
        setdate(b + v->bsize - 40, w->now[0], w->now[1], w->now[2]);
        w->rootok = 1;
    }
    if (!w->st1) w->st1 = dir;
    else w->st2 = dir;
}

/* the end of every change: the dates of directories d1, d2 (0 = none)
   not dated yet, then the volume's altered date */
static int fin(struct adfvol *v, unsigned long d1, unsigned long d2)
{
    struct adfw *w = v->w;
    unsigned long d[2];
    int i, r;
    d[0] = d1;
    d[1] = d2 != d1 ? d2 : 0;
    for (i = 0; i < 2; i++)
        if (d[i] && d[i] != w->st1 && d[i] != w->st2) {
            if ((r = rhdr(v, d[i], w->pb))) return r;
            stamp(v, w->pb, d[i]);
            if ((r = whdr(v, d[i], w->pb))) return r;
        }
    if (!w->rootok) {
        if ((r = rhdr(v, v->root, w->pb))) return r;
        setdate(w->pb + v->bsize - 40, w->now[0], w->now[1], w->now[2]);
        w->rootok = 1;
        if ((r = whdr(v, v->root, w->pb))) return r;
    }
    return ARC_OK;
}

/* ---- directories ----------------------------------------------------- */

struct ent { unsigned long dir, blk, pred, slot; };

/* the directory header dir into pb, checked */
static int rdir(struct adfvol *v, unsigned long dir)
{
    int r = rhdr(v, dir, v->w->pb);
    unsigned long st;
    if (r) return r;
    st = SEC(v->w->pb);
    return (dir == v->root ? st == ST_ROOT : st == ST_DIR) ? ARC_OK : ARC_ECORRUPT;
}

/* name s (n bytes) in the chain of slot of directory e->dir (its header
   in pb): exact = the stored name without case, else as adf_list shows
   it. The entry's header into b; ADF_ENOTFOUND when not there. Chains
   are bounded (Brent's loop test, as the reader's). */
static int chain(struct adfvol *v, struct ent *e, unsigned long slot, const char *s, int n,
                 int exact, unsigned char *b)
{
    unsigned long k = g32(v->w->pb + 24 + 4 * slot), pred = 0, tort = 0, pw = 1, lam = 0;
    int r;
    while (k) {
        if (k == tort || k == e->dir) return ARC_ECORRUPT;
        if (lam == pw) {
            tort = k;
            pw <<= 1;
            lam = 0;
        }
        lam++;
        if ((r = rhdr(v, k, b))) return r;
        if (g32(PARENT(b)) != e->dir) return ARC_ECORRUPT;
        if (exact ? samename(v, b, s, n) : adf_sameas(v, b, s, n)) {
            e->blk = k;
            e->pred = pred;
            e->slot = slot;
            return ARC_OK;
        }
        pred = k;
        k = g32(NEXT(b));
    }
    return ADF_ENOTFOUND;
}

/* the entry s (n bytes) of directory dir into b and *e. all = also by the
   name adf_list shows (a stored '/' shown as '_'), through every chain */
static int lookup(struct adfvol *v, unsigned long dir, const char *s, int n, int all,
                  struct ent *e, unsigned char *b)
{
    unsigned long i;
    int r;
    e->dir = dir;
    if ((r = rdir(v, dir))) return r;
    r = chain(v, e, adf_hash(v, (const unsigned char *)s, n), s, n, 1, b);
    if (r != ADF_ENOTFOUND || !all) return r;
    for (i = 0; i < v->tsize; i++) {
        r = chain(v, e, i, s, n, 0, b);
        if (r == ARC_OK) {
            /* found where its stored name says it belongs? */
            const unsigned char *p = b + v->bsize - 80;
            if (*p > 30 || adf_hash(v, p + 1, *p) != i) return ARC_ECORRUPT;
            return ARC_OK;
        }
        if (r != ADF_ENOTFOUND) return r;
    }
    return ADF_ENOTFOUND;
}

/* the directory that holds the last part of path: *dir, and that part
   (*name, *len). Directories on the way are followed (hard links too). */
static int parent(struct adfvol *v, const char *path, unsigned long *dir, const char **name, int *len)
{
    struct adfw *w = v->w;
    unsigned long d = v->root;
    const char *rest;
    int r;
    for (;;) {
        int n;
        struct ent e;
        while (*path == '/') path++;
        n = 0;
        while (path[n] && path[n] != '/') n++;
        rest = path + n;
        while (*rest == '/') rest++;
        if (!*rest) {
            if (!n) return ADF_ENOTFOUND;
            *dir = d;
            *name = path;
            *len = n;
            return rdir(v, d);
        }
        if ((r = lookup(v, d, path, n, 1, &e, w->qb))) return r;
        d = e.blk;
        if (SEC(w->qb) == ST_DLINK) {
            d = g32(w->qb + v->bsize - 44);
            if ((r = rhdr(v, d, w->qb))) return r;
        }
        if (SEC(w->qb) != ST_DIR) return ADF_ENOTFOUND;
        path += n;
    }
}

/* header h of block k linked into slot of dir, in ascending order: h is
   written (next, parent set), then the one block that points at it */
static int hook(struct adfvol *v, unsigned long dir, unsigned long slot, unsigned long k,
                unsigned char *h)
{
    struct adfw *w = v->w;
    unsigned long pred = 0, n, steps = 0;
    int r;
    if ((r = rdir(v, dir))) return r;
    n = g32(w->pb + 24 + 4 * slot);
    while (n && n < k) {
        if (++steps > v->nblocks) return ARC_ECORRUPT;
        if ((r = rhdr(v, n, w->qb))) return r;
        if (g32(PARENT(w->qb)) != dir) return ARC_ECORRUPT;
        pred = n;
        n = g32(NEXT(w->qb));
    }
    p32(NEXT(h), n);
    p32(PARENT(h), dir);
    if ((r = whdr(v, k, h))) return r;
    if (!pred) {
        p32(w->pb + 24 + 4 * slot, k);
        stamp(v, w->pb, dir);
        return whdr(v, dir, w->pb);
    }
    p32(NEXT(w->qb), k);
    return whdr(v, pred, w->qb);
}

/* entry e taken out of its chain (one write); nxt = its next field */
static int unhook(struct adfvol *v, struct ent *e, unsigned long nxt)
{
    struct adfw *w = v->w;
    int r;
    if ((r = rdir(v, e->dir))) return r;
    if (!e->pred) {
        if (g32(w->pb + 24 + 4 * e->slot) != e->blk) return ARC_ECORRUPT;
        p32(w->pb + 24 + 4 * e->slot, nxt);
        stamp(v, w->pb, e->dir);
        return whdr(v, e->dir, w->pb);
    }
    if ((r = rhdr(v, e->pred, w->qb))) return r;
    if (g32(NEXT(w->qb)) != e->blk) return ARC_ECORRUPT;
    p32(NEXT(w->qb), nxt);
    return whdr(v, e->pred, w->qb);
}

/* hard link l (its next link nl) taken out of target t's link chain */
static int unchain(struct adfvol *v, unsigned long t, unsigned long l, unsigned long nl)
{
    struct adfw *w = v->w;
    unsigned long k = t, steps = 0;
    int r;
    for (;;) {
        if ((r = rhdr(v, k, w->qb))) return r;
        if (g32(w->qb + v->bsize - 40) == l) {
            p32(w->qb + v->bsize - 40, nl);
            return whdr(v, k, w->qb);
        }
        k = g32(w->qb + v->bsize - 40);
        if (!k || ++steps > v->nblocks) return ARC_OK;   /* not in it: nothing to do */
    }
}

/* ---- the calls ------------------------------------------------------- */

static int begin(struct adfvol *v)
{
    struct adfw *w = v->w;
    if (!w) return ARC_EUNSUP;
    if (w->broken) return ADF_EWRITE;
    w->st1 = w->st2 = 0;
    w->rootok = 0;
    return ARC_OK;
}

static void wfree(struct adfvol *v)
{
    if (v->w) arc_free(v->w);
    v->w = 0;
}

int adf_writable(struct adfvol *v, adf_wrfn wr, void *wctx)
{
    struct adfw *w;
    unsigned long B = v->bsize, nbm, bpb, i, k, ext = 0, size, exts[16];
    unsigned char *b;
    int r, next;
    if (v->w) return ARC_OK;
    if (v->dostype > 3 || B < 512 || (B & 3)) return ARC_EUNSUP;
    bpb = (B / 4 - 1) * 32;
    nbm = (v->nblocks - v->lo + bpb - 1) / bpb;
    if (nbm * B > BMMAX) return ARC_EUNSUP;
    size = sizeof *w + nbm * sizeof(unsigned long) + (4 + nbm) * B + nbm;
    if (!(w = arc_alloc(size))) return ARC_ENOMEM;
    w->bmno = (unsigned long *)(w + 1);
    b = (unsigned char *)(w->bmno + nbm);
    w->pb = b;
    w->qb = b + B;
    w->hb = b + 2 * B;
    w->xb = b + 3 * B;
    w->bm = b + 4 * B;
    w->dirty = w->bm + nbm * B;
    w->nbm = nbm;
    w->bpb = bpb;
    w->wr = wr;
    w->wctx = wctx;
    v->w = w;
    v->wfree = wfree;
    /* the root, its bitmap flag and the bitmap blocks' numbers (25 in the
       root, the rest in extension blocks: B/4-1 each and the next) */
    if ((r = rdir(v, v->root))) goto fail;
    if (g32(w->pb + B - 200) != M32) {
        r = ADF_EVALIDATE;
        goto fail;
    }
    next = 0;
    for (i = 0; i < nbm; i++) {
        if (i < 25) k = g32(w->pb + B - 196 + 4 * i);
        else {
            unsigned long j = (i - 25) % (B / 4 - 1);
            if (!j) {
                ext = g32((i == 25 ? w->pb + B - 96 : w->qb + B - 4));
                /* each extension block is remembered: it must be marked
                 * used too, or a later write could take it */
                if (next >= (int)(sizeof(exts) / sizeof(exts[0])) || ext < v->lo || ext >= v->nblocks) {
                    r = ARC_ECORRUPT;
                    goto fail;
                }
                exts[next++] = ext;
                if ((r = adf_blk(v, ext, w->qb))) goto fail;
            }
            k = g32(w->qb + 4 * j);
        }
        if (k < v->lo || k >= v->nblocks) {
            r = ARC_ECORRUPT;
            goto fail;
        }
        w->bmno[i] = k;
    }
    if ((r = bmload(v))) goto fail;
    /* the root and the bitmap itself must be marked used */
    r = ARC_ECORRUPT;
    if (isfree(v, v->root)) goto fail;
    for (i = 0; i < nbm; i++)
        if (isfree(v, w->bmno[i])) goto fail;
    for (i = 0; i < (unsigned long)next; i++)
        if (isfree(v, exts[i])) goto fail;
    w->cur = v->root;
    return ARC_OK;
fail:
    wfree(v);
    v->wfree = 0;
    return r;
}

void adf_now(struct adfvol *v, long days, long mins, long ticks)
{
    if (!v->w) return;
    v->w->now[0] = days;
    v->w->now[1] = mins;
    v->w->now[2] = ticks;
}

unsigned long adf_freebytes(struct adfvol *v)
{
    if (!v->w) return 0;
    return v->w->nfree * (v->ffs ? v->bsize : v->bsize - 24);
}

int adf_mkdir(struct adfvol *v, const char *path)
{
    struct adfw *w;
    struct ent e;
    unsigned long dir, k, B = v->bsize;
    const char *name;
    int len, r;
    if ((r = begin(v))) return r;
    w = v->w;
    if ((r = parent(v, path, &dir, &name, &len))) return r;
    if (!okname(name, len)) return ADF_ENAME;
    r = lookup(v, dir, name, len, 0, &e, w->xb);
    if (r != ADF_ENOTFOUND) return r ? r : ADF_EEXIST;
    if (!(k = nextfree(v, dir))) return ADF_EFULL;
    setused(v, k);
    mset(w->hb, B);
    p32(w->hb, T_HEADER);
    p32(w->hb + 4, k);
    p32(w->hb + B - 4, ST_DIR);
    setdate(w->hb + B - 92, w->now[0], w->now[1], w->now[2]);
    setname(v, w->hb, name, len);
    if ((r = bmflush(v)) || (r = hook(v, dir, adf_hash(v, (const unsigned char *)name, len), k, w->hb)))
        return undo(v, r);
    return fin(v, dir, 0);
}

/* a name of the same hash slot as name, not in that chain, into s: the
   replaced file's stand-in. 0 when none was found. */
static int scratch(struct adfvol *v, unsigned long dir, const char *name, int len,
                   unsigned long slot, char *s, int *sl)
{
    static const char dig[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    struct ent e;
    int n = len > 26 ? 26 : len, i, r;
    for (i = 0; i < n; i++) s[i] = name[i];
    s[n] = '~';
    for (i = 0; i < 36 * 36 * 36; i++) {
        s[n + 1] = dig[i % 36];
        s[n + 2] = dig[i / 36 % 36];
        s[n + 3] = dig[i / 1296];
        if (adf_hash(v, (const unsigned char *)s, n + 4) != slot) continue;
        r = lookup(v, dir, s, n + 4, 0, &e, v->w->xb);
        if (r == ADF_ENOTFOUND) {
            *sl = n + 4;
            return 1;
        }
        if (r) return 0;
    }
    return 0;
}

/* the data of a new file (header block hdr, in hb, its table filled
   here): blocks taken in order, data written, extension blocks written;
   the header itself is left to hook() */
static int stream(struct adfvol *v, unsigned long hdr, unsigned long size,
                  long (*get)(void *ctx, void *buf, long n), void *gctx)
{
    struct adfw *w = v->w;
    unsigned long B = v->bsize, T = v->tsize, per = v->ffs ? B : B - 24;
    unsigned long cnt = 0, seq = 0, curno = hdr, pend = 0, rs = 0, rl = 0;
    unsigned char *cur = w->hb, *d = v->b[3];
    int r;
    while (size) {
        unsigned long n, chunk = size < per ? size : per;
        unsigned char *dst;
        if (cnt == T) {
            /* the table is full: an extension block next */
            unsigned long ext = take(v);
            if (!ext) return ADF_EFULL;
            p32(cur + 8, cnt);
            p32(cur + B - 8, ext);
            if (cur != w->hb && (r = whdr(v, curno, cur))) return r;
            cur = w->xb;
            curno = ext;
            mset(cur, B);
            p32(cur, T_LIST);
            p32(cur + 4, ext);
            p32(cur + B - 12, hdr);
            p32(cur + B - 4, ST_FILE);
            cnt = 0;
        }
        if (!(n = take(v))) return ADF_EFULL;
        p32(cur + 24 + 4 * (T - 1 - cnt), n);
        cnt++;
        if (!seq++) p32(w->hb + 16, n);
        if (v->ffs) {
            /* blocks in a row go out in one write */
            if (rl && (n != rs + rl || rl == v->run)) {
                if ((r = wblk(v, rs, d, rl))) return r;
                rl = 0;
            }
            if (!rl) rs = n;
            dst = d + rl * B;
            rl++;
        } else {
            /* the one before learns where this one is */
            if (pend) {
                p32(d + 16, n);
                if ((r = whdr(v, pend, d))) return r;
            }
            mset(d, 24);
            p32(d, T_DATA);
            p32(d + 4, hdr);
            p32(d + 8, seq);
            p32(d + 12, chunk);
            pend = n;
            dst = d + 24;
        }
        if (get(gctx, dst, (long)chunk) != (long)chunk) return ADF_ESHORT;
        if (chunk < per) mset(dst + chunk, per - chunk);
        size -= chunk;
    }
    if (rl && (r = wblk(v, rs, d, rl))) return r;
    if (pend && (r = whdr(v, pend, d))) return r;
    p32(cur + 8, cnt);
    if (cur != w->hb && (r = whdr(v, curno, cur))) return r;
    return ARC_OK;
}

int adf_putfile(struct adfvol *v, const char *path, unsigned long size,
                long (*get)(void *ctx, void *buf, long n), void *gctx,
                unsigned long prot, long days, long mins, long ticks,
                const char *comment)
{
    struct adfw *w;
    struct ent old;
    unsigned long dir, B = v->bsize, per, nd, need, hdr, slot, ost = 0, onext = 0, olnk = 0, start;
    const char *name;
    char sname[32];
    int len, r, rep = 0, sl = 0;
    if ((r = begin(v))) return r;
    w = v->w;
    per = v->ffs ? B : B - 24;
    if (!comment) comment = "";
    if ((r = parent(v, path, &dir, &name, &len))) return r;
    if (!okname(name, len) || slen(comment) > 79) return ADF_ENAME;
    slot = adf_hash(v, (const unsigned char *)name, len);
    r = lookup(v, dir, name, len, 0, &old, w->hb);
    if (!r) {
        /* replacing: a file or link only; checked now (its blocks freed in
           memory and taken back), so a damaged one stops us before a write */
        ost = SEC(w->hb);
        if (ost == ST_DIR) return ADF_EEXIST;
        if (ost == ST_FILE && g32(w->hb + B - 40)) return ADF_ELINKED;
        if (ost != ST_FILE && ost != ST_SLINK && ost != ST_FLINK && ost != ST_DLINK)
            return ARC_ECORRUPT;
        if ((r = freeall(v, w->hb, old.blk)) || (r = bmload(v))) return undo(v, r);
        if (!scratch(v, dir, name, len, slot, sname, &sl)) return ADF_EEXIST;
        rep = 1;
    } else if (r != ADF_ENOTFOUND) return r;
    nd = size / per + (size % per != 0);
    need = 1 + nd + (nd ? (nd - 1) / v->tsize : 0);
    if (nd > v->nblocks || need > w->nfree) return ADF_EFULL;
    /* near the directory, in one run if there is room for one */
    start = runat(v, dir, need);
    w->cur = start ? start : dir;
    if (!(hdr = take(v))) return ADF_EFULL;
    mset(w->hb, B);
    p32(w->hb, T_HEADER);
    p32(w->hb + 4, hdr);
    p32(w->hb + B - 4, ST_FILE);
    p32(w->hb + B - 192, prot);
    p32(w->hb + B - 188, size);
    setcomment(v, w->hb, comment);
    setdate(w->hb + B - 92, days, mins, ticks);
    if (rep) setname(v, w->hb, sname, sl);
    else setname(v, w->hb, name, len);
    if ((r = stream(v, hdr, size, get, gctx)) || (r = bmflush(v)) ||
        (r = hook(v, dir, slot, hdr, w->hb)))
        return undo(v, r);
    if (rep) {
        /* the old one out (its chain may have changed: found again) */
        if ((r = lookup(v, dir, name, len, 0, &old, w->hb))) return undo(v, r);
        ost = SEC(w->hb);
        onext = g32(NEXT(w->hb));
        olnk = g32(w->hb + B - 40);
        if (ost == ST_FLINK || ost == ST_DLINK) ost = g32(w->hb + B - 44);
        else ost = 0;
        if ((r = freeall(v, w->hb, old.blk))) return undo(v, r);
        if ((r = unhook(v, &old, onext))) return undo(v, r);
        /* the new one gets its name: one write, same chain */
        if ((r = rhdr(v, hdr, w->hb))) return undo(v, r);
        setname(v, w->hb, name, len);
        if ((r = whdr(v, hdr, w->hb))) return undo(v, r);
        if (ost && (r = unchain(v, ost, old.blk, olnk))) return undo(v, r);
        if ((r = bmflush(v))) return undo(v, r);
    }
    return fin(v, dir, 0);
}

int adf_delete(struct adfvol *v, const char *path)
{
    struct adfw *w;
    struct ent e;
    unsigned long dir, B = v->bsize, st, tgt = 0, i;
    const char *name;
    int len, r;
    if ((r = begin(v))) return r;
    w = v->w;
    if ((r = parent(v, path, &dir, &name, &len))) return r;
    if ((r = lookup(v, dir, name, len, 1, &e, w->hb))) return r;
    st = SEC(w->hb);
    if (st == ST_DIR) {
        for (i = 0; i < v->tsize; i++)
            if (g32(w->hb + 24 + 4 * i)) return ADF_ENOTEMPTY;
    } else if (st == ST_FLINK || st == ST_DLINK) {
        tgt = g32(w->hb + B - 44);
    } else if (st != ST_FILE && st != ST_SLINK)
        return ARC_ECORRUPT;
    if ((st == ST_FILE || st == ST_DIR) && g32(w->hb + B - 40)) return ADF_ELINKED;
    /* checked and freed in memory first; nothing written if it is bad */
    if ((r = freeall(v, w->hb, e.blk))) return undo(v, r);
    if ((r = unhook(v, &e, g32(NEXT(w->hb)))) ||
        (tgt && (r = unchain(v, tgt, e.blk, g32(w->hb + B - 40)))) ||
        (r = bmflush(v)))
        return undo(v, r);
    return fin(v, dir, 0);
}

int adf_rename(struct adfvol *v, const char *from, const char *to)
{
    struct adfw *w;
    struct ent e, t;
    unsigned long sdir, ddir, dslot;
    const char *sname, *dname;
    int slen_, dlen, r;
    if ((r = begin(v))) return r;
    w = v->w;
    if ((r = parent(v, to, &ddir, &dname, &dlen))) return r;
    if (!okname(dname, dlen)) return ADF_ENAME;
    if ((r = parent(v, from, &sdir, &sname, &slen_))) return r;
    if ((r = lookup(v, sdir, sname, slen_, 1, &e, w->hb))) return r;
    r = lookup(v, ddir, dname, dlen, 0, &t, w->xb);
    if (!r && t.blk != e.blk) return ADF_EEXIST;
    if (r && r != ADF_ENOTFOUND) return r;
    if (SEC(w->hb) == ST_DIR && ddir != sdir) {
        /* not into itself: up from the new directory to the root */
        unsigned long k = ddir, steps = 0;
        while (k != v->root) {
            if (k == e.blk) return ADF_ELOOP;
            if (++steps > v->nblocks) return ARC_ECORRUPT;
            if ((r = rhdr(v, k, w->xb))) return r;
            k = g32(PARENT(w->xb));
        }
    }
    dslot = adf_hash(v, (const unsigned char *)dname, dlen);
    if (sdir == ddir && dslot == e.slot) {
        /* the same chain: only the name changes */
        setname(v, w->hb, dname, dlen);
        if ((r = whdr(v, e.blk, w->hb))) return undo(v, r);
        return fin(v, sdir, 0);
    }
    if ((r = unhook(v, &e, g32(NEXT(w->hb))))) return undo(v, r);
    setname(v, w->hb, dname, dlen);
    if ((r = hook(v, ddir, dslot, e.blk, w->hb))) return undo(v, r);
    return fin(v, sdir, ddir);
}

/* the entry at path into hb, for the three setters; *k its block */
static int entry(struct adfvol *v, const char *path, unsigned long *k)
{
    struct ent e;
    unsigned long dir;
    const char *name;
    int len, r;
    if ((r = begin(v)) || (r = parent(v, path, &dir, &name, &len)) ||
        (r = lookup(v, dir, name, len, 1, &e, v->w->hb))) return r;
    *k = e.blk;
    return ARC_OK;
}

int adf_setprot(struct adfvol *v, const char *path, unsigned long prot)
{
    unsigned long k;
    int r;
    if ((r = entry(v, path, &k))) return r;
    p32(v->w->hb + v->bsize - 192, prot);
    if ((r = whdr(v, k, v->w->hb))) return undo(v, r);
    return fin(v, 0, 0);
}

int adf_setcomment(struct adfvol *v, const char *path, const char *comment)
{
    unsigned long k;
    int r;
    if (slen(comment) > 79) return ADF_ENAME;
    if ((r = entry(v, path, &k))) return r;
    setcomment(v, v->w->hb, comment);
    if ((r = whdr(v, k, v->w->hb))) return undo(v, r);
    return fin(v, 0, 0);
}

int adf_setdate(struct adfvol *v, const char *path, long days, long mins, long ticks)
{
    unsigned long k;
    int r;
    if ((r = entry(v, path, &k))) return r;
    setdate(v->w->hb + v->bsize - 92, days, mins, ticks);
    if ((r = whdr(v, k, v->w->hb))) return undo(v, r);
    return fin(v, 0, 0);
}

/* ---- a blank volume -------------------------------------------------- */

/* the boot block of Kickstart 1.x's Install: finds dos.library's
   resident and returns its init (every Kickstart boots it) */
static const unsigned char bootcode[] = {
    0x43, 0xfa, 0x00, 0x18, 0x4e, 0xae, 0xff, 0xa0, 0x4a, 0x80, 0x67, 0x0a,
    0x20, 0x40, 0x20, 0x68, 0x00, 0x16, 0x70, 0x00, 0x4e, 0x75, 0x70, 0xff,
    0x60, 0xfa, 'd', 'o', 's', '.', 'l', 'i', 'b', 'r', 'a', 'r', 'y', 0
};

int adf_format(adf_wrfn wr, void *wctx, int hd, int dostype, const char *name,
               long days, long mins, long ticks)
{
    unsigned long nb = hd ? 3520 : 1760, root = nb / 2, k, i, chunk, s, c;
    unsigned char *b;
    int len = slen(name), r = ADF_EWRITE, boot = dostype & ADF_BOOT;
    dostype &= ~ADF_BOOT;
    if (dostype < 0 || dostype > 3) return ARC_EUNSUP;
    if (!okname(name, len)) return ADF_ENAME;
    chunk = 11;
    if (!(b = arc_alloc(chunk * 512))) {
        chunk = 2;
        if (!(b = arc_alloc(chunk * 512))) return ARC_ENOMEM;
    }
    /* every block cleared; the boot block last, so a stopped format is
       not a DOS disk */
    for (k = 2; k < nb; k += c) {
        c = nb - k < chunk ? nb - k : chunk;
        if (wr(wctx, k * 512, b, (long)(c * 512)) != (long)(c * 512)) goto out;
    }
    /* the bitmap (root + 1): every block free but the root and itself;
       the bits past the volume are set too, as Format does */
    for (i = 4; i < 512; i++) b[i] = 0xff;
    for (k = root; k <= root + 1; k++) {
        unsigned long j = k - 2;
        b[4 + (j >> 5) * 4 + 3 - ((j >> 3) & 3)] &= ~(1 << (j & 7));
    }
    setck(b, 512, 0);
    if (wr(wctx, (root + 1) * 512, b, 512) != 512) goto out;
    /* the root */
    mset(b, 512);
    p32(b, T_HEADER);
    p32(b + 12, 72);
    p32(b + 512 - 200, M32);
    p32(b + 512 - 196, root + 1);
    setdate(b + 512 - 92, days, mins, ticks);
    setdate(b + 512 - 40, days, mins, ticks);
    setdate(b + 512 - 28, days, mins, ticks);
    b[512 - 80] = (unsigned char)len;
    mcopy(b + 512 - 79, (const unsigned char *)name, len);
    p32(b + 512 - 4, ST_ROOT);
    setck(b, 512, 20);
    if (wr(wctx, root * 512, b, 512) != 512) goto out;
    /* the boot block */
    mset(b, 1024);
    b[0] = 'D';
    b[1] = 'O';
    b[2] = 'S';
    b[3] = (unsigned char)dostype;
    p32(b + 8, root);
    if (boot) {
        mcopy(b + 12, bootcode, sizeof bootcode);
        /* the boot checksum: a sum with carries, inverted */
        for (i = 0, s = 0; i < 1024; i += 4) {
            unsigned long x = g32(b + i);
            s = (s + x) & M32;
            if (s < x) s = (s + 1) & M32;
        }
        p32(b + 4, ~s & M32);
    }
    if (wr(wctx, 0, b, 1024) != 1024) goto out;
    r = ARC_OK;
out:
    arc_free(b);
    return r;
}

const char *adf_errstr(int e)
{
    switch (e) {
    case ADF_EWRITE: return "the image could not be written";
    case ADF_EFULL: return "disk full";
    case ADF_EEXIST: return "the name is taken";
    case ADF_ENOTFOUND: return "not found";
    case ADF_ENOTEMPTY: return "directory not empty";
    case ADF_ENAME: return "not a valid name";
    case ADF_ELOOP: return "a directory cannot go into itself";
    case ADF_ELINKED: return "has hard links";
    case ADF_ESHORT: return "the file ended early";
    case ADF_EVALIDATE: return "the disk needs validating";
    }
    return arc_errstr(e);
}
