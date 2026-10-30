/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* renames - many renames at once: by a pattern (fs_patrename), or from a
 * list of new names (fs_renameset, also Ahiru's bulk rename in the
 * editor). Every new name is checked before anything moves: a real name,
 * not twice, not one already there that is not itself going away. Then
 * two steps, so that a and b can swap: each to a name of its own first,
 * then to its new one. Icons go along. Folders on disks. */
#include <string.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "renames.h"
#include "ops.h"
#include "os.h"
#include "fe.h"
#include "../core/core.h"
#include "../core/util.h"
#include "../core/rename.h"

/* the set being renamed, with indexes so every check is a halving, not
 * a walk over the whole folder (2000 files on a 7 MHz 68000) */
struct rset {
    struct place *p;
    const int *idx;
    char (*newn)[NAMEMAX + 4];
    int n;
    int *ix;                    /* the folder by name (place_index) */
    int *where;                 /* entry k -> its place in the set, -1 none */
};

static int renamed(struct rset *r, int j)
{
    return strcmp(r->newn[j], r->p->ent[r->idx[j]].name) != 0;
}

/* the entry whose icon name is (ends in .info): -1 none */
static int baseof(struct rset *r, const char *name)
{
    char base[NAMEMAX + 4];
    int l = strlen(name);
    if (!icons || !isinfo(name) || l - 5 > NAMEMAX) return -1;
    memcpy(base, name, l - 5);
    base[l - 5] = 0;
    return place_findix(r->p, r->ix, base);
}

/* entry k of p leaves its name in this set: renamed itself, or the icon
 * of one that is */
static int goesaway(struct rset *r, int k)
{
    int j = r->where[k], kb;
    if (j >= 0 && nocase(r->newn[j], r->p->ent[k].name) != 0) return 1;
    kb = baseof(r, r->p->ent[k].name);
    return kb >= 0 && r->where[kb] >= 0 && renamed(r, r->where[kb]);
}

/* is set member i an icon whose file is renamed in the same set? (it goes along) */
static int follows(struct rset *r, int i)
{
    int kb = baseof(r, r->p->ent[r->idx[i]].name), j;
    if (kb < 0) return 0;
    j = r->where[kb];
    return j >= 0 && j != i && renamed(r, j);
}

static const char *newname(void *ctx, int i)
{
    return ((struct rset *)ctx)->newn[i];
}

