/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zpkg's resolver (DESIGN D22 4.3): from the stored list and the installed packages, what -S, -Syu, -Syuu, -R and
 * -Rs change, in order, before anything is fetched. A dependency is a name and a minimum version, nothing else. The
 * base group (the kernel, zup, zpkg, base) is never installed or removed here: a dependency on it is met by the
 * effective base (8.4.2) or waits for the system update (4.4 step 3). Loops and missing dependencies are refused;
 * downgrades and holds are checked in reverse (every package left must still have what it needs).
 */
#include <string.h>
#include <stdlib.h>
#include "zpkg.h"

struct rs {
    const struct zp_world *w;
    struct zp_plan *pl;
    unsigned char *mark;            /* per list block: 0 new, 1 being visited, 2 done */
    int *stepof;                    /* per list block: its step's index, -1 none */
    const char **why;
    unsigned flags;
};

static const struct zp_inst *inst_find(const struct zp_world *w, const char *name)
{
    int i;
    for (i = 0; i < w->ninst; i++)
        if (!strcmp(w->inst[i].name, name))
            return &w->inst[i];
    return 0;
}

static int held(const struct zp_world *w, const char *name)
{
    int i;
    for (i = 0; i < w->nhold; i++)
        if (!strcmp(w->hold[i], name))
            return 1;
    return 0;
}

static int is_base(const struct zp_world *w, const char *name)
{
    const struct zp_inst *in = inst_find(w, name);
    const struct zp_pkg *p = zp_index_find(w->ix, name);
    return (in && in->base) || (p && p->base);
}

static int fail(struct rs *r, int err, const char *why, const char *a, const char *b)
{
    *r->why = why;
    r->pl->bad = a;
    r->pl->bad2 = b;
    return err;
}

static int add_step(struct rs *r, int kind, const struct zp_pkg *p, const struct zp_inst *from, const char *why)
{
    struct zp_step *s = &r->pl->step[r->pl->n];
    s->kind = kind;
    s->pkg = p;
    s->from = from;
    s->asdep = 0;
    s->why = why;
    if (p)
        r->stepof[p - r->w->ix->pkg] = r->pl->n;
    return r->pl->n++;
}

/* the version a package will have after the plan (0: not installed), and whether it is changed by the plan */
static zp_u32 final_version(struct rs *r, const char *name)
{
    const struct zp_pkg *p = zp_index_find(r->w->ix, name);
    const struct zp_inst *in = inst_find(r->w, name);
    if (p && r->stepof[p - r->w->ix->pkg] >= 0) {
        const struct zp_step *s = &r->pl->step[r->stepof[p - r->w->ix->pkg]];
        if (s->kind == ZP_STEP_INSTALL || s->kind == ZP_STEP_UPGRADE || s->kind == ZP_STEP_DOWNGRADE)
            return p->vnum;
        if (s->kind == ZP_STEP_REMOVE)
            return 0;
    }
    return in ? in->vnum : 0;
}

/* a dependency on the base group: 0 met, 1 waits for the list's system, <0 refused */
static int base_dep(struct rs *r, const struct zp_pkg *p, const struct zp_dep *d)
{
    const struct zp_inst *in = inst_find(r->w, d->name);
    zp_u32 have = in ? in->vnum : 0;
    if (have > r->w->effbase)
        have = r->w->effbase;
    if (in && have >= d->vnum)
        return 0;
    if (r->w->ix->sys.vnum >= d->vnum && !held(r->w, "base"))
        return 1;
    return fail(r, ZP_ENOENT, held(r->w, "base") ? "needs a newer base, and base is held"
                                                 : "needs a newer base than the list's system", p->name, d->name);
}

/* visit one list block: its dependencies first, then its own step. want: the step kind asked for (INSTALL,
 * UPGRADE, DOWNGRADE); asdep: brought in as a dependency. Returns 0, 1 (it waits or is held: the caller's package
 * then waits or is held too), or <0 */
