// SPDX-License-Identifier: GPL-2.0-only
/* unlha, unlzx (the same program, by its name; DESIGN D33, docs/d33-archives.md): list, test and unpack LhA and LZX
 * archives. Unpack only. An archive is untrusted: the readers (user/arcread.h) check it; here no name leads out of the
 * target folder, no link is ever made or followed on the way, nothing is overwritten without -f, and the unpacked total
 * has a limit that is said out loud. Exit status: 0 all well, 1 a member failed or was refused, 2 the usage or the archive. */
#ifdef ZHOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#define oprintf printf
#define oflush() fflush(stdout)
#define zwhy(d) (strerror(errno))
typedef unsigned u32;
struct ustat { u32 ino, mode, size, mtime, uid, gid, links; };
static int sys_open(const char *p, int f) { return open(p, f, 0600); }
#define sys_close close
#define sys_read read
#define sys_write write
#define sys_lseek lseek
#define sys_unlink unlink
#define sys_chmod chmod
static int sys_mkdir(const char *p) { return mkdir(p, 0755); }
static int fromst(int r, const struct stat *s, struct ustat *u)
{ if (r == 0) { u->mode = s->st_mode; u->size = (u32)s->st_size; } return r; }
static int sys_fstat(int fd, struct ustat *u) { struct stat s; return fromst(fstat(fd, &s), &s, u); }
static int sys_lstat(const char *p, struct ustat *u) { struct stat s; return fromst(lstat(p, &s), &s, u); }
static int sys_utimes(const char *p, u32 t) { struct timeval v[2] = { { t, 0 }, { t, 0 } }; return utimes(p, v); }
#define _zexit exit
/* libz's date conversion (its own source) and command line, as the PC test needs them */
struct zdt { int year, mon, day, hour, min, sec, wday, yday; };
static long zdays(int y, int m, int d);
#define ZDATE_LINK static __attribute__((unused))
#include "zdate.h"
static const char *zprog = "unlha";
static __attribute__((noreturn)) void zusage(const char *u) { fputs(u, stderr); exit(2); }
static __attribute__((noreturn)) void zbadopt(const char *o, const char *u)
{ fprintf(stderr, "%s: unknown option %s\n", zprog, o); zusage(u); }
static void zopt(int argc, char **argv, const char *u)
{
    const char *sl = strrchr(argv[0], '/');
    zprog = sl ? sl + 1 : argv[0];
    if (argc > 1 && !strcmp(argv[1], "--help")) { fputs(u, stdout); exit(0); }
    if (argc > 1 && !strcmp(argv[1], "--version")) { printf("%s 0.2\n", zprog); exit(0); }
}
#else
#include "libz.h"
#endif
#include "arcread.h"

static const char *me;
static int fd;
static int command, force, quiet, rc;
static const char *destdir = ".";
static char **want;
static int nwant;
static unsigned char *found;
static unsigned long limit = 512ul * 1024 * 1024, total;
static unsigned long nfiles, nmembers, sumsize, sumpacked;
static char *pathbuf;
#define PATHMAX 512

