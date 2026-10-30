/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* keymap - see keymap.h. Grew out of Ahiru's keys.c (10.10.2026). */
#include <string.h>
#include <stdlib.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "keymap.h"
#include "os.h"
#include "../core/core.h"
#include "../core/util.h"

#define MAXBIND 256

struct bind {
    unsigned char ctx;
    short k1, k2;               /* k2 0: a single key */
    short act;
    char *arg;                  /* NULL: none */
};

static struct bind binds[MAXBIND];
static int nbind;
static const struct kmact *acts;
static int nacts, nctx;
static unsigned single;         /* contexts that take one key at a time */
static const char *const *ctxnames, *const *ctxheads;

void km_setup(const struct kmact *a, int na, const char *const *names, const char *const *heads, int n)
{
    acts = a;
    nacts = na;
    ctxnames = names;
    ctxheads = heads;
    nctx = n;
}

void km_single(unsigned mask)
{
    single = mask;
}

static char *xstrdup(const char *s)
{
    char *d = xalloc(strlen(s) + 1);
    if (d) strcpy(d, s);
    return d;
}

static const struct { const char *n; int k; } knames[] = {
    { "Up", KM_UP }, { "Down", KM_DOWN }, { "Left", KM_LEFT }, { "Right", KM_RIGHT },
    { "S-Up", KM_SUP }, { "S-Down", KM_SDOWN }, { "S-Left", KM_SLEFT }, { "S-Right", KM_SRIGHT },
    { "C-Up", KM_CUP }, { "C-Down", KM_CDOWN }, { "C-Left", KM_CLEFT }, { "C-Right", KM_CRIGHT },
    { "Return", 13 }, { "Esc", 27 }, { "Tab", 9 }, { "Space", ' ' }, { "Del", 127 },
    { "BS", 8 }, { "S-BS", KM_SBS }, { "C-BS", KM_CBS }, { "Help", KM_HELP },
};
#define NKNAMES ((int)(sizeof(knames) / sizeof(knames[0])))

/* one key from s (advanced past it); 0 = not a key */
static int onekey(const char **s)
{
    const char *p = *s, *e;
    int i, l;
    if (*p != '<' || !p[1] || p[1] == '>') {
        *s = p + 1;
        return (unsigned char)*p;
    }
    if (!(e = strchr(p, '>'))) return 0;
    l = (int)(e - p - 1);
    *s = e + 1;
    for (i = 0; i < NKNAMES; i++)
        if ((int)strlen(knames[i].n) == l && strncmp(knames[i].n, p + 1, l) == 0) return knames[i].k;
    if (l == 3 && p[1] == 'C' && p[2] == '-') return p[3] & 31;              /* <C-d> */
    if (l >= 2 && l <= 3 && p[1] == 'F' && atoi(p + 2) >= 1 && atoi(p + 2) <= 10)
        return KM_F1 + atoi(p + 2) - 1;
    return 0;
}

void km_keyname(int k, char *dst)
{
    int i;
    for (i = 0; i < NKNAMES; i++)
        if (knames[i].k == k) {
            sfmt(dst, 12, "<%s>", knames[i].n);
            return;
        }
    if (k >= KM_F1 && k < KM_F1 + 10) sfmt(dst, 12, "<F%ld>", (long)(k - KM_F1 + 1));
    else if (k > 0 && k < 32) dst[0] = '<', dst[1] = 'C', dst[2] = '-', dst[3] = (char)(k + 96), dst[4] = '>', dst[5] = 0;
    else dst[0] = (char)k, dst[1] = 0;
}

static int actof(const char *name, int ctx)
{
    int i;
    for (i = 0; i < nacts; i++)
        if ((acts[i].ctx == ctx || acts[i].ctx == 0) && nocase(acts[i].name, name) == 0) return i;
    return -1;
}

int km_action(const char *name, int ctx)
{
    return actof(name, ctx);
}