static int visit(struct rs *r, const struct zp_pkg *p, int asdep, const char **waitwhy)
{
    const struct zp_index *ix = r->w->ix;
    int k = (int)(p - ix->pkg), i, st, kind, waits = 0, isheld = 0;
    const struct zp_inst *in = inst_find(r->w, p->name);
    const char *ww = 0;

    if (r->mark[k] == 2) {
        st = r->stepof[k];
        if (st >= 0 && (r->pl->step[st].kind == ZP_STEP_WAIT || r->pl->step[st].kind == ZP_STEP_HELD)) {
            *waitwhy = r->pl->step[st].why;
            return 1;
        }
        return 0;
    }
    if (r->mark[k] == 1)
        return fail(r, ZP_ELOOP, "the dependencies go round in a loop", p->name, 0);
    if (p->base)
        return fail(r, ZP_EPERM, "is part of the system: it comes only with the system update (zpkg -Syu)", p->name, 0);
    r->mark[k] = 1;
    for (i = 0; i < p->ndep; i++) {
        const struct zp_dep *d = zp_pkg_dep(ix, p, i);
        const struct zp_pkg *dp;
        const struct zp_inst *din;
        const char *dwhy = 0;
        if (!strcmp(d->name, p->name))
            return fail(r, ZP_ELOOP, "depends on itself", p->name, 0);
        if (is_base(r->w, d->name)) {
            st = base_dep(r, p, d);
            if (st < 0)
                return st;
            if (st) {
                waits = 1;
                ww = "waits for the system update (zpkg -Syu after the restart)";
            }
            continue;
        }
        din = inst_find(r->w, d->name);
        dp = zp_index_find(ix, d->name);
        if (din && din->vnum >= d->vnum && !(dp && r->stepof[dp - ix->pkg] >= 0 && final_version(r, d->name) < d->vnum)) {
            if (dp && r->mark[dp - ix->pkg] == 1)
                return fail(r, ZP_ELOOP, "the dependencies go round in a loop", p->name, d->name);
            continue;                   /* installed and new enough */
        }
        if (!dp)
            return fail(r, ZP_ENOENT, "needs a package the list does not have", p->name, d->name);
        if (dp->vnum < d->vnum)
            return fail(r, ZP_ENOENT, "needs a newer version than the list has", p->name, d->name);
        if (din && held(r->w, d->name)) {
            isheld = 1;
            ww = "needs a newer version of a held package";
            continue;
        }
        st = visit(r, dp, 1, &dwhy);
        if (st < 0)
            return st;
        if (st) {
            const struct zp_step *ds = &r->pl->step[r->stepof[dp - ix->pkg]];
            if (ds->kind == ZP_STEP_HELD)
                isheld = 1;
            else
                waits = 1;
            ww = dwhy;
        }
    }
    r->mark[k] = 2;
    if (!in)
        kind = ZP_STEP_INSTALL;
    else if (p->vnum > in->vnum)
        kind = ZP_STEP_UPGRADE;
    else if (p->vnum < in->vnum && (r->flags & ZP_SYNC_UU))
        kind = ZP_STEP_DOWNGRADE;
    else
        return 0;                       /* up to date (or newer installed, kept) */
    if (in && held(r->w, p->name)) {
        isheld = 1;
        ww = "is held (/etc/zpkg.conf)";
    }
    if (isheld || waits) {
        st = add_step(r, isheld ? ZP_STEP_HELD : ZP_STEP_WAIT, p, in, ww);
        *waitwhy = ww;
        return 1;
    }
    st = add_step(r, kind, p, in, 0);
    r->pl->step[st].asdep = (unsigned char)(asdep && !in);
    return 0;
}

static int setup(struct rs *r, const struct zp_world *w, struct zp_plan *pl, const char **why, unsigned flags)
{
    static const char *dummy;
    int i, cap = w->ix->npkg + w->ninst + 1;
    memset(pl, 0, sizeof *pl);
    r->w = w;
    r->pl = pl;
    r->why = why ? why : &dummy;
    r->flags = flags;
    pl->step = malloc(sizeof *pl->step * (size_t)cap);
    r->mark = malloc((size_t)(w->ix->npkg + 1));
    r->stepof = malloc(sizeof *r->stepof * (size_t)(w->ix->npkg + 1));
    if (!pl->step || !r->mark || !r->stepof) {
        free(r->mark);
        free(r->stepof);
        zp_plan_free(pl);
        *r->why = "out of memory";
        return ZP_ENOMEM;
    }
    memset(r->mark, 0, (size_t)(w->ix->npkg + 1));
    for (i = 0; i <= w->ix->npkg; i++)
        r->stepof[i] = -1;
    return 0;
}

