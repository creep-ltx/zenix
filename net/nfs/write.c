/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: writes and their safety (DESIGN D23 7). FS_WRITE copies the program's bytes into the unsaved buffer
 * as wsize pieces, each sent as an UNSTABLE WRITE (FILE_SYNC with the code y) with the writer's credentials; the
 * request is answered with the full count once every piece's WRITE was answered (short only on a real error or a
 * cancel). A piece stays until a COMMIT sent after its WRITE's answer returns the same write verifier; a verifier
 * that changed means the NAS restarted: the pieces are written again and committed again. Commits: 5 s after a
 * file's last write, at its last close, at FS_SYNC and FS_QUIT, and when the buffer is full. A piece never goes out
 * while an older piece of the same file over the same bytes is still out (the NAS may run calls in any order).
 */
#include "nfs_int.h"

void dc_drop_range(u16 node, u32 off, u32 n);
void dc_restamp(u16 node, u32 octime, u32 octime_ns, u64 osize);

static int have_verf;
static u8 verf[8];

void wr_reset(void) { have_verf = 0; }

static int waiters(void)                         /* a SYNC or QUIT waits: commits go at once */
{
    for (u32 i = 0; i < N.pf.nops; i++)
        if (N.ops[i].used && (N.ops[i].type == FS_SYNC || N.ops[i].type == FS_QUIT)) return 1;
    return 0;
}

int wr_node_busy(u16 node)
{
    int n = 0;
    for (u32 i = 0; i < N.pf.npieces; i++) if (N.pieces[i].state == PS_SEND && N.pieces[i].node == node) n++;
    return n;
}

int piece_may_send(struct call *c)
{
    struct piece *p = &N.pieces[c->piece];
    for (u32 i = 0; i < N.pf.npieces; i++) {
        struct piece *q = &N.pieces[i];
        if (q == p || q->state != PS_SEND || q->node != p->node || (s32)(q->cseq - p->cseq) >= 0) continue;
        if (q->off < p->off + p->len && p->off < q->off + q->len) return 0;   /* an older one over the same bytes */
    }
    return 1;
}

static void piece_free(int i)
{
    struct piece *p = &N.pieces[i];
    if (!p->state) return;
    struct node *n = nd(p->node);
    if (n && n->unsaved) n->unsaved--;
    N.unsaved_bytes -= p->len;
    zfree(p->data, p->len);
    p->data = 0;
    cred_put(p->cred);
    p->state = 0;
    for (u32 k = 0; k < N.pf.ncalls; k++)        /* a call for it still waiting (a resend) goes too */
        if (N.calls[k].state != C_FREE && N.calls[k].purpose == CP_PIECE && N.calls[k].piece == i &&
            N.calls[k].state != C_SENT) call_free((int)k);
        else if (N.calls[k].state == C_SENT && N.calls[k].purpose == CP_PIECE && N.calls[k].piece == i) N.calls[k].piece = -1;
}

static int piece_send(int i)
{
    struct piece *p = &N.pieces[i];
    int c = call_new(PG_NFS, P3_WRITE, -1, CP_PIECE, p->cred);
    if (c < 0) { p->resend = 1; return -1; }
    p->resend = 0;
    p->state = PS_SEND;
    N.calls[c].piece = (s16)i;
    N.calls[c].node = p->node;
    N.calls[c].count = p->len;
    N.calls[c].fsync = (u8)N.o.sync;
    return 0;
}

/* the request: answered when every byte copied was answered */
static void wr_check(int o)
{
    struct op *op = &N.ops[o];
    if (!op->used || op->type != FS_WRITE) return;
    u32 len = (u32)op->a[3];
    if (op->wlen < len && op->werr_at == 0xFFFFFFFFu) return;   /* still copying */
    if (op->wok + (u32)op->done < op->wlen) return;             /* pieces out */
    if (op->werr_at != 0xFFFFFFFFu) { op_finish(o, op->werr_at ? (long)op->werr_at : (op->err ? op->err : -EIO)); return; }
    op_finish(o, (long)op->wlen);
}

