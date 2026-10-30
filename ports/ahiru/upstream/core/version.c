/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* version - see version.h */
#include "version.h"

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

static const char mon3[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

/* days since 1.1.1978 (AmigaDOS), -1 for a date that cannot be */
static long amdays(int y, int m, int d)
{
    static const short cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    long n = 0;
    int k;
    if (y < 100) y += y < 78 ? 2000 : 1900;            /* "10" is 2010, "93" is 1993 */
    if (y < 1978 || y > 2099 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    for (k = 1978; k < y; k++) n += (k & 3) ? 365 : 366;
    n += cum[m - 1] + d - 1;
    if (m > 2 && !(y & 3)) n++;
    return n;
}

static int num(const char *s, int n, int *i)
{
    int v = 0, k = 0;
    while (*i < n && s[*i] >= '0' && s[*i] <= '9' && k < 5) v = v * 10 + (s[(*i)++] - '0'), k++;
    return k ? v : -1;
}

long ver_date(const char *s, int n)
{
    int i, j, a, b, c;
    /* "(d.m.y)" or "(y-m-d)" */
    for (i = 0; i < n; i++)
        if (s[i] == '(') {
            j = i + 1;
            if ((a = num(s, n, &j)) < 0 || j >= n) continue;
            if (s[j] == '.') {
                j++;
                if ((b = num(s, n, &j)) < 0 || j >= n || s[j] != '.') continue;
                j++;
                if ((c = num(s, n, &j)) < 0) continue;
                return amdays(c, b, a);
            }
            if (s[j] == '-') {
                j++;
                if ((b = num(s, n, &j)) < 0 || j >= n || s[j] != '-') continue;
                j++;
                if ((c = num(s, n, &j)) < 0) continue;
                return amdays(a, b, c);
            }
        }
    /* "Jan  3 2011" (what __DATE__ makes) */
    for (i = 0; i + 3 <= n; i++) {
        int m;
        for (m = 0; m < 12; m++)
            if (s[i] == mon3[m * 3] && s[i + 1] == mon3[m * 3 + 1] && s[i + 2] == mon3[m * 3 + 2]) break;
        if (m == 12 || (i && s[i - 1] != ' ' && s[i - 1] != '(')) continue;
        j = i + 3;
        while (j < n && s[j] == ' ') j++;
        if ((a = num(s, n, &j)) < 0) continue;
        while (j < n && s[j] == ' ') j++;
        if ((c = num(s, n, &j)) < 0 || c < 1978) continue;
        return amdays(c, m + 1, a);
    }
    return -1;
}

/* "name 40.2 (1.1.93)": the version is the first word that starts with
 * a digit and holds a dot or ends the run of digits; names can hold
 * digits too ("lha 2.15", "exec 40.10", "CFile 1.0d1") */
void ver_parse(const char *s, int n, struct verinfo *v)
{
    int i = 0, t;
    v->ver = 0;
    v->rev = -1;
    v->days = -1;
    for (t = 0; t < n && t < (int)sizeof(v->text) - 1 && s[t] && s[t] != 10 && s[t] != 13; t++) v->text[t] = s[t];
    v->text[t] = 0;
    n = t;
    while (i < n) {
        /* the start of a word */
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] >= '0' && s[i] <= '9' && (i == 0 || s[i - 1] == ' ')) {
            int j = i, a = 0, b = 0, nb = 0;
            while (j < n && s[j] >= '0' && s[j] <= '9') a = a < 100000 ? a * 10 + (s[j] - '0') : a, j++;   /* no overflow */
            if (j < n && s[j] == '.' && j + 1 < n && s[j + 1] >= '0' && s[j + 1] <= '9') {
                j++;
                while (j < n && s[j] >= '0' && s[j] <= '9') b = b < 100000 ? b * 10 + (s[j] - '0') : b, j++, nb++;
            }
            if (nb || j >= n || s[j] == ' ' || s[j] == '(') {
                v->ver = a;
                v->rev = nb ? b : -1;
                v->found = 1;
                v->days = ver_date(s + j, n - j);       /* after the version */
                return;
            }
        }
        while (i < n && s[i] != ' ') i++;
    }
    v->found = 1;                       /* a string, but no number in it */
    v->days = ver_date(s, n);
}

int ver_scan(const unsigned char *b, long n, int last, struct verinfo *v)
{
    long i, lim = last ? n - 5 : n - VER_OVERLAP;
    for (i = 0; i <= lim; i++) {
        if (b[i] != '$' || b[i + 1] != 'V' || b[i + 2] != 'E' || b[i + 3] != 'R' || b[i + 4] != ':') continue;
        {
            long s = i + 5, e;
            while (s < n && b[s] == ' ') s++;
            for (e = s; e < n && e - s < 63 && b[e] && b[e] != 10 && b[e] != 13; e++)
                ;
            ver_parse((const char *)b + s, (int)(e - s), v);
            return 1;
        }
    }
    return 0;
}

