// SPDX-License-Identifier: GPL-2.0-only
/* zup: installs the root half of an update (DESIGN D15, docs/d15-self-updates.md section 5) and, since DESIGN D22
 * (docs/d22-zpkg.md 4.2), single packages for zpkg; docs/man/zup.txt.
 * boot runs it at every start, after the root is mounted and before anything else (user/boot.c):
 *   1. a journal left by a power cut: finish the install (every new file complete and checked) or roll it back;
 *   2. "Zenix (previous)" (the rollback flag) or a kernel older than the installed base: put the previous version
 *      back from /var/cache/zpkg/prev-V;
 *   3. a bundle zenixboot left in memory (SYS_update INFO): check all of it before any change (SHA-256 of every part,
 *      versions, dependencies, space), install a newer zup first and run it for the rest, then the other packages
 *      in one transaction.
 * zup -f FILE (one package bundle, streamed from the file) and zup -r NAME (one package removed) are live
 * transactions (D22 4.2): the same engine, under /var/lib/zpkg/lock, their previous files in /var/cache/zpkg/live/
 * until the commit; they never touch a prev set (the system's way back) and never a base-group package.
 * A transaction: the journal (written and synced first), the volume marked "needs a check", every replaced or removed
 * file hard-linked into prev-V/f/ (no copies), each new file written as PATH.zup-new, checked against its SHA-256,
 * owner then mode set, renamed over PATH (crash-safe in ext2 since D15 step 0, and a running program keeps its code,
 * D13); then the undo log, the commit record, the old prev sets deleted, the journal removed, the volume clean again.
 * Config files (c in a manifest) are compared three ways: replaced only if the owner never changed them, else the new
 * one is written as PATH.new and the owner is told. Only the root filesystem is touched.
 * Built for the PC too (tools/zup-host-test.sh: tools/host/zup-host.c serves these system calls from a directory and
 * cuts the power at any one of them). A v1 ZEXE using only calls and structs that exist since D15. */
#ifdef ZHOST
#include "../tools/host/zup-host.h"
#else
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
#include "sha256.h"
#include "../kernel/fsproto.h"
#endif
#include "miniz.h"
#include "zpkgfmt.h"

#define DB       "/var/lib/zpkg"
#define JOURNAL  DB "/journal"
#define LOCK     DB "/lock"
#define RESULT   DB "/result"
#define CACHE    "/var/cache/zpkg"
#define MAXPKG   ZF_MAXPKG
#define MAXOPS   1024
#define PMAX     ZF_PMAX        /* every path given to a system call is at most 63 bytes */

static int failed;              /* an error was reported: the exit status */
static char result[200];        /* the one-line result for init and login (RESULT) */
static char lastmsg[160];       /* the last thing said: the reason a refusal gives in the result */

/* Every message goes out the moment it is said: libz's printf writes each call with one write() (only oputs and
 * oprintf collect output), so a zup that is stopped or loses its power has shown all it said (audit 2026-10-26 F3:
 * checked, it was never buffered; tools/update-test.sh slowzup checks the messages of the zups boot stopped). */
static void vsay(const char *fmt, va_list ap)
{
    vsnprintf(lastmsg, sizeof lastmsg, fmt, ap);
    printf("zup: %s\n", lastmsg);
}
static void say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsay(fmt, ap); va_end(ap); }
void zf_say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsay(fmt, ap); va_end(ap); }   /* user/zpkgfmt.c's */

/* ---- small helpers ---- */
static int pjoin(char *out, const char *a, const char *b)   /* a + b, at most 63 bytes */
{
    u32 la = strlen(a), lb = strlen(b);
    if (la + lb >= PMAX) return -ENAMETOOLONG;
    memcpy(out, a, la); memcpy(out + la, b, lb + 1);
    return 0;
}
static u8 iobuf[4096];
static int sha_file(const char *path, u8 out[32])           /* 0, or -errno */
{
    int fd = sys_open(path, O_RDONLY), n;
    if (fd < 0) return fd;
    struct sha256 c;
    sha256_init(&c);
    while ((n = sys_read(fd, iobuf, sizeof iobuf)) > 0) sha256_update(&c, iobuf, (u32)n);
    sys_close(fd);
    if (n < 0) return n;
    sha256_final(&c, out);
    return 0;
}
static int file_has(const char *path, const u8 sha[32])     /* the file exists (not a link) and has this SHA-256 */
{
    struct ustat st; u8 h[32];
    return sys_lstat(path, &st) == 0 && (st.mode & S_IFMT) == S_IFREG && sha_file(path, h) == 0 && !memcmp(h, sha, 32);
}
static int link_is(const char *path, const char *target)
{
    char b[PMAX];
    int n = sys_readlink(path, b, sizeof b - 1);
    if (n < 0) return 0;
    b[n] = 0;
    return !strcmp(b, target);
}
static char *slurp(const char *path, u32 *len)              /* the whole file, NUL-terminated (malloc'd), or 0 */
{
    struct ustat st;
    if (sys_stat(path, &st) < 0 || st.size > (1u << 20)) return 0;
    char *b = malloc(st.size + 1);
    int fd = sys_open(path, O_RDONLY);
    if (!b || fd < 0) { free(b); if (fd >= 0) sys_close(fd); return 0; }
    int n = sys_read(fd, b, st.size);
    sys_close(fd);
    if (n != (int)st.size) { free(b); return 0; }
    b[n] = 0;
    if (len) *len = (u32)n;
    return b;
}
static int write_all(int fd, const void *p, u32 n)
{
    while (n) {
        int w = sys_write(fd, p, n > 4096 ? 4096 : n);
        if (w <= 0) return w < 0 ? w : -EIO;
        p = (const u8 *)p + w; n -= (u32)w;
    }
    return 0;
}
static int write_file(const char *path, const void *p, u32 n)   /* O_TRUNC: the whole file */
{
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC), r;
    if (fd < 0) return fd;
    r = write_all(fd, p, n);
    sys_close(fd);
    return r;
}
static void mkdirs(const char *path)                         /* every directory of an absolute path */
{
    char b[PMAX];
    u32 n = strlen(path);
    if (n >= PMAX) return;
    memcpy(b, path, n + 1);
    for (u32 i = 1; i <= n; i++)
        if (b[i] == '/' || !b[i]) { char c = b[i]; b[i] = 0; sys_mkdir(b); b[i] = c; }
}
static void rm_tree(const char *path)                        /* a prev set: files and directories, nothing else */
{
    char **names, sub[PMAX];
    int n = dir_list(path, &names);
    for (int i = 0; i < n; i++) {
        struct ustat st;
        if (pjoin(sub, path, "/") || pjoin(sub, sub, names[i]) || sys_lstat(sub, &st) < 0) continue;
        if ((st.mode & S_IFMT) == S_IFDIR) rm_tree(sub); else sys_unlink(sub);
    }
    dir_free(names, n);
    sys_rmdir(path);
}

/* the "needs a check" mark of the root's server (FS_MARK; ZenixFS does not need it and says -ENOSYS) */
static int fs_mark(int unclean)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = FS_MARK;
    m.a[0] = (u32)unclean;
    int port = sys_port_find("fs");
    return port < 0 ? port : sys_call(port, &m);
}

/* ---- the lock (DESIGN D22 4.2): one zup or zpkg changes packages at a time ----
 * /var/lib/zpkg/lock holds the pid of the zpkg or zup that changes things, made with O_EXCL. A zup finds it held by
 * its parent (zpkg, or the zup that runs it as stage 2) and goes on under it; held by another living zup or zpkg it
 * refuses; a lock whose pid is not a living zup or zpkg (a power cut, a killed zpkg, a pid used again after a
 * restart, or its own pid) is stale and taken over. Only the holder removes it. */
static int lock_mine;
static int proc_get(int pid, struct uproc *out)              /* the living process with this pid: 1, else 0 */
{
    for (u32 i = 1; sys_procinfo(i, out) >= 0; i++)
        if (out->state && out->state != 5 && (int)out->pid == pid) return 1;   /* 5: a zombie */
    return 0;
}
static int is_pkgprog(const struct uproc *p) { return !strcmp(p->name, "zup") || !strcmp(p->name, "zpkg"); }
/* another living zup than this one and its parent: one may still run after the zpkg that held the lock for it died
 * (its journal is not a cut one), so a dead holder's lock is stale only when no other zup lives */
