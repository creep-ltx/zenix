/* SPDX-License-Identifier: GPL-2.0-only */
/* ZenixFS library, version 1 of the format (docs/zenixfs-format.md).
 * Status: host-tested only. Known shortcuts, to be removed before the Zenix port:
 *  - the whole free-space bitmap (and two marker bitmaps) is held in RAM (the plan says: windowed);
 *  - fsck keeps per-inode tables in RAM;
 *  - an ENOSPC in the middle of a tree change can leak a block or two until fsck (reserve makes it rare). */
#ifdef ZENIXFS_FREESTANDING
#include "zenixfs_env.h"      /* inside Zenix programs: no host headers (user/zenixfs_env.h) */
#else
#include <string.h>
#include <stdlib.h>
#define ZENIXFS_MALLOC(n) malloc(n)
#define ZENIXFS_FREE(p)   free(p)
#endif
#include "zenixfs.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define HDR 32
#define T_SUPER 1
#define T_COMMIT 2
#define T_BITMAP 3
#define T_LEAF 4
#define T_NODE 5
#define MAGIC16 0x5A42
#define SBMAGIC 0x5A4E4653u   /* 'ZNFS' */
#define RING 8
#define K_INODE 1
#define K_DIRENT 2
#define K_EXTENT 3
#define K_INLINE 4
#define K_ORPHAN 5
#define IFL_INLINE 1
#define VMAX 1024
#define KEYLEN 12
#define INODE_LEN 60
#define KIND_TREE 0
#define KIND_DATA 1

static inline u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static inline u64 rd64(const u8 *p) { return (u64)rd32(p) << 32 | rd32(p + 4); }
static inline void wr16(u8 *p, u16 v) { p[0] = v >> 8; p[1] = v; }
static inline void wr32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void wr64(u8 *p, u64 v) { wr32(p, v >> 32); wr32(p + 4, (u32)v); }

struct cb { u32 blk; u8 *data; u8 valid, dirty, kind, pin, fresh; u32 lru; struct cb *next; };   /* fresh: allocated in this transaction */
struct bmpage { u32 idx; u8 valid, dirty; u32 lru; u8 *data; };   /* one page of the free-space bitmap (B - HDR bytes) */
struct run { u32 start, len; };
#define PENDMAX 1024                 /* runs of blocks freed in this transaction, kept allocated until the commit (8 KB) */

struct zenixfs {
    struct zenixfs_dev *dev;
    u32 bshift, B, nblocks, nb, bpb, reserve, dstart, maxitem, maxname;
    u32 charset;
    u32 gen;                     /* the transaction being built */
    u32 root, height, free_blocks, next_ino;
    u64 now;
    u8 *bmsel;                   /* per bitmap page: which of its two copies is current */
    u8 *bmstaged;                /* per page: the other copy already holds newer content, switched at the commit */
    u8 *bmfull;                  /* per page: known to have no free block (a hint, cleared when something is freed there) */
    struct bmpage *bmp; u32 nbmp, bmclock;       /* the page cache: a few pages, not the whole map */
    struct run pend[PENDMAX]; u32 npend, pend_blocks;
    u32 bmbytes;                 /* bytes of payload per bitmap block (B - HDR) */
    struct cb *cbs; struct cb **htab; u32 cap, hsize, clock, ndirty;
    u32 meta_hint, data_hint;
    int in_reserve, failed, txn_dirty, readonly;
    u8 *sc_r, *sc_z, *sc_c, *sc_b, *sc_sel;   /* scratch blocks for read, truncate, commit, and the bitmap selector copy: allocated once at mount
                                         * (the Zenix malloc cannot give small blocks back, a server must not allocate while running) */
    struct zenixfs_stats st;
};

/* ---------- checksum and headers ---------- */
static u32 csum_of(const u8 *b, u32 n)
{
    u32 s = SBMAGIC;
    for (u32 i = 4; i < n; i += 4) { u32 w = rd32(b + i); s = ((s << 1) | (s >> 31)) + w; }
    return s;
}
static void hdr_set(u8 *b, u8 type, u8 level, u32 self)
{
    wr16(b + 4, MAGIC16); b[6] = type; b[7] = level; wr32(b + 8, self);
}
static void seal(u8 *b, u32 n, u32 gen)
{
    wr64(b + 16, gen);
    wr32(b, 0);
    wr32(b, csum_of(b, n));
}
static int verify(const u8 *b, u32 n, u8 type, u32 self)
{
    if (rd16(b + 4) != MAGIC16 || b[6] != type || rd32(b + 8) != self) return 0;
    return csum_of(b, n) == rd32(b);
}
static int verify_tree(const struct zenixfs *fs, const u8 *b, u32 self);