static void done(struct rs *r)
{
    free(r->mark);
    free(r->stepof);
}

/* after the plan: every package that will be there has what it needs, by the versions the plan leaves (downgrades
 * and holds in reverse, and a package visited before a later step took its dependency's version away) */
static int deps_met(struct rs *r, const char *name, const struct zp_dep *dep, int ndep)
{
    int j;
    for (j = 0; j < ndep; j++) {
        if (is_base(r->w, dep[j].name))
            continue;                   /* the base only moves through the system update (checked by visit) */
        if (final_version(r, dep[j].name) < dep[j].vnum)
            return fail(r, ZP_EPERM, "a package would lack the version it needs", name, dep[j].name);
    }
    return 0;
}

static int recheck(struct rs *r)
{
    const struct zp_world *w = r->w;
    int i, rc;
    for (i = 0; i < r->pl->n; i++) {
        const struct zp_step *s = &r->pl->step[i];
        if (s->kind == ZP_STEP_INSTALL || s->kind == ZP_STEP_UPGRADE || s->kind == ZP_STEP_DOWNGRADE)
            if ((rc = deps_met(r, s->pkg->name, &w->ix->dep[s->pkg->dep0], s->pkg->ndep)) < 0)
                return rc;
    }
    for (i = 0; i < w->ninst; i++) {
        const struct zp_inst *in = &w->inst[i];
        const struct zp_pkg *p = zp_index_find(w->ix, in->name);
        int st = p ? r->stepof[p - w->ix->pkg] : -1;
        if (st >= 0 && r->pl->step[st].kind != ZP_STEP_WAIT && r->pl->step[st].kind != ZP_STEP_HELD)
            continue;                   /* replaced: its new version's needs were checked above */
        if ((rc = deps_met(r, in->name, in->dep, in->ndep)) < 0)
            return rc;
    }
    return 0;
}

int zp_plan_sync(const struct zp_world *w, const char *const *names, int nnames, unsigned flags, struct zp_plan *pl,
                 const char **why)
{
    struct rs r;
    int i, rc;
    const char *ww;
    if ((rc = setup(&r, w, pl, why, flags)) < 0)
        return rc;
    for (i = 0; i < nnames; i++) {
        const struct zp_pkg *p = zp_index_find(w->ix, names[i]);
        if (!p) {
            rc = fail(&r, ZP_ENOENT, "is not in the package list", names[i], 0);
            goto out;
        }
        if (p->base) {
            rc = fail(&r, ZP_EPERM, "is part of the system: it comes only with the system update (zpkg -Syu)",
                      names[i], 0);
            goto out;
        }
        if ((rc = visit(&r, p, 0, &ww)) < 0)
            goto out;
    }
    if (flags & (ZP_SYNC_U | ZP_SYNC_UU)) {
        const struct zp_inst *base = inst_find(w, "base");
        for (i = 0; i < w->ninst; i++) {
            const struct zp_inst *in = &w->inst[i];
            const struct zp_pkg *p = zp_index_find(w->ix, in->name);
            if (in->base || !p || p->base)
                continue;               /* the system, and packages the list no longer has, stay as they are */
            if (p->vnum == in->vnum || (p->vnum < in->vnum && !(flags & ZP_SYNC_UU)))
                continue;
            if ((rc = visit(&r, p, 0, &ww)) < 0)
                goto out;
        }
        pl->system = (!base || w->ix->sys.vnum > base->vnum) && !held(w, "base");
    }
    rc = recheck(&r);
out:
    done(&r);
    if (rc < 0) {
        const char *a = pl->bad, *b = pl->bad2;
        zp_plan_free(pl);
        pl->bad = a;
        pl->bad2 = b;
    }
    return rc < 0 ? rc : 0;
}

/* does any package that stays (installed, not in the removal set) depend on name? -> that package, or 0 */
static const struct zp_inst *needed_by(const struct zp_world *w, const unsigned char *gone, const char *name)
{
    int i, j;
    for (i = 0; i < w->ninst; i++) {
        if (gone[i])
            continue;
        for (j = 0; j < w->inst[i].ndep; j++)
            if (!strcmp(w->inst[i].dep[j].name, name))
                return &w->inst[i];
    }
    return 0;
}