static int other_zup(int me, int ppid)
{
    struct uproc p;
    for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++)
        if (p.state && p.state != 5 && !strcmp(p.name, "zup") && (int)p.pid != me && (int)p.pid != ppid) return (int)p.pid;
    return 0;
}
/* the lock file is made and removed under the "needs a check" mark, as every change zup makes (F5) */
static int lock_take(void)                                  /* 0, or -EBUSY (said) */
{
    int me = sys_getpid(), ppid = 0, z;
    struct uproc p;
    if (proc_get(me, &p)) ppid = (int)p.ppid;
    for (int tries = 0; tries < 3; tries++) {
        char b[16];
        int wasclean = fs_mark(1) == 1;
        int fd = sys_open(LOCK, O_WRONLY | O_CREAT | O_EXCL), r = 0;
        if (fd >= 0) {
            int n = snprintf(b, sizeof b, "%d\n", me);
            r = write_all(fd, b, (u32)n);
            sys_close(fd);
            if (r < 0) sys_unlink(LOCK);
        }
        sys_sync();
        if (wasclean) fs_mark(0);
        if (fd >= 0) {
            if (r < 0) { say("cannot write %s (%d)", LOCK, r); return r; }
            lock_mine = 1;
            return 0;
        }
        if (fd != -EEXIST) { mkdirs(DB); if (tries) { say("cannot make %s (%d)", LOCK, fd); return fd; } continue; }
        u32 len;
        char *t = slurp(LOCK, &len);
        int holder = t ? (int)strtoul(t, 0, 10) : 0;
        free(t);
        if (holder > 0 && holder != me && proc_get(holder, &p) && is_pkgprog(&p)) {
            if (holder == ppid) return 0;                   /* our zpkg (or stage 1) holds it for us */
            say("another %s (pid %d) is changing packages: nothing done", p.name, holder);
            return -EBUSY;
        }
        if ((z = other_zup(me, ppid))) { say("another zup (pid %d) is still changing packages: nothing done", z); return -EBUSY; }
        wasclean = fs_mark(1) == 1;                         /* stale: taken over */
        sys_unlink(LOCK);
        sys_sync();
        if (wasclean) fs_mark(0);
    }
    say("cannot take %s", LOCK);
    return -EBUSY;
}
static void lock_drop(void)
{
    if (!lock_mine) return;
    u32 len;
    char *t = slurp(LOCK, &len);
    if (t && (int)strtoul(t, 0, 10) == sys_getpid()) {
        int wasclean = fs_mark(1) == 1;
        sys_unlink(LOCK);
        sys_sync();
        if (wasclean) fs_mark(0);
    }
    free(t);
    lock_mine = 0;
}

/* ---- manifests (user/zpkgfmt.c) ---- */
/* the installed manifests (/var/lib/zpkg/NAME.manifest): a table that grows as needed. A damaged manifest or no
 * memory sets dberr: live transactions refuse then (a package wrongly taken as "not installed" could lose files) */
static struct zf_pkg **db;
static int ndb, dbcap, dberr, dball;
static struct zf_pkg *installed(const char *name)
{
    for (int i = 0; i < ndb; i++)
        if (!strcmp(db[i]->name, name)) return db[i];
    char path[PMAX];
    u32 len;
    struct ustat st;
    if (pjoin(path, DB "/", name) || pjoin(path, path, ".manifest") || sys_lstat(path, &st) < 0) return 0;
    if (ndb == dbcap) {
        int nc = dbcap ? dbcap * 2 : 16;
        struct zf_pkg **n = realloc(db, (u32)nc * sizeof *n);
        if (!n) { say("out of memory for the package database"); dberr = 1; return 0; }
        db = n; dbcap = nc;
    }
    char *t = slurp(path, &len);
    struct zf_pkg *k = malloc(sizeof *k);
    if (!t || !k) { free(t); free(k); say("%s: cannot read it", path); dberr = 1; return 0; }
    if (zf_manifest_parse(k, t, len, path) || strcmp(k->name, name)) {
        say("%s: the package database is damaged", path);
        zf_pkg_free(k); free(k); dberr = 1;
        return 0;
    }
    return db[ndb++] = k;
}
static void installed_all(void)                             /* every installed manifest into the table */
{
    char **names;
    if (dball) return;
    dball = 1;
    int n = dir_list(DB, &names);
    for (int i = 0; i < n; i++) {
        u32 l = strlen(names[i]);
        if (l < 10 || strcmp(names[i] + l - 9, ".manifest")) continue;
        names[i][l - 9] = 0;
        if (zf_name_ok(names[i])) installed(names[i]);
    }
    dir_free(names, n);
}
static u32 base_version(void) { struct zf_pkg *b = installed("base"); return b ? b->vnum : 0; }

/* ---- the plan: one transaction's operations ----
 * R replace (the old file kept as a prev link), A add, D delete (prev link), l a new link, L a changed link (the old
 * target kept), u a removed link (its target kept); live only (D22 4.2): S a changed config file of a removed package
 * renamed PATH.save (a prev link first), M a new directory (so a rollback leaves none behind), X a directory of a
 * removed package removed if empty (put back by a rollback). The journal holds one line an operation:
 *   OP LINK MODE UID GID SHA TARGET OLD PATH         ("-" for what does not apply; LINK is T.N in prev-V/f/) */
struct op {
    char op, link[12];
    u16 mode, uid, gid;
    u8 sha[32]; int hassha;
    char *target, *old, *path;
    struct zf_pkg *pk; const char *mpath;   /* where the new content comes from: pk's tar member mpath, */
    const u8 *data; u32 datalen;            /* or these bytes (a manifest: the package's own, or a taken one rewritten) */
    int done;
};
static struct op *ops;                      /* allocated for the plan's size (D22 4.2), at most MAXOPS */
static int nops, opcap;
static u32 target_v, tnum;                  /* the version being installed and the transaction number within it */
static char prevdir[PMAX];
static int nkept;
static char kept[2][PMAX];
static int live;                            /* a live transaction (zup -f / -r): prevdir live/, no undo log */
static char livename[25];                   /* its package */

static struct op *op_new(void)
{
    if (nops == MAXOPS) return 0;
    if (nops == opcap) {
        int nc = opcap ? opcap * 2 : 32;
        if (nc > MAXOPS) nc = MAXOPS;
        struct op *n = realloc(ops, (u32)nc * sizeof *n);
        if (!n) return 0;
        ops = n; opcap = nc;
    }
    struct op *o = &ops[nops++];
    memset(o, 0, sizeof *o);
    return o;
}
static struct op *add_op(char op, const char *path)
{
    struct op *o = op_new();
    if (!o) { say(nops == MAXOPS ? "more than %d changes in one update" : "out of memory for the plan", MAXOPS); return 0; }
    o->op = op;
    o->path = strdup(path);
    if (op == 'R' || op == 'D' || op == 'S') snprintf(o->link, sizeof o->link, "%u.%d", tnum, nops);
    else strcpy(o->link, "-");
    return o->path ? o : 0;
}
static int same_meta(const struct ustat *st, const struct zf_ent *e)
{
    return (st->mode & 07777) == e->mode && st->uid == e->uid && st->gid == e->gid;
}
static int takes(const struct zf_pkg *k, const char *other)
{
    for (int i = 0; i < k->ntakes; i++) if (!strcmp(k->takes[i], other)) return 1;
    return 0;
}
/* the entry a path had before this package's plan: its own older version's, or (takes, D22 8.4.4) a taken package's */
static struct zf_ent *was_ent(struct zf_pkg *k, struct zf_pkg *old, const char *path)
{
    struct zf_ent *e = zf_find_ent(old, path);
    for (int t = 0; !e && t < k->ntakes; t++) e = zf_find_ent(installed(k->takes[t]), path);
    return e;
}
static int plan_pkg(struct zf_pkg *k)
{
    struct zf_pkg *old = installed(k->name);
    char tmp[PMAX];
    for (int i = 0; i < k->nent; i++) {
        struct zf_ent *e = &k->ent[i];
        struct ustat st;
        int have = sys_lstat(e->path, &st) == 0;
        struct op *o;
        if (e->kind == 'd') {                           /* made before the files: do_dirs, or (live) an M first */
            if (live && !have) {
                if (!(o = add_op('M', e->path))) return -ENOMEM;
                o->mode = e->mode; o->uid = e->uid; o->gid = e->gid;
            }
            continue;
        }
        if (e->kind == 'l') {
            if (have && (st.mode & S_IFMT) != S_IFLNK) { say("%s exists and is not a link: not replaced", e->path); return -EEXIST; }
            if (have && link_is(e->path, e->target)) continue;
            char oldt[PMAX] = "";
            if (have) { int n = sys_readlink(e->path, oldt, sizeof oldt - 1); oldt[n > 0 ? n : 0] = 0; }
            if (!(o = add_op(have ? 'L' : 'l', e->path))) return -ENOMEM;
            o->target = e->target; o->old = have ? strdup(oldt) : 0;
            continue;
        }
        if (have && (st.mode & S_IFMT) != S_IFREG) { say("%s exists and is not a file: not replaced", e->path); return -EEXIST; }
        const char *dest = e->path;
        if (e->kind == 'c' && have) {                   /* the three-way compare of a config file */
            u8 cur[32];
            if (sha_file(e->path, cur)) { say("%s: cannot read it", e->path); return -EIO; }
            if (!memcmp(cur, e->sha, 32)) continue;     /* already the new one */
            struct zf_ent *was = was_ent(k, old, e->path);
            if (!was || memcmp(cur, was->sha, 32)) {    /* the owner changed it (or we cannot know): theirs stays */
                if (pjoin(tmp, e->path, ".new") || !zf_path_ok(tmp)) { say("%s.new: the name is too long", e->path); return -ENAMETOOLONG; }
                dest = tmp;
                if (nkept < 2) strcpy(kept[nkept], e->path);
                nkept++;
                struct ustat st2;
                have = sys_lstat(dest, &st2) == 0;
                if (have && (st2.mode & S_IFMT) == S_IFREG && file_has(dest, e->sha)) continue;
                if (have && (st2.mode & S_IFMT) != S_IFREG) { say("%s exists and is not a file", dest); return -EEXIST; }
            }
        } else if (e->kind == 'c' && !have && was_ent(k, old, e->path)) {
            continue;                                   /* the owner deleted a config file the last version had */
        } else if (have && same_meta(&st, e) && st.size == e->size && file_has(e->path, e->sha)) {
            continue;                                   /* unchanged */
        }
        if (!(o = add_op(have ? 'R' : 'A', dest))) return -ENOMEM;
        o->mode = e->mode; o->uid = e->uid; o->gid = e->gid;
        memcpy(o->sha, e->sha, 32); o->hassha = 1;
        o->pk = k; o->mpath = e->path;
    }
    for (int i = 0; old && i < old->nent; i++) {        /* what the new version no longer has */
        struct zf_ent *e = &old->ent[i];
        struct ustat st;
        if (zf_find_ent(k, e->path) || sys_lstat(e->path, &st) < 0) continue;
        if (e->kind == 'f' && (st.mode & S_IFMT) == S_IFREG) { if (!add_op('D', e->path)) return -ENOMEM; }
        else if (e->kind == 'l' && (st.mode & S_IFMT) == S_IFLNK) {
            struct op *o = add_op('u', e->path);
            if (!o) return -ENOMEM;
            o->old = e->target;
        }
        /* c (the owner's settings) and d stay */
    }
    char mpath[PMAX];                                   /* the database entry itself, last */
    if (pjoin(mpath, DB "/", k->name) || pjoin(mpath, mpath, ".manifest")) return -ENAMETOOLONG;
    struct ustat st;
    struct op *o = add_op(sys_lstat(mpath, &st) == 0 ? 'R' : 'A', mpath);
    if (!o) return -ENOMEM;
    o->mode = 0644;
    sha256(k->man, k->manlen, o->sha); o->hassha = 1;
    o->data = k->man; o->datalen = k->manlen;
    return 0;
}
/* takes OTHER (D22 8.4.4): OTHER's manifest rewritten without the entries k lists, in the same transaction (an R of
 * its database entry with text zup makes; a D when no entry is left). Only from live transactions (zup -f) */