int fs_renameset(struct place *p, const int *idx, char (*newn)[NAMEMAX + 4], int n)
{
    int i, ok = 1, done = 0, step, *nx = NULL, nn = 0;
    char (*tmpn)[24];
    char **old, *st, keep[sizeof(opmsg)];
    struct rset r;
    if (pl_involume(p) || pl_inarc(p) || p->fail) {
        showmsg("renaming many at once works in a folder on a disk");
        return 0;
    }
    r.p = p, r.idx = idx, r.newn = newn, r.n = n;
    st = xalloc(n);
    nx = xalloc((n ? n : 1) * sizeof(int));
    r.ix = place_index(p);
    r.where = xalloc((p->n ? p->n : 1) * sizeof(int));
    if (!st || !nx || !r.ix || !r.where) {
        xfree(st);
        xfree(nx);
        xfree(r.ix);
        xfree(r.where);
        showmsg("not enough memory");
        return 0;
    }
    for (i = 0; i < p->n; i++) r.where[i] = -1;
    for (i = 0; i < n; i++) r.where[idx[i]] = i;
    /* st: 0 stays, 1 is renamed; 3 an icon that goes along with its file */
    for (i = 0; i < n; i++) st[i] = renamed(&r, i);
    for (i = 0; i < n; i++)
        if (st[i] && follows(&r, i)) st[i] = 3;
    /* a new name twice: side by side once sorted */
    for (i = 0; i < n; i++)
        if (st[i] != 3) nx[nn++] = i;
    name_sort(nx, nn, newname, &r);
    for (i = 1; i < nn && ok; i++)
        if (nocase(newn[nx[i]], newn[nx[i - 1]]) == 0) {
            sfmt(opmsg, sizeof(opmsg), "\"%.60s\" twice - nothing was renamed", newn[nx[i]]);
            ok = 0;
        }
    for (i = 0; i < n && ok; i++) {
        int l = strlen(newn[i]);
        if (st[i] != 1) continue;
        if (!l || strchr(newn[i], '/') || strchr(newn[i], ':') || l > NAMEMAX) {
            sfmt(opmsg, sizeof(opmsg), "\"%.60s\" is not a name - nothing was renamed", newn[i]);
            ok = 0;
        }
        if (ok && nocase(newn[i], p->ent[idx[i]].name) != 0) {
            int k = place_findix(p, r.ix, newn[i]);
            if (k >= 0 && !goesaway(&r, k)) {
                sfmt(opmsg, sizeof(opmsg), "\"%.60s\" is already there - nothing was renamed", newn[i]);
                ok = 0;
            }
        }
        /* the icon goes along: its new name must be free too */
        if (ok && icons && !isinfo(p->ent[idx[i]].name) && l + 5 <= NAMEMAX) {
            char in[NAMEMAX + 8];
            int k;
            sfmt(in, sizeof(in), "%s.info", p->ent[idx[i]].name);
            if (place_findix(p, r.ix, in) >= 0) {
                sfmt(in, sizeof(in), "%s.info", newn[i]);
                k = place_findix(p, r.ix, in);
                if (k >= 0 && !goesaway(&r, k)) {
                    sfmt(opmsg, sizeof(opmsg), "\"%.60s\" is already there - nothing was renamed", in);
                    ok = 0;
                }
            }
        }
    }
    xfree(nx);
    xfree(r.where);
    if (!ok) {
        xfree(r.ix);
        xfree(st);
        showmsg(opmsg);
        return 0;
    }
    tmpn = xalloc(n * sizeof(*tmpn));
    old = xalloc(n * sizeof(char *));
    if (!tmpn || !old) {
        xfree(tmpn);
        xfree(old);
        xfree(st);
        xfree(r.ix);
        showmsg("not enough memory");
        return 0;
    }
    for (i = 0; i < n; i++) {
        old[i] = p->ent[idx[i]].name;
        sfmt(tmpn[i], sizeof(tmpn[i]), "cfren-%lx-%ld", (ULONG)FindTask(NULL) & 0xFFFF, (long)i);
        if (st[i] == 1 && place_findix(p, r.ix, tmpn[i]) >= 0) {
            sfmt(opmsg, sizeof(opmsg), "\"%s\" is in the way - nothing was renamed", tmpn[i]);
            ok = 0;
        }
    }
    /* st during the run: 1 still the old name, 2 on its scratch name, 4 the new name */
    for (step = 0; step < 2 && ok; step++)
        for (i = 0; i < n && ok; i++) {
            struct ent *e = &p->ent[idx[i]];
            if (st[i] != (step ? 2 : 1)) continue;
            if (step == 0) {
                if (!fs_renameone(p, idx[i], tmpn[i])) ok = 0;
                else e->name = tmpn[i], st[i] = 2;
            } else {
                if (!fs_renameone(p, idx[i], newn[i])) ok = 0;
                else e->name = newn[i], st[i] = 4, done++;
            }
        }
    if (!ok) {
        /* undo: every new name back to its scratch name (those are free),
         * then every scratch name back to the old one (now all free) */
        int lost = 0;
        memcpy(keep, opmsg, sizeof(keep));
        for (i = 0; i < n; i++)
            if (st[i] == 4) {
                if (fs_renameone(p, idx[i], tmpn[i])) p->ent[idx[i]].name = tmpn[i], st[i] = 2;
                else lost++;
            }
        for (i = 0; i < n; i++)
            if (st[i] == 2) {
                if (fs_renameone(p, idx[i], old[i])) p->ent[idx[i]].name = old[i], st[i] = 1;
                else lost++;
            }
        if (lost) sfmt(opmsg, sizeof(opmsg), "%.100s - %d could not be put back (cfren-...)", keep, lost);
        else sfmt(opmsg, sizeof(opmsg), "%.120s - nothing was renamed", keep);
    }
    /* the entries' names point back where they were (the place is read
     * again now anyway) */
    for (i = 0; i < n; i++) p->ent[idx[i]].name = old[i];
    xfree(tmpn);
    xfree(old);
    xfree(st);
    xfree(r.ix);
    placechanged(p, NULL, 1);
    if (!ok) showmsg(opmsg);
    else {
        char mb[60];
        sfmt(mb, sizeof(mb), "%d renamed", done);
        showmsg(mb);
    }
    return ok;
}

