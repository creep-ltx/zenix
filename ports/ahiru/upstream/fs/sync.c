/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sync - two folders compared, and one made like the other.
 *
 * fs_compare marks, in each of two places, what the other lacks or holds
 * older or different: a file by its size and date, a folder by whether
 * anything under it differs (walked). fs_sync makes the target like the
 * source: it plans first (every copy, every new folder and, when asked,
 * every file the target has that the source does not), says how much,
 * asks, and only then works - with the core's own copy and its rules (a
 * file is written beside the old one and swapped in when whole). Folders
 * on disks only; archives have their own ways. */
#include <string.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "sync.h"
#include "ops.h"
#include "os.h"
#include "fe.h"
#include "../core/core.h"
#include "../core/util.h"

#define MAXDEPTH 16

/* two dates the same within a minute and a half: FAT and some
 * filesystems keep two seconds, a copy over the network rounds */
static int samedate(LONG a, LONG b)
{
    return a - b <= 1 && b - a <= 1;
}

/* ---- names found by halving (core.h's name_sort, name_find) ------------- */

/* ---- a folder's entries, read whole (an index by name for the walk) ----- */

struct dl {
    struct dent *e;
    int *ix;
    int n, cap;
};

static const char *dlname(void *ctx, int i)
{
    return ((struct dl *)ctx)->e[i].name;
}

static void dlfree(struct dl *d)
{
    xfree(d->e);
    xfree(d->ix);
    d->e = NULL;
    d->ix = NULL;
    d->n = d->cap = 0;
}

static int dlread(const char *path, struct dl *d)
{
    struct dscan *s;
    struct dent e;
    int r;
    d->e = NULL;
    d->ix = NULL;
    d->n = d->cap = 0;
    if (!(s = ds_open(path))) return 0;
    while ((r = ds_next(s, &e)) > 0) {
        if (e.type == ST_SOFTLINK || e.type == ST_LINKDIR) continue;     /* links are not followed */
        if (d->n == d->cap) {
            int nc = d->cap ? d->cap * 2 : 64;
            struct dent *ne = xalloc(nc * sizeof(struct dent));
            if (!ne) {
                ds_close(s);
                dlfree(d);
                return 0;
            }
            if (d->n) memcpy(ne, d->e, d->n * sizeof(struct dent));
            xfree(d->e);
            d->e = ne;
            d->cap = nc;
        }
        d->e[d->n++] = e;
    }
    ds_close(s);
    if (!r && !(d->ix = xalloc((d->n ? d->n : 1) * sizeof(int)))) r = -1;
    if (r) {
        dlfree(d);
        return 0;
    }
    for (r = 0; r < d->n; r++) d->ix[r] = r;
    name_sort(d->ix, d->n, dlname, d);
    return 1;
}

static struct dent *dlfind(struct dl *d, const char *name)
{
    int k = name_find(d->ix, d->n, dlname, d, name);
    return k < 0 ? NULL : &d->e[k];
}

/* ---- do two trees differ? -------------------------------------------------- */

static int treediffers(const char *a, const char *b, int depth)
{
    struct dl da, db;
    char *ca, *cb;
    int i, diff = 0;
    if (checkabort()) return 0;
    if (depth > MAXDEPTH) return 1;     /* too deep to look: say it differs, never "the same" */
    if (!dlread(a, &da)) return 1;
    if (!dlread(b, &db)) {
        dlfree(&da);
        return 1;
    }
    ca = xalloc(CPATH);
    cb = xalloc(CPATH);
    if (!ca || !cb) diff = 1;
    if (da.n != db.n) diff = 1;
    for (i = 0; i < da.n && !diff; i++) {
        struct dent *x = &da.e[i], *y = dlfind(&db, x->name);
        if (!y || (x->type > 0) != (y->type > 0)) diff = 1;
        else if (x->type < 0) diff = x->size != y->size || !samedate(x->days * 1440 + x->mins, y->days * 1440 + y->mins);
        else if (buildfull(ca, a, x->name) && buildfull(cb, b, y->name)) diff = treediffers(ca, cb, depth + 1);
    }
    xfree(ca);
    xfree(cb);
    dlfree(&da);
    dlfree(&db);
    return diff;
}