static int plan_takes(struct zf_pkg *k)
{
    for (int t = 0; t < k->ntakes; t++) {
        struct zf_pkg *ot = installed(k->takes[t]);
        if (!ot) continue;                              /* nothing installed to take from */
        char mpath[PMAX];
        u32 len, n = 0;
        if (pjoin(mpath, DB "/", ot->name) || pjoin(mpath, mpath, ".manifest")) return -ENAMETOOLONG;
        char *raw = slurp(mpath, &len), *out = raw ? malloc(len + 1) : 0;
        if (!out) { free(raw); say("%s: cannot read it", mpath); return -EIO; }
        int left = 0, gone = 0;
        for (char *l = raw, *e; *l; l = e) {            /* line by line: an entry k lists is dropped */
            for (e = l; *e && *e != '\n'; e++) ;
            u32 ll = (u32)(e - l) + (*e ? 1 : 0);
            int ent = (l[0] == 'f' || l[0] == 'c' || l[0] == 'd' || l[0] == 'l') && l[1] == ' ';
            if (ent) {
                char *p = e;                            /* the path is the last word */
                while (p > l && p[-1] != ' ') p--;
                char path[PMAX];
                u32 pl = (u32)(e - p);
                if (pl < PMAX) { memcpy(path, p, pl); path[pl] = 0; } else path[0] = 0;
                if (zf_find_ent(k, path)) { gone++; e += *e ? 1 : 0; continue; }
                left++;
            }
            memcpy(out + n, l, ll); n += ll;
            e += *e ? 1 : 0;
        }
        free(raw);
        if (!gone) { free(out); continue; }
        struct op *o = add_op(left ? 'R' : 'D', mpath);
        if (!o) { free(out); return -ENOMEM; }
        if (left) {
            o->mode = 0644;
            sha256(out, n, o->sha); o->hassha = 1;
            o->data = (const u8 *)out; o->datalen = n;
        } else free(out);
    }
    return 0;
}
/* zup -r: every file, link and config file of the package (a changed config file kept as PATH.save, or with -n
 * deleted), then its database entry. A path another installed package also lists stays */
static int owned_by_other(struct zf_pkg *k, const char *path)
{
    for (int i = 0; i < ndb; i++)
        if (db[i] != k && zf_find_ent(db[i], path)) return 1;
    return 0;
}
static int plan_remove(struct zf_pkg *k, int nosave)
{
    for (int i = 0; i < k->nent; i++) {
        struct zf_ent *e = &k->ent[i];
        struct ustat st;
        if (e->kind == 'd' || sys_lstat(e->path, &st) < 0 || owned_by_other(k, e->path)) continue;
        if (e->kind == 'l') {
            if ((st.mode & S_IFMT) != S_IFLNK) continue;
            struct op *o = add_op('u', e->path);
            if (!o) return -ENOMEM;
            o->old = e->target;
            continue;
        }
        if ((st.mode & S_IFMT) != S_IFREG) continue;
        char op = 'D';
        if (e->kind == 'c' && !nosave && !file_has(e->path, e->sha)) {       /* the owner changed it: kept */
            char sv[PMAX];
            struct ustat s2;
            if (pjoin(sv, e->path, ".save")) { say("%s.save: the name is too long", e->path); return -ENAMETOOLONG; }
            if (sys_lstat(sv, &s2) == 0) { say("%s exists: move it away first (it would be replaced)", sv); return -EEXIST; }
            op = 'S';
            if (nkept < 2) strcpy(kept[nkept], e->path);
            nkept++;
        }
        if (!add_op(op, e->path)) return -ENOMEM;
    }
    /* its directories no other package lists, deepest first (removed only if empty when their turn comes: X) */
    for (int d = ZF_PMAX; d > 1; d--)
        for (int i = 0; i < k->nent; i++) {
            struct zf_ent *e = &k->ent[i];
            struct ustat st;
            int depth = 0;
            for (const char *q = e->path; *q; q++) depth += *q == '/';
            if (e->kind != 'd' || depth != d - 1 || sys_lstat(e->path, &st) < 0 || (st.mode & S_IFMT) != S_IFDIR ||
                owned_by_other(k, e->path)) continue;
            struct op *o = add_op('X', e->path);
            if (!o) return -ENOMEM;
            o->mode = e->mode; o->uid = e->uid; o->gid = e->gid;
        }
    char mpath[PMAX];
    if (pjoin(mpath, DB "/", k->name) || pjoin(mpath, mpath, ".manifest")) return -ENAMETOOLONG;
    return add_op('D', mpath) ? 0 : -ENOMEM;
}
static void do_dirs(struct zf_pkg *k)
{
    for (int i = 0; i < k->nent; i++) {
        struct zf_ent *e = &k->ent[i];
        struct ustat st;
        if (e->kind != 'd' || sys_lstat(e->path, &st) == 0) continue;
        mkdirs(e->path);
        sys_chown(e->path, e->uid, e->gid);
        sys_chmod(e->path, e->mode);
    }
}

/* ---- the journal ---- */
static int put_op(char *line, u32 max, const struct op *o)
{
    char sh[65] = "-";
    if (o->hassha) zf_put_sha(sh, o->sha);
    return snprintf(line, max, "%c %s %o %u %u %s %s %s %s\n", o->op, o->link, o->mode, o->uid, o->gid, sh,
                    o->target ? o->target : "-", o->old ? o->old : "-", o->path);
}
static int ops_text(char **out, u32 *len)
{
    u32 cap = (u32)nops * 200 + 128, n = 0;
    char *b = malloc(cap);
    if (!b) return -ENOMEM;
    for (int i = 0; i < nops; i++) n += (u32)put_op(b + n, cap - n, &ops[i]);
    *out = b; *len = n;
    return 0;
}
/* the operations of journal or log text (in place); 0 or -EINVAL. S only in a live journal (D22 4.2) */
static int parse_ops(char *t)
{
    nops = 0;
    for (char *l = t, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        if (!l[0] || l[1] != ' ' || !strchr(live ? "RADlLuSMX" : "RADlLu", l[0])) continue;
        char *w[9]; int nw = 0;
        for (char *q = l; *q && nw < 9; ) {
            while (*q == ' ') *q++ = 0;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ') q++;
        }
        if (nw != 9 || nops == MAXOPS || strlen(w[1]) >= 12 || !zf_path_ok(w[8])) return -EINVAL;
        struct op *o = op_new();
        if (!o) return -ENOMEM;
        int bad = 0;
        o->op = w[0][0]; strcpy(o->link, w[1]);
        o->mode = (u16)zf_num(w[2], 8, &bad); o->uid = (u16)zf_num(w[3], 10, &bad); o->gid = (u16)zf_num(w[4], 10, &bad);
        if (strcmp(w[5], "-")) { if (zf_parse_sha(w[5], o->sha)) return -EINVAL; o->hassha = 1; }
        o->target = strcmp(w[6], "-") ? w[6] : 0;
        o->old = strcmp(w[7], "-") ? w[7] : 0;
        o->path = w[8];
        if (bad) return -EINVAL;
    }
    return 0;
}
static int journal_append(const char *line)
{
    int fd = sys_open(JOURNAL, O_WRONLY | O_APPEND), r;
    if (fd < 0) return fd;
    r = write_all(fd, line, strlen(line));
    sys_close(fd);
    sys_sync();
    return r;
}

/* ---- doing it ---- */
static int zupnew(char *out, const char *path) { return pjoin(out, path, ".zup-new"); }
static int prev_of(char *out, const struct op *o)
{
    return pjoin(out, prevdir, "/f/") || pjoin(out, out, o->link);
}

