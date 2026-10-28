/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: its state, the entry points of nfs.h, the option codes, the mount (GETPORT, MNT, FSINFO, GETATTR of
 * the root; the saved root checked after a restart, D23 4.1), "not answering" (5.3), credentials, memory and the log.
 */
#include "nfs_int.h"

struct nfs N;
int node_root(const u8 *fh, u32 n);
void node_close(u16 i);

/* ---- small things ---- */
u32 tdiff(u32 later, u32 earlier) { return later - earlier; }
int due(u32 t, u32 now) { return (s32)(now - t) >= 0; }

void *zalloc(u32 n)
{
    void *p = n ? nfs_alloc(n) : 0;
    if (p) { memset(p, 0, n); N.mem += n; }
    return p;
}
void zfree(void *p, u32 n)
{
    if (!p) return;
    nfs_free(p);
    N.mem -= n;
}

static char *put_s(char *d, char *end, const char *s)
{
    while (s && *s && d < end) *d++ = *s++;
    return d;
}
static char *put_n(char *d, char *end, unsigned long n)
{
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (k && d < end) *d++ = t[--k];
    return d;
}
/* "nfs0: " a b [n] c: one line for the kernel log; n -1 none */
void lg(const char *a, const char *b, long n, const char *c)
{
    char buf[160], *d = buf, *end = buf + sizeof buf - 1;
    d = put_s(d, end, N.dev); d = put_s(d, end, ": ");
    d = put_s(d, end, a); d = put_s(d, end, b);
    if (n != -1) { if (n < 0) { d = put_s(d, end, "-"); n = -n; } d = put_n(d, end, (unsigned long)n); }
    d = put_s(d, end, c);
    *d = 0;
    nfs_log(buf);
}

/* ---- credentials: interned, shared by the requests, calls and pieces that use them ---- */
int cred_get(const struct nfs_req *r)
{
    int fr = -1;
    u32 n = r->ngroups > NFS_NGROUPS ? NFS_NGROUPS : r->ngroups;
    for (u32 i = 1; i < N.pf.ncreds; i++) {      /* slot 0: root's, for the mount's own calls */
        struct cred *c = &N.creds[i];
        if (!c->refs) { if (fr < 0) fr = (int)i; continue; }
        if (c->uid == r->uid && c->gid == r->gid && c->n == n && !memcmp(c->g, r->groups, n * 4)) { c->refs++; return (int)i; }
    }
    if (r->uid == 0 && r->gid == 0 && n == 0) { N.creds[0].refs++; return 0; }
    if (fr < 0) return -1;
    struct cred *c = &N.creds[fr];
    c->refs = 1; c->gen++; c->uid = r->uid; c->gid = r->gid; c->n = (u16)n;
    memcpy(c->g, r->groups, n * 4);
    return fr;
}
void cred_put(int c)
{
    if (c >= 0 && c < (int)N.pf.ncreds && N.creds[c].refs > (c == 0 ? 1 : 0)) N.creds[c].refs--;
}

/* ---- the option codes (4.1): h s tN rN o y aN l, in one word ---- */
static int num(const char **p, u32 *out)
{
    const char *s = *p;
    u32 v = 0;
    if (*s < '0' || *s > '9') return -1;
    while (*s >= '0' && *s <= '9') { if (v > 100000) return -1; v = v * 10 + (u32)(*s++ - '0'); }
    *p = s; *out = v;
    return 0;
}
int nfs_codes_parse(const char *s, struct nfs_opts *o)
{
    o->soft = 0; o->timeo_ms = 30000; o->retrans = 3; o->ro = 0; o->sync = 0; o->actimeo = -1; o->lazy = 0;
    for (; s && *s; ) {
        char c = *s++;
        u32 v;
        switch (c) {
        case 'h': o->soft = 0; break;
        case 's': o->soft = 1; break;
        case 'o': o->ro = 1; break;
        case 'y': o->sync = 1; break;
        case 'l': o->lazy = 1; break;
        case 't': if (num(&s, &v) || !v || v > 3600) return -EINVAL; o->timeo_ms = v * 1000; break;
        case 'r': if (num(&s, &v) || !v || v > 100) return -EINVAL; o->retrans = (int)v; break;
        case 'a': if (num(&s, &v) || v > 3600) return -EINVAL; o->actimeo = (int)v; break;
        default: return -EINVAL;
        }
    }
    return 0;
}

