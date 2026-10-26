// SPDX-License-Identifier: GPL-2.0-only
/* zup: installs the root half of an update (DESIGN D15, docs/d15-self-updates.md section 5; docs/man/zup.txt).
 * boot runs it at every start, after the root is mounted and before anything else (user/boot.c):
 *   1. a journal left by a power cut: finish the install (every new file complete and checked) or roll it back;
 *   2. "Zenix (previous)" (the rollback flag) or a kernel older than the installed base: put the previous version
 *      back from /var/cache/zpkg/prev-V;
 *   3. a bundle zenixboot left in memory (SYS_update INFO): check all of it before any change (SHA-256 of every part,
 *      versions, dependencies, space), install a newer zup first and run it for the rest, then the other packages
 *      in one transaction.
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

#define DB       "/var/lib/zpkg"
#define JOURNAL  DB "/journal"
#define RESULT   DB "/result"
#define CACHE    "/var/cache/zpkg"
#define MAXPKG   16
#define MAXDEP   8
#define MAXOPS   1024
#define PMAX     64             /* every path given to a system call is at most 63 bytes */
#define HDR      96
#define TOCE     80

static int failed;              /* an error was reported: the exit status */
static char result[200];        /* the one-line result for init and login (RESULT) */
static char lastmsg[160];       /* the last thing said: the reason a refusal gives in the result */

/* Every message goes out the moment it is said: libz's printf writes each call with one write() (only oputs and
 * oprintf collect output), so a zup that is stopped or loses its power has shown all it said (audit 2026-10-26 F3:
 * checked, it was never buffered; tools/update-test.sh slowzup checks the messages of the zups boot stopped). */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lastmsg, sizeof lastmsg, fmt, ap);
    va_end(ap);
    printf("zup: %s\n", lastmsg);
}

static int has_nul(const void *p, u32 n) { const u8 *q = p; while (n--) if (!*q++) return 1; return 0; }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ---- small helpers ---- */
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
static int parse_sha(const char *s, u8 out[32])
{
    for (int i = 0; i < 32; i++) {
        int a = hexval(s[2 * i]), b = a < 0 ? -1 : hexval(s[2 * i + 1]);
        if (b < 0) return -1;
        out[i] = (u8)(a << 4 | b);
    }
    return s[64] ? -1 : 0;
}
static void put_sha(char *out, const u8 h[32])
{
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = hx[h[i] >> 4]; out[2 * i + 1] = hx[h[i] & 15]; }
    out[64] = 0;
}
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

/* ---- manifests ---- */
struct ent { char kind; u16 mode, uid, gid; u8 sha[32]; u32 size; char *path, *target; };
struct pkg {
    char *name, *vstr, *group, *text;
    u32 vnum, textlen;
    int ndep; char *dep[MAXDEP]; u32 depv[MAXDEP];
    int nent; struct ent *ent;
    const u8 *tgz; u32 tgzlen;              /* in the bundle */
    const u8 *man; u32 manlen;
};