/* forward: what is left of one operation (the install's last phase, and finishing after a power cut) */
static int finish_op(struct op *o)
{
    char nw[PMAX];
    if (zupnew(nw, o->path)) return -ENAMETOOLONG;
    switch (o->op) {
    case 'R': case 'A': {
        struct ustat st;
        if (sys_lstat(nw, &st) < 0) return file_has(o->path, o->sha) ? 0 : -ENOENT;   /* renamed already */
        int r;
        if ((r = sys_chown(nw, o->uid, o->gid)) < 0 || (r = sys_chmod(nw, o->mode)) < 0) return r;
        return sys_rename(nw, o->path);
    }
    case 'D': case 'u': {
        int r = sys_unlink(o->path);
        return r == -ENOENT ? 0 : r;
    }
    case 'M': {                                         /* a directory the package lists that was not there */
        struct ustat st;
        if (sys_lstat(o->path, &st) == 0) return (st.mode & S_IFMT) == S_IFDIR ? 0 : -EEXIST;
        int r = sys_mkdir(o->path);
        if (r < 0) return r;
        sys_chown(o->path, o->uid, o->gid);
        return sys_chmod(o->path, o->mode);
    }
    case 'X': {                                         /* a removed package's directory, if nothing is left in it */
        struct ustat st;
        sys_rmdir(o->path);
        return sys_lstat(o->path, &st) == 0 && (st.mode & S_IFMT) != S_IFDIR ? -EEXIST : 0;
    }
    case 'S': {                                         /* PATH -> PATH.save (its prev link holds the inode) */
        char sv[PMAX];
        struct ustat st;
        if (pjoin(sv, o->path, ".save")) return -ENAMETOOLONG;
        if (sys_lstat(o->path, &st) < 0) return sys_lstat(sv, &st) == 0 ? 0 : -ENOENT;   /* renamed already */
        return sys_rename(o->path, sv);
    }
    case 'l': case 'L':
        if (link_is(o->path, o->target)) return 0;
        sys_unlink(nw);
        if (sys_symlink(o->target, nw) < 0) return -EIO;
        return sys_rename(nw, o->path);
    }
    return 0;
}
/* backward: put one operation's old state back (idempotent: a power cut in the middle repeats it) */
static void undo_op(struct op *o)
{
    char nw[PMAX], pv[PMAX];
    if (zupnew(nw, o->path)) return;
    struct ustat sp, sf;
    switch (o->op) {
    case 'R': case 'D':
        if (prev_of(pv, o) || sys_lstat(pv, &sp) < 0) { sys_unlink(nw); return; }     /* never linked: never touched */
        if (sys_lstat(o->path, &sf) == 0 && sf.ino == sp.ino) { sys_unlink(nw); return; }   /* the same inode: nothing */
        sys_unlink(nw);
        if (sys_link(pv, nw) == 0) sys_rename(nw, o->path);
        return;
    case 'S': {
        char sv[PMAX];
        if (prev_of(pv, o) || pjoin(sv, o->path, ".save") || sys_lstat(pv, &sp) < 0) { sys_unlink(nw); return; }
        if (sys_lstat(o->path, &sf) < 0 || sf.ino != sp.ino) {
            sys_unlink(nw);
            if (sys_link(pv, nw) == 0) sys_rename(nw, o->path);
        }
        if (sys_lstat(o->path, &sf) == 0 && sf.ino == sp.ino && sys_lstat(sv, &sf) == 0 && sf.ino == sp.ino) sys_unlink(sv);
        return;
    }
    case 'A':
        sys_unlink(nw);
        sys_unlink(o->path);
        return;
    case 'M':
        sys_rmdir(o->path);                             /* only if empty: what the package put there went first */
        return;
    case 'X':
        if (sys_lstat(o->path, &sf) < 0 && sys_mkdir(o->path) == 0) { sys_chown(o->path, o->uid, o->gid); sys_chmod(o->path, o->mode); }
        return;
    case 'l':
        sys_unlink(nw);
        if (link_is(o->path, o->target)) sys_unlink(o->path);
        return;
    case 'L': case 'u':
        sys_unlink(nw);
        if (!o->old || link_is(o->path, o->old)) return;
        if (sys_symlink(o->old, nw) == 0) sys_rename(nw, o->path);
        return;
    }
}
static void undo_all(void) { for (int i = nops; i-- > 0; ) undo_op(&ops[i]); }

/* the new content of each R/A operation of package k into PATH.zup-new: its files from the tar.gz, streamed (from the
 * bundle in memory, or from the file zup -f reads: tgzfd at tgzoff, read a piece at a time) */
static int tgzfd = -1;
static u32 tgzoff;
static u8 inbuf[4096];
struct tarst { int fd; u32 left, pad, hdr; struct op *o; struct sha256 c; u8 h[512]; int err; struct zf_pkg *k; };
static struct op *op_for(struct zf_pkg *k, const char *mpath)
{
    for (int i = 0; i < nops; i++)
        if (ops[i].pk == k && ops[i].mpath && !strcmp(ops[i].mpath, mpath) && (ops[i].op == 'R' || ops[i].op == 'A')) return &ops[i];
    return 0;
}
static u32 octal(const u8 *p, int n, int *bad)
{
    u32 v = 0;
    int i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v >> 29) { *bad = 1; return 0; }
        v = v << 3 | (u32)(p[i] - '0');
    }
    return v;
}
static int new_close(struct tarst *t)                   /* a member is complete: check what reached the disk */
{
    u8 h[32];
    char nw[PMAX];
    sys_close(t->fd);
    t->fd = -1;
    sha256_final(&t->c, h);
    if (zupnew(nw, t->o->path)) return -ENAMETOOLONG;
    if (memcmp(h, t->o->sha, 32)) { say("%s: SHA-256 of the package's file does not match its manifest", t->o->mpath); return -EINVAL; }
    if (sha_file(nw, h) || memcmp(h, t->o->sha, 32)) { say("%s: what was written does not read back right", nw); return -EIO; }
    int r;
    if ((r = sys_chown(nw, t->o->uid, t->o->gid)) < 0 || (r = sys_chmod(nw, t->o->mode)) < 0) { say("%s: owner or mode not set", nw); return r; }
    t->o->done = 1;
    return 0;
}
static int tar_feed(struct tarst *t, const u8 *p, u32 n)
{
    while (n && !t->err) {
        if (t->left) {                                  /* a member's data */
            u32 k = n < t->left ? n : t->left;
            if (t->fd >= 0) {
                int r = write_all(t->fd, p, k);
                if (r) { say("%s: cannot write it (%d)", t->o->path, r); t->err = r; return r; }
                sha256_update(&t->c, p, k);
            }
            t->left -= k; p += k; n -= k;
            if (!t->left && t->fd >= 0 && (t->err = new_close(t))) return t->err;
            continue;
        }
        if (t->pad) { u32 k = n < t->pad ? n : t->pad; t->pad -= k; p += k; n -= k; continue; }
        u32 k = 512 - t->hdr;                           /* a header */
        if (k > n) k = n;
        memcpy(t->h + t->hdr, p, k);
        t->hdr += k; p += k; n -= k;
        if (t->hdr < 512) continue;
        t->hdr = 0;
        if (!t->h[0]) continue;                         /* the end blocks */
        int bad = 0;
        u32 size = octal(t->h + 124, 12, &bad);
        if (bad || memcmp(t->h + 257, "ustar", 5)) { say("package %s: a damaged tar header", t->k->name); return t->err = -EINVAL; }
        char name[PMAX + 1];
        name[0] = '/';
        u32 nl = 0;
        while (nl < 100 && t->h[nl]) nl++;
        if (t->h[345] || nl >= PMAX) { say("package %s: a name too long", t->k->name); return t->err = -EINVAL; }
        memcpy(name + 1, t->h, nl); name[nl + 1] = 0;
        t->left = size;
        t->pad = (512 - size % 512) % 512;
        t->o = (t->h[156] == '0' || t->h[156] == 0) ? op_for(t->k, name) : 0;
        t->fd = -1;
        if (t->o) {
            char nw[PMAX];
            if (zupnew(nw, t->o->path)) return t->err = -ENAMETOOLONG;
            t->fd = sys_open(nw, O_WRONLY | O_CREAT | O_TRUNC);
            if (t->fd < 0) { say("%s: cannot create it (%d)", nw, t->fd); return t->err = t->fd; }
            sha256_init(&t->c);
            if (!size && (t->err = new_close(t))) return t->err;
        }
    }
    return t->err;
}
/* bytes of k's tgz from pos: in place (the bundle in memory: all the rest) or a piece read from the file */
static int tgz_at(struct zf_pkg *k, u32 pos, const u8 **p, u32 *n)
{
    if (k->tgz) { *p = k->tgz + pos; *n = k->tgzlen - pos; return 0; }
    u32 want = k->tgzlen - pos < sizeof inbuf ? k->tgzlen - pos : sizeof inbuf;
    int r = sys_lseek(tgzfd, (int)(tgzoff + pos), 0);
    if (r >= 0) r = sys_read(tgzfd, inbuf, want);
    if (r != (int)want) { say("package %s: cannot read its files (%d)", k->name, r < 0 ? r : -EIO); return -EIO; }
    *p = inbuf; *n = want;
    return 0;
}
static int unpack(struct zf_pkg *k)
{
    static u8 *dict;
    static tinfl_decompressor *dec;
    if (!dict) dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!dec) dec = malloc(sizeof *dec);
    if (!dict || !dec) { say("out of memory"); return -ENOMEM; }
    const u8 *in;
    u32 len = k->tgzlen, pos = 10, inn = 0;
    if (len < 18) { say("package %s: not gzip data", k->name); return -EINVAL; }
    if (tgz_at(k, 0, &in, &inn)) return -EIO;
    if (in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { say("package %s: not gzip data", k->name); return -EINVAL; }
    u32 flg = in[3];                                    /* the header's fields lie in the first piece */
    if (flg & 4) { if (pos + 2 > inn) return -EINVAL; pos += 2 + (in[pos] | (u32)in[pos + 1] << 8); }
    if (flg & 8) while (pos < inn && in[pos++]) ;
    if (flg & 16) while (pos < inn && in[pos++]) ;
    if (flg & 2) pos += 2;
    if (pos >= inn) { say("package %s: a damaged gzip header", k->name); return -EINVAL; }
    in += pos; inn -= pos;
    static struct tarst t;
    memset(&t, 0, sizeof t);
    t.fd = -1; t.k = k;
    tinfl_init(dec);
    size_t dofs = 0;
    for (;;) {
        if (!inn && pos < len && tgz_at(k, pos, &in, &inn)) { t.err = -EIO; break; }
        size_t in_n = inn, out_n = TINFL_LZ_DICT_SIZE - dofs;
        int more = pos + inn < len;
        tinfl_status st = tinfl_decompress(dec, in, &in_n, dict, dict + dofs, &out_n, more ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        pos += (u32)in_n; in += in_n; inn -= (u32)in_n;
        if (out_n && tar_feed(&t, dict + dofs, (u32)out_n)) break;
        dofs = (dofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) break;
        if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && pos >= len)) { say("package %s: damaged compressed data", k->name); t.err = -EINVAL; break; }
    }
    if (t.fd >= 0) sys_close(t.fd);
    if (t.err) return t.err;
    if (t.left || t.hdr) { say("package %s: its archive ends in the middle of a file", k->name); return -EINVAL; }
    for (int i = 0; i < nops; i++)
        if (ops[i].pk == k && ops[i].mpath && !ops[i].done) { say("package %s: %s is not in its archive", k->name, ops[i].mpath); return -EINVAL; }
    return 0;
}
static int write_data(void)                             /* the manifests: the packages' own, and rewritten taken ones */
{
    for (int i = 0; i < nops; i++) {
        struct op *o = &ops[i];
        if (!o->data || o->done) continue;
        char nw[PMAX];
        u8 h[32];
        int r;
        if (zupnew(nw, o->path)) return -ENAMETOOLONG;
        /* through our own buffer: the bundle is no process's memory, a system call refuses it (-EFAULT) */
        int fd = sys_open(nw, O_WRONLY | O_CREAT | O_TRUNC);
        r = fd < 0 ? fd : 0;
        for (u32 at = 0; !r && at < o->datalen; at += sizeof iobuf) {
            u32 k = o->datalen - at < sizeof iobuf ? o->datalen - at : sizeof iobuf;
            memcpy(iobuf, o->data + at, k);
            r = write_all(fd, iobuf, k);
        }
        if (fd >= 0) sys_close(fd);
        if (r < 0) { say("%s: cannot write it (%d)", nw, r); return r; }
        if (sha_file(nw, h) || memcmp(h, o->sha, 32)) { say("%s: what was written does not read back right", nw); return -EIO; }
        sys_chown(nw, 0, 0);
        sys_chmod(nw, 0644);
        o->done = 1;
    }
    return 0;
}