static void wr_fill(int o)
{
    struct op *op = &N.ops[o];
    u32 len = (u32)op->a[3], off = (u32)op->a[1];
    while (op->wlen < len && op->werr_at == 0xFFFFFFFFu) {
        u32 n = len - op->wlen;
        if (n > N.wsize) n = N.wsize;
        u32 room = N.pf.cap > N.unsaved_bytes ? N.pf.cap - N.unsaved_bytes : 0;
        if (room < n) n = room >= 1024 ? room : 0;
        int slot = -1;
        for (u32 i = 0; n && i < N.pf.npieces; i++) if (!N.pieces[i].state) { slot = (int)i; break; }
        u8 *data = slot >= 0 ? zalloc(n) : 0;
        if (!data) {
            if (!N.unsaved_bytes) {              /* nothing to wait for: the memory is short */
                lg("no memory for written data (-ENOMEM)", 0, -1, 0);
                if (!op->wlen) op->err = -ENOMEM;
                op->werr_at = op->wlen;
                break;
            }
            op->flags |= OPF_WAITROOM;           /* room comes as the commits are answered (7.1) */
            wr_commit_all(0);
            return;
        }
        struct piece *p = &N.pieces[slot];
        memset(p, 0, sizeof *p);
        p->state = PS_SEND;
        p->node = op->node; p->off = off + op->wlen; p->len = n;
        p->data = data;
        memcpy(data, (const u8 *)op->a[2] + op->wlen, n);
        p->cred = op->cred; N.creds[op->cred].refs++;
        p->op = (s16)o;
        p->opoff = op->wlen;
        p->cseq = N.seq++;
        N.nodes[op->node].unsaved++;
        N.unsaved_bytes += n;
        dc_drop_range(op->node, p->off, n);
        piece_send(slot);
        op->wlen += n;
    }
    wr_check(o);
}

int wr_start(int o)
{
    struct op *op = &N.ops[o];
    if (op->step == 0) {
        int n = node_find((u32)op->a[0]);
        if (n < 0 || (N.nodes[n].flags & NF_STALE)) { op_finish(o, -NFS_ESTALE); return 0; }
        struct node *p = &N.nodes[n];
        if ((p->flags & NF_ATTR) && p->a.type != NF3REG) { op_finish(o, p->a.type == NF3DIR ? -EISDIR : -EINVAL); return 0; }
        if (!op->a[3]) { op_finish(o, 0); return 0; }
        if ((u64)(u32)op->a[1] + (u32)op->a[3] > 0xFFFFFFFFull) { op_finish(o, -EFBIG); return 0; }
        op->node = (u16)n;
        node_pin(op->node); op->pins[op->npins++] = op->node;
        op->wlen = op->wok = 0; op->done = 0; op->werr_at = 0xFFFFFFFFu;
        op->step = 1;
    }
    wr_fill(o);
    return 0;
}

void wr_cancel(int o)
{
    struct op *op = &N.ops[o];
    op_finish(o, op->wlen ? (long)op->wlen : -EINTR);   /* copied bytes reach the NAS anyway (5.2) */
}

/* a changed verifier: the NAS restarted and may have lost what was not committed (7.1) */
static void verf_seen(const u8 *v)
{
    if (have_verf && !memcmp(verf, v, 8)) return;
    int again = 0;
    if (have_verf) {
        for (u32 i = 0; i < N.pf.npieces; i++) {
            struct piece *p = &N.pieces[i];
            if (p->state == PS_WRITTEN && memcmp(p->verf, v, 8)) { piece_send((int)i); again++; }
        }
        if (again) lg("the NAS restarted: unsaved data sent again (", 0, again, " pieces)");
    }
    memcpy(verf, v, 8);
    have_verf = 1;
}

void wr_piece_reply(struct call *c, struct res *r)
{
    if (c->piece < 0) return;
    int i = c->piece;
    struct piece *p = &N.pieces[i];
    if (!p->state) return;
    int o = p->op;
    struct op *op = o >= 0 ? &N.ops[o] : 0;
    if (!r->err && r->count != p->len) { lg("a short write from the NAS (-EIO)", 0, -1, 0); r->err = -EIO; }
    struct node *n = nd(p->node);
    if (n && (r->has_dattr || r->has_pre)) {
        u32 oc = n->a.ctime, ocn = n->a.ctime_ns; u64 os = n->a.size;
        int ours = r->has_pre && r->pre_ctime == oc && r->pre_ctime_ns == ocn && r->pre_size == os;
        node_wcc(p->node, r, 1);
        if (ours) dc_restamp(p->node, oc, ocn, os);
    }
    if (r->err) {
        if (!op) lg("written data could not be saved: error ", 0, -r->err, " from the NAS");
        else {
            op->done += (long)p->len;
            if (p->opoff < op->werr_at) op->werr_at = p->opoff;
            op->err = r->err;
        }
        p->op = -1;
        piece_free(i);
        if (op) wr_check(o);
        ops_poke();
        return;
    }
    verf_seen(r->verf);
    if (r->committed == 2) piece_free(i);        /* FILE_SYNC: on the NAS's disk */
    else {
        p->state = PS_WRITTEN;
        memcpy(p->verf, r->verf, 8);
        p->wseq = ++N.wseq;
        if (n) {
            u32 t = nfs_now() + (n->opens == 0 || waiters() ? 0 : 5000);   /* 5 s after the last write (7.2) */
            n->commit_at = t ? t : 1;
        }
    }
    if (op) {
        op->wok += p->len;
        p->op = -1;
        wr_check(o);
    }
    ops_poke();
}

