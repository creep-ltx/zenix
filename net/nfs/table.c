/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: the handle table and the inode numbers (DESIGN D23 4.4), the attribute and name caches (6.1), the
 * data cache (6.2) and the directory position tables (6.3).
 *
 * A file is its (fileid, handle bytes). Its number is the fileid's low 32 bits when that number is free or already
 * this file's, else a probe from a hash of the fileid with the top bit set; the export's root is always 2; 0 and 1
 * are never given. An entry stays while the kernel holds the inode (FS_OPEN against FS_CLOSE), while it has unsaved
 * data and while a request is pinned on it; otherwise it is a cache entry, dropped by a clock hand when the table is
 * full. Each slot has a generation, so a name cached for a file never finds another file in the same slot.
 */
#include "nfs_int.h"

static u32 hash32(u32 v) { return (v * 2654435761u) ^ (v >> 15); }
static u32 fid32(u64 f) { return (u32)f ^ (u32)(f >> 32); }

struct node *nd(u16 i) { return i < N.pf.nnodes && (N.nodes[i].flags & NF_USED) ? &N.nodes[i] : 0; }

/* ---- the two hash indexes (open addressing, linear probes, deletion by shifting back) ---- */
static int ih_find(u32 ino)
{
    for (u32 h = hash32(ino) & N.hmask;; h = (h + 1) & N.hmask) {
        u16 i = N.inoh[h];
        if (i == N_NONE) return -1;
        if (N.nodes[i].ino == ino) return i;
    }
}
static void h_add(u16 *t, u32 key, u16 idx)
{
    u32 h = hash32(key) & N.hmask;
    while (t[h] != N_NONE) h = (h + 1) & N.hmask;
    t[h] = idx;
}
static void h_del(u16 *t, u32 (*keyof)(u16), u16 idx)
{
    u32 h = hash32(keyof(idx)) & N.hmask;
    while (t[h] != idx) { if (t[h] == N_NONE) return; h = (h + 1) & N.hmask; }
    t[h] = N_NONE;
    for (u32 j = (h + 1) & N.hmask; t[j] != N_NONE; j = (j + 1) & N.hmask) {   /* shift the run back */
        u16 k = t[j];
        u32 home = hash32(keyof(k)) & N.hmask;
        /* k may move to h if h lies cyclically in [home, j) */
        if (((j - home) & N.hmask) >= ((j - h) & N.hmask)) { t[h] = k; t[j] = N_NONE; h = j; }
    }
}
static u32 key_ino(u16 i) { return N.nodes[i].ino; }
static u32 key_fid(u16 i) { return fid32(N.nodes[i].fileid); }

int node_find(u32 ino) { return ih_find(ino); }

int tab_init(void)
{
    for (u32 h = 0; h <= N.hmask; h++) N.inoh[h] = N.fidh[h] = N_NONE;
    return 0;
}

void node_fh_free(struct node *p)
{
    if (p->fh && p->fh != p->fhi) zfree(p->fh, p->fhlen);
}

static void node_drop(u16 i)
{
    struct node *p = &N.nodes[i];
    h_del(N.inoh, key_ino, i);
    if (!(p->flags & NF_STALE)) h_del(N.fidh, key_fid, i);
    dc_drop(i);
    node_fh_free(p);
    p->fh = 0;
    p->fhlen = 0;
    p->flags = 0;
    p->gen++;
}

static int held(const struct node *p)
{
    return p->pins || p->opens || p->unsaved || p->commit_at || (p->flags & (NF_STALE | NF_ROOT));
}

/* a free slot; when none, the clock hand drops a cache entry nobody holds */
static int node_slot(void)
{
    for (u32 i = 0; i < N.pf.nnodes; i++) if (!(N.nodes[i].flags & NF_USED)) return (int)i;
    for (u32 k = 0; k < 2 * N.pf.nnodes; k++) {
        u16 i = (u16)(N.hand++ % N.pf.nnodes);
        struct node *p = &N.nodes[i];
        if (held(p)) continue;
        if (p->flags & NF_REF) { p->flags &= (u8)~NF_REF; continue; }
        node_drop(i);
        return i;
    }
    if (!N.logged_full) { lg("the handle table is full (files held open)", 0, -1, 0); N.logged_full = 1; }
    return -1;
}

static int same_fh(const struct node *p, const u8 *fh, u32 n) { return p->fhlen == n && !memcmp(p->fh, fh, n); }