/* after the commit record: the older prev sets go, the journal goes, the volume is clean again (repeatable). A live
 * transaction's commit never touches a prev set (D22 4.2): only its own live/ is emptied */
static void post_commit(int wasclean)
{
    if (live) rm_tree(prevdir);
    else {
        char **names;
        int n = dir_list(CACHE, &names);
        char me[24];
        snprintf(me, sizeof me, "prev-%u", target_v);
        for (int i = 0; i < n; i++)
            if (!strncmp(names[i], "prev-", 5) && strcmp(names[i], me)) {
                char p[PMAX];
                if (!pjoin(p, CACHE "/", names[i])) rm_tree(p);
            }
        dir_free(names, n);
    }
    sys_unlink(JOURNAL);
    sys_sync();
    if (wasclean) fs_mark(0);
}
static void undo_cleanup(int wasclean)                  /* a transaction rolled back: its prev links and log go */
{
    char p[PMAX], t[12];
    for (int i = 0; i < nops; i++)
        if (ops[i].link[0] != '-' && !prev_of(p, &ops[i])) sys_unlink(p);
    if (live) rm_tree(prevdir);
    else { snprintf(t, sizeof t, "/log.%u", tnum); if (!pjoin(p, prevdir, t)) sys_unlink(p); }
    sys_unlink(JOURNAL);
    sys_sync();
    if (wasclean) fs_mark(0);
}
static int write_log(void)                              /* prev-V/log.T: the undo record of this transaction */
{
    char p[PMAX], t[12], *txt;
    u32 len;
    if (live) return 0;                                 /* a live transaction keeps no undo record (D22 4.2) */
    snprintf(t, sizeof t, "/log.%u", tnum);
    if (pjoin(p, prevdir, t) || ops_text(&txt, &len)) return -ENOMEM;
    int r = write_file(p, txt, len);
    free(txt);
    return r;
}
static void set_prevdir(u32 v)
{
    if (live) strcpy(prevdir, CACHE "/live");
    else snprintf(prevdir, sizeof prevdir, CACHE "/prev-%u", v);
}

/* one transaction, planned in ops[] (target_v, tnum, prevdir set). 0, or -errno (then it was rolled back) */
static int run_transaction(struct zf_pkg **list, int n)
{
    char *txt, head[96], p[PMAX];
    u32 len;
    int r, wasclean = fs_mark(1) == 1;
    if ((r = ops_text(&txt, &len))) return r;
    mkdirs(DB);
    mkdirs(CACHE);
    if (live) {
        rm_tree(prevdir);                               /* what a cut live transaction may have left */
        snprintf(head, sizeof head, "zupj 1\nlive 1 %s\ntarget %u %u %d\n", livename, target_v, tnum, wasclean);
    } else
        snprintf(head, sizeof head, "zupj 1\ntarget %u %u %d\n", target_v, tnum, wasclean);
    /* a live journal is made with O_EXCL: it can never be another transaction's (recover() ran under the lock) */
    int fd = sys_open(JOURNAL, O_WRONLY | O_CREAT | (live ? O_EXCL : O_TRUNC));
    if (fd < 0 || (r = write_all(fd, head, strlen(head))) || (r = write_all(fd, txt, len)) || (r = write_all(fd, "end\n", 4))) {
        if (fd >= 0) sys_close(fd);
        free(txt);
        say("cannot write the journal (%d): nothing changed", fd < 0 ? fd : r);
        if (fd != -EEXIST) sys_unlink(JOURNAL);
        if (wasclean) fs_mark(0);
        return fd < 0 ? fd : r;
    }
    sys_close(fd);
    free(txt);
    sys_sync();
    if (!live) for (int i = 0; i < n; i++) do_dirs(list[i]);
    for (int i = 0; live && i < nops; i++)              /* live: the new directories first, journaled (M) */
        if (ops[i].op == 'M' && (r = finish_op(&ops[i])) < 0) { say("%s: cannot make the directory (%d)", ops[i].path, r); goto back; }
    mkdirs(prevdir);
    if (pjoin(p, prevdir, "/f")) return -ENAMETOOLONG;
    sys_mkdir(p);
    for (int i = 0; i < nops; i++) {                    /* the previous versions, by hard link */
        if (ops[i].link[0] == '-') continue;
        prev_of(p, &ops[i]);
        sys_unlink(p);
        if ((r = sys_link(ops[i].path, p)) < 0) { say("%s: cannot keep the previous version (%d)", ops[i].path, r); goto back; }
    }
    for (int i = 0; i < n; i++)
        if ((r = unpack(list[i]))) goto back;
    if ((r = write_data())) goto back;
    for (int i = 0; i < nops; i++)
        if ((r = finish_op(&ops[i])) < 0) { say("%s: cannot put it in place (%d)", ops[i].path, r); goto back; }
    if ((r = write_log()) < 0) { say("cannot write the undo log (%d)", r); goto back; }
    sys_sync();
    if ((r = journal_append("commit\n"))) goto back;
    post_commit(wasclean);
    return 0;
back:
    journal_append("rollback\n");
    undo_all();
    undo_cleanup(wasclean);
    return r ? r : -EIO;
}