/* ---------- bit helpers ---------- */
static inline int bit_get(const u8 *a, u32 i) { return (a[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(u8 *a, u32 i) { a[i >> 3] |= 1 << (i & 7); }
static inline void bit_clr(u8 *a, u32 i) { a[i >> 3] &= ~(1 << (i & 7)); }

/* ---------- block cache ---------- */
static struct cb *cb_find(struct zenixfs *fs, u32 blk)
{
    for (struct cb *c = fs->htab[blk & (fs->hsize - 1)]; c; c = c->next)
        if (c->valid && c->blk == blk) return c;
    return NULL;
}
static void cb_unhash(struct zenixfs *fs, struct cb *c)
{
    struct cb **pp = &fs->htab[c->blk & (fs->hsize - 1)];
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    c->next = NULL;
}
static struct cb *cb_alloc(struct zenixfs *fs, u32 blk)
{
    struct cb *best = NULL;
    for (u32 i = 0; i < fs->cap; i++) {
        struct cb *c = &fs->cbs[i];
        if (!c->valid) { best = c; break; }
        if (!c->dirty && !c->pin && (!best || c->lru < best->lru)) best = c;
    }
    if (!best) return NULL;
    if (best->valid) cb_unhash(fs, best);
    best->blk = blk; best->valid = 1; best->dirty = 0; best->fresh = 0; best->pin = 1; best->lru = ++fs->clock;
    best->next = fs->htab[blk & (fs->hsize - 1)];
    fs->htab[blk & (fs->hsize - 1)] = best;
    return best;
}
static void cb_markdirty(struct zenixfs *fs, struct cb *c)
{
    if (!c->dirty) { c->dirty = 1; fs->ndirty++; }
    fs->txn_dirty = 1;
}
static int cb_get(struct zenixfs *fs, u32 blk, int kind, struct cb **out)
{
    struct cb *c = cb_find(fs, blk);
    if (c) { c->pin++; c->lru = ++fs->clock; fs->st.cache_hits++; *out = c; return 0; }
    fs->st.cache_misses++;
    c = cb_alloc(fs, blk);
    if (!c) return ZENIXFS_EIO;
    c->kind = kind;
    fs->st.reads++;
    if (fs->dev->read(fs->dev->ctx, blk, c->data) < 0) { c->valid = 0; cb_unhash(fs, c); c->pin = 0; return ZENIXFS_EIO; }
    if (kind == KIND_TREE && !verify_tree(fs, c->data, blk)) { c->valid = 0; cb_unhash(fs, c); c->pin = 0; return ZENIXFS_ECORRUPT; }
    *out = c;
    return 0;
}
static struct cb *cb_new(struct zenixfs *fs, u32 blk, int kind)
{
    struct cb *c = cb_find(fs, blk);
    if (c) { c->pin++; }
    else { c = cb_alloc(fs, blk); if (!c) return NULL; }
    c->kind = kind;
    c->fresh = 1;
    memset(c->data, 0, fs->B);
    cb_markdirty(fs, c);
    return c;
}
static void cb_put(struct cb *c) { if (c->pin) c->pin--; }
static void cb_drop(struct zenixfs *fs, u32 blk)
{
    struct cb *c = cb_find(fs, blk);
    if (!c) return;
    if (c->dirty) { c->dirty = 0; fs->ndirty--; }
    if (!c->pin) { cb_unhash(fs, c); c->valid = 0; }
}

static int dev_write(struct zenixfs *fs, u32 blk, const void *buf)
{
    if (fs->readonly) return -1;                            /* a read-only mount never reaches the device */
    fs->st.writes++;
    return fs->dev->write(fs->dev->ctx, blk, buf);
}
static int dev_flush(struct zenixfs *fs) { fs->st.flushes++; return fs->dev->flush(fs->dev->ctx); }
#define BMBLK(i, copy) (9 + 2 * (i) + (copy))

/* ---------- the free-space bitmap: a few pages in RAM ----------
 * Page i (a block of the volume, two copies at BMBLK(i,0/1)) holds the bits of blocks i*bpb .. (i+1)*bpb-1; 1 = used.
 * A page that changes is written to its NON-current copy (when evicted, or at the commit) and the commit record then
 * says which copy is current, so the committed bitmap is never overwritten. Blocks freed in a transaction stay marked
 * used until the commit (they may still belong to the last commit), kept as runs in pend[]. */
static int bm_stage(struct zenixfs *fs, struct bmpage *pg)
{
    u8 *buf = fs->sc_b; u32 i = pg->idx, blk = BMBLK(i, fs->bmsel[i] ^ 1);
    memset(buf, 0, fs->B);
    memcpy(buf + HDR, pg->data, fs->bmbytes);
    hdr_set(buf, T_BITMAP, 0, blk);
    seal(buf, fs->B, fs->gen);
    if (dev_write(fs, blk, buf) < 0) { fs->failed = 1; return ZENIXFS_EIO; }
    fs->bmstaged[i] = 1; pg->dirty = 0;
    return 0;
}
static struct bmpage *bm_get(struct zenixfs *fs, u32 i)
{
    struct bmpage *v = NULL;
    for (u32 k = 0; k < fs->nbmp; k++) {
        struct bmpage *p = &fs->bmp[k];
        if (p->valid && p->idx == i) { p->lru = ++fs->bmclock; return p; }
        if (!v || (v->valid && (!p->valid || p->lru < v->lru))) v = p;
    }
    if (v->valid && v->dirty && bm_stage(fs, v)) return NULL;
    u32 blk = BMBLK(i, fs->bmsel[i] ^ (fs->bmstaged[i] ? 1 : 0));
    fs->st.reads++;
    if (fs->dev->read(fs->dev->ctx, blk, fs->sc_b) < 0 || !verify(fs->sc_b, fs->B, T_BITMAP, blk)) { v->valid = 0; return NULL; }
    memcpy(v->data, fs->sc_b + HDR, fs->bmbytes);
    v->idx = i; v->valid = 1; v->dirty = 0; v->lru = ++fs->bmclock;
    return v;
}

static u32 alloc_block(struct zenixfs *fs, int data, int *err)
{
    if (fs->free_blocks <= fs->reserve && !fs->in_reserve) { *err = ZENIXFS_ENOSPC; return 0; }
    u32 start = data ? fs->data_hint : fs->meta_hint;
    if (start < fs->dstart || start >= fs->nblocks) start = fs->dstart;
    u32 b = start, left = fs->nblocks - fs->dstart;
    while (left) {
        if (b >= fs->nblocks) b = fs->dstart;
        u32 i = b / fs->bpb, base = i * fs->bpb, pageend = base + fs->bpb;
        if (pageend > fs->nblocks) pageend = fs->nblocks;
        u32 step = pageend - b;
        if (fs->bmfull[i]) { b = pageend; left = left > step ? left - step : 0; continue; }
        struct bmpage *pg = bm_get(fs, i);
        if (!pg) { *err = ZENIXFS_EIO; return 0; }
        u32 bit = b - base, end = pageend - base;
        while (bit < end) {
            if (!(bit & 7) && pg->data[bit >> 3] == 0xFF) { bit += 8; continue; }
            if (!bit_get(pg->data, bit)) break;
            bit++;
        }
        if (bit < end) {
            u32 blk = base + bit;
            bit_set(pg->data, bit); pg->dirty = 1;
            fs->free_blocks--; fs->txn_dirty = 1;
            if (data) fs->data_hint = blk + 1; else fs->meta_hint = blk + 1;
            return blk;
        }
        if (b == base) fs->bmfull[i] = 1;                    /* looked at the whole page: nothing free */
        b = pageend; left = left > step ? left - step : 0;
    }
    *err = ZENIXFS_ENOSPC;
    return 0;
}
static inline int isnew(struct zenixfs *fs, u32 b) { struct cb *c = cb_find(fs, b); return c && c->fresh; }
static void free_block(struct zenixfs *fs, u32 b)
{
    int fresh = isnew(fs, b);
    cb_drop(fs, b);
    fs->free_blocks++; fs->txn_dirty = 1;
    if (fresh) {                                             /* never part of a commit: reusable at once */
        u32 i = b / fs->bpb;
        struct bmpage *pg = bm_get(fs, i);
        if (!pg) { fs->failed = 1; return; }
        bit_clr(pg->data, b - i * fs->bpb); pg->dirty = 1; fs->bmfull[i] = 0;
        return;
    }
    if (fs->npend && fs->pend[fs->npend - 1].start + fs->pend[fs->npend - 1].len == b) fs->pend[fs->npend - 1].len++;
    else if (fs->npend < PENDMAX) { fs->pend[fs->npend].start = b; fs->pend[fs->npend].len = 1; fs->npend++; }
    else { fs->failed = 1; return; }                         /* pressure() and the truncate loop keep room; this is a bug guard */
    fs->pend_blocks++;
}
/* at the commit: the blocks freed meanwhile become free in the bitmap that is being committed */
static int apply_pending(struct zenixfs *fs)
{
    for (u32 r = 0; r < fs->npend; r++)
        for (u32 k = 0; k < fs->pend[r].len; k++) {
            u32 b = fs->pend[r].start + k, i = b / fs->bpb;
            struct bmpage *pg = bm_get(fs, i);
            if (!pg) return ZENIXFS_EIO;
            bit_clr(pg->data, b - i * fs->bpb); pg->dirty = 1; fs->bmfull[i] = 0;
        }
    fs->npend = 0; fs->pend_blocks = 0;
    return 0;
}

/* Can n more tree changes be made without running out of blocks halfway? Checked before an operation changes anything: an
 * ENOSPC in the middle would leave blocks allocated that nothing uses. Each change copies at most height+2 blocks. Normal
 * operations must leave the reserve alone; unlink, rmdir and truncate (in_reserve) may use it. Blocks freed in this
 * transaction cannot be reused before the commit, so they do not count. */
static int room(struct zenixfs *fs, u32 n)
{
    u32 avail = fs->free_blocks > fs->pend_blocks ? fs->free_blocks - fs->pend_blocks : 0;
    u32 floor = fs->in_reserve ? 0 : fs->reserve;
    return avail >= floor + n * (fs->height + 2);
}

/* ---------- keys ---------- */
static void mkkey(u8 *k, u32 obj, u8 kind, u32 off)
{
    wr32(k, obj); k[4] = kind; k[5] = k[6] = k[7] = 0; wr32(k + 8, off);
}
static inline int kcmp(const u8 *a, const u8 *b) { return memcmp(a, b, KEYLEN); }
static int kdec(u8 *k)
{
    for (int i = KEYLEN - 1; i >= 0; i--) { if (k[i]) { k[i]--; return 1; } k[i] = 0xFF; }
    return 0;
}

/* ---------- leaf and node block access ---------- */
#define NITEMS(b) rd16((b) + 24)
#define SETN(b, n) wr16((b) + 24, (n))
#define DATAOFF(b) rd16((b) + 26)
#define SETDO(b, o) wr16((b) + 26, (o))
#define IH(b, i) ((b) + HDR + 16 * (i))      /* item header: key 12, off 2, size 2; node entry: key 12, child 4 */

/* a tree block read from the disk: header, checksum, AND its structure (a crafted image can have good checksums):
 * item count, item offsets and sizes inside the block and at most maxitem (every caller's buffer is VMAX), node levels
 * and children inside the data area. A block that fails is ECORRUPT: nothing is copied out of it. */
#define MAXHEIGHT 24
static int verify_tree(const struct zenixfs *fs, const u8 *b, u32 self)
{
    u32 B = fs->B;
    if (rd16(b + 4) != MAGIC16 || (b[6] != T_LEAF && b[6] != T_NODE) || rd32(b + 8) != self) return 0;
    if (csum_of(b, B) != rd32(b)) return 0;
    u32 n = NITEMS(b);
    if (n > (B - HDR) / 16) return 0;
    if (b[6] == T_LEAF) {
        u32 d = DATAOFF(b);
        if (b[7] != 0 || HDR + 16 * n > d || d > B) return 0;
        for (u32 i = 0; i < n; i++) {
            u32 off = rd16(IH(b, i) + 12), sz = rd16(IH(b, i) + 14);
            if (off < d || off + sz > B || sz > fs->maxitem) return 0;
        }
    } else {
        if (b[7] == 0 || b[7] >= MAXHEIGHT || n == 0) return 0;
        for (u32 i = 0; i < n; i++) {
            u32 ch = rd32(IH(b, i) + 12);
            if (ch < fs->dstart || ch >= fs->nblocks) return 0;
        }
    }
    return 1;
}
/* the tree block at the given level (0 = leaf): a block of another kind or level there is damage */
static int tree_get(struct zenixfs *fs, u32 blk, int level, struct cb **out)
{
    int e = cb_get(fs, blk, KIND_TREE, out);
    if (e) return e;
    const u8 *b = (*out)->data;
    if (b[7] != level || b[6] != (level ? T_NODE : T_LEAF)) { cb_put(*out); return ZENIXFS_ECORRUPT; }
    return 0;
}

static u32 leaf_free(const u8 *b)
{
    return DATAOFF(b) - (HDR + 16 * NITEMS(b));
}
/* first index with key >= k */
static u32 lower_bound(const u8 *b, const u8 *k)
{
    u32 lo = 0, hi = NITEMS(b);
    while (lo < hi) { u32 m = (lo + hi) / 2; if (kcmp(IH(b, m), k) < 0) lo = m + 1; else hi = m; }
    return lo;
}
static void leaf_insert(u8 *b, u32 idx, const u8 *key, const u8 *val, u32 len)
{
    u32 n = NITEMS(b);
    memmove(IH(b, idx + 1), IH(b, idx), 16 * (n - idx));
    u32 off = DATAOFF(b) - len;
    memcpy(b + off, val, len);
    memcpy(IH(b, idx), key, KEYLEN); wr16(IH(b, idx) + 12, off); wr16(IH(b, idx) + 14, len);
    SETDO(b, off); SETN(b, n + 1);
}
static void leaf_remove(u8 *b, u32 idx)
{
    u32 n = NITEMS(b), off = rd16(IH(b, idx) + 12), sz = rd16(IH(b, idx) + 14), d = DATAOFF(b);
    memmove(b + d + sz, b + d, off - d);
    memmove(IH(b, idx), IH(b, idx + 1), 16 * (n - idx - 1));
    SETN(b, n - 1);
    /* every item stored at or below the removed one moves up by its size; <= because an item of size 0 can sit at the very same offset */
    for (u32 i = 0; i < n - 1; i++) { u32 o = rd16(IH(b, i) + 12); if (o <= off) wr16(IH(b, i) + 12, o + sz); }
    SETDO(b, d + sz);
}
static void leaf_init(struct zenixfs *fs, struct cb *c, u32 blk)
{
    hdr_set(c->data, T_LEAF, 0, blk); SETN(c->data, 0); SETDO(c->data, fs->B);
}
static void node_init(struct cb *c, u32 blk, u8 level)
{
    hdr_set(c->data, T_NODE, level, blk); SETN(c->data, 0); SETDO(c->data, 0);
}
static void node_insert(u8 *b, u32 pos, const u8 *key, u32 child)
{
    u32 n = NITEMS(b);
    memmove(IH(b, pos + 1), IH(b, pos), 16 * (n - pos));
    memcpy(IH(b, pos), key, KEYLEN); wr32(IH(b, pos) + 12, child); SETN(b, n + 1);
}
static void node_remove(u8 *b, u32 pos)
{
    u32 n = NITEMS(b);
    memmove(IH(b, pos), IH(b, pos + 1), 16 * (n - pos - 1)); SETN(b, n - 1);
}
/* child slot to follow: last entry with key <= k (entry 0 is minus infinity) */
static u32 node_slot(const u8 *b, const u8 *k)
{
    u32 lo = 1, hi = NITEMS(b);
    while (lo < hi) { u32 m = (lo + hi) / 2; if (kcmp(IH(b, m), k) <= 0) lo = m + 1; else hi = m; }
    return lo - 1;
}

/* copy-on-write: returns a pinned, writable (new in this transaction) cb for blk */
static int cow(struct zenixfs *fs, u32 blk, struct cb **out)
{
    struct cb *c;
    int e = cb_get(fs, blk, KIND_TREE, &c);
    if (e) return e;
    if (isnew(fs, blk)) { cb_markdirty(fs, c); *out = c; return 0; }
    u32 nbk = alloc_block(fs, 0, &e);
    if (!nbk) { cb_put(c); return e; }
    struct cb *n = cb_new(fs, nbk, KIND_TREE);
    if (!n) { cb_put(c); return ZENIXFS_EIO; }
    memcpy(n->data, c->data, fs->B);
    wr32(n->data + 8, nbk);
    cb_put(c);
    free_block(fs, blk);
    *out = n;
    return 0;
}

/* ---------- tree search ---------- */
#define S_EXACT 0
#define S_CEIL 1
#define S_FLOOR 2
static int t_search(struct zenixfs *fs, const u8 *key0, int mode, u8 *okey, u8 *oval, int *olen)
{
    u8 key[KEYLEN];
    memcpy(key, key0, KEYLEN);
    for (;;) {
        u32 blk = fs->root;
        u8 lower[KEYLEN], upper[KEYLEN]; int lv = 0, uv = 0;
        for (int level = fs->height - 1; ; level--) {
            struct cb *c; int e = tree_get(fs, blk, level, &c);
            if (e) return e;
            u8 *b = c->data;
            if (level > 0) {
                u32 idx = node_slot(b, key), n = NITEMS(b);
                if (idx > 0) { memcpy(lower, IH(b, idx), KEYLEN); lv = 1; }
                if (idx + 1 < n) { memcpy(upper, IH(b, idx + 1), KEYLEN); uv = 1; }
                blk = rd32(IH(b, idx) + 12);
                cb_put(c);
                continue;
            }
            u32 n = NITEMS(b), lo = lower_bound(b, key), hit = 0, found = 0;
            int eq = lo < n && !kcmp(IH(b, lo), key);
            if (eq) { hit = lo; found = 1; }
            else if (mode == S_CEIL && lo < n) { hit = lo; found = 1; }
            else if (mode == S_FLOOR && lo > 0) { hit = lo - 1; found = 1; }
            if (found) {
                u32 off = rd16(IH(b, hit) + 12), sz = rd16(IH(b, hit) + 14);
                if (okey) memcpy(okey, IH(b, hit), KEYLEN);
                if (oval) memcpy(oval, b + off, sz);
                if (olen) *olen = sz;
                cb_put(c);
                return 0;
            }
            cb_put(c);
            if (mode == S_CEIL && uv) { memcpy(key, upper, KEYLEN); break; }
            if (mode == S_FLOOR && lv) { memcpy(key, lower, KEYLEN); if (!kdec(key)) return ZENIXFS_ENOENT; break; }
            return ZENIXFS_ENOENT;
        }
    }
}

/* ---------- tree insert ---------- */
struct ins_res { u32 nblk, sblk; u8 skey[KEYLEN]; };

static int t_ins(struct zenixfs *fs, u32 blk, int level, const u8 *key, const u8 *val, u32 len, int upsert, struct ins_res *r)
{
    int e; struct cb *c;
    r->sblk = 0;
    if (level > 0) {
        struct cb *rc; e = tree_get(fs, blk, level, &rc);
        if (e) return e;
        u32 idx = node_slot(rc->data, key), child = rd32(IH(rc->data, idx) + 12);
        cb_put(rc);
        struct ins_res cr;
        e = t_ins(fs, child, level - 1, key, val, len, upsert, &cr);
        if (e) return e;
        e = cow(fs, blk, &c);
        if (e) return e;
        u8 *b = c->data;
        wr32(IH(b, idx) + 12, cr.nblk);
        r->nblk = c->blk;
        if (cr.sblk) {
            u32 n = NITEMS(b), cap = (fs->B - HDR) / 16;
            if (n + 1 <= cap) node_insert(b, idx + 1, cr.skey, cr.sblk);
            else {
                u32 sb = alloc_block(fs, 0, &e);
                if (!sb) { cb_put(c); return e; }
                struct cb *s = cb_new(fs, sb, KIND_TREE);
                node_init(s, sb, level);
                u32 mid = n / 2;
                for (u32 i = mid; i < n; i++) node_insert(s->data, i - mid, IH(b, i), rd32(IH(b, i) + 12));
                SETN(b, mid);
                if (idx + 1 <= mid) node_insert(b, idx + 1, cr.skey, cr.sblk);
                else node_insert(s->data, idx + 1 - mid, cr.skey, cr.sblk);
                r->sblk = sb; memcpy(r->skey, IH(s->data, 0), KEYLEN);
                cb_put(s);
            }
        }
        cb_put(c);
        return 0;
    }
    /* leaf */
    e = tree_get(fs, blk, 0, &c);
    if (e) return e;
    u32 idx = lower_bound(c->data, key);
    int exists = idx < NITEMS(c->data) && !kcmp(IH(c->data, idx), key);
    if (exists && !upsert) { cb_put(c); return ZENIXFS_EEXIST; }
    u32 oldsz = exists ? rd16(IH(c->data, idx) + 14) : 0;
    cb_put(c);
    e = cow(fs, blk, &c);
    if (e) return e;
    u8 *b = c->data;
    r->nblk = c->blk;
    if (exists) leaf_remove(b, idx);
    (void)oldsz;
    if (leaf_free(b) >= 16 + len) { leaf_insert(b, idx, key, val, len); cb_put(c); return 0; }
    /* split */
    u32 sb = alloc_block(fs, 0, &e);
    if (!sb) { cb_put(c); return e; }
    struct cb *s = cb_new(fs, sb, KIND_TREE);
    leaf_init(fs, s, sb);
    u32 n = NITEMS(b), used = fs->B - DATAOFF(b), acc = 0, mid = 0;
    for (; mid < n; mid++) { if (acc >= used / 2) break; acc += rd16(IH(b, mid) + 14); }
    if (mid == 0) mid = 1;
    if (mid >= n) mid = n - 1;
    for (u32 i = mid; i < n; i++) {
        u32 off = rd16(IH(b, i) + 12), sz = rd16(IH(b, i) + 14);
        leaf_insert(s->data, i - mid, IH(b, i), b + off, sz);
    }
    for (u32 i = n; i > mid; i--) leaf_remove(b, i - 1);
    u8 skey[KEYLEN]; memcpy(skey, IH(s->data, 0), KEYLEN);
    u8 *tb = kcmp(key, skey) < 0 ? b : s->data;
    u32 tidx = lower_bound(tb, key);
    if (leaf_free(tb) < 16 + len) { cb_put(s); cb_put(c); return ZENIXFS_EIO; }
    leaf_insert(tb, tidx, key, val, len);
    r->sblk = sb; memcpy(r->skey, skey, KEYLEN);
    cb_put(s); cb_put(c);
    return 0;
}
static int t_insert(struct zenixfs *fs, const u8 *key, const u8 *val, u32 len, int upsert)
{
    struct ins_res r;
    if (len > fs->maxitem) return ZENIXFS_ENAMETOOLONG;
    if (!room(fs, 1)) return ZENIXFS_ENOSPC;
    int e = t_ins(fs, fs->root, fs->height - 1, key, val, len, upsert, &r);
    if (e) return e;
    fs->root = r.nblk;
    if (r.sblk) {
        u32 nr = alloc_block(fs, 0, &e);
        if (!nr) return e;
        struct cb *c = cb_new(fs, nr, KIND_TREE);
        node_init(c, nr, fs->height);
        u8 zero[KEYLEN] = {0};
        node_insert(c->data, 0, zero, r.nblk);
        node_insert(c->data, 1, r.skey, r.sblk);
        cb_put(c);
        fs->root = nr; fs->height++;
    }
    return 0;
}

/* ---------- tree delete ---------- */
struct del_res { u32 nblk; int empty; };

static int t_dl(struct zenixfs *fs, u32 blk, int level, const u8 *key, struct del_res *r, int is_root)
{
    int e; struct cb *c;
    r->empty = 0;
    if (level > 0) {
        struct cb *rc; e = tree_get(fs, blk, level, &rc);
        if (e) return e;
        u32 idx = node_slot(rc->data, key), child = rd32(IH(rc->data, idx) + 12);
        cb_put(rc);
        struct del_res cr;
        e = t_dl(fs, child, level - 1, key, &cr, 0);
        if (e) return e;
        e = cow(fs, blk, &c);
        if (e) return e;
        u8 *b = c->data;
        r->nblk = c->blk;
        if (cr.empty) {
            node_remove(b, idx);
            if (NITEMS(b) == 0) { u32 me = c->blk; cb_put(c); if (!is_root) { free_block(fs, me); r->empty = 1; } else r->nblk = me; return 0; }
        } else wr32(IH(b, idx) + 12, cr.nblk);
        cb_put(c);
        return 0;
    }
    e = tree_get(fs, blk, 0, &c);
    if (e) return e;
    u32 idx = lower_bound(c->data, key);
    if (idx >= NITEMS(c->data) || kcmp(IH(c->data, idx), key)) { cb_put(c); return ZENIXFS_ENOENT; }
    cb_put(c);
    e = cow(fs, blk, &c);
    if (e) return e;
    leaf_remove(c->data, idx);
    r->nblk = c->blk;
    if (NITEMS(c->data) == 0 && !is_root) { u32 me = c->blk; cb_put(c); free_block(fs, me); r->empty = 1; return 0; }
    cb_put(c);
    return 0;
}
static int t_delete(struct zenixfs *fs, const u8 *key)
{
    struct del_res r;
    if (!room(fs, 1)) return ZENIXFS_ENOSPC;
    int e = t_dl(fs, fs->root, fs->height - 1, key, &r, 1);
    if (e) return e;
    fs->root = r.nblk;
    while (fs->height > 1) {
        struct cb *c; e = cb_get(fs, fs->root, KIND_TREE, &c);
        if (e) return e;
        if (NITEMS(c->data) == 1) {
            u32 child = rd32(IH(c->data, 0) + 12), me = fs->root;
            cb_put(c); free_block(fs, me); fs->root = child; fs->height--;
        } else if (NITEMS(c->data) == 0) {
            u32 me = fs->root; cb_put(c); free_block(fs, me);
            u32 nr = alloc_block(fs, 0, &e);
            if (!nr) return e;
            struct cb *l = cb_new(fs, nr, KIND_TREE); leaf_init(fs, l, nr); cb_put(l);
            fs->root = nr; fs->height = 1;
        } else { cb_put(c); break; }
    }
    return 0;
}

/* ---------- commit ---------- */

static int commit(struct zenixfs *fs)
{
    if (!fs->txn_dirty) return 0;
    if (fs->readonly) return ZENIXFS_EROFS;
    if (fs->failed) return ZENIXFS_EIO;
    u32 B = fs->B;
    for (u32 i = 0; i < fs->cap; i++) {
        struct cb *c = &fs->cbs[i];
        if (!c->valid || !c->dirty) continue;
        if (c->kind == KIND_TREE) seal(c->data, B, fs->gen);
        if (dev_write(fs, c->blk, c->data) < 0) { fs->failed = 1; return ZENIXFS_EIO; }
        c->dirty = 0;
    }
    fs->ndirty = 0;
    if (apply_pending(fs)) { fs->failed = 1; return ZENIXFS_EIO; }
    for (u32 k = 0; k < fs->nbmp; k++)                       /* the changed bitmap pages go to their other copy */
        if (fs->bmp[k].valid && fs->bmp[k].dirty && bm_stage(fs, &fs->bmp[k])) return ZENIXFS_EIO;
    u8 *buf = fs->sc_c, *sel = fs->sc_sel;
    memcpy(sel, fs->bmsel, fs->nb);
    for (u32 i = 0; i < fs->nb; i++) if (fs->bmstaged[i]) sel[i] ^= 1;
    if (dev_flush(fs) < 0) { fs->failed = 1; return ZENIXFS_EIO; }
    memset(buf, 0, B);
    wr32(buf + 32, fs->root); wr32(buf + 36, fs->height); wr32(buf + 40, fs->free_blocks);
    wr32(buf + 44, fs->next_ino); wr64(buf + 48, fs->now); wr32(buf + 56, fs->nb);
    for (u32 i = 0; i < fs->nb; i++) if (sel[i]) buf[60 + i / 8] |= 1 << (i % 8);
    u32 slot = 1 + (fs->gen % RING);
    hdr_set(buf, T_COMMIT, 0, slot);
    seal(buf, B, fs->gen);
    if (dev_write(fs, slot, buf) < 0 || dev_flush(fs) < 0) { fs->failed = 1; return ZENIXFS_EIO; }
    memcpy(fs->bmsel, sel, fs->nb);
    memset(fs->bmstaged, 0, fs->nb);
    for (u32 i = 0; i < fs->cap; i++) fs->cbs[i].fresh = 0;
    fs->gen++; fs->txn_dirty = 0; fs->st.commits++;
    return 0;
}
/* called at the start of each operation: commit if the cache is filling with dirty blocks */
/* inside a long write, between two blocks: only when memory or the run list is really about to run out */
static int pressure_blk(struct zenixfs *fs)
{
    if (fs->readonly) return ZENIXFS_EROFS;
    if (fs->failed) return ZENIXFS_EIO;
    if (fs->ndirty * 4 >= fs->cap * 3 || fs->npend + 8 > PENDMAX) return commit(fs);
    return 0;
}
static int pressure(struct zenixfs *fs)
{
    if (fs->readonly) return ZENIXFS_EROFS;
    if (fs->failed) return ZENIXFS_EIO;
    if (fs->ndirty * 2 >= fs->cap) return commit(fs);
    if (fs->npend + 160 > PENDMAX) return commit(fs);                      /* room for the runs of the next operation: a write of up to
                                                                             * about 30 blocks then never needs a commit in its middle */
    if (fs->npend && fs->free_blocks - fs->pend_blocks < 2 * fs->reserve + 16) return commit(fs);   /* freed space is not usable until the commit */
    return 0;
}

/* ---------- mkfs and mount ---------- */
static u32 geometry_nb(u32 nblocks, u32 B) { u32 bpb = 8 * (B - HDR); return (nblocks + bpb - 1) / bpb; }

static void inode_put(u8 *v, const struct zenixfs_inode *in)
{
    wr32(v, in->mode); wr32(v + 4, in->uid); wr32(v + 8, in->gid); wr32(v + 12, in->nlink);
    wr64(v + 16, in->size); wr64(v + 24, in->mtime); wr64(v + 32, in->ctime);
    wr32(v + 40, in->flags); wr32(v + 44, in->parent); wr32(v + 48, in->rdev); wr32(v + 52, 0); wr32(v + 56, 0);
}
static void inode_get(const u8 *v, struct zenixfs_inode *in)
{
    in->mode = rd32(v); in->uid = rd32(v + 4); in->gid = rd32(v + 8); in->nlink = rd32(v + 12);
    in->size = rd64(v + 16); in->mtime = rd64(v + 24); in->ctime = rd64(v + 32);
    in->flags = rd32(v + 40); in->parent = rd32(v + 44); in->rdev = rd32(v + 48);
}

int zenixfs_mkfs(struct zenixfs_dev *dev, const char *label, uint32_t reserve, uint64_t now)
{
    u32 B = 1u << dev->bshift, n = dev->nblocks;
    if (dev->bshift < 10 || dev->bshift > 12) return ZENIXFS_EINVAL;
    u32 nb = geometry_nb(n, B), dstart = 9 + 2 * nb;
    if (nb > 8 * (B - 60) || n < dstart + 128) return ZENIXFS_EINVAL;
    if (!reserve) { reserve = n / 50; if (reserve < 32) reserve = 32; }
    u8 *buf = ZENIXFS_MALLOC(B);
    if (!buf) return ZENIXFS_EIO;
    memset(buf, 0, B);
    for (u32 i = 1; i <= RING; i++) if (dev->write(dev->ctx, i, buf) < 0) goto io;
    /* super */
    hdr_set(buf, T_SUPER, 0, 0);
    wr32(buf + 32, SBMAGIC); wr32(buf + 36, 1); wr32(buf + 40, dev->bshift); wr32(buf + 44, n); wr32(buf + 48, nb);
    wr32(buf + 52, reserve); wr64(buf + 84, now);
    if (label) strncpy((char *)buf + 92, label, 31);
    for (int i = 0; i < 16; i++) buf[68 + i] = (u8)(now * 2654435761u >> (i % 4 * 8)) ^ (u8)(i * 37);
    seal(buf, B, 0);
    if (dev->write(dev->ctx, 0, buf) < 0) goto io;
    /* bitmap pages: the system area and the root leaf (blocks 0 .. dstart) are used, and so is everything past the end */
    {
        u32 bpb = 8 * (B - HDR);
        for (u32 i = 0; i < nb; i++) {
            memset(buf, 0, B);
            u32 base = i * bpb;
            for (u32 j = 0; j < bpb; j++) {
                u32 blk = base + j;
                if (blk <= dstart || blk >= n) bit_set(buf + HDR, j);
            }
            hdr_set(buf, T_BITMAP, 0, BMBLK(i, 0)); seal(buf, B, 1);
            if (dev->write(dev->ctx, BMBLK(i, 0), buf) < 0) goto io;
        }
    }
    /* root leaf with the root directory inode */
    memset(buf, 0, B);
    hdr_set(buf, T_LEAF, 0, dstart); wr16(buf + 24, 0); wr16(buf + 26, B);
    {
        struct zenixfs_inode in = { ZENIXFS_S_IFDIR | 0755, 0, 0, 2, 0, now, now, 0, ZENIXFS_ROOT_INO, 0 };
        u8 key[KEYLEN], val[INODE_LEN];
        mkkey(key, ZENIXFS_ROOT_INO, K_INODE, 0); inode_put(val, &in);
        leaf_insert(buf, 0, key, val, INODE_LEN);
    }
    seal(buf, B, 1);
    if (dev->write(dev->ctx, dstart, buf) < 0) goto io;
    if (dev->flush(dev->ctx) < 0) goto io;
    /* commit record, generation 1 */
    memset(buf, 0, B);
    u32 used = dstart + 1, freeb = n - used;
    wr32(buf + 32, dstart); wr32(buf + 36, 1); wr32(buf + 40, freeb); wr32(buf + 44, ZENIXFS_ROOT_INO + 1);
    wr64(buf + 48, now); wr32(buf + 56, nb);
    hdr_set(buf, T_COMMIT, 0, 1 + (1 % RING)); seal(buf, B, 1);
    if (dev->write(dev->ctx, 1 + (1 % RING), buf) < 0 || dev->flush(dev->ctx) < 0) goto io;
    ZENIXFS_FREE(buf);
    return 0;
io:
    ZENIXFS_FREE(buf);
    return ZENIXFS_EIO;
}

static void fs_free(struct zenixfs *fs)
{
    if (!fs) return;
    if (fs->cbs) for (u32 i = 0; i < fs->cap; i++) ZENIXFS_FREE(fs->cbs[i].data);
    ZENIXFS_FREE(fs->cbs); ZENIXFS_FREE(fs->htab);
    if (fs->bmp) for (u32 i = 0; i < fs->nbmp; i++) ZENIXFS_FREE(fs->bmp[i].data);
    ZENIXFS_FREE(fs->bmp); ZENIXFS_FREE(fs->bmsel); ZENIXFS_FREE(fs->bmstaged); ZENIXFS_FREE(fs->bmfull);
    ZENIXFS_FREE(fs->sc_r); ZENIXFS_FREE(fs->sc_z); ZENIXFS_FREE(fs->sc_c); ZENIXFS_FREE(fs->sc_b);
    ZENIXFS_FREE(fs->sc_sel); ZENIXFS_FREE(fs);
}

static int orphans_free(struct zenixfs *fs);

int zenixfs_mount(struct zenixfs **out, struct zenixfs_dev *dev, uint32_t cache_blocks)
{
    return zenixfs_mount_ex(out, dev, cache_blocks, 0);
}
int zenixfs_mount_ex(struct zenixfs **out, struct zenixfs_dev *dev, uint32_t cache_blocks, uint32_t bm_pages)
{
    struct zenixfs_mount_opts o = { cache_blocks, bm_pages, 0, 0 };
    return zenixfs_mount_opts(out, dev, &o);
}
int zenixfs_mount_opts(struct zenixfs **out, struct zenixfs_dev *dev, const struct zenixfs_mount_opts *mo)
{
    u32 cache_blocks = mo->cache_blocks, bm_pages = mo->bm_pages, skip = mo->skip_commits;
    u32 B = 1u << dev->bshift;
    if (cache_blocks < 32) cache_blocks = 32;
    u8 *buf = ZENIXFS_MALLOC(B);
    if (!buf) return ZENIXFS_EIO;
    if (dev->read(dev->ctx, 0, buf) < 0) { ZENIXFS_FREE(buf); return ZENIXFS_EIO; }
    if (!verify(buf, B, T_SUPER, 0) || rd32(buf + 32) != SBMAGIC || rd32(buf + 36) != 1 || rd32(buf + 40) != dev->bshift ||
        rd32(buf + 64) != 0 || rd32(buf + 60) != 0) { ZENIXFS_FREE(buf); return ZENIXFS_ECORRUPT; }
    struct zenixfs *fs = ZENIXFS_MALLOC(sizeof *fs);
    if (!fs) { ZENIXFS_FREE(buf); return ZENIXFS_EIO; }
    memset(fs, 0, sizeof *fs);
    fs->dev = dev; fs->bshift = dev->bshift; fs->B = B; fs->nblocks = rd32(buf + 44); fs->nb = rd32(buf + 48);
    fs->charset = rd32(buf + 124); fs->reserve = rd32(buf + 52); fs->bpb = 8 * (B - HDR); fs->bmbytes = B - HDR; fs->dstart = 9 + 2 * fs->nb;
    fs->maxitem = (B - HDR) / 4 - 16; if (fs->maxitem > VMAX) fs->maxitem = VMAX;
    fs->maxname = fs->maxitem - 6; if (fs->maxname > ZENIXFS_NAME_MAX) fs->maxname = ZENIXFS_NAME_MAX;
    if (fs->nblocks > dev->nblocks || fs->nb != geometry_nb(fs->nblocks, B)) { ZENIXFS_FREE(buf); fs_free(fs); return ZENIXFS_ECORRUPT; }
    fs->cap = cache_blocks; fs->hsize = 1; while (fs->hsize < cache_blocks * 2) fs->hsize <<= 1;
    fs->cbs = ZENIXFS_MALLOC(sizeof(struct cb) * fs->cap); fs->htab = ZENIXFS_MALLOC(sizeof(struct cb *) * fs->hsize);
    fs->nbmp = bm_pages ? bm_pages : 8;
    if (fs->nbmp > fs->nb) fs->nbmp = fs->nb;
    fs->bmsel = ZENIXFS_MALLOC(fs->nb); fs->bmstaged = ZENIXFS_MALLOC(fs->nb); fs->bmfull = ZENIXFS_MALLOC(fs->nb);
    fs->bmp = ZENIXFS_MALLOC(sizeof(struct bmpage) * fs->nbmp);
    fs->sc_r = ZENIXFS_MALLOC(B); fs->sc_z = ZENIXFS_MALLOC(B); fs->sc_c = ZENIXFS_MALLOC(B); fs->sc_b = ZENIXFS_MALLOC(B);
    fs->sc_sel = ZENIXFS_MALLOC(fs->nb);
    if (!fs->cbs || !fs->htab || !fs->bmsel || !fs->bmstaged || !fs->bmfull || !fs->bmp || !fs->sc_r || !fs->sc_z || !fs->sc_c || !fs->sc_b || !fs->sc_sel) { ZENIXFS_FREE(buf); fs_free(fs); return ZENIXFS_EIO; }
    memset(fs->cbs, 0, sizeof(struct cb) * fs->cap); memset(fs->htab, 0, sizeof(struct cb *) * fs->hsize);
    memset(fs->bmsel, 0, fs->nb); memset(fs->bmstaged, 0, fs->nb); memset(fs->bmfull, 0, fs->nb);
    memset(fs->bmp, 0, sizeof(struct bmpage) * fs->nbmp);
    for (u32 i = 0; i < fs->cap; i++) { fs->cbs[i].data = ZENIXFS_MALLOC(B); if (!fs->cbs[i].data) { ZENIXFS_FREE(buf); fs_free(fs); return ZENIXFS_EIO; } }
    for (u32 i = 0; i < fs->nbmp; i++) { fs->bmp[i].data = ZENIXFS_MALLOC(fs->bmbytes); if (!fs->bmp[i].data) { ZENIXFS_FREE(buf); fs_free(fs); return ZENIXFS_EIO; } }
    /* commit records, newest valid first */
    u64 gens[RING], maxg = 0; u8 *recs = ZENIXFS_MALLOC((size_t)RING * B);
    if (!recs) { ZENIXFS_FREE(buf); fs_free(fs); return ZENIXFS_EIO; }
    for (u32 s = 0; s < RING; s++) {
        gens[s] = 0;
        if (dev->read(dev->ctx, 1 + s, recs + (size_t)s * B) < 0) continue;
        if (verify(recs + (size_t)s * B, B, T_COMMIT, 1 + s)) { gens[s] = rd64(recs + (size_t)s * B + 16); if (gens[s] > maxg) maxg = gens[s]; }
        else {                                              /* never written slots are all zero; anything else that fails is damage
                                                             * (or an interrupted write of the newest commit) */
            const u8 *q = recs + (size_t)s * B;
            u32 nz = 0;
            for (u32 i = 0; i < B; i++) nz |= q[i];
            if (nz) fs->st.mount_damage++;
        }
    }
    int ok = 0;
    for (int tries = 0; tries < RING && !ok; tries++) {
        int bs = -1;
        for (u32 s = 0; s < RING; s++) if (gens[s] && (bs < 0 || gens[s] > gens[bs])) bs = s;
        if (bs < 0) break;
        u8 *r = recs + (size_t)bs * B; u64 g = gens[bs]; gens[bs] = 0;
        if (skip) { skip--; continue; }                           /* recovery: ignore the newest commits on request */
        if (rd32(r + 56) != fs->nb) { fs->st.mount_damage++; continue; }
        int good = 1;
        for (u32 i = 0; i < fs->nb && good; i++) {                /* which copy of each bitmap page is current; checked now on volumes up to 4 MB of map */
            u32 sel = (r[60 + i / 8] >> (i % 8)) & 1, blk = BMBLK(i, sel);
            fs->bmsel[i] = sel;
            if (fs->nb <= 1024 && !(mo->flags & ZENIXFS_MNT_NOBITMAPCHECK) && (dev->read(dev->ctx, blk, buf) < 0 || !verify(buf, B, T_BITMAP, blk))) { good = 0; break; }
        }
        if (!good) { fs->st.mount_damage++; continue; }
        u32 root = rd32(r + 32);
        u32 height = rd32(r + 36);
        if (root < fs->dstart || root >= fs->nblocks || height == 0 || height >= MAXHEIGHT || dev->read(dev->ctx, root, buf) < 0 ||
            !verify_tree(fs, buf, root) || buf[7] != height - 1) { fs->st.mount_damage++; continue; }
        fs->root = root; fs->height = rd32(r + 36); fs->free_blocks = rd32(r + 40); fs->next_ino = rd32(r + 44);
        fs->now = rd64(r + 48); fs->gen = (u32)maxg + 1; (void)g; ok = 1;     /* above every valid record: after a rollback ours is the newest */
    }
    ZENIXFS_FREE(recs); ZENIXFS_FREE(buf);
    if (!ok) { fs_free(fs); return ZENIXFS_ECORRUPT; }
    fs->meta_hint = fs->dstart; fs->data_hint = fs->dstart + fs->nblocks / 32;
    fs->readonly = (mo->flags & ZENIXFS_MNT_READONLY) != 0;
    int e = (mo->flags & (ZENIXFS_MNT_READONLYISH | ZENIXFS_MNT_READONLY)) ? 0 : orphans_free(fs);   /* a recovery mount does not change anything by itself */
    if (e) { fs_free(fs); return e; }
    *out = fs;
    return 0;
}
uint32_t zenixfs_charset(struct zenixfs *fs) { return fs->charset; }
int zenixfs_set_charset(struct zenixfs *fs, uint32_t charset)
{
    u8 *buf = fs->sc_c;
    int e = fs->dev->read(fs->dev->ctx, 0, buf) < 0 ? ZENIXFS_EIO : 0;
    if (!e && !verify(buf, fs->B, T_SUPER, 0)) e = ZENIXFS_ECORRUPT;
    if (!e) { wr32(buf + 124, charset); seal(buf, fs->B, 0); e = fs->dev->write(fs->dev->ctx, 0, buf) < 0 || fs->dev->flush(fs->dev->ctx) < 0 ? ZENIXFS_EIO : 0; }
    if (!e) fs->charset = charset;
    return e;
}
int zenixfs_sync(struct zenixfs *fs) { return commit(fs); }
int zenixfs_unmount(struct zenixfs *fs) { int e = commit(fs); fs_free(fs); return e; }
void zenixfs_set_time(struct zenixfs *fs, uint64_t now) { fs->now = now; }
void zenixfs_get_stats(struct zenixfs *fs, struct zenixfs_stats *st) { *st = fs->st; }
int zenixfs_statfs(struct zenixfs *fs, struct zenixfs_statfs *st)
{
    st->bsize = fs->B; st->blocks = fs->nblocks; st->bfree = fs->free_blocks > fs->reserve ? fs->free_blocks - fs->reserve : 0;
    st->files = fs->next_ino - 1;
    return 0;
}

/* ---------- inodes ---------- */
static int ino_load(struct zenixfs *fs, u32 ino, struct zenixfs_inode *in)
{
    u8 key[KEYLEN], val[VMAX]; int len;
    mkkey(key, ino, K_INODE, 0);
    int e = t_search(fs, key, S_EXACT, NULL, val, &len);
    if (e) return e;
    if (len != INODE_LEN) return ZENIXFS_ECORRUPT;
    inode_get(val, in);
    return 0;
}
static int ino_store(struct zenixfs *fs, u32 ino, const struct zenixfs_inode *in)
{
    u8 key[KEYLEN], val[INODE_LEN];
    mkkey(key, ino, K_INODE, 0); inode_put(val, in);
    return t_insert(fs, key, val, INODE_LEN, 1);
}
int zenixfs_getattr(struct zenixfs *fs, uint32_t ino, struct zenixfs_inode *out) { return ino_load(fs, ino, out); }
int zenixfs_setattr(struct zenixfs *fs, uint32_t ino, const struct zenixfs_inode *in)
{
    struct zenixfs_inode cur; int e = pressure(fs); if (e) return e;
    e = ino_load(fs, ino, &cur); if (e) return e;
    cur.mode = (cur.mode & ZENIXFS_S_IFMT) | (in->mode & ~ZENIXFS_S_IFMT);
    cur.uid = in->uid; cur.gid = in->gid; cur.mtime = in->mtime; cur.ctime = fs->now;
    cur.flags = (cur.flags & 0xFF) | (in->flags & ~0xFFu);
    int saved = fs->in_reserve; fs->in_reserve = 1;               /* chmod works on a full volume */
    if (!room(fs, 1)) e = ZENIXFS_ENOSPC; else e = ino_store(fs, ino, &cur);
    fs->in_reserve = saved;
    return e;
}

/* ---------- directories ---------- */
/* A directory entry is one item per name, keyed by (directory, DIRENT, hash24 << 8 | seq). hash24 is a 24-bit hash of the
 * name; seq counts names with the same hash (0, 1, 2 ... without gaps: removing a name moves the last one of its chain
 * into the hole). So the key offset is a 32-bit position in the directory that stays valid while the directory changes:
 * it is the readdir cookie. Value: ino 4, type 1, namelen 1, name. */
static u32 name_hash(const u8 *n, int len)
{
    u32 h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= n[i]; h *= 16777619u; }
    return (((h >> 8) ^ h) & 0xFFFFFF) << 8;
}
/* the next entry of the hash chain "base" at sequence number q or above (chains may have gaps: a removed name leaves
 * one, nothing is moved, so readdir cookies stay valid); 0 and *at = its sequence number, or ENOENT */
static int dir_next(struct zenixfs *fs, u32 dir, u32 base, u32 q, u32 *at, u8 *val, int *vl)
{
    u8 key[KEYLEN], k2[KEYLEN];
    if (q > 255) return ZENIXFS_ENOENT;
    mkkey(key, dir, K_DIRENT, base | q);
    int e = t_search(fs, key, S_CEIL, k2, val, vl);
    if (e) return e;
    if (rd32(k2) != dir || k2[4] != K_DIRENT || (rd32(k2 + 8) & ~0xFFu) != base) return ZENIXFS_ENOENT;
    *at = rd32(k2 + 8) & 0xFF;
    return 0;
}
/* find the entry; fills ino/type and the sequence number */
static int dir_find_seq(struct zenixfs *fs, u32 dir, const u8 *name, int len, u32 *ino, u8 *type, u32 *seq)
{
    u32 base = name_hash(name, len), q = 0, at;
    for (;;) {
        u8 val[VMAX]; int vl;
        int e = dir_next(fs, dir, base, q, &at, val, &vl);
        if (e) return e;                                   /* ENOENT: the end of the chain */
        if (vl >= 6 && val[5] == len && !memcmp(val + 6, name, len)) {
            if (ino) *ino = rd32(val);
            if (type) *type = val[4];
            if (seq) *seq = at;
            return 0;
        }
        q = at + 1;
    }
}
static int dir_find(struct zenixfs *fs, u32 dir, const u8 *name, int len, u32 *ino, u8 *type)
{
    return dir_find_seq(fs, dir, name, len, ino, type, NULL);
}
static int dir_add(struct zenixfs *fs, u32 dir, const u8 *name, int len, u32 ino, u8 type)
{
    u8 key[KEYLEN], val[300];
    u32 base = name_hash(name, len), q = 0, at;
    for (; q < 256; q = at + 1) {                          /* the first free sequence number: a gap, or the end */
        u8 tmp[VMAX]; int vl;
        int e = dir_next(fs, dir, base, q, &at, tmp, &vl);
        if (e == ZENIXFS_ENOENT || (!e && at > q)) break;
        if (e) return e;
    }
    if (q >= 256) return ZENIXFS_ENOSPC;
    wr32(val, ino); val[4] = type; val[5] = (u8)len; memcpy(val + 6, name, len);
    mkkey(key, dir, K_DIRENT, base | q);
    return t_insert(fs, key, val, 6 + len, 0);
}
static int dir_del(struct zenixfs *fs, u32 dir, const u8 *name, int len)
{
    u32 base = name_hash(name, len), k;
    u8 key[KEYLEN];
    int e = dir_find_seq(fs, dir, name, len, NULL, NULL, &k);
    if (e) return e;
    mkkey(key, dir, K_DIRENT, base | k);                   /* only this name goes: the others keep their places */
    return t_delete(fs, key);
}
static int dir_empty(struct zenixfs *fs, u32 dir)
{
    u8 key[KEYLEN], k2[KEYLEN];
    mkkey(key, dir, K_DIRENT, 0);
    int e = t_search(fs, key, S_CEIL, k2, NULL, NULL);
    if (e == ZENIXFS_ENOENT) return 1;
    if (e) return e;
    return !(rd32(k2) == dir && k2[4] == K_DIRENT);
}
static int check_name(struct zenixfs *fs, const char *name, int len)
{
    if (len <= 0) return ZENIXFS_EINVAL;
    if (len > (int)fs->maxname) return ZENIXFS_ENAMETOOLONG;
    for (int i = 0; i < len; i++) if (name[i] == '/' || name[i] == 0) return ZENIXFS_EINVAL;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return ZENIXFS_EINVAL;
    return 0;
}
int zenixfs_lookup(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t *ino)
{
    struct zenixfs_inode d; int e = ino_load(fs, dir, &d);
    if (e) return e;
    if ((d.mode & ZENIXFS_S_IFMT) != ZENIXFS_S_IFDIR) return ZENIXFS_ENOTDIR;
    if (len == 1 && name[0] == '.') { *ino = dir; return 0; }
    if (len == 2 && name[0] == '.' && name[1] == '.') { *ino = d.parent; return 0; }
    return dir_find(fs, dir, (const u8 *)name, len, ino, NULL);
}
int zenixfs_readdir(struct zenixfs *fs, uint32_t dir, uint64_t *cookie, char *name, uint32_t *ino, uint8_t *type)
{
    struct zenixfs_inode d; int e = ino_load(fs, dir, &d);
    if (e) return e;
    if ((d.mode & ZENIXFS_S_IFMT) != ZENIXFS_S_IFDIR) return ZENIXFS_ENOTDIR;
    u64 c = *cookie;
    if (c == 0) { strcpy(name, "."); *ino = dir; *type = 4; *cookie = 1; return 1; }
    if (c == 1) { strcpy(name, ".."); *ino = d.parent; *type = 4; *cookie = 2; return 1; }
    /* cookie = key offset of the last name returned + 3; always fits in 32 bits */
    u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
    mkkey(key, dir, K_DIRENT, (u32)(c - 2));
    e = t_search(fs, key, S_CEIL, k2, val, &vl);
    if (e == ZENIXFS_ENOENT) return 0;
    if (e) return e;
    if (rd32(k2) != dir || k2[4] != K_DIRENT) return 0;
    if (vl < 6 || vl < 6 + val[5]) return ZENIXFS_ECORRUPT;
    memcpy(name, val + 6, val[5]); name[val[5]] = 0;
    *ino = rd32(val); *type = val[4];
    *cookie = (u64)rd32(k2 + 8) + 3;
    return 1;
}

/* ---------- file data ---------- */
static int ext_floor(struct zenixfs *fs, u32 ino, u32 lb, u32 *s, u32 *p, u32 *n)
{
    u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
    mkkey(key, ino, K_EXTENT, lb);
    int e = t_search(fs, key, S_FLOOR, k2, val, &vl);
    if (e) return e;
    if (rd32(k2) != ino || k2[4] != K_EXTENT) return ZENIXFS_ENOENT;
    *s = rd32(k2 + 8); *p = rd32(val); *n = rd32(val + 4);
    return 0;
}
static int ext_put(struct zenixfs *fs, u32 ino, u32 s, u32 p, u32 n)
{
    u8 key[KEYLEN], val[8];
    mkkey(key, ino, K_EXTENT, s); wr32(val, p); wr32(val + 4, n);
    return t_insert(fs, key, val, 8, 1);
}
static int ext_del(struct zenixfs *fs, u32 ino, u32 s)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_EXTENT, s);
    return t_delete(fs, key);
}
/* physical block of logical block lb, 0 for a hole */
static int f_map_get(struct zenixfs *fs, u32 ino, u32 lb, u32 *phys)
{
    u32 s, p, n;
    int e = ext_floor(fs, ino, lb, &s, &p, &n);
    if (e == ZENIXFS_ENOENT) { *phys = 0; return 0; }
    if (e) return e;
    *phys = (lb < s + n) ? p + (lb - s) : 0;
    return 0;
}
/* map logical block lb to phys, replacing and freeing any old mapping; merges neighbours */
static int f_map_set(struct zenixfs *fs, u32 ino, u32 lb, u32 phys)
{
    u32 s, p, n; int e = ext_floor(fs, ino, lb, &s, &p, &n);
    if (e && e != ZENIXFS_ENOENT) return e;
    if (!e && lb < s + n) {                       /* the block is mapped: split the extent around it */
        u32 old = p + (lb - s);
        if (old == phys) return 0;
        e = ext_del(fs, ino, s); if (e) return e;
        if (lb > s) { e = ext_put(fs, ino, s, p, lb - s); if (e) return e; }
        if (lb + 1 < s + n) { e = ext_put(fs, ino, lb + 1, old + 1, s + n - lb - 1); if (e) return e; }
        free_block(fs, old);
    }
    u32 ls = lb, lp = phys, ln = 1;
    e = ext_floor(fs, ino, lb, &s, &p, &n);
    if (e && e != ZENIXFS_ENOENT) return e;
    if (!e && s + n == lb && p + n == phys) { ls = s; lp = p; ln = n + 1; }
    if (ln > 1) e = ext_put(fs, ino, ls, lp, ln); else e = ext_put(fs, ino, ls, lp, ln);
    if (e) return e;
    u32 rs, rp, rn;
    e = ext_floor(fs, ino, lb + 1, &rs, &rp, &rn);
    if (!e && rs == lb + 1 && rp == lp + ln) {
        e = ext_del(fs, ino, rs); if (e) return e;
        e = ext_put(fs, ino, ls, lp, ln + rn); if (e) return e;
    } else if (e && e != ZENIXFS_ENOENT) return e;
    return 0;
}
/* drop every mapping at logical block >= lb, freeing the blocks */
static int f_map_clear_from(struct zenixfs *fs, u32 ino, u32 lb)
{
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
        if (fs->npend + 8 > PENDMAX) {                   /* a very fragmented file: write out what is freed so far to make room */
            int ce = commit(fs);                          /* (the callers keep the inode listed as an orphan or sized, so this state is consistent) */
            if (ce) return ce;
        }
        mkkey(key, ino, K_EXTENT, 0xFFFFFFFFu);
        int e = t_search(fs, key, S_FLOOR, k2, val, &vl);
        if (e == ZENIXFS_ENOENT) return 0;
        if (e) return e;
        if (rd32(k2) != ino || k2[4] != K_EXTENT) return 0;
        u32 s = rd32(k2 + 8), p = rd32(val), n = rd32(val + 4);
        if (s + n <= lb) return 0;
        if (s >= lb) { e = ext_del(fs, ino, s); if (e) return e; for (u32 i = 0; i < n; i++) free_block(fs, p + i); }
        else {
            u32 keep = lb - s;
            e = ext_put(fs, ino, s, p, keep); if (e) return e;
            for (u32 i = keep; i < n; i++) free_block(fs, p + i);
            return 0;
        }
    }
}
static int data_read_blk(struct zenixfs *fs, u32 phys, u8 *buf)
{
    struct cb *c = cb_find(fs, phys);
    if (c) { memcpy(buf, c->data, fs->B); fs->st.cache_hits++; return 0; }
    fs->st.reads++;
    return fs->dev->read(fs->dev->ctx, phys, buf) < 0 ? ZENIXFS_EIO : 0;
}
static int inline_load(struct zenixfs *fs, u32 ino, u8 *buf, int *len)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_INLINE, 0);
    int e = t_search(fs, key, S_EXACT, NULL, buf, len);
    if (e == ZENIXFS_ENOENT) { *len = 0; return 0; }
    return e;
}
static int inline_store(struct zenixfs *fs, u32 ino, const u8 *buf, int len)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_INLINE, 0);
    if (len == 0) { int e = t_delete(fs, key); return e == ZENIXFS_ENOENT ? 0 : e; }
    return t_insert(fs, key, buf, len, 1);
}