static void bindline(char *line, int *ctx, int lineno, char *bad, int badsize)
{
    char *p = line, *keys, *act, *arg = NULL;
    const char *k;
    int k1, k2 = 0, a, i;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == ';' || *p == '#') return;
    if (*p == '[' && p[1] && p[1] != ' ' && strchr(p, ']') && !strchr(p, ' ')) {   /* (not the [ key) */
        int pl = strlen(p);
        for (i = 0; i < nctx; i++) {
            int l = strlen(ctxnames[i]);
            char sec[24];
            if (l > 20 || l + 2 > pl) continue;     /* (never past the line's end) */
            memcpy(sec, p + 1, l);
            sec[l] = 0;
            if (p[l + 1] == ']' && nocase(sec, ctxnames[i]) == 0) {
                *ctx = i + 1;
                return;
            }
        }
        if (!bad[0]) sfmt(bad, badsize, "line %ld: unknown section", (long)lineno);
        return;
    }
    keys = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    while (*p == ' ' || *p == '\t') p++;
    act = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) {
        *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p) arg = p;
    }
    k = keys;
    k1 = onekey(&k);
    if (*k) k2 = onekey(&k);
    a = actof(act, *ctx);
    if (!k1 || (*k) || a < 0) {
        if (!bad[0]) sfmt(bad, badsize, "line %ld: %.40s not understood", (long)lineno, keys);
        return;
    }
    if (k2 && (single & (1u << *ctx))) {
        if (!bad[0]) sfmt(bad, badsize, "line %ld: [%s] takes one key, not %.20s", (long)lineno,
                          ctxnames[*ctx - 1], keys);
        return;
    }
    /* a new binding for these keys replaces the old one */
    for (i = 0; i < nbind; i++)
        if (binds[i].ctx == *ctx && binds[i].k1 == k1 && binds[i].k2 == k2) break;
    if (i == nbind) {
        if (nbind == MAXBIND) return;
        nbind++;
    } else {
        xfree(binds[i].arg);
    }
    binds[i].ctx = (unsigned char)*ctx;
    binds[i].k1 = (short)k1;
    binds[i].k2 = (short)k2;
    binds[i].act = (short)a;
    binds[i].arg = arg ? xstrdup(arg) : NULL;
}

static void bindtext(char *t, char *bad, int badsize)
{
    int ctx = 1, n = 0;
    char *line = t, *e;
    while (line && *line) {
        if ((e = strchr(line, '\n'))) *e = 0;
        {
            int l = (int)strlen(line);
            while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        }
        bindline(line, &ctx, ++n, bad, badsize);
        line = e ? e + 1 : NULL;
    }
}

/* a key bound alone and also as the start of a prefix: the prefix wins
 * and the key's own action is never reached - said, not silently lost */
static void shadowed(char *bad, int badsize)
{
    int i;
    char kn[12];
    for (i = 0; i < nbind && !bad[0]; i++)
        if (!binds[i].k2 && binds[i].act > 0 && km_isprefix(binds[i].ctx, binds[i].k1)) {
            km_keyname(binds[i].k1, kn);
            sfmt(bad, badsize, "%s starts two-key keys in [%s]: its %s is lost", kn,
                 ctxnames[binds[i].ctx - 1], acts[binds[i].act].name);
        }
}

void km_load(const char *file, const char *defaults, char *bad, int badsize)
{
    char *t;
    BPTR fh;
    LONG n, dl = strlen(defaults);
    char fbad[100];
    bad[0] = fbad[0] = 0;
    if ((t = xstrdup(defaults))) {
        bindtext(t, fbad, sizeof(fbad));
        xfree(t);
    }
    fbad[0] = 0;                        /* (the built-in ones are right) */
    os_quiet(1);
    fh = Open((STRPTR)file, MODE_OLDFILE);
    if (!fh) {
        /* the defaults are written only where there is no file at all: a
         * busy or unreadable one is the user's and stays */
        if (IoErr() == ERROR_OBJECT_NOT_FOUND && (fh = Open((STRPTR)file, MODE_NEWFILE))) {
            Write(fh, (APTR)defaults, dl);
            Close(fh);
        }
        os_quiet(0);
        return;
    }
    os_quiet(0);
    /* the whole file, however long (Seek's answer is the old position) */
    Seek(fh, 0, OFFSET_END);
    n = Seek(fh, 0, OFFSET_BEGINNING);
    if (n < 0 || n > 65535) n = 16383;
    if ((t = xalloc(n + 1))) {
        n = Read(fh, t, n);
        t[n > 0 ? n : 0] = 0;
        bindtext(t, fbad, sizeof(fbad));
        xfree(t);
    }
    Close(fh);
    shadowed(fbad, sizeof(fbad));
    if (fbad[0]) sfmt(bad, badsize, "%s %s", filepart(file), fbad);
}

