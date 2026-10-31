/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's files (DESIGN D24 4, 5.3): PUT, GET, LS, DEL, MKDIR and SPEED. Regular files only, opened O_NONBLOCK;
 * paths at most 255 bytes; a network mount refused until D23's cancel mark (step 1). */
#include "wasabi_int.h"

#define ZEXE_MAGIC 0x5a455845u                  /* 'ZEXE', 'ZEX2' (kernel/kernel.h) */
#define ZEX2_MAGIC 0x5a455832u
#define S_IFMT  0170000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

static const char nfs_words[] = "files on a network mount are not served yet (they need D24 5.3's helper)";
static const char self_words[] = "that is the running daemon - a new wasabid is tried as a trial instance and shipped "
                                 "as a package";

static int get_path(struct conn *c, const uint8_t *p, uint32_t len, uint32_t *off, char *path, int empty_root)
{
    int e = wsb_getstr(p, len, off, path, WSB_PATHMAX + 1);
    if (e == -ENAMETOOLONG) {
        qerr(c, 0, 0, "the path is longer than 255 bytes");
        return -1;
    }
    if (e) {
        qerr(c, 0, 0, "bad path");
        return -1;
    }
    if (!path[0] && empty_root)
        memcpy(path, "/", 2);
    if (path_ok(path)) {
        qerr(c, 0, 0, "a Zenix path starts with /");
        return -1;
    }
    return 0;
}

static int xfer_take(struct conn *c)
{
    if (g.xbusy) {
        qerr(c, 0, 0, "busy: another file transfer is running");
        return -1;
    }
    if (fd_take(1)) {
        qerr(c, 0, 0, "busy: all descriptors in use");
        return -1;
    }
    g.xbusy = 1;
    return 0;
}
static void xfer_give(struct conn *c)
{
    if (c->xfd >= 0) {
        wsb_os_close(c->xfd);
        c->xfd = -1;
        fd_give(1);
        g.xbusy = 0;
    }
}

void xfer_abort(struct conn *c)
{
    int had = c->xfd >= 0;
    xfer_give(c);
    if (had && c->job == J_PUT)
        wsb_os_unlink(c->xtmp);
    if (had && c->job == J_SPEEDIN)
        wsb_os_unlink(c->xpath);
    if (c->job == J_SPEEDOUT && c->xpath[0])
        wsb_os_unlink(c->xpath);
    c->rx.stream = 0;
}

/* a body refused at its start: its DATA is taken and dropped, the ERR comes at END (Wasabi's way) */
static void sink(struct conn *c, int job, int e, const char *why)
{
    c->job = job;
    c->xfd = -1;
    c->xerr = e ? e : -EINVAL;
    c->xwhy = why;
    c->rx.stream = 1;
}

static int is_self(const char *path)
{
    char r[WSB_PATHMAX + 1];
    if (!strcmp(path, "/sbin/wasabid"))
        return 1;
    /* a path that cannot be resolved (a link loop, a link text longer than the resolver takes): -1, refused by the
     * caller, never replaced unchecked (audit 2026-10-29 B2-3) */
    if (wsb_os_realpath(path, r, sizeof r))
        return -1;
    return !strcmp(r, "/sbin/wasabid");
}

void cmd_put(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, size, prot;
    char path[WSB_PATHMAX + 1];
    struct wsb_stat st;
    int e;
    if (wsb_get32(p, len, &off, &size) || wsb_get32(p, len, &off, &prot)) {
        qerr(c, 0, 0, "bad PUT header");
        c->closing = 1;
        return;
    }
    e = wsb_getstr(p, len, &off, path, sizeof path);
    c->xsize = size;
    c->xgot = 0;
    c->xflags = 0;
    c->xprot = prot;
    c->xpath[0] = c->xtmp[0] = 0;
    if (e) { sink(c, J_PUT, 0, e == -ENAMETOOLONG ? "the path is longer than 255 bytes" : "bad path"); return; }
    if (path_ok(path)) { sink(c, J_PUT, 0, "a Zenix path starts with /"); return; }
    e = is_self(path);
    if (e < 0) { sink(c, J_PUT, -ELOOP, "the path cannot be resolved (a link loop or a link too long)"); return; }
    if (e) { sink(c, J_PUT, 0, self_words); return; }
    if (on_nfs(path)) { sink(c, J_PUT, 0, nfs_words); return; }
    if (!wsb_os_stat(path, &st) && (st.mode & S_IFMT) != S_IFREG) {
        sink(c, J_PUT, (st.mode & S_IFMT) == S_IFDIR ? -EISDIR : -EINVAL, "not a regular file");
        return;
    }
    uint32_t dl = (uint32_t)strlen(path);
    while (dl && path[dl - 1] != '/')
        dl--;
    if (dl + 12 > WSB_PATHMAX) { sink(c, J_PUT, 0, "the path is too long for the temporary name beside it"); return; }
    if (g.xbusy) { sink(c, J_PUT, 0, "busy: another file transfer is running"); return; }
    if (fd_take(1)) { sink(c, J_PUT, 0, "busy: all descriptors in use"); return; }
    e = -EEXIST;
    if (!g.tmpseq)                      /* not 1, 2, 3 at every start: leftovers of a killed PUT block no name (B2-4) */
        g.tmpseq = (g.now * 2654435761u) >> 12;
    for (int t = 0; t < 8 && e == -EEXIST; t++) {
        struct wsb_buf b = { c->xtmp, 0, sizeof c->xtmp };
        bsn(&b, path, dl);
        bs(&b, ".wsb-");
        bu(&b, ++g.tmpseq);
        e = wsb_os_open(c->xtmp, WSB_O_WRITE, 0600);
    }
    if (e < 0) {
        fd_give(1);
        sink(c, J_PUT, e, "cannot make the temporary file");
        return;
    }
    g.xbusy = 1;
    c->xfd = e;
    c->xerr = 0;
    c->xwhy = 0;
    memcpy(c->xpath, path, strlen(path) + 1);
    c->job = J_PUT;
    c->rx.stream = 1;
}