int zenixfs_read(struct zenixfs *fs, uint32_t ino, uint64_t off, void *vbuf, uint32_t len)
{
    struct zenixfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) return ZENIXFS_EISDIR;
    if (off >= in.size) return 0;
    if (len > in.size - off) len = (u32)(in.size - off);
    u8 *buf = vbuf;
    if (in.flags & IFL_INLINE) {
        u8 tmp[VMAX]; int tl; e = inline_load(fs, ino, tmp, &tl);
        if (e) return e;
        for (u32 i = 0; i < len; i++) buf[i] = (off + i < (u64)tl) ? tmp[off + i] : 0;
        return len;
    }
    u8 *blk = fs->sc_r;
    u32 done = 0;
    while (done < len) {
        u64 pos = off + done; u32 lb = (u32)(pos >> fs->bshift), bo = (u32)(pos & (fs->B - 1)), chunk = fs->B - bo;
        if (chunk > len - done) chunk = len - done;
        u32 phys; e = f_map_get(fs, ino, lb, &phys);
        if (e) return e;
        if (!phys) memset(buf + done, 0, chunk);
        else {
            e = data_read_blk(fs, phys, blk);
            if (e) return e;
            memcpy(buf + done, blk + bo, chunk);
        }
        done += chunk;
    }
    return len;
}

