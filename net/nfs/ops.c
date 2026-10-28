/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core: the Zenix requests (kernel/fsproto.h) as small state machines (DESIGN D23 4.3, 4.5). No request waits
 * inside the core for another: each one issues its calls and goes on when their answers come. The nodes it depends
 * on are pinned until it ends; its link to the message is cut when it is answered.
 *
 * The path walk: LOOKUP per name not in the name cache (a cached name is used when the directory's search right is
 * known for the caller), symbolic links followed by READLINK at most 8 deep, absolute targets from the export's root,
 * ".." never above it, a link target plus the rest of the path at most FS_PATHMAX.
 */
#include "nfs_int.h"

void dc_drop_range(u16 node, u32 off, u32 n);
int node_root(const u8 *fh, u32 n);
void node_close(u16 i);

/* the purposes of an op's calls */
enum { PU_LOOKUP = 1, PU_WREADLINK, PU_WACCESS, PU_GETATTR, PU_ACCESS, PU_CREATE, PU_LOOKUP2, PU_SETATTR, PU_TRUNC,
       PU_READ, PU_READLINK, PU_MKDIR, PU_REMOVE, PU_RMDIR, PU_RENAME, PU_LINK, PU_SYMLINK, PU_READDIR, PU_FSSTAT,
       PU_SATTR };

#define ST_DONE 255

static struct res *R;                            /* the answer being handled (op_reply) */

/* ---- small helpers ---- */
static int issue(int o, int proc, u16 node, int purpose)
{
    struct op *op = &N.ops[o];
    int c = call_new(PG_NFS, proc, o, purpose, op->cred);
    if (c < 0) {
        lg("too many calls at once: a request fails (-ENOMEM)", 0, -1, 0);
        op->err = -ENOMEM;
        return -1;
    }
    N.calls[c].node = node;
    return c;
}

static void pin(struct op *op, u16 n)
{
    if (op->npins < 6) { op->pins[op->npins++] = n; node_pin(n); }
}
static void unpin(struct op *op, u16 n)
{
    for (int i = 0; i < op->npins; i++)
        if (op->pins[i] == n) { node_unpin(n); op->pins[i] = op->pins[--op->npins]; return; }
}

void op_finish(int o, long result)
{
    struct op *op = &N.ops[o];
    if (!op->used) return;
    rpc_drop_op(o);
    for (u32 i = 0; i < N.pf.npieces; i++) if (N.pieces[i].op == o) N.pieces[i].op = -1;
    while (op->npins) unpin(op, op->pins[0]);
    if (op->cred != 0xFF) cred_put(op->cred);
    op->used = 0;
    N.st.ops++;
    nfs_answer(op->id, result);                  /* the link to the message is cut here (4.3) */
}

static u32 acc_want(u32 flags, int dir)
{
    if (dir) return AC_READ;
    u32 w = 0;
    if ((flags & 3) == 0 || (flags & 3) == 2) w |= AC_READ;
    if (flags & 3) w |= AC_MODIFY;
    if (flags & O_EXEC) w |= AC_EXECUTE;
    return w;
}

/* the cached ACCESS answer for this caller: 1 has it, 0 asks */
static u16 cred_key(const struct op *op) { return (u16)(op->cred | N.creds[op->cred].gen << 5); }
/* the cached ACCESS answer for this caller's credentials (uid, gid and groups): 1 has it, 0 asks */
static int acc_known(u16 n, u16 key)
{
    struct node *p = nd(n);
    return p && p->acc_uid_ok && p->acc_uid == key && node_fresh(n);
}

