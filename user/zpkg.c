/* SPDX-License-Identifier: GPL-2.0-only */
/* zpkg: the package manager (DESIGN D22, docs/d22-zpkg.md; docs/man/zpkg.txt). It fetches and decides; /sbin/zup
 * installs (zup -f FILE, zup -r NAME: one package, one transaction each, D22 4.2). The repository is plain files on
 * a web server (3.2): CHANNEL/latest, CHANNEL/index-SERIAL and its .sig, pool/NAME-VNUM.zup and its .sig. The list's
 * signature is checked before a line of it is parsed (5.1); every package against the list's size and SHA-256 while
 * it streams to the cache (7.5); the header's version and channel class; then zup checks everything again.
 * The cores are system-call free and host-tested: the list, latest and the trust rules (user/zpkg/index.c), the
 * resolver (user/zpkg/resolve.c), HTTP (net/http/), the package formats (user/zpkgfmt.c); this file gives them the
 * files, the sockets with their time limits, the lock, zup and the messages. Plain HTTP only for now (D22 step 5;
 * HTTPS in step 9); a system update is downloaded and named, its update slot comes in step 7. */
#include "socket.h"
#include "zpkg/zpkg.h"
#include "zpkgfmt.h"
#include "sha256.h"
#include "../net/http/http.h"
#include "../crypto/zc/zc.h"
#include "../crypto/zc/zc_clock.h"
#include "builddate.h"
#include "zpkgkeys.h"
#include "rdb.h"
#include "../kernel/diskproto.h"

typedef unsigned char u8;

#define DB       "/var/lib/zpkg"
#define SYNC     DB "/sync"
#define LOCK     DB "/lock"
#define DEPS     DB "/deps"
#define CACHE    "/var/cache/zpkg/pkg"
#define CONF     "/etc/zpkg.conf"
#define ZUP      "/sbin/zup"
#define CONNECT_MS 15000
#define IDLE_MS    60000
#define MAXSERVER  8
#define MAXHOLD    32
#define NEEDKB     160                  /* free memory zpkg wants before it starts (D22 7.2: measured in step 5) */

static int quiet;
static void say(const char *fmt, ...)
{
    va_list ap;
    char b[240];
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    dprintf(1, "zpkg: %s\n", b);
}
void zf_say(const char *fmt, ...)       /* user/zpkgfmt.c's reasons */
{
    va_list ap;
    char b[200];
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    say("%s", b);
}

/* ---- files ---- */
static char *slurp(const char *path, u32 *len, u32 max)     /* the whole file, NUL-terminated, or 0 */
{
    struct ustat st;
    if (sys_stat(path, &st) < 0 || (st.mode & S_IFMT) != S_IFREG || st.size > max) return 0;
    char *b = malloc(st.size + 1);
    int fd = sys_open(path, O_RDONLY);
    if (!b || fd < 0) { free(b); if (fd >= 0) sys_close(fd); return 0; }
    int n = sys_read(fd, b, st.size);
    sys_close(fd);
    if (n != (int)st.size) { free(b); return 0; }
    b[n] = 0;
    if (len) *len = (u32)n;
    return b;
}
static int write_all(int fd, const void *p, u32 n)
{
    while (n) {
        int w = sys_write(fd, p, n > 4096 ? 4096 : n);
        if (w <= 0) return w < 0 ? w : -EIO;
        p = (const u8 *)p + w; n -= (u32)w;
    }
    return 0;
}
static int write_atomic(const char *path, const void *p, u32 n)   /* PATH.new written, synced, renamed over PATH */
{
    char tmp[64];
    if (strlen(path) + 5 > sizeof tmp) return -ENAMETOOLONG;
    strcpy(tmp, path); strcat(tmp, ".new");
    int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC), r;
    if (fd < 0) return fd;
    r = write_all(fd, p, n);
    sys_close(fd);
    if (!r) { sys_sync(); r = sys_rename(tmp, path); }
    if (r) sys_unlink(tmp);
    return r;
}
static u8 iobuf[4096];                  /* sha_fd_range's and boot_sha's (never at once) */
static int sha_fd_range(int fd, u32 n, u8 out[32])
{
    u8 *buf = iobuf;
    struct sha256 c;
    sha256_init(&c);
    while (n) {
        int k = sys_read(fd, buf, n < sizeof iobuf ? n : sizeof iobuf);
        if (k <= 0) return -EIO;
        sha256_update(&c, buf, (u32)k);
        n -= (u32)k;
    }
    sha256_final(&c, out);
    return 0;
}
static int file_sha(const char *path, u32 *size, u8 out[32])
{
    struct ustat st;
    if (sys_stat(path, &st) < 0) return -ENOENT;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return fd;
    int r = sha_fd_range(fd, st.size, out);
    sys_close(fd);
    if (size) *size = st.size;
    return r;
}

/* ---- the configuration (/etc/zpkg.conf, a base config file: the words below, # comments, others ignored) ---- */
static struct {
    char channel[16];
    char *server[MAXSERVER];
    int nserver;
    const char *hold[MAXHOLD];
    int nhold;
    u32 cache_kb;
} cf;
static int read_conf(void)
{
    u32 n;
    char *t = slurp(CONF, &n, 16384);
    strcpy(cf.channel, ZPKG_KEYCLASS == ZP_KEY_TEST ? "test" : "testing");
    cf.cache_kb = 4096;
    if (!t) return 0;
    for (char *l = t, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        char *w[3]; int nw = 0;
        for (char *q = l; *q && nw < 3; ) {
            while (*q == ' ' || *q == '\t') *q++ = 0;
            if (!*q || *q == '#') break;
            w[nw++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
        }
        if (nw != 2) continue;
        if (!strcmp(w[0], "channel") && strlen(w[1]) < sizeof cf.channel) strcpy(cf.channel, w[1]);
        else if (!strcmp(w[0], "server")) {
            if (cf.nserver == MAXSERVER) say("%s: more than %d servers: the rest ignored", CONF, MAXSERVER);
            else cf.server[cf.nserver++] = w[1];
        } else if (!strcmp(w[0], "hold")) {
            if (cf.nhold == MAXHOLD) say("%s: more than %d holds: the rest ignored", CONF, MAXHOLD);
            else cf.hold[cf.nhold++] = w[1];
        } else if (!strcmp(w[0], "cache")) cf.cache_kb = (u32)strtoul(w[1], 0, 10);
    }
    return 0;
}

/* ---- the installed packages (D15's manifests, read through zpkgfmt.c) and the deps file ---- */
static struct zf_pkg **inst;
static int ninst;
static struct zp_inst *winst;
static char *depsbuf;
static int load_db(void)
{
    char **names;
    int n = dir_list(DB, &names);
    if (n < 0) { say("the package database %s: %s: nothing done", DB, zwhy("cannot read")); return -ENOENT; }
    inst = malloc(sizeof *inst * (u32)(n + 1));
    if (!inst) { dir_free(names, n); return -ENOMEM; }
    for (int i = 0; i < n; i++) {
        u32 l = strlen(names[i]), len;
        char path[64];
        if (l < 10 || strcmp(names[i] + l - 9, ".manifest") || l > 40) continue;
        snprintf(path, sizeof path, DB "/%s", names[i]);
        char *t = slurp(path, &len, 1u << 20);
        struct zf_pkg *k = malloc(sizeof *k);
        if (!t || !k || zf_manifest_parse(k, t, len, path)) {
            say("%s: the package database is damaged: nothing done", path);
            dir_free(names, n);
            return -EIO;
        }
        inst[ninst++] = k;
    }
    dir_free(names, n);
    u32 dl;
    depsbuf = slurp(DEPS, &dl, 65536);
    winst = calloc((u32)ninst + 1, sizeof *winst);
    if (!winst) return -ENOMEM;
    for (int i = 0; i < ninst; i++) {
        struct zp_inst *w = &winst[i];
        struct zp_dep *d = calloc((u32)inst[i]->ndep + 1, sizeof *d);
        if (!d) return -ENOMEM;
        w->name = inst[i]->name;
        w->vnum = inst[i]->vnum;
        w->base = inst[i]->group && !strcmp(inst[i]->group, "base");
        for (int j = 0; j < inst[i]->ndep; j++) { d[j].name = inst[i]->dep[j]; d[j].vnum = inst[i]->depv[j]; }
        w->ndep = inst[i]->ndep;
        w->dep = d;
        if (depsbuf) {                                  /* a name on a line of its own */
            u32 nl = strlen(w->name);
            for (char *p = depsbuf; (p = strstr(p, w->name)); p += nl)
                if ((p == depsbuf || p[-1] == '\n') && (p[nl] == '\n' || !p[nl])) { w->asdep = 1; break; }
        }
    }
    return 0;
}
static struct zf_pkg *inst_find(const char *name)
{
    for (int i = 0; i < ninst; i++) if (!strcmp(inst[i]->name, name)) return inst[i];
    return 0;
}
static u32 effbase, kvnum, updflags;
static void set_effbase(void)
{
    struct uupdate u;
    struct zf_pkg *b = inst_find("base");
    memset(&u, 0, sizeof u);
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) == 0) { kvnum = u.kvnum; updflags = u.flags; }
    effbase = b ? b->vnum : 0;
    if (kvnum && kvnum < effbase) effbase = kvnum;  /* 8.4.2: the smaller of the base and the running kernel */
}
/* the deps file after a change: name added (asdep) or taken out (explicit, or removed) */
static void deps_set(const char *name, int asdep)
{
    u32 n = depsbuf ? strlen(depsbuf) : 0, nl = strlen(name), o = 0;
    char *nb = malloc(n + nl + 2);
    if (!nb) return;
    for (char *l = depsbuf, *e; l && *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        u32 ll = (u32)(e - l);
        if (*e) e++;
        if (ll && !(ll == nl && !strncmp(l, name, nl))) { memcpy(nb + o, l, ll); nb[o + ll] = '\n'; o += ll + 1; }
    }
    if (asdep) { memcpy(nb + o, name, nl); nb[o + nl] = '\n'; o += nl + 1; }
    nb[o] = 0;
    if (write_atomic(DEPS, nb, o)) say("%s: cannot write it (the package counts as installed by hand)", DEPS);
    free(depsbuf);
    depsbuf = nb;
}

/* ---- the lock (D22 4.1, the same rule as zup's: a holder that is not a living zup or zpkg, while no other zup lives,
 * is stale); held while zpkg changes anything, and zup runs under it as our child ---- */
