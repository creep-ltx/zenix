/* SPDX-License-Identifier: GPL-2.0-only */
/* wget [-q] [-c] [-O FILE] URL...: fetch files over HTTP and HTTPS (DESIGN D21 8, step 7; docs/man/wget.txt). The
 * command line, the names, the redirects and the resume rule are wget's core (user/wget/), the protocol net/http/,
 * TLS the system's one TLS path, the helper /lib/tls/tlspipe (DESIGN D58 section 5; irc's and mail's too: TLS 1.2,
 * the server's chain checked against /etc/ssl/anchors and the owner's roots in /etc/ssl/local/, the dates by
 * zc_clock's rules; the package tls); this file gives them the sockets (with time limits), the files and the messages.
 * One connection is kept open while the next URL goes to the same server (keep-alive). Exit statuses: GNU wget's
 * (wget.h). */
#include "socket.h"
#include "wget/wget.h"
#include "../net/http/http.h"

#define IDLE_MS    60000                /* nor may the server be silent longer (the handshake's arithmetic is ours) */

static int quiet;

static void say(const char *fmt, ...)
{
    va_list ap;
    if (quiet)
        return;
    va_start(ap, fmt);
    dprintf(2, "wget: ");
    vdprintf(2, fmt, ap);
    va_end(ap);
}

/* ---- the connection ---- */
#define TLSPIPE "/lib/tls/tlspipe"
static int said_clock;

struct conn {
    int fd, tls, eof;                   /* fd: the socket, or (TLS) the helper's output; -1 none */
    int wfd, pid;                       /* TLS: the helper's input and its process */
    struct http_url at;                 /* the scheme, host and port it is open to */
    unsigned char buf[4096];
    size_t have, off;
};
static struct conn C;

/* wait for fd to be ready for ev, at most ms: 1 ready, 0 the time is up, < 0 an error */
static int wait_fd(int fd, int ev, int ms)
{
    struct pollfd q;
    q.fd = fd;
    q.events = (short)ev;
    q.revents = 0;
    int r = poll(&q, 1, ms);
    return r < 0 ? -zerrno : r;
}

static long sock_send(void *u, const unsigned char *p, size_t n)
{
    struct conn *c = u;
    for (;;) {
        int k = send(c->fd, p, (u32)n, MSG_NOSIGNAL);
        if (k > 0)
            return k;
        if (k < 0 && zerrno == EAGAIN) {
            if (wait_fd(c->fd, POLLOUT, IDLE_MS) != 1)
                return -ETIMEDOUT;
            continue;
        }
        return k < 0 ? -zerrno : -EPIPE;
    }
}

static long sock_recv(void *u, unsigned char *p, size_t n)
{
    struct conn *c = u;
    for (;;) {
        int k = recv(c->fd, p, (u32)n, 0);
        if (k >= 0)
            return k;
        if (zerrno != EAGAIN)
            return -zerrno;
        if (wait_fd(c->fd, POLLIN, IDLE_MS) != 1)
            return -ETIMEDOUT;
    }
}

/* the helper's status once it ended (it is given 2 s after its input closed, then ended): 0 close_notify seen, 1 the
 * connection ended without it (or a network error), 3 a TLS error */
static int tls_end(struct conn *c)
{
    int st = -2;
    if (c->pid <= 0)
        return 1;
    for (u32 t0 = sys_ticks(); (st = sys_waitnb(c->pid)) == -2 && sys_ticks() - t0 < 200;)
        sys_sleep(5);
    if (st == -2) {
        sys_kill(c->pid);
        st = sys_wait(c->pid);
    }
    c->pid = -1;
    return st;
}

static void conn_close(struct conn *c)
{
    if (c->fd < 0)
        return;
    if (c->tls) {
        if (c->wfd >= 0)
            sys_close(c->wfd);          /* the helper sends close_notify and ends */
        c->wfd = -1;
        sys_close(c->fd);
        c->fd = -1;
        tls_end(c);
    } else {
        sys_close(c->fd);
        c->fd = -1;
    }
    c->have = c->off = 0;
}

static const char *errtext(int e)
{
    const char *t = zstrerror(e);
    return t ? t : "failed";
}

