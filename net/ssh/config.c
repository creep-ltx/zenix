/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ~/.ssh/config, the subset of DESIGN D21 4.4 (OpenSSH's ssh_config(5)): Host (patterns with *, ? and !), HostName,
 * User, Port, IdentityFile, ServerAliveInterval; keywords in any case, "Key value" or "Key=value", values may be
 * quoted. The first value given for a keyword wins. Unknown keywords and Match blocks are reported and ignored.
 */
#include <string.h>
#include "ssh_int.h"

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int kw(const char *t, long n, const char *k)
{
    long i;

    if (n != (long)strlen(k))
        return 0;
    for (i = 0; i < n; i++)
        if (lower((unsigned char)t[i]) != k[i])
            return 0;
    return 1;
}

static int glob(const char *p, long pn, const char *s, long sn)
{
    long pi = 0, si = 0, star = -1, mark = 0;

    while (si < sn) {
        if (pi < pn && (p[pi] == '?' || lower((unsigned char)p[pi]) == lower((unsigned char)s[si]))) {
            pi++;
            si++;
        } else if (pi < pn && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star >= 0) {
            pi = star + 1;
            si = ++mark;
        } else {
            return 0;
        }
    }
    while (pi < pn && p[pi] == '*')
        pi++;
    return pi == pn;
}

/* the next word of a line: quoted or not; returns its end (after a closing quote) or -1 for an open quote */
static long word(const char *t, long i, long e, long *ws, long *we)
{
    while (i < e && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r'))
        i++;
    if (i < e && t[i] == '"') {
        *ws = ++i;
        while (i < e && t[i] != '"')
            i++;
        if (i >= e)
            return -1;
        *we = i;
        return i + 1;
    }
    *ws = i;
    while (i < e && t[i] != ' ' && t[i] != '\t' && t[i] != '\r' && t[i] != '=')
        i++;
    *we = i;
    return i;
}

static void copy(char *dst, long cap, const char *src, long n, int *set)
{
    if (*set)
        return;
    if (n > cap - 1)
        n = cap - 1;
    memcpy(dst, src, (size_t)n);
    dst[n] = 0;
    *set = 1;
}

static int number(const char *t, long n, long max)
{
    long v = 0, i;

    if (n < 1 || n > 9)
        return -1;
    for (i = 0; i < n; i++) {
        if (t[i] < '0' || t[i] > '9')
            return -1;
        v = v * 10 + (t[i] - '0');
    }
    return v > max ? -1 : (int)v;
}

int ssh_config(const char *text, long n, const char *host, struct ssh_cfg *c,
               void (*report)(void *u, long line, const char *what), void *u)
{
    long i = 0, ln = 0, hn = (long)strlen(host);
    int active = 1, bad = 0, s_host = 0, s_user = 0, s_port = 0, s_id = 0, s_alive = 0;

    memset(c, 0, sizeof *c);
    c->alive = -1;
#define REPORT(w) do { bad++; if (report) report(u, ln, (w)); } while (0)
    while (i < n) {
        long e = i, ks, ke, vs, ve, j;

        while (e < n && text[e] != '\n')
            e++;
        ln++;
        j = word(text, i, e, &ks, &ke);
        i = e + 1;
        if (j < 0) {
            REPORT("an open quote");
            continue;
        }
        if (ks >= ke || text[ks] == '#')
            continue;
        while (j < e && (text[j] == ' ' || text[j] == '\t'))
            j++;
        if (j < e && text[j] == '=')
            j++;
        if (kw(text + ks, ke - ks, "host")) {
            int yes = 0, no = 0;

            while ((j = word(text, j, e, &vs, &ve)) >= 0 && vs < ve) {
                int neg = text[vs] == '!';

                if (glob(text + vs + neg, ve - vs - neg, host, hn)) {
                    if (neg) no = 1; else yes = 1;
                }
            }
            if (j < 0)
                REPORT("an open quote");
            active = yes && !no;
            continue;
        }
        if (kw(text + ks, ke - ks, "match")) {
            REPORT("Match is not supported: its block is skipped");
            active = 0;
            continue;
        }
        j = word(text, j, e, &vs, &ve);
        if (j < 0 || vs >= ve) {
            REPORT("a keyword without a value");
            continue;
        }
        if (kw(text + ks, ke - ks, "hostname")) {
            if (active) copy(c->hostname, sizeof c->hostname, text + vs, ve - vs, &s_host);
        } else if (kw(text + ks, ke - ks, "user")) {
            if (active) copy(c->user, sizeof c->user, text + vs, ve - vs, &s_user);
        } else if (kw(text + ks, ke - ks, "identityfile")) {
            if (active) copy(c->identity, sizeof c->identity, text + vs, ve - vs, &s_id);
        } else if (kw(text + ks, ke - ks, "port")) {
            int v = number(text + vs, ve - vs, 65535);

            if (v < 1)
                REPORT("a bad Port");
            else if (active && !s_port) {
                c->port = v;
                s_port = 1;
            }
        } else if (kw(text + ks, ke - ks, "serveraliveinterval")) {
            int v = number(text + vs, ve - vs, 86400);

            if (v < 0)
                REPORT("a bad ServerAliveInterval");
            else if (active && !s_alive) {
                c->alive = v;
                s_alive = 1;
            }
        } else {
            REPORT("an unknown keyword, ignored");
        }
    }
#undef REPORT
    return bad;
}