static int lock_mine;
static int proc_alive(int pid, struct uproc *p)
{
    for (u32 i = 1; sys_procinfo(i, p) >= 0; i++)
        if (p->state && p->state != 5 && (int)p->pid == pid) return 1;
    return 0;
}
static int is_pkgprog(const struct uproc *p)
{
    return !strcmp(p->name, "zup") || !strcmp(p->name, "zpkg") || !strcmp(p->name, "zpkg-new");   /* (D22 4.5's bootstrap) */
}
static int lock_take(void)
{
    int me = sys_getpid();
    char mine[48], b[16];
    snprintf(mine, sizeof mine, LOCK ".%d", me);
    for (int tries = 0; tries < 3; tries++) {
        int fd = sys_open(mine, O_WRONLY | O_CREAT | O_TRUNC), r;
        if (fd < 0) { say("cannot make %s (%s)", mine, zstrerror(fd)); return fd; }
        int n = snprintf(b, sizeof b, "%d\n", me);
        r = write_all(fd, b, (u32)n);
        sys_close(fd);
        if (r == 0) r = sys_link(mine, LOCK);           /* the lock appears whole, with its pid (zup's rule) */
        sys_unlink(mine);
        if (r == 0) { lock_mine = 1; return 0; }
        if (r != -EEXIST) { say("cannot make %s (%s)", LOCK, zstrerror(r)); return r; }
        u32 len;
        char *t = slurp(LOCK, &len, 64);
        int holder = t ? (int)strtoul(t, 0, 10) : 0;
        free(t);
        struct uproc p;
        if (holder > 0 && holder != me && proc_alive(holder, &p) && is_pkgprog(&p)) {
            say("another %s (pid %d) is changing packages: try again when it is done", p.name, holder);
            return -EBUSY;
        }
        for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++)
            if (p.state && p.state != 5 && is_pkgprog(&p) && (int)p.pid != me) {
                say("a %s (pid %d) is still changing packages: try again when it is done", p.name, (int)p.pid);
                return -EBUSY;
            }
        sys_unlink(LOCK);                               /* stale: taken over */
    }
    say("cannot take %s", LOCK);
    return -EBUSY;
}
static void lock_drop(void)
{
    if (lock_mine) { sys_unlink(LOCK); sys_sync(); }
    lock_mine = 0;
}

/* ---- HTTP over a socket, with time limits (plain http: only; https comes with D22 step 9) ---- */
struct conn {
    int fd;
    char host[HTTP_HOSTMAX + 1];
    unsigned port;
    unsigned char buf[4096];
    u32 have, off;
};
static struct conn cn;                                      /* fd set to -1 in main (bss, not data) */
static void conn_close(void)
{
    if (cn.fd >= 0) sys_close(cn.fd);
    cn.fd = -1;
    cn.have = cn.off = 0;
}
static int wait_fd(int fd, int ev, int ms)
{
    struct pollfd q;
    q.fd = fd; q.events = (short)ev; q.revents = 0;
    int r = poll(&q, 1, ms);
    return r < 0 ? -zerrno : r;
}
static int conn_open(const struct http_url *u, const char **why)
{
    static char msg[96];
    struct addrinfo h, *r = 0;
    char ps[8];
    int e, s;
    if (cn.fd >= 0 && !strcmp(cn.host, u->host) && cn.port == u->port) return 0;
    conn_close();
    if (u->tls) { *why = "https is not taken yet (D22 step 9): use an http:// server"; return -EPROTONOSUPPORT; }
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%u", u->port);
    if ((e = getaddrinfo(u->host, ps, &h, &r)) != 0) { *why = e == EAI_NONAME ? "no such host" : gai_strerror(e); return -ENOENT; }
    s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (s < 0) { freeaddrinfo(r); *why = "no socket (is the network up?)"; return -ENETDOWN; }
    e = connect(s, r->ai_addr, r->ai_addrlen) < 0 ? zerrno : 0;
    freeaddrinfo(r);
    if (e == EINPROGRESS) {
        int w = wait_fd(s, POLLOUT, CONNECT_MS);
        socklen_t el = sizeof e;
        if (w != 1) e = w == 0 ? ETIMEDOUT : -w;
        else if (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) < 0) e = zerrno;
    }
    if (e) {
        sys_close(s);
        snprintf(msg, sizeof msg, "connect to %s: %s", u->host, zstrerror(e) ? zstrerror(e) : "failed");
        *why = msg;
        return -e;
    }
    cn.fd = s;
    strcpy(cn.host, u->host);
    cn.port = u->port;
    return 0;
}
static int conn_fill(u32 deadline, const char **why)        /* bytes into cn.buf: > 0, 0 closed, < 0 error */
{
    for (;;) {
        u32 now = sys_uptime();
        if (now >= deadline) { *why = "the file took longer than its time"; return -ETIMEDOUT; }
        int k = recv(cn.fd, cn.buf, sizeof cn.buf, 0);
        if (k >= 0) { cn.off = 0; cn.have = (u32)k; return k; }
        if (zerrno != EAGAIN) { *why = "the connection failed"; return -zerrno; }
        u32 left = (deadline - now) * 1000;
        if (wait_fd(cn.fd, POLLIN, left < IDLE_MS ? (int)left : IDLE_MS) != 1) { *why = "the server stopped answering"; return -ETIMEDOUT; }
    }
}
static int conn_send(const char *s, u32 n)
{
    while (n) {
        int k = send(cn.fd, s, n, MSG_NOSIGNAL);
        if (k > 0) { s += k; n -= (u32)k; continue; }
        if (k < 0 && zerrno == EAGAIN && wait_fd(cn.fd, POLLOUT, IDLE_MS) == 1) continue;
        return -EPIPE;
    }
    return 0;
}

/* one URL into the file path (in the current directory: zpkg works in the cache and sync folders with short names):
 * written as PATH.part, resumed from it when total is known, the SHA-256 of the whole file into digest. At most
 * limit bytes are taken (the list's size: a server sending more is cut off) */
static int fetch(const char *url, const char *path, u32 limit, u32 total, u8 digest[32], u32 *size, const char **why)
{
    static struct http_resp r;
    static char req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    struct http_url u;
    struct sha256 c;
    char part[64];
    u32 from = 0, got = 0;
    int out, rc, hops = 0;
    if (http_url_parse(&u, url)) { *why = "a bad server address in " CONF; return -EINVAL; }
    if (strlen(path) + 6 > sizeof part) { *why = "the name is too long"; return -ENAMETOOLONG; }
    strcpy(part, path); strcat(part, ".part");
    sha256_init(&c);
    struct ustat st;
    if (total && sys_stat(part, &st) == 0 && st.size > 0 && st.size < total) {
        int fd = sys_open(part, O_RDONLY);         /* resume (7.5): the hash of what is there first */
        static u8 b[4096];
        int k;
        while (fd >= 0 && (k = sys_read(fd, b, sizeof b)) > 0) { sha256_update(&c, b, (u32)k); from += (u32)k; }
        if (fd >= 0) sys_close(fd);
    }
    if (from) say("%s: resuming at %u of %u bytes", path, from, total);
    out = sys_open(part, O_WRONLY | O_CREAT | (from ? O_APPEND : O_TRUNC));
    if (out < 0) { *why = "cannot write in the cache"; return out; }
    got = from;
    u32 deadline = sys_uptime() + 120 + (limit - from) / 1024;     /* a total per file: 2 min plus 1 s a KB */
    for (;;) {
        int tries, status = 0;
        for (tries = 0; tries < 2; tries++) {
            int reused = cn.fd >= 0 && !strcmp(cn.host, u.host) && cn.port == u.port;
            if ((rc = conn_open(&u, why)) < 0) goto fail;
            if ((rc = http_request(req, sizeof req, &u, from, 0)) < 0) { *why = "the request does not fit"; goto fail; }
            if (conn_send(req, (u32)rc) < 0) { conn_close(); if (reused) continue; *why = "the request could not be sent"; rc = -EPIPE; goto fail; }
            http_resp_init(&r, limit - from, 0);
            if (cn.off == cn.have) {
                rc = conn_fill(deadline, why);
                if (rc == 0 && reused) { conn_close(); continue; }
                if (rc <= 0) { conn_close(); if (!rc) *why = "the server closed the connection"; rc = rc ? rc : -EIO; goto fail; }
            }
            break;
        }
        if (tries == 2) { *why = "the connection keeps closing"; rc = -EIO; goto fail; }
        for (;;) {
            u32 used0;
            size_t used, bl;
            const unsigned char *b;
            rc = http_resp_feed(&r, cn.buf + cn.off, cn.have - cn.off, &used, &b, &bl);
            used0 = (u32)used;
            cn.off += used0;
            if (rc == HTTP_NEED) {
                rc = conn_fill(deadline, why);
                if (rc < 0) { conn_close(); goto fail; }
                if (rc == 0) {
                    rc = http_resp_eof(&r);
                    conn_close();
                    if (rc < 0) { *why = "the connection ended before the file did"; goto fail; }
                    break;
                }
                continue;
            }
            if (rc < 0) {
                *why = rc == HTTP_E2BIG ? "the server sent more than the list says" : "an answer zpkg does not take";
                conn_close();
                goto fail;
            }
            if (rc == HTTP_HEAD) {
                status = r.status;
                if (http_is_redirect(status) || (status != 200 && status != 206)) continue;
                if (from) {
                    int how = http_resume(&r, from, total);
                    if (how < 0) { *why = "a wrong Content-Range"; conn_close(); rc = how; goto fail; }
                    if (how == HTTP_RESTART) {              /* the whole file again */
                        say("%s: the server sent the whole file again (it ignored the range)", path);
                        sys_close(out);
                        out = sys_open(part, O_WRONLY | O_CREAT | O_TRUNC);
                        if (out < 0) { *why = "cannot write in the cache"; return out; }
                        sha256_init(&c);
                        got = from = 0;
                    }
                }
                continue;
            }
            if (rc == HTTP_DATA) {
                if (http_is_redirect(status) || (status != 200 && status != 206)) continue;
                if (write_all(out, b, (u32)bl)) { *why = "cannot write in the cache (full?)"; rc = -EIO; conn_close(); goto fail; }
                sha256_update(&c, b, (u32)bl);
                got += (u32)bl;
                continue;
            }
            if (rc == HTTP_END) break;
        }
        if (r.close) conn_close();
        if (http_is_redirect(status)) {
            if (!r.location[0]) { *why = "a redirect without a Location"; rc = HTTP_EPROTO; goto fail; }
            if ((rc = http_redirect(&u, r.location, &hops)) < 0) {
                *why = rc == HTTP_ELOOP ? "more than 3 redirects" : "a redirect zpkg does not follow"; goto fail;
            }
            continue;
        }
        if (status != 200 && status != 206) {
            sys_close(out);
            sys_unlink(part);
            *why = status == 404 ? "not on the server (404)" : "the server answered with an error";
            return -ENOENT;
        }
        break;
    }
    sys_close(out);
    sha256_final(&c, digest);
    *size = got;
    if ((rc = sys_rename(part, path)) < 0) { *why = "cannot rename it in the cache"; return rc; }
    return 0;
fail:
    sys_close(out);                                     /* .part stays: the next run resumes it */
    return rc < 0 ? rc : -EIO;
}