/* write whole-block range without touching the inline logic; updates size */
static int f_write_blocks(struct zenixfs *fs, u32 ino, u64 off, const u8 *buf, u32 len)
{
    u32 done = 0; int e;
    while (done < len) {
        e = pressure_blk(fs); if (e) return e;
        if (!room(fs, 4)) return ZENIXFS_ENOSPC;                       /* the data block, the extent change and the inode */
        u64 pos = off + done; u32 lb = (u32)(pos >> fs->bshift), bo = (u32)(pos & (fs->B - 1)), chunk = fs->B - bo;
        if (chunk > len - done) chunk = len - done;
        u32 old; e = f_map_get(fs, ino, lb, &old);
        if (e) return e;
        if (old && isnew(fs, old)) {
            struct cb *c = cb_find(fs, old);
            if (!c) { c = cb_alloc(fs, old); if (!c) return ZENIXFS_EIO; c->kind = KIND_DATA; if (data_read_blk(fs, old, c->data)) { c->valid = 0; cb_unhash(fs, c); return ZENIXFS_EIO; } }
            else c->pin++;
            c->kind = KIND_DATA;
            memcpy(c->data + bo, buf + done, chunk);
            cb_markdirty(fs, c); cb_put(c);
        } else {
            u32 nb = alloc_block(fs, 1, &e);
            if (!nb) return e;
            struct cb *c = cb_new(fs, nb, KIND_DATA);
            if (!c) return ZENIXFS_EIO;
            if (chunk < fs->B && old) { e = data_read_blk(fs, old, c->data); if (e) { cb_put(c); return e; } }
            memcpy(c->data + bo, buf + done, chunk);
            cb_put(c);
            e = f_map_set(fs, ino, lb, nb);
            if (e) { free_block(fs, nb); return e; }
        }
        done += chunk;
        struct zenixfs_inode in; e = ino_load(fs, ino, &in);
        if (e) return e;
        if (off + done > in.size) in.size = off + done;
        in.mtime = in.ctime = fs->now;
        e = ino_store(fs, ino, &in);
        if (e) return e;
    }
    return 0;
}
static int to_extents(struct zenixfs *fs, u32 ino, struct zenixfs_inode *in)
{
    u8 tmp[VMAX]; int tl, e = inline_load(fs, ino, tmp, &tl);
    if (e) return e;
    e = inline_store(fs, ino, NULL, 0); if (e) return e;
    in->flags &= ~IFL_INLINE;
    e = ino_store(fs, ino, in); if (e) return e;
    if (tl > 0) e = f_write_blocks(fs, ino, 0, tmp, tl);
    return e;
}
int zenixfs_write(struct zenixfs *fs, uint32_t ino, uint64_t off, const void *vbuf, uint32_t len)
{
    struct zenixfs_inode in; int e = pressure(fs);
    if (e) return e;
    e = ino_load(fs, ino, &in); if (e) return e;
    if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) return ZENIXFS_EISDIR;
    if (len == 0) return 0;
    if (off + len < off || off + len > 0x3FFFFFFFFFFFull) return ZENIXFS_EFBIG;
    const u8 *buf = vbuf;
    u32 inl = ZENIXFS_INLINE_MAX < fs->maxitem ? ZENIXFS_INLINE_MAX : fs->maxitem;
    if (in.flags & IFL_INLINE) {
        if (off + len <= inl) {
            u8 tmp[VMAX]; int tl; e = inline_load(fs, ino, tmp, &tl); if (e) return e;
            u32 nl = (u32)(off + len) > (u32)tl ? (u32)(off + len) : (u32)tl;
            if (off > (u64)tl) memset(tmp + tl, 0, off - tl);
            memcpy(tmp + off, buf, len);
            e = inline_store(fs, ino, tmp, nl); if (e) return e;
            if (off + len > in.size) in.size = off + len;
            in.mtime = in.ctime = fs->now;
            e = ino_store(fs, ino, &in); if (e) return e;
            return len;
        }
        e = to_extents(fs, ino, &in); if (e) return e;
    }
    e = f_write_blocks(fs, ino, off, buf, len);
    return e ? e : (int)len;
}
int zenixfs_truncate(struct zenixfs *fs, uint32_t ino, uint64_t size)
{
    struct zenixfs_inode in; int e = pressure(fs);
    if (e) return e;
    e = ino_load(fs, ino, &in); if (e) return e;
    if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) return ZENIXFS_EISDIR;
    if (size > 0x3FFFFFFFFFFFull) return ZENIXFS_EFBIG;
    u32 inl = ZENIXFS_INLINE_MAX < fs->maxitem ? ZENIXFS_INLINE_MAX : fs->maxitem;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    if (!room(fs, 6)) { fs->in_reserve = saved; return ZENIXFS_ENOSPC; }
    if (in.flags & IFL_INLINE) {
        if (size <= inl) {
            u8 tmp[VMAX]; int tl; e = inline_load(fs, ino, tmp, &tl);
            if (!e) { if ((u64)tl > size) tl = (int)size; e = inline_store(fs, ino, tmp, tl); }
        } else e = to_extents(fs, ino, &in);
        if (e) goto out;
        if (size <= inl) { in.size = size; in.mtime = in.ctime = fs->now; e = ino_store(fs, ino, &in); goto out; }
        e = ino_load(fs, ino, &in); if (e) goto out;
    }
    if (size < in.size) {
        u32 keep = (u32)((size + fs->B - 1) >> fs->bshift);
        e = f_map_clear_from(fs, ino, keep); if (e) goto out;
        if (size & (fs->B - 1)) {                 /* zero the tail of the last block */
            u32 lb = (u32)(size >> fs->bshift), bo = (u32)(size & (fs->B - 1)), phys;
            e = f_map_get(fs, ino, lb, &phys); if (e) goto out;
            if (phys) { u8 *z = fs->sc_z; memset(z, 0, fs->B - bo);
                e = f_write_blocks(fs, ino, size, z, fs->B - bo); if (e) goto out;
                e = ino_load(fs, ino, &in); if (e) goto out; }
        }
    }
    in.size = size; in.mtime = in.ctime = fs->now;
    e = ino_store(fs, ino, &in);
