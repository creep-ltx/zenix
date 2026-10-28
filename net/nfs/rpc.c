/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: XDR (RFC 4506), ONC RPC calls and replies with AUTH_SYS (RFC 5531), record marking over TCP with
 * fragments, the calls in flight matched by xid, the one connection and its reconnects (DESIGN D23 3.4, 5.3).
 *
 * Rules (3.4): every call has its xid (random start, then counting); calls go out without waiting for earlier
 * answers; a connection that breaks (reset, closed, or a call without an answer for timeo) is closed by abort and
 * made again after 1, 2, 4 ... 30 s (the OS layer binds the same source port), and every unanswered call is sent
 * again with its old xid, so the NAS's duplicate request cache can answer it. A call that was out on two connections
 * that broke, or whose answer was a record over the receive buffer, fails with -EIO and a log line. hard: a call is
 * tried until answered; soft: it fails with -EIO after retrans timeouts.
 */
#include "nfs_int.h"

/* ---- XDR ---- */
u32 x_u32(struct xdr *x)
{
    if (x->bad || x->end - x->p < 4) { x->bad = 1; return 0; }
    u32 v = (u32)x->p[0] << 24 | (u32)x->p[1] << 16 | (u32)x->p[2] << 8 | x->p[3];
    x->p += 4;
    return v;
}
u64 x_u64(struct xdr *x)
{
    u32 hi = x_u32(x);
    return (u64)hi << 32 | x_u32(x);
}
const u8 *x_fixed(struct xdr *x, u32 n)
{
    u32 pad = (n + 3) & ~3u;
    if (x->bad || pad < n || (u32)(x->end - x->p) < pad) { x->bad = 1; return 0; }
    const u8 *p = x->p;
    x->p += pad;
    return p;
}
const u8 *x_opaque(struct xdr *x, u32 max, u32 *len)
{
    u32 n = x_u32(x);
    if (n > max) { x->bad = 1; return 0; }
    *len = n;
    return x_fixed(x, n);
}
void x_put32(struct xdr *x, u32 v)
{
    if (x->bad || x->end - x->p < 4) { x->bad = 1; return; }
    x->p[0] = (u8)(v >> 24); x->p[1] = (u8)(v >> 16); x->p[2] = (u8)(v >> 8); x->p[3] = (u8)v;
    x->p += 4;
}
void x_put64(struct xdr *x, u64 v) { x_put32(x, (u32)(v >> 32)); x_put32(x, (u32)v); }
void x_putfixed(struct xdr *x, const void *b, u32 n)
{
    u32 pad = (n + 3) & ~3u;
    if (x->bad || (u32)(x->end - x->p) < pad) { x->bad = 1; return; }
    memcpy(x->p, b, n);
    memset(x->p + n, 0, pad - n);
    x->p += pad;
}
void x_putopaque(struct xdr *x, const void *b, u32 n) { x_put32(x, n); x_putfixed(x, b, n); }

void call_done(int i, struct res *r);
int next_wait_port(int port);
int piece_may_send(struct call *c);              /* write.c: no older overlapping piece of the file is out */

/* ---- the calls ---- */
static const u32 progs[3][2] = { { PMAP_PROG, 2 }, { MOUNT_PROG, 3 }, { NFS_PROG, 3 } };

int call_new(int prog, int proc, int op, int purpose, int cred)
{
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        if (c->state != C_FREE) continue;
        memset(c, 0, sizeof *c);
        c->state = C_WAIT;
        c->prog = (u8)prog; c->proc = (u8)proc; c->op = (s16)op; c->purpose = (u8)purpose;
        c->piece = -1; c->node = c->node2 = N_NONE;
        c->cred = (u8)cred;
        if (cred != 0xFF) N.creds[cred].refs++;
        c->xid = N.xid++;
        c->seq = N.seq++;
        c->deadline = nfs_now() + N.o.timeo_ms;
        if (op >= 0) { N.ops[op].inflight++; c->opref = 1; }
        N.st.calls++;
        return (int)i;
    }
    return -1;
}

void call_free(int i)
{
    struct call *c = &N.calls[i];
    if (c->state == C_FREE) return;
    if (c->opref && c->op >= 0 && N.ops[c->op].inflight) N.ops[c->op].inflight--;
    c->opref = 0;
    if (c->cred != 0xFF) cred_put(c->cred);
    c->state = C_FREE;
}