void body_piece(struct conn *c, const uint8_t *b, uint32_t n, int last)
{
    (void)last;
    for (uint32_t i = 0; i < n && c->xgot + i < 4; i++)
        c->xmagic[c->xgot + i] = b[i];
    c->xgot += n;
    if (c->xfd < 0 || c->xerr || !n)
        return;
    if (c->xgot > c->xsize) {
        c->xerr = -EINVAL;
        c->xwhy = "size mismatch";
    } else
        while (n) {
            long w = wsb_os_write(c->xfd, b, (long)n);
            if (w <= 0) {
                c->xerr = w < 0 ? (int)w : -ENOSPC;
                c->xwhy = c->job == J_SPEEDIN ? "write failed part way - is the volume full?" : "write failed";
                break;
            }
            b += w;
            n -= (uint32_t)w;
        }
    if (c->xerr) {
        xfer_give(c);
        wsb_os_unlink(c->job == J_PUT ? c->xtmp : c->xpath);
    }
}

void body_end(struct conn *c)
{
    struct wsb_stat st;
    int had = c->xfd >= 0;
    c->rx.stream = 0;
    xfer_give(c);
    if (c->xerr) {
        qerr(c, c->xwhy && c->xerr == -EINVAL ? 0 : c->xerr, 1, c->xwhy ? c->xwhy : "write failed");
        conn_jobdone(c);
        return;
    }
    if (c->xgot != c->xsize) {
        if (had)
            wsb_os_unlink(c->job == J_PUT ? c->xtmp : c->xpath);
        qerr(c, 0, 0, "size mismatch");
        conn_jobdone(c);
        return;
    }
    if (c->job == J_SPEEDIN) {                  /* the test file stays for the read-back half */
        qok(c);
        conn_jobdone(c);
        return;
    }
    uint32_t m = (uint32_t)c->xmagic[0] << 24 | (uint32_t)c->xmagic[1] << 16 | (uint32_t)c->xmagic[2] << 8 | c->xmagic[3];
    int zexe = c->xgot >= 4 && (m == ZEXE_MAGIC || m == ZEX2_MAGIC);
    uint32_t old = !wsb_os_stat(c->xpath, &st) && (st.mode & S_IFMT) == S_IFREG ? st.mode & 07777 : 0;
    int e = wsb_os_rename(c->xtmp, c->xpath);
    if (e) {
        wsb_os_unlink(c->xtmp);
        qerr(c, e, 1, "rename into place failed");
        conn_jobdone(c);
        return;
    }
    wsb_os_chmod(c->xpath, wsb_prot_to_mode(c->xprot, old, zexe));
    qok(c);
    conn_jobdone(c);
}

void cmd_get(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0;
    char path[WSB_PATHMAX + 1];
    struct wsb_stat st;
    st.mode = 0;
    if (get_path(c, p, len, &off, path, 0))
        return;
    if (on_nfs(path)) { qerr(c, 0, 0, nfs_words); return; }
    if (xfer_take(c))
        return;
    int fd = wsb_os_open(path, WSB_O_READ, 0);
    if (fd < 0) {
        fd_give(1);
        g.xbusy = 0;
        qerr(c, fd, 0, "cannot open for reading");
        return;
    }
    c->xfd = fd;
    if (wsb_os_fstat(fd, &st) || (st.mode & S_IFMT) != S_IFREG) {
        xfer_give(c);
        qerr(c, (st.mode & S_IFMT) == S_IFDIR ? -EISDIR : 0, 0, "not a regular file");
        return;
    }
    c->xpath[0] = 0;
    c->job = J_GET;
}

