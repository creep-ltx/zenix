/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* find - see find.h. Folders are walked as copy walks them: soft links
 * are not followed (a link to Work: inside a folder would search all of
 * Work:). Text is looked for line by line through a chunk buffer, in
 * files the viewer would show as text (the same sniff); a line longer
 * than the buffer is cut. */
#include <string.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "find.h"
#include "ops.h"
#include "fe.h"
#include "../core/core.h"
#include "../core/util.h"

#define MAXDEPTH 20
#define TBUF 8192

struct fw {
    const char *what;
    int wl, mode, wild, stop;
    unsigned long found;
    find_fn fn;
    void *ctx;
    unsigned long *seen;
    UBYTE *buf;
    char out[CPATH];            /* (here, not on the stack: the walk recurses) */
};

/* s holds sub, without case (Latin-1 letters too) */
static int holds(const char *s, const char *sub, int sl)
{
    int i;
    for (; *s; s++) {
        for (i = 0; i < sl; i++) {
            int a = (unsigned char)s[i], b = (unsigned char)sub[i];
            if (!a) return 0;
            if ((a >= 'A' && a <= 'Z') || (a >= 0xC0 && a <= 0xDE && a != 0xD7)) a += 32;
            if ((b >= 'A' && b <= 'Z') || (b >= 0xC0 && b <= 0xDE && b != 0xD7)) b += 32;
            if (a != b) break;
        }
        if (i == sl) return 1;
    }
    return 0;
}

static void progress(struct fw *w)
{
    char mb[80];
    sfmt(mb, sizeof(mb), "%s - %lu looked at, %lu found - Esc stops", w->mode == FIND_NAMES ? "finding" : "searching",
         *w->seen, w->found);
    showmsg(mb);
}

/* one line of a file: a hit? (0 = stop) */
static int line(struct fw *w, struct findhit *h, char *s, unsigned long n)
{
    if (!holds(s, w->what, w->wl)) return 1;
    while (*s == ' ' || *s == '\t') s++;
    h->line = n;
    h->text = s;
    w->found++;
    if (!w->fn(w->ctx, h)) w->stop = 1;
    return 0;
}

static void grepfile(struct fw *w, struct findhit *h)
{
    BPTR fh;
    LONG n, i, start, carry = 0;
    unsigned long ln = 1;
    int first = 1, go = 1;
    UBYTE *buf = w->buf;
    if (!(fh = Open((STRPTR)h->full, MODE_OLDFILE))) return;
    while (go && ((n = Read(fh, buf + carry, TBUF - carry)) > 0 || carry)) {
        LONG len = carry + (n > 0 ? n : 0);
        if (first) {
            int ty = sniffmem(buf, len);
            first = 0;
            if (ty != TY_TEXT && ty != TY_ANSI) break;
        }
        start = 0;
        for (i = 0; i < len && go; i++) {
            if (buf[i] != '\n') continue;
            buf[i] = 0;
            /* FIND_FILES: the first hit is the file's; the rest is skipped */
            if (!line(w, h, (char *)buf + start, ln)) go = w->mode == FIND_LINES && !w->stop;
            ln++;
            start = i + 1;
        }
        if (!go) break;
        if (n <= 0) {
            /* the last line without a newline */
            if (start < len) {
                buf[len] = 0;
                line(w, h, (char *)buf + start, ln);
            }
            break;
        }
        carry = len - start;
        if (carry >= TBUF - 1) carry = 0;          /* a line longer than the buffer: cut */
        else if (carry) memmove(buf, buf + start, carry);
        if (checkabort()) break;
    }
    Close(fh);
}

static void walk(struct fw *w, const char *dir, const char *rel, int depth)
{
    struct dscan *d;
    struct dent e;
    char *full, *r;
    struct findhit h;
    if (depth > MAXDEPTH || abortop || w->stop || !(d = ds_open(dir))) return;
    full = xalloc(CPATH);
    r = xalloc(CPATH);
    while (full && r && !w->stop && ds_next(d, &e) > 0 && !checkabort()) {
        if (!buildfull(full, dir, e.name)) continue;
        if (*rel) sfmt(r, CPATH, "%s/%s", rel, e.name);
        else sfmt(r, CPATH, "%s", e.name);
        h.full = full, h.rel = r, h.e = &e, h.line = 0, h.text = NULL;
        if (w->mode == FIND_NAMES) {
            if ((++*w->seen & 63) == 0) progress(w);
            if (w->wild ? os_patmatch(e.name) : holds(e.name, w->what, w->wl)) {
                w->found++;
                if (!w->fn(w->ctx, &h)) w->stop = 1;
            }
        } else if (e.type < 0 && e.size > 0) {
            if ((++*w->seen & 7) == 0) progress(w);
            grepfile(w, &h);
        }
        if (e.type == ST_USERDIR) walk(w, full, r, depth + 1);
    }
    xfree(full);
    xfree(r);
    ds_close(d);
}

int fs_find(const char *root, const char *what, int mode, find_fn fn, void *ctx, unsigned long *seen)
{
    struct fw *w;
    int r;
    *seen = 0;
    if (!(w = xalloc(sizeof(*w)))) return -2;
    memset(w, 0, sizeof(*w));
    w->what = what;
    w->wl = strlen(what);
    w->mode = mode;
    w->fn = fn;
    w->ctx = ctx;
    w->seen = seen;
    w->wild = mode == FIND_NAMES && (haswild(what) || strpbrk(what, "|[~"));
    if (w->wild && !os_patprep(what)) {
        xfree(w);
        return -1;
    }
    if (mode != FIND_NAMES && !(w->buf = xalloc(TBUF + 1))) {
        xfree(w);
        return -2;
    }
    cancelok = 1;
    abortop = 0;
    progress(w);
    walk(w, root, "", 0);
    cancelok = 0;
    r = !abortop && !w->stop;
    abortop = 0;
    if (w->wild) os_patfree();
    xfree(w->buf);
    xfree(w);
    return r;
}
