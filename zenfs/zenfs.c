/* SPDX-License-Identifier: GPL-2.0-only */
/* ZenFS library, version 1 of the format (docs/zenfs-format.md).
 * Status: host-tested only. Known shortcuts, to be removed before the Zenix port:
 *  - the whole free-space bitmap (and two marker bitmaps) is held in RAM (the plan says: windowed);
 *  - fsck keeps per-inode tables in RAM;
 *  - an ENOSPC in the middle of a tree change can leak a block or two until fsck (reserve makes it rare). */
#include <string.h>
#include "zenfs.h"

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;

#define HDR 32
#define T_SUPER 1
#define T_COMMIT 2
#define T_BITMAP 3
#define T_LEAF 4
#define T_NODE 5
#define MAGIC16 0x5A42
#define SBMAGIC 0x5A454E46u   /* 'ZENF' */
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

struct cb { u32 blk; u8 *data; u8 valid, dirty, kind, pin; u32 lru; struct cb *next; };

struct zfs {
    struct zfs_dev *dev;
    u32 bshift, B, nblocks, nb, bpb, reserve, dstart, maxitem, maxname;
    u32 charset;
    u32 gen;                     /* the transaction being built */
    u32 root, height, free_blocks, next_ino;
    u64 now;
    u8 *bm, *qbm, *nbm, *bmdirty, *bmsel;
    u32 bmbytes;                 /* bytes of payload per bitmap block (B - HDR) */
    struct cb *cbs; struct cb **htab; u32 cap, hsize, clock, ndirty;
    u32 meta_hint, data_hint;
    int in_reserve, failed, txn_dirty;
    struct zfs_stats st;
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
static int verify_tree(const u8 *b, u32 n, u32 self)
{
    if (rd16(b + 4) != MAGIC16 || (b[6] != T_LEAF && b[6] != T_NODE) || rd32(b + 8) != self) return 0;
    return csum_of(b, n) == rd32(b);
}

/* ---------- bit helpers ---------- */
static inline int bit_get(const u8 *a, u32 i) { return (a[i >> 3] >> (i & 7)) & 1; }
static inline void bit_set(u8 *a, u32 i) { a[i >> 3] |= 1 << (i & 7); }
static inline void bit_clr(u8 *a, u32 i) { a[i >> 3] &= ~(1 << (i & 7)); }

/* ---------- block cache ---------- */
static struct cb *cb_find(struct zfs *fs, u32 blk)
{
    for (struct cb *c = fs->htab[blk & (fs->hsize - 1)]; c; c = c->next)
        if (c->valid && c->blk == blk) return c;
    return NULL;
}
static void cb_unhash(struct zfs *fs, struct cb *c)
{
    struct cb **pp = &fs->htab[c->blk & (fs->hsize - 1)];
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    c->next = NULL;
}
static struct cb *cb_alloc(struct zfs *fs, u32 blk)
{
    struct cb *best = NULL;
    for (u32 i = 0; i < fs->cap; i++) {
        struct cb *c = &fs->cbs[i];
        if (!c->valid) { best = c; break; }
        if (!c->dirty && !c->pin && (!best || c->lru < best->lru)) best = c;
    }
    if (!best) return NULL;
    if (best->valid) cb_unhash(fs, best);
    best->blk = blk; best->valid = 1; best->dirty = 0; best->pin = 1; best->lru = ++fs->clock;
    best->next = fs->htab[blk & (fs->hsize - 1)];
    fs->htab[blk & (fs->hsize - 1)] = best;
    return best;
}
static void cb_markdirty(struct zfs *fs, struct cb *c)
{
    if (!c->dirty) { c->dirty = 1; fs->ndirty++; }
    fs->txn_dirty = 1;
}
static int cb_get(struct zfs *fs, u32 blk, int kind, struct cb **out)
{
    struct cb *c = cb_find(fs, blk);
    if (c) { c->pin++; c->lru = ++fs->clock; fs->st.cache_hits++; *out = c; return 0; }
    fs->st.cache_misses++;
    c = cb_alloc(fs, blk);
    if (!c) return ZFS_EIO;
    c->kind = kind;
    fs->st.reads++;
    if (fs->dev->read(fs->dev->ctx, blk, c->data) < 0) { c->valid = 0; cb_unhash(fs, c); c->pin = 0; return ZFS_EIO; }
    if (kind == KIND_TREE && !verify_tree(c->data, fs->B, blk)) { c->valid = 0; cb_unhash(fs, c); c->pin = 0; return ZFS_ECORRUPT; }
    *out = c;
    return 0;
}
static struct cb *cb_new(struct zfs *fs, u32 blk, int kind)
{
    struct cb *c = cb_find(fs, blk);
    if (c) { c->pin++; }
    else { c = cb_alloc(fs, blk); if (!c) return NULL; }
    c->kind = kind;
    memset(c->data, 0, fs->B);
    cb_markdirty(fs, c);
    return c;
}
static void cb_put(struct cb *c) { if (c->pin) c->pin--; }
static void cb_drop(struct zfs *fs, u32 blk)
{
    struct cb *c = cb_find(fs, blk);
    if (!c) return;
    if (c->dirty) { c->dirty = 0; fs->ndirty--; }
    if (!c->pin) { cb_unhash(fs, c); c->valid = 0; }
}

/* ---------- allocation ---------- */
static int blk_free_now(struct zfs *fs, u32 b) { return !bit_get(fs->bm, b) && !bit_get(fs->qbm, b); }

static u32 alloc_block(struct zfs *fs, int data, int *err)
{
    if (fs->free_blocks <= fs->reserve && !fs->in_reserve) { *err = ZFS_ENOSPC; return 0; }
    u32 start = data ? fs->data_hint : fs->meta_hint;
    if (start < fs->dstart || start >= fs->nblocks) start = fs->dstart;
    u32 b = start;
    for (u32 n = 0; n < fs->nblocks - fs->dstart; n++) {
        if (b >= fs->nblocks) b = fs->dstart;
        if (!(b & 7) && fs->bm[b >> 3] == 0xFF) { b += 8; n += 7; continue; }
        if (blk_free_now(fs, b)) {
            bit_set(fs->bm, b); bit_set(fs->nbm, b);
            fs->bmdirty[b / fs->bpb] = 1;
            fs->free_blocks--; fs->txn_dirty = 1;
            if (data) fs->data_hint = b + 1; else fs->meta_hint = b + 1;
            return b;
        }
        b++;
    }
    *err = ZFS_ENOSPC;
    return 0;
}
static void free_block(struct zfs *fs, u32 b)
{
    cb_drop(fs, b);
    bit_clr(fs->bm, b);
    if (bit_get(fs->nbm, b)) bit_clr(fs->nbm, b); else bit_set(fs->qbm, b);
    fs->bmdirty[b / fs->bpb] = 1;
    fs->free_blocks++; fs->txn_dirty = 1;
}
static inline int isnew(struct zfs *fs, u32 b) { return bit_get(fs->nbm, b); }

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
    for (u32 i = 0; i < n - 1; i++) { u32 o = rd16(IH(b, i) + 12); if (o < off) wr16(IH(b, i) + 12, o + sz); }
    SETDO(b, d + sz);
}
static void leaf_init(struct zfs *fs, struct cb *c, u32 blk)
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
static int cow(struct zfs *fs, u32 blk, struct cb **out)
{
    struct cb *c;
    int e = cb_get(fs, blk, KIND_TREE, &c);
    if (e) return e;
    if (isnew(fs, blk)) { cb_markdirty(fs, c); *out = c; return 0; }
    u32 nbk = alloc_block(fs, 0, &e);
    if (!nbk) { cb_put(c); return e; }
    struct cb *n = cb_new(fs, nbk, KIND_TREE);
    if (!n) { cb_put(c); return ZFS_EIO; }
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
static int t_search(struct zfs *fs, const u8 *key0, int mode, u8 *okey, u8 *oval, int *olen)
{
    u8 key[KEYLEN];
    memcpy(key, key0, KEYLEN);
    for (;;) {
        u32 blk = fs->root;
        u8 lower[KEYLEN], upper[KEYLEN]; int lv = 0, uv = 0;
        for (int level = fs->height - 1; ; level--) {
            struct cb *c; int e = cb_get(fs, blk, KIND_TREE, &c);
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
            if (mode == S_FLOOR && lv) { memcpy(key, lower, KEYLEN); if (!kdec(key)) return ZFS_ENOENT; break; }
            return ZFS_ENOENT;
        }
    }
}

/* ---------- tree insert ---------- */
struct ins_res { u32 nblk, sblk; u8 skey[KEYLEN]; };

static int t_ins(struct zfs *fs, u32 blk, int level, const u8 *key, const u8 *val, u32 len, int upsert, struct ins_res *r)
{
    int e; struct cb *c;
    r->sblk = 0;
    if (level > 0) {
        struct cb *rc; e = cb_get(fs, blk, KIND_TREE, &rc);
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
    e = cb_get(fs, blk, KIND_TREE, &c);
    if (e) return e;
    u32 idx = lower_bound(c->data, key);
    int exists = idx < NITEMS(c->data) && !kcmp(IH(c->data, idx), key);
    if (exists && !upsert) { cb_put(c); return ZFS_EEXIST; }
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
    if (leaf_free(tb) < 16 + len) { cb_put(s); cb_put(c); return ZFS_EIO; }
    leaf_insert(tb, tidx, key, val, len);
    r->sblk = sb; memcpy(r->skey, skey, KEYLEN);
    cb_put(s); cb_put(c);
    return 0;
}
static int t_insert(struct zfs *fs, const u8 *key, const u8 *val, u32 len, int upsert)
{
    struct ins_res r;
    if (len > fs->maxitem) return ZFS_ENAMETOOLONG;
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

static int t_dl(struct zfs *fs, u32 blk, int level, const u8 *key, struct del_res *r, int is_root)
{
    int e; struct cb *c;
    r->empty = 0;
    if (level > 0) {
        struct cb *rc; e = cb_get(fs, blk, KIND_TREE, &rc);
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
    e = cb_get(fs, blk, KIND_TREE, &c);
    if (e) return e;
    u32 idx = lower_bound(c->data, key);
    if (idx >= NITEMS(c->data) || kcmp(IH(c->data, idx), key)) { cb_put(c); return ZFS_ENOENT; }
    cb_put(c);
    e = cow(fs, blk, &c);
    if (e) return e;
    leaf_remove(c->data, idx);
    r->nblk = c->blk;
    if (NITEMS(c->data) == 0 && !is_root) { u32 me = c->blk; cb_put(c); free_block(fs, me); r->empty = 1; return 0; }
    cb_put(c);
    return 0;
}
static int t_delete(struct zfs *fs, const u8 *key)
{
    struct del_res r;
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
static int dev_write(struct zfs *fs, u32 blk, const void *buf) { fs->st.writes++; return fs->dev->write(fs->dev->ctx, blk, buf); }
static int dev_flush(struct zfs *fs) { fs->st.flushes++; return fs->dev->flush(fs->dev->ctx); }
#define BMBLK(i, copy) (9 + 2 * (i) + (copy))

static int commit(struct zfs *fs)
{
    if (!fs->txn_dirty) return 0;
    if (fs->failed) return ZFS_EIO;
    u32 B = fs->B;
    for (u32 i = 0; i < fs->cap; i++) {
        struct cb *c = &fs->cbs[i];
        if (!c->valid || !c->dirty) continue;
        if (c->kind == KIND_TREE) seal(c->data, B, fs->gen);
        if (dev_write(fs, c->blk, c->data) < 0) { fs->failed = 1; return ZFS_EIO; }
        c->dirty = 0;
    }
    fs->ndirty = 0;
    u8 *buf = ZFS_MALLOC(B);
    if (!buf) return ZFS_EIO;
    u8 newsel[8192];   /* bmsel copy: nb <= 8*(B-60) is checked at mkfs; limit here for the stack */
    u8 *sel = fs->nb <= sizeof newsel ? newsel : NULL;
    if (!sel) { ZFS_FREE(buf); return ZFS_EIO; }
    memcpy(sel, fs->bmsel, fs->nb);
    for (u32 i = 0; i < fs->nb; i++) {
        if (!fs->bmdirty[i]) continue;
        memset(buf, 0, B);
        memcpy(buf + HDR, fs->bm + (u64)i * fs->bmbytes, fs->bmbytes);
        u32 nc = sel[i] ^ 1, blk = BMBLK(i, nc);
        hdr_set(buf, T_BITMAP, 0, blk);
        seal(buf, B, fs->gen);
        if (dev_write(fs, blk, buf) < 0) { fs->failed = 1; ZFS_FREE(buf); return ZFS_EIO; }
        sel[i] = nc;
    }
    if (dev_flush(fs) < 0) { fs->failed = 1; ZFS_FREE(buf); return ZFS_EIO; }
    memset(buf, 0, B);
    wr32(buf + 32, fs->root); wr32(buf + 36, fs->height); wr32(buf + 40, fs->free_blocks);
    wr32(buf + 44, fs->next_ino); wr64(buf + 48, fs->now); wr32(buf + 56, fs->nb);
    for (u32 i = 0; i < fs->nb; i++) if (sel[i]) buf[60 + i / 8] |= 1 << (i % 8);
    u32 slot = 1 + (fs->gen % RING);
    hdr_set(buf, T_COMMIT, 0, slot);
    seal(buf, B, fs->gen);
    if (dev_write(fs, slot, buf) < 0 || dev_flush(fs) < 0) { fs->failed = 1; ZFS_FREE(buf); return ZFS_EIO; }
    ZFS_FREE(buf);
    memcpy(fs->bmsel, sel, fs->nb);
    memset(fs->bmdirty, 0, fs->nb);
    memset(fs->qbm, 0, fs->bmbytes * fs->nb);
    memset(fs->nbm, 0, fs->bmbytes * fs->nb);
    fs->gen++; fs->txn_dirty = 0; fs->st.commits++;
    return 0;
}
/* called at the start of each operation: commit if the cache is filling with dirty blocks */
static int pressure(struct zfs *fs)
{
    if (fs->failed) return ZFS_EIO;
    if (fs->ndirty * 2 >= fs->cap) return commit(fs);
    return 0;
}

/* ---------- mkfs and mount ---------- */
static u32 geometry_nb(u32 nblocks, u32 B) { u32 bpb = 8 * (B - HDR); return (nblocks + bpb - 1) / bpb; }

static void inode_put(u8 *v, const struct zfs_inode *in)
{
    wr32(v, in->mode); wr32(v + 4, in->uid); wr32(v + 8, in->gid); wr32(v + 12, in->nlink);
    wr64(v + 16, in->size); wr64(v + 24, in->mtime); wr64(v + 32, in->ctime);
    wr32(v + 40, in->flags); wr32(v + 44, in->parent); wr32(v + 48, in->rdev); wr32(v + 52, 0); wr32(v + 56, 0);
}
static void inode_get(const u8 *v, struct zfs_inode *in)
{
    in->mode = rd32(v); in->uid = rd32(v + 4); in->gid = rd32(v + 8); in->nlink = rd32(v + 12);
    in->size = rd64(v + 16); in->mtime = rd64(v + 24); in->ctime = rd64(v + 32);
    in->flags = rd32(v + 40); in->parent = rd32(v + 44); in->rdev = rd32(v + 48);
}

int zfs_mkfs(struct zfs_dev *dev, const char *label, uint32_t reserve, uint64_t now)
{
    u32 B = 1u << dev->bshift, n = dev->nblocks;
    if (dev->bshift < 10 || dev->bshift > 12) return ZFS_EINVAL;
    u32 nb = geometry_nb(n, B), dstart = 9 + 2 * nb;
    if (nb > 8 * (B - 60) || n < dstart + 64) return ZFS_EINVAL;
    if (!reserve) { reserve = n / 50; if (reserve < 16) reserve = 16; }
    u8 *buf = ZFS_MALLOC(B), *bmp = ZFS_MALLOC((size_t)nb * (B - HDR));
    if (!buf || !bmp) { ZFS_FREE(buf); ZFS_FREE(bmp); return ZFS_EIO; }
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
    /* bitmap: system area + root leaf used, everything past the end used */
    memset(bmp, 0, (size_t)nb * (B - HDR));
    for (u32 b = 0; b <= dstart; b++) bit_set(bmp, b);       /* dstart = root leaf */
    for (u32 b = n; b < nb * 8 * (B - HDR); b++) bit_set(bmp, b);
    for (u32 i = 0; i < nb; i++) {
        memset(buf, 0, B);
        memcpy(buf + HDR, bmp + (size_t)i * (B - HDR), B - HDR);
        hdr_set(buf, T_BITMAP, 0, BMBLK(i, 0)); seal(buf, B, 1);
        if (dev->write(dev->ctx, BMBLK(i, 0), buf) < 0) goto io;
    }
    /* root leaf with the root directory inode */
    memset(buf, 0, B);
    hdr_set(buf, T_LEAF, 0, dstart); wr16(buf + 24, 0); wr16(buf + 26, B);
    {
        struct zfs_inode in = { ZFS_S_IFDIR | 0755, 0, 0, 2, 0, now, now, 0, ZFS_ROOT_INO, 0 };
        u8 key[KEYLEN], val[INODE_LEN];
        mkkey(key, ZFS_ROOT_INO, K_INODE, 0); inode_put(val, &in);
        leaf_insert(buf, 0, key, val, INODE_LEN);
    }
    seal(buf, B, 1);
    if (dev->write(dev->ctx, dstart, buf) < 0) goto io;
    if (dev->flush(dev->ctx) < 0) goto io;
    /* commit record, generation 1 */
    memset(buf, 0, B);
    u32 used = dstart + 1, freeb = n - used;
    wr32(buf + 32, dstart); wr32(buf + 36, 1); wr32(buf + 40, freeb); wr32(buf + 44, ZFS_ROOT_INO + 1);
    wr64(buf + 48, now); wr32(buf + 56, nb);
    hdr_set(buf, T_COMMIT, 0, 1 + (1 % RING)); seal(buf, B, 1);
    if (dev->write(dev->ctx, 1 + (1 % RING), buf) < 0 || dev->flush(dev->ctx) < 0) goto io;
    ZFS_FREE(buf); ZFS_FREE(bmp);
    return 0;
io:
    ZFS_FREE(buf); ZFS_FREE(bmp);
    return ZFS_EIO;
}

static void fs_free(struct zfs *fs)
{
    if (!fs) return;
    if (fs->cbs) for (u32 i = 0; i < fs->cap; i++) ZFS_FREE(fs->cbs[i].data);
    ZFS_FREE(fs->cbs); ZFS_FREE(fs->htab); ZFS_FREE(fs->bm); ZFS_FREE(fs->qbm); ZFS_FREE(fs->nbm);
    ZFS_FREE(fs->bmdirty); ZFS_FREE(fs->bmsel); ZFS_FREE(fs);
}

static int orphans_free(struct zfs *fs);

int zfs_mount(struct zfs **out, struct zfs_dev *dev, uint32_t cache_blocks)
{
    u32 B = 1u << dev->bshift;
    if (cache_blocks < 32) cache_blocks = 32;
    u8 *buf = ZFS_MALLOC(B);
    if (!buf) return ZFS_EIO;
    if (dev->read(dev->ctx, 0, buf) < 0) { ZFS_FREE(buf); return ZFS_EIO; }
    if (!verify(buf, B, T_SUPER, 0) || rd32(buf + 32) != SBMAGIC || rd32(buf + 36) != 1 || rd32(buf + 40) != dev->bshift ||
        rd32(buf + 64) != 0 || rd32(buf + 60) != 0) { ZFS_FREE(buf); return ZFS_ECORRUPT; }
    struct zfs *fs = ZFS_MALLOC(sizeof *fs);
    if (!fs) { ZFS_FREE(buf); return ZFS_EIO; }
    memset(fs, 0, sizeof *fs);
    fs->dev = dev; fs->bshift = dev->bshift; fs->B = B; fs->nblocks = rd32(buf + 44); fs->nb = rd32(buf + 48);
    fs->charset = rd32(buf + 124); fs->reserve = rd32(buf + 52); fs->bpb = 8 * (B - HDR); fs->bmbytes = B - HDR; fs->dstart = 9 + 2 * fs->nb;
    fs->maxitem = (B - HDR) / 4 - 16; if (fs->maxitem > VMAX) fs->maxitem = VMAX;
    fs->maxname = fs->maxitem - 6; if (fs->maxname > ZFS_NAME_MAX) fs->maxname = ZFS_NAME_MAX;
    if (fs->nblocks > dev->nblocks || fs->nb != geometry_nb(fs->nblocks, B)) { ZFS_FREE(buf); fs_free(fs); return ZFS_ECORRUPT; }
    fs->cap = cache_blocks; fs->hsize = 1; while (fs->hsize < cache_blocks * 2) fs->hsize <<= 1;
    fs->cbs = ZFS_MALLOC(sizeof(struct cb) * fs->cap); fs->htab = ZFS_MALLOC(sizeof(struct cb *) * fs->hsize);
    size_t bmsz = (size_t)fs->nb * fs->bmbytes;
    fs->bm = ZFS_MALLOC(bmsz); fs->qbm = ZFS_MALLOC(bmsz); fs->nbm = ZFS_MALLOC(bmsz);
    fs->bmdirty = ZFS_MALLOC(fs->nb); fs->bmsel = ZFS_MALLOC(fs->nb);
    if (!fs->cbs || !fs->htab || !fs->bm || !fs->qbm || !fs->nbm || !fs->bmdirty || !fs->bmsel) { ZFS_FREE(buf); fs_free(fs); return ZFS_EIO; }
    memset(fs->cbs, 0, sizeof(struct cb) * fs->cap); memset(fs->htab, 0, sizeof(struct cb *) * fs->hsize);
    for (u32 i = 0; i < fs->cap; i++) { fs->cbs[i].data = ZFS_MALLOC(B); if (!fs->cbs[i].data) { ZFS_FREE(buf); fs_free(fs); return ZFS_EIO; } }
    memset(fs->qbm, 0, bmsz); memset(fs->nbm, 0, bmsz); memset(fs->bmdirty, 0, fs->nb);
    /* commit records, newest valid first */
    u64 gens[RING]; u8 *recs = ZFS_MALLOC((size_t)RING * B);
    if (!recs) { ZFS_FREE(buf); fs_free(fs); return ZFS_EIO; }
    for (u32 s = 0; s < RING; s++) {
        gens[s] = 0;
        if (dev->read(dev->ctx, 1 + s, recs + (size_t)s * B) < 0) continue;
        if (verify(recs + (size_t)s * B, B, T_COMMIT, 1 + s)) gens[s] = rd64(recs + (size_t)s * B + 16);
        else if (rd16(recs + (size_t)s * B + 4) == MAGIC16) fs->st.mount_damage++;   /* written once, now damaged */
    }
    int ok = 0;
    for (int tries = 0; tries < RING && !ok; tries++) {
        int bs = -1;
        for (u32 s = 0; s < RING; s++) if (gens[s] && (bs < 0 || gens[s] > gens[bs])) bs = s;
        if (bs < 0) break;
        u8 *r = recs + (size_t)bs * B; u64 g = gens[bs]; gens[bs] = 0;
        if (rd32(r + 56) != fs->nb) { fs->st.mount_damage++; continue; }
        int good = 1;
        for (u32 i = 0; i < fs->nb && good; i++) {
            u32 sel = (r[60 + i / 8] >> (i % 8)) & 1, blk = BMBLK(i, sel);
            if (dev->read(dev->ctx, blk, buf) < 0 || !verify(buf, B, T_BITMAP, blk)) { good = 0; break; }
            memcpy(fs->bm + (size_t)i * fs->bmbytes, buf + HDR, fs->bmbytes);
            fs->bmsel[i] = sel;
        }
        if (!good) { fs->st.mount_damage++; continue; }
        u32 root = rd32(r + 32);
        if (root >= fs->nblocks || dev->read(dev->ctx, root, buf) < 0 || !verify_tree(buf, B, root)) { fs->st.mount_damage++; continue; }
        fs->root = root; fs->height = rd32(r + 36); fs->free_blocks = rd32(r + 40); fs->next_ino = rd32(r + 44);
        fs->now = rd64(r + 48); fs->gen = (u32)g + 1; ok = 1;
    }
    ZFS_FREE(recs); ZFS_FREE(buf);
    if (!ok) { fs_free(fs); return ZFS_ECORRUPT; }
    fs->meta_hint = fs->dstart; fs->data_hint = fs->dstart + fs->nblocks / 32;
    int e = orphans_free(fs);
    if (e) { fs_free(fs); return e; }
    *out = fs;
    return 0;
}
uint32_t zfs_charset(struct zfs *fs) { return fs->charset; }
int zfs_set_charset(struct zfs *fs, uint32_t charset)
{
    u8 *buf = ZFS_MALLOC(fs->B);
    if (!buf) return ZFS_EIO;
    int e = fs->dev->read(fs->dev->ctx, 0, buf) < 0 ? ZFS_EIO : 0;
    if (!e && !verify(buf, fs->B, T_SUPER, 0)) e = ZFS_ECORRUPT;
    if (!e) { wr32(buf + 124, charset); seal(buf, fs->B, 0); e = fs->dev->write(fs->dev->ctx, 0, buf) < 0 || fs->dev->flush(fs->dev->ctx) < 0 ? ZFS_EIO : 0; }
    if (!e) fs->charset = charset;
    ZFS_FREE(buf);
    return e;
}
int zfs_sync(struct zfs *fs) { return commit(fs); }
int zfs_unmount(struct zfs *fs) { int e = commit(fs); fs_free(fs); return e; }
void zfs_set_time(struct zfs *fs, uint64_t now) { fs->now = now; }
void zfs_get_stats(struct zfs *fs, struct zfs_stats *st) { *st = fs->st; }
int zfs_statfs(struct zfs *fs, struct zfs_statfs *st)
{
    st->bsize = fs->B; st->blocks = fs->nblocks; st->bfree = fs->free_blocks > fs->reserve ? fs->free_blocks - fs->reserve : 0;
    st->files = fs->next_ino - 1;
    return 0;
}

/* ---------- inodes ---------- */
static int ino_load(struct zfs *fs, u32 ino, struct zfs_inode *in)
{
    u8 key[KEYLEN], val[VMAX]; int len;
    mkkey(key, ino, K_INODE, 0);
    int e = t_search(fs, key, S_EXACT, NULL, val, &len);
    if (e) return e;
    if (len != INODE_LEN) return ZFS_ECORRUPT;
    inode_get(val, in);
    return 0;
}
static int ino_store(struct zfs *fs, u32 ino, const struct zfs_inode *in)
{
    u8 key[KEYLEN], val[INODE_LEN];
    mkkey(key, ino, K_INODE, 0); inode_put(val, in);
    return t_insert(fs, key, val, INODE_LEN, 1);
}
int zfs_getattr(struct zfs *fs, uint32_t ino, struct zfs_inode *out) { return ino_load(fs, ino, out); }
int zfs_setattr(struct zfs *fs, uint32_t ino, const struct zfs_inode *in)
{
    struct zfs_inode cur; int e = pressure(fs); if (e) return e;
    e = ino_load(fs, ino, &cur); if (e) return e;
    cur.mode = (cur.mode & ZFS_S_IFMT) | (in->mode & ~ZFS_S_IFMT);
    cur.uid = in->uid; cur.gid = in->gid; cur.mtime = in->mtime; cur.ctime = fs->now;
    cur.flags = (cur.flags & 0xFF) | (in->flags & ~0xFFu);
    return ino_store(fs, ino, &cur);
}

/* ---------- directories ---------- */
static u32 name_hash(const u8 *n, int len)
{
    u32 h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= n[i]; h *= 16777619u; }
    return h;
}
/* find the entry; fills ino/type. *pos = byte offset of the entry in the value list when found */
static int dir_find(struct zfs *fs, u32 dir, const u8 *name, int len, u32 *ino, u8 *type)
{
    u8 key[KEYLEN], val[VMAX]; int vl;
    mkkey(key, dir, K_DIRENT, name_hash(name, len));
    int e = t_search(fs, key, S_EXACT, NULL, val, &vl);
    if (e) return e;
    for (int p = 0; p + 6 <= vl; ) {
        int nl = val[p + 5];
        if (nl == len && !memcmp(val + p + 6, name, len)) { if (ino) *ino = rd32(val + p); if (type) *type = val[p + 4]; return 0; }
        p += 6 + nl;
    }
    return ZFS_ENOENT;
}
static int dir_add(struct zfs *fs, u32 dir, const u8 *name, int len, u32 ino, u8 type)
{
    u8 key[KEYLEN], val[VMAX + 300]; int vl = 0;
    mkkey(key, dir, K_DIRENT, name_hash(name, len));
    int e = t_search(fs, key, S_EXACT, NULL, val, &vl);
    if (e == ZFS_ENOENT) vl = 0; else if (e) return e;
    wr32(val + vl, ino); val[vl + 4] = type; val[vl + 5] = (u8)len; memcpy(val + vl + 6, name, len);
    vl += 6 + len;
    return t_insert(fs, key, val, vl, 1);
}
static int dir_del(struct zfs *fs, u32 dir, const u8 *name, int len)
{
    u8 key[KEYLEN], val[VMAX]; int vl;
    mkkey(key, dir, K_DIRENT, name_hash(name, len));
    int e = t_search(fs, key, S_EXACT, NULL, val, &vl);
    if (e) return e;
    for (int p = 0; p + 6 <= vl; ) {
        int nl = val[p + 5];
        if (nl == len && !memcmp(val + p + 6, name, len)) {
            memmove(val + p, val + p + 6 + nl, vl - p - 6 - nl); vl -= 6 + nl;
            return vl ? t_insert(fs, key, val, vl, 1) : t_delete(fs, key);
        }
        p += 6 + nl;
    }
    return ZFS_ENOENT;
}
static int dir_empty(struct zfs *fs, u32 dir)
{
    u8 key[KEYLEN], k2[KEYLEN];
    mkkey(key, dir, K_DIRENT, 0);
    int e = t_search(fs, key, S_CEIL, k2, NULL, NULL);
    if (e == ZFS_ENOENT) return 1;
    if (e) return e;
    return !(rd32(k2) == dir && k2[4] == K_DIRENT);
}
static int check_name(struct zfs *fs, const char *name, int len)
{
    if (len <= 0) return ZFS_EINVAL;
    if (len > (int)fs->maxname) return ZFS_ENAMETOOLONG;
    for (int i = 0; i < len; i++) if (name[i] == '/' || name[i] == 0) return ZFS_EINVAL;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.')) return ZFS_EINVAL;
    return 0;
}
int zfs_lookup(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t *ino)
{
    struct zfs_inode d; int e = ino_load(fs, dir, &d);
    if (e) return e;
    if ((d.mode & ZFS_S_IFMT) != ZFS_S_IFDIR) return ZFS_ENOTDIR;
    if (len == 1 && name[0] == '.') { *ino = dir; return 0; }
    if (len == 2 && name[0] == '.' && name[1] == '.') { *ino = d.parent; return 0; }
    return dir_find(fs, dir, (const u8 *)name, len, ino, NULL);
}
int zfs_readdir(struct zfs *fs, uint32_t dir, uint64_t *cookie, char *name, uint32_t *ino, uint8_t *type)
{
    struct zfs_inode d; int e = ino_load(fs, dir, &d);
    if (e) return e;
    if ((d.mode & ZFS_S_IFMT) != ZFS_S_IFDIR) return ZFS_ENOTDIR;
    u64 c = *cookie;
    if (c == 0) { strcpy(name, "."); *ino = dir; *type = 4; *cookie = 1; return 1; }
    if (c == 1) { strcpy(name, ".."); *ino = d.parent; *type = 4; *cookie = 2; return 1; }
    if (c < 3) c = 3;
    u32 hash = (u32)((c - 3) >> 8), idx = (u32)((c - 3) & 255);
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
        mkkey(key, dir, K_DIRENT, hash);
        e = t_search(fs, key, S_CEIL, k2, val, &vl);
        if (e == ZFS_ENOENT) return 0;
        if (e) return e;
        if (rd32(k2) != dir || k2[4] != K_DIRENT) return 0;
        u32 h = rd32(k2 + 8);
        if (h != hash) idx = 0;
        u32 i = 0;
        for (int p = 0; p + 6 <= vl; i++) {
            int nl = val[p + 5];
            if (i >= idx) {
                memcpy(name, val + p + 6, nl); name[nl] = 0;
                *ino = rd32(val + p); *type = val[p + 4];
                *cookie = ((u64)h << 8 | (i + 1)) + 3;
                if (i + 1 >= 255) *cookie = ((u64)(h + 1) << 8) + 3;
                return 1;
            }
            p += 6 + nl;
        }
        if (h == 0xFFFFFFFFu) return 0;
        hash = h + 1; idx = 0;
    }
}

/* ---------- file data ---------- */
static int ext_floor(struct zfs *fs, u32 ino, u32 lb, u32 *s, u32 *p, u32 *n)
{
    u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
    mkkey(key, ino, K_EXTENT, lb);
    int e = t_search(fs, key, S_FLOOR, k2, val, &vl);
    if (e) return e;
    if (rd32(k2) != ino || k2[4] != K_EXTENT) return ZFS_ENOENT;
    *s = rd32(k2 + 8); *p = rd32(val); *n = rd32(val + 4);
    return 0;
}
static int ext_put(struct zfs *fs, u32 ino, u32 s, u32 p, u32 n)
{
    u8 key[KEYLEN], val[8];
    mkkey(key, ino, K_EXTENT, s); wr32(val, p); wr32(val + 4, n);
    return t_insert(fs, key, val, 8, 1);
}
static int ext_del(struct zfs *fs, u32 ino, u32 s)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_EXTENT, s);
    return t_delete(fs, key);
}
/* physical block of logical block lb, 0 for a hole */
static int f_map_get(struct zfs *fs, u32 ino, u32 lb, u32 *phys)
{
    u32 s, p, n;
    int e = ext_floor(fs, ino, lb, &s, &p, &n);
    if (e == ZFS_ENOENT) { *phys = 0; return 0; }
    if (e) return e;
    *phys = (lb < s + n) ? p + (lb - s) : 0;
    return 0;
}
/* map logical block lb to phys, replacing and freeing any old mapping; merges neighbours */
static int f_map_set(struct zfs *fs, u32 ino, u32 lb, u32 phys)
{
    u32 s, p, n; int e = ext_floor(fs, ino, lb, &s, &p, &n);
    if (e && e != ZFS_ENOENT) return e;
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
    if (e && e != ZFS_ENOENT) return e;
    if (!e && s + n == lb && p + n == phys) { ls = s; lp = p; ln = n + 1; }
    if (ln > 1) e = ext_put(fs, ino, ls, lp, ln); else e = ext_put(fs, ino, ls, lp, ln);
    if (e) return e;
    u32 rs, rp, rn;
    e = ext_floor(fs, ino, lb + 1, &rs, &rp, &rn);
    if (!e && rs == lb + 1 && rp == lp + ln) {
        e = ext_del(fs, ino, rs); if (e) return e;
        e = ext_put(fs, ino, ls, lp, ln + rn); if (e) return e;
    } else if (e && e != ZFS_ENOENT) return e;
    return 0;
}
/* drop every mapping at logical block >= lb, freeing the blocks */
static int f_map_clear_from(struct zfs *fs, u32 ino, u32 lb)
{
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN], val[VMAX]; int vl;
        mkkey(key, ino, K_EXTENT, 0xFFFFFFFFu);
        int e = t_search(fs, key, S_FLOOR, k2, val, &vl);
        if (e == ZFS_ENOENT) return 0;
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
static int data_read_blk(struct zfs *fs, u32 phys, u8 *buf)
{
    struct cb *c = cb_find(fs, phys);
    if (c) { memcpy(buf, c->data, fs->B); fs->st.cache_hits++; return 0; }
    fs->st.reads++;
    return fs->dev->read(fs->dev->ctx, phys, buf) < 0 ? ZFS_EIO : 0;
}
static int inline_load(struct zfs *fs, u32 ino, u8 *buf, int *len)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_INLINE, 0);
    int e = t_search(fs, key, S_EXACT, NULL, buf, len);
    if (e == ZFS_ENOENT) { *len = 0; return 0; }
    return e;
}
static int inline_store(struct zfs *fs, u32 ino, const u8 *buf, int len)
{
    u8 key[KEYLEN]; mkkey(key, ino, K_INLINE, 0);
    if (len == 0) { int e = t_delete(fs, key); return e == ZFS_ENOENT ? 0 : e; }
    return t_insert(fs, key, buf, len, 1);
}