static u32 ino_free(u64 fileid)
{
    u32 c = (u32)fileid;
    int k = c >= 3 ? ih_find(c) : -1;
    if (k >= 0 && N.nodes[k].fileid == fileid && !held(&N.nodes[k])) { node_drop((u16)k); k = -1; }
    /* (the same fileid with another handle, only cached: the old handle is dropped, the number stays the file's) */
    if (c >= 3 && k < 0) return c;
    u32 h = hash32(fid32(fileid)) | 0x80000000u;
    while (ih_find(h) >= 0) h = (h + 1) | 0x80000000u;
    return h;
}

static int fid_lookup(u64 fileid, const u8 *fh, u32 n)
{
    for (u32 h = hash32(fid32(fileid)) & N.hmask;; h = (h + 1) & N.hmask) {
        u16 i = N.fidh[h];
        if (i == N_NONE) return -1;
        struct node *p = &N.nodes[i];
        if (p->fileid == fileid && same_fh(p, fh, n)) return i;
    }
}

int node_get(const u8 *fh, u32 fhlen, const struct fattr *a)
{
    if (!fhlen || fhlen > FHMAX) return -1;
    if (N.root != N_NONE && same_fh(&N.nodes[N.root], fh, fhlen)) {
        if (a) node_attr(N.root, a);
        return N.root;
    }
    int i;
    if (!a) {                                    /* no attributes with the handle: only a file already known */
        for (u32 k = 0; k < N.pf.nnodes; k++)
            if ((N.nodes[k].flags & NF_USED) && !(N.nodes[k].flags & NF_STALE) && same_fh(&N.nodes[k], fh, fhlen))
                return (int)k;
        return -1;
    }
    i = fid_lookup(a->fileid, fh, fhlen);
    if (i < 0) {
        i = node_slot();
        if (i < 0) return -1;
        u8 *h = fhlen > NODE_FH ? zalloc(fhlen) : 0;
        if (fhlen > NODE_FH && !h) return -1;
        struct node *p = &N.nodes[i];
        u16 g = p->gen;
        memset(p, 0, sizeof *p);
        p->gen = g;
        p->flags = NF_USED;
        p->fileid = a->fileid;
        p->fh = h ? h : p->fhi; p->fhlen = (u8)fhlen; memcpy(p->fh, fh, fhlen);
        p->ino = ino_free(a->fileid);
        h_add(N.inoh, p->ino, (u16)i);
        h_add(N.fidh, fid32(p->fileid), (u16)i);
        N.st.nodes++;
    }
    node_attr((u16)i, a);
    return i;
}

/* the export's root: number 2, its handle from MNT (attributes come with the first GETATTR) */
int node_root(const u8 *fh, u32 n)
{
    int i = node_slot();
    if (i < 0) return -1;
    u8 *h = n > NODE_FH ? zalloc(n) : 0;
    if (n > NODE_FH && !h) return -1;
    struct node *p = &N.nodes[i];
    u16 g = p->gen;
    memset(p, 0, sizeof *p);
    p->gen = g;
    p->flags = NF_USED | NF_ROOT;
    p->ino = 2;
    p->fh = h ? h : p->fhi; p->fhlen = (u8)n; memcpy(p->fh, fh, n);
    h_add(N.inoh, 2, (u16)i);
    h_add(N.fidh, fid32(0), (u16)i);
    N.root = (u16)i;
    return i;
}

static u32 acmin(const struct node *p)
{
    if (N.o.actimeo >= 0) return (u32)N.o.actimeo * 1000;
    return p->a.type == NF3DIR ? 30000 : 3000;
}
static u32 acmax(void)
{
    if (N.o.actimeo >= 0) return (u32)N.o.actimeo * 1000;
    return 60000;
}

void node_attr(u16 i, const struct fattr *a)
{
    struct node *p = &N.nodes[i];
    int had = p->flags & NF_ATTR;
    int changed = !had || p->a.mtime != a->mtime || p->a.mtime_ns != a->mtime_ns || p->a.ctime != a->ctime ||
                  p->a.ctime_ns != a->ctime_ns || p->a.size != a->size;
    if (had && a->type == NF3DIR && (p->a.ctime != a->ctime || p->a.ctime_ns != a->ctime_ns || p->a.mtime != a->mtime ||
                                     p->a.mtime_ns != a->mtime_ns))
        p->namegen++;                            /* the directory changed: its cached names go */
    if ((p->flags & NF_ROOT) && p->fileid != a->fileid) {   /* the root's fileid, known from its first attributes */
        h_del(N.fidh, key_fid, i);
        p->fileid = a->fileid;
        h_add(N.fidh, fid32(p->fileid), i);
    }
    if (changed) p->acc_uid_ok = 0;
    p->a = *a;
    p->a.fileid = p->fileid;
    p->acto = changed ? acmin(p) : (p->acto * 2 > acmax() ? acmax() : p->acto * 2);
    if (p->acto < acmin(p)) p->acto = acmin(p);
    p->attr_at = nfs_now();
    p->flags |= NF_ATTR | NF_REF;
}

