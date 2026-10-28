/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: the arguments and results of the calls used (DESIGN D23 4.5): portmapper GETPORT (RFC 1833), MOUNT
 * v3 MNT and UMNT, and the NFSv3 calls of RFC 1813 Zenix uses (19 and NULL). Every length and count in a result is
 * checked against the record before use (10): a handle over 64 bytes, a name over 255, data longer than asked fail
 * the reply (-EIO), never the core.
 */
#include "nfs_int.h"

static void put_fh(struct xdr *x, u16 n)
{
    struct node *p = nd(n);
    if (!p) { x->bad = 1; return; }
    x_putopaque(x, p->fh, p->fhlen);
}

static void put_name(struct xdr *x, const char *s, u32 n) { x_putopaque(x, s, n); }

static void put_sattr(struct xdr *x, const struct sattr *s)
{
    x_put32(x, s->set_mode); if (s->set_mode) x_put32(x, s->mode);
    x_put32(x, s->set_uid); if (s->set_uid) x_put32(x, s->uid);
    x_put32(x, s->set_gid); if (s->set_gid) x_put32(x, s->gid);
    x_put32(x, s->set_size); if (s->set_size) x_put64(x, s->size);
    x_put32(x, s->atime_how); if (s->atime_how == 2) { x_put32(x, s->atime); x_put32(x, 0); }
    x_put32(x, s->mtime_how); if (s->mtime_how == 2) { x_put32(x, s->mtime); x_put32(x, 0); }
}

int enc_args(struct call *c, struct xdr *x)
{
    struct op *o = c->op >= 0 ? &N.ops[c->op] : 0;
    static const struct sattr none;
    const struct sattr *sa = o ? &o->sa : &none;
    if (c->prog == PG_PMAP) {                    /* GETPORT(MOUNT v3 over TCP) */
        x_put32(x, MOUNT_PROG); x_put32(x, 3); x_put32(x, 6); x_put32(x, 0);
        return 0;
    }
    if (c->prog == PG_MOUNT) {                   /* MNT, UMNT: the export's path */
        put_name(x, N.exp, (u32)strlen(N.exp));
        return 0;
    }
    switch (c->proc) {
    case P3_NULL: return 0;
    case P3_GETATTR: case P3_READLINK: case P3_FSSTAT: case P3_FSINFO:
        put_fh(x, c->node);
        return 0;
    case P3_SETATTR:
        put_fh(x, c->node); put_sattr(x, sa); x_put32(x, 0);   /* no guard */
        return 0;
    case P3_LOOKUP: case P3_REMOVE: case P3_RMDIR:
        put_fh(x, c->node); put_name(x, c->name, c->nlen);
        return 0;
    case P3_ACCESS:
        put_fh(x, c->node); x_put32(x, c->count);
        return 0;
    case P3_READ:
        put_fh(x, c->node); x_put64(x, c->off); x_put32(x, c->count);
        return 0;
    case P3_WRITE: {
        if (c->piece < 0) return -EIO;
        struct piece *p = &N.pieces[c->piece];
        put_fh(x, p->node); x_put64(x, p->off); x_put32(x, p->len);
        x_put32(x, c->fsync ? 2 : 0);            /* FILE_SYNC with the code y, else UNSTABLE (7.1) */
        x_putopaque(x, p->data, p->len);
        return 0;
    }
    case P3_CREATE:
        put_fh(x, c->node); put_name(x, c->name, c->nlen);
        x_put32(x, c->count);                    /* createmode3: UNCHECKED 0, EXCLUSIVE 2 */
        if (c->count == 2) x_putfixed(x, o ? o->cverf : (const u8 *)"\0\0\0\0\0\0\0\0", 8);
        else put_sattr(x, sa);
        return 0;
    case P3_MKDIR:
        put_fh(x, c->node); put_name(x, c->name, c->nlen); put_sattr(x, sa);
        return 0;
    case P3_SYMLINK:
        put_fh(x, c->node); put_name(x, c->name, c->nlen); put_sattr(x, sa); put_name(x, c->name2, c->nlen2);
        return 0;
    case P3_RENAME:
        put_fh(x, c->node); put_name(x, c->name, c->nlen); put_fh(x, c->node2); put_name(x, c->name2, c->nlen2);
        return 0;
    case P3_LINK:
        put_fh(x, c->node2); put_fh(x, c->node); put_name(x, c->name, c->nlen);
        return 0;
    case P3_READDIR:
        put_fh(x, c->node); x_put64(x, c->cookie); x_putfixed(x, c->verf, 8); x_put32(x, c->count);
        return 0;
    case P3_READDIRPLUS:
        put_fh(x, c->node); x_put64(x, c->cookie); x_putfixed(x, c->verf, 8);
        x_put32(x, c->count / 4); x_put32(x, c->count);   /* dircount, maxcount */
        return 0;
    case P3_COMMIT:
        put_fh(x, c->node); x_put64(x, 0); x_put32(x, 0);   /* the whole file */
        return 0;
    }
    return -EIO;
}