/* ---- set up ---- */
#define PART(p, n) do { off = (off + 7) & ~7u; if (b) (p) = (void *)(b + off); off += (u32)(n) * (u32)sizeof *(p); } while (0)

static u32 pow2_down(u32 v) { u32 p = 1024; while (p * 2 <= v && p < (1u << 20)) p *= 2; return p; }

int nfs_init(const struct nfs_conf *c)
{
    nfs_end();
    memset(&N, 0, sizeof N);
    if (!c || !c->export_path || c->export_path[0] != '/') return -EINVAL;
    if (nfs_codes_parse(c->codes, &N.o)) return -EINVAL;
    u32 el = (u32)strlen(c->export_path);
    if (el > FS_PATHMAX) return -ENAMETOOLONG;
    memcpy(N.exp, c->export_path, el + 1);
    const char *dev = c->dev ? c->dev : "nfs";
    u32 dl = (u32)strlen(dev);
    memcpy(N.dev, dev, dl < 15 ? dl : 15);
    struct prof *p = &N.pf;
    N.o_large = c->large;
    if (!c->large) {
        p->rsize = p->wsize = 8192; p->nnodes = 64; p->nnames = 64; p->nrdt = 8; p->ndc = 4; p->cap = 32768;
        p->nops = 8; p->npieces = 32; p->ncreds = 8;
    } else {
        u32 cb = c->cache_bytes ? c->cache_bytes : 256 * 1024;
        if (cb > 1024 * 1024) cb = 1024 * 1024;
        if (cb < 128 * 1024) cb = 128 * 1024;
        p->rsize = p->wsize = 32768; p->nnodes = 256; p->nnames = 256; p->nrdt = 64;
        p->cap = cb / 2; p->ndc = (cb / 2) / p->rsize;
        p->nops = 32; p->npieces = 128; p->ncreds = 16;
    }
    p->ncalls = p->nops * 2 + p->npieces + 8;
    N.rsize = p->rsize; N.wsize = p->wsize;
    N.rshift = 0; while ((1u << N.rshift) < N.rsize) N.rshift++;
    N.hmask = 1; while (N.hmask < p->nnodes * 2) N.hmask <<= 1; N.hmask--;
    N.rxcap = p->rsize + 1024; N.txcap = p->wsize + 1024;
    for (int pass = 0; pass < 2; pass++) {      /* pass 0 counts, pass 1 places: one block for everything */
        u32 off = 0;
        u8 *b = N.blk;
        PART(N.nodes, p->nnodes);
        PART(N.inoh, N.hmask + 1);
        PART(N.fidh, N.hmask + 1);
        PART(N.names, p->nnames);
        PART(N.rdts, p->nrdt);
        PART(N.chunk, 1);
        PART(N.dc, p->ndc);
        PART(N.ops, p->nops);
        PART(N.calls, p->ncalls);
        PART(N.pieces, p->npieces);
        PART(N.creds, p->ncreds);
        PART(N.rx, N.rxcap);
        PART(N.tx, N.txcap);
        if (!pass && !(N.blk = zalloc(N.blksz = off))) goto nomem;
    }
    tab_init();
    N.chunk->rdt = -1;
    N.root = N_NONE;
    N.quitop = -1;
    N.creds[0].refs = 1;                         /* root, for MNT and UMNT */
    N.seed = c->seed;
    N.xid = c->seed * 2654435761u + 0x5A17u;     /* random start (3.4) */
    wr_reset();
    /* the saved table of an earlier start (4.1): "NFS1", the root's handle and fileid */
    if (c->saved && c->nsaved >= 5 + 8) {
        const u8 *s = c->saved;
        u32 fl = s[4];
        if (!memcmp(s, "NFS1", 4) && fl && fl <= FHMAX && c->nsaved >= 5 + fl + 8) {
            N.saved_ok = 1; N.savedfhlen = (u8)fl; memcpy(N.savedfh, s + 5, fl);
            struct xdr x = { (u8 *)s + 5 + fl, (u8 *)s + 5 + fl + 8, 0 };
            N.savedfileid = x_u64(&x);
        }
    }
    /* the numbers the kernel holds after a restart: stale until closed (4.4 rule 5) */
    int stale = 0;
    for (int i = 0; c->opens && i < c->nopens; i++)
        if (c->opens[i] != 2) { node_stale_add(c->opens[i]); stale++; }
    if (stale) lg("started again: ", 0, stale, " open files on the mount can no longer be used (reopen them)");
    return 0;
nomem:
    nfs_end();
    return -ENOMEM;
}