void *arc_alloc(unsigned long n)
{
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void arc_free(void *p) { free(p); }

static long readfn(void *ctx, unsigned long off, void *buf, long n)
{
    long got = 0;
    (void)ctx;
    if (sys_lseek(fd, (long)off, 0) < 0) return -1;
    while (got < n) {
        long r = sys_read(fd, (char *)buf + got, n - got);
        if (r < 0) return -1;
        if (r == 0) break;
        got += r;
    }
    return got;
}

struct outf { int fd, failed; unsigned long left; };
static int writefn(void *ctx, const void *buf, long n)
{
    struct outf *o = ctx;
    if (o->fd < 0) return 1;                /* test: only the checksum */
    while (n > 0) {
        long r = sys_write(o->fd, buf, n);
        if (r <= 0) { o->failed = 1; return 0; }
        buf = (const char *)buf + r;
        n -= r;
    }
    return 1;
}

/* arc_errstr's words name another program for one of these */
static const char *why(int r)
{
    return r == ARC_EUNSUP ? "a level or method this program does not read" : arc_errstr(r);
}

static const char *mname(const struct arcent *e, char *b)
{
    int lzx = me[2] == 'l' && me[3] == 'z';
    if (e->method == AM_DIR) return "dir";
    if (e->method == AM_REFUSED) return "NO";
    if (lzx) return e->method == 0 ? "store" : e->method == 2 ? "lzx" : "?";
    b[0] = '-';
    b[1] = (char)(e->aux3 >> 16);
    b[2] = (char)(e->aux3 >> 8);
    b[3] = (char)e->aux3;
    b[4] = '-';
    b[5] = 0;
    {
        int i;
        for (i = 1; i < 4; i++) if (b[i] < 0x20 || b[i] > 0x7E) b[i] = '?';
    }
    return b;
}

static void civil(long days, int *y, int *m, int *d)     /* days since 1978-01-01 */
{
    struct zdt z;
    zdate((u32)(days + 2922) * 86400u, &z);
    *y = z.year;
    *m = z.mon;
    *d = z.day;
}

static void list_line(const struct arcent *e)
{
    char b[8], p[9];
    static const char pb[] = "hsparwed";
    int y, mo, d, i;
    /* AmigaDOS's bits, top first: h s p a set = on; r w e d set = denied */
    for (i = 0; i < 8; i++) {
        unsigned bit = 1u << (7 - i);
        int on = i < 4 ? (e->prot & bit) != 0 : (e->prot & bit) == 0;
        p[i] = on ? pb[i] : '-';
    }
    p[8] = 0;
    if (e->days || e->mins || e->ticks) {
        civil(e->days, &y, &mo, &d);
        oprintf("%9lu %9lu %04d-%02d-%02d %02d:%02d %s %-5s %s", e->size, e->packed, y, mo, d,
                (int)(e->mins / 60), (int)(e->mins % 60), p, mname(e, b), e->name);
    } else {
        oprintf("%9lu %9lu ---------- --:-- %s %-5s %s", e->size, e->packed, p, mname(e, b), e->name);
    }
    oprintf(e->isdir ? "/\n" : "\n");
    if (e->comment[0]) oprintf("%20s: %s\n", "", e->comment);
}

/* the folders on the way to path (under destdir), made; a soft link on the way is refused: 0 or -1 */
static int mkparents(char *path, int upto_last)
{
    char *s = path + strlen(destdir) + 1, *q;
    struct ustat st;
    for (q = s; ; q++) {
        char c = *q;
        if (c != '/' && c != 0) continue;
        if (c == 0 && upto_last == 0) break;
        *q = 0;
        if (sys_lstat(path, &st) == 0) {
            if ((st.mode & 0170000) != 0040000) {
                dprintf(2, "%s: %s: on the way is not a folder (a link is not followed)\n", me, path);
                *q = c;
                return -1;
            }
        } else if (sys_mkdir(path) < 0) {
            dprintf(2, "%s: %s: %s\n", me, path, zwhy("cannot make the folder"));
            *q = c;
            return -1;
        }
        *q = c;
        if (c == 0) break;
    }
    return 0;
}

static unsigned epoch(const struct arcent *e)
{
    return ((unsigned)e->days + 2922u) * 86400u + (unsigned)e->mins * 60u + (unsigned)e->ticks / 50u;   /* to 2106 */
}
/* the mode: LhA's Unix field if there is one, else the AmigaDOS bits; never set-user-id */
static unsigned modeof(const struct arcent *e, int lha)
{
    unsigned m = 0;
    if (lha && (e->aux2 & 0x10000)) return (unsigned)e->aux2 & 0777;
    if (!(e->prot & 8)) m |= 0444;
    if (!(e->prot & 4)) m |= 0200;
    if (!(e->prot & 2)) m |= 0111;
    return m ? m : 0600;
}

static int selected(const char *name)
{
    int i, hit = 0;
    if (!nwant) return 1;
    for (i = 0; i < nwant; i++) {
        size_t n = strlen(want[i]);
        while (n > 1 && want[i][n - 1] == '/') n--;
        if (!strncmp(name, want[i], n) && (name[n] == 0 || name[n] == '/')) { found[i] = 1; hit = 1; }
    }
    return hit;
}

static const struct arcsrc *gsrc;
static int (*xfn)(const struct arcsrc *, const struct arcent *, const struct arcsink *);

static int member(void *ctx, const struct arcent *e)
{
    struct outf o;
    struct arcsink sink;
    int r, lha = xfn == lha_extract;
    (void)ctx;
    if (!selected(e->name)) return 1;
    nmembers++;
    sumsize += e->size;
    sumpacked += e->packed;
    if (command == 'l') { list_line(e); return 1; }
    if (e->method == AM_REFUSED) {
        dprintf(2, "%s: %s: refused: the name leads out of the folder, is a link or is too long\n", me, e->name);
        rc = 1;
        return 1;
    }
    if (e->method == AM_OTHER) {
        char b[8];
        dprintf(2, "%s: %s: method %s is not supported\n", me, e->name, mname(e, b));
        rc = 1;
        return 1;
    }
    o.fd = -1;
    o.failed = 0;
    sink.write = writefn;
    sink.ctx = &o;
    if (command == 'x') {
        int n = (int)strlen(destdir) + 1 + (int)strlen(e->name);
        if (n >= PATHMAX) { dprintf(2, "%s: %s: the path is too long\n", me, e->name); rc = 1; return 1; }
        strcpy(pathbuf, destdir);
        strcat(pathbuf, "/");
        strcat(pathbuf, e->name);
        if (e->isdir) {
            if (mkparents(pathbuf, 1)) { rc = 1; return 1; }
            if (!quiet) oprintf("%s/\n", e->name);
            return 1;
        }
        if (mkparents(pathbuf, 0)) { rc = 1; return 1; }
        {
            struct ustat st;
            if (sys_lstat(pathbuf, &st) == 0) {
                if (!force || (st.mode & 0170000) == 0040000) {
                    dprintf(2, "%s: %s: already there%s\n", me, e->name, force ? " (a folder)" : " (-f overwrites)");
                    rc = 1;
                    return 1;
                }
                if (sys_unlink(pathbuf) < 0) { dprintf(2, "%s: %s: %s\n", me, e->name, zwhy("cannot remove")); rc = 1; return 1; }
            }
        }
        o.fd = sys_open(pathbuf, O_WRONLY | O_CREAT | O_EXCL);
        if (o.fd < 0) { dprintf(2, "%s: %s: %s\n", me, e->name, zwhy("cannot create")); rc = 1; return 1; }
    }
    if (e->isdir) return 1;
    r = xfn(gsrc, e, &sink);
    if (o.fd >= 0) {
        sys_close(o.fd);
        if (r) {
            sys_unlink(pathbuf);        /* what failed its check does not stay */
        } else {
            if (e->days || e->mins || e->ticks) sys_utimes(pathbuf, epoch(e));
            sys_chmod(pathbuf, modeof(e, lha));
        }
    }
    if (r) {
        if (o.failed) dprintf(2, "%s: %s: cannot write: %s\n", me, e->name, zwhy("write error"));
        else dprintf(2, "%s: %s: %s\n", me, e->name, why(r));
        rc = 1;
        return 1;
    }
    nfiles++;
    if (!quiet) {
        if (command == 'x') oprintf("%s\n", e->name);
        else oprintf("ok %s\n", e->name);
    }
    return 1;
}

/* the first pass adds up what is to be unpacked: a limit is checked before anything is written. A member of an LZX group costs all
 * the group's bytes before it, written or not. */
static unsigned long curgroup, curneed;
static void addtotal(unsigned long n)
{
    if (total + n < total) total = (unsigned long)-1;
    else total += n;
}
static int count(void *ctx, const struct arcent *e)
{
    (void)ctx;
    if (!selected(e->name)) return 1;
    if (xfn == lzx_extract && e->method == 2) {
        unsigned long need = e->aux1 + e->size;
        if (need < e->size) need = (unsigned long)-1;
        if (e->dataoff != curgroup) {
            addtotal(curneed);
            curgroup = e->dataoff;
            curneed = 0;
        }
        if (need > curneed) curneed = need;
    } else addtotal(e->size);
    return 1;
}

static const char *USAGE;
static void usage(void) { zusage(USAGE); }

int main(int argc, char **argv)
{
    int i = 1, r, lzx;
    struct arcsrc src;
    struct ustat st;
    int (*listfn)(const struct arcsrc *, arc_addfn, void *);
    const char *arcname = 0;
    me = strrchr(argv[0], '/');
    me = me ? me + 1 : argv[0];
    lzx = !strcmp(me, "unlzx");
    if (!lzx) me = "unlha";
    USAGE = lzx ? "usage: unlzx l|t|x [-f] [-q] [-d folder] [-L megabytes] archive [name...]\n"
                : "usage: unlha l|t|x [-f] [-q] [-d folder] [-L megabytes] archive [name...]\n";
    zopt(argc, argv, USAGE);
    if (argc < 2) usage();
    {
        const char *c = argv[i++];
        if (*c == '-') c++;
        if (*c == '-') zbadopt(argv[1], USAGE);
        if ((c[0] != 'l' && c[0] != 't' && c[0] != 'x') || c[1]) usage();
        command = c[0];
    }
    for (; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-f")) force = 1;
        else if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-d") && i + 1 < argc) destdir = argv[++i];
        else if (!strcmp(a, "-L") && i + 1 < argc) {
            long mb = atoi(argv[++i]);
            if (mb <= 0 || mb > 4000) usage();
            limit = (unsigned long)mb * 1024 * 1024;
        } else zbadopt(a, USAGE);
    }
    if (i >= argc) usage();
    arcname = argv[i++];
    want = argv + i;
    nwant = argc - i;
    if (nwant) {
        found = calloc((size_t)nwant, 1);
        if (!found) { dprintf(2, "%s: out of memory\n", me); return 2; }
    }
    pathbuf = malloc(PATHMAX);
    if (!pathbuf) { dprintf(2, "%s: out of memory\n", me); return 2; }
    fd = sys_open(arcname, O_RDONLY);
    if (fd < 0 || sys_fstat(fd, &st) < 0) { dprintf(2, "%s: %s: %s\n", me, arcname, zwhy("cannot open")); return 2; }
    if ((st.mode & 0170000) == 0040000) { dprintf(2, "%s: %s: a folder\n", me, arcname); return 2; }
    src.read = readfn;
    src.ctx = 0;
    src.size = st.size;
    gsrc = &src;
    listfn = lzx ? lzx_list : lha_list;
    xfn = lzx ? lzx_extract : lha_extract;
    arc_list_refused = 1;
    if (command != 'l') {
        r = listfn(&src, count, 0);
        addtotal(curneed);
        if (nwant) memset(found, 0, (size_t)nwant);
        if (r && r != ARC_ESTOP) {
            dprintf(2, "%s: %s: %s\n", me, arcname, why(r));
            /* the members before the damage are still taken */
            rc = 2;
        }
        if (total > limit) {
            dprintf(2, "%s: %s: unpacks to %lu MB, over the limit of %lu MB: nothing done (-L sets it)\n",
                    me, arcname, total >> 20, limit >> 20);
            return 2;
        }
    }
    if (command == 'l') oprintf("     size    packed date       time  attr     meth  name\n");
    r = listfn(&src, member, 0);
    if (r && r != ARC_ESTOP) {
        if (!rc) dprintf(2, "%s: %s: %s\n", me, arcname, why(r));
        rc = 2;
    }
    if (command == 'l') oprintf("%lu members, %lu bytes, %lu packed\n", nmembers, sumsize, sumpacked);
    else if (!quiet) oprintf("%lu files %s\n", nfiles, command == 'x' ? "unpacked" : "checked");
    for (i = 0; i < nwant; i++) {
        if (!found[i]) {
            dprintf(2, "%s: %s: not in the archive\n", me, want[i]);
            if (!rc) rc = 1;
        }
    }
    lzx_end();
    arc_freetables();
    oflush();
    sys_close(fd);
    return rc;
}