int node_fresh(u16 i)
{
    struct node *p = nd(i);
    if (!p || !(p->flags & NF_ATTR) || N.o.actimeo == 0) return 0;
    return tdiff(nfs_now(), p->attr_at) < p->acto;
}

/* a directory (or file) after our own change: wcc's "before" equal to what we had means nobody else changed it, so
 * the names (and data) cached stay; otherwise as any new attributes */
void node_wcc(u16 i, const struct res *r, int ours)
{
    struct node *p = nd(i);
    if (!p) return;
    if (!r->has_dattr) { p->flags &= (u8)~NF_ATTR; if (!ours) p->namegen++; return; }
    if (ours && r->has_pre && (p->flags & NF_ATTR) && r->pre_ctime == p->a.ctime && r->pre_ctime_ns == p->a.ctime_ns &&
        r->pre_mtime == p->a.mtime && r->pre_mtime_ns == p->a.mtime_ns && r->pre_size == p->a.size) {
        u32 acto = p->acto;
        p->a = r->dattr;
        p->a.fileid = p->fileid;
        p->attr_at = nfs_now();
        p->acto = acto;
        return;
    }
    node_attr(i, &r->dattr);
}

void node_staled(u16 i)
{
    struct node *p = nd(i);
    if (!p) return;
    p->gen++;                                    /* names, data and listings that point at it are no longer found */
    p->flags &= (u8)~NF_ATTR;
    p->acc_uid_ok = 0;
}

void node_dirchanged(u16 i)
{
    struct node *p = nd(i);
    if (p) { p->namegen++; p->flags &= (u8)~NF_ATTR; }
}

void node_pin(u16 i) { if (i < N.pf.nnodes) N.nodes[i].pins++; }
void node_unpin(u16 i) { if (i < N.pf.nnodes && N.nodes[i].pins) N.nodes[i].pins--; }

void node_ustat(u16 i, struct nfs_ustat *st)
{
    static const u32 fmt[8] = { 0, 0100000, 0040000, 0060000, 0020000, 0120000, 0140000, 0010000 };
    struct node *p = &N.nodes[i];
    st->ino = p->ino;
    st->mode = fmt[p->a.type & 7] | (p->a.mode & 01777);  /* set-user-id and set-group-id never shown (8.3) */
    st->size = (u32)p->a.size;
    st->mtime = p->a.mtime;
    st->uid = p->a.uid > 65535 ? 65534 : p->a.uid;
    st->gid = p->a.gid > 65535 ? 65534 : p->a.gid;
    st->links = p->a.nlink;
}

/* after a restart: a number the kernel holds is answered -ESTALE until its FS_CLOSE, never given again (4.4 rule 5) */
void node_stale_add(u32 ino)
{
    int i = ih_find(ino);
    if (i >= 0) { N.nodes[i].opens++; return; }
    i = node_slot();
    if (i < 0) return;
    struct node *p = &N.nodes[i];
    u16 g = p->gen;
    memset(p, 0, sizeof *p);
    p->gen = g;
    p->flags = NF_USED | NF_STALE;
    p->ino = ino;
    p->opens = 1;
    h_add(N.inoh, ino, (u16)i);
}

void node_close(u16 i)
{
    struct node *p = &N.nodes[i];
    if (p->opens) p->opens--;
    if (!p->opens && (p->flags & NF_STALE)) node_drop(i);
}

u32 ino_for_fileid(u64 fileid)
{
    for (u32 h = hash32(fid32(fileid)) & N.hmask;; h = (h + 1) & N.hmask) {
        u16 i = N.fidh[h];
        if (i == N_NONE) break;
        if (N.nodes[i].fileid == fileid) return N.nodes[i].ino;
    }
    return ino_free(fileid);
}

/* ---- the name cache ---- */
static u32 name_set(u16 dir, const char *s, u32 n)
{
    u32 h = dir * 31u + n;
    for (u32 k = 0; k < n; k++) h = h * 33u + (u8)s[k];
    return (hash32(h) % (N.pf.nnames / 4)) * 4;
}

int name_get(u16 dir, const char *s, u32 n)
{
    if (n > NC_NAME || !N.names) return -1;
    struct node *d = nd(dir);
    if (!d) return -1;
    u32 b = name_set(dir, s, n);
    for (u32 k = b; k < b + 4; k++) {
        struct nament *e = &N.names[k];
        if (e->len != n || e->dir != dir || memcmp(e->name, s, n)) continue;
        struct node *c = nd(e->child);
        if (e->dgen != d->gen || e->dng != d->namegen || !c || e->cgen != c->gen) { e->len = 0; return -1; }
        e->age = 0;
        c->flags |= NF_REF;
        return e->child;
    }
    return -1;
}