int rpc_busy(int prog)
{
    for (u32 i = 0; i < N.pf.ncalls; i++)
        if (N.calls[i].state != C_FREE && (prog < 0 || N.calls[i].prog == prog)) return 1;
    return 0;
}

/* the op ended (answered or cancelled): calls not yet out are dropped, those out are kept to swallow their answer
 * (the link to the request is cut, 4.3) */
void rpc_drop_op(int op)
{
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        if (c->state == C_FREE || c->op != op) continue;
        if (c->state == C_SENT) { if (c->opref) N.ops[op].inflight--; c->opref = 0; c->op = -1; }
        else call_free((int)i);
    }
}

/* ---- encoding and sending ---- */
static void put_cred(struct xdr *x, int cred)
{
    static const char host[] = "zenix";             /* servers only log it (3.3) */
    if (cred == 0xFF) { x_put32(x, 0); x_put32(x, 0); return; }   /* AUTH_NONE */
    struct cred *cr = &N.creds[cred];
    x_put32(x, 1);                                  /* AUTH_SYS */
    x_put32(x, 4 + 4 + 8 + 4 + 4 + 4 + 4 * cr->n);
    x_put32(x, 0);                                  /* stamp */
    x_putopaque(x, host, 5);
    x_put32(x, cr->uid); x_put32(x, cr->gid);
    x_put32(x, cr->n);
    for (u32 i = 0; i < cr->n; i++) x_put32(x, cr->g[i]);
}

static int port_of(int prog)
{
    return prog == PG_PMAP ? PMAP_PORT : prog == PG_MOUNT ? (int)N.mport : NFS_PORT;
}

/* the waiting call that goes first (the oldest), or -1; only those for the port connected (or any when down) */
static int next_wait(int prog)
{
    int best = -1;
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        if (c->state != C_WAIT || (prog >= 0 && c->prog != prog)) continue;
        if (best < 0 || (s32)(c->seq - N.calls[best].seq) < 0) best = (int)i;
    }
    return best;
}


static void fail_call(int i, int err)
{
    struct res r;
    memset(&r, 0, sizeof r);
    r.err = err;
    call_done(i, &r);
}

static void flush(void)
{
    while (N.cstate == CS_UP && N.txhead < N.txtail) {
        long n = nfs_send(N.tx + N.txhead, (long)(N.txtail - N.txhead));
        if (n < 0) { rpc_broken((int)n); return; }
        if (n == 0) return;
        N.txhead += (u32)n;
    }
    if (N.txhead == N.txtail) N.txhead = N.txtail = 0;
}

static void encode_waiting(void)
{
    u32 now = nfs_now();
    for (;;) {
        int best = -1;
        for (u32 i = 0; i < N.pf.ncalls; i++) {
            struct call *c = &N.calls[i];
            if (c->state != C_WAIT || c->prog != (N.cport == PMAP_PORT ? PG_PMAP : N.cport == NFS_PORT ? PG_NFS : PG_MOUNT))
                continue;
            if (c->purpose == CP_PIECE && !piece_may_send(c)) continue;
            if (best < 0 || (s32)(c->seq - N.calls[best].seq) < 0) best = (int)i;
        }
        if (best < 0) return;
        struct call *c = &N.calls[best];
        if (N.txhead) {                          /* room at the end: move what is left to the front */
            memmove(N.tx, N.tx + N.txhead, N.txtail - N.txhead);
            N.txtail -= N.txhead; N.txhead = 0;
        }
        struct xdr x = { N.tx + N.txtail + 4, N.tx + N.txcap, 0 };
        x_put32(&x, c->xid);
        x_put32(&x, 0);                          /* CALL */
        x_put32(&x, 2);                          /* RPC version 2 */
        x_put32(&x, progs[c->prog][0]);
        x_put32(&x, progs[c->prog][1]);
        x_put32(&x, c->proc);
        put_cred(&x, c->prog == PG_PMAP ? 0xFF : c->cred);
        x_put32(&x, 0); x_put32(&x, 0);          /* verifier AUTH_NONE */
        int e = enc_args(c, &x);
        if (e < 0) { fail_call(best, e); continue; }
        if (x.bad) {
            if (N.txtail == 0) { lg("a request too big for the send buffer", 0, -1, 0); fail_call(best, -EIO); continue; }
            return;                              /* wait until the socket took more */
        }
        u32 len = (u32)(x.p - (N.tx + N.txtail + 4));
        u8 *h = N.tx + N.txtail;
        h[0] = (u8)(0x80 | len >> 24); h[1] = (u8)(len >> 16); h[2] = (u8)(len >> 8); h[3] = (u8)len;
        N.txtail += 4 + len;
        if (c->tmo || c->brk) N.st.resends++;
        c->state = C_SENT;
        c->sent_at = now;
        if (!c->tmo) c->deadline = now + N.o.timeo_ms;
        flush();
        if (N.cstate != CS_UP) return;
    }
}