/* ---- the list: -Sy (4.3, 5.1-5.3) ---- */
static struct zp_index ix;
static char *ixtext;
static int have_ix;
static struct zp_trust tr;
static const char *snapshot;
static u32 ymd(u32 t)                                       /* seconds since 1970 -> YYYYMMDD */
{
    long z = (long)(t / 86400u) + 719468, era = z / 146097, doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    long y = yoe + era * 400 + (m <= 2);
    return (u32)(y * 10000 + m * 100 + d);
}
static void set_trust(void)
{
    char b[ZC_CLOCK_LINE];
    uint32_t t = 0;
    int fd = sys_open(ZC_CLOCK_PATH, O_RDONLY), n = 0, ok = 0;
    if (fd >= 0) { n = sys_read(fd, b, sizeof b); sys_close(fd); ok = n > 0 && zc_clock_parse(b, (size_t)n, &t) == 0; }
    tr.channel = snapshot ? 0 : cf.channel;
    tr.snapshot = snapshot;
    tr.keyclass = ZPKG_KEYCLASS;
    tr.floor = ymd(ZENIX_BUILD_DATE) * 100;
    tr.clockfloor = ymd(zc_clock_floor(ZENIX_BUILD_DATE, ok, t));
    int bits = sys_clockbits();
    tr.clock = bits > 0 && (bits & (CLOCK_RTC | CLOCK_NET)) ? ymd(sys_time()) : 0;
}
static char syncname[48];                                   /* CHANNEL.index or snapshot-NAME.index, in SYNC */
static const char *errname(int rc)
{
    switch (rc) {
    case ZP_ECHANNEL: return "not this channel's list";
    case ZP_ECLASS: return "signed with a key of the wrong kind for this channel";
    case ZP_EREPLAY: return "older than the list already here (a replay?)";
    case ZP_ESERIAL: return "its serial number is wrong";
    case ZP_EEXPIRED: return "expired";
    case ZP_ESNAPSHOT: return "not the snapshot asked for";
    }
    return "damaged";
}
static void say_expired(void)
{
    say("the package list expired on %u, and the clock says %u%s", ix.expires, zp_today(&tr),
        tr.clock > ix.expires ? ": check the date (zpkg --ignore-expiry goes on once)" : "");
}
/* A ZenixFS root runs the kernel with the ZenixFS server (DESIGN D34): its system is the list's "systemz" line. zenixboot
 * would refuse the other bundle on such a machine (DESIGN D19 step 1), so zpkg never fetches it there. */
static int root_zenixfs(void)
{
    struct umountinfo mi;
    return sys_mountinfo(0, &mi) == 0 && !strcmp(mi.type, "zenixfs");
}
static void sys_for_root(struct zp_index *x)
{
    if (x->sysz.file && root_zenixfs()) x->sys = x->sysz;
}
static int load_stored(void)
{
    u32 n;
    char path[64];
    snprintf(path, sizeof path, SYNC "/%s", syncname);
    char *t = slurp(path, &n, ZP_INDEX_MAX);
    const char *why;
    if (!t) return 0;
    if (zp_index_parse(&ix, t, n, &why)) { say("the stored package list is damaged (%s): fetched again", why); free(t); return 0; }
    sys_for_root(&ix);
    if (snapshot && zp_index_usable(&ix, &tr, 0, &why) == ZP_ESNAPSHOT) { zp_index_free(&ix); free(t); return 0; }
    ixtext = t;
    have_ix = 1;
    tr.stored = ix.serial;
    return 1;
}
static int use_stored(void)                                 /* its expiry, at every use */
{
    unsigned warn = 0;
    const char *why;
    if (!have_ix) { say("no package list yet: zpkg -Sy fetches it"); return -ENOENT; }
    int rc = zp_index_usable(&ix, &tr, &warn, &why);
    if (rc == ZP_EEXPIRED) { say_expired(); return rc; }
    if (rc) { say("the stored package list is %s", errname(rc)); return rc; }
    if (warn & ZP_W_CLOCK_BEHIND) say("the clock is behind the package list's date: check it");
    if (warn & ZP_W_UNCHECKED) say("snapshot %s: its age and expiry are not checked", snapshot);
    return 0;
}
static int sync_list(void)
{
    char url[512], dir[48];
    u8 digest[32];
    u32 size;
    const char *why = "";
    if (!cf.nserver) { say("no server configured: add a line \"server http://HOST:PORT/PATH\" to %s (man zpkg)", CONF); return -ENOENT; }
    if (sys_chdir(SYNC) < 0) { sys_mkdir(SYNC); if (sys_chdir(SYNC) < 0) { say("cannot use %s", SYNC); return -EIO; } }
    if (snapshot) snprintf(dir, sizeof dir, "snapshots/%s", snapshot); else strcpy(dir, cf.channel);
    for (int m = 0; m < cf.nserver; m++) {
        const char *base = cf.server[m];
        zp_u32 serial;
        u32 n;
        snprintf(url, sizeof url, "%s/%s/latest", base, dir);
        if (fetch(url, "latest.tmp", ZP_LATEST_MAX, 0, digest, &size, &why) < 0) { say("%s: %s", base, why); continue; }
        char *lb = slurp("latest.tmp", &n, ZP_LATEST_MAX);
        sys_unlink("latest.tmp");
        if (!lb || zp_latest_parse(lb, n, &serial)) { say("%s: a bad latest", base); free(lb); continue; }
        free(lb);
        if (!snapshot) {
            int d = zp_latest_decide(serial, tr.stored);
            if (d == ZP_LATEST_BEHIND) { say("%s: this mirror is behind (%u, the list here is %u)", base, serial, tr.stored); continue; }
            if (d == ZP_LATEST_SAME && have_ix) { if (!quiet) say("the package list is up to date (%u)", ix.serial); return use_stored(); }
        } else if (have_ix && serial == tr.stored) return use_stored();
        snprintf(url, sizeof url, "%s/%s/index-%u.sig", base, dir, serial);
        if (fetch(url, "sig.tmp", ZC_SIG_SIZE, 0, digest, &size, &why) < 0 || size != ZC_SIG_SIZE) {
            say("%s: the list's signature: %s", base, size != ZC_SIG_SIZE && !*why ? "not 80 bytes" : why);
            sys_unlink("sig.tmp");
            continue;
        }
        char *sig = slurp("sig.tmp", &n, ZC_SIG_SIZE);
        sys_unlink("sig.tmp");
        snprintf(url, sizeof url, "%s/%s/index-%u", base, dir, serial);
        if (!sig || fetch(url, "index.tmp", ZP_INDEX_MAX, 0, digest, &size, &why) < 0) {
            say("%s: the list: %s", base, why); free(sig); sys_unlink("index.tmp"); continue;
        }
        /* the signature over the bytes fetched, before a line is parsed (5.1) */
        int which = -1, rc = zc_sig_verify((const u8 *)sig, ZC_SIG_SIZE, ZC_SIG_LABEL_INDEX, digest, zpkg_keys, ZPKG_NKEYS, &which);
        if (rc != ZC_SIG_OK) { say("%s: the list's signature does not verify: refused", base); free(sig); sys_unlink("index.tmp"); continue; }
        char *list = slurp("index.tmp", &n, ZP_INDEX_MAX), *copy = list ? malloc(n + 1) : 0;
        sys_unlink("index.tmp");
        struct zp_index nx;
        unsigned warn = 0;
        if (!copy) { free(list); free(sig); say("out of memory"); return -ENOMEM; }
        memcpy(copy, list, n + 1);
        if (zp_index_parse(&nx, copy, n, &why)) { say("%s: the list is refused: %s", base, why); free(copy); free(list); free(sig); continue; }
        sys_for_root(&nx);
        if (nx.serial != serial) { say("%s: the list's serial is not latest's", base); zp_index_free(&nx); free(copy); free(list); free(sig); continue; }
        rc = zp_index_check(&nx, &tr, &warn, &why);
        if (rc) {
            if (rc == ZP_EEXPIRED) say("%s: the package list expired on %u, and the clock says %u%s", base, nx.expires, zp_today(&tr),
                                       tr.clock > nx.expires ? ": check the date (zpkg --ignore-expiry goes on once)" : "");
            else say("%s: the list is refused: %s", base, errname(rc));
            zp_index_free(&nx); free(copy); free(list); free(sig);
            continue;
        }
        /* stored: the .sig, then the list written as .new and renamed, the list last (4.1) */
        char sp[64];
        snprintf(sp, sizeof sp, "%s.sig", syncname);
        if (write_atomic(sp, sig, ZC_SIG_SIZE) || write_atomic(syncname, list, n)) say("cannot store the list (the disk is full?)");
        free(sig); free(list);
        if (have_ix) { zp_index_free(&ix); free(ixtext); }
        ix = nx; ixtext = copy; have_ix = 1; tr.stored = ix.serial;
        say("a new package list: %u of %u%s%s", ix.serial, ix.date, warn & ZP_W_CLOCK_BEHIND ? " (the clock is behind its date: check it)" : "",
            warn & ZP_W_UNCHECKED ? " (a snapshot: its age and expiry not checked)" : "");
        return 0;
    }
    if (have_ix) { say("no server answered with a newer list: the stored one is used"); return use_stored(); }
    say("no server answered with a usable package list");
    return -ENOENT;
}