out:
    fs->in_reserve = saved;
    return e;
}

/* ---------- namespace operations ---------- */
static int new_inode(struct zenixfs *fs, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t parent, uint32_t *ino)
{
    struct zenixfs_inode in = { mode, uid, gid, (mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR ? 2 : 1, 0, fs->now, fs->now, 0, parent, 0 };
    if ((mode & ZENIXFS_S_IFMT) != ZENIXFS_S_IFDIR) in.flags = IFL_INLINE;
    if (fs->next_ino == 0xFFFFFFFFu) return ZENIXFS_ENOSPC;
    *ino = fs->next_ino++;
    fs->txn_dirty = 1;
    return ino_store(fs, *ino, &in);
}
static int dir_check(struct zenixfs *fs, u32 dir, struct zenixfs_inode *d)
{
    int e = ino_load(fs, dir, d);
    if (e) return e;
    return (d->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR ? 0 : ZENIXFS_ENOTDIR;
}
static int touch_dir(struct zenixfs *fs, u32 dir, struct zenixfs_inode *d, int nlink_delta)
{
    d->nlink += nlink_delta; d->mtime = d->ctime = fs->now;
    return ino_store(fs, dir, d);
}
static int make(struct zenixfs *fs, u32 dir, const char *name, int len, u32 mode, u32 uid, u32 gid, u32 *out)
{
    struct zenixfs_inode d; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, name, len))) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if (dir_find(fs, dir, (const u8 *)name, len, NULL, NULL) == 0) return ZENIXFS_EEXIST;
    if (!room(fs, 5)) return ZENIXFS_ENOSPC;
    u32 ino;
    if ((e = new_inode(fs, mode, uid, gid, dir, &ino))) return e;
    u8 type = (mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR ? 4 : (mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK ? 10 : 8;
    if ((e = dir_add(fs, dir, (const u8 *)name, len, ino, type))) {
        u8 k[KEYLEN]; mkkey(k, ino, K_INODE, 0);
        t_delete(fs, k);                                      /* do not leave an inode that no name points to */
        return e;
    }
    if ((e = touch_dir(fs, dir, &d, type == 4 ? 1 : 0))) return e;
    if (out) *out = ino;
    return 0;
}
int zenixfs_create(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino)
{ return make(fs, dir, name, len, ZENIXFS_S_IFREG | (mode & 07777), uid, gid, ino); }
int zenixfs_mkdir(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino)
{ return make(fs, dir, name, len, ZENIXFS_S_IFDIR | (mode & 07777), uid, gid, ino); }
int zenixfs_symlink(struct zenixfs *fs, uint32_t dir, const char *name, int len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino)
{
    u32 i; int e = make(fs, dir, name, len, ZENIXFS_S_IFLNK | 0777, uid, gid, &i);
    if (e) return e;
    int tl = (int)strlen(target);
    if (tl) { e = zenixfs_write(fs, i, 0, target, tl); if (e < 0) return e; }
    if (ino) *ino = i;
    return 0;
}
int zenixfs_readlink(struct zenixfs *fs, uint32_t ino, char *buf, int max)
{
    struct zenixfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if ((in.mode & ZENIXFS_S_IFMT) != ZENIXFS_S_IFLNK) return ZENIXFS_EINVAL;
    return zenixfs_read(fs, ino, 0, buf, in.size < (u64)max ? (u32)in.size : (u32)max);
}
int zenixfs_link(struct zenixfs *fs, uint32_t dir, const char *name, int len, uint32_t ino)
{
    struct zenixfs_inode d, in; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, name, len))) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = ino_load(fs, ino, &in))) return e;
    if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) return ZENIXFS_EISDIR;
    if (dir_find(fs, dir, (const u8 *)name, len, NULL, NULL) == 0) return ZENIXFS_EEXIST;
    if (!room(fs, 4)) return ZENIXFS_ENOSPC;
    if ((e = dir_add(fs, dir, (const u8 *)name, len, ino, (in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK ? 10 : 8))) return e;
    in.nlink++; in.ctime = fs->now;
    if ((e = ino_store(fs, ino, &in))) return e;
    return touch_dir(fs, dir, &d, 0);
}
static int has_orphan(struct zenixfs *fs, u32 ino)
{
    u8 key[KEYLEN]; mkkey(key, 0, K_ORPHAN, ino);
    return t_search(fs, key, S_EXACT, NULL, NULL, NULL) == 0;
}
/* free an inode and its data. The inode is first stored with nlink 0 and listed as an orphan, so that a commit in the middle
 * of freeing a big or fragmented file (or a crash) leaves a consistent volume that the next mount finishes cleaning. */