void cmd_del(struct conn *c, const uint8_t *p, uint32_t len, int mkdir)
{
    uint32_t off = 0;
    char path[WSB_PATHMAX + 1];
    struct wsb_stat st;
    int e;
    if (get_path(c, p, len, &off, path, 0))
        return;
    if (on_nfs(path)) { qerr(c, 0, 0, nfs_words); return; }
    if (mkdir)
        e = wsb_os_mkdir(path);
    else if ((e = wsb_os_lstat(path, &st)) == 0)
        e = (st.mode & S_IFMT) == S_IFDIR ? wsb_os_rmdir(path) : wsb_os_unlink(path);
    if (e)
        qerr(c, e, 1, mkdir ? "mkdir failed" : "delete failed");
    else
        qok(c);
}

/* LS: the names first (the directory closed at once: no descriptor held across turns), then a line each */
void cmd_ls(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, cap = 1024, n = 0;
    char path[WSB_PATHMAX + 1], name[64];
    if (get_path(c, p, len, &off, path, 1))
        return;
    if (on_nfs(path)) { qerr(c, 0, 0, nfs_words); return; }
    if (fd_take(1)) { qerr(c, 0, 0, "busy: all descriptors in use"); return; }
    int fd = wsb_os_opendir(path), r;
    if (fd < 0) {
        fd_give(1);
        qerr(c, fd, 0, "cannot list that path");
        return;
    }
    char *b = wsb_os_alloc(cap);
    c->bend = 0;
    while (b && (r = wsb_os_readdir(fd, name)) > 0) {
        name[63] = 0;
        uint32_t l = (uint32_t)strlen(name) + 1;
        if (n + l > cap) {
            char *nb = cap * 2 <= WSB_LSMAX ? wsb_os_alloc(cap * 2) : 0;
            if (!nb) {
                c->bend = "the directory has more names than LS sends (32 KB of names)";
                break;
            }
            memcpy(nb, b, n);
            wsb_os_free(b);
            b = nb;
            cap *= 2;
        }
        memcpy(b + n, name, l - 1);
        b[n + l - 1] = 0;
        n += l;
    }
    wsb_os_close(fd);
    fd_give(1);
    if (!b) {
        qerr(c, -ENOMEM, 0, "out of memory");
        return;
    }
    memcpy(c->xpath, path, strlen(path) + 1);
    c->buf = b;
    c->blen = n;
    c->bpos = 0;
    c->job = J_LS;
}

static void ls_line(struct conn *c, struct wsb_buf *w, const char *name)
{
    char full[WSB_PATHMAX + 2];
    struct wsb_stat st;
    struct wsb_buf f = { full, 0, sizeof full };
    uint32_t d, m, t;
    bs(&f, c->xpath);
    if (f.n > 1)
        bc(&f, '/');
    bs(&f, name);
    if (f.n > WSB_PATHMAX || (wsb_os_stat(full, &st) && wsb_os_lstat(full, &st)))
        memset(&st, 0, sizeof st);
    wsb_datestamp(st.mtime, &d, &m, &t);
    bc(w, (st.mode & S_IFMT) == S_IFDIR ? 'd' : 'f');
    bc(w, ' ');
    bu(w, st.size);
    bc(w, ' ');
    bu(w, wsb_mode_to_prot(st.mode));
    bc(w, ' ');
    bu(w, d);
    bc(w, ' ');
    bu(w, m);
    bc(w, ' ');
    bu(w, t);
    bc(w, ' ');
    bs(w, name);
    bc(w, '\n');
}

static void speed_end(struct conn *c)
{
    if (c->xfd >= 0) {
        xfer_give(c);
        wsb_os_unlink(c->xpath);                /* the read-back half tidies up */
    }
    c->xpath[0] = 0;
    qend(c);
    conn_jobdone(c);
}