/* the hunk file's first code hunk: where its bytes start in the file */
static long firstcode(const unsigned char *b, long n)
{
    long o = 4, k, first, last;
    if (n < 32 || be32(b) != 0x3F3) return -1;      /* HUNK_HEADER */
    while (o + 4 <= n && (k = (long)be32(b + o)) != 0) {       /* resident library names */
        o += 4 + k * 4;
        if (k < 0 || k > 1000) return -1;
    }
    o += 4;
    if (o + 12 > n) return -1;
    first = (long)be32(b + o + 4);
    last = (long)be32(b + o + 8);
    if (first < 0 || last < first || last - first > 1000) return -1;
    o += 12 + (last - first + 1) * 4;               /* the hunk sizes */
    if (o + 8 > n || (be32(b + o) & 0x3FFFFFFF) != 0x3E9) return -1;     /* HUNK_CODE */
    return o + 8;
}

int ver_romtag(const unsigned char *b, long n, struct verinfo *v)
{
    long c = firstcode(b, n), i;
    if (c < 0) return 0;
    for (i = c; i + 26 <= n; i += 2) {
        if (b[i] != 0x4A || b[i + 1] != 0xFC) continue;
        /* rt_MatchTag points at the RomTag itself: in the file, before
         * relocation, that is its own offset in the hunk */
        if ((long)be32(b + i + 2) != i - c) continue;
        if (b[i + 12] != 9 && b[i + 12] != 3 && b[i + 12] != 8) continue;  /* library, device, resource */
        v->found = 2;
        v->ver = b[i + 11];
        v->rev = -1;
        v->days = -1;
        v->text[0] = 0;
        {
            /* rt_IdString, an offset in the same hunk before relocation:
             * "clipboard.device 47.1 (4.10.21)" - the revision is there
             * (what the Version command shows for such a file) */
            long id = (long)be32(b + i + 18), e;
            if (id > 0 && c + id < n) {
                for (e = c + id; e < n && e - (c + id) < 63 && b[e] && b[e] != 10 && b[e] != 13; e++)
                    ;
                if (e < n) {
                    struct verinfo w;
                    w.found = 0;
                    ver_parse((const char *)b + c + id, (int)(e - (c + id)), &w);
                    if (w.ver == v->ver) {          /* the same version: trust it */
                        w.found = 2;
                        *v = w;
                    }
                }
            }
        }
        return 1;
    }
    return 0;
}

int ver_cmp(const struct verinfo *a, const struct verinfo *b)
{
    int an = a->found && (a->ver || a->rev >= 0), bn = b->found && (b->ver || b->rev >= 0);
    if (an && bn) {
        if (a->ver != b->ver) return a->ver < b->ver ? -1 : 1;
        if (a->rev >= 0 && b->rev >= 0 && a->rev != b->rev) return a->rev < b->rev ? -1 : 1;
    } else if (an || bn) {
        return 0;                       /* one says a number, the other not */
    }
    /* the same version (or neither has one): the build dates decide - set
     * when it was compiled, unlike the file's date on an Amiga with no clock */
    if (a->found && b->found && a->days >= 0 && b->days >= 0 && a->days != b->days)
        return a->days < b->days ? -1 : 1;
    return 0;
}

void ver_str(const struct verinfo *v, char *d, int size)
{
    char t[24];
    int n = 0, x, k;
    if (!v->found || (!v->ver && v->rev < 0 && v->days < 0) || size < 2) {
        if (size > 0) d[0] = 0;
        return;
    }
    /* small decimal printing, no stdio in core */
    x = v->ver;
    k = 0;
    do t[k++] = (char)('0' + x % 10); while ((x /= 10) && k < 10);
    while (k && n < size - 1) d[n++] = t[--k];
    if (v->rev >= 0 && n < size - 2) {
        d[n++] = '.';
        x = v->rev;
        k = 0;
        do t[k++] = (char)('0' + x % 10); while ((x /= 10) && k < 10);
        while (k && n < size - 1) d[n++] = t[--k];
    }
    if (v->days >= 0 && n < size - 12) {
        /* " (24-Apr-10)" */
        long dd = v->days;
        int y = 1978, m = 0, ml, yd;
        static const char md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        while (dd >= (yd = (y & 3) ? 365 : 366)) dd -= yd, y++;
        while (m < 11 && dd >= (ml = md[m] + (m == 1 && !(y & 3)))) dd -= ml, m++;
        d[n++] = ' ', d[n++] = '(';
        d[n++] = (char)('0' + (dd + 1) / 10), d[n++] = (char)('0' + (dd + 1) % 10), d[n++] = '-';
        d[n++] = mon3[m * 3], d[n++] = mon3[m * 3 + 1], d[n++] = mon3[m * 3 + 2], d[n++] = '-';
        d[n++] = (char)('0' + (y % 100) / 10), d[n++] = (char)('0' + y % 10), d[n++] = ')';
    }
    d[n] = 0;
}
