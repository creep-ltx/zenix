/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ops - see ops.h. Ported from the E CFile 0.5b52 with its fixes. */
#include <string.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/graphics.h>
#include "ops.h"
#include "arcfs.h"
#include "arcw.h"
#include "mount.h"
#include "../core/version.h"
#include "place.h"
#include "fe.h"
#include "os.h"
#include "../core/core.h"
#include "../core/util.h"
#include "../core/arc.h"


#define MAXDEPTH 20

static UBYTE *cbuf;             /* the copy buffer, only while an op runs */
static ULONG cbufsz;
static int gfails;              /* entries a run could not handle */
static int gskip;               /* soft links left out of copied trees */
static int unprotall;           /* "a" at the unprotect prompt */
static struct fsshared fsown = { "", 0, 0, 1, 0, 0 };     /* icons on */
struct fsshared *fssh = &fsown;

struct fsshared *fs_sh(void)
{
    return fssh->job && (APTR)SysBase->ThisTask == fssh->jobtask ? fssh->job : fssh;
}

/* ---- small helpers ------------------------------------------------------ */

static int quietops;            /* wipetree: our own scratch, no messages */

void faultmsg(const char *what)
{
    if (quietops) return;
    char fb[80];
    os_fault(IoErr(), fb, sizeof(fb));
    sfmt(opmsg, sizeof(opmsg), "%s: %s", what, fb);
    showmsg(opmsg);
}

int buildfull(char *dst, const char *dir, const char *name)
{
    strcpy(dst, dir);
    return addpart(dst, name, CPATH - 4);
}

int isinfo(const char *name)
{
    int l = strlen(name);
    return l >= 5 && nocase(name + l - 5, ".info") == 0;
}

/* the .info beside dir/name, when there is one */
int sidecarof(const char *dir, const char *name, char *dst)
{
    char nm[NAMEMAX + 8];
    sfmt(nm, sizeof(nm), "%s.info", name);
    return buildfull(dst, dir, nm) && os_pathtype(dst) == 1;
}

/* an archive pane lists name.info beside name */
int hasinfo(struct place *p, const char *name)
{
    char nm[NAMEMAX + 8];
    sfmt(nm, sizeof(nm), "%s.info", name);
    return place_find(p, nm) >= 0;
}

/* the copy buffer, sized to the machine: big chunks are fewer packet
 * round trips (the E campaign's I1), small ones keep a 512K Amiga able
 * to run at all. Allocated per operation, freed after. */
int cbuf_get(void)
{
    ULONG big = AvailMem(MEMF_ANY | MEMF_LARGEST);
    if (cbuf) return 1;                 /* already held: not a second one */
    static const ULONG sizes[3] = { 262144, 65536, 16384 };
    int i;
    for (i = 0; i < 3; i++) {
        if (big < sizes[i] * 4 && i < 2) continue;    /* leave the machine room */
        if ((cbuf = AllocMem(sizes[i], MEMF_ANY))) {
            cbufsz = sizes[i];
            return 1;
        }
    }
    if ((cbuf = AllocMem(4096, MEMF_ANY))) cbufsz = 4096;
    return cbuf != NULL;
}

void cbuf_free(void)
{
    if (cbuf) FreeMem(cbuf, cbufsz);
    cbuf = NULL;
    cbufsz = 0;
}

/* ---- safe writes: never empty the old file first ---------------------- */

/* "CFile<tag><task>.tmp" in path's own directory (a same-volume Rename) */
void sidename(char *dst, const char *path, int tag)
{
    char nm[24];
    int n = pathlen(path);
    memcpy(dst, path, n);
    dst[n] = 0;
    sfmt(nm, sizeof(nm), "CFile%c%lx.tmp", tag, (ULONG)FindTask(NULL));
    addpart(dst, nm, CPATH);
}

/* put the finished tmp in place of path: the old file goes aside first
 * and is dropped only once the new one is in. keep = carry the old
 * file's bits and comment (a save; a copy brings its own). A write-
 * protected old file is refused, as an Open over it would have been. */
int swapin(const char *tmp, const char *path, int keep)
{
    char old[CPATH];
    struct FileInfoBlock *fib;
    BPTR l;
    LONG prot = 0;
    int had = 0, ok = 0;
    char cmt[80];
    cmt[0] = 0;
    if (!(fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) return 0;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) {
        if (Examine(l, fib) && fib->fib_DirEntryType < 0) {
            had = 1;
            prot = fib->fib_Protection;
            sfmt(cmt, sizeof(cmt), "%s", fib->fib_Comment);
        }
        UnLock(l);
    }
    FreeMem(fib, sizeof(*fib));
    if (had) {
        if (prot & FIBF_WRITE) return 0;
        sidename(old, path, 'o');
        DeleteFile((STRPTR)old);            /* our own leftover, if any */
        if (!Rename((STRPTR)path, (STRPTR)old)) return 0;
    }
    if (Rename((STRPTR)tmp, (STRPTR)path)) {
        ok = 1;
        if (had) {
            if (keep) {
                SetProtection((STRPTR)path, prot);
                if (cmt[0]) SetComment((STRPTR)path, (STRPTR)cmt);
            }
            SetProtection((STRPTR)old, 0);
            DeleteFile((STRPTR)old);
        }
    } else if (had) {
        Rename((STRPTR)old, (STRPTR)path);    /* put the old one back */
    }
    return ok;
}

/* open a save target: the scratch name beside path when path already
 * holds a file (*swap set), else path itself. 0 = failed. */
/* path, or what a soft link at path points to: a save through a link
 * changes the file it points to, it does not replace the link (2.0+) */
const char *linktarget(const char *path, char *buf)
{
    BPTR l;
    if (!os_islink(path) || !(l = Lock((STRPTR)path, ACCESS_READ))) return path;
    if (!os_lockname(l, buf, CPATH)) buf[0] = 0;
    UnLock(l);
    return buf[0] ? buf : path;
}