/* ---- results ---- */
static void get_fattr(struct xdr *x, struct fattr *a)
{
    a->type = x_u32(x); a->mode = x_u32(x); a->nlink = x_u32(x); a->uid = x_u32(x); a->gid = x_u32(x);
    a->size = x_u64(x);
    x_u64(x);                                    /* used */
    x_u32(x); x_u32(x);                          /* rdev */
    x_u64(x);                                    /* fsid */
    a->fileid = x_u64(x);
    x_u32(x); x_u32(x);                          /* atime */
    a->mtime = x_u32(x); a->mtime_ns = x_u32(x);
    a->ctime = x_u32(x); a->ctime_ns = x_u32(x);
    if (a->type < NF3REG || a->type > NF3FIFO) x->bad = 1;
}
static void post_attr(struct xdr *x, struct fattr *a, u8 *has)
{
    u32 f = x_u32(x);
    if (f > 1) { x->bad = 1; return; }
    *has = (u8)f;
    if (f) get_fattr(x, a);
}
static void wcc(struct xdr *x, struct res *r)
{
    u32 f = x_u32(x);
    if (f > 1) { x->bad = 1; return; }
    r->has_pre = (u8)f;
    if (f) {
        r->pre_size = x_u64(x);
        r->pre_mtime = x_u32(x); r->pre_mtime_ns = x_u32(x);
        r->pre_ctime = x_u32(x); r->pre_ctime_ns = x_u32(x);
    }
    post_attr(x, &r->dattr, &r->has_dattr);
}
static void get_fh(struct xdr *x, struct res *r)
{
    u32 n;
    const u8 *p = x_opaque(x, FHMAX, &n);
    if (!p) return;
    if (!n) { x->bad = 1; return; }
    r->fhlen = (u8)n; memcpy(r->fh, p, n); r->has_fh = 1;
}
static void post_fh(struct xdr *x, struct res *r)
{
    u32 f = x_u32(x);
    if (f > 1) { x->bad = 1; return; }
    if (f) get_fh(x, r);
}

int nfs_errno(u32 s)
{
    switch (s) {
    case 1: return -EPERM;
    case 2: return -ENOENT;
    case 5: return -EIO;
    case 6: return -ENXIO;
    case 13: return -EACCES;
    case 17: return -EEXIST;
    case 18: return -EXDEV;
    case 19: return -ENODEV;
    case 20: return -ENOTDIR;
    case 21: return -EISDIR;
    case 22: return -EINVAL;
    case 27: return -EFBIG;
    case 28: return -ENOSPC;
    case 30: return -EROFS;
    case 31: return -EMLINK;
    case 63: return -ENAMETOOLONG;
    case 66: return -ENOTEMPTY;
    case 69: return -ENOSPC;                     /* DQUOT */
    case 70: case 10001: return -NFS_ESTALE;     /* STALE, BADHANDLE */
    case 10004: return -EOPNOTSUPP;
    }
    lg("the NAS answered error ", 0, (long)s, " (-EIO)");
    return -EIO;
}