/* a TCP connection to u's host and port (zconnect: the system's one time limit): the socket, or -1 and *why */
static int tcp_open(const struct http_url *u, const char **why)
{
    static char msg[80];
    const char *w;
    int s;
    say("connecting to %s port %u\n", u->host, u->port);
    if ((s = zconnect(u->host, (int)u->port, ZC_NONBLOCK, &w)) < 0) {
        snprintf(msg, sizeof msg, "%s port %u: %s", u->host, u->port, w);
        *why = msg;
        return -1;
    }
    return s;
}

/* the connection for u (the kept one when it goes to the same server): 0, or an exit status with *why */
static int conn_open(struct conn *c, const struct http_url *u, const char **why)
{
    static char msg[160];
    int r;
    if (c->fd >= 0 && wget_same_server(&c->at, u))
        return 0;
    conn_close(c);
    if ((c->fd = tcp_open(u, why)) < 0)
        return WGET_EXIT_NET;
    c->at = *u;
    c->tls = u->tls;
    c->have = c->off = 0;
    c->eof = 0;
    c->wfd = c->pid = -1;
    if (!u->tls)
        return 0;
    /* TLS: the helper with the socket as its descriptor 2, the plain text through two pipes (as irc and mail) */
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if (sys_pipe(in) < 0 || sys_pipe(out) < 0) {
        snprintf(msg, sizeof msg, "no pipes for TLS: %s", zwhy("failed"));
        *why = msg;
        if (in[0] >= 0) { sys_close(in[0]); sys_close(in[1]); }
        sys_close(c->fd);
        c->fd = -1;
        return WGET_EXIT_GENERIC;
    }
    char ps[8], *argv[5];
    snprintf(ps, sizeof ps, "%u", u->port);
    argv[0] = "tlspipe"; argv[1] = (char *)u->host; argv[2] = ps; argv[3] = "http/1.1"; argv[4] = 0;
    int io[3] = { in[0], out[1], c->fd };
    say("TLS handshake with %s (on a 68020 this takes up to a minute)\n", u->host);
    c->pid = sys_spawn(TLSPIPE, argv, io);
    sys_close(in[0]);
    sys_close(out[1]);
    sys_close(c->fd);                   /* the helper has it now */
    c->fd = out[0];
    c->wfd = in[1];
    if (c->pid < 0) {
        snprintf(msg, sizeof msg, "cannot start %s: %s (the package tls)", TLSPIPE, errtext(c->pid));
        *why = msg;
        c->pid = -1;
        conn_close(c);
        return WGET_EXIT_GENERIC;
    }
    /* its first lines: "info TEXT" while it waits, then "ok", "ok clock YYYY-MM-DD" or "error KIND: TEXT" */
    for (;;) {
        char l[160];
        int n = 0;
        while (n < (int)sizeof l - 1) {
            if (wait_fd(c->fd, POLLIN, IDLE_MS + 180000) != 1 || sys_read(c->fd, l + n, 1) != 1) {
                n = -1;
                break;
            }
            if (l[n] == '\n')
                break;
            n++;
        }
        if (n == (int)sizeof l - 1) {          /* longer than the buffer: the rest of the line skipped */
            char ch;
            while (wait_fd(c->fd, POLLIN, IDLE_MS) == 1 && sys_read(c->fd, &ch, 1) == 1 && ch != '\n')
                ;
        }
        if (n < 0) {
            r = tls_end(c);
            *why = "TLS: the helper ended without an answer";
            conn_close(c);
            return WGET_EXIT_NET;
        }
        l[n] = 0;
        if (!strncmp(l, "info ", 5)) {
            say("%s\n", l + 5);
            continue;
        }
        if (!strncmp(l, "ok", 2)) {
            if (!strncmp(l, "ok clock ", 9) && !said_clock) {
                say("the clock is not set: certificate dates checked only against %s\n", l + 9);
                said_clock = 1;
            }
            return 0;
        }
        /* "error certificate: ..." (exit 5), "error network: ..." (4), any other a TLS failure (7) */
        const char *t = strncmp(l, "error ", 6) ? l : l + 6;
        r = !strncmp(t, "certificate:", 12) ? WGET_EXIT_TLS : !strncmp(t, "network:", 8) ? WGET_EXIT_NET
            : WGET_EXIT_PROTO;
        snprintf(msg, sizeof msg, "TLS: %s", t);
        *why = msg;
        conn_close(c);
        return r;
    }
}