void name_put(u16 dir, const char *s, u32 n, u16 child)
{
    if (n > NC_NAME || !n || !N.names) return;
    struct node *d = nd(dir), *c = nd(child);
    if (!d || !c) return;
    u32 b = name_set(dir, s, n), pick = b;
    for (u32 k = b; k < b + 4; k++) {
        struct nament *e = &N.names[k];
        if (e->len == n && e->dir == dir && !memcmp(e->name, s, n)) { pick = k; break; }
        if (N.names[k].age < 255) N.names[k].age++;
        if (!e->len || e->age > N.names[pick].age) pick = k;
    }
    struct nament *e = &N.names[pick];
    e->dir = dir; e->dgen = d->gen; e->dng = d->namegen; e->child = child; e->cgen = c->gen;
    e->len = (u8)n; e->age = 0;
    memcpy(e->name, s, n);
}

void name_del(u16 dir, const char *s, u32 n)
{
    if (n > NC_NAME || !N.names) return;
    u32 b = name_set(dir, s, n);
    for (u32 k = b; k < b + 4; k++) {
        struct nament *e = &N.names[k];
        if (e->len == n && e->dir == dir && !memcmp(e->name, s, n)) e->len = 0;
    }
}

/* ---- the data cache: whole rsize pieces, valid while the file's ctime and size are what they were ---- */
int dc_find(u16 node, u32 blk)
{
    struct node *p = nd(node);
    if (!p) return -1;
    for (u32 k = 0; k < N.pf.ndc; k++) {
        struct dpiece *d = &N.dc[k];
        if (!d->data || !d->len || d->node != node || d->gen != p->gen || d->blk != blk) continue;
        if (d->ctime != p->a.ctime || d->ctime_ns != p->a.ctime_ns || d->size != p->a.size) { d->len = 0; return -1; }
        d->lru = ++N.lru;
        return (int)k;
    }
    return -1;
}

int dc_slot(void)
{
    int best = -1;
    for (u32 k = 0; k < N.pf.ndc; k++) {
        struct dpiece *d = &N.dc[k];
        if (!d->data) {
            d->data = zalloc(N.rsize);
            if (d->data) { d->len = 0; return (int)k; }
            continue;
        }
        if (!d->len) return (int)k;
        if (best < 0 || (s32)(d->lru - N.dc[best].lru) < 0) best = (int)k;
    }
    return best;
}

void dc_drop(u16 node)
{
    if (!N.dc) return;
    for (u32 k = 0; k < N.pf.ndc; k++) if (N.dc[k].node == node) N.dc[k].len = 0;
}

/* the pieces of a file over [off, off + n) go (our own write) */
void dc_drop_range(u16 node, u32 off, u32 n)
{
    for (u32 k = 0; k < N.pf.ndc; k++) {
        struct dpiece *d = &N.dc[k];
        if (d->node != node || !d->len) continue;
        u64 s = (u64)d->blk << N.rshift;
        if (s < (u64)off + n && s + N.rsize > off) d->len = 0;
    }
}

/* after our own write the file's new ctime and size: the pieces still cached stay valid */
void dc_restamp(u16 node, u32 octime, u32 octime_ns, u64 osize)
{
    struct node *p = nd(node);
    if (!p) return;
    for (u32 k = 0; k < N.pf.ndc; k++) {
        struct dpiece *d = &N.dc[k];
        if (d->node != node || d->gen != p->gen || !d->len) continue;
        if (d->ctime == octime && d->ctime_ns == octime_ns && d->size == osize) {
            d->ctime = p->a.ctime; d->ctime_ns = p->a.ctime_ns; d->size = p->a.size;
        }
    }
}

/* ---- readdir position tables ---- */
int rdt_find(u16 node, int make)
{
    struct node *p = nd(node);
    if (!p) return -1;
    int best = 0;
    for (u32 k = 0; k < N.pf.nrdt; k++) {
        struct rdt *t = &N.rdts[k];
        if (t->n && t->node == node && t->gen == p->gen) { t->lru = ++N.lru; return (int)k; }
        if (!N.rdts[best].n) continue;
        if (!t->n || (s32)(t->lru - N.rdts[best].lru) < 0) best = (int)k;
    }
    if (!make) return -1;
    struct rdt *t = &N.rdts[best];
    memset(t, 0, sizeof *t);
    t->node = node; t->gen = p->gen; t->lru = ++N.lru;
    t->n = 1; t->pos[0] = 0; t->cookie[0] = 0;
    t->ctime = p->a.ctime; t->ctime_ns = p->a.ctime_ns;
    if (N.chunk->rdt == best) N.chunk->valid = 0;
    return best;
}