void nfs_end(void)
{
    if (N.pieces) for (u32 i = 0; i < N.pf.npieces; i++) if (N.pieces[i].data) zfree(N.pieces[i].data, N.pieces[i].len);
    if (N.dc) for (u32 i = 0; i < N.pf.ndc; i++) if (N.dc[i].data) zfree(N.dc[i].data, N.rsize);
    if (N.blk) {
        for (u32 i = 0; i < N.pf.nnodes; i++) node_fh_free(&N.nodes[i]);
        zfree(N.blk, N.blksz);
    }
    memset(&N, 0, sizeof N);
}

/* ---- the mount ---- */
static void mount_fail(int err, int permanent)
{
    N.mstate = permanent ? MS_FAILED : MS_NONE;
    N.merr = err;
    ops_mounted();
}

int mount_kick(void)
{
    if (N.mstate == MS_UP) return 1;
    if (N.mstate == MS_FAILED) return N.merr ? N.merr : -EIO;
    if (N.mstate == MS_GOING) return 0;
    N.mstate = MS_GOING;
    int c = N.mport ? call_new(PG_MOUNT, MNT_MNT, -1, CP_MOUNT, 0) : call_new(PG_PMAP, PM_GETPORT, -1, CP_MOUNT, 0xFF);
    if (c < 0) { N.mstate = MS_NONE; return -ENOMEM; }
    return 0;
}

static void mount_next(int prog, int proc)
{
    int c = call_new(prog, proc, -1, CP_MOUNT, 0);
    if (c < 0) { mount_fail(-ENOMEM, 0); return; }
    if (prog == PG_NFS) N.calls[c].node = N.root;
}