int fs_compare(struct place *a, struct place *b, int *na, int *nb)
{
    int i, j, *ia, *ib;
    char pa[CPATH], pb[CPATH];
    *na = *nb = 0;
    if (pl_involume(a) || pl_involume(b) || pl_inarc(a) || pl_inarc(b) || a->fail || b->fail) {
        showmsg("compare works between two folders on disks");
        return 0;
    }
    ia = place_index(a);
    ib = place_index(b);
    if (!ia || !ib) {
        xfree(ia);
        xfree(ib);
        showmsg("not enough memory");
        return 0;
    }
    cancelok = 1;
    abortop = 0;
    showmsg("comparing - Esc stops");
    for (i = 0; i < a->n; i++) a->ent[i].mark = 0;
    for (j = 0; j < b->n; j++) b->ent[j].mark = 0;
    for (i = 0; i < a->n && !abortop; i++) {
        struct ent *x = &a->ent[i];
        int k = place_findix(b, ib, x->name);
        struct ent *y = k >= 0 ? &b->ent[k] : NULL;
        if (x->link || (y && y->link)) continue;    /* links are not followed (as sync) */
        if (!y || (x->kind == KD_FILE) != (y->kind == KD_FILE)) {
            x->mark = 1;
            continue;
        }
        if (x->kind == KD_FILE) {
            if (samedate(x->date, y->date)) {
                if (x->size != y->size) x->mark = y->mark = 1;
            } else if (x->date > y->date) {
                x->mark = 1;
            } else {
                y->mark = 1;
            }
        } else if (buildfull(pa, a->path, x->name) && buildfull(pb, b->path, y->name) &&
                   treediffers(pa, pb, 0)) {
            x->mark = y->mark = 1;          /* something below differs */
        }
    }
    for (j = 0; j < b->n && !abortop; j++)
        if (!b->ent[j].link && place_findix(a, ia, b->ent[j].name) < 0) b->ent[j].mark = 1;
    cancelok = 0;
    xfree(ia);
    xfree(ib);
    for (i = 0; i < a->n; i++) *na += a->ent[i].mark;
    for (j = 0; j < b->n; j++) *nb += b->ent[j].mark;
    if (abortop) {
        abortop = 0;
        showmsg("compare stopped");
        return 0;
    }
    return 1;
}

/* ---- the plan, then the work --------------------------------------------- */

#define OP_COPY 0               /* a file, new or newer */
#define OP_TREE 1               /* a folder the target lacks, whole */
#define OP_DEL 2                /* (mirror) the target's, not in the source */
#define OP_DELTREE 3

struct op {
    char *src, *dst;
    UBYTE kind;
    ULONG size;
};

static struct op *ops;
static int nops, capops;
static struct pool *opool;
static ULONG plancopy, plandel, planbytes, plannewer;

static int addop(int kind, const char *src, const char *dst, ULONG size)
{
    if (nops == capops) {
        int nc = capops ? capops * 2 : 128;
        struct op *no = xalloc(nc * sizeof(struct op));
        if (!no) return 0;
        if (nops) memcpy(no, ops, nops * sizeof(struct op));
        xfree(ops);
        ops = no;
        capops = nc;
    }
    ops[nops].src = src ? pool_str(&opool, src) : NULL;
    ops[nops].dst = pool_str(&opool, dst);
    if ((src && !ops[nops].src) || !ops[nops].dst) return 0;
    ops[nops].kind = (UBYTE)kind;
    ops[nops].size = size;
    nops++;
    if (kind == OP_DEL || kind == OP_DELTREE) plandel++;
    else plancopy++, planbytes += size;
    return 1;
}

static void planfree(void)
{
    xfree(ops);
    ops = NULL;
    nops = capops = 0;
    pool_free(&opool);
    plancopy = plandel = planbytes = plannewer = 0;
}

static int plan(const char *a, const char *b, int mirror, int depth)
{
    struct dl da, db;
    char *ca, *cb;
    int i, ok = 1;
    if (checkabort()) return 0;
    if (depth > MAXDEPTH) {
        sfmt(opmsg, sizeof(opmsg), "folders nested more than %d deep - nothing was changed", MAXDEPTH);
        return 0;
    }
    if (!dlread(a, &da)) return 0;
    if (!dlread(b, &db)) {
        dlfree(&da);
        return 0;
    }
    ca = xalloc(CPATH);
    cb = xalloc(CPATH);
    if (!ca || !cb) ok = 0;
    for (i = 0; i < da.n && ok; i++) {
        struct dent *x = &da.e[i], *y = dlfind(&db, x->name);
        if (!buildfull(ca, a, x->name) || !buildfull(cb, b, y ? y->name : x->name)) {
            ok = 0;
            break;
        }
        if (x->type > 0) {
            if (!y) {
                /* the whole folder; its size for the bar */
                statbytes = 0;
                treestat(ca, 0);
                ok = addop(OP_TREE, ca, cb, statbytes);
            } else if (y->type > 0) {
                ok = plan(ca, cb, mirror, depth + 1);
            } else {
                ok = 0;
                sfmt(opmsg, sizeof(opmsg), "\"%.40s\" is a folder here, a file there - sorted out by hand", x->name);
            }
        } else if (!y) {
            ok = addop(OP_COPY, ca, cb, x->size);
        } else if (y->type > 0) {
            ok = 0;
            sfmt(opmsg, sizeof(opmsg), "\"%.40s\" is a file here, a folder there - sorted out by hand", x->name);
        } else {
            LONG dx = x->days * 1440 + x->mins, dy = y->days * 1440 + y->mins;
            if (samedate(dx, dy) ? x->size != y->size : dx > dy) ok = addop(OP_COPY, ca, cb, x->size);
            else if (!samedate(dx, dy)) plannewer++;     /* newer there: never overwritten */
        }
    }
    for (i = 0; i < db.n && ok && mirror; i++) {
        struct dent *y = &db.e[i];
        if (dlfind(&da, y->name)) continue;
        if (!buildfull(cb, b, y->name)) {
            ok = 0;
            break;
        }
        ok = addop(y->type > 0 ? OP_DELTREE : OP_DEL, NULL, cb, 0);
    }
    xfree(ca);
    xfree(cb);
    dlfree(&da);
    dlfree(&db);
    return ok;
}