/* ---- downloads into the cache (7.5): checked against the list while they stream, again before use ---- */
static int cached_ok(const char *file, u32 fsize, const char *sha)
{
    u8 want[32], got[32];
    u32 n;
    if (zp_hex32(want, sha) || file_sha(file, &n, got)) return 0;
    if (n == fsize && !memcmp(got, want, 32)) return 1;
    sys_unlink(file);
    return 0;
}
/* the header and table of a bundle file: its package name (from the part names), version and channel */
static int read_head(const char *file, struct zf_bundle *b, char pname[25], int *system)
{
    static u8 hdr[ZF_HDR + ZF_MAXTOC * ZF_TOCE];
    struct ustat st;
    unsigned need = ZF_HDR, have = 0;
    int fd = sys_open(file, O_RDONLY), r = -EINVAL;
    if (fd < 0 || sys_fstat(fd, &st) < 0) { if (fd >= 0) sys_close(fd); return -ENOENT; }
    for (;;) {
        int n = need <= sizeof hdr ? sys_read(fd, hdr + have, need - have) : -1;
        if (n < 0 || (u32)n != need - have) break;
        have = need;
        if ((r = zf_bundle_head(b, hdr, have, st.size, &need)) != -EAGAIN) break;
        r = -EINVAL;
    }
    sys_close(fd);
    if (r) return r;
    pname[0] = 0;
    *system = 0;
    for (unsigned i = 0; i < b->ntoc; i++) {
        char name[41], pn[25];
        unsigned off, size;
        const unsigned char *sha;
        if (zf_bundle_entry(b, i, name, &off, &size, &sha)) return -EINVAL;
        int k = zf_part_kind(name, pn);
        if (k < 0) return -EINVAL;
        if (!k) { if (!strcmp(name, "boot/zenix")) *system = 1; continue; }
        if (*system || !pname[0]) strcpy(pname, pn);
        else if (strcmp(pname, pn) && !*system) *system = 2;        /* more than one package: a system bundle */
        if (*system == 1 && strcmp(pn, "zup") && strcmp(pn, "zpkg") && strcmp(pn, "base")) return -EPERM;
    }
    return 0;
}
static int fetching;                    /* zpkg --fetch (DESIGN D35): the files are for another machine's root, not this one's base */
/* the file of a list's block (p, or the system bundle with p 0) into the cache: 0, or < 0 (said) */
static int download(const struct zp_pkg *p)
{
    const char *file = p ? p->file : ix.sys.file, *sha = p ? p->sha : ix.sys.sha, *why = "";
    u32 fsize = p ? p->fsize : ix.sys.fsize, size;
    char url[512], sigf[48];
    u8 want[32], digest[32];
    int ok = cached_ok(file, fsize, sha);
    if (zp_hex32(want, sha)) return -EINVAL;
    for (int m = 0; m < cf.nserver && !ok; m++) {
        snprintf(url, sizeof url, "%s/pool/%s", cf.server[m], file);
        for (int attempt = 0; attempt < 2 && !ok; attempt++) {
            struct ustat st;
            int resumed = sys_stat(file, &st) < 0 && strlen(file) < 40 && ({ char pp[48]; snprintf(pp, sizeof pp, "%s.part", file); sys_stat(pp, &st) == 0; });
            if (fetch(url, file, fsize, fsize, digest, &size, &why) < 0) { say("%s: %s: %s", file, cf.server[m], why); break; }
            if (size != fsize || memcmp(digest, want, 32)) {
                say("%s: %s: its size or SHA-256 is not the list's%s", file, cf.server[m], resumed ? " (after a resume): fetched again" : ": refused");
                sys_unlink(file);
                if (!resumed) break;
                continue;
            }
            ok = 1;
        }
    }
    if (!ok) return -ENOENT;
    struct zf_bundle b;
    char pname[25];
    int sys = 0, r = read_head(file, &b, pname, &sys);
    const char *w2 = "";
    if (r || (p && sys) || (!p && sys != 1) || zp_bundle_fits(&ix, p, p ? pname : 0, b.vnum, b.channel, ZPKG_KEYCLASS, &w2)) {
        say("%s: refused: %s", file, r ? "not a package bundle" : w2[0] ? w2 : "not what the list says");
        sys_unlink(file);
        return -EINVAL;
    }
    if (!p && b.minv > effbase && !fetching) { say("%s: it needs base %u, %u is installed: refused", file, b.minv, effbase); return -EINVAL; }
    snprintf(sigf, sizeof sigf, "%s.sig", file);        /* beside it for -U and --back (not verified now: 5.1) */
    struct ustat st;
    if (sys_stat(sigf, &st) == 0 && st.size == ZC_SIG_SIZE) return 0;
    for (int m = 0; m < cf.nserver; m++) {
        snprintf(url, sizeof url, "%s/pool/%s.sig", cf.server[m], file);
        if (fetch(url, sigf, ZC_SIG_SIZE, 0, digest, &size, &why) == 0 && size == ZC_SIG_SIZE) return 0;
        sys_unlink(sigf);
    }
    say("%s: its .sig is not on the server (kept without it: --back and -U need it)", file);
    return 0;
}

/* ---- zup ---- */
static int run_zup(const char *a1, const char *a2, const char *a3)
{
    char *argv[] = { ZUP, (char *)a1, (char *)a2, (char *)a3, 0 };
    int pid = sys_spawn(ZUP, argv, 0);
    if (pid < 0) { say("cannot start %s (%s)", ZUP, zstrerror(pid)); return pid; }
    int st = sys_wait(pid);
    return st == 0 ? 0 : -EIO;
}
/* the programs of a package that run now keep running the old version (D13): named */
static void say_running(const char *name)
{
    struct uproc p;
    struct zf_pkg *k = 0;
    char path[64];
    u32 len;
    snprintf(path, sizeof path, DB "/%s.manifest", name);
    char *t = slurp(path, &len, 1u << 20);
    if (!t) return;
    k = malloc(sizeof *k);
    if (!k || zf_manifest_parse(k, t, len, path)) { free(k); free(t); return; }
    for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++) {
        if (!p.state || p.state == 5 || !p.ppid || (int)p.pid == sys_getpid()) continue;   /* (the kernel's own: boot, diskd) */
        for (int j = 0; j < k->nent; j++) {
            const char *b = strrchr(k->ent[j].path, '/');
            if (k->ent[j].kind == 'f' && b && !strncmp(b + 1, p.name, 15) && strlen(b + 1) <= 15) {
                say("%s (pid %u) still runs the old version until it is started again", p.name, p.pid);
                break;
            }
        }
    }
    zf_pkg_free(k);
    free(k);
}

/* ---- the system update onto the boot partition (D22 4.5, as built 2026-10-31; user/zpkg/bootfs.c) ----
 * zenixboot (under Kickstart's own dos, at every start from the AmigaDOS boot partition) installs
 * Zenix/zenix-update.zup and its .sig (D15, D21 6.4). zpkg writes those two files there itself: the partition is
 * found (exactly one FFS partition of the RDB with a Zenix folder holding a kernel), the whole volume checked, the
 * .sig and then the bundle written as new files in the safe order, then read back from the disk and checked. */
static const char B_ZUP[] = "zenix-update.zup", B_SIG[] = "zenix-update.zup.sig";
static int dport = -1;
struct dpart { int h; };
static int dmsg(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    int r = sys_call(dport, &m);
    return r == -ECONNRESET ? sys_call(dport, &m) : r;     /* a restarted diskd kept every handle (diskcall.h) */
}
static int dk_read(void *u, zp_u32 b, unsigned char *buf) { return dmsg(DISK_READ, (u32)((struct dpart *)u)->h, b, 1, (u32)buf) < 0 ? -EIO : 0; }
static int dk_write(void *u, zp_u32 b, unsigned char *buf) { return dmsg(DISK_WRITE, (u32)((struct dpart *)u)->h, b, 1, (u32)buf) < 0 ? -EIO : 0; }
static int dk_flush(void *u) { return dmsg(DISK_FLUSH, (u32)((struct dpart *)u)->h, 0, 0, 0) < 0 ? -EIO : 0; }
static int rdb_h;
static u32 rdb_base;
static int rdb_rd(unsigned lba, void *buf) { return dmsg(DISK_READ, (u32)rdb_h, rdb_base + lba, 1, (u32)buf) < 0 ? -1 : 0; }

#define DT_RDB_IN_MBR 0x52444200u       /* diskd: MBR type 0x76, an Amiga RDB inside (Emu68's convention) */
struct bootpart { char name[16]; struct zp_ffs f; struct dpart d; };
/* the boot partition: 0 (bp filled, opened for reading), or < 0 with the reason said */
static int boot_find2(struct bootpart *bp, struct diskinfo *di, struct rdb *rr, char *why2);
static int boot_find(struct bootpart *bp)
{
    static char why2[240];
    struct diskinfo *di = malloc(sizeof *di * 20);          /* (on the heap: zpkg must still load on 1 MB machines) */
    struct rdb *rp_ = malloc(sizeof *rp_);
    int rc = boot_find2(bp, di, rp_, why2);
    free(di);
    free(rp_);
    return rc;
}
static int boot_find2(struct bootpart *bp, struct diskinfo *di, struct rdb *rr, char *why2)
{
#define r (*rr)
    struct bootpart c;
    int n = 0, found = 0, hd = -1;
    const char *why = 0;
    if (!di || !rr) { say("out of memory"); return -ENOMEM; }
    why2[0] = 0;
    dport = sys_port_find("disk");
    if (dport < 0) { say("no disk server: the system update cannot be put on a boot partition"); return -ENODEV; }
    while (n < 20 && dmsg(DISK_LISTSZ, (u32)n, (u32)&di[n], sizeof di[n], 0) >= 0) n++;
    for (int i = 0; i < n; i++) if (!strcmp(di[i].name, "ide0")) hd = dmsg(DISK_OPEN, (u32)di[i].name, 0, 0, 0);
    for (int i = 0; hd >= 0 && i < n; i++) {
        if (strcmp(di[i].name, "ide0") && di[i].dostype != DT_RDB_IN_MBR) continue;
        rdb_h = hd; rdb_base = strcmp(di[i].name, "ide0") ? di[i].start : 0;
        if (rdb_read(rdb_rd, &r) != 1) continue;
        for (int k = 0; k < r.nparts; k++) {
            const struct rdb_part *rp = &r.part[k];
            if ((rp->dostype != ZP_DOS1 && rp->dostype != ZP_DOS3) || !rp->sectors) continue;
            for (int j = 0; j < n; j++) {
                if (di[j].start != rdb_base + rp->start || di[j].sectors != rp->sectors || di[j].dostype != rp->dostype) continue;
                struct zp_bootvol v;
                memset(&c, 0, sizeof c);
                strcpy(c.name, di[j].name);
                c.d.h = dmsg(DISK_OPEN, (u32)di[j].name, 0, 0, 0);
                if (c.d.h < 0) continue;
                c.f.blocksize = rp->blksize; c.f.reserved = rp->reserved; c.f.dostype = rp->dostype;
                c.f.nblocks = di[j].sectors; c.f.canflush = (di[j].caps & DISKCAP_FLUSH) != 0;
                c.f.read = dk_read; c.f.flush = dk_flush; c.f.u = 0;
                struct zp_ffs f = c.f;
                f.u = &c.d;
                const char *w = 0;
                if (zp_boot_open(&f, "Zenix", &v, &w) == 0) {
                    struct zp_bootfile bf;
                    int kern = 0;
                    static const char *const kn[3] = { "zenix", "zenix.new", "zenix.old" };   /* zenixboot's own rule */
                    for (int q = 0; q < 3 && !kern; q++) { kern = zp_boot_find(&v, kn[q], &bf, 0) == 0; zp_boot_file_free(&bf); }
                    zp_boot_close(&v);
                    if (!kern) continue;
                    if (found++) { say("two AmigaDOS partitions with a Zenix folder (%s, %s): zpkg does not choose one", bp->name, c.name); return -EEXIST; }
                    *bp = c;
                } else {                            /* (every partition it could not check: said if none is found) */
                    u32 l = (u32)strlen(why2);
                    snprintf(why2 + l, 240 - l, "%s%s (%s): %s", l ? "; " : "", di[j].name, di[j].label, w ? w : "refused");
                    why = why2;
                }
            }
        }
    }
    if (found) { bp->f.u = &bp->d; return 0; }
    if (why) {
        say("no usable AmigaDOS boot partition with a Zenix folder was found; these could not be checked: %s. If one "
            "of them is the boot partition, that is why nothing is written; start AmigaOS on it once (it checks and "
            "repairs its volumes) and try again", why);
        return -EINVAL;
    }
    say("no AmigaDOS boot partition with a Zenix folder (zenixboot's) was found, so the system (the kernel and the "
        "base) is not updated here: this Zenix was not started by zenixboot from a disk (a floppy start?). Programs "
        "update as before; the system needs a start from a boot partition (man zenixboot)");
    return -ENOENT;
#undef r
}
struct fsrc { int fd; };
static int file_src(void *u, zp_u32 off, unsigned char *buf, zp_u32 n)
{
    struct fsrc *s = u;
    if (sys_lseek(s->fd, (int)off, 0) < 0) return -EIO;
    return sys_read(s->fd, buf, n) == (int)n ? 0 : -EIO;
}
static int boot_sha(const struct zp_bootvol *v, const struct zp_bootfile *bf, u8 out[32])
{
    u8 *b = iobuf;
    struct sha256 c;
    sha256_init(&c);
    for (u32 off = 0; off < bf->size; ) {
        u32 k = bf->size - off < sizeof iobuf ? bf->size - off : sizeof iobuf;
        if (zp_boot_read(v, bf, off, b, k)) return -EIO;
        sha256_update(&c, b, k);
        off += k;
    }
    sha256_final(&c, out);
    return 0;
}
/* the system bundle FILE (in the cache, with FILE.sig) onto the boot partition: 0, or 1 (said). verified: its .sig
 * was checked just now (-U) */