int zfs_read(struct zfs *fs, uint32_t ino, uint64_t off, void *vbuf, uint32_t len)
{
    struct zfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if ((in.mode & ZFS_S_IFMT) == ZFS_S_IFDIR) return ZFS_EISDIR;
    if (off >= in.size) return 0;
    if (len > in.size - off) len = (u32)(in.size - off);
    u8 *buf = vbuf;
    if (in.flags & IFL_INLINE) {
        u8 tmp[VMAX]; int tl; e = inline_load(fs, ino, tmp, &tl);
        if (e) return e;
        for (u32 i = 0; i < len; i++) buf[i] = (off + i < (u64)tl) ? tmp[off + i] : 0;
        return len;
    }
    u8 *blk = ZFS_MALLOC(fs->B);
    if (!blk) return ZFS_EIO;
    u32 done = 0;
    while (done < len) {
        u64 pos = off + done; u32 lb = (u32)(pos >> fs->bshift), bo = (u32)(pos & (fs->B - 1)), chunk = fs->B - bo;
        if (chunk > len - done) chunk = len - done;
        u32 phys; e = f_map_get(fs, ino, lb, &phys);
        if (e) { ZFS_FREE(blk); return e; }
        if (!phys) memset(buf + done, 0, chunk);
        else {
            e = data_read_blk(fs, phys, blk);
            if (e) { ZFS_FREE(blk); return e; }
            memcpy(buf + done, blk + bo, chunk);
        }
        done += chunk;
    }
    ZFS_FREE(blk);
    return len;
}

