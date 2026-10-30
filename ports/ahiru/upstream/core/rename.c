/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* rename - new names from a pattern, see rename.h. Plain C. */
#include "rename.h"

#define MAXCAP 16

static int low(int c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
    return c;
}

/* does s match p? caps[] get the start and length of what each * and ?
 * took (no case, as AmigaDOS). One way back only: on a mismatch the last
 * * takes one more character. Each * takes the shortest run that lets
 * the rest match ("*.*" on "a.b.c" gives "a" and "b.c"), and the time
 * grows as name x pattern - trying every split was exponential. */
static int match(const char *p, const char *s, const char **cs, int *cl)
{
    const char *bp = 0, *bs = 0;
    int nc = 0, bnc = 0;
    while (*s) {
        if (*p == '*') {
            cs[nc] = s;
            cl[nc++] = 0;
            bp = ++p, bs = s, bnc = nc;
        } else if (*p == '?') {
            cs[nc] = s;
            cl[nc++] = 1;
            p++, s++;
        } else if (*p && low((unsigned char)*p) == low((unsigned char)*s)) {
            p++, s++;
        } else {
            if (!bp) return 0;
            cl[bnc - 1]++;             /* the last * takes one more */
            p = bp, s = ++bs, nc = bnc;
        }
    }
    while (*p == '*') {
        cs[nc] = s;
        cl[nc++] = 0;
        p++;
    }
    return !*p;
}

static int count(const char *p)
{
    int n = 0;
    for (; *p; p++) n += *p == '*' || *p == '?';
    return n;
}

int ren_make(const char *from, const char *to, const char *name, long num, char *out, int size)
{
    const char *cs[MAXCAP];
    int cl[MAXCAP], nc, o = 0, star = 0, ques = 0, j;
    char kind[MAXCAP];
    if (!from[0]) from = "*";
    nc = count(from);
    if (nc > MAXCAP || !match(from, name, cs, cl)) return 0;
    /* which capture is a * and which a ? (numbered apart in to) */
    for (j = 0, o = 0; from[j]; j++)
        if (from[j] == '*' || from[j] == '?') kind[o++] = from[j];
    o = 0;
    while (*to) {
        if (*to == '*' || *to == '?') {
            /* the n-th * of to takes the n-th * of from; ? likewise */
            int want = *to == '*' ? star++ : ques++, seen = 0, c, k;
            for (c = 0; c < nc; c++)
                if (kind[c] == *to && seen++ == want) break;
            if (c < nc)
                for (k = 0; k < cl[c]; k++) {
                    if (o >= size - 1) return 0;
                    out[o++] = cs[c][k];
                }
            to++;
        } else if (*to == '#') {
            /* a number, as many digits as #s (more when it needs them) */
            int w = 0, k;
            char d[12];
            long v = num;
            while (*to == '#') w++, to++;
            k = 0;
            do d[k++] = (char)('0' + v % 10); while ((v /= 10) && k < 11);
            while (k < w && k < 11) d[k++] = '0';
            while (k) {
                if (o >= size - 1) return 0;
                out[o++] = d[--k];
            }
        } else {
            if (o >= size - 1) return 0;
            out[o++] = *to++;
        }
    }
    out[o] = 0;
    return o > 0;
}
