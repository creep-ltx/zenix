/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* arcfs - see arcfs.h. The readers in core/ do the formats; this file
 * gives them an AmigaDOS file to read, memory, and a place to write.
 *
 * lha, lzx and zip are flat lists of stored paths: the list is read
 * once on entering and each directory is a prefix of it. ADF and ISO
 * are real trees: each directory is read from the image when shown,
 * so a 650 MB CD costs no more memory than a floppy.
 */
#include <string.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/memory.h>
#include "arcfs.h"
#include "place.h"
#include "fe.h"
#include "os.h"
#include "ops.h"
#include "../core/core.h"
#include "../core/util.h"
#include "../core/arc.h"
#include "../core/lha.h"
#include "../core/lzx.h"
#include "../core/zip.h"
#include "../core/adf.h"
#include "../core/iso.h"
#include "../core/dms.h"
#include "../core/adfw.h"
#include "arcw.h"

#define MAXDEPTH 20

/* a member of a flat archive */
struct mem {
    char *name, *note;
    ULONG size, packed, dataoff, aux1, aux2, aux3, prot;
    LONG days, mins, ticks;
    WORD method;
    UBYTE isdir;
    int hnext;                  /* the next member in its hash chain, -1 none */
};

/* one entry of a directory, whatever the format */
struct kid {
    char *name, *note;
    ULONG size, prot;
    LONG days, mins, ticks;
    UBYTE isdir;
    UBYTE link;                 /* an ADF hard or soft link: never walked into to delete */
    ULONG dataoff;              /* an ISO file's extent: read without a search from the root */
};

struct kids {
    struct kid *k;
    int n, cap;
    struct pool *pl;
    int full;                   /* an add failed: the list is not whole */
};

/* the last directory kidof listed, while a session is held */
static struct kids kc;
static struct ast *kcp;
static char kcdir[CPATH];
static void kidsfree(struct kids *K);

static void kcfree(void)
{
    kidsfree(&kc);
    kcp = NULL;
}

struct fsrc {
    BPTR fh;
    LONG pos;
};

static struct ast {
    int kind;
    int damaged;                /* the listing stopped early: read-only */
    char file[CPATH];
    struct mem *m;
    int nm, cap;
    int mhead[128];             /* member names by hash: a lookup per extract
                                   was O(n) - a rebuild O(n^2) (the audit) */
    struct pool *pl;
    /* while a session is open (begin..end) */
    struct fsrc fs;
    struct arcsrc src;
    struct adfvol adf;
    struct isovol iso;
    int held;                   /* a session kept open (see begin) */
    struct place *owner;
    struct ast *next;           /* every archive some place stands in */
} *alist;

static const char *knames[] = { "", "lha", "lzx", "zip", "ADF", "ISO", "DMS" };

/* ---- the source: an AmigaDOS file, a Seek only when not already there - */

static long fsread(void *ctx, unsigned long off, void *buf, long n)
{
    struct fsrc *f = ctx;
    LONG r;
    if (f->pos != (LONG)off) {
        if (Seek(f->fh, (LONG)off, OFFSET_BEGINNING) < 0) {
            f->pos = -1;
            return -1;
        }
        f->pos = (LONG)off;
    }
    r = Read(f->fh, buf, n);
    f->pos = r < 0 ? -1 : f->pos + r;
    return r;
}

/* a held session stays open across many copyout calls (a copy of many
 * entries): lzx keeps its decoder, a directory is listed once - opening
 * per entry re-unpacked a merged lzx group for every file (the audit) */

static int begin(struct place *p)
{
    struct ast *A = p->as;
    int r = ARC_OK;
    if (A->held && A->fs.fh) return ARC_OK;
    if (!(A->fs.fh = Open((STRPTR)A->file, MODE_OLDFILE))) return ARC_EREAD;
    Seek(A->fs.fh, 0, OFFSET_END);
    A->src.size = (ULONG)Seek(A->fs.fh, 0, OFFSET_BEGINNING);
    A->fs.pos = 0;
    A->src.read = fsread;
    A->src.ctx = &A->fs;
    if (A->kind == AK_ADF) r = adf_open(&A->src, &A->adf);
    else if (A->kind == AK_ISO) r = iso_open(&A->src, &A->iso);
    if (r) {
        Close(A->fs.fh);
        A->fs.fh = 0;
    }
    return r;
}

static void end(struct place *p)
{
    struct ast *A = p->as;
    if (A->held || !A->fs.fh) return;
    if (A->kind == AK_ADF) adf_close(&A->adf);
    else if (A->kind == AK_ISO) iso_close(&A->iso);
    else if (A->kind == AK_LZX) lzx_end();
    Close(A->fs.fh);
    A->fs.fh = 0;
}

/* ---- what a file is ------------------------------------------------- */

const char *arcfs_kindname(struct place *p)
{
    return knames[p->as ? p->as->kind : AK_NONE];
}

/* ---- the member list of a flat archive -------------------------------- */

static unsigned mhash(const char *s)
{
    unsigned h = 0;
    int c;
    while ((c = (unsigned char)*s++)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
        h = h * 31 + c;
    }
    return h & 127;
}

static void memfree(struct ast *A)
{
    int i;
    for (i = 0; i < 128; i++) A->mhead[i] = -1;
    xfree(A->m);
    A->m = NULL;
    A->nm = A->cap = 0;
    pool_free(&A->pl);
}

static int memadd(void *ctx, const struct arcent *e)
{
    struct ast *A = ctx;
    struct mem *m;
    if (A->nm == A->cap) {
        int nc = A->cap ? A->cap * 2 : 128;
        struct mem *nmv = xalloc(nc * sizeof(struct mem));
        /* out of memory: the list is cut, and the readers report that
         * as a normal end - so it is marked here, or a rebuild from it
         * would drop every member after the cut (the audit) */
        if (!nmv) return A->damaged = 1, 0;
        if (A->nm) memcpy(nmv, A->m, A->nm * sizeof(struct mem));
        xfree(A->m);
        A->m = nmv;
        A->cap = nc;
    }
    m = &A->m[A->nm];
    if (!(m->name = pool_str(&A->pl, e->name))) return A->damaged = 1, 0;
    m->note = e->comment[0] ? pool_str(&A->pl, e->comment) : NULL;
    m->size = e->size;
    m->packed = e->packed;
    m->dataoff = e->dataoff;
    m->aux1 = e->aux1;
    m->aux2 = e->aux2;
    m->aux3 = e->aux3;
    m->prot = e->prot;
    m->days = e->days;
    m->mins = e->mins;
    m->ticks = e->ticks;
    m->method = (WORD)e->method;
    m->isdir = e->isdir;
    {
        unsigned h = mhash(m->name);
        m->hnext = A->mhead[h];
        A->mhead[h] = A->nm;
    }
    A->nm++;
    return 1;
}

static int flat(int k) { return k == AK_LHA || k == AK_LZX || k == AK_ZIP; }