BPTR safeopen(const char *path, char *tmp, int *swap)
{
    *swap = os_pathtype(path) == 1;
    if (*swap) sidename(tmp, path, 'n');
    else strcpy(tmp, path);
    return Open((STRPTR)tmp, MODE_NEWFILE);
}

/* finish what safeopen began: close (a buffered handler reports its
 * last error there, from V36), then swap in. FALSE = the old file is
 * untouched and the scratch is gone. */
int safeclose(BPTR fh, const char *path, const char *tmp, int swap, int ok, int keep)
{
    if (v_dos >= 36) {
        if (!Close(fh)) ok = 0;
    } else {
        Close(fh);
    }
    if (ok && swap && !swapin(tmp, path, keep)) ok = 0;
    if (!ok) DeleteFile((STRPTR)tmp);
    return ok;
}

/* the version a file says it is (core/version.c), read the way the
 * Version command does: an executable in full (up to 2 MB), anything
 * else its first 64K - a big data file has no version to find */
void fileversion(const char *path, struct verinfo *v)
{
    BPTR fh;
    UBYTE *b;
    LONG pos = 0, n, lim = 65536;
    int first = 1;
    memset(v, 0, sizeof(*v));
    if (!(b = xalloc(4096))) return;
    if (!(fh = Open((STRPTR)path, MODE_OLDFILE))) {
        xfree(b);
        return;
    }
    for (;;) {
        if (pos && Seek(fh, pos, OFFSET_BEGINNING) < 0) break;
        if ((n = Read(fh, b, 4096)) <= 0) break;
        if (first) {
            if (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 3 && b[3] == 0xF3) lim = 2097152;
            ver_romtag(b, n, v);
            first = 0;
        }
        {
            struct verinfo w;
            memset(&w, 0, sizeof(w));
            if (ver_scan(b, n, n < 4096, &w)) {
                if (v->found != 2 || w.ver || w.rev >= 0) *v = w;
                break;
            }
        }
        if (n < 4096 || pos + n >= lim) break;
        pos += n - VER_OVERLAP;
    }
    Close(fh);
    xfree(b);
}