int km_isprefix(int ctx, int k)
{
    int i;
    for (i = 0; i < nbind; i++)
        if (binds[i].ctx == ctx && binds[i].k1 == k && binds[i].k2 && binds[i].act > 0) return 1;
    return 0;
}

int km_find(int ctx, int k1, int k2, const char **arg)
{
    int i;
    if (arg) *arg = NULL;
    for (i = 0; i < nbind; i++)
        if (binds[i].ctx == ctx && binds[i].k1 == k1 && binds[i].k2 == k2) {
            if (arg) *arg = binds[i].arg;
            return binds[i].act;
        }
    return 0;
}

int km_hints(int ctx, int k, const char **items, int max, char (*lab)[12], char (*desc)[48])
{
    int i, n = 0;
    for (i = 0; i < nbind && n < max; i++) {
        if (binds[i].ctx != ctx || binds[i].k1 != k || !binds[i].k2 || binds[i].act <= 0) continue;
        km_keyname(binds[i].k2, lab[n]);
        if (binds[i].arg) sfmt(desc[n], 48, "%.20s %.24s", acts[binds[i].act].desc, binds[i].arg);
        else sfmt(desc[n], 48, "%.46s", acts[binds[i].act].desc);
        items[2 * n] = lab[n];
        items[2 * n + 1] = desc[n];
        n++;
    }
    return n;
}

char *km_help(const char *head, const char *tail)
{
    int size = 4000 + nbind * 80;
    char *t = xalloc(size), kb[32], line[200];
    int c, a, i, l;
    if (!t) return NULL;
    sfmt(t, size, "%s", head);
    for (c = 1; c <= nctx; c++) {
        l = strlen(t);
        sfmt(t + l, size - l, "%s%s\n", c > 1 ? "\n" : "", ctxheads[c - 1]);
        for (a = 1; a < nacts; a++) {
            if (acts[a].ctx != c) continue;
            for (i = 0; i < nbind; i++) {
                if (binds[i].ctx != c || binds[i].act != a) continue;
                km_keyname(binds[i].k1, kb);
                if (binds[i].k2) km_keyname(binds[i].k2, kb + strlen(kb));
                if (binds[i].arg) sfmt(line, sizeof(line), "  %-12s %s %s\n", kb, acts[a].desc, binds[i].arg);
                else sfmt(line, sizeof(line), "  %-12s %s\n", kb, acts[a].desc);
                l = strlen(t);
                if (l + (int)strlen(line) < size - 400) strcpy(t + l, line);
            }
        }
    }
    l = strlen(t);
    sfmt(t + l, size - l, "%s", tail);
    return t;
}

void km_first(int ctx, int act, char *dst)
{
    int i;
    for (i = 0; i < nbind; i++)
        if (binds[i].ctx == ctx && binds[i].act == act) {
            char t[40];
            km_keyname(binds[i].k1, t);
            if (binds[i].k2) km_keyname(binds[i].k2, t + strlen(t));
            sfmt(dst, KM_FIRSTMAX, "%s", t);
            return;
        }
    strcpy(dst, "?");
}

void km_free(void)
{
    int i;
    for (i = 0; i < nbind; i++) xfree(binds[i].arg);
    nbind = 0;
}