static int verify_file(const char *file, struct zf_bundle *b, char pname[25], int *sys);
static int system_put(const char *file, int verified)
{
    struct zf_bundle b;
    char pn[25], sigf[56];
    int sys = 0, rc = 1;
    u32 size, ksize = 0;
    u8 sha[32], sig[ZC_SIG_SIZE], got[32];
    struct bootpart bp;
    struct zp_bootvol v;
    struct zp_bootfile bf;
    const char *why = "";
    if (verified ? read_head(file, &b, pn, &sys) : verify_file(file, &b, pn, &sys)) return 1;  /* the .sig: as zenixboot will */
    if (sys != 1) { say("%s: not a system bundle", file); return 1; }
    for (unsigned i = 0; i < b.ntoc; i++) {
        char name[41]; unsigned off, sz; const unsigned char *h;
        if (zf_bundle_entry(&b, i, name, &off, &sz, &h) == 0 && !strcmp(name, "boot/zenix")) ksize = sz;
    }
    snprintf(sigf, sizeof sigf, "%s.sig", file);
    int fd = sys_open(sigf, O_RDONLY);
    int sn = fd >= 0 ? sys_read(fd, sig, sizeof sig) : -1;
    if (fd >= 0) sys_close(fd);
    if (sn != ZC_SIG_SIZE || file_sha(file, &size, sha)) { say("%s: cannot read it and its .sig", file); return 1; }
    if (boot_find(&bp)) return 1;
    int h = dmsg(DISK_OPEN, (u32)bp.name, DISK_RAW, 0, 0);
    if (h < 0) { say("%s: cannot open it for writing (%s)", bp.name, h == -EBUSY ? "something has it mounted" : zstrerror(h)); return 1; }
    bp.d.h = h;
    bp.f.write = dk_write;
    if (zp_boot_open(&bp.f, "Zenix", &v, &why)) { say("%s, the boot partition: %s: nothing written", bp.name, why); return 1; }
    if (zp_boot_find(&v, B_ZUP, &bf, &why) == 0) {         /* one there already: the same, or restart first */
        u8 hv[4] = { 0, 0, 0, 0 }, s2[ZC_SIG_SIZE];
        int same = bf.size == size && boot_sha(&v, &bf, got) == 0 && !memcmp(got, sha, 32);
        zp_boot_read(&v, &bf, 12, hv, 4);                   /* (its header's version, for the message) */
        zp_boot_file_free(&bf);
        same = same && zp_boot_find(&v, B_SIG, &bf, &why) == 0 && bf.size == ZC_SIG_SIZE &&
               zp_boot_read(&v, &bf, 0, s2, ZC_SIG_SIZE) == 0 && !memcmp(s2, sig, ZC_SIG_SIZE);
        zp_boot_file_free(&bf);
        zp_boot_close(&v);
        if (same) { say("Zenix %s (%u) is on the boot partition already: it is installed at the next restart (reboot)", b.vstr, b.vnum); return 0; }
        say("%s: Zenix/%s holds Zenix %u, which zenixboot has not finished with (it retires it once it knows that "
            "system came up, at most three starts later): restart, then zpkg -Syu again", bp.name, B_ZUP,
            (u32)hv[0] << 24 | (u32)hv[1] << 16 | (u32)hv[2] << 8 | hv[3]);
        return 1;
    }
    int havesig = zp_boot_find(&v, B_SIG, &bf, &why) == 0;
    /* room for both files, and for zenixboot's own steps after them (zenix.new, the state file) */
    u32 need = zp_boot_blocks(size) + (havesig ? 0 : zp_boot_blocks(ZC_SIG_SIZE)) + zp_boot_blocks(ksize) + 16;
    if (v.nfree < need) {
        say("%s: the boot partition has %u KB free, the update needs %u KB there: nothing written", bp.name, v.nfree / 2, need / 2);
        goto out;
    }
    if (v.nleak) say("%s: %u blocks of the boot partition were in use with no file (a cut write): given back now", bp.name, v.nleak);
    u32 t = sys_clockbits() > 0 && (sys_clockbits() & (CLOCK_RTC | CLOCK_NET)) ? sys_time() : ZENIX_BUILD_DATE;
    u32 secs = t > 252460800u ? t - 252460800u : 0;
    zp_u32 date[3] = { secs / 86400, secs % 86400 / 60, secs % 60 * 50 };
    say("writing Zenix %s (%u) to the boot partition (%s, Zenix/%s, %u KB)", b.vstr, b.vnum, bp.name, B_ZUP, (size + 1023) / 1024);
    if (havesig) {                                          /* left by a cut between the two files: its one block again */
        int r = bf.size == ZC_SIG_SIZE ? zp_boot_pwrite(&v, &bf, 0, sig, ZC_SIG_SIZE) : -EINVAL;
        zp_boot_file_free(&bf);
        if (r) { say("%s: Zenix/%s is there and cannot be written (%s): nothing more written", bp.name, B_SIG, r == -EINVAL ? "not 80 bytes" : "a write error"); goto out; }
    } else {
        struct fsrc ss;
        int sfd = sys_open(sigf, O_RDONLY);
        ss.fd = sfd;
        int r = sfd < 0 ? -EIO : zp_boot_create(&v, B_SIG, ZC_SIG_SIZE, file_src, &ss, date, &why);
        if (sfd >= 0) sys_close(sfd);
        if (r) { say("%s: Zenix/%s: %s: nothing more written", bp.name, B_SIG, why); goto out; }
    }
    {
        struct fsrc bs;
        bs.fd = sys_open(file, O_RDONLY);
        int r = bs.fd < 0 ? -EIO : zp_boot_create(&v, B_ZUP, size, file_src, &bs, date, &why);
        if (bs.fd >= 0) sys_close(bs.fd);
        if (r) { say("%s: Zenix/%s: %s", bp.name, B_ZUP, why); goto out; }
    }
    zp_boot_close(&v);
    /* read back from the disk, through a new walk of the whole volume */
    bp.f.write = 0;
    if (zp_boot_open(&bp.f, "Zenix", &v, &why)) { say("%s: the boot partition after the write: %s", bp.name, why); return 1; }
    int ok = zp_boot_find(&v, B_ZUP, &bf, &why) == 0 && bf.size == size && boot_sha(&v, &bf, got) == 0 && !memcmp(got, sha, 32);
    zp_boot_file_free(&bf);
    u8 sig2[ZC_SIG_SIZE];
    ok = ok && zp_boot_find(&v, B_SIG, &bf, &why) == 0 && bf.size == ZC_SIG_SIZE && zp_boot_read(&v, &bf, 0, sig2, ZC_SIG_SIZE) == 0 &&
         !memcmp(sig2, sig, ZC_SIG_SIZE);
    zp_boot_file_free(&bf);
    if (!ok) { say("%s: the update does not read back right from the boot partition: zenixboot will refuse it", bp.name); goto out; }
    say("Zenix %s (%u) is ready on the boot partition: it is installed at the next restart (reboot)", b.vstr, b.vnum);
    rc = 0;
out:
    zp_boot_close(&v);
    return rc;
}