/* ---- after a power cut: the journal ---- */
static int recover(void)
{
    u32 len;
    char *t = slurp(JOURNAL, &len);
    if (!t) return 0;
    say("a journal from an install that did not end: finishing it or rolling it back");
    u32 v = 0, tn = 0; int undo = 0;
    int ended = len >= 4 && !strcmp(t + len - 4, "end\n");
    int committed = strstr(t, "\ncommit\n") != 0, rolling = strstr(t, "\nrollback\n") != 0;
    if (!ended && !committed && !rolling) {             /* cut while the journal was written: nothing had changed */
        say("the journal was cut short: nothing had been changed");
        sys_unlink(JOURNAL);
        free(t);
        return 0;
    }
    /* a live transaction (D22 4.2) says so in its second line, before any operation: its directory is live/, never
     * a prev set, and its end never touches one */
    int badlive = 0;
    live = !strncmp(t, "zupj 1\nlive 1 ", 14);
    if (live) {
        char *e = strchr(t + 14, '\n');
        u32 nl = e ? (u32)(e - (t + 14)) : 0;
        if (!e || nl >= sizeof livename) badlive = 1;
        else { memcpy(livename, t + 14, nl); livename[nl] = 0; badlive = !zf_name_ok(livename); }
    }
    char *l = strstr(t, "\ntarget ");
    char *u = live ? 0 : strstr(t, "\nundo ");
    /* the volume stays marked "needs a check" after a power cut in the middle of an install: a block write may have
     * been cut too (at worst a leaked inode, D15 step 0), which fsck.ext2 finds */
    if (l) { char *sp; v = (u32)strtoul(l + 8, &sp, 10); tn = (u32)strtoul(sp, &sp, 10); }
    else if (u) { v = (u32)strtoul(u + 6, 0, 10); undo = 1; }
    if (badlive || (live && tn != 1)) v = 0;
    if (!v || parse_ops(t) < 0) { say("the journal is damaged: removed (check the root with fsck.ext2)"); sys_unlink(JOURNAL); free(t); live = 0; return -EINVAL; }
    target_v = v; tnum = tn;
    set_prevdir(v);
    if (undo) {                                         /* a rollback to the previous version, cut: go on with it */
        undo_all();
        rm_tree(prevdir);
        sys_unlink(JOURNAL);
        sys_sync();
        snprintf(result, sizeof result, "zup: rolled back to the version before %u (finished after a power cut)", v);
        free(t);
        return 1;
    }
    if (committed) {
        post_commit(0);
        if (live) say("the change of package %s was done (finished after a power cut)", livename);
        else snprintf(result, sizeof result, "zup: the update to %u was installed (finished after a power cut)", v);
        free(t);
        live = 0;
        return 1;
    }
    int can = !rolling;
    for (int i = 0; can && i < nops; i++) {             /* can it be finished? every new file complete and checked */
        struct op *o = &ops[i];
        char nw[PMAX];
        if (o->op != 'R' && o->op != 'A') continue;
        if (zupnew(nw, o->path) || !(file_has(nw, o->sha) || file_has(o->path, o->sha))) can = 0;
    }
    if (can) {
        int r = 0;
        for (int i = 0; i < nops && !r; i++) r = finish_op(&ops[i]) < 0;
        if (!r && write_log() >= 0 && journal_append("commit\n") == 0) {
            post_commit(0);
            if (live) say("finished the change of package %s after a power cut", livename);
            else {
                say("finished the install of %u", v);
                snprintf(result, sizeof result, "zup: the update to %u was installed (finished after a power cut)", v);
            }
            free(t);
            live = 0;
            return 1;
        }
    }
    if (!rolling) journal_append("rollback\n");
    undo_all();
    undo_cleanup(0);
    if (live) say("rolled the unfinished change of package %s back (zpkg can do it again)", livename);
    else {
        say("rolled the unfinished install of %u back", v);
        snprintf(result, sizeof result, "zup: the update to %u was cut off and rolled back; it is tried again at the next start", v);
    }
    failed = 1;
    free(t);
    live = 0;
    return 1;
}

/* ---- "Zenix (previous)": the newest prev set back ---- */
static u32 newest_prev(void)
{
    char **names;
    int n = dir_list(CACHE, &names);
    u32 best = 0;
    for (int i = 0; i < n; i++)
        if (!strncmp(names[i], "prev-", 5)) { u32 v = (u32)strtoul(names[i] + 5, 0, 10); if (v > best) best = v; }
    dir_free(names, n);
    return best;
}
static int rollback(u32 v)
{
    char p[PMAX], t[12];
    set_prevdir(v);
    /* the undo logs, newest transaction first, each backwards: one journal */
    u32 cap = 4096, len = 0;
    char *j = malloc(cap);
    if (!j) return -ENOMEM;
    int wasclean = fs_mark(1) == 1;
    len = (u32)snprintf(j, cap, "zupj 1\nundo %u\nclean %d\n", v, wasclean);
    for (u32 tn = 1; tn <= 16; tn++) {
        snprintf(t, sizeof t, "/log.%u", tn);
        u32 ll;
        if (pjoin(p, prevdir, t)) break;
        char *lg = slurp(p, &ll);
        if (!lg) continue;
        if (len + ll + 16 > cap) { cap = (len + ll) * 2 + 16; char *nj = realloc(j, cap); if (!nj) { free(lg); free(j); return -ENOMEM; } j = nj; }
        memcpy(j + len, lg, ll); len += ll;
        if (ll && j[len - 1] != '\n') j[len++] = '\n';
        free(lg);
    }
    memcpy(j + len, "end\n", 4); len += 4;
    int r = write_file(JOURNAL, j, len);
    sys_sync();
    if (r < 0) { free(j); say("cannot write the journal (%d): nothing changed", r); return r; }
    j[len] = 0;
    parse_ops(j);
    undo_all();                                         /* the last transaction's last change first */
    rm_tree(prevdir);
    sys_unlink(JOURNAL);
    sys_sync();
    if (wasclean) fs_mark(0);
    free(j);
    return 0;
}

/* ---- the update ---- */
static int dep_ok(struct zf_bundle *b, const char *name, u32 v)
{
    for (int i = 0; b && i < b->npkg; i++) if (!strcmp(b->pk[i].name, name) && b->pk[i].vnum >= v) return 1;
    struct zf_pkg *k = installed(name);
    return k && k->vnum >= v;
}
/* room on the root for what the plan writes (new files and manifests; the old ones stay as prev links) */
static int space_ok(void)
{
    u32 need = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'R' || ops[i].op == 'A') {
            struct zf_ent *e = ops[i].mpath ? zf_find_ent(ops[i].pk, ops[i].mpath) : 0;
            need += (e ? e->size : ops[i].datalen) + 1024;
        }
    struct ustatfs fs;
    if (sys_statfs(&fs) == 0 && fs.bsize >= 1024 && (fs.bfree * (fs.bsize / 1024) < need / 1024 + 64 || fs.ffree < (u32)nops + 16)) {
        say("not enough space on the root: %u KB and %d files needed", need / 1024 + 64, nops + 16);
        return -ENOSPC;
    }
    return 0;
}
static int install(struct zf_bundle *b, u32 kvnum, int stage2)
{
    struct zf_pkg *todo[MAXPKG];
    int ntodo = 0, need_zup = 0;
    u32 base = base_version();
    if (b->vnum < base) { say("the bundle (%u) is older than the installed base (%u): refused", b->vnum, base); return -EINVAL; }
    if (b->minv > base) { say("the bundle needs base %u or newer, %u is installed: refused", b->minv, base); return -EINVAL; }
    for (int i = 0; i < b->npkg; i++) {
        struct zf_pkg *k = &b->pk[i], *old = installed(k->name);
        if (old && old->vnum > k->vnum) { say("%s %u is older than the installed %u: refused", k->name, k->vnum, old->vnum); return -EINVAL; }
        if (k->group && !strcmp(k->group, "base") && k->vnum > kvnum) {
            say("%s %u is newer than the running kernel (%u): the kernel goes first (zenixboot): refused", k->name, k->vnum, kvnum);
            return -EINVAL;
        }
        if (k->ntakes) { say("%s takes another package's files: only zup -f does that (D22 8.4.4): refused", k->name); return -EINVAL; }
        if (old && old->vnum == k->vnum) continue;
        for (int d = 0; d < k->ndep; d++)
            if (!dep_ok(b, k->dep[d], k->depv[d])) { say("%s needs %s %u or newer: refused", k->name, k->dep[d], k->depv[d]); return -EINVAL; }
        todo[ntodo++] = k;
        if (!strcmp(k->name, "zup")) need_zup = 1;
    }
    if (!ntodo) { say("version %u is installed already: nothing to do", b->vnum); return 1; }
    target_v = b->vnum;
    set_prevdir(target_v);
    tnum = 1;
    for (; tnum < 16; tnum++) {                         /* an earlier transaction of this version (zup's own) */
        char p[PMAX], t[12];
        struct ustat st;
        snprintf(t, sizeof t, "/log.%u", tnum);
        if (pjoin(p, prevdir, t) || sys_stat(p, &st) < 0) break;
    }
    nops = 0; nkept = 0;
    for (int i = 0; i < ntodo; i++) { int r = plan_pkg(todo[i]); if (r) return r; }
    int r = space_ok();
    if (r) return r;
    if (need_zup && !stage2 && ntodo > 1) {             /* zup first, then the new zup does the rest */
        struct zf_pkg *z[1];
        for (int i = 0; i < ntodo; i++) if (!strcmp(todo[i]->name, "zup")) z[0] = todo[i];
        nops = 0; nkept = 0;
        r = plan_pkg(z[0]);
        if (r) return r;
        say("installing zup %u first", z[0]->vnum);
        if ((r = run_transaction(z, 1))) { say("zup %u was not installed (%d): rolled back", z[0]->vnum, r); return r; }
        char *argv[] = { "/sbin/zup", "-2", 0 };
        int pid = sys_spawn("/sbin/zup", argv, 0);
        if (pid < 0) { say("cannot start the new /sbin/zup (%d)", pid); return pid; }
        int st = sys_wait(pid);
        return st == 0 ? 2 : st == 1 ? 3 : -EIO;        /* 2/3: the new zup did the rest (and wrote the result) */
    }
    say("installing %u (%s): %d packages, %d changes", b->vnum, b->vstr, ntodo, nops);
    r = run_transaction(todo, ntodo);
    if (r) { say("the install of %u failed (%d) and was rolled back", b->vnum, r); return r; }
    char names[80];
    u32 nl = 0;
    names[0] = 0;
    for (int i = 0; i < ntodo && nl < sizeof names - 26; i++) nl += (u32)snprintf(names + nl, sizeof names - nl, " %s", todo[i]->name);
    int n = snprintf(result, sizeof result, "zup: installed Zenix %s (%u):%s%s, %d changes", b->vstr, b->vnum,
                     stage2 ? " zup and" : "", names, nops);
    if (nkept) snprintf(result + n, sizeof result - (u32)n, "; kept your %s%s (the new one: %s.new)", kept[0],
                        nkept > 1 ? " and others" : "", kept[0]);
    say("%s", result + 5);
    return 0;
}