void mount_reply(struct call *c, struct res *r)
{
    if (N.mstate != MS_GOING) return;
    if (c->prog == PG_PMAP) {
        if (r->err) { mount_fail(r->err, 0); return; }
        if (!r->port) {
            lg("the NAS's portmapper knows no MOUNT v3 over TCP: is NFS on?", 0, -1, 0);
            mount_fail(-ECONNREFUSED, 0);
            return;
        }
        N.mport = r->port;
        mount_next(PG_MOUNT, MNT_MNT);
        return;
    }
    if (c->prog == PG_MOUNT) {
        if (r->err) {
            if (r->status) lg("the NAS refused the export ", N.exp, (long)r->status, " (is it exported to this address?)");
            mount_fail(r->err, r->status != 0);
            return;
        }
        if (N.saved_ok && (N.savedfhlen != r->fhlen || memcmp(N.savedfh, r->fh, r->fhlen))) {
            lg("the NAS's export is not the one that was mounted: mount it again", 0, -1, 0);
            mount_fail(-EIO, 1);
            return;
        }
        if (N.root == N_NONE) {
            if (node_root(r->fh, r->fhlen) < 0) { mount_fail(-ENOMEM, 0); return; }
            memcpy(N.rootfh, r->fh, r->fhlen); N.rootfhlen = r->fhlen;
        }
        mount_next(PG_NFS, P3_FSINFO);
        return;
    }
    if (c->proc == P3_FSINFO) {
        if (r->has_attr) node_attr(N.root, &r->attr);
        if (!r->err) {
            u32 rs = N.pf.rsize, ws = N.pf.wsize;
            if (r->rtmax && r->rtmax < rs) rs = r->rtmax;
            if (r->rtpref >= 4096 && r->rtpref < rs) rs = r->rtpref;
            if (r->wtmax && r->wtmax < ws) ws = r->wtmax;
            if (r->wtpref >= 4096 && r->wtpref < ws) ws = r->wtpref;
            N.rsize = pow2_down(rs);
            N.wsize = ws < 1024 ? 1024 : ws;
            N.rshift = 0; while ((1u << N.rshift) < N.rsize) N.rshift++;
        }
        mount_next(PG_NFS, P3_GETATTR);
        return;
    }
    /* GETATTR of the root */
    if (r->err) { mount_fail(r->err, 0); return; }
    if (N.saved_ok && N.savedfileid != r->attr.fileid) {
        lg("the NAS's export is not the one that was mounted: mount it again", 0, -1, 0);
        mount_fail(-EIO, 1);
        return;
    }
    node_attr(N.root, &r->attr);
    if (!N.saved_ok) {                           /* kept for a restart (D11's saved table) */
        u8 b[5 + FHMAX + 8];
        memcpy(b, "NFS1", 4); b[4] = N.rootfhlen;
        memcpy(b + 5, N.rootfh, N.rootfhlen);
        struct xdr x = { b + 5 + N.rootfhlen, b + sizeof b, 0 };
        x_put64(&x, r->attr.fileid);
        nfs_save(b, 5u + N.rootfhlen + 8u);
        N.saved_ok = 1; N.savedfhlen = N.rootfhlen; memcpy(N.savedfh, N.rootfh, N.rootfhlen);
        N.savedfileid = r->attr.fileid;
        lg("mounted ", N.exp, -1, "");
    }
    N.mstate = MS_UP;
    ops_mounted();
}

void core_notanswering(int on)
{
    if (on && !N.notans) {
        N.notans = 1;
        lg(N.o.soft ? "the NAS is not answering (soft: requests fail after the retries)" :
                      "the NAS is not answering, still trying", 0, -1, 0);
        ops_notanswering();
    } else if (!on && N.notans) {
        N.notans = 0;
        lg("the NAS answers again", 0, -1, 0);
    }
}

void answer_ops_err(int err)
{
    for (u32 i = 0; i < N.pf.nops; i++)
        if (N.ops[i].used && N.ops[i].type != FS_QUIT) op_finish((int)i, err);
}

/* ---- the entry points ---- */
void nfs_request(const struct nfs_req *r)
{
    if (!N.ops) { nfs_answer(r->id, -EIO); return; }
    if (!(r->kflags & NFS_REQ_KERNEL) && r->uid != 0) { nfs_answer(r->id, -EACCES); goto out; }   /* 4.3 */
    if (r->type == FS_CLOSE) {                   /* answered at once, never waits for the network (7.3) */
        int n = node_find((u32)r->a[0]);
        if (n >= 0) { node_close((u16)n); wr_closed((u16)n); }
        nfs_answer(r->id, 0);
        goto out;
    }
    if (N.done || N.killed) { nfs_answer(r->id, -EIO); return; }
    int cancelled = (r->kflags & NFS_REQ_CANCEL) && !(r->type == FS_SYNC && r->a[0] == 1);   /* bounded: never cut */
    if (cancelled && r->type != FS_QUIT) { nfs_answer(r->id, -EINTR); return; }   /* a cancelled one still queued (5.2) */
    int o = -1;
    for (u32 i = 0; i < N.pf.nops; i++) if (!N.ops[i].used) { o = (int)i; break; }
    if (o < 0) { lg("too many requests at once (-EAGAIN)", 0, -1, 0); nfs_answer(r->id, -EAGAIN); return; }
    int cr = cred_get(r);
    if (cr < 0) { lg("too many different users at once (-ENOMEM)", 0, -1, 0); nfs_answer(r->id, -ENOMEM); return; }
    struct op *op = &N.ops[o];
    memset(op, 0, sizeof *op);
    op->used = 1;
    op->id = r->id; op->type = r->type;
    memcpy(op->a, r->a, sizeof op->a);
    op->uid = r->uid; op->gid = r->gid; op->umask = r->umask;
    op->cred = (u8)cr;
    if (r->kflags & NFS_REQ_LINKS) op->flags |= OPF_LINKS;
    op->node = op->node2 = N_NONE;
    op_start(o);
    if (cancelled && N.ops[o].used && N.ops[o].id == r->id) nfs_cancel(r->id);   /* FS_QUIT: quit, dropping the rest */
out:
    rpc_kick();
}