static int is_dotname(const char *s, u32 n) { return (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'); }

/* ---- the walk ---- */
static int walk_begin(int o, int wi, uintptr_t path, uintptr_t cwd, int flags)
{
    struct op *op = &N.ops[o];
    struct walk *w = &op->w[wi];
    const char *s = (const char *)path;
    u32 n = 0;
    if (!s) return -EFAULT;
    while (n < FS_PATHMAX && s[n]) n++;
    if (n >= FS_PATHMAX) return -ENAMETOOLONG;
    memcpy(w->buf, s, n + 1);
    w->pos = 0; w->depth = 0; w->flags = (u8)flags; w->linked = 0; w->busy = 0; w->nlen = 0;
    w->node = N_NONE;
    int start;
    if (s[0] == '/') start = N.root;
    else {
        start = node_find((u32)cwd);
        if (start < 0 || (N.nodes[start].flags & NF_STALE)) return -NFS_ESTALE;
    }
    w->cur = (u16)start;
    pin(op, w->cur);
    return 0;
}

static void walk_move(struct op *op, struct walk *w, u16 to)
{
    pin(op, to);
    unpin(op, w->cur);
    w->cur = to;
}

/* go on walking: 1 waiting for a call, 0 done (w->node pinned, or op->err) */
static int walk_run(int o, int wi)
{
    struct op *op = &N.ops[o];
    struct walk *w = &op->w[wi];
    if (w->busy == 1) return 0;                  /* already ended (its last answer came) */
    for (;;) {
        while (w->buf[w->pos] == '/') w->pos++;
        if (!w->buf[w->pos]) {                   /* the end: the directory reached */
            if (w->flags & WALK_PARENT) { w->nlen = 0; }
            w->node = w->cur; pin(op, w->node); w->busy = 1;
            return 0;
        }
        u32 e = w->pos;
        while (w->buf[e] && w->buf[e] != '/') e++;
        u32 q = e;
        while (w->buf[q] == '/') q++;
        int last = !w->buf[q];
        const char *name = w->buf + w->pos;
        u32 len = e - w->pos;
        if (len > NAMEMAX) { op->err = -ENAMETOOLONG; return 0; }
        if ((w->flags & WALK_PARENT) && last) {
            w->nlen = (u16)len;
            w->node = w->cur; pin(op, w->node); w->busy = 1;
            return 0;
        }
        struct node *d = nd(w->cur);
        if (!d) { op->err = -NFS_ESTALE; return 0; }
        if ((d->flags & NF_ATTR) && d->a.type != NF3DIR) { op->err = -ENOTDIR; return 0; }
        if (len == 1 && name[0] == '.') { w->pos = (u16)q; continue; }
        if (len == 2 && name[0] == '.' && name[1] == '.' && w->cur == N.root) { w->pos = (u16)q; continue; }
        int child = name_get(w->cur, name, len);
        if (child >= 0 && !acc_known(w->cur, cred_key(op))) {   /* a cached name: the search right is asked first */
            int c = issue(o, P3_ACCESS, w->cur, PU_WACCESS);
            if (c < 0) return 0;
            N.calls[c].count = AC_READ | AC_LOOKUP | AC_MODIFY | AC_EXTEND | AC_DELETE | AC_EXECUTE;
            N.calls[c].walk = (u8)wi;
            return 1;
        }
        if (child >= 0 && !(N.nodes[w->cur].acc & AC_LOOKUP)) { op->err = -EACCES; return 0; }
        if (child < 0) {
            int c = issue(o, P3_LOOKUP, w->cur, PU_LOOKUP);
            if (c < 0) return 0;
            N.calls[c].name = name; N.calls[c].nlen = (u16)len; N.calls[c].walk = (u8)wi;
            return 1;
        }
        struct node *cn = &N.nodes[child];
        if ((cn->flags & NF_ATTR) && cn->a.type == NF3LNK && (!last || (w->flags & WALK_FOLLOW))) {
            if (w->depth >= 8) { op->err = -ELOOP; return 0; }
            int c = issue(o, P3_READLINK, (u16)child, PU_WREADLINK);
            if (c < 0) return 0;
            N.calls[c].walk = (u8)wi;
            N.calls[c].count = (u32)q;           /* where the rest of the path starts */
            return 1;
        }
        if (last) { w->node = (u16)child; pin(op, w->node); w->pos = (u16)q; w->busy = 1; return 0; }
        walk_move(op, w, (u16)child);
        w->pos = (u16)q;
    }
}

/* an answer for the walk: 0 go on walking, < 0 the walk failed (op->err) */
static int walk_reply(int o, struct call *c)
{
    struct op *op = &N.ops[o];
    struct walk *w = &op->w[c->walk];
    if (c->purpose == PU_WACCESS) {
        if (R->has_attr) node_attr(c->node, &R->attr);
        if (R->err) { op->err = R->err; return -1; }
        struct node *p = nd(c->node);
        if (p) { p->acc = (u8)R->access; p->acc_uid = cred_key(op); p->acc_uid_ok = 1; }
        return 0;
    }
    if (c->purpose == PU_LOOKUP) {
        if (R->has_dattr) node_attr(c->node, &R->dattr);
        if (R->err) {
            op->err = R->err;
            if (R->err == -ENOENT && !w->linked) {          /* the last name is missing: O_CREAT may make it */
                u32 e = w->pos;
                while (w->buf[e] && w->buf[e] != '/') e++;
                u32 q = e;
                while (w->buf[q] == '/') q++;
                if (!w->buf[q]) { w->nlen = (u16)(e - w->pos); w->node = w->cur; pin(op, w->node); w->busy = 2; }
            }
            return -1;
        }
        if (!R->has_fh || !R->has_attr) { lg("a LOOKUP answer without a handle or attributes", 0, -1, 0); op->err = -EIO; return -1; }
        int child = node_get(R->fh, R->fhlen, &R->attr);
        if (child < 0) { op->err = -ENOMEM; return -1; }
        name_put(c->node, c->name, c->nlen, (u16)child);
        u32 e = w->pos;
        while (w->buf[e] && w->buf[e] != '/') e++;
        u32 q = e;
        while (w->buf[q] == '/') q++;
        int last = !w->buf[q];
        struct node *cn = &N.nodes[child];
        if (cn->a.type == NF3LNK && (!last || (w->flags & WALK_FOLLOW))) {
            /* the next turn finds it in the name cache and reads the link */
            if (!acc_known(c->node, cred_key(op))) {  /* LOOKUP succeeded: searching was allowed */
                struct node *d = nd(c->node);
                if (d) { d->acc = AC_LOOKUP; d->acc_uid = cred_key(op); d->acc_uid_ok = 1; }
            }
            return 0;
        }
        if (last) { w->node = (u16)child; pin(op, w->node); w->pos = (u16)q; op->flags |= OPF_FRESH; w->busy = 1; return 0; }
        walk_move(op, w, (u16)child);
        w->pos = (u16)q;
        return 0;
    }
    /* PU_WREADLINK: the target, then the rest of the path */
    if (R->has_attr) node_attr(c->node, &R->attr);
    if (R->err) { op->err = R->err; return -1; }
    u32 tl = R->count, rest = c->count;
    for (u32 i = 0; i < tl; i++) if (!R->data[i]) { op->err = -EIO; return -1; }
    u32 rl = (u32)strlen(w->buf + rest);
    if (!tl) { op->err = -ENOENT; return -1; }
    if (tl + 1 + rl >= FS_PATHMAX) { op->err = -ENAMETOOLONG; return -1; }
    memmove(w->buf + tl + 1, w->buf + rest, rl + 1);
    memcpy(w->buf, R->data, tl);
    w->buf[tl] = '/';
    if (!rl) w->buf[tl] = 0;
    w->pos = 0;
    w->depth++;
    w->linked = 1;
    if (w->buf[0] == '/') walk_move(op, w, N.root);
    return 0;
}

/* the walk ended with its node: fetch fresh attributes unless this request already did */
static int need_attr2(int o, u16 n, int always)
{
    struct op *op = &N.ops[o];
    if ((op->flags & OPF_FRESH) || (!always && node_fresh(n))) return 0;
    int c = issue(o, P3_GETATTR, n, PU_GETATTR);
    return c < 0 ? -1 : 1;
}
static int need_attr(int o, u16 n) { return need_attr2(o, n, 0); }

static void fill_stat(u16 n, uintptr_t dst)
{
    struct nfs_ustat st;
    node_ustat(n, &st);
    if (dst) memcpy((void *)dst, &st, sizeof st);
}

/* the cached answer for the export's root (5.3: mount and df never hang) */
static void root_stat_cached(uintptr_t dst)
{
    struct nfs_ustat st;
    if (N.root != N_NONE && (N.nodes[N.root].flags & NF_ATTR)) { node_ustat(N.root, &st); }
    else { memset(&st, 0, sizeof st); st.ino = 2; st.mode = 0040755; st.links = 2; }
    if (dst) memcpy((void *)dst, &st, sizeof st);
}

static int path_is_root(uintptr_t p)
{
    const char *s = (const char *)p;
    if (!s) return 0;
    while (*s == '/') s++;
    return !*s;
}

static void statfs_answer(uintptr_t dst)
{
    if (dst) memcpy((void *)dst, &N.sfs, sizeof N.sfs);
}

/* ---- the requests ---- */
static void run(int o);

static int wait_mount(int o)
{
    int m = mount_kick();
    if (m > 0) return 0;
    if (m < 0) { op_finish(o, m); return 1; }
    N.ops[o].flags |= OPF_WAITMOUNT;
    return 1;
}

void op_start(int o)
{
    struct op *op = &N.ops[o];
    op->step = 0;
    op->started = nfs_now();
    switch (op->type) {
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)op->a[0];
        if (fi) {
            memset(fi, 0, sizeof *fi);
            memcpy(fi->dev, N.dev, strlen(N.dev) < 15 ? strlen(N.dev) : 15);
            memcpy(fi->type, "nfs", 3);
        }
        if (N.o.lazy) { op_finish(o, 2); return; }   /* lazy: no network until the first use (4.1) */
        op->deadline = op->started + 10000;
        if (!op->deadline) op->deadline = 1;
        if (wait_mount(o)) return;
        op_finish(o, 2);
        return;
    }
    case FS_MARK: op_finish(o, -ENOSYS); return;
    case FS_QUIT:
        N.quitting = 1;
        N.quitop = (s16)o;
        op->deadline = op->started + 10000;
        if (!op->deadline) op->deadline = 1;
        break;
    case FS_STATFS:
        op->deadline = op->started + 2000;
        if (!op->deadline) op->deadline = 1;
        if (N.notans || N.mstate == MS_FAILED) { statfs_answer(op->a[0]); op_finish(o, 0); return; }
        break;
    case FS_STAT: case FS_LSTAT:
        if (path_is_root(op->a[0])) {
            op->deadline = op->started + 2000;
            if (!op->deadline) op->deadline = 1;
            if (N.notans) { root_stat_cached(op->a[2]); op_finish(o, 0); return; }
        }
        break;
    case FS_FSTAT:
        if ((u32)op->a[0] == 2) {
            op->deadline = op->started + 2000;
            if (!op->deadline) op->deadline = 1;
            if (N.notans) { root_stat_cached(op->a[1]); op_finish(o, 0); return; }
        }
        break;
    case FS_SYNC:
        if (op->a[0] == 1) { op->deadline = op->started + 2000; if (!op->deadline) op->deadline = 1; }
        if (N.notans) {
            if (N.unsaved_bytes) lg("sync: the NAS is not answering: ", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB kept unsaved (-EIO)");
            op_finish(o, N.unsaved_bytes ? -EIO : 0);
            return;
        }
        break;
    }
    if (N.o.ro) {
        u32 t = op->type;
        if (t == FS_WRITE || t == FS_MKDIR || t == FS_UNLINK || t == FS_RMDIR || t == FS_RENAME || t == FS_LINK ||
            t == FS_SYMLINK || t == FS_CHMOD || t == FS_CHOWN || t == FS_UTIMES ||
            (t == FS_OPEN && (op->a[2] & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)))) { op_finish(o, -EROFS); return; }
    }
    run(o);
}