/* ---- live transactions: zup -f FILE and zup -r NAME (DESIGN D22 4.2) ----
 * zup trusts no caller: every rule is its own. Never a base-group package (by its group and by name), never a
 * package the newest prev set's undo logs touch (the system's way back would put half of it back), never a path
 * another package owns (unless taken), under the package database or the cache, or below a symbolic link. */
static int base_name(const char *n) { return !strcmp(n, "zup") || !strcmp(n, "zpkg") || !strcmp(n, "base"); }
static int in_prev(const char *name)                    /* its manifest in the newest prev set's undo logs */
{
    u32 pv = newest_prev();
    if (!pv) return 0;
    char dir[PMAX], p[PMAX], t[12], want[PMAX + 2];
    snprintf(dir, sizeof dir, CACHE "/prev-%u", pv);
    snprintf(want, sizeof want, " " DB "/%s.manifest\n", name);
    int hit = 0;
    for (u32 tn = 1; tn <= 16 && !hit; tn++) {
        u32 ll;
        snprintf(t, sizeof t, "/log.%u", tn);
        if (pjoin(p, dir, t)) break;
        char *lg = slurp(p, &ll);
        if (lg && strstr(lg, want)) hit = 1;
        free(lg);
    }
    return hit;
}
static int live_refused(struct zf_pkg *k, const char *what)    /* 0, or -EPERM (said) */
{
    if ((k->group && !strcmp(k->group, "base")) || base_name(k->name)) {
        say("%s is in the group base: it changes only with the system (a system update, then a restart): refused", k->name);
        return -EPERM;
    }
    if (in_prev(k->name)) {
        say("%s is part of the previous system the boot menu can go back to: it %s with the next system update", k->name, what);
        return -EPERM;
    }
    return 0;
}
static int under(const char *path, const char *dir)
{
    u32 n = strlen(dir);
    return !strncmp(path, dir, n) && (path[n] == '/' || !path[n]);
}
static int paths_ok(struct zf_pkg *k)
{
    struct zf_pkg *old = installed(k->name);
    for (int i = 0; i < k->nent; i++) {
        struct zf_ent *e = &k->ent[i];
        if (under(e->path, DB) || under(e->path, CACHE)) { say("%s: a package may not write there: refused", e->path); return -EPERM; }
        char b[PMAX];
        u32 n = strlen(e->path);
        memcpy(b, e->path, n + 1);
        for (u32 j = 1; j < n; j++) {                   /* no symbolic link in a directory on the way */
            struct ustat st;
            if (b[j] != '/') continue;
            b[j] = 0;
            if (sys_lstat(b, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK) {
                say("%s: %s is a symbolic link: refused", e->path, b);
                return -EPERM;
            }
            b[j] = '/';
        }
        int mine = zf_find_ent(old, e->path) != 0;
        for (int j = 0; j < ndb; j++) {                 /* another package's path: d may be shared (pacman's rule) */
            struct zf_ent *x;
            if (db[j] == old || !(x = zf_find_ent(db[j], e->path))) continue;
            if ((x->kind == 'd') != (e->kind == 'd')) { say("%s: a directory in one package and not in %s: refused", e->path, db[j]->name); return -EEXIST; }
            if (x->kind == 'd') continue;
            if (!takes(k, db[j]->name)) { say("%s belongs to package %s: refused", e->path, db[j]->name); return -EEXIST; }
            mine = 1;
        }
        struct ustat st;
        if (!mine && e->kind != 'd' && sys_lstat(e->path, &st) == 0) {
            say("%s exists and belongs to no package: refused (move it away first)", e->path);
            return -EEXIST;
        }
    }
    return 0;
}
/* the file's package bundle (D22 3.1: no kernel, one package), read without holding it in memory: the header and
 * table, the manifest (copied: kept as is for the database, and parsed), the tgz's SHA-256 a piece at a time */
static int read_pkgfile(const char *file, struct zf_bundle *b, struct zf_pkg *k)
{
    struct ustat st;
    u8 hdr[ZF_HDR + 2 * ZF_TOCE];
    int fd = sys_open(file, O_RDONLY), r = -EINVAL;
    if (fd < 0) { say("%s: cannot open it (%d)", file, fd); return fd; }
    if (sys_fstat(fd, &st) < 0 || (st.mode & S_IFMT) != S_IFREG || st.size > (16u << 20)) {
        say("%s: not a package file (a regular file up to 16 MB)", file); goto out;
    }
    unsigned need = ZF_HDR, have = 0;
    for (;;) {
        if (need > sizeof hdr) { say("%s: not a package file (one package, two parts): a system bundle is installed by zenixboot at a restart", file); goto out; }
        int n = sys_read(fd, hdr + have, need - have);
        if (n < 0 || (u32)n != need - have) { say("%s: the bundle is shorter than its header", file); goto out; }
        have = need;
        r = zf_bundle_head(b, hdr, have, st.size, &need);
        if (r != -EAGAIN) break;
        r = -EINVAL;
    }
    if (r) goto out;
    r = -EINVAL;
    const u8 *msha = 0, *tsha = 0;
    u32 moff = 0, mlen = 0, toff = 0, tlen = 0;
    char pn[25] = "";
    for (unsigned i = 0; i < b->ntoc; i++) {
        char name[41], pname[25];
        unsigned off, size;
        const unsigned char *sha;
        if (zf_bundle_entry(b, i, name, &off, &size, &sha)) goto out;
        int kind = zf_part_kind(name, pname);
        if (kind < 0) goto out;
        if (!kind) { say("%s: a system bundle (%s): it is installed by zenixboot at a restart, not here", file, name); goto out; }
        if (pn[0] && strcmp(pn, pname)) { say("%s: a package file holds one package", file); goto out; }
        strcpy(pn, pname);
        if (kind == 1) { if (msha) goto dup; msha = sha; moff = off; mlen = size; }
        else { if (tsha) goto dup; tsha = sha; toff = off; tlen = size; }
    }
    if (!msha || !tsha) { say("package %s: its manifest or its files are missing", pn); goto out; }
    if (mlen > (1u << 20)) { say("package %s: its manifest is too big", pn); goto out; }
    u8 *man = malloc(mlen + 1);
    char *text = malloc(mlen + 1);
    if (!man || !text) { free(man); free(text); say("out of memory"); r = -ENOMEM; goto out; }
    u8 h[32];
    if (sys_lseek(fd, (int)moff, 0) < 0 || sys_read(fd, man, mlen) != (int)mlen) { free(man); free(text); say("%s: cannot read it", file); r = -EIO; goto out; }
    sha256(man, mlen, h);
    if (memcmp(h, msha, 32)) { free(man); free(text); say("pkg/%s.manifest: SHA-256 does not match (the bundle is damaged)", pn); goto out; }
    memcpy(text, man, mlen); text[mlen] = 0;
    if (strlen(text) != mlen) { free(man); free(text); say("package %s: a NUL in its manifest", pn); goto out; }
    if (zf_manifest_parse(k, text, mlen, pn)) { free(man); zf_pkg_free(k); goto out; }
    k->man = man; k->manlen = mlen;
    if (strcmp(k->name, pn)) { say("package %s: its manifest names %s", pn, k->name); goto out; }
    struct sha256 c;                                    /* the tgz: its SHA-256 now, inflated later (a second pass) */
    sha256_init(&c);
    if (sys_lseek(fd, (int)toff, 0) < 0) { say("%s: cannot read it", file); r = -EIO; goto out; }
    for (u32 at = 0; at < tlen; ) {
        u32 want = tlen - at < sizeof inbuf ? tlen - at : sizeof inbuf;
        int n = sys_read(fd, inbuf, want);
        if (n != (int)want) { say("%s: cannot read it", file); r = -EIO; goto out; }
        sha256_update(&c, inbuf, want);
        at += want;
    }
    sha256_final(&c, h);
    if (memcmp(h, tsha, 32)) { say("pkg/%s.tgz: SHA-256 does not match (the bundle is damaged)", pn); goto out; }
    k->tgz = 0; k->tgzlen = tlen;
    tgzfd = fd; tgzoff = toff;
    return 0;
dup:
    say("%s: a part twice", file);
out:
    sys_close(fd);
    return r;
}
static int live_begin(const char *name, u32 v)
{
    live = 1;
    strcpy(livename, name);
    target_v = v; tnum = 1;
    set_prevdir(v);
    nops = 0; nkept = 0;
    return 0;
}
static int install_file(const char *file, int down, u32 effbase)
{
    static struct zf_bundle b;
    static struct zf_pkg k;
    int r = read_pkgfile(file, &b, &k);
    if (r) return r;
    struct zf_pkg *old = installed(k.name);
    installed_all();
    if (dberr) { say("the package database cannot be read: nothing changed"); return -EIO; }
    if (b.vnum != k.vnum) { say("package %s: the bundle says version %u, its manifest %u: refused", k.name, b.vnum, k.vnum); return -EINVAL; }
    if ((r = live_refused(&k, "changes"))) return r;
    for (int t = 0; t < k.ntakes; t++) {
        struct zf_pkg *ot = installed(k.takes[t]);
        if (base_name(k.takes[t]) || (ot && ot->group && !strcmp(ot->group, "base"))) {
            say("%s takes %s, a base-group package: refused", k.name, k.takes[t]);
            return -EPERM;
        }
        if (ot && (r = live_refused(ot, "is taken"))) return r;
    }
    if (old && old->vnum == k.vnum) { say("%s %u is installed already: nothing to do", k.name, k.vnum); return 1; }
    if (old && old->vnum > k.vnum && !down) {
        say("%s %u is older than the installed %u: refused (zpkg --back asks for an older one)", k.name, k.vnum, old->vnum);
        return -EINVAL;
    }
    if (b.minv > effbase) { say("%s needs base %u or newer, the system is %u: refused", k.name, b.minv, effbase); return -EINVAL; }
    for (int d = 0; d < k.ndep; d++)
        if (!dep_ok(0, k.dep[d], k.depv[d])) { say("%s needs %s %u or newer: refused", k.name, k.dep[d], k.depv[d]); return -EINVAL; }
    if ((r = paths_ok(&k))) return r;
    live_begin(k.name, k.vnum);
    if ((r = plan_pkg(&k)) || (r = plan_takes(&k)) || (r = space_ok())) return r;
    struct zf_pkg *list[1] = { &k };
    say("installing %s %u (%s): %d changes", k.name, k.vnum, k.vstr, nops);
    if ((r = run_transaction(list, 1))) { say("the install of %s failed (%d) and was rolled back", k.name, r); return r; }
    say("installed %s %s (%u)%s%s%s", k.name, k.vstr, k.vnum, nkept ? "; kept your " : "", nkept ? kept[0] : "",
        nkept ? (nkept > 1 ? " and others (the new ones: .new)" : " (the new one: .new)") : "");
    return 0;
}
static int remove_pkg(const char *name, int nosave)
{
    if (!zf_name_ok(name)) { say("%s: not a package name", name); return -EINVAL; }
    installed_all();
    if (dberr) { say("the package database cannot be read: nothing changed"); return -EIO; }
    struct zf_pkg *k = installed(name);
    if (!k) { say("%s is not installed", name); return -ENOENT; }
    int r = live_refused(k, "is removed");
    if (r) return r;
    for (int i = 0; i < ndb; i++)                       /* nothing installed may need it (zpkg removes dependents first) */
        for (int d = 0; db[i] != k && d < db[i]->ndep; d++)
            if (!strcmp(db[i]->dep[d], name)) { say("%s needs %s: refused", db[i]->name, name); return -EPERM; }
    live_begin(name, k->vnum);
    if ((r = plan_remove(k, nosave))) return r;
    say("removing %s %u: %d changes", name, k->vnum, nops);
    if ((r = run_transaction(0, 0))) { say("the removal of %s failed (%d) and was rolled back", name, r); return r; }
    say("removed %s%s%s%s", name, nkept ? "; kept your changed " : "", nkept ? kept[0] : "",
        nkept ? (nkept > 1 ? " and others as .save" : " as .save") : "");
    return 0;
}

/* the result line, written under the "needs a check" mark like every other change zup makes: rewriting the file
 * frees and allocates blocks, and a power cut in the middle must not leave its leak on a volume marked clean (F5) */
static void save_result(void)
{
    if (!result[0]) return;
    int wasclean = fs_mark(1) == 1;
    mkdirs(DB);
    u32 n = strlen(result);
    result[n] = '\n';
    write_file(RESULT, result, n + 1);
    result[n] = 0;
    sys_sync();
    if (wasclean) fs_mark(0);
}

static int list(void)
{
    char **names;
    int n = dir_list(DB, &names);
    if (n < 0) { printf("zup: no package database (%s)\n", DB); return 1; }
    for (int i = 0; i < n; i++) {
        u32 l = strlen(names[i]);
        if (l < 10 || strcmp(names[i] + l - 9, ".manifest")) continue;
        names[i][l - 9] = 0;
        struct zf_pkg *k = installed(names[i]);
        if (k) printf("%-12s %u (%s)%s\n", k->name, k->vnum, k->vstr, k->group ? "  base group" : "");
    }
    dir_free(names, n);
    u32 pv = newest_prev();
    if (pv) printf("previous version kept: the one before %u (Zenix (previous) in the boot menu)\n", pv);
    return 0;
}

static int usage(void)
{
    printf("usage: zup [-l]          (boot runs zup at every start; see man zup)\n"
           "       zup -f FILE [-d]  install one package file (zpkg runs this)\n"
           "       zup -r NAME [-n]  remove one package\n");
    return 2;
}
static int live_main(int argc, char **argv)            /* zup -f FILE [-d], zup -r NAME [-n] */
{
    int f = !strcmp(argv[1], "-f"), opt = 0;
    if (argc < 3 || argc > 4) return usage();
    if (argc == 4) {
        if (strcmp(argv[3], f ? "-d" : "-n")) return usage();
        opt = 1;
    }
    if (sys_getuid() != 0) { printf("zup: only root installs or removes packages\n"); return 1; }
    signal(SIGINT, SIG_IGN); signal(SIGHUP, SIG_IGN); signal(SIGTERM, SIG_IGN);    /* never ended half-way */
    struct uupdate u;
    memset(&u, 0, sizeof u);
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0) { say("this kernel has no updates (SYS_update): nothing done"); return 1; }
    if (u.flags & UPD_F_TRIAL) {
        say("a new system is on trial (not confirmed yet): no package changes now, \"Zenix (previous)\" would break them");
        return 1;
    }
    struct ustat st;
    if (sys_stat(DB, &st) < 0) { say("no package database here (%s): nothing done", DB); return 1; }
    if (lock_take()) return 1;
    int r = recover() < 0 ? -EIO : 0;
    if (r) say("nothing done: check the root (fsck.ext2) first");
    else {
        u32 base = base_version(), eff = base < u.kvnum ? base : u.kvnum;
        r = f ? install_file(argv[2], opt, eff) : remove_pkg(argv[2], opt);
    }
    lock_drop();
    return r < 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);           /* DESIGN D16 rule 11: `zup | less` and quitting less never end an update half-way */
    int stage2 = argc > 1 && !strcmp(argv[1], "-2");
    if (argc > 1 && !strcmp(argv[1], "-l")) return list();
    if (argc > 1 && (!strcmp(argv[1], "-f") || !strcmp(argv[1], "-r"))) return live_main(argc, argv);
    if (argc > 1 && !stage2) return usage();
    if (sys_getuid() != 0) { printf("zup: only root installs updates\n"); return 1; }