void nfs_input(const void *buf, long n)
{
    if (!N.ops) return;
    if (n > 0 && N.cstate == CS_UP) rpc_input(buf, n);
    else if (n <= 0) rpc_broken(n ? (int)n : -ECONNRESET);
    rpc_kick();
}

void nfs_tick(void)
{
    if (!N.ops || N.done) return;
    u32 now = nfs_now();
    rpc_timers(now);
    ops_timers(now);
    wr_timers(now);
    rpc_kick();
}

void nfs_cancel(u32 id)
{
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (!op->used || op->id != id) continue;
        if (op->type == FS_SYNC && op->a[0] == 1) break;    /* a bounded sync is never cut short (fsproto.h) */
        if (op->type == FS_WRITE) wr_cancel((int)i);
        else if (op->type == FS_QUIT) {          /* a cancelled umount drops the rest (7.4) */
            if (N.unsaved_bytes) lg("umount cancelled: ", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB not saved on the NAS");
            N.done = 1;
            nfs_disconnect();
            N.cstate = CS_DOWN;
            for (u32 k = 0; k < N.pf.nops; k++) if (N.ops[k].used && k != i) op_finish((int)k, -EIO);
            op_finish((int)i, -EINTR);
        } else op_finish((int)i, -EINTR);
        break;
    }
    rpc_kick();
}

void nfs_kill(void)
{
    if (!N.ops) return;
    int held = 0;
    for (u32 i = 0; i < N.pf.nops; i++) if (N.ops[i].used) held++;
    if (N.unsaved_bytes) lg("killed: ", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB not saved on the NAS");
    N.killed = 1;
    N.done = 1;
    for (u32 i = 0; i < N.pf.nops; i++) if (N.ops[i].used) op_finish((int)i, -EIO);
    if (N.cstate != CS_DOWN) nfs_disconnect();
    N.cstate = CS_DOWN;
    (void)held;
}

int nfs_poll(u32 *timeout_ms)
{
    int b = 0;
    u32 t = ~0u;
    if (!N.ops) { if (timeout_ms) *timeout_ms = t; return NFS_P_DONE; }
    if (N.done) b |= NFS_P_DONE;
    if (N.cstate == CS_UP && N.txhead < N.txtail) b |= NFS_P_OUT;
    if (N.cstate != CS_DOWN) b |= NFS_P_CONN;
    if (!N.done) {
        for (u32 i = 0; i < N.pf.nops; i++) if (!N.ops[i].used) { b |= NFS_P_TAKE; break; }
        u32 now = nfs_now(), d;
        if ((d = rpc_next(now)) < t) t = d;
        if ((d = ops_next(now)) < t) t = d;
        if ((d = wr_next(now)) < t) t = d;
    }
    if (timeout_ms) *timeout_ms = t;
    return b;
}

void nfs_get_stats(struct nfs_stats *s)
{
    *s = N.st;
    s->unsaved = N.unsaved_bytes;
    s->mem = N.mem;
}