/* the waiting requests go on once the mount is up (or fail with its error) */
void ops_mounted(void)
{
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (!op->used || !(op->flags & OPF_WAITMOUNT)) continue;
        op->flags &= (u8)~OPF_WAITMOUNT;
        if (N.mstate == MS_FAILED) { op_finish((int)i, N.merr); continue; }
        if (op->type == FS_ROOT) { op_finish((int)i, 2); continue; }
        run((int)i);
    }
}

/* the NAS stopped answering: syncs answer -EIO at once, keeping the data (5.3) */
void ops_notanswering(void)
{
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (!op->used) continue;
        if (op->type == FS_SYNC) {
            lg("sync: the NAS is not answering: ", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB kept unsaved (-EIO)");
            op_finish((int)i, -EIO);
        } else if (op->type == FS_STATFS) { statfs_answer(op->a[0]); op_finish((int)i, 0); }
        else if ((op->type == FS_STAT || op->type == FS_LSTAT) && path_is_root(op->a[0])) { root_stat_cached(op->a[2]); op_finish((int)i, 0); }
        else if (op->type == FS_FSTAT && (u32)op->a[0] == 2) { root_stat_cached(op->a[1]); op_finish((int)i, 0); }
    }
}

void ops_timers(u32 now)
{
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (!op->used || !op->deadline || !due(op->deadline, now)) continue;
        op->deadline = 0;
        switch (op->type) {
        case FS_ROOT:
            lg("the NAS did not answer (mount)", 0, -1, 0);
            op_finish((int)i, N.merr ? N.merr : -ETIMEDOUT);
            break;
        case FS_STATFS: statfs_answer(op->a[0]); op_finish((int)i, 0); break;
        case FS_STAT: case FS_LSTAT: root_stat_cached(op->a[2]); op_finish((int)i, 0); break;
        case FS_FSTAT: root_stat_cached(op->a[1]); op_finish((int)i, 0); break;
        case FS_SYNC:
            if (N.unsaved_bytes)
                lg("", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB not saved: the NAS did not answer before the reset");
            op_finish((int)i, N.unsaved_bytes ? -EIO : 0);
            break;
        case FS_QUIT:
            op->step = op->step < 50 ? 50 : 52;  /* the commits gave up: UMNT; UMNT gave up: the end */
            op->flags &= (u8)~OPF_WAITROOM;
            run((int)i);
            break;
        }
    }
    ops_poke();
}

/* requests that wait for something to change (room, a piece answered, the commits) look again */
void ops_poke(void)
{
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (op->used && (op->flags & OPF_WAITROOM) && !op->inflight) { op->flags &= (u8)~OPF_WAITROOM; run((int)i); }
    }
}

u32 ops_next(u32 now)
{
    u32 best = ~0u;
    for (u32 i = 0; i < N.pf.nops; i++) {
        struct op *op = &N.ops[i];
        if (!op->used) continue;
        if (!op->deadline) continue;
        u32 d = due(op->deadline, now) ? 0 : tdiff(op->deadline, now);
        if (d < best) best = d;
    }
    return best;
}

/* ---- READ: rsize pieces from the data cache or the NAS, up to 4 out at once on the large profile ----
 * rb_base is the first block not yet in (the bits of rb_mask: the blocks after it that are), rb_next the next block
 * to ask, rb_end the file's end once known (a short piece, eof, or an error after some bytes) */
static void read_copy(struct op *op, u32 blk, const u8 *data, u32 n)
{
    u64 bs = (u64)blk << N.rshift;
    u64 off = op->a[1], end = off + (u32)op->a[3];
    u64 s = bs > off ? bs : off, e = bs + n < end ? bs + n : end;
    if (s < e) memcpy((u8 *)op->a[2] + (u32)(s - off), data + (u32)(s - bs), (u32)(e - s));
}