int fs_sync(struct place *a, struct place *b)
{
    char mb[200], s1[12];
    int i, ok = 1, done = 0, mirror = 0, k;
    if (pl_involume(a) || pl_involume(b) || pl_inarc(a) || pl_inarc(b) || a->fail || b->fail) {
        showmsg("sync works between two folders on disks");
        return 0;
    }
    if (insidedir(a->path, b->path) || insidedir(b->path, a->path)) {
        showmsg("one folder is inside the other - sync would chase itself");
        return 0;
    }
    k = askkey("make the other side like this one: (c)opy what is new or newer, (m)irror (also delete what this side lacks)?") | 32;
    if (k != 'c' && k != 'm') return 0;
    mirror = k == 'm';
    op_begin();
    planfree();
    opmsg[0] = 0;
    cancelok = 1;
    abortop = 0;
    showmsg("planning - Esc stops");
    if (!plan(a->path, b->path, mirror, 0) || abortop) {
        cancelok = 0;
        showmsg(abortop ? "stopped - nothing was changed" : opmsg[0] ? opmsg : "cannot read the folders");
        abortop = 0;
        planfree();
        return 0;
    }
    cancelok = 0;
    if (!plancopy && !plandel) {
        if (plannewer) sfmt(mb, sizeof(mb), "nothing to copy - %lu newer on the other side (left as they are)", plannewer);
        else sfmt(mb, sizeof(mb), "the two are the same already");
        planfree();
        showmsg(mb);
        return 1;
    }
    fmtsize(s1, planbytes);
    if (plandel && !plancopy)
        sfmt(mb, sizeof(mb), "delete %lu there that this side lacks - go? (y)es (n)o", plandel);
    else if (plandel)
        sfmt(mb, sizeof(mb), "copy %lu (%s), delete %lu there that this side lacks%s - go? (y)es (n)o",
             plancopy, s1, plandel, plannewer ? ", leave the newer ones there" : "");
    else
        sfmt(mb, sizeof(mb), "copy %lu (%s) to the other side%s - go? (y)es (n)o", plancopy, s1,
             plannewer ? " (newer ones there are left)" : "");
    if ((askkey(mb) | 32) != 'y') {
        planfree();
        return 0;
    }
    if (!cbuf_get()) {
        planfree();
        showmsg("not enough memory");
        return 0;
    }
    cancelok = 1;
    abortop = 0;
    progshow(planbytes + plandel);
    for (i = 0; i < nops && ok && !abortop; i++) {
        struct op *o = &ops[i];
        switch (o->kind) {
        case OP_COPY: ok = copyfile(o->src, o->dst); break;
        case OP_TREE: ok = copytree(o->src, o->dst, 0); break;
        case OP_DEL: ok = zap(o->dst, 1); progadd(1); break;
        case OP_DELTREE: ok = deltree(o->dst, 0, 0); progadd(1); break;
        }
        if (!ok && (o->kind == OP_DEL || o->kind == OP_DELTREE)) {
            char fb[60];
            os_fault(IoErr(), fb, sizeof(fb));
            sfmt(opmsg, sizeof(opmsg), "cannot delete %.60s: %s", filepart(o->dst), fb);
        }
        if (ok) done++;
    }
    cancelok = 0;
    progoff();
    cbuf_free();
    placechanged(NULL, NULL, 2);
    if (abortop) sfmt(mb, sizeof(mb), "stopped - %d of %d done", done, nops);
    else if (!ok) sfmt(mb, sizeof(mb), "%d of %d done - %s", done, nops, opmsg);
    else sfmt(mb, sizeof(mb), "synced: %d done", done);
    showmsg(mb);
    abortop = 0;
    planfree();
    return ok;
}