#ifdef DIAG_SLOWZUP     /* test builds (tools/update-test.sh slowzup): a stage 2 that hangs, for boot's time limit */
    if (stage2) { say("diag: DIAG_SLOWZUP: stage 2 hangs"); for (;;) sys_sleep(100); }   /* say, as every message: the
                                                                 * slowzup test sees it after boot stopped this zup */
#endif
    struct uupdate u;
    memset(&u, 0, sizeof u);
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0) return 0;      /* a kernel without updates: nothing to do */
    /* D22 4.2: the lock, when there is a journal to finish or a way back to take and no bundle (with a bundle zup runs
     * only at boot, where nothing else does; without one a zup started by hand must never take a running live
     * transaction's journal for a cut one). A lock left from before the restart is stale */
    struct ustat st;
    u32 base = base_version(), pv = newest_prev();
    if (!(u.addr && u.size) && (sys_lstat(JOURNAL, &st) == 0 || (pv > u.kvnum && ((u.flags & UPD_F_ROLLBACK) || base > u.kvnum)))
        && lock_take())
        return 1;
    recover();
    base = base_version(); pv = newest_prev();
    if (pv > u.kvnum && ((u.flags & UPD_F_ROLLBACK) || base > u.kvnum)) {
        say("%s: putting the version before %u back", (u.flags & UPD_F_ROLLBACK) ? "Zenix (previous) was chosen" :
            "the installed base is newer than the kernel", pv);
        int r = rollback(pv);
        if (r == 0) snprintf(result, sizeof result, "zup: rolled back to the version before %u", pv);
        else { snprintf(result, sizeof result, "zup: the rollback to the version before %u failed (%d)", pv, r); failed = 1; }
        say("%s", result + 5);
        save_result();
        lock_drop();
        return failed;
    } else if (u.flags & UPD_F_ROLLBACK) {
        say("Zenix (previous) was chosen: the root has nothing newer than the kernel (%u) to roll back", u.kvnum);
    }
    if (!u.addr || !u.size) { save_result(); lock_drop(); return failed; }
    static struct zf_bundle b;
    int r = zf_bundle_parse(&b, (const u8 *)u.addr, u.size);
    if (r == 0) r = install(&b, u.kvnum, stage2);
    if (r == 2 || r == 3) { lock_drop(); return r - 2; }   /* the new zup did it, result and note included */
    if (r < 0) {
        snprintf(result, sizeof result, "zup: the update to %u was not installed: %s", b.vnum, lastmsg);
        failed = 1;
    }
    sys_update(UPD_NOTE, r < 0 ? UPDNOTE_FAILED : UPDNOTE_DONE, b.vnum);
    save_result();
    lock_drop();
    return failed;
}
