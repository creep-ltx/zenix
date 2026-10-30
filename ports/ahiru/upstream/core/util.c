/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* util - see util.h. No Amiga headers here, on purpose. */
#include <string.h>
#include <stdarg.h>
#include "util.h"

/* ---- sfmt -------------------------------------------------------- */

struct out { char *d; int left; int n; };

static void put(struct out *o, char c)
{
    if (o->left > 1) {
        *o->d++ = c;
        o->left--;
        o->n++;
    }
}

static void putnum(struct out *o, unsigned long v, int neg, int base, int upper,
                   int width, int left, int zero)
{
    char tmp[16];
    int n = 0, len, pad;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = dig[v % base];
        v /= base;
    } while (v);
    len = n + neg;
    pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) put(o, ' ');
    if (neg) put(o, '-');
    if (!left && zero) while (pad-- > 0) put(o, '0');
    while (n) put(o, tmp[--n]);
    if (left) while (pad-- > 0) put(o, ' ');
}

int sfmt(char *dst, int size, const char *f, ...)
{
    va_list ap;
    struct out o;
    if (size < 1) return 0;
    o.d = dst;
    o.left = size;
    o.n = 0;
    va_start(ap, f);
    while (*f) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, c;
        if (*f != '%') {
            put(&o, *f++);
            continue;
        }
        f++;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            prec = 0;
            f++;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l') {
            lng = 1;
            f++;
        }
        c = *f ? *f++ : 0;
        switch (c) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
            putnum(&o, v < 0 ? (unsigned long)(-v) : (unsigned long)v, v < 0, 10, 0,
                   width, left, zero);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            putnum(&o, v, 0, c == 'u' ? 10 : 16, c == 'X', width, left, zero);
            break;
        }
        case 'c': {
            int pad = width > 1 ? width - 1 : 0;
            if (!left) while (pad-- > 0) put(&o, ' ');
            put(&o, (char)va_arg(ap, int));
            if (left) while (pad-- > 0) put(&o, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0, pad, i;
            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || n < prec)) n++;
            pad = width > n ? width - n : 0;
            if (!left) while (pad-- > 0) put(&o, ' ');
            for (i = 0; i < n; i++) put(&o, s[i]);
            if (left) while (pad-- > 0) put(&o, ' ');
            break;
        }
        case '%':
            put(&o, '%');
            break;
        case 0:
            f--;
            break;
        default:
            put(&o, (char)c);
        }
    }
    *o.d = 0;
    va_end(ap);
    return o.n;
}

/* ---- paths ------------------------------------------------------- */

int addpart(char *path, const char *name, int size)
{
    int l = strlen(path), n = strlen(name), sep = 0;
    if (l > 0 && path[l - 1] != ':' && path[l - 1] != '/') sep = 1;
    if (l + sep + n + 1 > size) return 0;
    if (sep) path[l++] = '/';
    memcpy(path + l, name, n + 1);
    return 1;
}

const char *filepart(const char *path)
{
    const char *last = path;
    for (; *path; path++)
        if (*path == '/' || *path == ':') last = path + 1;
    return last;
}

int pathlen(const char *path)
{
    int i, sl = -1, co = -1;
    for (i = 0; path[i]; i++) {
        if (path[i] == '/') sl = i;
        if (path[i] == ':') co = i;
    }
    if (sl >= 0) return sl;
    if (co >= 0) return co + 1;
    return 0;
}

int isroot(const char *path)
{
    int l = strlen(path);
    return l > 0 && path[l - 1] == ':' && strchr(path, ':') == path + l - 1;
}

int pathup(char *path)
{
    int l = strlen(path), start;
    if (l == 0 || isroot(path)) return -1;
    if (path[l - 1] == '/') path[--l] = 0;    /* "a/b/" counts as "a/b" */
    start = pathlen(path);
    if (path[start] == '/') {
        path[start] = 0;
        return start + 1;
    }
    path[start] = 0;    /* "Work:x" -> "Work:", "x" -> "" */
    return start;
}

/* ---- patterns ---------------------------------------------------- */

static int up(int c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7)) return c - 32;
    return c;
}

int haswild(const char *s)
{
    for (; *s; s++)
        if (*s == '#' || *s == '?' || *s == '*') return 1;
    return 0;
}

/* every place in the pattern the name could have reached so far, one
 * character at a time: time grows as name x pattern, never more (trying
 * every split for each * was exponential - hours on a 68000) */
#define PMAX 255

/* how long the token at pat[i] is; *loop: 1 = any characters, 2 = any
 * number of the next character (#x), 0 = one character */
static int ptok(const unsigned char *p, int *loop)
{
    *loop = 0;
    if (p[0] == '*') return *loop = 1, 1;
    if (p[0] == '#' && p[1] == '?') return *loop = 1, 2;
    if (p[0] == '#' && p[1]) return *loop = 2, 2;
    return 1;
}

/* a loop can also be taken no times: what i reaches, i + its length does */
static void pclose(const unsigned char *p, int len, unsigned char *set)
{
    int i, loop, l;
    for (i = 0; i < len; i++)
        if (set[i]) {
            l = ptok(p + i, &loop);
            if (loop) set[i + l] = 1;
        }
}

int pmatch(const char *pat, const char *name)
{
    const unsigned char *p = (const unsigned char *)pat, *n = (const unsigned char *)name;
    unsigned char a[PMAX + 1], b[PMAX + 1], *cur = a, *nxt = b, *t;
    int len = (int)strlen(pat), i, loop, any;
    if (len > PMAX) return 0;
    memset(cur, 0, (size_t)len + 1);
    cur[0] = 1;
    pclose(p, len, cur);
    for (; *n; n++) {
        int c = up(*n);
        memset(nxt, 0, (size_t)len + 1);
        any = 0;
        for (i = 0; i < len; i++) {
            if (!cur[i]) continue;
            ptok(p + i, &loop);
            if (loop == 1 || (loop == 2 && up(p[i + 1]) == c)) nxt[i] = 1, any = 1;
            else if (!loop && (p[i] == '?' || up(p[i]) == c)) nxt[i + 1] = 1, any = 1;
        }
        if (!any) return 0;
        pclose(p, len, nxt);
        t = cur, cur = nxt, nxt = t;
    }
    return cur[len];
}

/* ---- dates ------------------------------------------------------- */

static const char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
static const char mnames[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

void dsplit(long days, int *y, int *m, int *d)
{
    int yy = 1978, mm = 0, yl = 365, ml;
    if (days < 0) days = 0;
    while (days >= yl) {
        days -= yl;
        yy++;
        yl = (yy & 3) == 0 ? 366 : 365;      /* exact for 1901-2099 */
    }
    for (;;) {
        ml = mdays[mm];
        if (mm == 1 && (yy & 3) == 0) ml = 29;
        if (days < ml) break;
        days -= ml;
        mm++;
    }
    *y = yy;
    *m = mm + 1;
    *d = (int)days + 1;
}

void datestr(long days, char *dst)
{
    int y, m, d;
    dsplit(days, &y, &m, &d);
    sfmt(dst, 10, "%02d-%.3s-%02d", d, mnames + (m - 1) * 3, y % 100);
}

void ddmon(long days, char *dst)
{
    int y, m, d;
    dsplit(days, &y, &m, &d);
    sfmt(dst, 6, "%02d%.3s", d, mnames + (m - 1) * 3);
}

void timestr(long mins, char *dst)
{
    if (mins < 0) mins = 0;
    sfmt(dst, 6, "%02ld:%02ld", (mins / 60) % 24, mins % 60);
}