void produce_files(struct conn *c)
{
    if (c->job == J_LS) {
        char line[160];
        for (;;) {
            uint32_t room = qroom(c);
            if (c->bpos >= c->blen) {
                if (c->bend ? qerr(c, 0, 0, c->bend) : qend(c))
                    return;
                conn_jobdone(c);
                return;
            }
            if (room < sizeof line)
                return;
            uint8_t *d = qres(c, room);
            uint32_t n = 0;
            while (c->bpos < c->blen) {
                struct wsb_buf w = { line, 0, sizeof line };
                ls_line(c, &w, c->buf + c->bpos);
                if (n + w.n > room)
                    break;
                memcpy(d + n, line, w.n);
                n += w.n;
                c->bpos += (uint32_t)strlen(c->buf + c->bpos) + 1;
            }
            qcommit(c, WT_DATA, n);
        }
    }
    while (c->job == J_GET || c->job == J_SPEEDOUT) {
        uint32_t room = qroom(c), want;
        if (room > WSB_SMALL)
            room = WSB_SMALL;
        if (room < 1024)
            return;
        uint8_t *d = qres(c, room);
        long n;
        if (c->job == J_GET) {
            n = wsb_os_read(c->xfd, d, (long)room);
            if (n == 0) {
                xfer_give(c);
                qend(c);
                conn_jobdone(c);
                return;
            }
            if (n < 0) {
                if (n == -EAGAIN)
                    return;
                xfer_give(c);
                qerr(c, (int)n, 0, "read failed");
                conn_jobdone(c);
                return;
            }
        } else {
            if (c->xgot >= c->xsize) {
                speed_end(c);
                return;
            }
            want = c->xsize - c->xgot < room ? c->xsize - c->xgot : room;
            if (c->xfd >= 0) {
                n = wsb_os_read(c->xfd, d, (long)want);
                if (n <= 0) {                   /* a short test file: END tells the truth */
                    speed_end(c);
                    return;
                }
            } else {
                for (uint32_t i = 0; i < want; i++)
                    d[i] = (uint8_t)(c->xgot + i);
                n = (long)want;
            }
            c->xgot += (uint32_t)n;
        }
        qcommit(c, WT_DATA, (uint32_t)n);
    }
}

void cmd_speed(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags, size;
    char target[WSB_PATHMAX + 1];
    int from = 0, e;
    if (wsb_get32(p, len, &off, &flags) || wsb_get32(p, len, &off, &size)) {
        qerr(c, 0, 0, "bad SPEED header");
        c->closing = 1;
        return;
    }
    from = flags & WS_FROMAMIGA;
    target[0] = 0;
    e = len > 8 ? wsb_getstr(p, len, &off, target, sizeof target) : 0;
    c->xsize = size;
    c->xgot = 0;
    c->xpath[0] = 0;
    c->xfd = -1;
    if (!size || size > WSB_SPEEDMAX) {
        if (from) qerr(c, 0, 0, "size must be 1 byte to 256 MB"); else sink(c, J_SPEEDIN, 0, "size must be 1 byte to 256 MB");
        return;
    }
    if (e) {
        if (from) qerr(c, 0, 0, "bad SPEED target"); else sink(c, J_SPEEDIN, 0, "bad SPEED target");
        return;
    }
    if (target[0]) {
        const char *why = 0;
        struct wsb_buf b = { c->xpath, 0, sizeof c->xpath };
        int nfs = 0, mi;
        struct wsb_statfs sf;
        bs(&b, target);
        if (b.n > 1)
            bc(&b, '/');
        bs(&b, "wasabi-speed.tmp");
        if (path_ok(target) || b.n > WSB_PATHMAX)
            why = "the target is a directory with a path of at most 46 bytes";
        else if ((mi = mount_of(target, &nfs)) < 0 || nfs)
            why = nfs ? nfs_words : "no filesystem there";
        else if (!from && (wsb_os_statfs((unsigned)mi, &sf) || !sf.bsize ||
                           sf.bfree < (size / sf.bsize) + (WSB_SPEEDROOM / sf.bsize) + 1))
            why = "not room for the size and 64 KB more there";
        else if (g.xbusy)
            why = "busy: another file transfer is running";
        if (why) {
            c->xpath[0] = 0;
            if (from) qerr(c, 0, 0, why); else sink(c, J_SPEEDIN, 0, why);
            return;
        }
        if (fd_take(1)) {
            c->xpath[0] = 0;
            if (from) qerr(c, 0, 0, "busy: all descriptors in use"); else sink(c, J_SPEEDIN, 0, "busy: all descriptors in use");
            return;
        }
        if (!from)
            wsb_os_unlink(c->xpath);
        e = wsb_os_open(c->xpath, from ? WSB_O_READ : WSB_O_WRITE, 0644);
        if (e < 0) {
            fd_give(1);
            c->xpath[0] = 0;
            if (from) qerr(c, e, 0, e == -ENOENT ? "no test file to read back - run the upload half first" : "cannot open the test file");
            else sink(c, J_SPEEDIN, e, "cannot create the test file there");
            return;
        }
        c->xfd = e;
        g.xbusy = 1;
    }
    c->xerr = 0;
    c->xwhy = 0;
    if (from)
        c->job = J_SPEEDOUT;
    else {
        c->job = J_SPEEDIN;
        c->rx.stream = 1;
    }
}