static void connect_to(int port)
{
    N.cport = port;
    N.cstate = CS_CONNECTING;
    N.cstart = nfs_now();
    N.rxn = 0; N.hdrn = 0; N.fraglen = 0; N.skipping = 0; N.last = 0;
    N.txhead = N.txtail = 0;
    N.st.reconnects++;
    int r = nfs_connect(port);
    if (r < 0) nfs_connected(r);
}

void rpc_kick(void)
{
    if (N.done) return;
    if (N.cstate == CS_UP) {
        int w = next_wait(-1), out = 0;
        for (u32 i = 0; i < N.pf.ncalls; i++) if (N.calls[i].state == C_SENT) out = 1;
        /* the oldest waiting call is for another program: change connection once nothing is out on this one */
        if (w >= 0 && port_of(N.calls[w].prog) != N.cport && !out && next_wait_port(N.cport) < 0) {
            nfs_disconnect();
            N.cstate = CS_DOWN;
        } else if (!out && w < 0 && N.cport != NFS_PORT) {
            nfs_disconnect();                    /* the portmapper's and mountd's connections only while used */
            N.cstate = CS_DOWN;
            return;
        } else {
            encode_waiting();
            flush();
            return;
        }
    }
    if (N.cstate != CS_DOWN) return;
    int w = next_wait(-1);
    if (w < 0) return;
    if (N.retry_at && !due(N.retry_at, nfs_now())) return;
    int port = port_of(N.calls[w].prog);
    if (!port) { fail_call(w, -EIO); return; }
    N.retry_at = 0;
    connect_to(port);
}

/* a waiting call for the program on this port */
int next_wait_port(int port)
{
    int prog = port == PMAP_PORT ? PG_PMAP : port == NFS_PORT ? PG_NFS : PG_MOUNT;
    return next_wait(prog);
}

void nfs_connected(int err)
{
    if (N.cstate != CS_CONNECTING) return;
    if (err == 0) {
        N.cstate = CS_UP;
        rpc_kick();
        return;
    }
    nfs_disconnect();
    N.cstate = CS_DOWN;
    N.backoff = N.backoff ? (N.backoff * 2 > 30000 ? 30000 : N.backoff * 2) : 1000;
    N.retry_at = nfs_now() + N.backoff;
    if (!N.retry_at) N.retry_at = 1;
    if (!N.notans) lg("no answer on port ", 0, N.cport, ": is NFS on, is a firewall in the way?");
    core_notanswering(1);
}

/* the connection broke: everything out goes again on the next one (with its old xid) */
static void broken(int counts)
{
    nfs_disconnect();
    int out = 0;
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        if (c->state != C_SENT) continue;
        out = 1;
        if (c->op < 0 && c->purpose < CP_MOUNT) { call_free((int)i); continue; }   /* nobody waits for it */
        if (counts && ++c->brk >= 2) {
            lg("a request broke the connection twice: given up (-EIO)", 0, -1, 0);
            fail_call((int)i, -EIO);
            continue;
        }
        c->state = C_WAIT;
    }
    N.cstate = CS_DOWN;
    N.txhead = N.txtail = 0;
    N.rxn = 0; N.hdrn = 0; N.fraglen = 0; N.skipping = 0;
    if (out || rpc_busy(-1)) {
        N.backoff = N.backoff ? (N.backoff * 2 > 30000 ? 30000 : N.backoff * 2) : 1000;
        N.retry_at = nfs_now() + N.backoff;
        if (!N.retry_at) N.retry_at = 1;
    }
}