/* write whole-block range without touching the inline logic; updates size */
static int f_write_blocks(struct zfs *fs, u32 ino, u64 off, const u8 *buf, u32 len)
{
    u32 done = 0; int e;
    while (done < len) {
        e = pressure(fs); if (e) return e;
        u64 pos = off + done; u32 lb = (u32)(pos >> fs->bshift), bo = (u32)(pos & (fs->B - 1)), chunk = fs->B - bo;
        if (chunk > len - done) chunk = len - done;
        u32 old; e = f_map_get(fs, ino, lb, &old);
        if (e) return e;
        if (old && isnew(fs, old)) {
            struct cb *c = cb_find(fs, old);
            if (!c) { c = cb_alloc(fs, old); if (!c) return ZFS_EIO; c->kind = KIND_DATA; if (data_read_blk(fs, old, c->data)) { c->valid = 0; cb_unhash(fs, c); return ZFS_EIO; } }
            else c->pin++;
            c->kind = KIND_DATA;
            memcpy(c->data + bo, buf + done, chunk);
            cb_markdirty(fs, c); cb_put(c);
        } else {
            u32 nb = alloc_block(fs, 1, &e);
            if (!nb) return e;
            struct cb *c = cb_new(fs, nb, KIND_DATA);
            if (!c) return ZFS_EIO;
            if (chunk < fs->B && old) { e = data_read_blk(fs, old, c->data); if (e) { cb_put(c); return e; } }
            memcpy(c->data + bo, buf + done, chunk);
            cb_put(c);
            e = f_map_set(fs, ino, lb, nb);
            if (e) return e;
        }
        done += chunk;
        struct zfs_inode in; e = ino_load(fs, ino, &in);
        if (e) return e;
        if (off + done > in.size) in.size = off + done;
        in.mtime = in.ctime = fs->now;
        e = ino_store(fs, ino, &in);
        if (e) return e;
    }
    return 0;
}
static int to_extents(struct zfs *fs, u32 ino, struct zfs_inode *in)
{
    u8 tmp[VMAX]; int tl, e = inline_load(fs, ino, tmp, &tl);
    if (e) return e;
    e = inline_store(fs, ino, NULL, 0); if (e) return e;
    in->flags &= ~IFL_INLINE;
    e = ino_store(fs, ino, in); if (e) return e;
    if (tl > 0) e = f_write_blocks(fs, ino, 0, tmp, tl);
    return e;
}
int zfs_write(struct zfs *fs, uint32_t ino, uint64_t off, const void *vbuf, uint32_t len)
{
    struct zfs_inode in; int e = pressure(fs);
    if (e) return e;
    e = ino_load(fs, ino, &in); if (e) return e;
    if ((in.mode & ZFS_S_IFMT) == ZFS_S_IFDIR) return ZFS_EISDIR;
    if (len == 0) return 0;
    if (off + len < off || off + len > 0x3FFFFFFFFFFFull) return ZFS_EFBIG;
    const u8 *buf = vbuf;
    u32 inl = ZFS_INLINE_MAX < fs->maxitem ? ZFS_INLINE_MAX : fs->maxitem;
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
int zfs_truncate(struct zfs *fs, uint32_t ino, uint64_t size)
{
    struct zfs_inode in; int e = pressure(fs);
    if (e) return e;
    e = ino_load(fs, ino, &in); if (e) return e;
    if ((in.mode & ZFS_S_IFMT) == ZFS_S_IFDIR) return ZFS_EISDIR;
    if (size > 0x3FFFFFFFFFFFull) return ZFS_EFBIG;
    u32 inl = ZFS_INLINE_MAX < fs->maxitem ? ZFS_INLINE_MAX : fs->maxitem;
    int saved = fs->in_reserve; fs->in_reserve = 1;
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
            if (phys) { u8 *z = ZFS_MALLOC(fs->B); if (!z) { e = ZFS_EIO; goto out; } memset(z, 0, fs->B - bo);
                e = f_write_blocks(fs, ino, size, z, fs->B - bo); ZFS_FREE(z); if (e) goto out;
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
static int new_inode(struct zfs *fs, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t parent, uint32_t *ino)
{
    struct zfs_inode in = { mode, uid, gid, (mode & ZFS_S_IFMT) == ZFS_S_IFDIR ? 2 : 1, 0, fs->now, fs->now, 0, parent, 0 };
    if ((mode & ZFS_S_IFMT) != ZFS_S_IFDIR) in.flags = IFL_INLINE;
    if (fs->next_ino == 0xFFFFFFFFu) return ZFS_ENOSPC;
    *ino = fs->next_ino++;
    fs->txn_dirty = 1;
    return ino_store(fs, *ino, &in);
}
static int dir_check(struct zfs *fs, u32 dir, struct zfs_inode *d)
{
    int e = ino_load(fs, dir, d);
    if (e) return e;
    return (d->mode & ZFS_S_IFMT) == ZFS_S_IFDIR ? 0 : ZFS_ENOTDIR;
}
static int touch_dir(struct zfs *fs, u32 dir, struct zfs_inode *d, int nlink_delta)
{
    d->nlink += nlink_delta; d->mtime = d->ctime = fs->now;
    return ino_store(fs, dir, d);
}
static int make(struct zfs *fs, u32 dir, const char *name, int len, u32 mode, u32 uid, u32 gid, u32 *out)
{
    struct zfs_inode d; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, name, len))) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if (dir_find(fs, dir, (const u8 *)name, len, NULL, NULL) == 0) return ZFS_EEXIST;
    u32 ino;
    if ((e = new_inode(fs, mode, uid, gid, dir, &ino))) return e;
    u8 type = (mode & ZFS_S_IFMT) == ZFS_S_IFDIR ? 4 : (mode & ZFS_S_IFMT) == ZFS_S_IFLNK ? 10 : 8;
    if ((e = dir_add(fs, dir, (const u8 *)name, len, ino, type))) return e;
    if ((e = touch_dir(fs, dir, &d, type == 4 ? 1 : 0))) return e;
    if (out) *out = ino;
    return 0;
}
int zfs_create(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino)
{ return make(fs, dir, name, len, ZFS_S_IFREG | (mode & 07777), uid, gid, ino); }
int zfs_mkdir(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t mode, uint32_t uid, uint32_t gid, uint32_t *ino)
{ return make(fs, dir, name, len, ZFS_S_IFDIR | (mode & 07777), uid, gid, ino); }
int zfs_symlink(struct zfs *fs, uint32_t dir, const char *name, int len, const char *target, uint32_t uid, uint32_t gid, uint32_t *ino)
{
    u32 i; int e = make(fs, dir, name, len, ZFS_S_IFLNK | 0777, uid, gid, &i);
    if (e) return e;
    int tl = (int)strlen(target);
    if (tl) { e = zfs_write(fs, i, 0, target, tl); if (e < 0) return e; }
    if (ino) *ino = i;
    return 0;
}
int zfs_readlink(struct zfs *fs, uint32_t ino, char *buf, int max)
{
    struct zfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if ((in.mode & ZFS_S_IFMT) != ZFS_S_IFLNK) return ZFS_EINVAL;
    return zfs_read(fs, ino, 0, buf, in.size < (u64)max ? (u32)in.size : (u32)max);
}
int zfs_link(struct zfs *fs, uint32_t dir, const char *name, int len, uint32_t ino)
{
    struct zfs_inode d, in; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, name, len))) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = ino_load(fs, ino, &in))) return e;
    if ((in.mode & ZFS_S_IFMT) == ZFS_S_IFDIR) return ZFS_EISDIR;
    if (dir_find(fs, dir, (const u8 *)name, len, NULL, NULL) == 0) return ZFS_EEXIST;
    if ((e = dir_add(fs, dir, (const u8 *)name, len, ino, (in.mode & ZFS_S_IFMT) == ZFS_S_IFLNK ? 10 : 8))) return e;
    in.nlink++; in.ctime = fs->now;
    if ((e = ino_store(fs, ino, &in))) return e;
    return touch_dir(fs, dir, &d, 0);
}
static int has_orphan(struct zfs *fs, u32 ino)
{
    u8 key[KEYLEN]; mkkey(key, 0, K_ORPHAN, ino);
    return t_search(fs, key, S_EXACT, NULL, NULL, NULL) == 0;
}
static int inode_destroy(struct zfs *fs, u32 ino, struct zfs_inode *in)
{
    u8 key[KEYLEN]; int e;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    if (in->flags & IFL_INLINE) { e = inline_store(fs, ino, NULL, 0); if (e) goto out; }
    else { e = f_map_clear_from(fs, ino, 0); if (e) goto out; }
    mkkey(key, ino, K_INODE, 0);
    e = t_delete(fs, key);
out:
    fs->in_reserve = saved;
    return e;
}
static int drop_link(struct zfs *fs, u32 ino)
{
    struct zfs_inode in; int e = ino_load(fs, ino, &in);
    if (e) return e;
    if (in.nlink > 0) in.nlink--;
    in.ctime = fs->now;
    if (in.nlink == 0 && !has_orphan(fs, ino)) { return inode_destroy(fs, ino, &in); }
    return ino_store(fs, ino, &in);
}
int zfs_unlink(struct zfs *fs, uint32_t dir, const char *name, int len)
{
    struct zfs_inode d; u32 ino; u8 type; int e = pressure(fs);
    if (e) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = dir_find(fs, dir, (const u8 *)name, len, &ino, &type))) return e;
    if (type == 4) return ZFS_EISDIR;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    e = dir_del(fs, dir, (const u8 *)name, len);
    if (!e) e = drop_link(fs, ino);
    if (!e) e = touch_dir(fs, dir, &d, 0);
    fs->in_reserve = saved;
    return e;
}
int zfs_rmdir(struct zfs *fs, uint32_t dir, const char *name, int len)
{
    struct zfs_inode d, v; u32 ino; u8 type; int e = pressure(fs);
    if (e) return e;
    if ((e = dir_check(fs, dir, &d))) return e;
    if ((e = dir_find(fs, dir, (const u8 *)name, len, &ino, &type))) return e;
    if (type != 4) return ZFS_ENOTDIR;
    if (ino == ZFS_ROOT_INO) return ZFS_EINVAL;
    e = dir_empty(fs, ino); if (e < 0) return e;
    if (!e) return ZFS_ENOTEMPTY;
    if ((e = ino_load(fs, ino, &v))) return e;
    int saved = fs->in_reserve; fs->in_reserve = 1;
    e = dir_del(fs, dir, (const u8 *)name, len);
    if (!e) e = inode_destroy(fs, ino, &v);
    if (!e) e = touch_dir(fs, dir, &d, -1);
    fs->in_reserve = saved;
    return e;
}
int zfs_rename(struct zfs *fs, uint32_t odir, const char *oname, int olen, uint32_t ndir, const char *nname, int nlen)
{
    struct zfs_inode od, nd; u32 ino, tino; u8 type, ttype; int e = pressure(fs);
    if (e) return e;
    if ((e = check_name(fs, nname, nlen))) return e;
    if ((e = dir_check(fs, odir, &od))) return e;
    if ((e = dir_check(fs, ndir, &nd))) return e;
    if ((e = dir_find(fs, odir, (const u8 *)oname, olen, &ino, &type))) return e;
    if (odir == ndir && olen == nlen && !memcmp(oname, nname, olen)) return 0;
    if (type == 4) {                       /* a directory must not move into itself */
        u32 x = ndir;
        for (int guard = 0; guard < 100000; guard++) {
            if (x == ino) return ZFS_EINVAL;
            if (x == ZFS_ROOT_INO) break;
            struct zfs_inode t; if ((e = ino_load(fs, x, &t))) return e;
            x = t.parent;
        }
    }
    int saved = fs->in_reserve; fs->in_reserve = 1;
    e = dir_find(fs, ndir, (const u8 *)nname, nlen, &tino, &ttype);
    if (e == 0) {
        if (tino == ino) { fs->in_reserve = saved; return 0; }
        if (type == 4 && ttype != 4) { e = ZFS_ENOTDIR; goto out; }
        if (type != 4 && ttype == 4) { e = ZFS_EISDIR; goto out; }
        if (ttype == 4) {
            int em = dir_empty(fs, tino); if (em < 0) { e = em; goto out; } if (!em) { e = ZFS_ENOTEMPTY; goto out; }
            struct zfs_inode tv; if ((e = ino_load(fs, tino, &tv))) goto out;
            if ((e = dir_del(fs, ndir, (const u8 *)nname, nlen))) goto out;
            if ((e = inode_destroy(fs, tino, &tv))) goto out;
            if ((e = ino_load(fs, ndir, &nd))) goto out;
            nd.nlink--; if ((e = ino_store(fs, ndir, &nd))) goto out;
        } else {
            if ((e = dir_del(fs, ndir, (const u8 *)nname, nlen))) goto out;
            if ((e = drop_link(fs, tino))) goto out;
        }
    } else if (e != ZFS_ENOENT) goto out;
    if ((e = dir_del(fs, odir, (const u8 *)oname, olen))) goto out;
    if ((e = dir_add(fs, ndir, (const u8 *)nname, nlen, ino, type))) goto out;
    if (type == 4 && odir != ndir) {
        struct zfs_inode mv;
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
int zfs_orphan_add(struct zfs *fs, uint32_t ino)
{
    u8 key[KEYLEN]; int e = pressure(fs);
    if (e) return e;
    mkkey(key, 0, K_ORPHAN, ino);
    return t_insert(fs, key, key, 0, 1);
}
int zfs_orphan_done(struct zfs *fs, uint32_t ino)
{
    u8 key[KEYLEN]; struct zfs_inode in; int e = pressure(fs);
    if (e) return e;
    mkkey(key, 0, K_ORPHAN, ino);
    e = t_delete(fs, key);
    if (e) return e;
    if (ino_load(fs, ino, &in) == 0 && in.nlink == 0) return inode_destroy(fs, ino, &in);
    return 0;
}
static int orphans_free(struct zfs *fs)
{
    for (;;) {
        u8 key[KEYLEN], k2[KEYLEN]; mkkey(key, 0, K_ORPHAN, 0);
        int e = t_search(fs, key, S_CEIL, k2, NULL, NULL);
        if (e == ZFS_ENOENT) return 0;
        if (e) return e;
        if (rd32(k2) != 0 || k2[4] != K_ORPHAN) return 0;
        if ((e = zfs_orphan_done(fs, rd32(k2 + 8)))) return e;
    }
}

/* ---------- fsck ---------- */
struct fk { u32 ino, mode, nlink, flags, refs, subdirs; u64 size; u32 maxext; int seen_inline; };
struct fctx {
    struct zfs *fs; void (*log)(void *, const char *); void *lctx; int problems;
    u8 *used; struct fk *tab; u32 ntab, captab; u8 prev[KEYLEN]; int have_prev;
    u32 tree_blocks; u64 counted;
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
        struct fk *n = ZFS_MALLOC(sizeof(struct fk) * c->captab);
        if (c->tab) { memcpy(n, c->tab, sizeof(struct fk) * c->ntab); ZFS_FREE(c->tab); }
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
    struct zfs *fs = c->fs; struct cb *cb;
    int e = cb_get(fs, blk, KIND_TREE, &cb);
    if (e) { fprob(c, "tree block # unreadable or bad checksum", blk, 0); return; }
    u8 *b = cb->data;
    fuse_blk(c, blk, "tree"); c->tree_blocks++;
    if (b[7] != level) fprob(c, "block # has the wrong level", blk, 0);
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
            for (u32 p = 0; p + 6 <= sz; ) {
                u32 ino = rd32(v + p); u8 type = v[p + 4]; int nl = v[p + 5];
                if (p + 6 + nl > sz || nl == 0) { fprob(c, "directory # has a bad entry", obj, 0); break; }
                if (name_hash(v + p + 6, nl) != koff) fprob(c, "directory # entry hash mismatch", obj, 0);
                struct fk *t = ftab_get(c, ino, 1); t->refs++;
                if (type == 4) { t->seen_inline |= 0x200; ftab_get(c, obj, 1)->subdirs++; }
                p += 6 + nl;
            }
        } else if (kind == K_ORPHAN) {
            ftab_get(c, koff, 1)->seen_inline |= 0x400;
        } else fprob(c, "unknown item kind in leaf #", blk, 0);
    }
    cb_put(cb);
}
int zfs_fsck(struct zfs *fs, void (*log)(void *, const char *), void *lctx)
{
    struct fctx c; memset(&c, 0, sizeof c);
    c.fs = fs; c.log = log; c.lctx = lctx;
    int e = commit(fs); if (e) { fprob(&c, "cannot commit before check", 0, 0); return c.problems; }
    c.used = ZFS_MALLOC(((size_t)fs->nb * fs->bmbytes) + 8);
    memset(c.used, 0, ((size_t)fs->nb * fs->bmbytes) + 8);
    for (u32 b = 0; b < fs->dstart; b++) bit_set(c.used, b);
    fwalk(&c, fs->root, fs->height - 1);
    for (u32 i = 0; i < c.ntab; i++) {
        struct fk *f = &c.tab[i];
        int has_inode = f->seen_inline & 0x100, orphan = f->seen_inline & 0x400;
        if (!has_inode) { if (f->ino != 0) fprob(&c, "inode # referenced or has data but no inode item", f->ino, 0); continue; }
        int isdir = (f->mode & ZFS_S_IFMT) == ZFS_S_IFDIR;
        if (isdir) {
            u32 want = 2 + f->subdirs;
            if (f->nlink != want) fprob(&c, "directory # has nlink % (counted differently)", f->ino, f->nlink);
            if (f->ino != ZFS_ROOT_INO && f->refs != 1) fprob(&c, "directory # is referenced % times", f->ino, f->refs);
        } else {
            if (f->nlink != f->refs && !(orphan && f->nlink == 0 && f->refs == 0)) fprob(&c, "inode # has nlink % not matching its entries", f->ino, f->nlink);
            if ((f->flags & IFL_INLINE) && f->maxext) fprob(&c, "inode # is inline but has extents", f->ino, 0);
            if (!(f->flags & IFL_INLINE) && (f->seen_inline & 1)) fprob(&c, "inode # has inline data but is not inline", f->ino, 0);
            if (!(f->flags & IFL_INLINE) && f->maxext && ((u64)(f->maxext - 1) << fs->bshift) >= f->size && f->size) fprob(&c, "inode # has data beyond its size", f->ino, 0);
        }
        if (f->nlink == 0 && !orphan && f->ino != ZFS_ROOT_INO) fprob(&c, "inode # has nlink 0 and is not an orphan", f->ino, 0);
    }
    /* bitmap against what the tree uses */
    u32 freecnt = 0;
    for (u32 b = 0; b < fs->nblocks; b++) {
        int u = bit_get(c.used, b), m = bit_get(fs->bm, b);
        if (u && !m) fprob(&c, "block # is used but marked free", b, 0);
        if (!u && m) fprob(&c, "block # is marked used but nothing uses it", b, 0);
        if (!m) freecnt++;
    }
    if (freecnt != fs->free_blocks) fprob(&c, "free block count is % but bitmap says #", freecnt, fs->free_blocks);
    ZFS_FREE(c.used); ZFS_FREE(c.tab);
    return c.problems;
}
