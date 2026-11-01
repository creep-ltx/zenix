/* SPDX-License-Identifier: GPL-2.0-only */
/* The system calls of zup's host build (tools/host/zup-host.h), on a directory of the PC. Every change can be the
 * last: ZUP_CUT=N ends the program (exit 99, as a power cut would) just before the Nth change, so the test can run
 * zup again on what is left. Owners are not changed on the PC (the test runs as a normal user): chown is recorded in
 * the trace only, and the mode is kept as given. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "zup-host.h"

static long ncut = -1, nchange;
static FILE *trace;
static const char *root;

static void init(void)
{
    if (root) return;
    root = getenv("ZUP_ROOT");
    if (!root) { fprintf(stderr, "zup-host: ZUP_ROOT is not set\n"); exit(2); }
    const char *c = getenv("ZUP_CUT");
    if (c && *c) ncut = atol(c);
    const char *t = getenv("ZUP_TRACE");
    if (t && *t) trace = fopen(t, "a");
    setvbuf(stdout, 0, _IONBF, 0);
}
static const char *P(const char *p, char *buf)
{
    init();
    snprintf(buf, 4096, "%s%s", root, p);
    return buf;
}
static int err(void) { return -errno; }
static void change(const char *what, const char *a, const char *b)
{
    init();
    nchange++;
    if (ncut >= 0 && nchange >= ncut) {
        if (trace) { fprintf(trace, "CUT before %s %s%s%s\n", what, a, b ? " " : "", b ? b : ""); fclose(trace); }
        _exit(99);
    }
    if (trace) { fprintf(trace, "%s %s%s%s\n", what, a, b ? " " : "", b ? b : ""); fflush(trace); }
}

int sys_open(const char *p, int fl)
{
    char b[4096];
    int hf = (fl & 3) | (fl & O_CREAT ? O_CREAT : 0) | (fl & O_EXCL ? O_EXCL : 0) | (fl & O_TRUNC ? O_TRUNC : 0) | (fl & O_APPEND ? O_APPEND : 0);
    if (fl & 3) change("open-write", p, 0);
    int fd = open(P(p, b), hf | O_NOFOLLOW * 0, 0644);
    return fd < 0 ? err() : fd;
}
int sys_close(int fd) { return close(fd) < 0 ? err() : 0; }
int sys_read(int fd, void *b, u32 n) { ssize_t r = read(fd, b, n); return r < 0 ? err() : (int)r; }
static void *bundle;
static long bundle_len;
int sys_write(int fd, const void *b, u32 n)
{
    if (bundle && (const char *)b < (const char *)bundle + bundle_len && (const char *)b + n > (const char *)bundle)
        return -EFAULT;                                 /* as on Zenix: the bundle is no process's memory */
    if (fd > 2) change("write", "", 0);
    ssize_t r = write(fd, b, n);
    return r < 0 ? err() : (int)r;
}
static void fill(struct ustat *s, const struct stat *st)
{
    s->ino = (u32)st->st_ino; s->mode = st->st_mode; s->size = (u32)st->st_size; s->mtime = (u32)st->st_mtime;
    s->uid = 0; s->gid = 0; s->links = (u32)st->st_nlink;
}
int sys_stat(const char *p, struct ustat *s) { char b[4096]; struct stat st; if (stat(P(p, b), &st) < 0) return err(); fill(s, &st); return 0; }
int sys_lstat(const char *p, struct ustat *s) { char b[4096]; struct stat st; if (lstat(P(p, b), &st) < 0) return err(); fill(s, &st); return 0; }
int sys_rename(const char *a, const char *b2) { char x[4096], y[4096]; change("rename", a, b2); return rename(P(a, x), P(b2, y)) < 0 ? err() : 0; }
int sys_link(const char *a, const char *b2) { char x[4096], y[4096]; change("link", a, b2); return link(P(a, x), P(b2, y)) < 0 ? err() : 0; }
int sys_unlink(const char *p)
{
    char b[4096]; struct stat st;
    if (lstat(P(p, b), &st) < 0) return err();         /* nothing to remove: no change, no cut point */
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    change("unlink", p, 0);
    return unlink(b) < 0 ? err() : 0;
}
int sys_mkdir(const char *p) { char b[4096]; struct stat st; if (lstat(P(p, b), &st) == 0) return -EEXIST; change("mkdir", p, 0); return mkdir(b, 0755) < 0 ? err() : 0; }
int sys_rmdir(const char *p) { char b[4096]; change("rmdir", p, 0); return rmdir(P(p, b)) < 0 ? err() : 0; }
int sys_chmod(const char *p, u32 mode) { char b[4096], m[16]; snprintf(m, sizeof m, "%o", mode); change("chmod", p, m); return chmod(P(p, b), mode & 07777) < 0 ? err() : 0; }
int sys_chown(const char *p, u32 uid, u32 gid) { char b[4096], m[24]; struct stat st; snprintf(m, sizeof m, "%u:%u", uid, gid); change("chown", p, m); return lstat(P(p, b), &st) < 0 ? err() : 0; }
int sys_symlink(const char *t, const char *p) { char b[4096]; change("symlink", t, p); return symlink(t, P(p, b)) < 0 ? err() : 0; }
int sys_readlink(const char *p, char *buf, u32 max) { char b[4096]; ssize_t r = readlink(P(p, b), buf, max); return r < 0 ? err() : (int)r; }
int sys_sync(void) { change("sync", "", 0); return 0; }
int sys_statfs(struct ustatfs *f)
{
    struct statvfs v; char b[4096];
    if (statvfs(P("/", b), &v) < 0) return err();
    const char *lim = getenv("ZUP_FREE_KB");             /* a small disk, for the "not enough space" test */
    f->bsize = 1024; f->blocks = 1u << 20; f->bfree = lim ? (unsigned)atol(lim) : (unsigned)(v.f_bavail * (v.f_frsize / 1024));
    f->files = 1u << 20; f->ffree = 1u << 19;
    return 0;
}
int sys_getuid(void) { return 0; }
int sys_getpid(void) { return (int)getpid(); }
int sys_mountinfo(u32 i, struct umountinfo *m)
{
    if (i) return -1;
    memset(m, 0, sizeof *m); strcpy(m->path, "/"); strcpy(m->type, "ext2");
    return 0;
}
int sys_lseek(int fd, int off, int whence) { off_t r = lseek(fd, off, whence); return r < 0 ? err() : (int)r; }
int sys_fstat(int fd, struct ustat *s) { struct stat st; if (fstat(fd, &st) < 0) return err(); fill(s, &st); return 0; }
static int *pids, npids;
int sys_procinfo(u32 i, struct uproc *p)
{
    if (i == 1 || !pids) {                              /* a fresh list of the PC's processes at each walk's start */
        DIR *d = opendir("/proc");
        struct dirent *e;
        int cap = 0;
        npids = 0;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
            if (npids == cap) pids = realloc(pids, sizeof *pids * (size_t)(cap = cap ? cap * 2 : 256));
            pids[npids++] = atoi(e->d_name);
        }
        if (d) closedir(d);
    }
    if (i < 1 || (int)i > npids) return -EINVAL;
    memset(p, 0, sizeof *p);
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", pids[i - 1]);
    FILE *f = fopen(path, "r");
    if (!f) return 0;                                   /* gone meanwhile: a free slot */
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *a = strchr(buf, '('), *b = strrchr(buf, ')');
    if (!a || !b || b - a - 1 > 15) return 0;
    memcpy(p->name, a + 1, (size_t)(b - a - 1));
    char st = b[2];
    p->pid = (u32)pids[i - 1];
    p->ppid = (u32)atoi(b + 4);
    p->state = st == 'Z' || st == 'X' ? 5 : 1;
    return 0;
}
int sys_spawn(const char *path, char *const *argv, const int *stdio)
{
    (void)stdio;
    init();
    if (trace) { fprintf(trace, "spawn %s %s\n", path, argv[1] ? argv[1] : ""); fflush(trace); }
    pid_t pid = fork();
    if (pid == 0) {
        const char *self = getenv("ZUP_SELF");
        execv(self, argv);
        _exit(127);
    }
    return pid < 0 ? err() : pid;
}
int sys_wait(int pid) { int st; if (waitpid(pid, &st, 0) < 0) return err(); return WIFEXITED(st) ? WEXITSTATUS(st) : 139; }
int sys_port_find(const char *n) { return strcmp(n, "fs") ? -ENOENT : 3; }
int sys_call(int port, struct umsg *m)
{
    char b[4096];
    if (port != 3 || m->type != FS_MARK) return -ENOSYS;
    FILE *f = fopen(P("/../fsclean", b), "r");
    int was = 1;
    if (f) { was = fgetc(f) == '1'; fclose(f); }
    change("mark", m->a[0] ? "unclean" : "clean", 0);
    f = fopen(b, "w");
    if (f) { fputc(m->a[0] ? '0' : '1', f); fclose(f); }
    return was;
}
int sys_update(u32 op, u32 a, u32 b2)
{
    init();
    const char *kv = getenv("ZUP_KVNUM");
    if (!kv) return -ENOSYS;
    if (op == UPD_INFO) {
        struct uupdate *u = (struct uupdate *)a;
        memset(u, 0, b2 < sizeof *u ? b2 : sizeof *u);
        u->kvnum = (u32)strtoul(kv, 0, 10);
        u->flags = (u32)strtoul(getenv("ZUP_FLAGS") ? getenv("ZUP_FLAGS") : "0", 0, 10);
        const char *bf = getenv("ZUP_BUNDLE");
        if (bf && *bf) {
            FILE *f = fopen(bf, "rb");
            if (!f) return -ENOENT;
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (!bundle) bundle = malloc((size_t)n + 1);
            if (fread(bundle, 1, (size_t)n, f) != (size_t)n) { fclose(f); return -EIO; }
            fclose(f);
            u->addr = (u32)bundle; u->size = (u32)n; bundle_len = n;
        }
        return 0;
    }
    if (op == UPD_NOTE) {
        if (trace) { fprintf(trace, "note %u %u\n", a, b2); fflush(trace); }
        return 0;
    }
    return 0;
}
int zerrno;
int dir_list(const char *path, char ***names)
{
    char b[4096];
    const char *bad = getenv("ZUP_LIST_FAIL");         /* a directory that cannot be read, as a damaged disk's */
    *names = 0;
    if (bad && strstr(path, bad)) { zerrno = EIO; return -1; }
    DIR *d = opendir(P(path, b));
    if (!d) { zerrno = errno; return -1; }
    int n = 0, cap = 16;
    char **v = malloc(sizeof *v * (size_t)cap);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n + 1 >= cap) v = realloc(v, sizeof *v * (size_t)(cap *= 2));
        v[n++] = strdup(e->d_name);
    }
    closedir(d);
    v[n] = 0;
    *names = v;
    return n;
}
void dir_free(char **names, int n) { for (int i = 0; i < n; i++) free(names[i]); free(names); }
