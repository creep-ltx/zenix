/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* place - see place.h. Behaviour follows the E CFile 0.5b52. */
#include <string.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "place.h"
#include "fe.h"
#include "os.h"
#include "ops.h"
#include "arcfs.h"
#include "../core/core.h"
#include "../core/util.h"

/* ---- name storage: 4K blocks, freed together per read ---------------- */

struct pool {
    struct pool *next;
    int used, size;
    char data[1];
};

void pool_free(struct pool **pp)
{
    while (*pp) {
        struct pool *n = (*pp)->next;
        xfree(*pp);
        *pp = n;
    }
}

char *pool_str(struct pool **pp, const char *s)
{
    int l = strlen(s) + 1;
    struct pool *b = *pp;
    if (!b || b->used + l > b->size) {
        int sz = l > 4000 ? l : 4000;
        if (!(b = xalloc(sizeof(struct pool) + sz))) return NULL;
        b->size = sz;
        b->next = *pp;
        *pp = b;
    }
    memcpy(b->data + b->used, s, l);
    b->used += l;
    return b->data + b->used - l;
}

/* ---- entries ------------------------------------------------------------- */

int place_add(struct place *P, const char *name, int kind, ULONG size, LONG date)
{
    struct ent *e;
    if (P->n == P->cap) {
        int nc = P->cap ? P->cap * 2 : 256;
        struct ent *ne = xalloc(nc * sizeof(struct ent));
        if (!ne) return 0;
        if (P->n) memcpy(ne, P->ent, P->n * sizeof(struct ent));
        xfree(P->ent);
        P->ent = ne;
        P->cap = nc;
    }
    e = &P->ent[P->n];
    if (!(e->name = pool_str(&P->names, name))) return 0;
    e->size = size;
    e->date = date;
    e->kind = (UBYTE)kind;
    e->mark = 0;
    e->link = 0;
    e->prot = 0;
    P->n++;
    return 1;
}

int place_find(struct place *P, const char *name)
{
    int i;
    for (i = 0; i < P->n; i++)
        if (nocase(P->ent[i].name, name) == 0) return i;
    return -1;
}

static const char *plname(void *ctx, int i)
{
    return ((struct place *)ctx)->ent[i].name;
}

int *place_index(struct place *P)
{
    int *ix = xalloc((P->n ? P->n : 1) * sizeof(int)), i;
    if (!ix) return NULL;
    for (i = 0; i < P->n; i++) ix[i] = i;
    name_sort(ix, P->n, plname, P);
    return ix;
}

int place_findix(struct place *P, const int *ix, const char *name)
{
    return name_find(ix, P->n, plname, P, name);
}

int place_marks(struct place *P)
{
    int i, n = 0;
    for (i = 0; i < P->n; i++) n += P->ent[i].mark != 0;
    return n;
}

static int nameismarked(struct place *P, const char *nm)
{
    int i;
    for (i = 0; i < P->n; i++)
        if (P->ent[i].mark && nocase(P->ent[i].name, nm) == 0) return 1;
    return 0;
}

/* a marked icon whose own file is also marked travels as that file's
 * sidecar, not on its own */
static int markednodup(struct place *P, int idx)
{
    struct ent *e = &P->ent[idx];
    char base[NAMEMAX + 4];
    if (!e->mark) return 0;
    if (!icons || !isinfo(e->name)) return 1;
    sfmt(base, sizeof(base), "%s", e->name);
    base[strlen(base) - 5] = 0;
    return !nameismarked(P, base);
}

int place_pick(struct place *P, int cur, int **idx)
{
    int i, n = 0, nm = place_marks(P);
    *idx = NULL;
    if (!nm && (cur < 0 || cur >= P->n)) return 0;
    if (!(*idx = xalloc((nm ? nm : 1) * sizeof(int)))) {
        showmsg("not enough memory");
        return 0;
    }
    if (!nm) {
        (*idx)[0] = cur;
        return 1;
    }
    for (i = 0; i < P->n; i++)
        if (markednodup(P, i)) (*idx)[n++] = i;
    if (!n) xfree(*idx), *idx = NULL;
    return n;
}

/* ---- sorting ----------------------------------------------------------- */

static int smode, srev, snat;

/* higher tier first, then by mode, then by name */
static int before(struct ent *a, struct ent *b)
{
    int c = 0;
    if (a->kind != b->kind) return a->kind > b->kind;
    if (smode == PS_SIZE) c = a->size > b->size ? -1 : a->size < b->size ? 1 : 0;
    else if (smode == PS_DATE) c = a->date > b->date ? -1 : a->date < b->date ? 1 : 0;
    else if (smode == PS_EXT) c = snat ? natcmp(extof(a->name), extof(b->name)) : nocase(extof(a->name), extof(b->name));
    if (c == 0) c = snat ? natcmp(a->name, b->name) : nocase(a->name, b->name);
    if (srev) c = -c;
    return c < 0;
}