int dec_res(struct call *c, struct xdr *x, struct res *r)
{
    if (c->prog == PG_PMAP) {
        r->port = x_u32(x);
        if (r->port > 65535) x->bad = 1;
        return x->bad ? -1 : 0;
    }
    if (c->prog == PG_MOUNT) {
        if (c->proc == MNT_UMNT) return 0;
        r->status = x_u32(x);
        if (x->bad) return -1;
        if (r->status) { r->err = r->status == 10004 ? -EOPNOTSUPP : r->status < 100 ? -(int)r->status : -EIO; return 0; }
        get_fh(x, r);
        u32 n = x_u32(x);                        /* the auth flavours */
        if (n > 64) x->bad = 1;
        for (u32 i = 0; i < n && !x->bad; i++) x_u32(x);
        return x->bad ? -1 : 0;
    }
    r->status = x_u32(x);
    if (x->bad) return -1;
    int ok = r->status == NFS3_OK;
    if (!ok && r->status != NFS3ERR_JUKEBOX && r->status != NFS3ERR_BAD_COOKIE) r->err = nfs_errno(r->status);
    switch (c->proc) {
    case P3_NULL: break;
    case P3_GETATTR:
        if (ok) { get_fattr(x, &r->attr); r->has_attr = 1; }
        break;
    case P3_SETATTR: case P3_REMOVE: case P3_RMDIR:
        wcc(x, r);
        break;
    case P3_LOOKUP:
        if (ok) { get_fh(x, r); post_attr(x, &r->attr, &r->has_attr); }
        post_attr(x, &r->dattr, &r->has_dattr);
        break;
    case P3_ACCESS:
        post_attr(x, &r->attr, &r->has_attr);
        if (ok) r->access = x_u32(x);
        break;
    case P3_READLINK: {
        post_attr(x, &r->attr, &r->has_attr);
        if (ok) { u32 n; r->data = x_opaque(x, 4096, &n); r->count = n; }
        break;
    }
    case P3_READ: {
        post_attr(x, &r->attr, &r->has_attr);
        if (ok) {
            u32 cnt = x_u32(x), n;
            r->eof = (int)x_u32(x);
            r->data = x_opaque(x, c->count, &n);  /* never more than asked */
            if (r->eof > 1 || n != cnt) x->bad = 1;
            r->count = n;
        }
        break;
    }
    case P3_WRITE: {
        wcc(x, r);
        if (ok) {
            r->count = x_u32(x); r->committed = x_u32(x);
            const u8 *v = x_fixed(x, 8);
            if (v) memcpy(r->verf, v, 8);
            if (r->committed > 2 || r->count > c->count) x->bad = 1;
        }
        break;
    }
    case P3_CREATE: case P3_MKDIR: case P3_SYMLINK: {
        if (ok) { post_fh(x, r); post_attr(x, &r->attr, &r->has_attr); }
        struct fattr keep = r->attr; u8 had = r->has_attr;
        wcc(x, r);                               /* the directory's */
        r->attr = keep; r->has_attr = had;
        break;
    }
    case P3_RENAME: {
        struct res t;
        wcc(x, r);                               /* from's directory (to's is only checked) */
        memset(&t, 0, sizeof t);
        wcc(x, &t);
        break;
    }
    case P3_LINK: {
        post_attr(x, &r->attr, &r->has_attr);
        struct fattr keep = r->attr; u8 had = r->has_attr;
        wcc(x, r);
        r->attr = keep; r->has_attr = had;
        break;
    }
    case P3_READDIR: case P3_READDIRPLUS: {
        post_attr(x, &r->dattr, &r->has_dattr);
        if (ok) {
            const u8 *v = x_fixed(x, 8);
            if (v) memcpy(r->verf, v, 8);
            r->dir = *x;                         /* the entries: walked by dec_dirent */
            x->p = x->end;
        }
        break;
    }
    case P3_FSSTAT:
        post_attr(x, &r->attr, &r->has_attr);
        if (ok) {
            r->tbytes = x_u64(x); x_u64(x); r->abytes = x_u64(x);
            r->tfiles = x_u64(x); x_u64(x); r->afiles = x_u64(x);
            x_u32(x);
        }
        break;
    case P3_FSINFO:
        post_attr(x, &r->attr, &r->has_attr);
        if (ok) {
            r->rtmax = x_u32(x); r->rtpref = x_u32(x); x_u32(x);
            r->wtmax = x_u32(x); r->wtpref = x_u32(x); x_u32(x);
            r->dtpref = x_u32(x);
            x_u64(x); x_u32(x); x_u32(x); x_u32(x);
        }
        break;
    case P3_COMMIT:
        wcc(x, r);
        if (ok) { const u8 *v = x_fixed(x, 8); if (v) memcpy(r->verf, v, 8); }
        break;
    default:
        return -1;
    }
    return x->bad ? -1 : 0;
}

/* the next entry of a READDIR(PLUS) answer: 1 an entry, 0 the end (r->eof set), -1 a malformed list */
int dec_dirent(struct xdr *x, int plus, u64 *fileid, const u8 **name, u32 *nlen, u64 *cookie, struct res *r)
{
    u32 more = x_u32(x);
    if (x->bad || more > 1) return -1;
    if (!more) {
        u32 e = x_u32(x);
        if (x->bad || e > 1) return -1;
        r->eof = (int)e;
        return 0;
    }
    *fileid = x_u64(x);
    *name = x_opaque(x, NAMEMAX, nlen);
    *cookie = x_u64(x);
    r->has_attr = 0; r->has_fh = 0;
    if (plus) { post_attr(x, &r->attr, &r->has_attr); post_fh(x, r); }
    return x->bad ? -1 : 1;
}