/* ---- -S, -Syu, -Syuu, -Sw ---- */
static int names_ok(char **names, int n)
{
    for (int i = 0; i < n; i++) if (!zf_name_ok(names[i])) { say("%s: not a package name", names[i]); return 0; }
    return 1;
}
static void world(struct zp_world *w)
{
    w->ix = &ix; w->inst = winst; w->ninst = ninst; w->effbase = effbase; w->hold = cf.hold; w->nhold = cf.nhold;
}
static void say_plan_error(const struct zp_plan *pl, const char *why)
{
    if (pl->bad2) say("%s: %s (%s)", pl->bad ? pl->bad : "?", why, pl->bad2);
    else say("%s: %s", pl->bad ? pl->bad : "the plan", why);
}
static int changes_allowed(int ram)
{
    struct umountinfo mi;
    if (updflags & UPD_F_TRIAL) {
        say("a new system is on trial (not confirmed yet): nothing is installed now, \"Zenix (previous)\" would break it");
        return 0;
    }
    if (sys_mountinfo(0, &mi) == 0) {
        if (!strcmp(mi.dev, "ram0") && !ram) {
            say("the root is a RAM disk: what zpkg installs would be gone at the next start (--ram to go on anyway)");
            return 0;
        }
    }
    return 1;
}
static int cmd_sync(char **names, int n, int y, int u, int down_only, int ram)
{
    int rc;
    struct zp_plan pl;
    const char *why;
    if (y && (rc = sync_list())) return 1;
    if (!y && use_stored()) return 1;
    if (!u && !n && !down_only) return 0;               /* -Sy alone */
    if (!names_ok(names, n)) return 1;
    struct zp_world w;
    world(&w);
    memset(&pl, 0, sizeof pl);
    if ((rc = zp_plan_sync(&w, (const char *const *)names, n, u >= 2 ? ZP_SYNC_UU : u ? ZP_SYNC_U : 0, &pl, &why))) {
        say_plan_error(&pl, why);
        return 1;
    }
    int todo = 0, waits = 0;
    for (int i = 0; i < pl.n; i++) {
        struct zp_step *s = &pl.step[i];
        if (s->kind == ZP_STEP_HELD) say("%s: held (%s)", s->pkg ? s->pkg->name : "?", s->why ? s->why : CONF);
        else if (s->kind == ZP_STEP_INSTALL || s->kind == ZP_STEP_UPGRADE || s->kind == ZP_STEP_DOWNGRADE) todo++;
        else if (s->kind == ZP_STEP_WAIT) waits++;
    }
    if (!todo && !waits && !pl.system) { say("nothing to do"); zp_plan_free(&pl); return 0; }
    if ((todo || waits || pl.system) && !changes_allowed(ram)) { zp_plan_free(&pl); return 1; }
    if (pl.system && root_zenixfs() && !ix.sysz.file) {
        say("the list has no system for a ZenixFS root (a kernel with the ZenixFS server): nothing installed");
        zp_plan_free(&pl);
        return 1;
    }
    if (sys_chdir(CACHE) < 0) { sys_mkdir("/var/cache/zpkg"); sys_mkdir(CACHE); if (sys_chdir(CACHE) < 0) { say("cannot use %s", CACHE); return 1; } }
    /* everything downloaded and verified first, the connection closed, then the manifests checked (4.3) */
    int bad = 0;
    for (int i = 0; i < pl.n; i++) {
        struct zp_step *s = &pl.step[i];
        if (s->kind == ZP_STEP_INSTALL || s->kind == ZP_STEP_UPGRADE || s->kind == ZP_STEP_DOWNGRADE || s->kind == ZP_STEP_WAIT)
            if (download(s->pkg)) bad++;
    }
    if (pl.system && download(0)) bad++;
    conn_close();
    if (bad) { say("%d file%s could not be fetched: nothing installed", bad, bad > 1 ? "s" : ""); zp_plan_free(&pl); return 1; }
    for (int i = 0; i < pl.n; i++) {                    /* the manifests' dependencies are the list's */
        struct zp_step *s = &pl.step[i];
        if (s->kind != ZP_STEP_INSTALL && s->kind != ZP_STEP_UPGRADE && s->kind != ZP_STEP_DOWNGRADE) continue;
        struct zf_bundle b;
        char pn[25];
        int sys;
        if (read_head(s->pkg->file, &b, pn, &sys)) { bad++; continue; }
        /* (zup checks every file and path again; here the declared dependencies against the signed list) */
        for (unsigned t = 0; t < b.ntoc; t++) {
            char name[41], pk[25];
            unsigned off, size;
            const unsigned char *sha;
            if (zf_bundle_entry(&b, t, name, &off, &size, &sha) || zf_part_kind(name, pk) != 1 || size > (1u << 20)) continue;
            int fd = sys_open(s->pkg->file, O_RDONLY);
            char *text = malloc(size + 1);
            struct zf_pkg m;
            u8 h[32];
            if (fd < 0 || !text || sys_lseek(fd, (int)off, 0) < 0 || sys_read(fd, text, size) != (int)size) { if (fd >= 0) sys_close(fd); free(text); bad++; break; }
            sys_close(fd);
            text[size] = 0;
            sha256(text, size, h);
            if (memcmp(h, sha, 32) || zf_manifest_parse(&m, text, size, s->pkg->name)) { free(text); bad++; break; }
            int same = m.ndep == s->pkg->ndep && m.vnum == s->pkg->vnum && !strcmp(m.name, s->pkg->name);
            for (int d = 0; same && d < m.ndep; d++) {
                const struct zp_dep *ld = zp_pkg_dep(&ix, s->pkg, d);
                same = !strcmp(ld->name, m.dep[d]) && ld->vnum == m.depv[d];
            }
            same = same && m.ntakes == s->pkg->ntakes;
            for (int t2 = 0; same && t2 < m.ntakes; t2++) same = !strcmp(m.takes[t2], ix.takes[s->pkg->takes0 + t2]);
            if (!same) { say("%s: its manifest does not say what the list says (name, version, dependencies, takes): refused", s->pkg->file); bad++; }
            /* file conflicts across the whole set before the first change (4.3; zup checks each one again) */
            for (int e = 0; same && e < m.nent; e++) {
                if (m.ent[e].kind == 'd') continue;
                for (int q = 0; q < ninst; q++) {
                    struct zf_ent *x = zf_find_ent(inst[q], m.ent[e].path);
                    int taken = 0;
                    if (!x || x->kind == 'd' || !strcmp(inst[q]->name, m.name)) continue;
                    for (int t2 = 0; t2 < m.ntakes; t2++) taken |= !strcmp(m.takes[t2], inst[q]->name);
                    for (int o2 = 0; !taken && o2 < pl.n; o2++)     /* its owner's new version in this set drops it */
                        if (pl.step[o2].pkg && !strcmp(pl.step[o2].pkg->name, inst[q]->name) && pl.step[o2].kind != ZP_STEP_WAIT) taken = 2;
                    if (taken) continue;                    /* (taken, or its owner's new version is in this set: zup checks the order) */
                    say("%s: %s belongs to %s: refused", m.name, m.ent[e].path, inst[q]->name);
                    bad++;
                    break;
                }
            }
            zf_pkg_free(&m);
        }
    }
    if (bad) { say("nothing installed"); zp_plan_free(&pl); return 1; }
    if (down_only) { say("downloaded and checked: %d file%s in %s", todo + waits + pl.system, todo + waits + pl.system == 1 ? "" : "s", CACHE); zp_plan_free(&pl); return 0; }
    int failed = 0, done = 0;
    for (int i = 0; i < pl.n && !failed; i++) {
        struct zp_step *s = &pl.step[i];
        if (s->kind != ZP_STEP_INSTALL && s->kind != ZP_STEP_UPGRADE && s->kind != ZP_STEP_DOWNGRADE) continue;
        if (run_zup("-f", s->pkg->file, s->kind == ZP_STEP_DOWNGRADE ? "-d" : 0)) { failed = 1; break; }
        done++;
        int named = 0;
        for (int k = 0; k < n; k++) named |= !strcmp(names[k], s->pkg->name);
        if (s->kind == ZP_STEP_INSTALL) deps_set(s->pkg->name, s->asdep && !named);
        else if (named && s->from && s->from->asdep) deps_set(s->pkg->name, 0);
        if (s->kind != ZP_STEP_INSTALL) say_running(s->pkg->name);
    }
    for (int k = 0; k < n && !failed; k++) {           /* named by hand: installed explicitly from now on */
        const struct zp_inst *in = 0;
        for (int j = 0; j < ninst; j++) if (!strcmp(winst[j].name, names[k])) in = &winst[j];
        if (in && in->asdep) deps_set(names[k], 0);
    }
    if (waits) {
        say("%d package%s wait%s for the system update (downloaded): after the restart, zpkg -Syu again", waits,
            waits > 1 ? "s" : "", waits > 1 ? "" : "s");
        for (int i = 0; i < pl.n; i++) if (pl.step[i].kind == ZP_STEP_WAIT) say("  %s %s", pl.step[i].pkg->name, pl.step[i].pkg->vstr);
    }
    if (failed) say("stopped: %d of %d installed (zup said why); zpkg -Syu again goes on", done, todo);
    else if (todo) say("%d package%s installed", done, done == 1 ? "" : "s");
    int sysfail = pl.system && !failed && system_put(ix.sys.file, 0);     /* the system last: the boot partition */
    zp_plan_free(&pl);
    return failed || sysfail;
}

/* ---- --fetch DIR [--zfs] (DESIGN D35): the list's system bundle and every package a new machine gets, into DIR, checked as
 * always; zinstall installs from there. Nothing here is installed. Test programs and the bootstrap package stay out. ---- */
static int cmd_fetch(const char *dir, int zfs)
{
    int rc = sync_list();
    if (rc) return 1;
    if (use_stored()) return 1;
    if (zfs) {
        if (!ix.sysz.file) { say("the list has no system for a ZenixFS root: nothing fetched"); return 1; }
        ix.sys = ix.sysz;
    }
    if (!ix.sys.file) { say("the list has no system: nothing fetched"); return 1; }
    if (sys_chdir(dir) < 0) { say("cannot use %s", dir); return 1; }
    fetching = 1;
    int bad = 0;
    say("%s", ix.sys.file);
    if (download(0)) bad++;
    for (int i = 0; i < ix.npkg; i++) {
        const struct zp_pkg *p = &ix.pkg[i];
        if (p->base || !p->file || !strcmp(p->name, "zenix-tests") || !strcmp(p->name, "zpkg-new")) continue;
        say("%s", p->file);
        if (download(p)) bad++;
    }
    if (bad) { say("%d file(s) could not be fetched", bad); return 1; }
    return 0;
}

