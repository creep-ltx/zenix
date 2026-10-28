/* SPDX-License-Identifier: GPL-2.0-only */
/* wget's core (DESIGN D21 8 step 7, D22 7.4-7.5): see wget.h. No system calls, no allocation, no writable globals. */
#include <string.h>
#include "wget.h"

int wget_args(struct wget_opts *o, int argc, char *const *argv, const char **why)
{
    int i, opts = 1;

    memset(o, 0, sizeof *o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (opts && !strcmp(a, "--")) {
            opts = 0;
            continue;
        }
        if (opts && a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--quiet"))
                o->quiet = 1;
            else if (!strcmp(a, "--continue"))
                o->cont = 1;
            else if (!strncmp(a, "--output-document=", 18) && a[18])
                o->output = a + 18;
            else if (!strcmp(a, "--output-document") && i + 1 < argc)
                o->output = argv[++i];
            else {
                *why = "unknown option";
                return WGET_EXIT_USAGE;
            }
            continue;
        }
        if (opts && a[0] == '-' && a[1]) {
            const char *c;

            for (c = a + 1; *c; c++) {
                if (*c == 'q')
                    o->quiet = 1;
                else if (*c == 'c')
                    o->cont = 1;
                else if (*c == 'O') {
                    if (c[1])
                        o->output = c + 1;
                    else if (i + 1 < argc)
                        o->output = argv[++i];
                    else {
                        *why = "-O needs a file name";
                        return WGET_EXIT_USAGE;
                    }
                    break;
                } else {
                    *why = "unknown option";
                    return WGET_EXIT_USAGE;
                }
            }
            continue;
        }
        if (o->nurl == WGET_URLMAX) {
            *why = "more than 16 URLs";
            return WGET_EXIT_USAGE;
        }
        o->url[o->nurl++] = a;
    }
    if (!o->nurl) {
        *why = "missing URL";
        return WGET_EXIT_USAGE;
    }
    return 0;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                        : -1;
}

/* a name is cut here, so that wget_nextname's ".N" always fits */
#define BASEMAX (WGET_NAMEMAX - 10)