void rpc_broken(int err)
{
    (void)err;
    if (N.cstate == CS_DOWN) return;
    broken(N.cstate == CS_UP);
}

/* ---- receiving ---- */
void call_done(int i, struct res *r)
{
    struct call *c = &N.calls[i];
    if (r->status == NFS3ERR_JUKEBOX && c->prog == PG_NFS) {   /* "try later": 1, 2, 4 s, never passed on (4.5) */
        c->state = C_LATER;
        c->jb_at = nfs_now() + (1000u << (c->jb < 2 ? c->jb : 2));
        if (c->jb < 200) c->jb++;
        c->xid = N.xid++;                        /* a new call: the old xid's answer is in the NAS's cache */
        c->deadline = c->jb_at + N.o.timeo_ms;
        return;
    }
    if (c->prog == PG_NFS && (r->status == NFS3ERR_STALE || r->status == NFS3ERR_BADHANDLE) && c->node != N_NONE)
        node_staled(c->node);                    /* its handle is gone: names and data cached for it go too */
    if (c->opref && c->op >= 0 && N.ops[c->op].inflight) N.ops[c->op].inflight--;   /* answered: the op may go on */
    c->opref = 0;
    switch (c->purpose) {
    case CP_MOUNT: mount_reply(c, r); break;
    case CP_UMNT: umount_reply(c, r); break;
    case CP_PIECE: wr_piece_reply(c, r); break;
    case CP_COMMIT: wr_commit_reply(c, r); break;
    default:
        if (c->op >= 0) op_reply(c->op, c, r);
        else N.st.dropped_late++;
    }
    call_free(i);
}

static void record(u8 *b, u32 n)
{
    struct xdr x = { b, b + n, 0 };
    u32 xid = x_u32(&x);
    int i;
    for (i = 0; i < (int)N.pf.ncalls; i++)
        if (N.calls[i].state == C_SENT && N.calls[i].xid == xid) break;
    if (i == (int)N.pf.ncalls) { N.st.dropped_late++; return; }   /* late (cancelled), or a duplicate */
    struct call *c = &N.calls[i];
    struct res r;
    memset(&r, 0, sizeof r);
    N.st.replies++;
    N.backoff = 0;
    if (N.notans) core_notanswering(0);
    u32 mtype = x_u32(&x), stat = x_u32(&x);
    if (x.bad || mtype != 1 || stat > 1) { lg("a malformed RPC reply (-EIO)", 0, -1, 0); r.err = -EIO; }
    else if (stat == 0) {                             /* MSG_ACCEPTED */
        u32 vlen;
        x_u32(&x);
        x_opaque(&x, 400, &vlen);
        u32 acc = x_u32(&x);
        if (x.bad) { lg("a malformed RPC reply", 0, -1, 0); r.err = -EIO; }
        else if (acc != 0) {
            lg("the NAS did not take a call (RPC accept status ", 0, (long)acc, ")");
            r.err = -EIO;
        } else if (dec_res(c, &x, &r) < 0 || x.bad) {
            lg("a malformed reply (procedure ", 0, c->proc, "): -EIO");
            memset(&r, 0, sizeof r);
            r.err = -EIO;
        }
    } else {                                     /* MSG_DENIED: RPC_MISMATCH or AUTH_ERROR */
        u32 why = x_u32(&x), what = x_u32(&x);
        if (why == 1) lg("the NAS refused our credentials (auth error ", 0, (long)what,
                         "): a privileged port or the export's insecure option?");
        else lg("the NAS refused the call (RPC version)", 0, -1, 0);
        r.err = -EACCES;
    }
    call_done(i, &r);
}