static int path_ok(const char *p)
{
    u32 n = strlen(p);
    if (p[0] != '/' || n < 2 || n > PMAX - 9 || p[n - 1] == '/' || strstr(p, "//") || strstr(p, "/./") || strstr(p, "/../"))
        return 0;
    if (!strcmp(p + n - 2, "/.") || (n >= 3 && !strcmp(p + n - 3, "/..")))    /* a final . or .. (audit3 B6) */
        return 0;
    for (const char *q = p; *q; q++)
        if (!isalnum(*q) && !strchr("._+/-", *q)) return 0;
    return 1;
}
/* a package name (used in paths: the database's NAME.manifest): [a-z0-9][a-z0-9+.-]*, at most 24 (audit3 B6) */
static int name_ok(const char *p)
{
    u32 n = strlen(p);
    if (n < 1 || n > 24 || !((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9'))) return 0;
    for (; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '+' || *p == '.' || *p == '-')) return 0;
    return 1;
}
static u32 num(const char *s, int base, int *bad)
{
    char *e;
    if (!*s) { *bad = 1; return 0; }
    u32 v = (u32)strtoul(s, &e, base);
    if (*e) *bad = 1;
    return v;
}
/* text: a manifest (malloc'd, NUL-terminated; kept by the pkg). 0, or -EINVAL with the reason said */
static int parse_manifest(struct pkg *p, char *text, u32 len, const char *where)
{
    memset(p, 0, sizeof *p);
    p->text = text; p->textlen = len;
    if (strncmp(text, "zpkg 1\n", 7)) { say("%s: not a zpkg 1 manifest", where); return -EINVAL; }
    int cap = 0;
    for (char *l = text, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        char *w[10]; int nw = 0;
        for (char *q = l; *q && nw < 10; ) {
            while (*q == ' ') *q++ = 0;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ') q++;
        }
        if (!nw) continue;
        int bad = 0;
        if (!strcmp(w[0], "name") && nw == 2) { p->name = w[1]; if (!name_ok(w[1])) bad = 1; }
        else if (!strcmp(w[0], "version") && nw == 3) { p->vnum = num(w[1], 10, &bad); p->vstr = w[2]; }
        else if (!strcmp(w[0], "group") && nw == 2) p->group = w[1];
        else if (!strcmp(w[0], "depends") && nw == 3) {
            if (p->ndep == MAXDEP) { say("%s: more than %d dependencies", where, MAXDEP); return -EINVAL; }
            p->dep[p->ndep] = w[1]; p->depv[p->ndep++] = num(w[2], 10, &bad);
            if (!name_ok(w[1])) bad = 1;
        } else if ((!strcmp(w[0], "f") || !strcmp(w[0], "c") || !strcmp(w[0], "d") || !strcmp(w[0], "l")) && nw >= 3) {
            struct ent x;
            memset(&x, 0, sizeof x);
            x.kind = w[0][0];
            if (x.kind == 'f' || x.kind == 'c') {
                if (nw != 7) bad = 1;
                else {
                    x.mode = (u16)num(w[1], 8, &bad); x.uid = (u16)num(w[2], 10, &bad); x.gid = (u16)num(w[3], 10, &bad);
                    if (parse_sha(w[4], x.sha)) bad = 1;
                    x.size = num(w[5], 10, &bad); x.path = w[6];
                }
            } else if (x.kind == 'd') {
                if (nw != 5) bad = 1;
                else { x.mode = (u16)num(w[1], 8, &bad); x.uid = (u16)num(w[2], 10, &bad); x.gid = (u16)num(w[3], 10, &bad); x.path = w[4]; }
            } else {
                if (nw != 3) bad = 1;
                else { x.target = w[1]; x.path = w[2]; if (strlen(x.target) >= PMAX - 9) bad = 1; }
            }
            if (bad || !path_ok(x.path) || (x.mode & ~07777u)) { say("%s: a bad entry (%s)", where, w[0]); return -EINVAL; }
            if (p->nent == cap) {
                cap = cap ? cap * 2 : 64;
                struct ent *ne = realloc(p->ent, (u32)cap * sizeof *ne);
                if (!ne) { say("out of memory"); return -ENOMEM; }
                p->ent = ne;
            }
            p->ent[p->nent++] = x;
        }
        /* any other line: something a later format added (append-only): ignored */
        if (bad) { say("%s: a bad line (%s)", where, w[0]); return -EINVAL; }
    }
    if (!p->name || !p->vstr || !p->vnum) { say("%s: no name or version", where); return -EINVAL; }
    return 0;
}
static struct ent *find_ent(struct pkg *p, const char *path)
{
    for (int i = 0; p && i < p->nent; i++)
        if (!strcmp(p->ent[i].path, path)) return &p->ent[i];
    return 0;
}

/* the installed manifest of a package (/var/lib/zpkg/NAME.manifest), or 0 */
static struct pkg *installed(const char *name)
{
    static struct pkg db[MAXPKG * 2];
    static int ndb;
    for (int i = 0; i < ndb; i++)
        if (!strcmp(db[i].name, name)) return &db[i];
    char path[PMAX];
    u32 len;
    if (pjoin(path, DB "/", name) || pjoin(path, path, ".manifest")) return 0;
    char *t = slurp(path, &len);
    if (!t || ndb == MAXPKG * 2) { free(t); return 0; }
    if (parse_manifest(&db[ndb], t, len, path)) { free(t); return 0; }
    return &db[ndb++];
}
static u32 base_version(void) { struct pkg *b = installed("base"); return b ? b->vnum : 0; }

/* ---- the bundle (in memory, from zenixboot: read in place, never passed to a system call) ---- */
struct bundle { const u8 *p; u32 len, vnum, minv; char vstr[17]; int npkg; struct pkg pk[MAXPKG]; };

static int sha_ok(const u8 *data, u32 len, const u8 *want)
{
    u8 h[32];
    sha256(data, len, h);
    return !memcmp(h, want, 32);
}
static int bundle_parse(struct bundle *b, const u8 *p, u32 len)
{
    memset(b, 0, sizeof *b);
    b->p = p; b->len = len;
    if (len < HDR || memcmp(p, "ZUP1", 4) || be32(p + 4) != 1 || be32(p + 8) != HDR) { say("the bundle is not a ZUP1 bundle"); return -EINVAL; }
    u32 ntoc = be32(p + 52), total = be32(p + 56);
    if (total != len) { say("the bundle is %u bytes, its header says %u", len, total); return -EINVAL; }
    if (be32(p + 48) != HDR || ntoc < 1 || ntoc > 64 || ntoc * TOCE > len - HDR) { say("the bundle's table of contents is bad"); return -EINVAL; }
    struct sha256 c; u8 h[32];
    sha256_init(&c); sha256_update(&c, p, 64); sha256_update(&c, p + HDR, ntoc * TOCE); sha256_final(&c, h);
    if (memcmp(h, p + 64, 32)) { say("the bundle's table of contents does not match its SHA-256"); return -EINVAL; }
    b->vnum = be32(p + 12); b->minv = be32(p + 36);
    memcpy(b->vstr, p + 16, 16); b->vstr[16] = 0;
    for (u32 i = 0; i < ntoc; i++) {
        const u8 *e = p + HDR + i * TOCE;
        u32 off = be32(e + 40), size = be32(e + 44);
        char name[41];
        memcpy(name, e, 40); name[40] = 0;
        if (!has_nul(e, 40)) { say("part %u: its name is not terminated", i); return -EINVAL; }
        if (off < HDR + ntoc * TOCE || off > len || size > len - off) { say("%s: outside the bundle", name); return -EINVAL; }
        if (!sha_ok(p + off, size, e + 48)) { say("%s: SHA-256 does not match (the bundle is damaged)", name); return -EINVAL; }
        if (strncmp(name, "pkg/", 4)) continue;             /* boot/: zenixboot's */
        char *dot = strrchr(name, '.');
        if (!dot || dot - name - 4 < 1 || dot - name - 4 > 24) { say("%s: not a package part", name); return -EINVAL; }
        *dot = 0;
        if (!name_ok(name + 4)) { say("%s: a bad package name (only a-z 0-9 + . -)", name); return -EINVAL; }
        struct pkg *k = 0;
        for (int j = 0; j < b->npkg; j++) if (!strcmp(b->pk[j].name, name + 4)) k = &b->pk[j];
        if (!k) {
            if (b->npkg == MAXPKG) { say("more than %d packages in the bundle", MAXPKG); return -EINVAL; }
            k = &b->pk[b->npkg++];
            k->name = strdup(name + 4);
            if (!k->name) return -ENOMEM;
        }
        if (!strcmp(dot + 1, "manifest")) { k->man = p + off; k->manlen = size; }
        else if (!strcmp(dot + 1, "tgz")) { k->tgz = p + off; k->tgzlen = size; }
        else { say("%s.%s: not a package part", name, dot + 1); return -EINVAL; }
    }
    for (int j = 0; j < b->npkg; j++) {
        struct pkg *k = &b->pk[j];
        char *pname = k->name;
        const u8 *man = k->man, *tgz = k->tgz; u32 ml = k->manlen, tl = k->tgzlen;
        if (!man || !tgz) { say("package %s: its manifest or its files are missing", pname); return -EINVAL; }
        char *t = malloc(ml + 1);
        if (!t) return -ENOMEM;
        memcpy(t, man, ml); t[ml] = 0;
        if (has_nul(t, ml)) { say("package %s: a NUL in its manifest", pname); return -EINVAL; }
        if (parse_manifest(k, t, ml, pname)) return -EINVAL;
        if (strcmp(k->name, pname)) { say("package %s: its manifest names %s", pname, k->name); return -EINVAL; }
        k->man = man; k->manlen = ml; k->tgz = tgz; k->tgzlen = tl;
        free(pname);
    }
    return 0;
}

/* ---- the plan: one transaction's operations ----
 * R replace (the old file kept as a prev link), A add, D delete (prev link), l a new link, L a changed link (the old
 * target kept), u a removed link (its target kept). The journal holds one line an operation:
 *   OP LINK MODE UID GID SHA TARGET OLD PATH         ("-" for what does not apply; LINK is T.N in prev-V/f/) */
struct op {
    char op, link[12];
    u16 mode, uid, gid;
    u8 sha[32]; int hassha;
    char *target, *old, *path;
    struct pkg *pk; const char *mpath;      /* where the new content comes from: pk's tar member mpath, or its manifest */
    int done;
};
static struct op ops[MAXOPS];
static int nops;
static u32 target_v, tnum;                  /* the version being installed and the transaction number within it */
static char prevdir[PMAX];
static int nkept;
static char kept[2][PMAX];

static struct op *add_op(char op, const char *path)
{
    if (nops == MAXOPS) { say("more than %d changes in one update", MAXOPS); return 0; }
    struct op *o = &ops[nops++];
    memset(o, 0, sizeof *o);
    o->op = op;
    o->path = strdup(path);
    if (op == 'R' || op == 'D') snprintf(o->link, sizeof o->link, "%u.%d", tnum, nops);
    else strcpy(o->link, "-");
    return o->path ? o : 0;
}
static int same_meta(const struct ustat *st, const struct ent *e)
{
    return (st->mode & 07777) == e->mode && st->uid == e->uid && st->gid == e->gid;
}
static int plan_pkg(struct pkg *k)
{
    struct pkg *old = installed(k->name);
    char tmp[PMAX];
    for (int i = 0; i < k->nent; i++) {
        struct ent *e = &k->ent[i];
        struct ustat st;
        int have = sys_lstat(e->path, &st) == 0;
        struct op *o;
        if (e->kind == 'd') continue;                   /* made before the files (do_dirs) */
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
            struct ent *was = find_ent(old, e->path);
            if (!was || memcmp(cur, was->sha, 32)) {    /* the owner changed it (or we cannot know): theirs stays */
                if (pjoin(tmp, e->path, ".new") || !path_ok(tmp)) { say("%s.new: the name is too long", e->path); return -ENAMETOOLONG; }
                dest = tmp;
                if (nkept < 2) strcpy(kept[nkept], e->path);
                nkept++;
                struct ustat st2;
                have = sys_lstat(dest, &st2) == 0;
                if (have && (st2.mode & S_IFMT) == S_IFREG && file_has(dest, e->sha)) continue;
                if (have && (st2.mode & S_IFMT) != S_IFREG) { say("%s exists and is not a file", dest); return -EEXIST; }
            }
        } else if (e->kind == 'c' && !have && find_ent(old, e->path)) {
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
        struct ent *e = &old->ent[i];
        struct ustat st;
        if (find_ent(k, e->path) || sys_lstat(e->path, &st) < 0) continue;
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
    o->pk = k; o->mpath = 0;
    return 0;
}
static void do_dirs(struct pkg *k)
{
    for (int i = 0; i < k->nent; i++) {
        struct ent *e = &k->ent[i];
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
    if (o->hassha) put_sha(sh, o->sha);
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
/* the operations of journal or log text (in place); 0 or -EINVAL */
static int parse_ops(char *t)
{
    nops = 0;
    for (char *l = t, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        if (!l[0] || l[1] != ' ' || !strchr("RADlLu", l[0])) continue;
        char *w[9]; int nw = 0;
        for (char *q = l; *q && nw < 9; ) {
            while (*q == ' ') *q++ = 0;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ') q++;
        }
        if (nw != 9 || nops == MAXOPS || strlen(w[1]) >= 12 || !path_ok(w[8])) return -EINVAL;
        struct op *o = &ops[nops++];
        memset(o, 0, sizeof *o);
        int bad = 0;
        o->op = w[0][0]; strcpy(o->link, w[1]);
        o->mode = (u16)num(w[2], 8, &bad); o->uid = (u16)num(w[3], 10, &bad); o->gid = (u16)num(w[4], 10, &bad);
        if (strcmp(w[5], "-")) { if (parse_sha(w[5], o->sha)) return -EINVAL; o->hassha = 1; }
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
    case 'A':
        sys_unlink(nw);
        sys_unlink(o->path);
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

/* the new content of each R/A operation of package k into PATH.zup-new: its files from the tar.gz, streamed */
struct tarst { int fd; u32 left, pad, hdr; struct op *o; struct sha256 c; u8 h[512]; int err; struct pkg *k; };
static struct op *op_for(struct pkg *k, const char *mpath)
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
static int unpack(struct pkg *k)
{
    static u8 *dict;
    static tinfl_decompressor *dec;
    if (!dict) dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!dec) dec = malloc(sizeof *dec);
    if (!dict || !dec) { say("out of memory"); return -ENOMEM; }
    const u8 *in = k->tgz;
    u32 len = k->tgzlen, pos = 10;
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { say("package %s: not gzip data", k->name); return -EINVAL; }
    u32 flg = in[3];
    if (flg & 4) { if (pos + 2 > len) return -EINVAL; pos += 2 + (in[pos] | (u32)in[pos + 1] << 8); }
    if (flg & 8) while (pos < len && in[pos++]) ;
    if (flg & 16) while (pos < len && in[pos++]) ;
    if (flg & 2) pos += 2;
    if (pos >= len) { say("package %s: a damaged gzip header", k->name); return -EINVAL; }
    static struct tarst t;
    memset(&t, 0, sizeof t);
    t.fd = -1; t.k = k;
    tinfl_init(dec);
    size_t dofs = 0;
    for (;;) {
        size_t in_n = len - pos, out_n = TINFL_LZ_DICT_SIZE - dofs;
        tinfl_status st = tinfl_decompress(dec, in + pos, &in_n, dict, dict + dofs, &out_n, 0);
        pos += (u32)in_n;
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
static int write_manifests(void)
{
    for (int i = 0; i < nops; i++) {
        struct op *o = &ops[i];
        if (!o->pk || o->mpath || o->done) continue;
        char nw[PMAX];
        u8 h[32];
        int r;
        if (zupnew(nw, o->path)) return -ENAMETOOLONG;
        /* through our own buffer: the bundle is no process's memory, a system call refuses it (-EFAULT) */
        int fd = sys_open(nw, O_WRONLY | O_CREAT | O_TRUNC);
        r = fd < 0 ? fd : 0;
        for (u32 at = 0; !r && at < o->pk->manlen; at += sizeof iobuf) {
            u32 k = o->pk->manlen - at < sizeof iobuf ? o->pk->manlen - at : sizeof iobuf;
            memcpy(iobuf, o->pk->man + at, k);
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

/* after the commit record: the older prev sets go, the journal goes, the volume is clean again (repeatable) */
static void post_commit(int wasclean)
{
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
    sys_unlink(JOURNAL);
    sys_sync();
    if (wasclean) fs_mark(0);
}
static void undo_cleanup(int wasclean)                  /* a transaction rolled back: its prev links and log go */
{
    char p[PMAX], t[12];
    for (int i = 0; i < nops; i++)
        if (ops[i].link[0] != '-' && !prev_of(p, &ops[i])) sys_unlink(p);
    snprintf(t, sizeof t, "/log.%u", tnum);
    if (!pjoin(p, prevdir, t)) sys_unlink(p);
    sys_unlink(JOURNAL);
    sys_sync();
    if (wasclean) fs_mark(0);
}
static int write_log(void)                              /* prev-V/log.T: the undo record of this transaction */
{
    char p[PMAX], t[12], *txt;
    u32 len;
    snprintf(t, sizeof t, "/log.%u", tnum);
    if (pjoin(p, prevdir, t) || ops_text(&txt, &len)) return -ENOMEM;
    int r = write_file(p, txt, len);
    free(txt);
    return r;
}
static void set_prevdir(u32 v) { snprintf(prevdir, sizeof prevdir, CACHE "/prev-%u", v); }

/* one transaction, planned in ops[] (target_v, tnum, prevdir set). 0, or -errno (then it was rolled back) */
static int run_transaction(struct pkg **list, int n)
{
    char *txt, head[80], p[PMAX];
    u32 len;
    int r, wasclean = fs_mark(1) == 1;
    if ((r = ops_text(&txt, &len))) return r;
    mkdirs(DB);
    mkdirs(CACHE);
    snprintf(head, sizeof head, "zupj 1\ntarget %u %u %d\n", target_v, tnum, wasclean);
    int fd = sys_open(JOURNAL, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || (r = write_all(fd, head, strlen(head))) || (r = write_all(fd, txt, len)) || (r = write_all(fd, "end\n", 4))) {
        if (fd >= 0) sys_close(fd);
        free(txt);
        say("cannot write the journal (%d): nothing changed", fd < 0 ? fd : r);
        sys_unlink(JOURNAL);
        if (wasclean) fs_mark(0);
        return fd < 0 ? fd : r;
    }
    sys_close(fd);
    free(txt);
    sys_sync();
    for (int i = 0; i < n; i++) do_dirs(list[i]);
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
    if ((r = write_manifests())) goto back;
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
    char *l = strstr(t, "\ntarget ");
    char *u = strstr(t, "\nundo ");
    /* the volume stays marked "needs a check" after a power cut in the middle of an install: a block write may have
     * been cut too (at worst a leaked inode, D15 step 0), which fsck.ext2 finds */
    if (l) { char *sp; v = (u32)strtoul(l + 8, &sp, 10); tn = (u32)strtoul(sp, &sp, 10); }
    else if (u) { v = (u32)strtoul(u + 6, 0, 10); undo = 1; }
    if (!v || parse_ops(t) < 0) { say("the journal is damaged: removed (check the root with fsck.ext2)"); sys_unlink(JOURNAL); free(t); return -EINVAL; }
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
        snprintf(result, sizeof result, "zup: the update to %u was installed (finished after a power cut)", v);
        free(t);
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
            say("finished the install of %u", v);
            snprintf(result, sizeof result, "zup: the update to %u was installed (finished after a power cut)", v);
            free(t);
            return 1;
        }
    }
    if (!rolling) journal_append("rollback\n");
    undo_all();
    undo_cleanup(0);
    say("rolled the unfinished install of %u back", v);
    snprintf(result, sizeof result, "zup: the update to %u was cut off and rolled back; it is tried again at the next start", v);
    failed = 1;
    free(t);
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
static int dep_ok(struct bundle *b, const char *name, u32 v)
{
    for (int i = 0; i < b->npkg; i++) if (!strcmp(b->pk[i].name, name) && b->pk[i].vnum >= v) return 1;
    struct pkg *k = installed(name);
    return k && k->vnum >= v;
}
static int install(struct bundle *b, u32 kvnum, int stage2)
{
    struct pkg *todo[MAXPKG];
    int ntodo = 0, need_zup = 0;
    u32 base = base_version();
    if (b->vnum < base) { say("the bundle (%u) is older than the installed base (%u): refused", b->vnum, base); return -EINVAL; }
    if (b->minv > base) { say("the bundle needs base %u or newer, %u is installed: refused", b->minv, base); return -EINVAL; }
    for (int i = 0; i < b->npkg; i++) {
        struct pkg *k = &b->pk[i], *old = installed(k->name);
        if (old && old->vnum > k->vnum) { say("%s %u is older than the installed %u: refused", k->name, k->vnum, old->vnum); return -EINVAL; }
        if (k->group && !strcmp(k->group, "base") && k->vnum > kvnum) {
            say("%s %u is newer than the running kernel (%u): the kernel goes first (zenixboot): refused", k->name, k->vnum, kvnum);
            return -EINVAL;
        }
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
    /* space: what the plan writes (new files and manifests; the old ones stay as prev links until the next update) */
    nops = 0; nkept = 0;
    for (int i = 0; i < ntodo; i++) { int r = plan_pkg(todo[i]); if (r) return r; }
    u32 need = 0;
    for (int i = 0; i < nops; i++)
        if (ops[i].op == 'R' || ops[i].op == 'A') {
            struct ent *e = ops[i].mpath ? find_ent(ops[i].pk, ops[i].mpath) : 0;
            need += (e ? e->size : ops[i].pk->manlen) + 1024;
        }
    struct ustatfs fs;
    if (sys_statfs(&fs) == 0 && fs.bsize >= 1024 && (fs.bfree * (fs.bsize / 1024) < need / 1024 + 64 || fs.ffree < (u32)nops + 16)) {
        say("not enough space on the root: %u KB and %d files needed", need / 1024 + 64, nops + 16);
        return -ENOSPC;
    }
    if (need_zup && !stage2 && ntodo > 1) {             /* zup first, then the new zup does the rest */
        struct pkg *z[1];
        for (int i = 0; i < ntodo; i++) if (!strcmp(todo[i]->name, "zup")) z[0] = todo[i];
        nops = 0; nkept = 0;
        int r = plan_pkg(z[0]);
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
    int r = run_transaction(todo, ntodo);
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
        struct pkg *k = installed(names[i]);
        if (k) printf("%-12s %u (%s)%s\n", k->name, k->vnum, k->vstr, k->group ? "  base group" : "");
    }
    dir_free(names, n);
    u32 pv = newest_prev();
    if (pv) printf("previous version kept: the one before %u (Zenix (previous) in the boot menu)\n", pv);
    return 0;
}

int main(int argc, char **argv)
{
    int stage2 = argc > 1 && !strcmp(argv[1], "-2");
    if (argc > 1 && !strcmp(argv[1], "-l")) return list();
    if (argc > 1 && !stage2) { printf("usage: zup [-l]   (boot runs zup at every start; see man zup)\n"); return 2; }
    if (sys_getuid() != 0) { printf("zup: only root installs updates\n"); return 1; }
#ifdef DIAG_SLOWZUP     /* test builds (tools/update-test.sh slowzup): a stage 2 that hangs, for boot's time limit */
    if (stage2) { say("diag: DIAG_SLOWZUP: stage 2 hangs"); for (;;) sys_sleep(100); }   /* say, as every message: the
                                                                 * slowzup test sees it after boot stopped this zup */
#endif
    struct uupdate u;
    memset(&u, 0, sizeof u);
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0) return 0;      /* a kernel without updates: nothing to do */
    recover();
    u32 base = base_version(), pv = newest_prev();
    if (pv > u.kvnum && ((u.flags & UPD_F_ROLLBACK) || base > u.kvnum)) {
        say("%s: putting the version before %u back", (u.flags & UPD_F_ROLLBACK) ? "Zenix (previous) was chosen" :
            "the installed base is newer than the kernel", pv);
        int r = rollback(pv);
        if (r == 0) snprintf(result, sizeof result, "zup: rolled back to the version before %u", pv);
        else { snprintf(result, sizeof result, "zup: the rollback to the version before %u failed (%d)", pv, r); failed = 1; }
        say("%s", result + 5);
        save_result();
        return failed;
    } else if (u.flags & UPD_F_ROLLBACK) {
        say("Zenix (previous) was chosen: the root has nothing newer than the kernel (%u) to roll back", u.kvnum);
    }
    if (!u.addr || !u.size) { save_result(); return failed; }
    static struct bundle b;
    int r = bundle_parse(&b, (const u8 *)u.addr, u.size);
    if (r == 0) r = install(&b, u.kvnum, stage2);
    if (r == 2 || r == 3) return r - 2;                 /* the new zup did it, result and note included */
    if (r < 0) {
        snprintf(result, sizeof result, "zup: the update to %u was not installed: %s", b.vnum, lastmsg);
        failed = 1;
    }
    sys_update(UPD_NOTE, r < 0 ? UPDNOTE_FAILED : UPDNOTE_DONE, b.vnum);
    save_result();
    return failed;
}
