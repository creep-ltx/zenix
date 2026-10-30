/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* list - what the screen shows of a place (the core reads it), and
 * path arithmetic. */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/exall.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"
#include "core/util.h"
#include "fs/arcfs.h"

DList *dl_new(void)
{
    DList *l = calloc(1, sizeof(DList));
    return l;
}

void dl_free(DList *l)
{
    if (!l) return;
    place_free(&l->pl);
    free(l->vis);
    free(l);
}

long dflags(DList *l, long i)
{
    struct ent *e = DENT(l, i);
    long f = (e->prot & 0xFF) << 16;
    if (e->mark) f |= F_MARK;
    if (l->pl.path[0] == 0) return f | F_DIR | (e->kind == KD_ASSIGN ? F_ASN : F_VOL);
    if (e->link) f |= F_LINK;
    if (e->kind != KD_FILE) return f | F_DIR;
    if (*extof(e->name) == 0 && kindof(e->name) == K_PLAIN &&
        (!(e->prot & FIBF_EXECUTE) || (e->prot & FIBF_SCRIPT)))
        f |= F_EXEC;
    return f;
}

APTR quietreq(void)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    APTR old = pr->pr_WindowPtr;
    pr->pr_WindowPtr = (APTR)-1;
    return old;
}

void loudreq(APTR old)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    pr->pr_WindowPtr = old;
}

static int isinfoname(const char *n)
{
    int l = (int)strlen(n);
    return l > 5 && nocase(n + l - 5, ".info") == 0;
}

/* what this screen shows of the place: icons hidden, the filter */
static void showsome(DList *l)
{
    long i;
    l->n = 0;
    if (l->viscap < l->pl.n) {
        int *nv = malloc((l->pl.n + 16) * sizeof(int));
        if (!nv) return;
        free(l->vis);
        l->vis = nv;
        l->viscap = l->pl.n + 16;
    }
    for (i = 0; i < l->pl.n; i++) {
        const char *name = l->pl.ent[i].name;
        if ((l->how & RD_HIDE) && hideinfo && isinfoname(name)) continue;
        if ((l->how & RD_FILTER) && filter[0] && !nchas(name, filter)) continue;
        l->vis[l->n++] = (int)i;
    }
}

/* the shown ones again (the filter changed), the order as it is */
void dl_show(DList *l)
{
    showsome(l);
}

void dl_sort(DList *l)
{
    place_sort(&l->pl, sortmode | PS_NATURAL, sortrev);
    showsome(l);
}

int dl_read(DList *l, const char *path, int how)
{
    APTR old;
    l->how = how;
    old = quietreq();
    /* a folder, or a folder inside an archive or disk image */
    l->ok = path == l->pl.path || place_go(&l->pl, path);
    if (!l->ok) {
        if (path != l->pl.path) sfmt(l->pl.path, CPATH, "%s", path);
        place_free(&l->pl);
        l->pl.fail = 1;
    } else {
        place_read(&l->pl, sortmode | PS_NATURAL, sortrev);
        l->ok = !l->pl.fail;
    }
    loudreq(old);
    showsome(l);
    return l->ok;
}

long dl_find(DList *l, const char *name)
{
    long i;
    for (i = 0; i < l->n; i++)
        if (nocase(DNAME(l, i), name) == 0) return i;
    return -1;
}

void dl_clamp(DList *l, int rows)
{
    if (l->sel >= l->n) l->sel = l->n - 1;
    if (l->sel < 0) l->sel = 0;
    if (l->top > l->sel) l->top = l->sel;
    if (l->sel >= l->top + rows) l->top = l->sel - rows + 1;
    if (l->top > l->n - rows) l->top = l->n - rows;
    if (l->top < 0) l->top = 0;
}

/* ---- paths ---------------------------------------------------------- */

void joinpath(char *dst, const char *dir, const char *name)
{
    int l;
    if (dst != dir) strcpy(dst, dir);
    l = (int)strlen(dst);
    if (l > 0 && dst[l - 1] != ':' && dst[l - 1] != '/') {
        if (l < PATHMAX - 1) dst[l++] = '/';
        dst[l] = 0;
    }
    strncat(dst, name, PATHMAX - 1 - l);
}

void parentpath(char *dst, const char *path)
{
    int l = (int)strlen(path), i;
    if (dst != path) strcpy(dst, path);
    if (l == 0 || dst[l - 1] == ':') {
        dst[0] = 0;             /* a root's parent is the volume list */
        return;
    }
    if (dst[l - 1] == '/') dst[--l] = 0;
    for (i = l - 1; i >= 0; i--) {
        if (dst[i] == '/') {
            dst[i] = 0;
            return;
        }
        if (dst[i] == ':') {
            dst[i + 1] = 0;
            return;
        }
    }
    dst[0] = 0;
}

const char *lastpart(const char *path)
{
    int l = (int)strlen(path), i;
    if (l > 0 && path[l - 1] == ':') return path;    /* a volume */
    for (i = l - 1; i >= 0; i--)
        if (path[i] == '/' || path[i] == ':') return path + i + 1;
    return path;
}

int canonical(const char *path, char *dst)
{
    BPTR lock;
    int ok = 0;
    APTR old = quietreq();
    if ((lock = Lock((STRPTR)path, SHARED_LOCK)) != 0) {
        ok = NameFromLock(lock, (STRPTR)dst, PATHMAX) ? 1 : 0;
        UnLock(lock);
    }
    loudreq(old);
    return ok;
}

int pathexists(const char *path)
{
    BPTR lock;
    APTR old = quietreq();
    lock = Lock((STRPTR)path, SHARED_LOCK);
    loudreq(old);
    if (!lock) return 0;
    UnLock(lock);
    return 1;
}

int isdirpath(const char *path)
{
    BPTR lock;
    int r = -1;
    struct FileInfoBlock *fib;
    APTR old = quietreq();
    if ((lock = Lock((STRPTR)path, SHARED_LOCK)) != 0) {
        if ((fib = AllocDosObject(DOS_FIB, NULL)) != NULL) {
            if (Examine(lock, fib)) r = fib->fib_DirEntryType > 0;
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(lock);
    }
    loudreq(old);
    return r;
}