static int inode_destroy(struct zenixfs *fs, u32 ino, struct zenixfs_inode *in)
{
    u8 key[KEYLEN], okey[KEYLEN]; int e;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    mkkey(okey, 0, K_ORPHAN, ino);
    e = t_insert(fs, okey, okey, 0, 1);
    if (e) goto out;
    in->nlink = 0;
    e = ino_store(fs, ino, in);
    if (e) goto out;
    if (in->flags & IFL_INLINE) { e = inline_store(fs, ino, NULL, 0); if (e) goto out; }
    else { e = f_map_clear_from(fs, ino, 0); if (e) goto out; }
    mkkey(key, ino, K_INODE, 0);
    e = t_delete(fs, key);
    if (e) goto out;
    e = t_delete(fs, okey);
    if (e == ZENIXFS_ENOENT) e = 0;
out:
    fs->in_reserve = saved;
    return e;
}
static int drop_link(struct zenixfs *fs, u32 ino)
{
    struct zenixfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if (in.nlink > 0) in.nlink--;
    in.ctime = fs->now;
    if (in.nlink == 0 && !has_orphan(fs, ino)) { return inode_destroy(fs, ino, &in); }
    return ino_store(fs, ino, &in);
}
int zenixfs_unlink(struct zenixfs *fs, uint32_t dir, const char *name, int len)
{
    struct zenixfs_inode d; u32 ino; u8 type; int e = pressure(fs);
    if (e) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = dir_find(fs, dir, (const u8 *)name, len, &ino, &type))) return e;
    if (type == 4) return ZENIXFS_EISDIR;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    if (!room(fs, 8)) { fs->in_reserve = saved; return ZENIXFS_ENOSPC; }
    e = dir_del(fs, dir, (const u8 *)name, len);
    if (!e) e = drop_link(fs, ino);
    if (!e) e = touch_dir(fs, dir, &d, 0);
    fs->in_reserve = saved;
    return e;
}
int zenixfs_rmdir(struct zenixfs *fs, uint32_t dir, const char *name, int len)
{
    struct zenixfs_inode d, v; u32 ino; u8 type; int e = pressure(fs);
    if (e) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = dir_find(fs, dir, (const u8 *)name, len, &ino, &type))) return e;
    if (type != 4) return ZENIXFS_ENOTDIR;
    if (ino == ZENIXFS_ROOT_INO) return ZENIXFS_EINVAL;
    e = dir_empty(fs, ino); if (e < 0) return e;
    if (!e) return ZENIXFS_ENOTEMPTY;
    if ((e = ino_load(fs, ino, &v))) return e;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    if (!room(fs, 8)) { fs->in_reserve = saved; return ZENIXFS_ENOSPC; }
    e = dir_del(fs, dir, (const u8 *)name, len);
    if (!e) e = inode_destroy(fs, ino, &v);
    if (!e) e = touch_dir(fs, dir, &d, -1);
    fs->in_reserve = saved;
    return e;
}
int zenixfs_rename(struct zenixfs *fs, uint32_t odir, const char *oname, int olen, uint32_t ndir, const char *nname, int nlen)
{
    struct zenixfs_inode od, nd; u32 ino, tino; u8 type, ttype; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, nname, nlen))) return e;
    if ((e = dir_check(fs, odir, &od))) return e;
    if ((e = dir_check(fs, ndir, &nd))) return e;
    if ((e = dir_find(fs, odir, (const u8 *)oname, olen, &ino, &type))) return e;
    if (odir == ndir && olen == nlen && !memcmp(oname, nname, olen)) return 0;
    if (type == 4) {                       /* a directory must not move into itself */
        u32 x = ndir;
        for (int guard = 0; guard < 100000; guard++) {
            if (x == ino) return ZENIXFS_EINVAL;
            if (x == ZENIXFS_ROOT_INO) break;
            struct zenixfs_inode t; if ((e = ino_load(fs, x, &t))) return e;
            x = t.parent;
        }
    }
    int saved = fs->in_reserve; fs->in_reserve = 1;
    if (!room(fs, 12)) { fs->in_reserve = saved; return ZENIXFS_ENOSPC; }
    e = dir_find(fs, ndir, (const u8 *)nname, nlen, &tino, &ttype);
    if (e == 0) {
        if (tino == ino) { fs->in_reserve = saved; return 0; }
        if (type == 4 && ttype != 4) { e = ZENIXFS_ENOTDIR; goto out; }
        if (type != 4 && ttype == 4) { e = ZENIXFS_EISDIR; goto out; }
        if (ttype == 4) {
            int em = dir_empty(fs, tino); if (em < 0) { e = em; goto out; } if (!em) { e = ZENIXFS_ENOTEMPTY; goto out; }
            struct zenixfs_inode tv; if ((e = ino_load(fs, tino, &tv))) goto out;
            if ((e = dir_del(fs, ndir, (const u8 *)nname, nlen))) goto out;
            if ((e = inode_destroy(fs, tino, &tv))) goto out;
            if ((e = ino_load(fs, ndir, &nd))) goto out;
            nd.nlink--; if ((e = ino_store(fs, ndir, &nd))) goto out;
        } else {
            if ((e = dir_del(fs, ndir, (const u8 *)nname, nlen))) goto out;
            if ((e = drop_link(fs, tino))) goto out;
        }
    } else if (e != ZENIXFS_ENOENT) goto out;
    if ((e = dir_del(fs, odir, (const u8 *)oname, olen))) goto out;
    if ((e = dir_add(fs, ndir, (const u8 *)nname, nlen, ino, type))) goto out;
    if (type == 4 && odir != ndir) {
        struct zenixfs_inode mv;
        if ((e = ino_load(fs, ino, &mv))) goto out;
        mv.parent = ndir; mv.ctime = fs->now;
        if ((e = ino_store(fs, ino, &mv))) goto out;
        if ((e = ino_load(fs, odir, &od))) goto out;
        od.nlink--;
        if ((e = touch_dir(fs, odir, &od, 0))) goto out;
        if ((e = ino_load(fs, ndir, &nd))) goto out;
        e = touch_dir(fs, ndir, &nd, 1);
    } else {
        if ((e = ino_load(fs, odir, &od))) goto out;
        if ((e = touch_dir(fs, odir, &od, 0))) goto out;
        if (odir != ndir) {
            if ((e = ino_load(fs, ndir, &nd))) goto out;
            e = touch_dir(fs, ndir, &nd, 0);
        }
    }