int wget_name(char *out, size_t cap, const struct http_url *u)
{
    const char *p = u->path, *q, *s, *e, *end;
    size_t n = 0;

    if (cap < BASEMAX + 1 || p[0] != '/')
        return HTTP_EINVAL;
    end = p + strlen(p);
    q = strchr(p, '?');
    e = q ? q : end;
    for (s = e; s > p && s[-1] != '/'; s--)
        ;
    if (s == e || (e - s == 1 && s[0] == '.') || (e - s == 2 && s[0] == '.' && s[1] == '.')) {
        strcpy(out, "index.html");      /* a directory: GNU wget's default name */
        n = 10;
        s = e;                          /* the query, if any, follows */
    }
    for (; s < end && n < BASEMAX; s++) {
        int c = (unsigned char)*s;

        if (c == '%' && end - s >= 3 && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            int v = hexval(s[1]) << 4 | hexval(s[2]);

            if (v != '/' && v >= 0x20 && v != 0x7f) {
                out[n++] = (char)v;
                s += 2;
                continue;
            }
            /* kept as written: '/' or a control character would not make a safe name */
        }
        if (c == '/') {                 /* only in the query: written as GNU wget's unix rule does */
            if (n + 3 > BASEMAX)
                break;
            memcpy(out + n, "%2F", 3);
            n += 3;
            continue;
        }
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (!strcmp(out, ".") || !strcmp(out, ".."))
        strcpy(out, "index.html");      /* decoded from %2E: never a directory's name */
    return 0;
}

int wget_nextname(char *out, size_t cap, const char *base, unsigned n)
{
    size_t b = strlen(base), k = 0;
    char t[12];

    do {
        t[k++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    if (b + 1 + k > WGET_NAMEMAX || b + 1 + k + 1 > cap)
        return HTTP_EINVAL;
    memmove(out, base, b);
    out[b] = '.';
    while (k)
        out[++b] = t[--k];
    out[b + 1] = 0;
    return 0;
}

int wget_exit_merge(int a, int b)
{
    if (a == WGET_EXIT_OK)
        return b;
    if (b == WGET_EXIT_OK)
        return a;
    if (a == WGET_EXIT_GENERIC)
        return b;
    if (b == WGET_EXIT_GENERIC)
        return a;
    return a < b ? a : b;
}

int wget_plan_start(struct wget_plan *p, const struct wget_opts *o, int i, const char **why)
{
    memset(p, 0, sizeof *p);
    if (i < 0 || i >= o->nurl || http_url_parse(&p->url, o->url[i])) {
        *why = "not an http:// or https:// URL wget can fetch";
        return WGET_EXIT_USAGE;
    }
    if (o->output) {
        if (strlen(o->output) > WGET_NAMEMAX) {
            *why = "the -O file name is too long";
            return WGET_EXIT_USAGE;
        }
        strcpy(p->name, o->output);
        p->to_stdout = !strcmp(o->output, "-");
        p->append = i > 0;              /* every URL into the one file, truncated once */
        p->need_size = o->cont && i == 0 && !p->to_stdout;
        return 0;
    }
    if (wget_name(p->name, sizeof p->name, &p->url)) {
        *why = "no file name for this URL";
        return WGET_EXIT_USAGE;
    }
    p->need_size = o->cont;
    p->need_free_name = !o->cont;
    return 0;
}

void wget_plan_have(struct wget_plan *p, int exists, unsigned long size)
{
    if (p->need_size && exists && size > 0)
        p->from = size;
}

int wget_plan_answer(struct wget_plan *p, const struct http_resp *r, int *exitcode, const char **why)
{
    int rc;

    *exitcode = WGET_EXIT_OK;
    *why = "";
    if (http_is_redirect(r->status)) {
        if (!r->location[0]) {
            *why = "a redirect without a Location";
            *exitcode = WGET_EXIT_PROTO;
            return WGET_FAIL;
        }
        rc = http_redirect_rel(&p->url, r->location, &p->hops);
        if (rc == 0)
            return WGET_FOLLOW;
        *exitcode = WGET_EXIT_PROTO;
        *why = rc == HTTP_ELOOP ? "more than 3 redirects"
             : rc == HTTP_EPERM ? "a redirect from https to http is never followed"
                                : "a redirect to a URL wget cannot fetch";
        return WGET_FAIL;
    }
    if (r->status == 416 && p->from) {
        *why = "the file is already complete";
        return WGET_DONE;
    }
    if (r->status >= 200 && r->status < 300) {
        p->total = r->has_range ? r->range_total : r->has_len ? r->len : 0;
        if (p->from) {
            rc = http_resume(r, p->from, r->has_range ? r->range_total : r->len);
            if (rc == HTTP_RESUME) {
                p->append = 1;
                return WGET_WRITE;
            }
            if (rc == HTTP_RESTART) {   /* the server ignored the range: the file is never truncated (-c) */
                *why = "the server does not support continued downloads, which conflicts with -c; refusing to "
                       "truncate the existing file";
                *exitcode = WGET_EXIT_GENERIC;
                return WGET_FAIL;
            }
            *why = "the server's answer to the resume does not fit (Content-Range)";
            *exitcode = WGET_EXIT_PROTO;
            return WGET_FAIL;
        }
        if (r->status == 206) {
            *why = "a partial answer to a request for the whole file";
            *exitcode = WGET_EXIT_PROTO;
            return WGET_FAIL;
        }
        return WGET_WRITE;
    }
    if (r->status >= 400) {
        *why = "the server answered with an error";
        *exitcode = WGET_EXIT_SERVER;
        return WGET_FAIL;
    }
    *why = "an answer wget does not take";
    *exitcode = WGET_EXIT_PROTO;
    return WGET_FAIL;
}

static int lc(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int wget_same_server(const struct http_url *a, const struct http_url *b)
{
    const char *x = a->host, *y = b->host;

    if (a->tls != b->tls || a->port != b->port)
        return 0;
    for (; *x && *y; x++, y++)
        if (lc((unsigned char)*x) != lc((unsigned char)*y))
            return 0;
    return *x == *y;
}