static void read_mark(struct op *op, u32 blk, u32 n, int eof)
{
    u32 bit = blk - op->rb_base;
    if (bit < 32) op->rb_mask |= 1u << bit;
    if (n < N.rsize || eof) {
        u64 e = ((u64)blk << N.rshift) + n;
        if (e < op->rb_end) op->rb_end = e;
    }
}

static void read_go(int o)
{
    struct op *op = &N.ops[o];
    u64 off = op->a[1], end = off + (u32)op->a[3];
    u32 lastblk = (u32)((end - 1) >> N.rshift);
    u32 depth = N.o_large ? 4 : 1;
    for (;;) {
        while (op->rb_mask & 1) { op->rb_mask >>= 1; op->rb_base++; }
        u64 e = end < op->rb_end ? end : op->rb_end;
        u64 front = (u64)op->rb_base << N.rshift;
        if (front >= e) { op_finish(o, e > off ? (long)(e - off) : 0); return; }
        if (op->rb_next < op->rb_base) op->rb_next = op->rb_base;
        if (op->rb_next > lastblk || ((u64)op->rb_next << N.rshift) >= e || op->rb_next - op->rb_base >= 32) {
            if (op->inflight) return;
            op_finish(o, front > off ? (long)((front < e ? front : e) - off) : 0);   /* (not reached) */
            return;
        }
        if (op->inflight >= depth) return;
        int k = dc_find(op->node, op->rb_next);
        if (k >= 0) {
            read_copy(op, op->rb_next, N.dc[k].data, N.dc[k].len);
            read_mark(op, op->rb_next, N.dc[k].len, 0);
            op->rb_next++;
            continue;
        }
        int c = issue(o, P3_READ, op->node, PU_READ);
        if (c < 0) { op_finish(o, op->err); return; }
        N.calls[c].off = (u64)op->rb_next << N.rshift;
        N.calls[c].count = N.rsize;
        N.calls[c].cookie = op->rb_next;
        op->rb_next++;
    }
}