out:
    fs->in_reserve = saved;
    return e;
}

/* ---------- orphans ---------- */
int zenixfs_orphan_add(struct zenixfs *fs, uint32_t ino)
{
    u8 key[KEYLEN]; int e = pressure(fs);
    if (e) return e;
    mkkey(key, 0, K_ORPHAN, ino);
    return t_insert(fs, key, key, 0, 1);
}
int zenixfs_orphan_done(struct zenixfs *fs, uint32_t ino)
{
    u8 key[KEYLEN]; struct zenixfs_inode in; int e = pressure(fs);
    if (e) return e;
    mkkey(key, 0, K_ORPHAN, ino);
    e = t_delete(fs, key);
    if (e) return e;
    if (ino_load(fs, ino, &in) == 0 && in.nlink == 0) return inode_destroy(fs, ino, &in);
    return 0;
}
static int orphans_free(struct zenixfs *fs)
{
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN]; mkkey(key, 0, K_ORPHAN, 0);
        int e = t_search(fs, key, S_CEIL, k2, NULL, NULL);
        if (e == ZENIXFS_ENOENT) return 0;
        if (e) return e;
        if (rd32(k2) != 0 || k2[4] != K_ORPHAN) return 0;
        if ((e = zenixfs_orphan_done(fs, rd32(k2 + 8)))) return e;
    }
}

