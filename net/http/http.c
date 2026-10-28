/* SPDX-License-Identifier: GPL-2.0-only */
/* The HTTP/1.1 client's core (DESIGN D22 7.4): see http.h. No system calls, no allocation. */
#include <string.h>
#include "http.h"

enum { S_STATUS, S_HEADERS, S_LEN, S_CLOSE, S_CSIZE, S_CDATA, S_CCRLF, S_TRAILER, S_DONE, S_ERR };

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (lc((unsigned char)*a) != lc((unsigned char)*b))
            return 0;
    return *a == *b;
}

/* a decimal number of at most 4294967295; -1 when it is not one */
static int dec(const char *s, const char *e, unsigned long *v)
{
    unsigned long x = 0;
    if (s == e)
        return -1;
    for (; s < e; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        if (x > (0xFFFFFFFFul - (unsigned long)(*s - '0')) / 10)
            return -1;
        x = x * 10 + (unsigned long)(*s - '0');
    }
    *v = x;
    return 0;
}

static int host_ok(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || n > HTTP_HOSTMAX)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '.' || s[i] == '-'))
            return 0;
    return 1;
}

static int path_ok(const char *s)
{
    size_t n = 0;
    if (*s != '/')
        return 0;
    for (; *s; s++, n++)
        if ((unsigned char)*s <= ' ' || (unsigned char)*s >= 127)
            return 0;
    return n <= HTTP_PATHMAX;
}

/* the path into d without a fragment ("#..." is never sent, RFC 3986 3.5); HTTP_EINVAL when it is not one */
static int set_path(char *d, const char *s)
{
    size_t n = 0;
    if (!path_ok(s))
        return HTTP_EINVAL;
    while (s[n] && s[n] != '#')
        n++;
    memcpy(d, s, n);
    d[n] = 0;
    return 0;
}

/* the part after "scheme://": HOST[:PORT][/PATH] into u, whose tls and default port are set */
static int parse_authority(struct http_url *u, const char *h)
{
    const char *e, *c;
    unsigned long port;
    for (e = h; *e && *e != '/'; e++)
        ;
    for (c = h; c < e && *c != ':'; c++)
        ;
    if (!host_ok(h, (size_t)(c - h)))
        return HTTP_EINVAL;
    if (c < e) {
        if (dec(c + 1, e, &port) || port == 0 || port > 65535)
            return HTTP_EINVAL;
        u->port = (unsigned)port;
    }
    memcpy(u->host, h, (size_t)(c - h));
    u->host[c - h] = 0;
    if (!*e)
        e = "/";
    return set_path(u->path, e);
}

int http_url_parse(struct http_url *u, const char *s)
{
    memset(u, 0, sizeof *u);
    if (!strncmp(s, "http://", 7)) {
        u->port = 80;
        return parse_authority(u, s + 7);
    }
    if (!strncmp(s, "https://", 8)) {
        u->tls = 1;
        u->port = 443;
        return parse_authority(u, s + 8);
    }
    return HTTP_EINVAL;
}

int http_is_redirect(int st)
{
    return st == 301 || st == 302 || st == 303 || st == 307 || st == 308;
}

int http_redirect(struct http_url *u, const char *loc, int *hops)
{
    struct http_url n;
    if (++*hops > HTTP_MAXREDIR)
        return HTTP_ELOOP;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) {
        if (http_url_parse(&n, loc))
            return HTTP_EINVAL;
        if (u->tls && !n.tls)
            return HTTP_EPERM;          /* never from https to http */
        *u = n;
        return 0;
    }
    if (loc[0] == '/' && loc[1] != '/') {
        return set_path(u->path, loc);
    }
    return HTTP_EPERM;                  /* relative or scheme-relative: not followed */
}

/* RFC 3986 5.2.4: the "." and ".." segments of the absolute path p[0..n) taken out, in place; the new length */
static size_t dot_segments(char *p, size_t n)
{
    char *in = p, *end = p + n, *out = p;
    while (in < end) {                  /* in is at a '/' */
        char *s = in + 1, *e = s;
        while (e < end && *e != '/')
            e++;
        if ((e - s == 1 && s[0] == '.') || (e - s == 2 && s[0] == '.' && s[1] == '.')) {
            if (e - s == 2)
                while (out > p && *--out != '/')
                    ;                   /* the last segment written goes */
            if (e == end)
                *out++ = '/';           /* "/a/.." is "/", "/a/." is "/a/" */
        } else {
            memmove(out, in, (size_t)(e - in));
            out += e - in;
        }
        in = e;
    }
    return (size_t)(out - p);
}