void rpc_input(const u8 *b, long n)
{
    while (n > 0) {
        if (N.hdrn < 4) {                        /* a fragment's header: the last bit and 31 bits of length */
            N.hdr[N.hdrn++] = *b++; n--;
            if (N.hdrn < 4) continue;
            N.last = N.hdr[0] >> 7;
            N.fraglen = (u32)(N.hdr[0] & 0x7F) << 24 | (u32)N.hdr[1] << 16 | (u32)N.hdr[2] << 8 | N.hdr[3];
            if (!N.skipping && N.rxn + N.fraglen > N.rxcap) {
                if (N.rxn + N.fraglen > (4u << 20) || N.rxn + N.fraglen < N.rxn) {   /* not a sane record: */
                    lg("the NAS sent a record of an impossible size: connection closed", 0, -1, 0);
                    broken(1);
                    return;
                }
                N.skipping = 1;                  /* too big for the buffer: skipped, its call fails (3.4) */
                N.skipxid = N.rxn >= 4 ? (u32)N.rx[0] << 24 | (u32)N.rx[1] << 16 | (u32)N.rx[2] << 8 | N.rx[3] : 0;
                N.skipped = N.rxn;
            }
            if (N.fraglen) continue;
        } else {
            u32 take = N.fraglen < (u32)n ? N.fraglen : (u32)n;
            if (N.skipping) {
                if (N.skipped < 4) {             /* the xid may still be in the first bytes */
                    for (u32 k = 0; k < take && N.skipped + k < 4; k++)
                        N.skipxid = N.skipxid << 8 | b[k];
                }
                N.skipped += take;
            } else {
                memcpy(N.rx + N.rxn, b, take);
                N.rxn += take;
            }
            b += take; n -= (long)take; N.fraglen -= take;
            if (N.fraglen) continue;
        }
        N.hdrn = 0;
        if (!N.last) continue;                   /* more fragments of the same record */
        if (N.skipping) {
            N.skipping = 0;
            for (u32 i = 0; i < N.pf.ncalls; i++) {
                struct call *c = &N.calls[i];
                if (c->state == C_SENT && c->xid == N.skipxid) {
                    lg("an answer bigger than the receive buffer: the request fails (-EIO)", 0, -1, 0);
                    fail_call((int)i, -EIO);
                    break;
                }
            }
        } else if (N.rxn) {
            record(N.rx, N.rxn);
        }
        N.rxn = 0;
        if (N.cstate != CS_UP) return;           /* (an answer's handling may have closed it) */
    }
}

/* ---- timers ---- */
void rpc_timers(u32 now)
{
    int abort = 0;
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        if (c->state == C_LATER) {
            if (due(c->jb_at, now)) c->state = C_WAIT;
            continue;
        }
        if (c->state != C_WAIT && c->state != C_SENT) continue;
        if (!due(c->deadline, now)) continue;
        c->deadline = now + N.o.timeo_ms;
        N.st.timeouts++;
        if (c->tmo < 255) c->tmo++;
        if (c->op < 0 && c->purpose < CP_MOUNT) { call_free((int)i); continue; }   /* an answer nobody waits for */
        core_notanswering(1);
        if (N.o.soft && c->tmo >= N.o.retrans) {
            lg("a request timed out (soft): -EIO", 0, -1, 0);
            if (c->state == C_SENT) abort = 1;
            fail_call((int)i, -EIO);
            continue;
        }
        if (c->state == C_SENT) abort = 1;
    }
    if (abort && N.cstate == CS_UP) broken(0);  /* no answer within timeo: the connection is made again */
    if (N.cstate == CS_CONNECTING && tdiff(now, N.cstart) >= 10000) nfs_connected(-ETIMEDOUT);
}

u32 rpc_next(u32 now)
{
    u32 best = ~0u;
    for (u32 i = 0; i < N.pf.ncalls; i++) {
        struct call *c = &N.calls[i];
        u32 t;
        if (c->state == C_LATER) t = c->jb_at;
        else if (c->state == C_WAIT || c->state == C_SENT) t = c->deadline;
        else continue;
        u32 d = due(t, now) ? 0 : tdiff(t, now);
        if (d < best) best = d;
    }
    if (N.cstate == CS_DOWN && N.retry_at && rpc_busy(-1)) {
        u32 d = due(N.retry_at, now) ? 0 : tdiff(N.retry_at, now);
        if (d < best) best = d;
    }
    if (N.cstate == CS_CONNECTING) {
        u32 d = tdiff(now, N.cstart) >= 10000 ? 0 : 10000 - tdiff(now, N.cstart);
        if (d < best) best = d;
    }
    return best;
}