/* ---- -R, -Rs, -Rn, -Rns ---- */
static int cmd_remove(char **names, int n, int s, int nosave, int ram)
{
    struct zp_plan pl;
    const char *why;
    if (!n || !names_ok(names, n)) { say("which packages? zpkg -R NAME..."); return 2; }
    static struct zp_index empty;
    if (!have_ix) ix = empty;
    struct zp_world w;
    world(&w);
    memset(&pl, 0, sizeof pl);
    for (int i = 0; i < n; i++) if (!inst_find(names[i])) { say("%s is not installed", names[i]); return 1; }
    if (zp_plan_remove(&w, (const char *const *)names, n, s ? ZP_RM_S : 0, &pl, &why)) { say_plan_error(&pl, why); return 1; }
    if (!changes_allowed(ram)) { zp_plan_free(&pl); return 1; }
    int failed = 0, done = 0;
    for (int i = 0; i < pl.n && !failed; i++) {
        const struct zp_step *st = &pl.step[i];
        if (st->kind != ZP_STEP_REMOVE || !st->from) continue;
        if (run_zup("-r", st->from->name, nosave ? "-n" : 0)) failed = 1;
        else { done++; deps_set(st->from->name, 0); }
    }
    say(failed ? "stopped after %d removal%s (zup said why)" : "%d package%s removed", done, done == 1 ? "" : "s");
    zp_plan_free(&pl);
    return failed;
}

/* ---- -U FILE...: a package or system bundle from a file, its .sig verified first (5.1) ---- */
static int verify_file(const char *file, struct zf_bundle *b, char pname[25], int *sys)
{
    char sigf[72];
    u32 n;
    if (strlen(file) + 5 > sizeof sigf - 1) { say("%s: the name is too long", file); return -ENAMETOOLONG; }
    snprintf(sigf, sizeof sigf, "%s.sig", file);
    char *sig = slurp(sigf, &n, ZC_SIG_SIZE);
    if (!sig || n != ZC_SIG_SIZE) { free(sig); say("%s: no signature (%s, 80 bytes): refused", file, sigf); return -EPERM; }
    int r = read_head(file, b, pname, sys);
    if (r) { free(sig); say("%s: not a bundle zpkg takes%s", file, r == -EPERM ? " (a system bundle with packages outside the base group)" : ""); return -EINVAL; }
    r = zc_sig_verify((const u8 *)sig, ZC_SIG_SIZE, ZC_SIG_LABEL_BUNDLE, b->p + 64, zpkg_keys, ZPKG_NKEYS, 0);
    free(sig);
    if (r != ZC_SIG_OK) { say("%s: the signature does not verify: refused", file); return -EPERM; }
    if (zp_channel_class(b->channel) != ZPKG_KEYCLASS) { say("%s: its channel %s does not fit this build's key: refused", file, b->channel); return -EPERM; }
    return 0;
}
/* FILE and FILE.sig copied into the cache (root's) under the bundle's own name, so nobody can change it between the
 * check and zup (review 2026-10-28); the copy's name into out */
static int copy_in(const char *from, const char *to)
{
    static u8 buf[4096];
    int a = sys_open(from, O_RDONLY), b, k, r = 0;
    if (a < 0) return a;
    char part[64];
    snprintf(part, sizeof part, "%s.part", to);
    b = sys_open(part, O_WRONLY | O_CREAT | O_TRUNC);
    if (b < 0) { sys_close(a); return b; }
    while ((k = sys_read(a, buf, sizeof buf)) > 0) if ((r = write_all(b, buf, (u32)k))) break;
    if (k < 0) r = k;
    sys_close(a); sys_close(b);
    if (!r) r = sys_rename(part, to);
    if (r) sys_unlink(part);
    return r;
}
static int cache_copy(const char *file, char out[48])
{
    char sigf[72], so[56];
    snprintf(out, 48, "upgrade-%d.zup", sys_getpid());
    snprintf(so, sizeof so, "%s.sig", out);
    snprintf(sigf, sizeof sigf, "%s.sig", file);
    if (strlen(file) > 60) { say("%s: the name is too long", file); return -ENAMETOOLONG; }
    if (file[0] != '/' && strcmp(file, out)) {          /* a name relative to where zpkg was started */
        say("%s: give the file with its whole path (/...)", file); return -EINVAL;
    }
    struct ustat sst;                                   /* the cable's path (D21 6.5): no .sig, nothing copied */
    if (sys_stat(sigf, &sst) < 0) { say("%s: no signature (%s, 80 bytes): refused", file, sigf); return -EPERM; }
    if (sys_chdir(CACHE) < 0) { sys_mkdir("/var/cache/zpkg"); sys_mkdir(CACHE); if (sys_chdir(CACHE) < 0) return -EIO; }
    if (copy_in(file, out) || copy_in(sigf, so)) { sys_unlink(out); sys_unlink(so); say("%s: cannot copy it (and its .sig) into the cache", file); return -EIO; }
    return 0;
}
static int cmd_upgrade(char **files, int n, int down, int ram)
{
    int failed = 0;
    if (!n) { say("which files? zpkg -U FILE..."); return 2; }
    if (!changes_allowed(ram)) return 1;
    for (int i = 0; i < n && !failed; i++) {
        char copy[48], cs[56];
        if (!down && cache_copy(files[i], copy)) { failed = 1; break; }
        if (!down) { snprintf(cs, sizeof cs, "%s.sig", copy); files[i] = copy; }
        struct zf_bundle b;
        char pn[25];
        int sys;
        if (verify_file(files[i], &b, pn, &sys)) { failed = 1; break; }
        if (sys == 1) {
            struct zf_pkg *base = inst_find("base");
            if (base && b.vnum <= base->vnum) { say("%s: Zenix %u is not newer than the installed %u: refused", files[i], b.vnum, base->vnum); failed = 1; break; }
            char nm[48], ns[56], cs3[56];
            snprintf(nm, sizeof nm, "zenix-%u.zup", b.vnum);
            snprintf(ns, sizeof ns, "%s.sig", nm);
            snprintf(cs3, sizeof cs3, "%s.sig", files[i]);
            if (!down && sys_rename(files[i], nm) == 0) sys_rename(cs3, ns);
            if (system_put(down ? files[i] : nm, 1)) { failed = 1; break; }
            continue;
        }
        if (sys) { say("%s: more than one package: refused", files[i]); failed = 1; break; }
        struct zf_pkg *old = inst_find(pn);
        if (run_zup("-f", files[i], down ? "-d" : 0)) { failed = 1; break; }
        if (!old) deps_set(pn, 0);
        else say_running(pn);
        if (!down) {                                     /* the copy kept under the bundle's own name (--back) */
            char nm[48], ns[56];
            snprintf(nm, sizeof nm, "%s-%u.zup", pn, b.vnum);
            snprintf(ns, sizeof ns, "%s.sig", nm);
            if (sys_rename(copy, nm) == 0) sys_rename(cs, ns);
        }
    }
    if (!down) { char cp[48], cs2[56]; snprintf(cp, sizeof cp, "upgrade-%d.zup", sys_getpid()); snprintf(cs2, sizeof cs2, "%s.sig", cp); sys_unlink(cp); sys_unlink(cs2); }
    return failed;
}

/* ---- --back NAME: the newest cached version older than the installed one, by -U -d (asks first) ---- */
static int cmd_back(const char *name, int yes, int ram)
{
    struct zf_pkg *k = inst_find(name);
    char **names, best[48] = "";
    u32 bestv = 0;
    if (!zf_name_ok(name) || !k) { say("%s is not installed", name); return 1; }
    if (sys_chdir(CACHE) < 0) { say("no cache (%s): nothing to go back to", CACHE); return 1; }
    int n = dir_list(".", &names);
    if (n < 0) { say("the cache %s: %s: nothing done", CACHE, zwhy("cannot read")); return 1; }
    u32 nl = strlen(name);
    for (int i = 0; i < n; i++) {
        u32 l = strlen(names[i]);
        if (strncmp(names[i], name, nl) || names[i][nl] != '-' || l < nl + 6 || strcmp(names[i] + l - 4, ".zup")) continue;
        char *e;
        u32 v = (u32)strtoul(names[i] + nl + 1, &e, 10);
        if (e != names[i] + l - 4 || v >= k->vnum || v <= bestv || l >= sizeof best) continue;
        bestv = v; strcpy(best, names[i]);
    }
    dir_free(names, n);
    if (!bestv) { say("no older version of %s in the cache (%s)", name, CACHE); return 1; }
    say("%s %u is installed; go back to %u (%s)?%s", name, k->vnum, bestv, best, yes ? "" : " [y/N]");
    if (!yes) {
        char a[8] = "";
        int r = sys_read(0, a, sizeof a - 1);
        if (r <= 0 || (a[0] != 'y' && a[0] != 'Y')) { say("nothing done"); return 1; }
    }
    char *f[1] = { best };
    return cmd_upgrade(f, 1, 1, ram);
}

/* ---- -Sc: the cache down to the installed version and the one before of each package (7.5) ---- */
static int cmd_clean(int quietly)
{
    char **names;
    if (sys_chdir(CACHE) < 0) return 0;
    int n = dir_list(".", &names), gone = 0;
    if (n < 0) { say("the cache %s: %s: nothing removed", CACHE, zwhy("cannot read")); return 1; }
    for (int i = 0; i < n; i++) {
        char *f = names[i], *dash = strrchr(f, '-'), *e;
        if (!dash) continue;
        u32 v = (u32)strtoul(dash + 1, &e, 10);
        if (strncmp(e, ".zup", 4)) { if (strstr(f, ".part") || strstr(f, ".tmp")) { sys_unlink(f); gone++; } continue; }
        *dash = 0;
        int sysb = !strcmp(f, "zenix") || !strcmp(f, "zenixz");   /* the system bundles (zenixz: for a ZenixFS root, D34) */
        struct zf_pkg *k = sysb ? inst_find("base") : inst_find(f);
        int keep = 0;
        if (k && !sysb) {
            if (v == k->vnum) keep = 1;
            else if (v < k->vnum) {                     /* the newest older one stays */
                keep = 1;
                for (int j = 0; j < n; j++) {
                    char *g = names[j], *d2 = strrchr(g, '-');
                    if (j == i || !d2 || (u32)(d2 - g) != strlen(f) || strncmp(g, f, strlen(f))) continue;
                    u32 w = (u32)strtoul(d2 + 1, 0, 10);
                    if (w > v && w < k->vnum) keep = 0;
                }
            }
        } else if (sysb && k) keep = v > k->vnum;   /* a system bundle only while it waits */
        *dash = '-';
        if (!keep) { sys_unlink(f); gone++; }
    }
    dir_free(names, n);
    if (!quietly) say("the cache: %d file%s removed; the installed version and the one before of each package stay", gone, gone == 1 ? "" : "s");
    return 0;
}
static void cache_cap(void)
{
    char **names;
    u32 total = 0;
    if (sys_chdir(CACHE) < 0) return;
    int n = dir_list(".", &names);
    if (n < 0) { say("the cache %s: %s: its size not checked", CACHE, zwhy("cannot read")); return; }
    for (int i = 0; i < n; i++) { struct ustat st; if (sys_stat(names[i], &st) == 0) total += st.size; }
    dir_free(names, n);
    if (total / 1024 > cf.cache_kb) cmd_clean(1);
}