int http_redirect_rel(struct http_url *u, const char *loc, int *hops)
{
    struct http_url n;
    const char *c;
    size_t ln, base, k, q;
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8))
        return http_redirect(u, loc, hops);
    if (++*hops > HTTP_MAXREDIR)
        return HTTP_ELOOP;
    for (c = loc; (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '+' ||
                  *c == '-' || *c == '.'; c++)
        ;
    if (*c == ':' && c > loc && !(loc[0] >= '0' && loc[0] <= '9'))
        return HTTP_EINVAL;             /* another scheme (ftp:, mailto:...): nothing this client fetches */
    n = *u;
    if (loc[0] == '/' && loc[1] == '/') {   /* "//HOST[:PORT]/PATH": the same scheme, so never https to http */
        n.port = n.tls ? 443 : 80;
        if (parse_authority(&n, loc + 2))
            return HTTP_EINVAL;
        for (q = 0; n.path[q] && n.path[q] != '?'; q++)
            ;
        k = dot_segments(n.path, q);
        memmove(n.path + k, n.path + q, strlen(n.path + q) + 1);
        *u = n;
        return 0;
    }
    for (ln = 0; loc[ln] && loc[ln] != '#'; ln++)
        ;                               /* the fragment is never sent */
    for (base = 0; u->path[base] && u->path[base] != '?'; base++)
        ;
    if (ln == 0)
        return 0;                       /* the same resource again (RFC 3986 5.2.2: an empty reference) */
    if (loc[0] == '/')
        base = 0;                       /* an absolute path */
    else if (loc[0] != '?')
        while (base > 0 && u->path[base - 1] != '/')
            base--;                     /* merged with the base's directory (5.2.3); a query keeps the whole path */
    if (base + ln > HTTP_PATHMAX)
        return HTTP_EINVAL;
    memcpy(n.path, u->path, base);
    memcpy(n.path + base, loc, ln);
    n.path[base + ln] = 0;
    for (q = 0; n.path[q] && n.path[q] != '?'; q++)
        ;
    k = dot_segments(n.path, q);
    memmove(n.path + k, n.path + q, base + ln - q + 1);
    if (!path_ok(n.path))
        return HTTP_EINVAL;
    memcpy(u->path, n.path, strlen(n.path) + 1);
    return 0;
}

struct out {
    char *p;
    size_t n, cap;
};

static void put(struct out *o, const char *s)
{
    size_t k = strlen(s);
    if (o->n + k < o->cap)
        memcpy(o->p + o->n, s, k);
    o->n += k;
}