static struct mem *mfind(struct ast *A, const char *rel)
{
    int i;
    for (i = A->nm ? A->mhead[mhash(rel)] : -1; i >= 0 && i < A->nm; i = A->m[i].hnext)
        if (nocase(A->m[i].name, rel) == 0) return &A->m[i];
    return NULL;
}

/* n chars of a and b equal, without case */
static int nceq(const char *a, const char *b, int n)
{
    while (n-- > 0) {
        int x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

/* ---- one directory's entries ------------------------------------------ */

static void kidsfree(struct kids *K)
{
    xfree(K->k);
    K->k = NULL;
    K->n = K->cap = 0;
    pool_free(&K->pl);
}

static int kidadd(struct kids *K, const char *name, int isdir, ULONG size, ULONG prot,
                  LONG days, LONG mins, LONG ticks, const char *note)
{
    struct kid *k;
    if (K->n == K->cap) {
        int nc = K->cap ? K->cap * 2 : 64;
        struct kid *nk = xalloc(nc * sizeof(struct kid));
        if (!nk) return K->full = 1, 0;
        if (K->n) memcpy(nk, K->k, K->n * sizeof(struct kid));
        xfree(K->k);
        K->k = nk;
        K->cap = nc;
    }
    k = &K->k[K->n];
    if (!(k->name = pool_str(&K->pl, name))) return K->full = 1, 0;
    k->note = note && note[0] ? pool_str(&K->pl, note) : NULL;
    k->isdir = (UBYTE)isdir;
    k->size = isdir ? 0 : size;
    k->prot = prot;
    k->days = days;
    k->mins = mins;
    k->ticks = ticks;
    K->n++;
    return 1;
}

static int kidcb(void *ctx, const struct arcent *e)
{
    struct kids *K = ctx;
    if (!kidadd(K, e->name, e->isdir, e->size, e->prot, e->days, e->mins, e->ticks, e->comment)) return 0;
    /* kidcb lists ADF and ISO trees only; ISO's method is always 0 */
    K->k[K->n - 1].link = e->method == ADF_HLINK || e->method == ADF_SLINK;
    K->k[K->n - 1].dataoff = e->dataoff;
    return 1;
}

/* the entries of rel ("" = the top); a session must be open for trees */
static int collect(struct place *p, const char *rel, struct kids *K)
{
    struct ast *A = p->as;
    int i, j, sl = strlen(rel), cl;
    char comp[256];
    if (A->kind == AK_ADF || A->kind == AK_ISO) {
        int r = A->kind == AK_ADF ? adf_list(&A->adf, rel, kidcb, K) : iso_list(&A->iso, rel, kidcb, K);
        /* a listing cut by memory reads as a normal end: say it */
        return r ? r : K->full ? ARC_ENOMEM : ARC_OK;
    }
    for (i = 0; i < A->nm; i++) {
        struct mem *m = &A->m[i];
        const char *n = m->name, *sl2;
        if (sl) {
            if (!nceq(n, rel, sl) || n[sl] != '/') continue;
            n += sl + 1;
        }
        if (!*n) continue;
        if ((sl2 = strchr(n, '/'))) {
            /* deeper down: its first part is a directory here, once */
            cl = sl2 - n;
            if (cl > 255) cl = 255;
            memcpy(comp, n, cl);
            comp[cl] = 0;
            for (j = 0; j < K->n; j++)
                if (K->k[j].isdir && nocase(K->k[j].name, comp) == 0) break;
            if (j == K->n && !kidadd(K, comp, 1, 0, 0, 0, 0, 0, NULL)) return ARC_ENOMEM;
        } else if (m->isdir) {
            for (j = 0; j < K->n; j++)
                if (K->k[j].isdir && nocase(K->k[j].name, n) == 0) break;
            if (j < K->n) {
                /* named earlier by a deeper path: now it has a date */
                K->k[j].days = m->days;
                K->k[j].mins = m->mins;
                K->k[j].prot = m->prot;
            } else if (!kidadd(K, n, 1, 0, m->prot, m->days, m->mins, m->ticks, m->note)) {
                return ARC_ENOMEM;
            }
        } else if (!kidadd(K, n, 0, m->size, m->prot, m->days, m->mins, m->ticks, m->note)) {
            return ARC_ENOMEM;
        }
    }
    return ARC_OK;
}

/* where place p is inside its archive ("" = the top) */
static const char *subof(struct place *p)
{
    const char *s;
    s = p->path + p->arc;
    return *s == '/' ? s + 1 : s;
}

/* name of pane p's directory as a path from the archive's top */
static int relof(struct place *p, const char *name, char *rel)
{
    const char *s = subof(p);
    if (strlen(s) + strlen(name) + 2 > CPATH) return 0;
    if (*s) sfmt(rel, CPATH, "%s/%s", s, name);
    else strcpy(rel, name);
    return 1;
}

/* ---- entering, leaving, reading ---------------------------------------- */

void arcfs_leave(struct place *p)
{
    struct ast **a;
    p->arc = 0;
    if (!p->as) return;
    arcfs_release(p);
    memfree(p->as);
    for (a = &alist; *a; a = &(*a)->next)
        if (*a == p->as) {
            *a = p->as->next;
            break;
        }
    xfree(p->as);
    p->as = NULL;
}

static void dmsunpack(struct place *p, const char *file);

/* read an archive's member list (flat) or check an image opens (trees) */
static int load(struct place *p, const char *file, int k)
{
    struct ast *A = p->as;
    int r;
    if (!A) {
        /* the place's own archive state, from its first archive on */
        if (!(A = p->as = xalloc(sizeof(struct ast)))) return ARC_ENOMEM;
        A->owner = p;
        A->next = alist;
        alist = A;
    }
    memfree(A);
    A->kind = k;
    A->damaged = 0;                     /* memadd may set it while listing */
    if (A->file != file) strcpy(A->file, file);
    if ((r = begin(p)) == ARC_OK) {
        if (k == AK_LHA) r = lha_list(&A->src, memadd, A);
        else if (k == AK_LZX) r = lzx_list(&A->src, memadd, A);
        else if (k == AK_ZIP) r = zip_list(&A->src, memadd, A);
        end(p);
    }
    if (r && flat(k)) A->damaged = 1;
    return r;
}

int arcfs_load(struct place *p, const char *file)
{
    int k = arcfs_kind(file), r;
    if (k == AK_NONE || k == AK_DMS) return ARC_EUNSUP;
    if (strlen(file) + 2 > CPATH) return ARC_EUNSUP;
    r = load(p, file, k);
    if (r && (!p->as || !flat(k) || p->as->nm == 0)) {
        arcfs_leave(p);
        return r;
    }
    return ARC_OK;
}

int arcfs_reload(struct place *p)
{
    arcfs_release(p);
    return load(p, p->as->file, p->as->kind);
}

const char *arcfs_file(struct place *p) { return p->as->file; }
int arcfs_kindof(struct place *p) { return p->as ? p->as->kind : AK_NONE; }
const char *arcfs_sub(struct place *p) { return subof(p); }
int arcfs_nmem(struct place *p) { return p->as->nm; }

const char *arcfs_mem(struct place *p, int i, ULONG *size, int *isdir)
{
    *size = p->as->m[i].size;
    *isdir = p->as->m[i].isdir;
    return p->as->m[i].name;
}

/* an archive file's members, without a pane (the writer's check) */
int arcfs_listfile(const char *file, int kind, arc_addfn add, void *ctx)
{
    struct fsrc fs;
    struct arcsrc src;
    int r;
    if (!(fs.fh = Open((STRPTR)file, MODE_OLDFILE))) return ARC_EREAD;
    Seek(fs.fh, 0, OFFSET_END);
    src.size = (ULONG)Seek(fs.fh, 0, OFFSET_BEGINNING);
    fs.pos = 0;
    src.read = fsread;
    src.ctx = &fs;
    if (kind == AK_LHA) r = lha_list(&src, add, ctx);
    else if (kind == AK_LZX) r = lzx_list(&src, add, ctx);
    else if (kind == AK_ZIP) r = zip_list(&src, add, ctx);
    else r = ARC_EUNSUP;
    Close(fs.fh);
    return r;
}

int arcfs_open(struct place *p, const char *file)
{
    struct ast *A;
    int k = arcfs_kind(file), r = ARC_OK;
    char mb[120];
    if (k == AK_NONE) return 0;
    if (k == AK_DMS) {
        dmsunpack(p, file);
        return 1;
    }
    if (strlen(file) + 2 > CPATH) {
        showmsg("the path is too long");
        return 1;
    }
    arcfs_leave(p);
    r = load(p, file, k);
    A = p->as;
    if (r && (!A || !flat(k) || A->nm == 0)) {
        if (k == AK_ADF && r == ARC_EUNSUP) showmsg("not a DOS disk - nothing to list");
        else if (k == AK_ADF || k == AK_ISO) sfmt(mb, sizeof(mb), "cannot read the image: %s", arc_errstr(r)), showmsg(mb);
        else sfmt(mb, sizeof(mb), "cannot read the archive: %s", arc_errstr(r)), showmsg(mb);
        arcfs_leave(p);
        return 1;
    }
    strcpy(p->path, file);
    p->arc = strlen(file);
    if (r) {
        /* what could be read is shown; the rest is said */
        sfmt(mb, sizeof(mb), "the archive is damaged after %d entries", A->nm);
        showmsg(mb);
    }
    return 1;
}

int arcfs_read(struct place *p)
{
    struct ast *A = p->as;
    struct place *P = p;
    struct kids K;
    int r, i;
    /* gone somewhere else (a jump, the parent of the archive)? */
    if (!A || A->kind == AK_NONE || (int)strlen(P->path) < P->arc || !nceq(P->path, A->file, P->arc) ||
        (P->path[P->arc] && P->path[P->arc] != '/')) {
        arcfs_leave(p);
        return 1;
    }
    memset(&K, 0, sizeof(K));
    if (!flat(A->kind) && (r = begin(p))) return r;
    r = collect(p, subof(p), &K);
    end(p);
    for (i = 0; i < K.n; i++) {
        struct kid *k = &K.k[i];
        if (!place_add(p, k->name, k->isdir ? KD_DIR : KD_FILE, k->size, k->days * 1440 + k->mins)) break;
        p->ent[p->n - 1].prot = k->prot;
    }
    kidsfree(&K);
    return r < 0 ? r : 0;
}

/* ---- writing members out -------------------------------------------- */

struct fsink {
    BPTR fh;
    LONG err;
};

static int fswrite(void *ctx, const void *buf, long n)
{
    struct fsink *s = ctx;
    if (checkabort()) return 0;
    if (Write(s->fh, (APTR)buf, n) != n) {
        s->err = IoErr();
        if (!s->err) s->err = ERROR_DISK_FULL;
        return 0;
    }
    progadd(n);
    return 1;
}

/* one member to the sink; a session is open */
static int extract(struct place *p, const char *rel, struct arcsink *out, const struct kid *k)
{
    struct ast *A = p->as;
    struct arcent e;
    struct mem *m;
    int r;
    if (A->kind == AK_ADF) return adf_extract(&A->adf, rel, out);
    if (A->kind == AK_ISO && k && k->dataoff) {
        /* the listing gave its extent: no walk from the root per file */
        memset(&e, 0, sizeof(e));
        e.dataoff = k->dataoff;
        e.size = k->size;
        return iso_extract(&A->iso, &e, out);
    }
    if (A->kind == AK_ISO) {
        if ((r = iso_find(&A->iso, rel, &e))) return r;
        return iso_extract(&A->iso, &e, out);
    }
    if (!(m = mfind(A, rel))) return ARC_ECORRUPT;
    memset(&e, 0, sizeof(e));
    sfmt(e.name, sizeof(e.name), "%s", m->name);
    e.size = m->size;
    e.packed = m->packed;
    e.dataoff = m->dataoff;
    e.aux1 = m->aux1;
    e.aux2 = m->aux2;
    e.aux3 = m->aux3;
    e.method = m->method;
    e.prot = m->prot;
    if (A->kind == AK_LHA) return lha_extract(&A->src, &e, out);
    if (A->kind == AK_LZX) return lzx_extract(&A->src, &e, out);
    return zip_extract(&A->src, &e, out);
}

/* a member written to dst the safe way: beside, then swapped in */
static int outfile(struct place *p, const char *rel, const char *dst, const struct kid *k)
{
    char tmp[CPATH], fb[80];
    struct fsink fs;
    struct arcsink sk;
    struct DateStamp ds;
    int swap = os_pathtype(dst) == 1, r, ok;
    if (swap) sidename(tmp, dst, 'n');
    else strcpy(tmp, dst);
    if (!(fs.fh = Open((STRPTR)tmp, MODE_NEWFILE))) {
        os_fault(IoErr(), fb, sizeof(fb));
        sfmt(opmsg, sizeof(opmsg), "cannot write the target: %s", fb);
        return 0;
    }
    fs.err = 0;
    sk.write = fswrite;
    sk.ctx = &fs;
    r = extract(p, rel, &sk, k);
    ok = r == ARC_OK;
    if (v_dos >= 36) {
        if (!Close(fs.fh) && ok) ok = 0, fs.err = IoErr(), r = ARC_EREAD;
    } else {
        Close(fs.fh);
    }
    if (ok && swap && !swapin(tmp, dst, 0)) {
        DeleteFile((STRPTR)tmp);
        sfmt(opmsg, sizeof(opmsg), "cannot replace the target");
        return 0;
    }
    if (!ok) {
        DeleteFile((STRPTR)tmp);            /* no partial targets */
        if (fs.err) {
            os_fault(fs.err, fb, sizeof(fb));
            sfmt(opmsg, sizeof(opmsg), "copy failed: %s", fb);
        } else {
            sfmt(opmsg, sizeof(opmsg), "%s: %s", k->name, arc_errstr(r));
        }
        return 0;
    }
    SetProtection((STRPTR)dst, k->prot);
    if (k->days || k->mins) {
        ds.ds_Days = k->days;
        ds.ds_Minute = k->mins;
        ds.ds_Tick = k->ticks;
        os_setdate(dst, &ds);
    }
    if (k->note) SetComment((STRPTR)dst, (STRPTR)k->note);
    return 1;
}

/* what the listing says about one entry (for its bits, date, note) */
static int kidof(struct place *p, const char *rel, struct kids *K, struct kid **kp)
{
    char dir[CPATH];
    const char *nm = rel + pathlen(rel);
    int i, r, n = pathlen(rel);
    memcpy(dir, rel, n);
    dir[n] = 0;
    if (n && dir[n - 1] == '/') dir[n - 1] = 0;
    if (*nm == '/') nm++;
    if (p->as->held) {
        /* a held session: the directory's list is kept for the next entry */
        if (kcp != p->as || strcmp(kcdir, dir)) {
            kcfree();
            if ((r = collect(p, dir, &kc))) {
                kcfree();
                return r;
            }
            kcp = p->as;
            strcpy(kcdir, dir);
        }
        K = &kc;
    } else if ((r = collect(p, dir, K))) {
        return r;
    }
    for (i = 0; i < K->n; i++)
        if (nocase(K->k[i].name, nm) == 0) {
            *kp = &K->k[i];
            return ARC_OK;
        }
    return ARC_ECORRUPT;
}

/* a member's name as a name on disk: ':' and '/' (a zip from Linux, a
 * crafted CD) would reach another folder or volume; empty, "." and
 * ".." name nothing to write (0 = skip it) */
static int safename(char *d, const char *s)
{
    int n;
    for (n = 0; s[n] && n < 107; n++) d[n] = s[n] == ':' || s[n] == '/' ? '_' : s[n];
    d[n] = 0;
    return n && strcmp(d, ".") && strcmp(d, "..");
}

static int outtree(struct place *p, const char *rel, const char *dst, int depth)
{
    struct kids K;
    char *cr, *cd;
    BPTR lk;
    int i, ok = 1, r;
    if (depth > MAXDEPTH) {
        sfmt(opmsg, sizeof(opmsg), "directory tree too deep");
        return 0;
    }
    memset(&K, 0, sizeof(K));
    if ((r = collect(p, rel, &K))) {
        kidsfree(&K);
        sfmt(opmsg, sizeof(opmsg), "cannot read the archive: %s", arc_errstr(r));
        return 0;
    }
    if ((lk = CreateDir((STRPTR)dst))) {
        UnLock(lk);
    } else if (os_pathtype(dst) != 2) {
        kidsfree(&K);
        sfmt(opmsg, sizeof(opmsg), "cannot create the target directory");
        return 0;
    }
    cr = xalloc(CPATH);
    cd = xalloc(CPATH);
    if (!cr || !cd) ok = 0, sfmt(opmsg, sizeof(opmsg), "not enough memory");
    for (i = 0; ok && i < K.n && !abortop; i++) {
        struct kid *k = &K.k[i];
        char sn[110];
        if (!safename(sn, k->name)) continue;
        if (strlen(rel) + strlen(k->name) + 2 > CPATH || !buildfull(cd, dst, sn)) {
            sfmt(opmsg, sizeof(opmsg), "the path is too long");
            ok = 0;
            break;
        }
        if (*rel) sfmt(cr, CPATH, "%s/%s", rel, k->name);
        else strcpy(cr, k->name);
        ok = k->isdir ? outtree(p, cr, cd, depth + 1) : outfile(p, cr, cd, k);
    }
    xfree(cr);
    xfree(cd);
    kidsfree(&K);
    return ok && !abortop;
}

int arcfs_copyout(struct place *p, const char *name, const char *dst, int isdir)
{
    char rel[CPATH];
    struct kids K;
    struct kid *k;
    int r, ok = 0;
    if (!relof(p, name, rel)) {
        sfmt(opmsg, sizeof(opmsg), "the path is too long");
        return 0;
    }
    if ((r = begin(p))) {
        sfmt(opmsg, sizeof(opmsg), "cannot read the archive: %s", arc_errstr(r));
        return 0;
    }
    if (isdir) {
        ok = outtree(p, rel, dst, 0);
    } else {
        memset(&K, 0, sizeof(K));
        if ((r = kidof(p, rel, &K, &k))) sfmt(opmsg, sizeof(opmsg), "%s: %s", name, arc_errstr(r));
        else ok = outfile(p, rel, dst, k);
        kidsfree(&K);
    }
    end(p);
    return ok;
}

static ULONG treesize(struct place *p, const char *rel, int depth)
{
    struct kids K;
    char *cr;
    ULONG t = 0;
    int i;
    if (depth > MAXDEPTH) return 0;
    memset(&K, 0, sizeof(K));
    if (collect(p, rel, &K) == ARC_OK && (cr = xalloc(CPATH))) {
        for (i = 0; i < K.n; i++) {
            if (!K.k[i].isdir) {
                t += K.k[i].size;
            } else if (strlen(rel) + strlen(K.k[i].name) + 2 <= CPATH) {
                if (*rel) sfmt(cr, CPATH, "%s/%s", rel, K.k[i].name);
                else strcpy(cr, K.k[i].name);
                t += treesize(p, cr, depth + 1);
            }
        }
        xfree(cr);
    }
    kidsfree(&K);
    return t;
}

/* the whole archive into dst (an existing folder): the writer's rebuild */
int arcfs_extractall(struct place *p, const char *dst)
{
    int r, ok;
    if ((r = begin(p))) {
        sfmt(opmsg, sizeof(opmsg), "cannot read the archive: %s", arc_errstr(r));
        return 0;
    }
    ok = outtree(p, "", dst, 0);
    end(p);
    return ok;
}

ULONG arcfs_treesize(struct place *p, const char *name)
{
    char rel[CPATH];
    ULONG t;
    if (!relof(p, name, rel) || begin(p)) return 0;
    t = treesize(p, rel, 0);
    end(p);
    return t;
}

int arcfs_temp(struct place *p, const char *name, char *tmp)
{
    char rel[CPATH];
    struct kids K;
    struct kid *k;
    const char *where;
    int ok = 0, r;
    /* T: when it is there, else RAM: - asked quietly (1.3 may have neither) */
    os_quiet(1);
    where = os_pathtype("T:") == 2 ? "T:" : os_pathtype("RAM:") == 2 ? "RAM:" : NULL;
    os_quiet(0);
    if (!where) {
        showmsg("no T: or RAM: to unpack into for viewing");
        return 0;
    }
    sfmt(tmp, CPATH, "%sCFileV%lx", where, (ULONG)FindTask(NULL));
    if (!relof(p, name, rel)) return 0;
    if ((r = begin(p))) {
        sfmt(opmsg, sizeof(opmsg), "cannot read the archive: %s", arc_errstr(r));
        showmsg(opmsg);
        return 0;
    }
    memset(&K, 0, sizeof(K));
    if ((r = kidof(p, rel, &K, &k)) == ARC_OK) {
        cancelok = 1;
        abortop = 0;
        ok = outfile(p, rel, tmp, k);
        cancelok = 0;
        if (!ok && !abortop) showmsg(opmsg);
        abortop = 0;
    }
    kidsfree(&K);
    end(p);
    return ok;
}

int arcfs_guard(struct place *p)
{
    if (!p->arc) return 0;
    if (p->as->damaged) showmsg("this archive is damaged - CFile reads what it can, but will not write to it");
    else if (p->as->kind == AK_ISO) showmsg("a CD image is read-only - copy the files out first");
    else showmsg("this archive cannot be written - copy the files out first");
    return 1;
}

/* ---- DMS: a disk archive becomes an ADF beside it, then opens ---------- */

static int dmstrack(void *ctx, int t)
{
    (void)ctx;
    (void)t;
    return !checkabort();
}

static void dmsunpack(struct place *p, const char *file)
{
    int r, swap, ok;
    struct arcsrc src;
    struct fsrc fsr;
    struct dmsinfo di;
    struct fsink fs;
    struct arcsink sk;
    char name[NAMEMAX + 8], dst[CPATH], tmp[CPATH], mb[160], fb[80];
    const char *base = file + pathlen(file);
    int l;
    if (*base == '/') base++;
    sfmt(name, sizeof(name), "%s", base);
    l = strlen(name);
    if (l > 4 && name[l - 4] == '.') name[l - 4] = 0;
    sfmt(name + strlen(name), 8, ".adf");
    if (!buildfull(dst, p->path, name)) {
        showmsg("the path is too long");
        return;
    }
    if (!(fsr.fh = Open((STRPTR)file, MODE_OLDFILE))) {
        showmsg("cannot read the DMS file");
        return;
    }
    Seek(fsr.fh, 0, OFFSET_END);
    src.size = (ULONG)Seek(fsr.fh, 0, OFFSET_BEGINNING);
    fsr.pos = 0;
    src.read = fsread;
    src.ctx = &fsr;
    if ((r = dms_info(&src, &di)) || di.encrypted || di.fms) {
        Close(fsr.fh);
        showmsg(r ? "not a readable DMS file" : di.encrypted ? "the DMS file is locked with a password" : "an FMS file archive, not a disk");
        return;
    }
    sfmt(mb, sizeof(mb), "unpack the disk to %s? (y)es (n)o", name);
    if (os_pathtype(dst)) sfmt(mb, sizeof(mb), "%s exists - replace it with the unpacked disk? (y)es (n)o", name);
    if ((askkey(mb) | 32) != 'y') {
        Close(fsr.fh);
        return;
    }
    if (os_pathtype(dst) == 2) {
        Close(fsr.fh);
        showmsg("a directory with that name is in the way");
        return;
    }
    swap = os_pathtype(dst) == 1;
    if (swap) sidename(tmp, dst, 'n');
    else strcpy(tmp, dst);
    if (!(fs.fh = Open((STRPTR)tmp, MODE_NEWFILE))) {
        Close(fsr.fh);
        os_fault(IoErr(), fb, sizeof(fb));
        sfmt(mb, sizeof(mb), "cannot write the target: %s", fb);
        showmsg(mb);
        return;
    }
    fs.err = 0;
    sk.write = fswrite;
    sk.ctx = &fs;
    cancelok = 1;
    abortop = 0;
    progshow(di.diskhd ? 1802240UL : 901120UL);
    r = dms_unpack(&src, &sk, dmstrack, NULL);
    ok = r == ARC_OK;
    Close(fsr.fh);
    if (v_dos >= 36) {
        if (!Close(fs.fh) && ok) ok = 0, fs.err = IoErr();
    } else {
        Close(fs.fh);
    }
    if (ok && swap && !swapin(tmp, dst, 0)) ok = 0;
    if (!ok) DeleteFile((STRPTR)tmp);
    cancelok = 0;
    progoff();
    placechanged(p, name, 2);
    if (ok && !abortop) {
        /* the disk opens at once; a non-DOS one stays as an .adf, said so */
        abortop = 0;
        if (arcfs_open(p, dst) && p->arc) {
            placechanged(p, "", 0);
            sfmt(mb, sizeof(mb), "unpacked to %s", name);
            showmsg(mb);
        }
        return;
    }
    if (abortop) {
        showmsg("cancelled - no disk image was kept");
    } else if (!ok) {
        if (fs.err) os_fault(fs.err, fb, sizeof(fb)), sfmt(mb, sizeof(mb), "unpack failed: %s", fb);
        else sfmt(mb, sizeof(mb), "unpack failed: %s", arc_errstr(r));
        showmsg(mb);
    } else {
        sfmt(mb, sizeof(mb), "unpacked to %s - Enter opens it", name);
        showmsg(mb);
    }
    abortop = 0;
}

/* ---- writing ADF images ------------------------------------------------
 * The writer (core/adfw.c) changes the volume in a crash-safe order; this
 * gives it the image file to write and the files to put in. An image
 * is changed in place, one change at a time. */

static long imgwrite(void *ctx, unsigned long off, const void *buf, long n)
{
    struct fsrc *f = ctx;
    LONG r;
    if (f->pos != (LONG)off) {
        if (Seek(f->fh, (LONG)off, OFFSET_BEGINNING) < 0) {
            f->pos = -1;
            return -1;
        }
        f->pos = (LONG)off;
    }
    r = Write(f->fh, (APTR)buf, n);
    f->pos = r < 0 ? -1 : f->pos + r;
    return r;
}

static const char *aerr(int r)
{
    return r <= ADF_EWRITE ? adf_errstr(r) : arc_errstr(r);
}

int arcfs_isadf(struct place *p)
{
    return p->arc && p->as && p->as->kind == AK_ADF;
}

/* open pane p's image for changing: 0, or a message in opmsg */
static int beginw(struct place *p)
{
    struct ast *A = p->as;
    struct FileInfoBlock *fib;
    struct DateStamp ds;
    BPTR l;
    int r, prot = 0;
    if ((fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) {
        if ((l = Lock((STRPTR)A->file, ACCESS_READ))) {
            if (Examine(l, fib)) prot = fib->fib_Protection & FIBF_WRITE;
            UnLock(l);
        }
        FreeMem(fib, sizeof(*fib));
    }
    if (prot) {
        sfmt(opmsg, sizeof(opmsg), "the disk image is write-protected");
        return -1;
    }
    arcfs_release(p);                   /* a held read session, and its kept list */
    kcfree();
    if ((r = begin(p))) {
        sfmt(opmsg, sizeof(opmsg), "cannot open the image: %s", aerr(r));
        return r;
    }
    if ((r = adf_writable(&A->adf, imgwrite, &A->fs))) {
        if (r == ARC_EUNSUP)
            sfmt(opmsg, sizeof(opmsg), "CFile reads this disk but cannot write it (dircache, long names or a very big partition)");
        else if (r == ADF_EVALIDATE)
            sfmt(opmsg, sizeof(opmsg), "the disk needs validating first - nothing was changed");
        else
            sfmt(opmsg, sizeof(opmsg), "cannot write the image: %s", aerr(r));
        end(p);
        return r;
    }
    DateStamp(&ds);
    adf_now(&A->adf, ds.ds_Days, ds.ds_Minute, ds.ds_Tick);
    return 0;
}

LONG arcfs_free(struct place *p)
{
    struct ast *A = p->as;
    LONG n = -1;
    if (!arcfs_isadf(p) || begin(p)) return -1;
    if (adf_writable(&A->adf, imgwrite, &A->fs) == 0) n = (LONG)adf_freebytes(&A->adf);
    end(p);
    return n;
}

struct fget {
    BPTR fh;
    LONG err;
};

static long fget(void *ctx, void *buf, long n)
{
    struct fget *g = ctx;
    LONG r;
    if (checkabort()) return 0;
    r = Read(g->fh, buf, n);
    if (r < 0) {
        g->err = IoErr();
        return 0;
    }
    progadd(r);
    return r;
}

/* one real file into the image at rel, with its bits, date and note */
static int putone(struct place *q, const char *src, const char *rel)
{
    struct ast *A = q->as;
    struct FileInfoBlock *fib;
    struct fget g;
    BPTR l;
    int r;
    char fb[80];
    if (!(fib = AllocMem(sizeof(*fib), MEMF_PUBLIC | MEMF_CLEAR))) {
        sfmt(opmsg, sizeof(opmsg), "not enough memory");
        return 0;
    }
    if (!(l = Lock((STRPTR)src, ACCESS_READ)) || !Examine(l, fib)) {
        if (l) UnLock(l);
        FreeMem(fib, sizeof(*fib));
        sfmt(opmsg, sizeof(opmsg), "cannot read the source");
        return 0;
    }
    UnLock(l);
    g.err = 0;
    if (!(g.fh = Open((STRPTR)src, MODE_OLDFILE))) {
        FreeMem(fib, sizeof(*fib));
        sfmt(opmsg, sizeof(opmsg), "cannot read the source");
        return 0;
    }
    r = adf_putfile(&A->adf, rel, (unsigned long)fib->fib_Size, fget, &g, (unsigned long)fib->fib_Protection,
                    fib->fib_Date.ds_Days, fib->fib_Date.ds_Minute, fib->fib_Date.ds_Tick, fib->fib_Comment);
    Close(g.fh);
    FreeMem(fib, sizeof(*fib));
    if (r && !abortop) {
        if (g.err) os_fault(g.err, fb, sizeof(fb)), sfmt(opmsg, sizeof(opmsg), "copy failed: %s", fb);
        else sfmt(opmsg, sizeof(opmsg), "%s: %s", filepart(rel), aerr(r));
    }
    return r == 0;
}

static int putskipped;          /* links left out of the trees just put in */

/* a real directory tree into the image at rel (an existing folder merges) */
static int puttree(struct place *q, const char *src, const char *rel, int depth)
{
    struct ast *A = q->as;
    struct dscan *d;
    struct dent e;
    char *cs, *cr;
    int ok = 1, r;
    if (depth > MAXDEPTH) {
        sfmt(opmsg, sizeof(opmsg), "directory tree too deep");
        return 0;
    }
    if (!(d = ds_open(src))) {
        sfmt(opmsg, sizeof(opmsg), "cannot read the source");
        return 0;
    }
    r = adf_mkdir(&A->adf, rel);
    if (r == ADF_EEXIST) {
        /* a merge - but only into a directory, not over a file */
        struct kids K;
        memset(&K, 0, sizeof(K));
        if (collect(q, rel, &K) != ARC_OK) r = ADF_EEXIST + 1000;
        kidsfree(&K);
        if (r != ADF_EEXIST) {
            ds_close(d);
            sfmt(opmsg, sizeof(opmsg), "%s: a file with that name is in the way", filepart(rel));
            return 0;
        }
        r = 0;
    }
    if (r) {
        ds_close(d);
        sfmt(opmsg, sizeof(opmsg), "%s: %s", filepart(rel), aerr(r));
        return 0;
    }
    cs = xalloc(CPATH);
    cr = xalloc(CPATH);
    if (!cs || !cr) ok = 0, sfmt(opmsg, sizeof(opmsg), "not enough memory");
    while (ok && !abortop && (r = ds_next(d, &e)) > 0) {
        if (!buildfull(cs, src, e.name) || strlen(rel) + strlen(e.name) + 2 > CPATH) {
            sfmt(opmsg, sizeof(opmsg), "the path is too long");
            ok = 0;
            break;
        }
        sfmt(cr, CPATH, "%s/%s", rel, e.name);
        if (e.type == ST_SOFTLINK || e.type == ST_LINKDIR) {
            putskipped++;                       /* a link is not followed */
            continue;
        }
        ok = e.type > 0 ? puttree(q, cs, cr, depth + 1) : putone(q, cs, cr);
    }
    if (ok && r < 0) ok = 0, sfmt(opmsg, sizeof(opmsg), "cannot read the whole source directory");
    ds_close(d);
    xfree(cs);
    xfree(cr);
    return ok && !abortop;
}

/* a directory in the image, with everything in it */
/* is rel a link (an ADF hard or soft link)? Asked of its directory */
static int islinkw(struct place *p, const char *rel)
{
    struct kids K;
    struct kid *k;
    int l = 0;
    memset(&K, 0, sizeof(K));
    if (kidof(p, rel, &K, &k) == ARC_OK) l = k->link;
    kidsfree(&K);
    return l;
}

static int deltreew(struct place *p, const char *rel, int depth)
{
    struct ast *A = p->as;
    struct kids K;
    char *cr;
    int i, ok = 1, r;
    if (depth > MAXDEPTH) return 0;
    if (depth == 0 && islinkw(p, rel)) {
        /* a hard link to a directory: the link goes, never what it
         * points to (walking it emptied the real directory - the audit) */
        if ((r = adf_delete(&A->adf, rel))) {
            sfmt(opmsg, sizeof(opmsg), "%s: %s", filepart(rel), aerr(r));
            return 0;
        }
        return 1;
    }
    memset(&K, 0, sizeof(K));
    if ((r = collect(p, rel, &K))) {
        kidsfree(&K);
        sfmt(opmsg, sizeof(opmsg), "cannot read the image: %s", aerr(r));
        return 0;
    }
    if (!(cr = xalloc(CPATH))) ok = 0;
    for (i = 0; ok && i < K.n; i++) {
        if (checkabort()) {
            ok = 0;
            break;
        }
        sfmt(cr, CPATH, "%s/%s", rel, K.k[i].name);
        if (K.k[i].isdir && !K.k[i].link) ok = deltreew(p, cr, depth + 1);
        else if ((r = adf_delete(&A->adf, cr))) ok = 0, sfmt(opmsg, sizeof(opmsg), "%s: %s", K.k[i].name, aerr(r));
    }
    xfree(cr);
    kidsfree(&K);
    if (ok && (r = adf_delete(&A->adf, rel))) ok = 0, sfmt(opmsg, sizeof(opmsg), "%s: %s", filepart(rel), aerr(r));
    return ok;
}

/* the entries idx[] of pane p into image pane q, as tn[] */
int arcfs_adfput(struct place *p, struct place *q, const int *idx, char (*tn)[NAMEMAX + 4], int n, int ismove)
{
    struct place *P = p;
    char src[CPATH], rel[CPATH], stage[CPATH], itname[NAMEMAX + 8], mb[160];
    int s, ok = 1, done = 0, hadicon, iconok, keptsrc = 0, nmoved = 0, *moved = NULL;
    if (ismove && pl_inarc(p) && arcfs_isadf(p) && nocase(p->as->file, q->as->file) == 0) {
        /* two writers on one image would each keep their own bitmap */
        showmsg("source and target are the same disk image - rename inside it instead");
        return 0;
    }
    if (ismove && pl_inarc(p) && !(moved = xalloc(n * sizeof(int)))) {
        showmsg("not enough memory");
        return 0;
    }
    LONG fr;
    BPTR l;
    opmsg[0] = 0;
    stage[0] = 0;
    statbytes = 0;
    for (s = 0; s < n; s++) {
        struct ent *e = &P->ent[idx[s]];
        if (e->kind == KD_FILE) statbytes += e->size;
        else if (pl_inarc(p)) statbytes += arcfs_treesize(p, e->name);
        else if (buildfull(src, P->path, e->name)) treestat(src, 0);
    }
    if ((fr = arcfs_free(q)) >= 0 && statbytes > (ULONG)fr) {
        char f1[12], f2[12];
        fmtsize(f1, statbytes);
        fmtsize(f2, (ULONG)fr);
        sfmt(mb, sizeof(mb), "needs %s, the disk has %s free - nothing copied", f1, f2);
        showmsg(mb);
        return 0;
    }
    if (beginw(q)) {
        showmsg(opmsg);
        return 0;
    }
    if (!cbuf_get()) {
        end(q);
        showmsg("not enough memory");
        return 0;
    }
    cancelok = 1;
    abortop = 0;
    progshow(statbytes);
    if (pl_inarc(p) && !arcfs_isadf(p)) arcfs_hold(p);    /* (an image source re-opens per entry) */
    for (s = 0; s < n && ok && !abortop; s++) {
        struct ent *e = &P->ent[idx[s]];
        int isdir = e->kind != KD_FILE;
        const char *from = src;
        relof(q, tn[s], rel);
        if (pl_inarc(p)) {
            /* from another archive: through a scratch copy beside the image */
            end(q);
            sidename(stage, q->as->file, 'v');
            wipetree(stage);
            ok = arcfs_copyout(p, e->name, stage, isdir);
            if (ok && beginw(q)) ok = 0;
        } else if (!buildfull(src, P->path, e->name)) {
            sfmt(opmsg, sizeof(opmsg), "the path is too long");
            ok = 0;
        }
        if (pl_inarc(p)) from = stage;
        putskipped = 0;
        if (ok) ok = isdir ? puttree(q, from, rel, 0) : putone(q, from, rel);
        if (stage[0]) wipetree(stage), stage[0] = 0;
        /* the icon travels with it */
        hadicon = iconok = 0;
        if (ok && icons && !isinfo(e->name) && !pl_inarc(p) && sidecarof(P->path, e->name, src)) {
            sfmt(itname, sizeof(itname), "%s.info", tn[s]);
            relof(q, itname, rel);
            hadicon = 1;
            iconok = putone(q, src, rel);
        }
        if (ok) done++;
        if (ok && ismove && !pl_inarc(p) && buildfull(src, P->path, e->name)) {
            /* the source goes only for what is now on the disk: a tree
             * with links left out stays, an icon not copied stays */
            if (isdir && putskipped) keptsrc++;
            else if (isdir) deltree(src, 0, 0);
            else zap(src, 1);
            if (hadicon && iconok && sidecarof(P->path, e->name, src)) zap(src, 0);
        } else if (ok && ismove && pl_inarc(p)) {
            /* out of another image or archive: removed from it after */
            if (moved) moved[nmoved++] = idx[s];
        }
    }
    cancelok = 0;
    arcfs_release(p);
    end(q);
    cbuf_free();
    progoff();
    if (nmoved && !abortop) {
        if (arcfs_isadf(p)) {
            for (s = 0; s < nmoved; s++)
                if (!arcfs_adfdelone(p, P->ent[moved[s]].name, P->ent[moved[s]].kind != KD_FILE)) break;
        } else {
            arcw_delents(p, moved, nmoved);
        }
    }
    xfree(moved);
    placechanged(NULL, NULL, 2);
    if (abortop) sfmt(mb, sizeof(mb), "cancelled - %d of %d done, the disk is valid", done, n), showmsg(mb);
    else if (!ok) showmsg(opmsg[0] ? opmsg : "the copy failed");
    else if (keptsrc) showmsg("links inside were not copied - those source folders were kept");
    abortop = 0;
    (void)l;
    return ok;
}

/* delete one entry of image pane p (a move out of it) - 1 done */
int arcfs_adfdelone(struct place *p, const char *name, int isdir)
{
    char rel[CPATH];
    int ok, r;
    if (!relof(p, name, rel) || beginw(p)) return 0;
    if (isdir) ok = deltreew(p, rel, 0);
    else if ((r = adf_delete(&p->as->adf, rel))) ok = 0, sfmt(opmsg, sizeof(opmsg), "%s: %s", name, aerr(r));
    else ok = 1;
    end(p);
    return ok;
}

void arcfs_adfdelete(struct place *p, int cur)
{
    struct place *P = p;
    int nmark = place_marks(p), i, ok = 1, done = 0, *idx, n, s;
    char mb[160], rel[CPATH];
    if (P->fail || !(n = place_pick(p, cur, &idx))) return;
    if (nmark) sfmt(mb, sizeof(mb), "delete %d marked entries from the disk image? (y)es (n)o", nmark);
    else sfmt(mb, sizeof(mb), "delete \"%s\" from the disk image? (y)es (n)o", P->ent[idx[0]].name);
    if ((askkey(mb) | 32) != 'y') {
        xfree(idx);
        return;
    }
    opmsg[0] = 0;
    if (beginw(p)) {
        xfree(idx);
        showmsg(opmsg);
        return;
    }
    cancelok = 1;
    abortop = 0;
    /* (a marked icon whose file is marked goes with the file, once) */
    for (s = 0; s < n && ok && !abortop; s++) {
        struct ent *e = &P->ent[i = idx[s]];
        int r;
        relof(p, e->name, rel);
        if (e->kind != KD_FILE) ok = deltreew(p, rel, 0);
        else if ((r = adf_delete(&p->as->adf, rel))) ok = 0, sfmt(opmsg, sizeof(opmsg), "%s: %s", e->name, aerr(r));
        if (ok) done++;
        /* the icon goes with its file */
        if (ok && icons && !isinfo(e->name) && strlen(rel) + 6 < CPATH) {
            strcat(rel, ".info");
            adf_delete(&p->as->adf, rel);
        }
    }
    cancelok = 0;
    end(p);
    xfree(idx);
    placechanged(NULL, NULL, 1);
    if (abortop) showmsg("stopped - the disk is valid");
    else if (!ok) showmsg(opmsg);
    abortop = 0;
}

void arcfs_adfrename(struct place *p, const char *from, const char *to)
{
    char a[CPATH], b[CPATH];
    int r;
    opmsg[0] = 0;
    if (!relof(p, from, a) || !relof(p, to, b) || strlen(a) + 6 > CPATH || strlen(b) + 6 > CPATH) {
        showmsg("the path is too long");
        return;
    }
    if (beginw(p)) {
        showmsg(opmsg);
        return;
    }
    if ((r = adf_rename(&p->as->adf, a, b))) {
        sfmt(opmsg, sizeof(opmsg), "cannot rename: %s", aerr(r));
    } else if (icons && !isinfo(from)) {
        strcat(a, ".info");
        strcat(b, ".info");
        adf_rename(&p->as->adf, a, b);        /* there or not */
    }
    end(p);
    placechanged(p, to, 0);
    if (r) showmsg(opmsg);
}

void arcfs_adfmkdir(struct place *p, const char *name)
{
    char rel[CPATH];
    int r;
    opmsg[0] = 0;
    if (!relof(p, name, rel)) return;
    if (beginw(p)) {
        showmsg(opmsg);
        return;
    }
    if ((r = adf_mkdir(&p->as->adf, rel))) sfmt(opmsg, sizeof(opmsg), "cannot create: %s", aerr(r));
    end(p);
    placechanged(p, name, 0);
    if (r) showmsg(opmsg);
}

/* n with a name ending in .adf: a blank 880K disk, named after the file */
int arcfs_newadf(const char *path)
{
    char vol[32], tmp[CPATH], fb[80];
    struct fsrc fs;
    struct DateStamp ds;
    int swap, r, n;
    const char *f = filepart(path);
    for (n = 0; f[n] && f[n] != '.' && n < 30; n++) vol[n] = f[n];
    vol[n] = 0;
    if (!n) strcpy(vol, "Empty");
    swap = os_pathtype(path) == 1;
    if (swap) sidename(tmp, path, 'n');
    else strcpy(tmp, path);
    if (!(fs.fh = Open((STRPTR)tmp, MODE_NEWFILE))) {
        os_fault(IoErr(), fb, sizeof(fb));
        sfmt(opmsg, sizeof(opmsg), "cannot create: %s", fb);
        return 0;
    }
    /* the boot blocks first: AmigaDOS cannot Seek past a file's end, and
     * the formatter starts at block 2; from there it writes in order, so
     * the file grows as it goes (no 880K of zeros written twice) */
    {
        static const UBYTE z[16];
        int i;
        r = 0;
        for (i = 0; i < 1024 / 16 && !r; i++)
            if (Write(fs.fh, (APTR)z, 16) != 16) r = ADF_EWRITE;
        if (r) {
            Close(fs.fh);
            DeleteFile((STRPTR)tmp);
            sfmt(opmsg, sizeof(opmsg), "cannot make the disk image: %s", aerr(r));
            return 0;
        }
    }
    fs.pos = 1024;
    DateStamp(&ds);
    /* FFS where the machine reads it; OFS for a 1.3 Amiga (its ROM has
     * no FFS for floppies). Bootable: every Kickstart boots it */
    r = adf_format(imgwrite, &fs, 0, (v_dos >= 36 ? 1 : 0) | ADF_BOOT, vol, ds.ds_Days, ds.ds_Minute, ds.ds_Tick);
    if (!r) {
        /* and it must have come out whole */
        Seek(fs.fh, 0, OFFSET_END);
        if (Seek(fs.fh, 0, OFFSET_BEGINNING) != 901120) r = ADF_EWRITE;
    }
    Close(fs.fh);
    if (r || (swap && !swapin(tmp, path, 0))) {
        DeleteFile((STRPTR)tmp);
        sfmt(opmsg, sizeof(opmsg), "cannot make the disk image: %s", r ? aerr(r) : "cannot replace the old one");
        return 0;
    }
    return 1;
}

/* one real file into image pane q as name (the editor's way back) */
int arcfs_adfputfile(struct place *q, const char *src, const char *name)
{
    char rel[CPATH];
    int ok;
    opmsg[0] = 0;
    if (!relof(q, name, rel)) return 0;
    if (beginw(q)) {
        showmsg(opmsg);
        return 0;
    }
    ok = putone(q, src, rel);
    end(q);
    if (!ok) showmsg(opmsg);
    return ok;
}

/* a scratch name for an archive member being edited (T:, else RAM:) */
int arcfs_scratch(char *dst)
{
    const char *where;
    os_quiet(1);
    where = os_pathtype("T:") == 2 ? "T:" : os_pathtype("RAM:") == 2 ? "RAM:" : NULL;
    os_quiet(0);
    if (!where) return 0;
    sfmt(dst, CPATH, "%sCFileE%lx", where, (ULONG)FindTask(NULL));
    return 1;
}

/* after a change to pane p's archive: p's member list read again, and
 * the other pane's too when it shows the same archive (a stale list
 * there read old offsets, and a rebuild from it dropped new members) */
void arcfs_reloadsame(struct place *p)
{
    struct ast *o;
    arcfs_reload(p);
    for (o = alist; o; o = o->next)
        if (o != p->as && o->owner->arc && o->kind == p->as->kind && nocase(o->file, p->as->file) == 0)
            arcfs_reload(o->owner);
}

/* the listing stopped at damage: CFile shows what it could read, but
 * writing would drop the unreadable rest */
int arcfs_damaged(struct place *p)
{
    return p->as && p->as->damaged;
}

/* keep pane p's archive open across many copyout calls (see begin) */
int arcfs_hold(struct place *p)
{
    int r;
    if (!p->as || !p->as->kind) return ARC_EUNSUP;
    if ((r = begin(p)) == ARC_OK) p->as->held = 1;
    return r;
}

void arcfs_release(struct place *p)
{
    if (!p->as || !p->as->held) return;
    p->as->held = 0;
    kcfree();
    end(p);
}

/* any place still in an archive or image: the archive code is in use
 * (CFile drops it from memory when not - src/modload.c) */
int arcfs_busy(void)
{
    return alist != NULL;
}