static void run(int o)
{
    struct op *op = &N.ops[o];
    if (!op->used) return;
    if (op->type != FS_QUIT && op->type != FS_ROOT && N.mstate != MS_UP && wait_mount(o)) return;
    for (;;) {
        if (op->err) {
            if (op->type == FS_OPEN && op->step == 1 && op->err == -ENOENT && op->w[0].busy == 2 &&
                (op->a[2] & O_CREAT)) { op->err = 0; op->step = 10; continue; }
            op_finish(o, op->err);
            return;
        }
        switch (op->type) {
        /* ---------------- OPEN ---------------- */
        case FS_OPEN: {
            u32 fl = (u32)op->a[2];
            switch (op->step) {
            case 0: {
                int excl = (fl & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
                int e = walk_begin(o, 0, op->a[0], op->a[1], excl ? 0 : WALK_FOLLOW);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }   /* fall through */
            case 1:
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                if ((fl & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) { op_finish(o, -EEXIST); return; }
                op->node = op->w[0].node;
                op->step = 2;
                /* fall through */
            case 2: {
                int r = need_attr2(o, op->node, 1);   /* every open asks again: close-to-open (6.1) */
                op->step = 3;
                if (r) return;
            }   /* fall through */
            case 3: {
                struct node *p = &N.nodes[op->node];
                int wr = (fl & (O_WRONLY | O_RDWR)) != 0;
                if (p->a.type == NF3DIR) { if (wr) { op_finish(o, -EISDIR); return; } }
                else if (p->a.type != NF3REG) { op_finish(o, -ENXIO); return; }   /* devices, fifos, sockets (8.3) */
                if (p->a.size >> 32) { op_finish(o, -EFBIG); return; }
                if ((fl & O_EXEC) && (p->a.type != NF3REG || !(p->a.mode & 0111))) { op_finish(o, -EACCES); return; }
                op->step = 4;
                if (!acc_known(op->node, cred_key(op))) {
                    int c = issue(o, P3_ACCESS, op->node, PU_ACCESS);
                    if (c < 0) continue;
                    N.calls[c].count = AC_READ | AC_LOOKUP | AC_MODIFY | AC_EXTEND | AC_DELETE | AC_EXECUTE;
                    return;
                }
            }   /* fall through */
            case 4: {
                struct node *p = &N.nodes[op->node];
                u32 want = acc_want(fl, p->a.type == NF3DIR);
                if ((p->acc & want) != want) { op_finish(o, -EACCES); return; }
                if ((fl & O_TRUNC) && (fl & (O_WRONLY | O_RDWR)) && p->a.size) { op->step = 20; continue; }
                op->step = 30;
                continue;
            }
            case 10: {                           /* create: the walk left the directory and the name */
                struct walk *w = &op->w[0];
                const char *name = w->buf + w->pos;
                if (!w->nlen || is_dotname(name, w->nlen)) { op_finish(o, -EEXIST); return; }
                op->node2 = w->node;             /* the directory (pinned by the walk) */
                memset(&op->sa, 0, sizeof op->sa);
                int c = issue(o, P3_CREATE, op->node2, PU_CREATE);
                if (c < 0) continue;
                N.calls[c].name = name; N.calls[c].nlen = w->nlen;
                if (fl & O_EXCL) {
                    u32 v = N.seed ^ ++N.cverf;
                    u32 t = nfs_now();
                    memcpy(op->cverf, &v, 4); memcpy(op->cverf + 4, &t, 4);
                    N.calls[c].count = 2;
                } else {
                    op->sa.set_mode = 1; op->sa.mode = 0666 & ~(u32)op->umask & 07777;
                    N.calls[c].count = 0;
                }
                op->step = 11;
                return;
            }
            case 12: {                           /* after CREATE (EXCLUSIVE): the mode and the times (4.5) */
                memset(&op->sa, 0, sizeof op->sa);
                op->sa.set_mode = 1; op->sa.mode = 0666 & ~(u32)op->umask & 07777;
                op->sa.atime_how = 1; op->sa.mtime_how = 1;
                if (issue(o, P3_SETATTR, op->node, PU_SATTR) < 0) continue;
                op->step = 30;
                return;
            }
            case 20:                             /* O_TRUNC: after the file's own writes are answered */
                if (wr_node_busy(op->node)) { op->flags |= OPF_WAITROOM; return; }
                wr_drop_node(op->node);
                dc_drop(op->node);
                memset(&op->sa, 0, sizeof op->sa);
                op->sa.set_size = 1;
                if (issue(o, P3_SETATTR, op->node, PU_TRUNC) < 0) continue;
                op->step = 30;
                return;
            case 30: {
                struct node *p = &N.nodes[op->node];
                p->opens++;
                fill_stat(op->node, op->a[3]);
                op_finish(o, (long)p->ino);
                return;
            }
            }
            return;
        }
        /* ---------------- READ ---------------- */
        case FS_READ:
            if (op->step == 0) {
                int n = node_find((u32)op->a[0]);
                if (n < 0 || (N.nodes[n].flags & NF_STALE)) { op_finish(o, -NFS_ESTALE); return; }
                op->node = (u16)n;
                pin(op, op->node);
                op->step = 1;
                if (!(N.nodes[n].flags & NF_ATTR)) { if (issue(o, P3_GETATTR, op->node, PU_GETATTR) < 0) continue; return; }
            }
            if (op->step == 1) {
                struct node *p = &N.nodes[op->node];
                if (p->a.type == NF3DIR) { op_finish(o, -EISDIR); return; }
                if (!op->a[3]) { op_finish(o, 0); return; }
                op->rb_base = op->rb_next = (u32)(op->a[1] >> N.rshift);
                op->rb_mask = 0;
                op->rb_end = ~(u64)0;
                op->step = 2;
            }
            read_go(o);
            return;
        /* ---------------- WRITE ---------------- */
        case FS_WRITE:
            wr_start(o);
            return;
        /* ---------------- STAT, LSTAT, FSTAT ---------------- */
        case FS_STAT: case FS_LSTAT: case FS_FSTAT: {
            uintptr_t dst = op->type == FS_FSTAT ? op->a[1] : op->a[2];
            if (op->step == 0) {
                if (op->type == FS_FSTAT) {
                    int n = node_find((u32)op->a[0]);
                    if (n < 0 || (N.nodes[n].flags & NF_STALE)) { op_finish(o, -NFS_ESTALE); return; }
                    op->node = (u16)n; pin(op, op->node);
                    op->step = 2;
                } else {
                    int e = walk_begin(o, 0, op->a[0], op->a[1], op->type == FS_STAT ? WALK_FOLLOW : 0);
                    if (e) { op_finish(o, e); return; }
                    op->step = 1;
                }
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                op->node = op->w[0].node;
                op->step = 2;
            }
            if (op->step == 2) {
                op->step = 3;
                int r = need_attr(o, op->node);
                if (r > 0) return;
                if (r < 0) continue;
            }
            if (N.nodes[op->node].a.size >> 32) { op_finish(o, -EFBIG); return; }
            fill_stat(op->node, dst);
            op_finish(o, 0);
            return;
        }
        /* ---------------- CHDIR ---------------- */
        case FS_CHDIR:
            if (op->step == 0) {
                int e = walk_begin(o, 0, op->a[0], op->a[1], WALK_FOLLOW);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                op->node = op->w[0].node;
                op->step = 2;
                int r = need_attr(o, op->node);
                if (r > 0) return;
                if (r < 0) continue;
            }
            if (op->step == 2) {
                if (N.nodes[op->node].a.type != NF3DIR) { op_finish(o, -ENOTDIR); return; }
                op->step = 3;
                if (!acc_known(op->node, cred_key(op))) {
                    int c = issue(o, P3_ACCESS, op->node, PU_ACCESS);
                    if (c < 0) continue;
                    N.calls[c].count = AC_READ | AC_LOOKUP | AC_MODIFY | AC_EXTEND | AC_DELETE | AC_EXECUTE;
                    return;
                }
            }
            if (!(N.nodes[op->node].acc & AC_LOOKUP)) { op_finish(o, -EACCES); return; }
            op_finish(o, (long)N.nodes[op->node].ino);
            return;
        /* ---------------- READLINK ---------------- */
        case FS_READLINK:
            if (op->step == 0) {
                int e = walk_begin(o, 0, op->a[0], op->a[1], 0);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                op->node = op->w[0].node;
                if (N.nodes[op->node].a.type != NF3LNK) { op_finish(o, -EINVAL); return; }
                if (issue(o, P3_READLINK, op->node, PU_READLINK) < 0) continue;
                op->step = 2;
            }
            return;
        /* ---------------- MKDIR, UNLINK, RMDIR, SYMLINK: one name in a directory ---------------- */
        case FS_MKDIR: case FS_UNLINK: case FS_RMDIR: case FS_SYMLINK: {
            int pi = op->type == FS_SYMLINK ? 1 : 0;     /* SYMLINK: a1 the link's path, a2 the cwd */
            uintptr_t cwd = op->type == FS_SYMLINK ? op->a[2] : op->a[1];
            if (op->step == 0) {
                if (op->type == FS_SYMLINK) {           /* the target text, kept in the second walk's buffer */
                    const char *t = (const char *)op->a[0];
                    u32 n = 0;
                    if (!t) { op_finish(o, -EFAULT); return; }
                    while (n < FS_PATHMAX && t[n]) n++;
                    if (n >= FS_PATHMAX) { op_finish(o, -ENAMETOOLONG); return; }
                    if (!n) { op_finish(o, -ENOENT); return; }
                    memcpy(op->w[1].buf, t, n + 1);
                    op->w[1].nlen = (u16)n;
                }
                int e = walk_begin(o, 0, op->a[pi], cwd, WALK_PARENT);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                struct walk *w = &op->w[0];
                const char *name = w->buf + w->pos;
                if (!w->nlen || is_dotname(name, w->nlen)) {
                    int e = op->type == FS_MKDIR || op->type == FS_SYMLINK ? -EEXIST : op->type == FS_UNLINK ? -EISDIR : -EINVAL;
                    op_finish(o, e);
                    return;
                }
                op->node2 = w->node;
                memset(&op->sa, 0, sizeof op->sa);
                int proc = op->type == FS_MKDIR ? P3_MKDIR : op->type == FS_UNLINK ? P3_REMOVE :
                           op->type == FS_RMDIR ? P3_RMDIR : P3_SYMLINK;
                int pu = op->type == FS_MKDIR ? PU_MKDIR : op->type == FS_UNLINK ? PU_REMOVE :
                         op->type == FS_RMDIR ? PU_RMDIR : PU_SYMLINK;
                if (op->type == FS_MKDIR) { op->sa.set_mode = 1; op->sa.mode = 0777 & ~(u32)op->umask & 07777; }
                if (op->type == FS_SYMLINK) { op->sa.set_mode = 1; op->sa.mode = 0777; }
                int c = issue(o, proc, op->node2, pu);
                if (c < 0) continue;
                N.calls[c].name = name; N.calls[c].nlen = w->nlen;
                if (op->type == FS_SYMLINK) { N.calls[c].name2 = op->w[1].buf; N.calls[c].nlen2 = op->w[1].nlen; }
                op->step = 2;
            }
            return;
        }
        /* ---------------- RENAME, LINK: two paths ---------------- */
        case FS_RENAME: case FS_LINK:
            if (op->step == 0) {
                int e = walk_begin(o, 0, op->a[0], op->a[2], op->type == FS_RENAME ? WALK_PARENT : WALK_FOLLOW);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                op->flags &= (u8)~OPF_FRESH;
                int e = walk_begin(o, 1, op->a[1], op->a[2], WALK_PARENT);
                if (e) { op_finish(o, e); return; }
                op->step = 2;
            }
            if (op->step == 2) {
                if (walk_run(o, 1)) return;
                if (op->err) continue;
                struct walk *w0 = &op->w[0], *w1 = &op->w[1];
                const char *n1 = w1->buf + w1->pos;
                if (!w1->nlen || is_dotname(n1, w1->nlen)) { op_finish(o, op->type == FS_LINK ? -EEXIST : -EINVAL); return; }
                int c;
                if (op->type == FS_RENAME) {
                    const char *n0 = w0->buf + w0->pos;
                    if (!w0->nlen || is_dotname(n0, w0->nlen)) { op_finish(o, -EINVAL); return; }
                    c = issue(o, P3_RENAME, w0->node, PU_RENAME);
                    if (c < 0) continue;
                    N.calls[c].name = n0; N.calls[c].nlen = w0->nlen;
                    N.calls[c].node2 = w1->node; N.calls[c].name2 = n1; N.calls[c].nlen2 = w1->nlen;
                } else {
                    if (N.nodes[w0->node].a.type == NF3DIR) { op_finish(o, -EPERM); return; }   /* no hard links to directories */
                    c = issue(o, P3_LINK, w1->node, PU_LINK);
                    if (c < 0) continue;
                    N.calls[c].name = n1; N.calls[c].nlen = w1->nlen; N.calls[c].node2 = w0->node;
                }
                op->step = 3;
            }
            return;
        /* ---------------- CHMOD, CHOWN, UTIMES: SETATTR ---------------- */
        case FS_CHMOD: case FS_CHOWN: case FS_UTIMES:
            if (op->step == 0) {
                if (op->type == FS_CHMOD && (op->a[2] & 06000)) { op_finish(o, -EPERM); return; }   /* nosuid (8.3) */
                int e = walk_begin(o, 0, op->a[0], op->a[1], WALK_FOLLOW);
                if (e) { op_finish(o, e); return; }
                op->step = 1;
            }
            if (op->step == 1) {
                if (walk_run(o, 0)) return;
                if (op->err) continue;
                op->node = op->w[0].node;
                memset(&op->sa, 0, sizeof op->sa);
                if (op->type == FS_CHMOD) { op->sa.set_mode = 1; op->sa.mode = (u32)op->a[2] & 07777; }
                else if (op->type == FS_CHOWN) {
                    if ((u32)op->a[2] != 0xFFFFFFFFu) { op->sa.set_uid = 1; op->sa.uid = (u32)op->a[2]; }
                    if ((u32)op->a[3] != 0xFFFFFFFFu) { op->sa.set_gid = 1; op->sa.gid = (u32)op->a[3]; }
                } else if ((u32)op->a[2] == FS_UTIME_NOW) { op->sa.atime_how = 1; op->sa.mtime_how = 1; }
                else { op->sa.atime_how = 2; op->sa.mtime_how = 2; op->sa.atime = op->sa.mtime = (u32)op->a[2]; }
                if (issue(o, P3_SETATTR, op->node, PU_SETATTR) < 0) continue;
                op->step = 2;
            }
            return;
        /* ---------------- READDIR ---------------- */
        case FS_READDIR: {
            if (op->step == 0) {
                int n = node_find((u32)op->a[0]);
                if (n < 0 || (N.nodes[n].flags & NF_STALE)) { op_finish(o, -NFS_ESTALE); return; }
                op->node = (u16)n; pin(op, op->node);
                op->pos = (u32)op->a[1];
                op->step = 1;
                int r = need_attr(o, op->node);
                if (r > 0) return;
                if (r < 0) continue;
            }
            struct node *p = &N.nodes[op->node];
            if (p->a.type != NF3DIR) { op_finish(o, -ENOTDIR); return; }
            int t = rdt_find(op->node, 1);
            struct rdt *rt = &N.rdts[t];
            if (rt->ctime != p->a.ctime || rt->ctime_ns != p->a.ctime_ns) {   /* changed: positions start again */
                rt->n = 1; rt->pos[0] = 0; rt->cookie[0] = 0; memset(rt->verf, 0, 8);
                rt->ctime = p->a.ctime; rt->ctime_ns = p->a.ctime_ns;
                if (N.chunk->rdt == t) N.chunk->valid = 0;
            }
            struct chunk *ch = N.chunk;
            if (ch->valid && ch->rdt == t && ch->node == op->node && ch->gen == p->gen && op->pos >= ch->start) {
                if (op->pos < ch->start + ch->n) {
                    struct nfs_udirent *ue = (struct nfs_udirent *)op->a[2];
                    if (ue) *ue = ch->e[op->pos - ch->start];
                    if (op->a[3]) *(u32 *)op->a[3] = op->pos + 1;
                    op_finish(o, 1);
                    return;
                }
                if (op->pos == ch->start + ch->n && ch->eof) {
                    if (op->a[3]) *(u32 *)op->a[3] = op->pos;
                    op_finish(o, 0);
                    return;
                }
            }
            if (++op->step > 200) { lg("a directory listing does not settle: -EIO", 0, -1, 0); op_finish(o, -EIO); return; }
            int k = 0;
            for (int i = 0; i < rt->n; i++) if (rt->pos[i] <= op->pos && rt->pos[i] >= rt->pos[k]) k = i;
            int c = issue(o, N.noplus ? P3_READDIR : P3_READDIRPLUS, op->node, PU_READDIR);
            if (c < 0) continue;
            N.calls[c].cookie = rt->cookie[k];
            memcpy(N.calls[c].verf, rt->verf, 8);
            N.calls[c].count = N.noplus ? 2048 : (N.rxcap - 512 < 4096 ? N.rxcap - 512 : 4096);
            N.calls[c].off = rt->pos[k];         /* the position of the first entry it brings */
            N.calls[c].cseq = (u32)t;
            return;
        }
        /* ---------------- STATFS ---------------- */
        case FS_STATFS:
            if (op->step == 0) {
                if (issue(o, P3_FSSTAT, N.root, PU_FSSTAT) < 0) continue;
                op->step = 1;
            }
            return;
        /* ---------------- SYNC ---------------- */
        case FS_SYNC:
            if (op->step == 0) { op->pos = N.seq; op->step = 1; wr_commit_all(0); }
            {
                int left = 0;
                for (u32 i = 0; i < N.pf.npieces; i++) {
                    struct piece *pc = &N.pieces[i];
                    if (pc->state && (s32)(pc->cseq - op->pos) < 0) left = 1;
                }
                if (!left) { op_finish(o, 0); return; }
                wr_commit_all(0);
                op->flags |= OPF_WAITROOM;
            }
            return;
        /* ---------------- QUIT: commits (at most 10 s), UMNT (at most 2 s), the end (7.4) ---------------- */
        case FS_QUIT:
            if (op->step == 0) { op->step = 1; if (N.mstate == MS_UP) wr_commit_all(0); }
            if (op->step == 1) {
                if (N.unsaved_bytes && N.mstate == MS_UP && !N.notans) { wr_commit_all(0); op->flags |= OPF_WAITROOM; return; }
                op->step = 50;
            }
            if (op->step == 50) {
                if (N.unsaved_bytes)
                    lg("umount: ", 0, (long)(N.unsaved_bytes + 1023) / 1024, " KB not saved on the NAS");
                op->step = 52;
                if (N.mstate == MS_UP && N.mport && !N.notans && call_new(PG_MOUNT, MNT_UMNT, o, CP_UMNT, 0) >= 0) {
                    op->step = 51;
                    op->deadline = nfs_now() + 2000;
                    if (!op->deadline) op->deadline = 1;
                    return;
                }
            }
            if (op->step == 51) return;
            N.done = 1;
            if (N.cstate != CS_DOWN) nfs_disconnect();
            N.cstate = CS_DOWN;
            answer_ops_err(-EIO);
            op_finish(o, 0);
            return;
        default:
            op_finish(o, -ENOSYS);
            return;
        }
    }
}

/* an answer for one of the op's calls */
void op_reply(int o, struct call *c, struct res *r)
{
    struct op *op = &N.ops[o];
    if (!op->used) return;
    R = r;
    switch (c->purpose) {
    case PU_LOOKUP: case PU_WREADLINK: case PU_WACCESS:
        walk_reply(o, c);
        break;
    case PU_GETATTR:
        if (r->err) { op->err = r->err; break; }
        node_attr(c->node, &r->attr);
        op->flags |= OPF_FRESH;
        break;
    case PU_ACCESS:
        if (r->has_attr) node_attr(c->node, &r->attr);
        if (r->err) { op->err = r->err; break; }
        { struct node *p = nd(c->node); if (p) { p->acc = (u8)r->access; p->acc_uid = cred_key(op); p->acc_uid_ok = 1; } }
        break;
    case PU_CREATE: {
        node_wcc(c->node, r, 1);
        if (r->err) { op->err = r->err; break; }
        int n = r->has_fh && r->has_attr ? node_get(r->fh, r->fhlen, &r->attr) : -1;
        if (n < 0) {                             /* no handle in the answer: look the name up */
            struct walk *w = &op->w[0];
            int k = issue(o, P3_LOOKUP, c->node, PU_LOOKUP2);
            if (k >= 0) { N.calls[k].name = w->buf + w->pos; N.calls[k].nlen = w->nlen; op->step = 11; }
            break;
        }
        op->node = (u16)n; pin(op, op->node);
        name_put(c->node, c->name, c->nlen, (u16)n);
        op->flags |= OPF_FRESH;
        op->step = (op->a[2] & O_EXCL) ? 12 : 30;
        break;
    }
    case PU_LOOKUP2: {
        if (r->err) { op->err = r->err; break; }
        int n = r->has_fh && r->has_attr ? node_get(r->fh, r->fhlen, &r->attr) : -1;
        if (n < 0) { op->err = -EIO; break; }
        op->node = (u16)n; pin(op, op->node);
        name_put(c->node, c->name, c->nlen, (u16)n);
        op->step = (op->a[2] & O_EXCL) ? 12 : 30;
        break;
    }
    case PU_SATTR: case PU_TRUNC: case PU_SETATTR: {
        if (r->has_dattr) node_attr(c->node, &r->dattr);
        else node_dirchanged(c->node);
        if (r->err) { op->err = r->err; break; }
        if (c->purpose == PU_SETATTR) { op_finish(o, 0); return; }
        break;
    }
    case PU_READ: {
        if (r->has_attr) node_attr(c->node, &r->attr);
        u32 blk = (u32)c->cookie;
        if (r->err) {                            /* bytes before it are a short count; none: the error */
            u64 at = (u64)blk << N.rshift;
            if (at <= op->a[1]) { op->err = r->err; break; }
            if (at < op->rb_end) op->rb_end = at;
            break;
        }
        read_copy(op, blk, r->data, r->count);
        read_mark(op, blk, r->count, r->eof);
        struct node *p = nd(c->node);
        if (p && r->count && (r->count == N.rsize || r->eof)) {
            int k = dc_find(c->node, blk);
            if (k < 0) k = dc_slot();
            if (k >= 0) {
                struct dpiece *d = &N.dc[k];
                memcpy(d->data, r->data, r->count);
                d->node = c->node; d->gen = p->gen; d->blk = blk; d->len = r->count;
                d->ctime = p->a.ctime; d->ctime_ns = p->a.ctime_ns; d->size = p->a.size;
                d->lru = ++N.lru;
            }
        }
        break;
    }
    case PU_READLINK: {
        if (r->has_attr) node_attr(c->node, &r->attr);
        if (r->err) { op->err = r->err; break; }
        u32 n = r->count < (u32)op->a[3] ? r->count : (u32)op->a[3];
        if (op->a[2] && n) memcpy((void *)op->a[2], r->data, n);
        op_finish(o, (long)n);
        return;
    }
    case PU_MKDIR: case PU_SYMLINK: {
        node_wcc(c->node, r, 1);
        if (r->err) { op->err = r->err; break; }
        if (r->has_fh && r->has_attr) {
            int n = node_get(r->fh, r->fhlen, &r->attr);
            if (n >= 0) name_put(c->node, c->name, c->nlen, (u16)n);
        }
        op_finish(o, 0);
        return;
    }
    case PU_REMOVE: case PU_RMDIR: {
        int child = name_get(c->node, c->name, c->nlen);
        node_wcc(c->node, r, 1);
        if (r->err) { op->err = r->err; break; }
        name_del(c->node, c->name, c->nlen);
        if (child >= 0) {
            struct node *p = &N.nodes[child];
            if (p->a.nlink <= 1 && c->purpose == PU_REMOVE && !p->opens) wr_drop_node((u16)child);
            p->flags &= (u8)~NF_ATTR;            /* its link count changed */
        }
        op_finish(o, 0);
        return;
    }
    case PU_RENAME: {
        node_wcc(c->node, r, 1);
        if (c->node2 != c->node) node_dirchanged(c->node2);
        if (r->err) { op->err = r->err; break; }
        name_del(c->node, c->name, c->nlen);
        name_del(c->node2, c->name2, c->nlen2);
        op_finish(o, 0);
        return;
    }
    case PU_LINK: {
        if (r->has_attr) node_attr(c->node2, &r->attr);
        node_wcc(c->node, r, 1);
        if (r->err) { op->err = r->err; break; }
        name_put(c->node, c->name, c->nlen, c->node2);
        op_finish(o, 0);
        return;
    }
    case PU_FSSTAT: {
        if (r->has_attr) node_attr(c->node, &r->attr);
        if (r->err) { op->err = r->err; break; }
        u64 cap = 0xFFFFFFFFull;                 /* counts capped below 4 TB in 1 KB units (4.5) */
        u64 b = r->tbytes >> 10, f = r->abytes >> 10;
        N.sfs.bsize = 1024;
        N.sfs.blocks = (u32)(b > cap ? cap : b);
        N.sfs.bfree = (u32)(f > cap ? cap : f);
        N.sfs.files = (u32)(r->tfiles > cap ? cap : r->tfiles);
        N.sfs.ffree = (u32)(r->afiles > cap ? cap : r->afiles);
        N.statfs_ok = 1;
        statfs_answer(op->a[0]);
        op_finish(o, 0);
        return;
    }
    case PU_READDIR: {
        struct node *p = nd(c->node);
        int t = (int)c->cseq;
        struct rdt *rt = &N.rdts[t];
        if (r->status == NFS3ERR_BAD_COOKIE || (!r->err && p && r->has_dattr &&
            (r->dattr.ctime != rt->ctime || r->dattr.ctime_ns != rt->ctime_ns) && (p->flags & NF_ATTR))) {
            if (r->has_dattr) node_attr(c->node, &r->dattr);
            if (p) { rt->ctime = ~p->a.ctime; }  /* the listing starts again (6.3) */
            break;
        }
        if (r->status == NFS3ERR_NOTSUPP && !N.noplus && c->proc == P3_READDIRPLUS) { N.noplus = 1; break; }
        if (r->err) { op->err = r->err; break; }
        if (!p || rt->node != c->node || rt->gen != p->gen) break;   /* the table went meanwhile: ask again */
        if (r->has_dattr) node_attr(c->node, &r->dattr);
        memcpy(rt->verf, r->verf, 8);
        struct chunk *ch = N.chunk;
        ch->valid = 1; ch->node = c->node; ch->gen = p->gen; ch->rdt = (s16)t;
        ch->start = (u32)c->off; ch->n = 0; ch->eof = 0;
        u32 pos = (u32)c->off;
        u64 fid, ck = c->cookie;
        const u8 *nm; u32 nl;
        int plus = c->proc == P3_READDIRPLUS, more;
        struct xdr x = r->dir;
        struct res er;
        memset(&er, 0, sizeof er);
        while ((more = dec_dirent(&x, plus, &fid, &nm, &nl, &ck, &er)) > 0) {
            int ok = nl > 0;
            for (u32 i = 0; i < nl; i++) if (!nm[i] || nm[i] == '/') ok = 0;
            if (!ok) { if (!rt->logged) lg("a name with a NUL or / from the NAS: left out", 0, -1, 0); rt->logged = 1; continue; }
            if (nl > 63) { if (!rt->logged) lg("a name longer than 63 bytes: left out of the listing", 0, -1, 0); rt->logged = 1; continue; }
            struct nfs_udirent *ue = &ch->e[ch->n];
            u32 ino;
            int child = -1;
            if (is_dotname((const char *)nm, nl)) {
                if (nl == 1 || c->node == N.root) ino = nl == 1 ? p->ino : 2;
                else if (plus && er.has_fh && er.has_attr && (child = node_get(er.fh, er.fhlen, &er.attr)) >= 0) ino = N.nodes[child].ino;
                else ino = ino_for_fileid(fid);
            } else if (plus && er.has_fh && er.has_attr && (child = node_get(er.fh, er.fhlen, &er.attr)) >= 0) {
                ino = N.nodes[child].ino;
                name_put(c->node, (const char *)nm, nl, (u16)child);
            } else ino = ino_for_fileid(fid);
            static const u8 dt[8] = { 0, 1, 2, 4, 3, 7, 6, 5 };   /* NF3* -> the ext2 file type */
            ue->ino = ino;
            ue->type = plus && er.has_attr ? dt[er.attr.type & 7] : 0;
            memcpy(ue->name, nm, nl); ue->name[nl] = 0;
            ch->n++;
            pos++;
            if (ch->n == RD_CHUNK) break;
        }
        if (more < 0) { lg("a malformed directory listing", 0, -1, 0); op->err = -EIO; ch->valid = 0; break; }
        if (more == 0) ch->eof = (u8)er.eof;
        if (more == 0 && !er.eof && !ch->n) ch->eof = 1;   /* nothing and no end: taken as the end (no endless asking) */
        if (!ch->eof && rt->n < RD_STARTS && pos > rt->pos[rt->n - 1]) { rt->pos[rt->n] = pos; rt->cookie[rt->n] = ck; rt->n++; }
        else if (!ch->eof && pos > rt->pos[rt->n - 1]) { rt->pos[rt->n - 1] = pos; rt->cookie[rt->n - 1] = ck; }
        break;
    }
    }
    R = 0;
    if (op->used && !op->inflight) run(o);
    else if (op->used && op->type == FS_READ) run(o);
}

/* UMNT answered (or failed): the end of FS_QUIT */
void umount_reply(struct call *c, struct res *r)
{
    (void)r;
    if (c->op < 0 || !N.ops[c->op].used || N.ops[c->op].type != FS_QUIT) return;
    int o = c->op;
    N.ops[o].step = 52;
    N.ops[o].deadline = 0;
    c->op = -1;
    run(o);
}