/* ---- queries ---- */
static int ci_has(const char *s, const char *w)
{
    for (; *s; s++) {
        u32 i = 0;
        while (w[i] && s[i] && tolower((unsigned char)s[i]) == tolower((unsigned char)w[i])) i++;
        if (!w[i]) return 1;
    }
    return 0;
}
static int cmd_search(char **words, int n)
{
    if (use_stored()) return 1;
    for (int i = 0; i < ix.npkg; i++) {
        const struct zp_pkg *p = &ix.pkg[i];
        int ok = 1;
        for (int j = 0; j < n && ok; j++) ok = ci_has(p->name, words[j]) || (p->desc && ci_has(p->desc, words[j]));
        if (!ok) continue;
        struct zf_pkg *k = inst_find(p->name);
        dprintf(1, "%s %s%s%s\n    %s\n", p->name, p->vstr, k ? " [installed" : "", k ? (k->vnum == p->vnum ? "]" : ", another version]") : "",
               p->desc ? p->desc : "");
    }
    return 0;
}
static int cmd_info_list(char **names, int n)
{
    if (use_stored()) return 1;
    int rc = 0;
    for (int i = 0; i < n; i++) {
        const struct zp_pkg *p = zp_index_find(&ix, names[i]);
        if (!p) { say("%s is not in the package list", names[i]); rc = 1; continue; }
        dprintf(1, "Name        : %s\nVersion     : %s (%u)\nDescription : %s\n", p->name, p->vstr, p->vnum, p->desc ? p->desc : "");
        dprintf(1, "Group       : %s\nDepends on  :", p->base ? "base (comes with the system update)" : "none");
        for (int d = 0; d < p->ndep; d++) dprintf(1, " %s>=%u", zp_pkg_dep(&ix, p, d)->name, zp_pkg_dep(&ix, p, d)->vnum);
        dprintf(1, "%s\n", p->ndep ? "" : " nothing");
        if (p->ntakes) { dprintf(1, "Takes       :"); for (int t = 0; t < p->ntakes; t++) dprintf(1, " %s", ix.takes[p->takes0 + t]); dprintf(1, "\n"); }
        if (p->file) dprintf(1, "File        : %s, %u bytes (%u installed)\n", p->file, p->fsize, p->size);
        struct zf_pkg *k = inst_find(p->name);
        dprintf(1, "Installed   : %s\n\n", k ? k->vstr : "no");
    }
    return rc;
}
static int cmd_query(char **names, int n, char what)
{
    int rc = 0;
    if (what == 'o') {
        for (int i = 0; i < n; i++) {
            int found = 0;
            for (int j = 0; j < ninst; j++)
                if (zf_find_ent(inst[j], names[i]) && zf_find_ent(inst[j], names[i])->kind != 'd') {
                    dprintf(1, "%s is owned by %s %s\n", names[i], inst[j]->name, inst[j]->vstr);
                    found = 1;
                }
            if (!found) { say("no package owns %s", names[i]); rc = 1; }
        }
        return rc;
    }
    if (what == 'u') {
        if (use_stored()) return 1;
        for (int j = 0; j < ninst; j++) {
            const struct zp_pkg *p = zp_index_find(&ix, inst[j]->name);
            if (p && p->vnum > inst[j]->vnum) dprintf(1, "%s %s -> %s\n", inst[j]->name, inst[j]->vstr, p->vstr);
        }
        if (ix.sys.vnum > effbase && inst_find("base") && ix.sys.vnum > inst_find("base")->vnum)
            dprintf(1, "Zenix (the system) %u -> %s (%u)\n", inst_find("base")->vnum, ix.sys.vstr, ix.sys.vnum);
        return 0;
    }
    for (int j = 0; j < ninst; j++) {
        struct zf_pkg *k = inst[j];
        int pick = !n;
        for (int i = 0; i < n; i++) pick |= !strcmp(names[i], k->name);
        if (!pick) continue;
        u32 needbase = 0;
        for (int d = 0; d < k->ndep; d++) if (!strcmp(k->dep[d], "base")) needbase = k->depv[d];
        const char *mark = needbase > effbase ? "  (needs base newer than the one running)" : "";
        if (what == 'l') {
            for (int e = 0; e < k->nent; e++) dprintf(1, "%s %s\n", k->name, k->ent[e].path);
        } else if (what == 'i') {
            int nf = 0;
            for (int e = 0; e < k->nent; e++) nf += k->ent[e].kind != 'd';
            dprintf(1, "Name        : %s\nVersion     : %s (%u)%s\nGroup       : %s\nDepends on  :", k->name, k->vstr, k->vnum, mark,
                   k->group ? k->group : "none");
            for (int d = 0; d < k->ndep; d++) dprintf(1, " %s>=%u", k->dep[d], k->depv[d]);
            dprintf(1, "%s\nFiles       : %d\nInstalled as: %s\n\n", k->ndep ? "" : " nothing", nf,
                   winst[j].asdep ? "a dependency" : "asked for");
        } else dprintf(1, "%s %s%s\n", k->name, k->vstr, mark);
    }
    for (int i = 0; i < n; i++) if (!inst_find(names[i])) { say("%s is not installed", names[i]); rc = 1; }
    return rc;
}

static int usage(void)
{
    dprintf(1, "usage: zpkg -Sy | -Syu | -Syuu | -S NAME... | -Sw NAME... | -Ss WORD... | -Si NAME... | -Sc\n"
           "       zpkg -Q [NAME...] | -Qi NAME... | -Ql NAME... | -Qo PATH... | -Qu\n"
           "       zpkg -R[s][n] NAME... | -U FILE... | --back NAME\n"
           "       zpkg --fetch DIR [--zfs]\n"
           "  options: --snapshot NAME  --ignore-expiry  --reset-serial CHANNEL  --ram  --yes   (man zpkg)\n");
    return 2;
}

int main(int argc, char **argv)
{
    char *args[64];
    int na = 0, ignore = 0, ram = 0, yes = 0;
    const char *op = 0, *back = 0, *reset = 0, *fetchdir = 0;
    int fetchz = 0;
    signal(SIGPIPE, SIG_IGN);
    cn.fd = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--snapshot") && i + 1 < argc) snapshot = argv[++i];
        else if (!strcmp(argv[i], "--back") && i + 1 < argc) back = argv[++i];
        else if (!strcmp(argv[i], "--reset-serial") && i + 1 < argc) reset = argv[++i];
        else if (!strcmp(argv[i], "--fetch") && i + 1 < argc) fetchdir = argv[++i];
        else if (!strcmp(argv[i], "--zfs")) fetchz = 1;
        else if (!strcmp(argv[i], "--ignore-expiry")) ignore = 1;
        else if (!strcmp(argv[i], "--ram")) ram = 1;
        else if (!strcmp(argv[i], "--yes")) yes = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (argv[i][0] == '-' && argv[i][1] && argv[i][1] != '-' && !op) op = argv[i] + 1;
        else if (argv[i][0] == '-') return usage();
        else if (na < 64) args[na++] = argv[i];
        else { say("more than 64 names"); return 2; }
    }
    if (!op && !back && !reset && !fetchdir) return usage();
    char kind = op ? op[0] : 0;
    int y = 0, u = 0, w = 0, s = 0, inf = 0, c = 0, l = 0, o = 0, nn = 0, qu = 0;
    for (const char *p = op ? op + 1 : ""; *p; p++)
        switch (*p) {
        case 'y': y++; break;
        case 'u': u++; break;
        case 'w': w = 1; break;
        case 's': s = 1; break;
        case 'i': inf = 1; break;
        case 'c': c = 1; break;
        case 'l': l = 1; break;
        case 'o': o = 1; break;
        case 'n': nn = 1; break;
        default: return usage();
        }
    if (kind && !strchr("SQRU", kind)) return usage();
    if (snapshot && !zp_snapshot_name_ok(snapshot)) { say("%s: not a snapshot name", snapshot); return 2; }
    (void)qu;
    read_conf();
    if (snapshot) snprintf(syncname, sizeof syncname, "snapshot-%s.index", snapshot);
    else snprintf(syncname, sizeof syncname, "%s.index", cf.channel);
    set_trust();
    if (ignore) tr.flags |= ZP_T_IGNORE_EXPIRY;
    struct ustat st;
    if (sys_stat(DB, &st) < 0) { say("no package database here (%s): zpkg changes nothing", DB); return 1; }
    int changing = back || reset || fetchdir || kind == 'R' || kind == 'U' || (kind == 'S' && !s && !inf);
    if (changing && sys_getuid() != 0) { say("only root changes packages (try sudo)"); return 1; }
    if (changing) {
        u32 m[4];
        if (sys_meminfo(m) == 0 && (m[0] + m[1]) / 1024 < NEEDKB) {
            say("only %u KB of memory are free: zpkg and zup need about %u KB (close a program and try again)", (m[0] + m[1]) / 1024, NEEDKB);
            return 1;
        }
        if (lock_take()) return 1;
    }
    int rc = 0;
    if ((rc = load_db())) { lock_drop(); return 1; }
    set_effbase();
    load_stored();
    if (reset) {
        char p[64];
        snprintf(p, sizeof p, SYNC "/%s.index", reset);
        if (!zf_name_ok(reset) && strncmp(reset, "snapshot-", 9)) { say("%s: not a channel", reset); rc = 2; }
        else { sys_unlink(p); say("THE STORED LIST OF %s IS GONE: the next zpkg -Sy takes any list signed for it, also an older one", reset); }
    } else if (fetchdir) rc = cmd_fetch(fetchdir, fetchz);
    else if (back) rc = cmd_back(back, yes, ram);
    else if (kind == 'S' && s) rc = cmd_search(args, na);
    else if (kind == 'S' && inf) rc = cmd_info_list(args, na);
    else if (kind == 'S' && c) rc = cmd_clean(0);
    else if (kind == 'S') { rc = cmd_sync(args, na, y, u, w, ram); cache_cap(); }
    else if (kind == 'R') rc = cmd_remove(args, na, s, nn, ram);
    else if (kind == 'U') rc = cmd_upgrade(args, na, 0, ram);
    else if (kind == 'Q') rc = cmd_query(args, na, inf ? 'i' : l ? 'l' : o ? 'o' : u ? 'u' : 0);
    conn_close();
    lock_drop();
    return rc;
}