static void putnum(struct out *o, unsigned long v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do {
        b[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(o, b + i);
}

int http_request(char *buf, size_t cap, const struct http_url *u, unsigned long from, int close)
{
    struct out o;
    o.p = buf;
    o.n = 0;
    o.cap = cap;
    if (!host_ok(u->host, strlen(u->host)) || !path_ok(u->path))
        return HTTP_EINVAL;
    put(&o, "GET ");
    put(&o, u->path);
    put(&o, " HTTP/1.1\r\nHost: ");
    put(&o, u->host);
    if (u->port != (u->tls ? 443u : 80u)) {
        put(&o, ":");
        putnum(&o, u->port);
    }
    put(&o, "\r\n");
    if (from) {
        put(&o, "Range: bytes=");
        putnum(&o, from);
        put(&o, "-\r\n");
    }
    if (close)
        put(&o, "Connection: close\r\n");
    put(&o, "\r\n");
    if (o.n >= cap || o.n > 0x7FFFFFFF)
        return HTTP_EINVAL;
    buf[o.n] = 0;
    return (int)o.n;
}

void http_resp_init(struct http_resp *r, unsigned long limit, int head)
{
    memset(r, 0, sizeof *r);
    r->st = S_STATUS;
    r->limit = limit;
    r->head = head;
}

static int fail(struct http_resp *r, int e)
{
    r->st = S_ERR;
    return e;
}

/* "HTTP/1.x SSS reason" */
static int status_line(struct http_resp *r, char *l)
{
    unsigned long st;
    if (strncmp(l, "HTTP/1.", 7) || (l[7] != '0' && l[7] != '1') || l[8] != ' ')
        return HTTP_EPROTO;
    if (dec(l + 9, l + 12, &st) || st < 100 || st > 599 || (l[12] && l[12] != ' '))
        return HTTP_EPROTO;
    r->minor = l[7] - '0';
    r->status = (int)st;
    r->close = r->minor == 0;           /* HTTP/1.0: closed unless it says keep-alive */
    return 0;
}

static const char *trim(char *s, char **end)
{
    char *e = s + strlen(s);
    while (*s == ' ' || *s == '\t')
        s++;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *e = 0;
    *end = e;
    return s;
}

/* does a comma-separated header value hold the token? */
static int has_token(const char *v, const char *tok)
{
    char t[32];
    while (*v) {
        size_t n = 0;
        while (*v == ' ' || *v == '\t' || *v == ',')
            v++;
        while (*v && *v != ',' && *v != ' ' && *v != '\t') {
            if (n < sizeof t - 1)
                t[n++] = *v;
            v++;
        }
        t[n] = 0;
        if (n && ieq(t, tok))
            return 1;
    }
    return 0;
}

static int header_line(struct http_resp *r, char *l)
{
    char *c, *e;
    const char *v;
    if (*l == ' ' || *l == '\t')
        return HTTP_EPROTO;             /* obsolete line folding */
    for (c = l; *c && *c != ':'; c++)
        if (*c == ' ' || *c == '\t')
            return HTTP_EPROTO;         /* a space in a field name */
    if (!*c || c == l)
        return HTTP_EPROTO;
    *c = 0;
    v = trim(c + 1, &e);
    if (ieq(l, "content-length")) {
        unsigned long n;
        if (dec(v, e, &n))
            return HTTP_EPROTO;
        if (r->has_len && n != r->len)
            return HTTP_EPROTO;         /* two different lengths */
        r->has_len = 1;
        r->len = n;
    } else if (ieq(l, "transfer-encoding")) {
        if (!ieq(v, "chunked") || r->chunked)
            return HTTP_ENOTSUP;
        r->chunked = 1;
    } else if (ieq(l, "connection")) {
        if (has_token(v, "close"))
            r->close = 1;
        else if (r->minor == 0 && has_token(v, "keep-alive"))
            r->close = 0;
    } else if (ieq(l, "content-range")) {
        const char *d, *s;
        unsigned long a, b, t;
        if (r->has_range || strncmp(v, "bytes ", 6))
            return HTTP_EPROTO;
        v += 6;
        if (v[0] == '*' && v[1] == '/')     /* a 416's form (an asterisk, then /TOTAL): no range */
            return dec(v + 2, e, &a) ? HTTP_EPROTO : 0;
        for (d = v; *d && *d != '-'; d++)
            ;
        for (s = d; *s && *s != '/'; s++)
            ;
        if (!*d || !*s || dec(v, d, &a) || dec(d + 1, s, &b) || dec(s + 1, e, &t) || a > b || b >= t)
            return HTTP_EPROTO;         /* also an unknown total ("*"): this client needs it */
        r->has_range = 1;
        r->range_first = a;
        r->range_last = b;
        r->range_total = t;
    } else if (ieq(l, "location")) {
        size_t n = (size_t)(e - v);
        if (n >= sizeof r->location || r->location[0])
            return HTTP_EPROTO;
        memcpy(r->location, v, n);
        r->location[n] = 0;
    }
    return 0;
}

/* the headers are complete: the body's form */
static int headers_done(struct http_resp *r)
{
    if (r->status < 200) {              /* 1xx: an interim answer, the real one follows (101 is refused) */
        if (r->status == 101)
            return HTTP_EPROTO;
        {
            unsigned long limit = r->limit;
            int head = r->head;
            unsigned hb = r->hbytes;
            http_resp_init(r, limit, head);
            r->hbytes = hb;             /* every interim header counts against the 4 KB */
        }
        return HTTP_NEED;
    }
    if (r->chunked && r->has_len)
        return HTTP_EPROTO;             /* both: refused rather than guessed */
    if (r->head || r->status == 204 || r->status == 304) {
        r->st = S_DONE;
        return HTTP_HEAD;
    }
    if (r->chunked)
        r->st = S_CSIZE;
    else if (r->has_len) {
        if (r->len > r->limit)
            return HTTP_E2BIG;
        r->left = r->len;
        r->st = r->len ? S_LEN : S_DONE;
    } else {
        r->close = 1;                   /* the body ends when the connection does */
        r->st = S_CLOSE;
    }
    return HTTP_HEAD;
}

/* a chunk-size line: HEX [;extensions] */
static int chunk_size(struct http_resp *r, const char *l)
{
    unsigned long v = 0;
    int digits = 0;
    for (; *l && *l != ';' && *l != ' ' && *l != '\t'; l++) {
        int c = lc((unsigned char)*l), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return HTTP_EPROTO;
        if (v > (0xFFFFFFFFul >> 4))
            return HTTP_E2BIG;          /* would overflow */
        v = v << 4 | (unsigned long)d;
        digits++;
    }
    if (!digits)
        return HTTP_EPROTO;
    if (v > r->limit - r->got)
        return HTTP_E2BIG;
    r->left = v;
    r->st = v ? S_CDATA : S_TRAILER;
    return 0;
}

int http_resp_feed(struct http_resp *r, const unsigned char *in, size_t n, size_t *used, const unsigned char **body,
                   size_t *blen)
{
    size_t i = 0;
    int rc;
    *body = 0;
    *blen = 0;
    while (i < n || r->st == S_DONE) {
        switch (r->st) {
        case S_DONE:
            *used = i;
            return HTTP_END;
        case S_ERR:
            *used = i;
            return HTTP_EPROTO;
        case S_LEN:
        case S_CDATA:
        case S_CLOSE: {
            size_t k = n - i;
            if (r->st != S_CLOSE && k > r->left)
                k = r->left;
            if (r->st == S_CLOSE && k > r->limit - r->got)
                return *used = i, fail(r, HTTP_E2BIG);
            *body = in + i;
            *blen = k;
            r->got += k;
            if (r->st != S_CLOSE) {
                r->left -= k;
                if (!r->left)
                    r->st = r->st == S_LEN ? S_DONE : S_CCRLF;
            }
            *used = i + k;
            return HTTP_DATA;
        }
        default:
            break;
        }
        /* a line: the status, a header, a chunk size, the CRLF after a chunk, a trailer */
        {
            unsigned char c = in[i++];
            if (r->st != S_CSIZE && r->st != S_CCRLF && ++r->hbytes > HTTP_HDRMAX)
                return *used = i, fail(r, HTTP_E2BIG);
            if (c != '\n') {
                if (r->llen >= sizeof r->line - 1)
                    return *used = i, fail(r, HTTP_E2BIG);
                if (c == 0)
                    return *used = i, fail(r, HTTP_EPROTO);
                r->line[r->llen++] = (char)c;
                continue;
            }
            if (r->llen && r->line[r->llen - 1] == '\r')
                r->llen--;
            r->line[r->llen] = 0;
            r->llen = 0;
            switch (r->st) {
            case S_STATUS:
                if ((rc = status_line(r, r->line)) < 0)
                    return *used = i, fail(r, rc);
                r->st = S_HEADERS;
                break;
            case S_HEADERS:
                if (r->line[0]) {
                    if ((rc = header_line(r, r->line)) < 0)
                        return *used = i, fail(r, rc);
                    break;
                }
                rc = headers_done(r);
                if (rc < 0)
                    return *used = i, fail(r, rc);
                if (rc == HTTP_HEAD) {
                    *used = i;
                    return HTTP_HEAD;
                }
                break;
            case S_CSIZE:
                if ((rc = chunk_size(r, r->line)) < 0)
                    return *used = i, fail(r, rc);
                break;
            case S_CCRLF:
                if (r->line[0])
                    return *used = i, fail(r, HTTP_EPROTO);
                r->st = S_CSIZE;
                break;
            case S_TRAILER:
                if (!r->line[0])
                    r->st = S_DONE;
                break;
            }
        }
    }
    *used = i;
    return HTTP_NEED;
}

int http_resp_eof(struct http_resp *r)
{
    if (r->st == S_CLOSE || r->st == S_DONE) {
        r->st = S_DONE;
        return HTTP_END;
    }
    r->st = S_ERR;
    return HTTP_EIO;
}

int http_resume(const struct http_resp *r, unsigned long from, unsigned long total)
{
    if (r->status == 200) {
        if (r->has_len && r->len != total)
            return HTTP_EPROTO;
        return HTTP_RESTART;
    }
    if (r->status != 206 || !r->has_range || r->range_first != from || r->range_total != total ||
        r->range_last != total - 1 || (r->has_len && r->len != r->range_last - r->range_first + 1))
        return HTTP_EPROTO;
    return HTTP_RESUME;
}