static void commit_node(u16 node)
{
    struct node *n = nd(node);
    if (!n || n->cin) return;
    int first = -1;
    for (u32 i = 0; i < N.pf.npieces; i++)
        if (N.pieces[i].state == PS_WRITTEN && N.pieces[i].node == node) { first = (int)i; break; }
    n->commit_at = 0;
    if (first < 0) return;
    int c = call_new(PG_NFS, P3_COMMIT, -1, CP_COMMIT, N.pieces[first].cred);   /* the writer's credentials */
    if (c < 0) { n->commit_at = nfs_now() + 1000; if (!n->commit_at) n->commit_at = 1; return; }
    N.calls[c].node = node;
    N.calls[c].cseq = N.wseq;
    n->cin = 1;
}

void wr_commit_reply(struct call *c, struct res *r)
{
    struct node *n = nd(c->node);
    if (!n) return;
    n->cin = 0;
    node_wcc(c->node, r, 1);
    if (r->err) {
        u32 lost = 0;
        for (u32 i = 0; i < N.pf.npieces; i++) {
            struct piece *p = &N.pieces[i];
            if (p->state == PS_WRITTEN && p->node == c->node && (s32)(p->wseq - c->cseq) <= 0) { lost += (int)p->len; piece_free((int)i); }
        }
        if (lost) lg("written data may not be saved: the NAS answered the commit with error ", 0, -r->err, "");
        ops_poke();
        return;
    }
    verf_seen(r->verf);
    int more = 0;
    for (u32 i = 0; i < N.pf.npieces; i++) {
        struct piece *p = &N.pieces[i];
        if (p->state != PS_WRITTEN || p->node != c->node) continue;
        if ((s32)(p->wseq - c->cseq) <= 0 && !memcmp(p->verf, r->verf, 8)) piece_free((int)i);
        else more = 1;
    }
    if (more && !n->commit_at) {
        u32 t = nfs_now() + (n->opens == 0 || waiters() ? 0 : 5000);
        n->commit_at = t ? t : 1;
    }
    ops_poke();
}

int wr_commit_all(int bounded)
{
    (void)bounded;
    int left = 0;
    for (u32 i = 0; i < N.pf.npieces; i++) {
        struct piece *p = &N.pieces[i];
        if (!p->state) continue;
        left++;
        if (p->state == PS_WRITTEN) commit_node(p->node);
    }
    return left;
}

void wr_drop_node(u16 node)
{
    for (u32 i = 0; i < N.pf.npieces; i++)
        if (N.pieces[i].state == PS_WRITTEN && N.pieces[i].node == node) piece_free((int)i);
    struct node *n = nd(node);
    if (n) n->commit_at = 0;
}

void wr_room(void) { ops_poke(); }

int wr_resend_all(void)
{
    int k = 0;
    for (u32 i = 0; i < N.pf.npieces; i++) if (N.pieces[i].state == PS_WRITTEN) { piece_send((int)i); k++; }
    return k;
}

void wr_timers(u32 now)
{
    for (u32 i = 0; i < N.pf.npieces; i++) {
        struct piece *p = &N.pieces[i];
        if (p->state == PS_SEND && p->resend) piece_send((int)i);
        if (p->state != PS_WRITTEN) continue;
        struct node *n = nd(p->node);
        if (n && n->commit_at && due(n->commit_at, now)) commit_node(p->node);
    }
}

u32 wr_next(u32 now)
{
    u32 best = ~0u;
    for (u32 i = 0; i < N.pf.npieces; i++) {
        struct piece *p = &N.pieces[i];
        if (p->state == PS_SEND && p->resend) return 100;
        if (p->state != PS_WRITTEN) continue;
        struct node *n = nd(p->node);
        if (!n || !n->commit_at) continue;
        u32 d = due(n->commit_at, now) ? 0 : tdiff(n->commit_at, now);
        if (d < best) best = d;
    }
    return best;
}

/* the last close of a file: its data is committed in the background (7.3) */
void wr_closed(u16 node)
{
    struct node *n = nd(node);
    if (n && n->unsaved && !n->opens) { u32 t = nfs_now(); n->commit_at = t ? t : 1; }
}