int fs_patrename(struct place *p, int cur)
{
    static char from[60] = "*", to[60] = "*";
    char (*newn)[NAMEMAX + 4] = NULL;
    const char **lines = NULL;
    char *text = NULL;
    int *idx = NULL, n = 0, i, nm = 0, r = 0;
    long num = 1;
    (void)cur;
    if (pl_involume(p) || pl_inarc(p) || p->fail || !p->n) {
        showmsg("renaming by pattern works in a folder on a disk");
        return 0;
    }
    if (!lineinput("rename from (* any, ? one): ", from, sizeof(from) - 1, 0) || !from[0]) {
        askdone();
        return 0;
    }
    if (!lineinput("to (* ? as matched, # a number): ", to, sizeof(to) - 1, 0) || !to[0]) {
        askdone();
        return 0;
    }
    for (i = 0; i < p->n; i++) nm += p->ent[i].mark != 0;
    /* the marked ones (else every one) that the pattern matches, numbered
     * in the order they are shown; counted first, so the memory is for
     * the matches only (a 512K machine, a folder of 2000) */
    {
        char t[NAMEMAX + 4];
        long tl = 0;
        int want = 0;
        for (i = 0; i < p->n; i++) {
            if (nm && !p->ent[i].mark) continue;
            if (isinfo(p->ent[i].name) && icons) continue;      /* icons follow their files */
            if (!ren_make(from, to, p->ent[i].name, num + want, t, NAMEMAX + 1)) continue;
            want++;
            tl += strlen(p->ent[i].name) + strlen(t) + 7;
        }
        if (!want) {
            showmsg("the pattern matches nothing here");
            goto out;
        }
        idx = xalloc(want * sizeof(int));
        newn = xalloc(want * sizeof(*newn));
        lines = xalloc(want * sizeof(char *));
        text = xalloc(tl + 1);
        if (!idx || !newn || !lines || !text) {
            showmsg("not enough memory");
            goto out;
        }
        tl = 0;
        for (i = 0; i < p->n && n < want; i++) {
            int l;
            if (nm && !p->ent[i].mark) continue;
            if (isinfo(p->ent[i].name) && icons) continue;
            if (!ren_make(from, to, p->ent[i].name, num, newn[n], NAMEMAX + 1)) continue;
            num++;
            idx[n] = i;
            l = strlen(p->ent[i].name) + strlen(newn[n]) + 7;
            sfmt(text + tl, l, "%s  ->  %s", p->ent[i].name, newn[n]);
            lines[n] = text + tl;
            tl += l;
            n++;
        }
    }
    if (!n) {
        showmsg("the pattern matches nothing here");
        goto out;
    }
    {
        char q[80];
        sfmt(q, sizeof(q), "%d to rename - Return renames them, Esc leaves them", n);
        if (!asklist(q, lines, n)) {
            showmsg("nothing was renamed");
            goto out;
        }
    }
    r = fs_renameset(p, idx, newn, n);
out:
    xfree(idx);
    xfree(newn);
    xfree(lines);
    xfree(text);
    return r;
}