/* a file's date in days, -1 when it cannot be read */
LONG filedays(const char *path)
{
    struct FileInfoBlock *fib;
    BPTR l;
    LONG d = -1;
    if (!(fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) return -1;
    if ((l = Lock((STRPTR)path, ACCESS_READ))) {
        if (Examine(l, fib)) d = fib->fib_Date.ds_Days;
        UnLock(l);
    }
    FreeMem(fib, sizeof(*fib));
    return d;
}

/* what a file is, from its first 512 bytes (TY_*); -1 = unreadable */
int sniffpath(const char *path)
{
    unsigned char hb[1084];        /* a module's mark is at 1080 */
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    LONG n;
    if (!fh) return -1;
    n = Read(fh, hb, sizeof(hb));
    Close(fh);
    return sniffmem(hb, n > 0 ? n : 0);
}

/* carry protection, date and comment from src to dst */
static void copyattribs(const char *src, const char *dst)
{
    struct FileInfoBlock *fib;
    BPTR l;
    if (!(fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) return;
    if ((l = Lock((STRPTR)src, ACCESS_READ))) {
        if (Examine(l, fib)) {
            UnLock(l);
            l = 0;
            SetProtection((STRPTR)dst, fib->fib_Protection);
            os_setdate(dst, &fib->fib_Date);
            if (fib->fib_Comment[0]) SetComment((STRPTR)dst, fib->fib_Comment);
        }
        if (l) UnLock(l);
    }
    FreeMem(fib, sizeof(*fib));
}

/* ---- copy one file ---------------------------------------------------- */

int copyfile(const char *src, const char *dst)
{
    BPTR in, out;
    char tmp[CPATH];
    int swap, ok = 1, bad = 0;
    LONG n, err = 0;
    ULONG crc = 0;
    /* VERIFY needs the CRC table: without it every sum is 0 and a bad
     * copy would pass */
    if (!cbuf || (verifycopy && !arc_crcready())) {
        sfmt(opmsg, sizeof(opmsg), "not enough memory");
        return 0;
    }
    if (!(in = Open((STRPTR)src, MODE_OLDFILE))) {
        faultmsg("cannot read the source");
        return 0;
    }
    /* an existing target is not emptied: the copy lands beside it */
    swap = os_pathtype(dst) == 1;
    if (swap) sidename(tmp, dst, 'n');
    else strcpy(tmp, dst);
    if (!(out = Open((STRPTR)tmp, MODE_NEWFILE))) {
        Close(in);
        faultmsg("cannot write the target");
        return 0;
    }
    for (;;) {
        if (checkabort()) {
            ok = 0;
            break;
        }
        n = Read(in, cbuf, cbufsz);
        if (n < 0) {
            ok = 0;
            err = IoErr();
            break;
        }
        if (n == 0) break;
        if (Write(out, cbuf, n) != n) {
            ok = 0;
            err = IoErr();
            break;
        }
        if (verifycopy) crc = arc_crc32(crc, cbuf, n);
        progadd(n);                         /* real bytes: the bar creeps */
    }
    Close(in);
    /* a buffered handler reports its last write error at Close (V36+;
     * 1.3's Close returns nothing to read) */
    if (v_dos >= 36) {
        if (!Close(out) && ok) ok = 0, err = IoErr();
    } else {
        Close(out);
    }
    /* VERIFY: the copy read back from the disk must be what was read
     * from the original - before it replaces anything */
    if (ok && verifycopy) {
        ULONG c2 = 0;
        if (!(out = Open((STRPTR)tmp, MODE_OLDFILE))) ok = 0, err = IoErr();
        else {
            while ((n = Read(out, cbuf, cbufsz)) > 0) c2 = arc_crc32(c2, cbuf, n);
            if (n < 0) ok = 0, err = IoErr();
            Close(out);
            if (ok && c2 != crc) ok = 0, bad = 1;
        }
    }
    if (ok && swap && !swapin(tmp, dst, 0)) {
        DeleteFile((STRPTR)tmp);
        faultmsg("cannot replace the target");
        return 0;
    }
    if (!ok) {
        DeleteFile((STRPTR)tmp);            /* no partial targets */
        if (bad) {
            sfmt(opmsg, sizeof(opmsg), "the copy of %.40s read back wrong - not kept (VERIFY)", filepart(src));
            showmsg(opmsg);
        } else if (!abortop) {
            char fb[80];
            os_fault(err, fb, sizeof(fb));
            sfmt(opmsg, sizeof(opmsg), "copy failed: %s", fb);
            showmsg(opmsg);
        }
        return 0;
    }
    copyattribs(src, dst);
    return 1;
}

/* ---- walking trees ------------------------------------------------------
 * Delete walks real directories only; copy and measure also follow hard-
 * linked directories but never soft links (a link to Work: inside a
 * folder used to empty Work:). An empty name never walks. */

static int walkdel(struct dent *e) { return e->type == ST_USERDIR && e->name[0]; }
static int walkcopy(struct dent *e)
{
    return (e->type == ST_USERDIR || e->type == ST_LINKDIR) && e->name[0];
}

void treestat(const char *path, int depth)
{
    struct dscan *d;
    struct dent e;
    char *child;
    if (depth > MAXDEPTH || !(d = ds_open(path))) return;
    if ((child = xalloc(CPATH))) {
        while (ds_next(d, &e) > 0 && !checkabort()) {
            statfiles++;
            if (walkcopy(&e)) {
                if (buildfull(child, path, e.name)) treestat(child, depth + 1);
            } else {
                statbytes += e.size;
            }
        }
        xfree(child);
    }
    ds_close(d);
}

int copytree(const char *src, const char *dst, int depth)
{
    struct dscan *d;
    struct dent e;
    char *cs, *cd;
    BPTR lk;
    int ok = 1, r;
    if (depth > MAXDEPTH) {
        showmsg("directory tree too deep");
        return 0;
    }
    /* the source must be readable BEFORE the target exists */
    if (!(d = ds_open(src))) {
        faultmsg("cannot read the source");
        return 0;
    }
    if ((lk = CreateDir((STRPTR)dst))) {
        UnLock(lk);
    } else if (os_pathtype(dst) != 2) {
        ds_close(d);
        faultmsg("cannot create the target directory");
        return 0;
    }
    cs = xalloc(CPATH);
    cd = xalloc(CPATH);
    if (!cs || !cd) ok = 0;
    while (ok && (r = ds_next(d, &e)) > 0) {
        if (!buildfull(cs, src, e.name) || !buildfull(cd, dst, e.name)) {
            showmsg("the path is too long");
            ok = 0;
        } else if (walkcopy(&e)) {
            ok = copytree(cs, cd, depth + 1);
        } else if (e.type == ST_SOFTLINK) {
            gskip++;                        /* a soft link is not followed */
        } else if (e.name[0]) {
            ok = copyfile(cs, cd);
        }
    }
    if (ok && r < 0) ok = 0;                /* a scan that died fails the copy */
    ds_close(d);
    xfree(cs);
    xfree(cd);
    if (ok) copyattribs(src, dst);
    return ok;
}

/* delete one object; ask = may prompt to clear its delete protection */
int zap(const char *path, int ask)
{
    LONG err, prot = -1;
    struct FileInfoBlock *fib;
    BPTR l;
    char pm[140];
    int k;
    if (DeleteFile((STRPTR)path)) return 1;
    err = IoErr();
    if (err == ERROR_OBJECT_NOT_FOUND) return 1;
    /* a disk image in a drive: out of it first (mount.h) */
    if (err == ERROR_OBJECT_IN_USE && mnt_release(path)) {
        if (DeleteFile((STRPTR)path)) return 1;
        err = IoErr();
    }
    if (err != ERROR_DELETE_PROTECTED) return 0;    /* no bit fixes the rest */
    if (ask && !unprotall) {
        sfmt(pm, sizeof(pm), "\"%s\" is protected - unprotect? (y)es (n)o (a)ll", filepart(path));
        k = askkey(pm) | 32;
        if (k == 'a') unprotall = 1;
        else if (k != 'y') return 0;
    }
    if ((fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) {
        if ((l = Lock((STRPTR)path, ACCESS_READ))) {
            if (Examine(l, fib)) prot = fib->fib_Protection;
            UnLock(l);
        }
        FreeMem(fib, sizeof(*fib));
    }
    if (prot == -1) return 0;
    /* clear the d bit only, and put every bit back if it still fails */
    SetProtection((STRPTR)path, prot & ~FIBF_DELETE);
    if (DeleteFile((STRPTR)path)) return 1;
    SetProtection((STRPTR)path, prot);
    return 0;
}

/* a directory's names, read whole before anything is deleted (deleting
 * during a scan invalidates it) */
struct snap {
    char *buf;
    LONG used, cap;
};

static int snapadd(struct snap *s, const char *name, int isdir)
{
    int l = strlen(name) + 2;
    if (s->used + l > s->cap) {
        LONG nc = s->cap ? s->cap * 2 : 2048;
        char *nb = xalloc(nc);
        if (!nb) return 0;
        if (s->used) memcpy(nb, s->buf, s->used);
        xfree(s->buf);
        s->buf = nb;
        s->cap = nc;
    }
    s->buf[s->used] = (char)isdir;
    strcpy(s->buf + s->used + 1, name);
    s->used += l;
    return 1;
}

static void dsay(const char *m)
{
    if (!quietops) showmsg(m);
}

int deltree(const char *path, int depth, int tick)
{
    struct dscan *d;
    struct dent e;
    struct snap s = { NULL, 0, 0 };
    char *child;
    LONG i;
    int ok = 1, r;
    if (depth > MAXDEPTH) {
        dsay("directory tree too deep");
        gfails++;
        return 0;
    }
    /* the top may itself be a link (a pane entry, a moved folder's
     * source): remove the link, never walk into it */
    if (depth == 0 && os_islink(path)) {
        if (zap(path, tick)) return 1;
        faultmsg("cannot delete the link");
        gfails++;
        return 0;
    }
    if (!(d = ds_open(path))) {
        faultmsg("cannot read the directory");
        gfails++;
        return 0;
    }
    while ((r = ds_next(d, &e)) > 0)
        if (!snapadd(&s, e.name, walkdel(&e))) {
            r = -1;
            break;
        }
    ds_close(d);
    if (r < 0) {
        xfree(s.buf);
        dsay("cannot read the whole directory - nothing deleted there");
        gfails++;
        return 0;
    }
    if (!(child = xalloc(CPATH))) {
        xfree(s.buf);
        return 0;
    }
    for (i = 0; i < s.used && !checkabort(); i += strlen(s.buf + i + 1) + 2) {
        const char *nm = s.buf + i + 1;
        int one;
        if (!buildfull(child, path, nm)) {
            dsay("the path is too long");
            gfails++;
            ok = 0;
            continue;
        }
        if (s.buf[i]) {
            one = deltree(child, depth + 1, tick);
        } else {
            one = zap(child, 1);
            if (one && tick) progadd(1);
            if (!one) {
                char pm[140];
                sfmt(pm, sizeof(pm), "cannot delete \"%s\"", nm);
                faultmsg(pm);
                gfails++;
            }
        }
        if (!one) ok = 0;
    }
    xfree(child);
    xfree(s.buf);
    if (abortop) return 0;
    if (ok) {
        if (zap(path, 1)) {
            if (tick) progadd(1);
        } else {
            faultmsg("cannot delete the directory");
            gfails++;
            ok = 0;
        }
    }
    return ok;
}

/* our own scratch tree, gone without questions (its bits came from
 * the copies, so a protected file in it is ours to unprotect) */
void wipetree(const char *path)
{
    int u = unprotall, f = gfails;
    char keep[sizeof(opmsg)];
    strcpy(keep, opmsg);                /* the real news is the caller's */
    quietops = 1;
    unprotall = 1;
    if (os_pathtype(path) == 2) {
        int i;
        deltree(path, 0, 0);
        /* a tool's shell can still hold it as its current directory
         * for a moment after Execute returns: wait, and try again */
        for (i = 0; i < 25 && os_pathtype(path) == 2; i++) {
            Delay(4);
            DeleteFile((STRPTR)path);
        }
    } else {
        zap(path, 0);
    }
    unprotall = u;
    gfails = f;
    quietops = 0;
    strcpy(opmsg, keep);
}

/* TRUE when b is a (or inside a): a copy into itself never ends */
int insidedir(const char *a, const char *b)
{
    BPTR la, l, pl;
    int inside = 0;
    if (!(la = Lock((STRPTR)a, ACCESS_READ))) return 0;
    l = Lock((STRPTR)b, ACCESS_READ);
    while (l) {
        if (os_samelock(la, l) == 2) {
            inside = 1;
            UnLock(l);
            break;
        }
        pl = ParentDir(l);
        UnLock(l);
        l = pl;
    }
    UnLock(la);
    return inside;
}

/* ---- copy and move -------------------------------------------------------- */

/* resolve one entry's target name: collision prompts, no file I/O.
 * 1 go, 0 skip, -1 refused (message shown), -2 Esc: give up the run */
static int resolveone(struct place *p, struct place *q, const char *name, int isdir, int force, char *tname,
                      char (*chosen)[NAMEMAX + 4], int nchosen)
{
    char src[CPATH], dst[CPATH], mb[160];
    int t, k, i;
    if (strlen(name) > NAMEMAX) {
        /* an archive or CD may hold longer names than AmigaDOS takes */
        sfmt(mb, sizeof(mb), "a name too long for AmigaDOS: %.60s...", name);
        showmsg(mb);
        return -1;
    }
    strcpy(tname, name);
    if (pl_inarc(p)) {
        /* a member's ':' or '/' (a zip from Linux) must not reach a path */
        char *c;
        for (c = tname; *c; c++)
            if (*c == ':' || *c == '/') *c = '_';
    }
    if (!buildfull(src, p->path, name)) {
        showmsg("the path is too long");
        return -1;
    }
    for (;;) {
        if (!buildfull(dst, q->path, tname)) {
            showmsg("the path is too long");
            return -1;
        }
        /* a name an earlier entry of this run already takes counts as
         * taken (renamed to b, then b itself, overwrote the first) */
        for (i = 0; i < nchosen; i++)
            if (nocase(chosen[i], tname) == 0) break;
        if (i < nchosen) {
            sfmt(mb, sizeof(mb), "\"%s\" is taken by an earlier entry: (s)kip (r)ename?", tname);
            k = askkey(mb);
            if ((k | 32) == 'r') {
                if (!lineinput("new name: ", tname, NAMEMAX, 1) || !tname[0]) {
                    askdone();
                    return 0;
                }
                continue;
            }
            return k == 27 ? -2 : 0;
        }
        if ((t = pl_inarc(q) ? arcw_has(q, tname) : os_pathtype(dst)) == 0) break;
        if (!pl_inarc(q)) {
            BPTR a = Lock((STRPTR)src, ACCESS_READ), b = Lock((STRPTR)dst, ACCESS_READ);
            int same = os_samelock(a, b) == 2;
            if (a) UnLock(a);
            if (b) UnLock(b);
            if (same) {
                showmsg("source and target are the same file");
                return -1;
            }
        }
        {
            /* a file over a file: what versions they say they are (a
             * library over a newer one is the classic mistake); dates
             * only when neither says, and they may be wrong (no clock) */
            int c = 0, known = 0;
            char what[90];
            what[0] = 0;
            if (!isdir && t == 1 && !pl_inarc(p) && !pl_inarc(q)) {
                struct verinfo vs, vd;
                char a[32], b[32];
                fileversion(src, &vs);
                fileversion(dst, &vd);
                ver_str(&vs, a, sizeof(a));
                ver_str(&vd, b, sizeof(b));
                if (a[0] && b[0]) {
                    const char *tag;
                    c = ver_cmp(&vs, &vd);
                    known = 1;
                    tag = c < 0 ? ", NEWER there" : c > 0 ? ", older there" : ", same";
                    sfmt(what, sizeof(what), " %s over %s%s", a, b, tag);
                    /* the build dates make it long: when the numbers differ
                     * they are not what decides, and go first */
                    if ((int)strlen(tname) + (int)strlen(what) + 32 > askwidth() - 8 &&
                        (vs.ver != vd.ver || vs.rev != vd.rev)) {
                        vs.days = vd.days = -1;
                        ver_str(&vs, a, sizeof(a));
                        ver_str(&vd, b, sizeof(b));
                        sfmt(what, sizeof(what), " %s over %s%s", a, b, tag);
                    }
                } else {
                    LONG ds = filedays(src), dd = filedays(dst);
                    char x[12], y[12];
                    if (ds >= 0 && dd >= 0 && ds != dd) {
                        datestr(ds, x);
                        datestr(dd, y);
                        sfmt(what, sizeof(what), " dated %s over %s", x, y);
                    }
                }
            }
            if (force && !(known && c < 0)) {
                k = 'o';                    /* C/M: but never a silent downgrade */
            } else {
                /* the name gives way before the versions and the keys do */
                int room = askwidth() - 8 - (int)strlen(what) - 30;
                if (room < 8) room = 8;
                if (room > 24) room = 24;
                char nm[28];
                sfmt(nm, sizeof(nm), "%s", tname);
                if ((int)strlen(nm) > room) nm[room] = 0;     /* (sfmt has no %.*s) */
                sfmt(mb, sizeof(mb), "\"%s\"%s: (s)kip (o)verwrite (r)ename?", nm, what[0] ? what : " exists");
                k = askkey(mb);
            }
        }
        if ((k | 32) == 'o') {
            if (isdir && t == 1) {
                showmsg("a file with the target name is in the way");
                return -1;
            }
            if (!isdir && t == 2) {
                showmsg("the target exists as a directory");
                return -1;
            }
            break;                          /* a directory merges in */
        } else if ((k | 32) == 'r') {
            if (!lineinput("new name: ", tname, NAMEMAX, 1) || !tname[0]) {
                askdone();
                return 0;
            }
        } else if (k == 27) {
            return -2;
        } else {
            return 0;
        }
    }
    if (isdir && !pl_inarc(p) && !pl_inarc(q) && os_pathtype(dst) == 2 && insidedir(dst, src)) {
        /* Work:a/a merged into Work:a: the source is inside the target,
         * and the merge (and a move's delete) ate its own data */
        showmsg("the source is inside the target");
        return -1;
    }
    if (isdir && !pl_inarc(q) && insidedir(src, q->path)) {
        showmsg("the target is inside the source");
        return -1;
    }
    return 1;
}

/* transfer one resolved entry - file I/O only. 1 done, -1 failed */
static int transferone(struct place *p, struct place *q, const char *name, const char *tname, int isdir,
                       int ismove, int samevol)
{
    char src[CPATH], dst[CPATH], tmp[CPATH];
    int t = 0;
    if (!buildfull(src, p->path, name) || !buildfull(dst, q->path, tname)) {
        showmsg("the path is too long");
        return -1;
    }
    if (pl_inarc(p)) {
        if (!arcfs_copyout(p, name, dst, isdir)) return -1;
        /* a move out of a disk image: gone from it only once copied */
        if (ismove && !arcfs_adfdelone(p, name, isdir)) return -1;
        return 1;
    }
    if (ismove) {
        if (samevol) t = os_pathtype(dst);
        if (samevol && t == 0) {
            if (!Rename((STRPTR)src, (STRPTR)dst)) {
                faultmsg("cannot move");
                return -1;
            }
        } else if (samevol && t == 1 && !isdir) {
            /* Rename cannot land on a name: rename beside, swap in */
            sidename(tmp, dst, 'm');
            if (!Rename((STRPTR)src, (STRPTR)tmp)) {
                faultmsg("cannot move");
                return -1;
            }
            if (!swapin(tmp, dst, 0)) {
                Rename((STRPTR)tmp, (STRPTR)src);
                faultmsg("cannot replace the target");
                return -1;
            }
        } else if (isdir && (t == 2 || !samevol)) {
            /* a merge, or another volume: copy, then drop the source -
             * unless links inside were left out: they would be lost */
            int sk = gskip;
            if (!copytree(src, dst, 0)) return -1;
            if (gskip != sk) {
                sfmt(opmsg, sizeof(opmsg), "soft links inside \"%s\" were not copied - the source was kept", name);
                return 1;
            }
            if (!deltree(src, 0, 0)) return -1;
        } else if (samevol) {
            showmsg("cannot replace the target");
            return -1;
        } else {
            if (!copyfile(src, dst)) return -1;
            if (!zap(src, 0)) {
                faultmsg("copied, but cannot delete the source");
                return -1;
            }
        }
    } else {
        if (isdir ? !copytree(src, dst, 0) : !copyfile(src, dst)) return -1;
    }
    return 1;
}

/* a new operation: an "all" answered in an earlier one does not carry over */
void op_begin(void)
{
    unprotall = 0;
}

void fs_copy(struct place *p, struct place *q, int cur, int ismove, int force)
{
    int npick, nsel = 0, i, s, r, samevol = 0, ndone = 0;
    int haderr = 0, showbar = 0, anydir = 0, *ridx = NULL, *pick = NULL;
    char (*rnames)[NAMEMAX + 4] = NULL;
    char tname[NAMEMAX + 4], tpath[CPATH], mb[160];
    struct place *P = p;
    op_begin();
    if (pl_involume(p)) {
        showmsg("no file operations in the volume list");
        return;
    }
    if (P->fail) return;
    if (pl_involume(q)) {
        showmsg("the target is the volume list - enter a volume first");
        return;
    }
    if (q->fail) {
        showmsg("the target folder cannot be read");
        return;
    }
    if (pl_inarc(q) && !arcw_canwrite(q) && !arcfs_isadf(q) && arcfs_guard(q)) return;
    if (pl_inarc(p) && ismove && !arcfs_isadf(p) && !arcw_canwrite(p)) {
        showmsg("cannot move out of a CD image - copy (c) instead");
        return;
    }
    if (!(npick = place_pick(p, cur, &pick))) return;
    ridx = xalloc(npick * sizeof(int));
    rnames = xalloc(npick * sizeof(*rnames));
    if (!ridx || !rnames) {
        showmsg("not enough memory");
        goto out;
    }
    {   /* one volume or two? (Rename, or copy and delete) */
        BPTR la = Lock((STRPTR)P->path, ACCESS_READ), lb = Lock((STRPTR)q->path, ACCESS_READ);
        samevol = la && lb && os_samelock(la, lb) >= 1;
        if (la) UnLock(la);
        if (lb) UnLock(lb);
    }
    /* phase 1: resolve the whole set before anything is touched */
    for (s = 0; s < npick; s++) {
        i = pick[s];
        r = resolveone(p, q, P->ent[i].name, P->ent[i].kind != KD_FILE, force, tname, rnames, nsel);
        if (r < 0) goto out;                /* refused or Esc: nothing happened */
        if (r == 1) {
            ridx[nsel] = i;
            strcpy(rnames[nsel], tname);
            nsel++;
        }
    }
    if (nsel == 0) goto out;
    if (pl_inarc(q)) {
        /* into a disk image: the built-in writer; into an archive: the
         * tool and the checked commit (arcw.c) */
        if (arcfs_isadf(q)) arcfs_adfput(p, q, ridx, rnames, nsel, ismove);
        else arcw_add(p, q, ridx, rnames, nsel, ismove);
        goto out;
    }
    if (!cbuf_get()) {
        showmsg("not enough memory");
        goto out;
    }
    /* phase 2: a same-volume move is all Renames, no bar; the rest gets
     * a byte count for the bar and a free-space question */
    cancelok = 1;
    abortop = 0;
    if (!(ismove && samevol)) {
        LONG fr;
        statbytes = statfiles = 0;
        for (s = 0; s < nsel; s++) {
            struct ent *e = &P->ent[ridx[s]];
            if (e->kind != KD_FILE) {
                anydir = 1;
                if (pl_inarc(p)) statbytes += arcfs_treesize(p, e->name);
                else if (buildfull(tpath, P->path, e->name)) treestat(tpath, 0);
            } else {
                statbytes += e->size;
            }
        }
        if (abortop) goto done;
        q->freeok = 0;
        fr = os_freebytes(q->path);
        if (fr >= 0 && statbytes > (ULONG)fr) {
            char f1[12], f2[12];
            fmtsize(f1, statbytes);
            fmtsize(f2, (ULONG)fr);
            sfmt(mb, sizeof(mb), "needs %s, %s free - proceed? (y)es (n)o", f1, f2);
            if ((askkey(mb) | 32) != 'y') goto done;
        }
        if (anydir || nsel > 1 || statbytes > 131072) showbar = 1;
        if (showbar) progshow(statbytes);
    }
    /* phase 3: transfer, nothing left to ask (Esc stops it). A move out
     * of an lha/lzx/zip copies first; what was copied leaves the
     * archive afterwards in one checked commit */
    gskip = 0;
    {
        int flatmove = ismove && pl_inarc(p) && arcw_canwrite(p), ndel = 0, j;
        int *delidx = flatmove ? xalloc(nsel * 2 * sizeof(int)) : NULL;
        if (flatmove && !delidx) {
            showmsg("not enough memory");
            goto done;
        }
        if (pl_inarc(p)) arcfs_hold(p);    /* one session for every entry */
        for (s = 0; s < nsel && !haderr && !abortop; s++) {
            struct ent *e = &P->ent[ridx[s]];
            char iname[NAMEMAX + 8], itname[NAMEMAX + 8], isrc[CPATH];
            r = transferone(p, q, e->name, rnames[s], e->kind != KD_FILE, flatmove ? 0 : ismove, samevol);
            if (r < 0) haderr = 1;
            else ndone++;
            if (r > 0 && flatmove) delidx[ndel++] = ridx[s];
            /* the icon follows the resolved name (silent, best effort) */
            if (!haderr && icons && !isinfo(e->name) &&
                (pl_inarc(p) ? hasinfo(p, e->name) : sidecarof(P->path, e->name, isrc))) {
                sfmt(iname, sizeof(iname), "%s.info", e->name);
                sfmt(itname, sizeof(itname), "%s.info", rnames[s]);
                if (transferone(p, q, iname, itname, 0, flatmove ? 0 : ismove, samevol) > 0 && flatmove)
                    for (j = 0; j < P->n; j++)
                        if (nocase(P->ent[j].name, iname) == 0) delidx[ndel++] = j;
            }
        }
        arcfs_release(p);
        if (flatmove && ndel && !abortop) {
            progoff();
            arcw_delents(p, delidx, ndel);
        }
        xfree(delidx);
    }
done:
    cancelok = 0;
    progoff();
    cbuf_free();
    placechanged(NULL, NULL, showbar ? 2 : 1);
    if (abortop) {
        sfmt(mb, sizeof(mb), "cancelled - %d of %d done", ndone, nsel);
        showmsg(mb);
    } else if (haderr) {
        showmsg(opmsg);
    } else if (gskip) {
        sfmt(mb, sizeof(mb), "%d soft link%s inside were not copied%s", gskip, gskip == 1 ? "" : "s",
             ismove ? " - the folders holding them were kept" : "");
        showmsg(mb);
    }
    abortop = 0;
out:
    xfree(pick);
    xfree(ridx);
    xfree(rnames);
}

/* ---- delete ------------------------------------------------------------ */

static int delone(struct place *p, int i)
{
    struct ent *e = &p->ent[i];
    char dpath[CPATH], ipath[CPATH];
    int ok;
    if (!buildfull(dpath, p->path, e->name)) {
        showmsg("the path is too long");
        gfails++;
        return 0;
    }
    if (e->kind != KD_FILE && !e->link) {
        ok = deltree(dpath, 0, 1);
    } else if ((ok = zap(dpath, 1))) {      /* a link goes as a link */
        progadd(1);
    } else {
        char pm[140];
        sfmt(pm, sizeof(pm), "cannot delete \"%s\"", e->name);
        faultmsg(pm);
        gfails++;
    }
    /* the icon goes along (best effort: never fails the delete) */
    if (ok && icons && !isinfo(e->name) && sidecarof(p->path, e->name, ipath)) zap(ipath, 0);
    return ok;
}

void fs_delete(struct place *p, int cur)
{
    int nmark, s, n, showbar = 0, *pick;
    struct place *P = p;
    char mb[160], dpath[CPATH];
    if (pl_involume(p)) {
        showmsg("no file operations in the volume list");
        return;
    }
    if (pl_inarc(p)) {
        if (arcfs_isadf(p)) arcfs_adfdelete(p, cur);
        else if (arcw_canwrite(p)) arcw_delete(p, cur);
        else arcfs_guard(p);
        return;
    }
    if (P->fail || !(n = place_pick(p, cur, &pick))) return;
    nmark = place_marks(p);
    if (nmark) sfmt(mb, sizeof(mb), "delete %d marked entries? (y)es (n)o", nmark);
    else if (P->ent[cur].kind != KD_FILE) sfmt(mb, sizeof(mb), "delete \"%s\" and all contents? (y)es (n)o", P->ent[cur].name);
    else sfmt(mb, sizeof(mb), "delete \"%s\"? (y)es (n)o", P->ent[cur].name);
    if ((askkey(mb) | 32) != 'y') {
        xfree(pick);
        return;
    }
    cancelok = 1;
    abortop = 0;
    statbytes = statfiles = 0;
    for (s = 0; s < n; s++) {
        struct ent *e = &P->ent[pick[s]];
        statfiles++;
        if (e->kind != KD_FILE && buildfull(dpath, P->path, e->name)) treestat(dpath, 0);
    }
    if (statfiles > 10) showbar = 1, progshow(statfiles);
    gfails = 0;
    unprotall = 0;
    for (s = 0; s < n && !abortop; s++) delone(p, pick[s]);
    xfree(pick);
    cancelok = 0;
    progoff();
    placechanged(NULL, NULL, showbar ? 2 : 1);
    if (abortop) showmsg("delete cancelled");
    else if (gfails == 1) showmsg(opmsg);
    else if (gfails > 1) {
        sfmt(mb, sizeof(mb), "%d entries not deleted - last: %s", gfails, opmsg);
        showmsg(mb);
    }
    abortop = 0;
}

/* ---- rename and new ---------------------------------------------------- */

void fs_rename(struct place *p, int cur)
{
    int nmark, s, n, any = 0, stopped = 0, *pick;
    struct place *P = p;
    char tname[NAMEMAX + 4], src[CPATH], dst[CPATH], isrc[CPATH], idst[CPATH], itn[NAMEMAX + 8];
    char land[NAMEMAX + 4];
    if (pl_involume(p)) {
        showmsg("no file operations in the volume list");
        return;
    }
    if (P->fail) return;
    if (pl_inarc(p)) {
        /* inside an archive: the cursor entry, by a rebuild (an image: in place) */
        if (cur < 0) return;
        if (!arcw_canwrite(p) && !arcfs_isadf(p)) {
            arcfs_guard(p);
            return;
        }
        if (strlen(P->ent[cur].name) > NAMEMAX) {
            /* a member name runs to 255 (a zip from Linux) */
            showmsg("that name is too long to edit here");
            return;
        }
        strcpy(tname, P->ent[cur].name);
        if (!lineinput("rename to: ", tname, NAMEMAX, 1) || !tname[0] || !strcmp(tname, P->ent[cur].name)) {
            askdone();
            return;
        }
        if (strchr(tname, '/') || strchr(tname, ':')) {
            showmsg("plain names only");
            return;
        }
        if (nocase(tname, P->ent[cur].name) && arcw_has(p, tname)) {
            showmsg("that name already exists in the archive");
            return;
        }
        strcpy(src, P->ent[cur].name);
        if (arcfs_isadf(p)) arcfs_adfrename(p, src, tname);
        else arcw_rename(p, src, tname);
        return;
    }
    if (!(n = place_pick(p, cur, &pick))) return;
    nmark = place_marks(p);
    land[0] = 0;
    /* the marked set renames one at a time; Esc stops the rest, an
     * unchanged name skips that one */
    for (s = 0; s < n && !stopped; s++) {
        struct ent *e = &P->ent[pick[s]];
        if (strlen(e->name) > NAMEMAX) {
            showmsg("that name is too long to edit here");
            continue;
        }
        strcpy(tname, e->name);
        if (!lineinput("rename to: ", tname, NAMEMAX, 1)) {
            stopped = 1;
            continue;
        }
        if (!tname[0] || strcmp(tname, e->name) == 0) continue;    /* case changes are real */
        if (!buildfull(src, P->path, e->name) || !buildfull(dst, P->path, tname)) {
            showmsg("the path is too long");
            stopped = 1;
            continue;
        }
        if (!Rename((STRPTR)src, (STRPTR)dst)) {
            faultmsg("cannot rename");
            stopped = 1;
            continue;
        }
        any = 1;
        strcpy(land, tname);
        if (icons && !isinfo(e->name) && sidecarof(P->path, e->name, isrc)) {
            sfmt(itn, sizeof(itn), "%s.info", tname);
            if (!buildfull(idst, P->path, itn) || !Rename((STRPTR)isrc, (STRPTR)idst))
                showmsg("renamed - but its icon could not follow");
        }
    }
    xfree(pick);
    if (any) placechanged(p, nmark ? NULL : land, 0);
    else askdone();
}

/* entry i of a folder on a disk to the name to, its icon along: no
 * question, no reading again (bulk rename does both once). 1 = done,
 * else opmsg says why */
int fs_renameone(struct place *p, int i, const char *to)
{
    char src[CPATH], dst[CPATH], isrc[CPATH], idst[CPATH], itn[NAMEMAX + 8];
    struct ent *e = &p->ent[i];
    if (pl_involume(p) || pl_inarc(p) || i < 0 || i >= p->n) return 0;
    if (!to[0] || strchr(to, '/') || strchr(to, ':') || strlen(to) > NAMEMAX) {
        sfmt(opmsg, sizeof(opmsg), "not a name: %.60s", to);
        return 0;
    }
    if (!buildfull(src, p->path, e->name) || !buildfull(dst, p->path, to)) {
        sfmt(opmsg, sizeof(opmsg), "the path is too long");
        return 0;
    }
    /* the icon goes along: a different icon already on its new name
     * would leave it behind, so nothing moves */
    isrc[0] = idst[0] = 0;
    if (icons && !isinfo(e->name) && sidecarof(p->path, e->name, isrc)) {
        sfmt(itn, sizeof(itn), "%s.info", to);
        if (!buildfull(idst, p->path, itn)) idst[0] = 0;
        else if (nocase(e->name, to) != 0 && os_pathtype(idst) != 0) {
            sfmt(opmsg, sizeof(opmsg), "%.60s is already there", itn);
            return 0;
        }
    }
    if (!Rename((STRPTR)src, (STRPTR)dst)) {
        char fb[80];
        os_fault(IoErr(), fb, sizeof(fb));
        sfmt(opmsg, sizeof(opmsg), "cannot rename %.40s: %s", e->name, fb);
        return 0;
    }
    if (isrc[0] && idst[0]) Rename((STRPTR)isrc, (STRPTR)idst);
    return 1;
}

int fs_new(struct place *p, char *name, char *path)
{
    int l, i, wantdir = 0;
    char tname[NAMEMAX + 4], dpath[CPATH];
    if (pl_involume(p)) {
        showmsg("no file operations in the volume list");
        return 0;
    }
    if (pl_inarc(p) && !arcw_canwrite(p) && !arcfs_isadf(p) && arcfs_guard(p)) return 0;
    if (p->fail) return 0;
    tname[0] = 0;
    if (!lineinput("new (name/ = dir): ", tname, NAMEMAX + 1, 0) || !tname[0]) {
        askdone();
        return 0;
    }
    l = strlen(tname);
    if (tname[l - 1] == '/') tname[--l] = 0, wantdir = 1;
    for (i = 0; i < l; i++)
        if (tname[i] == '/' || tname[i] == ':') {
            showmsg("plain names only");
            return 0;
        }
    if (!l) {
        askdone();
        return 0;
    }
    strcpy(name, tname);
    if (pl_inarc(p) && !wantdir) {
        if (arcw_has(p, tname)) showmsg("that name already exists - e edits it");
        else return FS_NEWMEMBER;
        return 0;
    }
    if (wantdir) {
        fs_mkdir(p, tname);
        return 0;
    }
    if (!buildfull(dpath, p->path, tname)) {
        showmsg("the path is too long");
        return 0;
    }
    if (!wantdir && l > 4 && nocase(tname + l - 4, ".adf") == 0) {
        /* a blank disk image, named after the file */
        if (os_pathtype(dpath)) {
            showmsg("that name already exists");
            return 0;
        }
        if (arcfs_newadf(dpath)) {
            placechanged(p, tname, 0);
            showmsg("a blank 880K disk - Enter opens it");
        } else {
            showmsg(opmsg);
        }
        return 0;
    }
    if (!wantdir) {
        /* a file: the editor on it; it exists only once saved */
        if (os_pathtype(dpath)) {
            showmsg("that name already exists - e edits it");
            return 0;
        }
        strcpy(path, dpath);
        return FS_NEWFILE;
    }
    return 0;
}

int fs_mkdir(struct place *p, const char *name)
{
    char dpath[CPATH];
    BPTR lk;
    if (pl_involume(p) || p->fail || !name[0] || strchr(name, '/') || strchr(name, ':')) return 0;
    if (pl_inarc(p)) {
        if (!arcw_canwrite(p) && !arcfs_isadf(p)) {
            arcfs_guard(p);
            return 0;
        }
        if (arcw_has(p, name)) {
            showmsg("that name already exists in the archive");
            return 0;
        }
        if (arcfs_isadf(p)) arcfs_adfmkdir(p, name);
        else arcw_mkdir(p, name);
        return arcw_has(p, name) == 2;
    }
    if (!buildfull(dpath, p->path, name)) {
        showmsg("the path is too long");
        return 0;
    }
    if (!(lk = CreateDir((STRPTR)dpath))) {
        faultmsg("cannot create");
        return 0;
    }
    UnLock(lk);
    placechanged(p, name, 0);
    return 1;
}

/* ---- measure a directory ('=') ----------------------------------------- */

int fs_measure(struct place *p, int i)
{
    struct ent *e;
    char path[CPATH];
    if (pl_involume(p) || p->fail || i < 0) return 0;
    e = &p->ent[i];
    if (e->kind == KD_FILE) return 0;       /* a file shows its size already */
    if (!buildfull(path, p->path, e->name)) return 0;
    showmsg("measuring - Esc stops");
    statbytes = statfiles = 0;
    cancelok = 1;
    abortop = 0;
    if (pl_inarc(p)) statbytes = arcfs_treesize(p, e->name);
    else treestat(path, 0);
    cancelok = 0;
    if (abortop) {
        abortop = 0;
        showmsg("measuring stopped");
        return 0;
    }
    askdone();
    e->size = statbytes;
    return 1;
}