int zp_plan_remove(const struct zp_world *w, const char *const *names, int nnames, unsigned flags,
                   struct zp_plan *pl, const char **why)
{
    struct rs r;
    unsigned char *gone;
    int i, j, rc = 0, more, left;
    if ((rc = setup(&r, w, pl, why, flags)) < 0)
        return rc;
    gone = malloc((size_t)w->ninst + 1);
    if (!gone) {
        done(&r);
        zp_plan_free(pl);
        if (why)
            *why = "out of memory";
        return ZP_ENOMEM;
    }
    memset(gone, 0, (size_t)w->ninst + 1);
    for (i = 0; i < nnames; i++) {
        const struct zp_inst *in = inst_find(w, names[i]);
        if (!in) {
            rc = fail(&r, ZP_ENOENT, "is not installed", names[i], 0);
            goto out;
        }
        if (in->base) {
            rc = fail(&r, ZP_EPERM, "is part of the system and is never removed", names[i], 0);
            goto out;
        }
        for (j = 0; j < w->ninst; j++) {
            int k;
            if (!w->inst[j].base)
                continue;
            for (k = 0; k < w->inst[j].ndep; k++)
                if (!strcmp(w->inst[j].dep[k].name, names[i])) {
                    rc = fail(&r, ZP_EPERM, "is needed by the system", names[i], w->inst[j].name);
                    goto out;
                }
        }
        gone[in - w->inst] = 1;
    }
    /* -Rs: dependencies installed only as such that nothing staying needs, until none is added */
    if (flags & ZP_RM_S) {
        do {
            more = 0;
            for (i = 0; i < w->ninst; i++) {
                if (!gone[i])
                    continue;
                for (j = 0; j < w->inst[i].ndep; j++) {
                    const struct zp_inst *d = inst_find(w, w->inst[i].dep[j].name);
                    if (!d || d->base || !d->asdep || gone[d - w->inst])
                        continue;
                    gone[d - w->inst] = 1;      /* tentatively; kept only if nothing staying needs it */
                    if (needed_by(w, gone, d->name))
                        gone[d - w->inst] = 0;
                    else
                        more = 1;
                }
            }
        } while (more);
        /* a dependency let go above may be needed by a later-kept one only through a package that stays: settled by
         * the loop (a kept package never depends on a gone one, checked below) */
    }
    for (i = 0; i < w->ninst; i++) {
        const struct zp_inst *by;
        if (!gone[i])
            continue;
        if ((by = needed_by(w, gone, w->inst[i].name)) != 0) {
            rc = fail(&r, ZP_EPERM, "is needed by an installed package", w->inst[i].name, by->name);
            goto out;
        }
    }
    /* the order: a package before what it depends on (dependents first) */
    do {
        left = 0;
        more = 0;
        for (i = 0; i < w->ninst; i++) {
            int needed = 0, k;
            if (gone[i] != 1)
                continue;
            left = 1;
            for (k = 0; k < w->ninst && !needed; k++)
                if (gone[k] == 1 && k != i)
                    for (j = 0; j < w->inst[k].ndep; j++)
                        if (!strcmp(w->inst[k].dep[j].name, w->inst[i].name))
                            needed = 1;
            if (!needed) {
                struct zp_step *s = &pl->step[pl->n++];
                memset(s, 0, sizeof *s);
                s->kind = ZP_STEP_REMOVE;
                s->from = &w->inst[i];
                gone[i] = 2;
                more = 1;
            }
        }
        if (left && !more)              /* they depend on each other in a loop: all go, in list order */
            for (i = 0; i < w->ninst; i++)
                if (gone[i] == 1) {
                    struct zp_step *s = &pl->step[pl->n++];
                    memset(s, 0, sizeof *s);
                    s->kind = ZP_STEP_REMOVE;
                    s->from = &w->inst[i];
                    gone[i] = 2;
                    break;
                }
    } while (left);
out:
    free(gone);
    done(&r);
    if (rc < 0) {
        const char *a = pl->bad, *b = pl->bad2;
        zp_plan_free(pl);
        pl->bad = a;
        pl->bad2 = b;
    }
    return rc < 0 ? rc : 0;
}

void zp_plan_free(struct zp_plan *pl)
{
    free(pl->step);
    memset(pl, 0, sizeof *pl);
}