/* ---------- fsck ---------- */
struct fk { u32 ino, mode, nlink, flags, refs, subdirs; u64 size; u32 maxext; int seen_inline; };
struct fctx {
    struct zenixfs *fs; void (*log)(void *, const char *); void *lctx; int problems;
    u8 *used; struct fk *tab; u32 ntab, captab; u8 prev[KEYLEN]; int have_prev;
    u32 tree_blocks; u64 counted;
    u32 prev_obj, prev_hash, prev_seq;
};
/* message with up to two numbers: each '#' or '%' in fmt is replaced by a (first) and b (second) */
static void fprob(struct fctx *c, const char *fmt, u32 a, u32 b)
{
    char out[200]; char *q = out; int which = 0;
    for (const char *s = fmt; *s && q < out + 170; s++) {
        if (*s == '#' || *s == '%') {
            u32 v = which++ ? b : a; char t[12]; int n = 0;
            if (!v) t[n++] = '0';
            while (v) { t[n++] = '0' + v % 10; v /= 10; }
            while (n) *q++ = t[--n];
        } else *q++ = *s;
    }
    *q = 0;
    c->problems++;
    if (c->log) c->log(c->lctx, out);
}
static struct fk *ftab_get(struct fctx *c, u32 ino, int create)
{
    u32 lo = 0, hi = c->ntab;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (c->tab[m].ino < ino) lo = m + 1; else hi = m; }
    if (lo < c->ntab && c->tab[lo].ino == ino) return &c->tab[lo];
    if (!create) return NULL;
    if (c->ntab == c->captab) {
        c->captab = c->captab ? c->captab * 2 : 64;
        struct fk *n = ZENIXFS_MALLOC(sizeof(struct fk) * c->captab);
        if (c->tab) { memcpy(n, c->tab, sizeof(struct fk) * c->ntab); ZENIXFS_FREE(c->tab); }
        c->tab = n;
    }
    memmove(&c->tab[lo + 1], &c->tab[lo], sizeof(struct fk) * (c->ntab - lo));
    memset(&c->tab[lo], 0, sizeof(struct fk)); c->tab[lo].ino = ino; c->ntab++;
    return &c->tab[lo];
}
static void fuse_blk(struct fctx *c, u32 b, const char *what)
{
    if (b >= c->fs->nblocks) { fprob(c, "block # out of range", b, 0); return; }
    if (bit_get(c->used, b)) fprob(c, "block # used twice", b, 0);
    bit_set(c->used, b);
    (void)what;
}
static void fwalk(struct fctx *c, u32 blk, int level)
{
    struct zenixfs *fs = c->fs; struct cb *cb;
    int e = cb_get(fs, blk, KIND_TREE, &cb);
    if (e) { fprob(c, "tree block # unreadable or bad checksum", blk, 0); return; }
    u8 *b = cb->data;
    fuse_blk(c, blk, "tree"); c->tree_blocks++;
    if (b[7] != level || b[6] != (level ? T_NODE : T_LEAF)) { fprob(c, "block # has the wrong level", blk, 0); cb_put(cb); return; }
    u32 n = NITEMS(b);
    if (level > 0) {
        for (u32 i = 0; i < n; i++) {
            if (i > 1 && kcmp(IH(b, i - 1), IH(b, i)) >= 0) fprob(c, "node # keys out of order", blk, 0);
            fwalk(c, rd32(IH(b, i) + 12), level - 1);
        }
        cb_put(cb); return;
    }
    u32 d = DATAOFF(b);
    if (HDR + 16 * n > d || d > fs->B) fprob(c, "leaf # item area overlaps", blk, 0);
    for (u32 i = 0; i < n; i++) {
        u8 *ih = IH(b, i); u32 off = rd16(ih + 12), sz = rd16(ih + 14);
        if (off < d || off + sz > fs->B) { fprob(c, "leaf # item % bad offset", blk, i); continue; }
        if (c->have_prev && kcmp(c->prev, ih) >= 0) fprob(c, "leaf # key order broken at item %", blk, i);
        memcpy(c->prev, ih, KEYLEN); c->have_prev = 1;
        u32 obj = rd32(ih); u8 kind = ih[4]; u32 koff = rd32(ih + 8); const u8 *v = b + off;
        if (kind == K_INODE) {
            if (sz != INODE_LEN) { fprob(c, "inode # has bad size", obj, 0); continue; }
            struct fk *f = ftab_get(c, obj, 1);
            f->mode = rd32(v); f->nlink = rd32(v + 12); f->size = rd64(v + 16); f->flags = rd32(v + 40); f->seen_inline |= 0x100;
        } else if (kind == K_EXTENT) {
            struct fk *f = ftab_get(c, obj, 1);
            u32 p = rd32(v), len = rd32(v + 4);
            if (sz != 8 || !len) { fprob(c, "inode # has a bad extent", obj, 0); continue; }
            for (u32 j = 0; j < len; j++) { if (p + j < c->fs->dstart) fprob(c, "inode # extent inside system area", obj, 0); else fuse_blk(c, p + j, "data"); }
            if (koff + len > f->maxext) f->maxext = koff + len;
        } else if (kind == K_INLINE) {
            ftab_get(c, obj, 1)->seen_inline |= 1;
        } else if (kind == K_DIRENT) {
            u32 ino = rd32(v); u8 type = v[4]; int nl = sz >= 6 ? v[5] : 0;
            if (sz < 6 || nl == 0 || 6 + nl != (int)sz) { fprob(c, "directory # has a bad entry", obj, 0); continue; }
            if ((name_hash(v + 6, nl) & ~0xFFu) != (koff & ~0xFFu)) fprob(c, "directory # entry hash mismatch", obj, 0);
            if (c->prev_hash == (koff & ~0xFFu) && c->prev_obj == obj && (koff & 0xFF) <= c->prev_seq)
                fprob(c, "directory # hash chain out of order", obj, 0);
            c->prev_obj = obj; c->prev_hash = koff & ~0xFFu; c->prev_seq = koff & 0xFF;
            struct fk *t = ftab_get(c, ino, 1); t->refs++;
            if (type == 4) { t->seen_inline |= 0x200; ftab_get(c, obj, 1)->subdirs++; }
        } else if (kind == K_ORPHAN) {
            ftab_get(c, koff, 1)->seen_inline |= 0x400;
        } else fprob(c, "unknown item kind in leaf #", blk, 0);
    }
    cb_put(cb);
}
int zenixfs_fsck(struct zenixfs *fs, void (*log)(void *, const char *), void *lctx)
{
    struct fctx c; memset(&c, 0, sizeof c);
    c.fs = fs; c.log = log; c.lctx = lctx;
    int e = commit(fs); if (e) { fprob(&c, "cannot commit before check", 0, 0); return c.problems; }
    c.used = ZENIXFS_MALLOC(((size_t)fs->nb * fs->bmbytes) + 8);
    memset(c.used, 0, ((size_t)fs->nb * fs->bmbytes) + 8);
    for (u32 b = 0; b < fs->dstart; b++) bit_set(c.used, b);
    fwalk(&c, fs->root, fs->height - 1);
    for (u32 i = 0; i < c.ntab; i++) {
        struct fk *f = &c.tab[i];
        int has_inode = f->seen_inline & 0x100, orphan = f->seen_inline & 0x400;
        if (!has_inode) { if (f->ino != 0) fprob(&c, "inode # referenced or has data but no inode item", f->ino, 0); continue; }
        int isdir = (f->mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR;
        if (isdir && orphan) {
            /* a directory being freed: nothing to check */
        } else if (isdir) {
            u32 want = 2 + f->subdirs;
            if (f->nlink != want) fprob(&c, "directory # has nlink % (counted differently)", f->ino, f->nlink);
            if (f->ino != ZENIXFS_ROOT_INO && f->refs != 1) fprob(&c, "directory # is referenced % times", f->ino, f->refs);
        } else {
            if (f->nlink != f->refs && !(orphan && f->nlink == 0 && f->refs == 0)) fprob(&c, "inode # has nlink % not matching its entries", f->ino, f->nlink);
            if ((f->flags & IFL_INLINE) && f->maxext) fprob(&c, "inode # is inline but has extents", f->ino, 0);
            if (!(f->flags & IFL_INLINE) && (f->seen_inline & 1)) fprob(&c, "inode # has inline data but is not inline", f->ino, 0);
            if (!(f->flags & IFL_INLINE) && f->maxext && ((u64)(f->maxext - 1) << fs->bshift) >= f->size && f->size) fprob(&c, "inode # has data beyond its size", f->ino, 0);
        }
        if (f->nlink == 0 && !orphan && f->ino != ZENIXFS_ROOT_INO) fprob(&c, "inode # has nlink 0 and is not an orphan", f->ino, 0);
    }
    /* bitmap against what the tree uses, page by page */
    u32 freecnt = 0;
    for (u32 i = 0; i < fs->nb; i++) {
        struct bmpage *pg = bm_get(fs, i);
        if (!pg) { fprob(&c, "free-space map page # is unreadable or damaged", i, 0); continue; }
        for (u32 j = 0; j < fs->bpb; j++) {
            u32 b = i * fs->bpb + j;
            if (b >= fs->nblocks) break;
            int u = bit_get(c.used, b), m = bit_get(pg->data, j);
            if (u && !m) fprob(&c, "block # is used but marked free", b, 0);
            if (!u && m) fprob(&c, "block # is marked used but nothing uses it", b, 0);
            if (!m) freecnt++;
        }
    }
    if (freecnt != fs->free_blocks) fprob(&c, "free block count is % but bitmap says #", freecnt, fs->free_blocks);
    ZENIXFS_FREE(c.used); ZENIXFS_FREE(c.tab);
    return c.problems;
}


/* ---------- recovery ---------- */
/* a bitmap page from scratch, without reading it (its stored copy is damaged) */
static struct bmpage *bm_blank(struct zenixfs *fs, u32 i)
{
    struct bmpage *v = NULL;
    for (u32 k = 0; k < fs->nbmp; k++) {
        struct bmpage *p = &fs->bmp[k];
        if (p->valid && p->idx == i) { v = p; break; }
        if (!v || (v->valid && (!p->valid || p->lru < v->lru))) v = p;
    }
    if (v->valid && v->idx != i && v->dirty && bm_stage(fs, v)) return NULL;
    memset(v->data, 0, fs->bmbytes);
    v->idx = i; v->valid = 1; v->dirty = 1; v->lru = ++fs->bmclock;
    return v;
}
int zenixfs_rebuild_bitmap(struct zenixfs *fs)
{
    struct fctx c; memset(&c, 0, sizeof c);
    c.fs = fs;
    c.used = ZENIXFS_MALLOC(((size_t)fs->nb * fs->bmbytes) + 8);
    if (!c.used) return ZENIXFS_EIO;
    memset(c.used, 0, ((size_t)fs->nb * fs->bmbytes) + 8);
    for (u32 b = 0; b < fs->dstart; b++) bit_set(c.used, b);
    fwalk(&c, fs->root, fs->height - 1);                       /* every block the tree reaches */
    int changed = 0; u32 freecnt = 0;
    fs->npend = 0; fs->pend_blocks = 0;                         /* whatever was waiting to be freed is decided by the tree now */
    for (u32 i = 0; i < fs->nb; i++) {
        struct bmpage *pg = bm_get(fs, i);
        int fresh = 0;
        if (!pg) { pg = bm_blank(fs, i); fresh = 1; if (!pg) { ZENIXFS_FREE(c.used); ZENIXFS_FREE(c.tab); return ZENIXFS_EIO; } }
        for (u32 j = 0; j < fs->bpb; j++) {
            u32 b = i * fs->bpb + j;
            int want = b >= fs->nblocks ? 1 : bit_get(c.used, b);
            if (bit_get(pg->data, j) != want) { changed++; if (want) bit_set(pg->data, j); else bit_clr(pg->data, j); pg->dirty = 1; }
            if (!want) freecnt++;
        }
        if (fresh) pg->dirty = 1;
        fs->bmfull[i] = 0;
    }
    fs->free_blocks = freecnt;
    fs->txn_dirty = 1;
    ZENIXFS_FREE(c.used); ZENIXFS_FREE(c.tab);
    int e = commit(fs);
    return e ? e : changed;
}

int zenixfs_rewrite_super(struct zenixfs_dev *dev, const char *label, uint32_t reserve, uint32_t charset, uint64_t now)
{
    u32 B = 1u << dev->bshift, n = dev->nblocks;
    if (dev->bshift < 10 || dev->bshift > 12) return ZENIXFS_EINVAL;
    u32 nb = geometry_nb(n, B);
    if (!reserve) { reserve = n / 50; if (reserve < 32) reserve = 32; }
    u8 *buf = ZENIXFS_MALLOC(B);
    if (!buf) return ZENIXFS_EIO;
    memset(buf, 0, B);
    hdr_set(buf, T_SUPER, 0, 0);
    wr32(buf + 32, SBMAGIC); wr32(buf + 36, 1); wr32(buf + 40, dev->bshift); wr32(buf + 44, n); wr32(buf + 48, nb);
    wr32(buf + 52, reserve); wr64(buf + 84, now);
    if (label) strncpy((char *)buf + 92, label, 31);
    wr32(buf + 124, charset);
    seal(buf, B, 0);
    int e = dev->write(dev->ctx, 0, buf) < 0 || dev->flush(dev->ctx) < 0 ? ZENIXFS_EIO : 0;
    ZENIXFS_FREE(buf);
    return e;
}

/* how many blocks of the file are mapped by extents (the walker compares with size / block size to notice lost extents) */
int zenixfs_mapped_blocks(struct zenixfs *fs, uint32_t ino, uint64_t *mapped)
{
    struct zenixfs_inode in;
    int e = ino_load(fs, ino, &in);
    if (e) return e;
    *mapped = 0;
    if (in.flags & IFL_INLINE) return 1;                       /* stored inside the tree: no extents */
    u32 off = 0;
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
        mkkey(key, ino, K_EXTENT, off);
        e = t_search(fs, key, S_CEIL, k2, val, &vl);
        if (e == ZENIXFS_ENOENT) break;
        if (e) return e;
        if (rd32(k2) != ino || k2[4] != K_EXTENT || vl != 8) break;
        *mapped += rd32(val + 4);
        off = rd32(k2 + 8) + 1;
        if (off == 0) break;
    }
    return 0;
}