/* more bytes into c->buf: > 0, 0 the end (the server closed; *trunc: TLS ended without close_notify), < 0 an error */
static long conn_fill(struct conn *c, int *trunc)
{
    long k;
    *trunc = 0;
    if (c->tls) {
        if (wait_fd(c->fd, POLLIN, IDLE_MS) != 1)
            return -ETIMEDOUT;
        k = sys_read(c->fd, c->buf, sizeof c->buf);
        if (k == 0) {                   /* the helper ended: was there a close_notify? */
            int st = tls_end(c);
            if (st != 0)
                *trunc = 1;
            if (st == 3)
                return -EIO;
        }
    } else
        k = sock_recv(c, c->buf, sizeof c->buf);
    if (k > 0) {
        c->off = 0;
        c->have = (size_t)k;
    }
    return k;
}

static int conn_send(struct conn *c, const char *s, size_t n)
{
    if (c->tls) {
        while (n) {
            int w = sys_write(c->wfd, s, (u32)n);
            if (w <= 0)
                return w < 0 ? w : -EPIPE;
            s += w;
            n -= (size_t)w;
        }
        return 0;
    }
    while (n) {
        long k = sock_send(c, (const unsigned char *)s, n);
        if (k < 0)
            return (int)k;
        s += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ---- one URL ---- */
static int writeall(int fd, const unsigned char *b, size_t n)
{
    while (n) {
        int w = sys_write(fd, b, (u32)n);
        if (w <= 0)
            return w < 0 ? w : -EIO;
        b += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fail(int i, const struct wget_opts *o, int ex, const char *why)
{
    say("%s: %s\n", o->url[i], why);
    return ex;
}

static int fetch(const struct wget_opts *o, int i)
{
    static struct http_resp r;
    static char req[HTTP_PATHMAX + HTTP_HOSTMAX + 128];
    static char base[WGET_NAMEMAX + 1];
    struct wget_plan p;
    const char *why = "";
    int rc, act, ex, tries, need, out = -1, own = 0;
    unsigned long wrote = 0;
    u32 t0 = sys_uptime();
    if ((rc = wget_plan_start(&p, o, i, &why)))
        return fail(i, o, rc, why);
    if (p.need_size) {
        struct ustat st;
        int ok = sys_stat(p.name, &st) == 0;
        wget_plan_have(&p, ok, ok ? st.size : 0);
    }
    if (p.need_free_name) {
        struct ustat st;
        unsigned n = 1;
        strcpy(base, p.name);
        while (sys_stat(p.name, &st) == 0)
            if (wget_nextname(p.name, sizeof p.name, base, n++))
                return fail(i, o, WGET_EXIT_FILE, "no free file name");
    }
    for (;;) {
        for (tries = 0; tries < 2; tries++) {   /* a kept connection the server closed meanwhile: once again */
            int reused = C.fd >= 0 && wget_same_server(&C.at, &p.url), tr;
            if ((rc = conn_open(&C, &p.url, &why)))
                return fail(i, o, rc, why);
            rc = http_request(req, sizeof req, &p.url, p.from, 0);
            if (rc < 0)
                return fail(i, o, WGET_EXIT_GENERIC, "the request does not fit");
            if (conn_send(&C, req, (size_t)rc) < 0) {
                conn_close(&C);
                if (reused)
                    continue;
                return fail(i, o, WGET_EXIT_NET, "the request could not be sent");
            }
            http_resp_init(&r, 0xFFFFFFFFul, 0);
            if (C.off == C.have) {
                long k = conn_fill(&C, &tr);
                if (k <= 0) {
                    conn_close(&C);
                    if (reused && k == 0)
                        continue;
                    return fail(i, o, WGET_EXIT_NET, k == -ETIMEDOUT ? "the server stopped answering" : "no answer");
                }
            }
            break;
        }
        if (tries == 2)
            return fail(i, o, WGET_EXIT_NET, "the connection keeps closing");
        act = 0;
        need = 0;
        for (;;) {                      /* the headers, then the body */
            const unsigned char *body;
            size_t used, blen;
            int tr;
            if (need) {                 /* the parser took everything and wants more */
                long k = conn_fill(&C, &tr);
                need = 0;
                if (k < 0) {
                    conn_close(&C);
                    if (own)
                        sys_close(out);
                    return fail(i, o, WGET_EXIT_NET, k == -ETIMEDOUT ? "the server stopped answering"
                                : C.tls ? "the TLS connection failed" : "read error");
                }
                if (k == 0) {
                    rc = tr ? HTTP_EIO : http_resp_eof(&r);
                    conn_close(&C);
                    if (rc == HTTP_END)
                        break;
                    if (own)
                        sys_close(out);
                    say("%s: %s after %lu bytes\n", o->url[i], tr ? "truncated (no TLS close_notify)"
                        : "the connection ended early", wrote);
                    return WGET_EXIT_NET;
                }
            }
            rc = http_resp_feed(&r, C.buf + C.off, C.have - C.off, &used, &body, &blen);
            C.off += used;
            if (rc == HTTP_NEED) {
                need = 1;
                continue;
            }
            if (rc < 0) {
                conn_close(&C);
                if (own)
                    sys_close(out);
                return fail(i, o, WGET_EXIT_PROTO, rc == HTTP_E2BIG ? "the answer is too long" :
                            rc == HTTP_ENOTSUP ? "a transfer coding wget does not take" : "not an HTTP answer it takes");
            }
            if (rc == HTTP_HEAD) {
                act = wget_plan_answer(&p, &r, &ex, &why);
                if (act == WGET_FAIL) {
                    conn_close(&C);
                    say("%s: %d: %s\n", o->url[i], r.status, why);
                    return ex;
                }
                if (act == WGET_FOLLOW)
                    say("%d: following to %s://%s:%u%s\n", r.status, p.url.tls ? "https" : "http", p.url.host,
                        p.url.port, p.url.path);
                else if (act == WGET_DONE)
                    say("%s: the file is already complete\n", p.name);
                else if (act == WGET_WRITE) {
                    if (p.to_stdout)
                        out = 1;
                    else if ((out = sys_open(p.name, O_WRONLY | O_CREAT | (p.append ? O_APPEND : O_TRUNC))) >= 0)
                        own = 1;
                    else {
                        int e = out;
                        conn_close(&C);
                        say("%s: cannot write: %s\n", p.name, errtext(e));
                        return WGET_EXIT_FILE;
                    }
                    if (p.from)
                        say("%d: resuming '%s' at %lu of %lu bytes\n", r.status, p.name, p.from, p.total);
                    else if (!p.to_stdout)
                        say("%d: %s%lu bytes, saving to '%s'\n", r.status, p.total ? "" : "length unknown, ",
                            p.total, p.name);
                }
                continue;
            }
            if (rc == HTTP_DATA) {
                if (act == WGET_WRITE) {
                    int e = writeall(out, body, blen);
                    if (e < 0) {
                        conn_close(&C);
                        if (own)
                            sys_close(out);
                        say("%s: write error: %s\n", p.to_stdout ? "standard output" : p.name, errtext(e));
                        return WGET_EXIT_FILE;
                    }
                    wrote += blen;
                }
                continue;
            }
            if (r.close)                /* HTTP_END */
                conn_close(&C);
            break;
        }
        if (act == WGET_FOLLOW)
            continue;
        if (own)
            sys_close(out);
        if (act == WGET_WRITE && !p.to_stdout)
            say("'%s' saved: %lu bytes in %u s\n", p.name, wrote, sys_uptime() - t0);
        return 0;
    }
}

static const char USAGE[] = "usage: wget [-q] [-c] [-O FILE] URL...\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    struct wget_opts o;
    const char *why = "";
    int i, rc, ex = 0;
    if ((rc = wget_args(&o, argc, argv, &why))) {
        if (o.bad)
            zbadopt(o.bad, USAGE);
        dprintf(2, "wget: %s\n%s", why, USAGE);
        return rc;
    }
    quiet = o.quiet;
    C.fd = C.wfd = C.pid = -1;
    signal(SIGPIPE, SIG_IGN);
    int pr = getpriority(PRIO_PROCESS, 0);      /* one level lower, as ssh: the handshake's arithmetic takes up to a
                                                 * minute on a 68020, netd keeps up, other consoles stay snappy */
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr + 1);
    for (i = 0; i < o.nurl; i++)
        ex = wget_exit_merge(ex, fetch(&o, i));
    conn_close(&C);
    return ex;
}