/* Shell sort, Knuth gaps: FFS hands out near-sorted names, which the
 * gap ladder exploits (the E CFile's C1 measurement) */
void place_sort(struct place *P, int mode, int rev)
{
    int n = P->n, gap = 1, i, j;
    struct ent t;
    smode = mode & 15;
    snat = (mode & PS_NATURAL) != 0;
    srev = rev;
    while (gap * 3 + 1 < n) gap = gap * 3 + 1;
    for (; gap >= 1; gap = (gap - 1) / 3)
        for (i = gap; i < n; i++)
            for (j = i; j >= gap && before(&P->ent[j], &P->ent[j - gap]); j -= gap) {
                t = P->ent[j];
                P->ent[j] = P->ent[j - gap];
                P->ent[j - gap] = t;
            }
}

/* ---- reading --------------------------------------------------------------- */

static void addvol(void *ctx, const char *name, int kind)
{
    place_add((struct place *)ctx, name, kind == VL_VOLUME ? KD_DIR : KD_ASSIGN, 0, 0);
}

void place_read(struct place *P, int mode, int rev)
{
    struct dscan *d;
    struct dent e;
    int r;
    P->n = 0;
    P->fail = 0;
    P->freeok = 0;
    pool_free(&P->names);
    if (P->arc && (r = arcfs_read(P)) <= 0) {
        if (r < 0) P->fail = 1;
    } else if (pl_involume(P)) {
        os_volumes(addvol, P);
    } else if (!(d = ds_open(P->path))) {
        P->fail = 1;
    } else {
        while ((r = ds_next(d, &e)) > 0) {
            int kind = e.type > 0 ? KD_DIR : KD_FILE;
            if (e.type == ST_SOFTLINK) {
                /* a soft link says nothing about its target: ask, so a
                 * link to a file is a file (the b52 rule) */
                char full[CPATH];
                strcpy(full, P->path);
                os_quiet(1);            /* a link to an absent volume: no requester */
                kind = addpart(full, e.name, CPATH) && os_pathtype(full) == 2 ? KD_DIR : KD_FILE;
                os_quiet(0);
            }
            if (!place_add(P, e.name, kind, kind == KD_DIR ? 0 : e.size, e.days * 1440 + e.mins)) {
                P->fail = P->n == 0;        /* out of memory: say so rather than cut silently */
                showmsg("not enough memory for the whole directory");
                break;
            }
            P->ent[P->n - 1].prot = e.prot;
            /* on 1.3 too (os_islink is 0 there): the entry's own type */
            if (e.type == ST_SOFTLINK || e.type == ST_LINKDIR || e.type == ST_LINKFILE) P->ent[P->n - 1].link = 1;
        }
        if (r < 0) P->fail = 1;
        ds_close(d);
    }
    place_sort(P, mode, rev);
}

/* path's first n characters are the archive file's name (no case) */
static int startswith(const char *path, const char *file)
{
    int n = strlen(file), i;
    if ((int)strlen(path) < n || (path[n] != 0 && path[n] != '/')) return 0;
    for (i = 0; i < n; i++) {
        int x = (unsigned char)path[i], y = (unsigned char)file[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

int place_go(struct place *P, const char *path)
{
    char f[CPATH];
    int l = strlen(path), i, t, k;
    if (l >= CPATH - 1) return 0;
    if (P->arc && startswith(path, arcfs_file(P))) {
        /* still inside the archive it stands in */
        strcpy(P->path, path);
        return 1;
    }
    /* the archive it stands in is left only once the new place is known
     * good: a 0 leaves P where it was */
    os_quiet(1);
    t = *path ? os_pathtype(path) : 2;
    os_quiet(0);
    if (t == 2) {
        arcfs_leave(P);
        strcpy(P->path, path);
        return 1;
    }
    /* the longest part of it that is a file: an archive to go into */
    for (i = l; i > 0; i--) {
        if (i < l && path[i] != '/') continue;
        memcpy(f, path, i);
        f[i] = 0;
        os_quiet(1);
        t = os_pathtype(f);
        os_quiet(0);
        if (t == 2) return 0;           /* a real folder, without the rest */
        if (t != 1) continue;
        k = arcfs_kind(f);
        if (k == AK_NONE || k == AK_DMS) return 0;
        arcfs_leave(P);
        if (!arcfs_open(P, f) || !P->arc) {
            /* the old archive is gone: stand in the folder holding f */
            arcfs_leave(P);
            strcpy(P->path, f);
            pathup(P->path);
            return 0;
        }
        strcpy(P->path, path);
        return 1;
    }
    return 0;
}

void place_free(struct place *P)
{
    if (P->arc || P->as) arcfs_leave(P);
    xfree(P->ent);
    P->ent = NULL;
    P->n = P->cap = 0;
    pool_free(&P->names);
}
