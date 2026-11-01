/* SPDX-License-Identifier: GPL-2.0-only */
/* The system calls of zup's host build (tools/host/zup-host.h) served by the real ext2 server on an image file
 * (tools/host/zup-fsd.c: user/fsd.c and kernel/ext2.c), for tools/zup-host-test.sh section 3: a power cut after any
 * block write (ZUP_DISKCUT), then zup again on what reached the image, then the PC's e2fsck. The environment:
 *   HOSTDISK    the image (the whole file is the ext2 volume)
 *   ZUP_BUNDLE, ZUP_FLAGS, ZUP_KVNUM, ZUP_SELF   as tools/host/zup-host.c
 *   ZUP_DISKCUT=N   only N more sectors are written; the next write ends the program (exit 99)
 * A spawned zup (stage 2) is this program again (fork and exec): it mounts the image itself, and this one forgets its
 * cache and mounts again when it ends; a stage 2 cut by the power ends this one too. */
#define _GNU_SOURCE
#include <sys/wait.h>
#include <unistd.h>
#include "zup-host.h"

/* the message numbers (kernel/fsproto.h; not included: its struct ustatfs is zup-host.h's too) */
enum { FS_OPEN = 1, FS_READ, FS_WRITE, FS_STAT, FS_FSTAT, FS_READDIR, FS_CHDIR, FS_MKDIR, FS_UNLINK, FS_RMDIR,
       FS_RENAME, FS_STATFS, FS_SYNC, FS_CHMOD, FS_LINK, FS_SYMLINK, FS_READLINK, FS_LSTAT = 19, FS_CHOWN = 20,
       FS_CLOSE = 24 };
struct udirent { u32 ino, type; char name[64]; };
int zfs_req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3);
void zfs_remount(void);

#define NFD 16
static struct { int used, append; u32 ino, off; } fds[NFD];

int sys_open(const char *p, int fl)
{
    struct ustat st;
    u32 f = (u32)(fl & 3) | (fl & O_CREAT ? 4 : 0) | (fl & O_EXCL ? 8 : 0) | (fl & O_TRUNC ? 16 : 0);
    int fd = 3;
    while (fd < NFD && fds[fd].used) fd++;
    if (fd == NFD) return -EMFILE;
    int r = zfs_req(FS_OPEN, (u32)p, 2, f, (u32)&st);
    if (r < 0) return r;
    fds[fd].used = 1; fds[fd].ino = (u32)r; fds[fd].off = 0; fds[fd].append = (fl & O_APPEND) != 0;
    return fd;
}
static int bad_fd(int fd) { return fd < 3 || fd >= NFD || !fds[fd].used; }
int sys_close(int fd)
{
    if (bad_fd(fd)) return -EBADF;
    fds[fd].used = 0;
    return zfs_req(FS_CLOSE, fds[fd].ino, 0, 0, 0);
}
int sys_read(int fd, void *b, u32 n)
{
    if (bad_fd(fd)) return -EBADF;
    int r = zfs_req(FS_READ, fds[fd].ino, fds[fd].off, (u32)b, n);
    if (r > 0) fds[fd].off += (u32)r;
    return r;
}
int sys_write(int fd, const void *b, u32 n)
{
    if (fd == 1 || fd == 2) return (int)write(fd, b, n);
    if (bad_fd(fd)) return -EBADF;
    if (fds[fd].append) { struct ustat st; if (zfs_req(FS_FSTAT, fds[fd].ino, (u32)&st, 0, 0) == 0) fds[fd].off = st.size; }
    int r = zfs_req(FS_WRITE, fds[fd].ino, fds[fd].off, (u32)b, n);
    if (r > 0) fds[fd].off += (u32)r;
    return r;
}
int sys_stat(const char *p, struct ustat *s) { return zfs_req(FS_STAT, (u32)p, 2, (u32)s, 0); }
int sys_lstat(const char *p, struct ustat *s) { return zfs_req(FS_LSTAT, (u32)p, 2, (u32)s, 0); }
int sys_rename(const char *a, const char *b) { return zfs_req(FS_RENAME, (u32)a, (u32)b, 2, 0); }
int sys_link(const char *a, const char *b) { return zfs_req(FS_LINK, (u32)a, (u32)b, 2, 0); }
int sys_unlink(const char *p) { return zfs_req(FS_UNLINK, (u32)p, 2, 0, 0); }
int sys_mkdir(const char *p) { return zfs_req(FS_MKDIR, (u32)p, 2, 0, 0); }
int sys_rmdir(const char *p) { return zfs_req(FS_RMDIR, (u32)p, 2, 0, 0); }
int sys_chmod(const char *p, u32 mode) { return zfs_req(FS_CHMOD, (u32)p, 2, mode, 0); }
int sys_chown(const char *p, u32 uid, u32 gid) { return zfs_req(FS_CHOWN, (u32)p, 2, uid, gid); }
int sys_symlink(const char *t, const char *p) { return zfs_req(FS_SYMLINK, (u32)t, (u32)p, 2, 0); }
int sys_readlink(const char *p, char *b, u32 max) { return zfs_req(FS_READLINK, (u32)p, 2, (u32)b, max); }
int sys_sync(void) { return zfs_req(FS_SYNC, 0, 0, 0, 0); }
int sys_statfs(struct ustatfs *f) { return zfs_req(FS_STATFS, (u32)f, 0, 0, 0); }
int sys_getuid(void) { return 0; }
int sys_getpid(void) { return (int)getpid(); }
int sys_mountinfo(u32 i, struct umountinfo *m)
{
    if (i) return -1;
    memset(m, 0, sizeof *m); strcpy(m->path, "/"); strcpy(m->type, "ext2");
    return 0;
}
int sys_lseek(int fd, int off, int whence)
{
    if (bad_fd(fd) || whence != 0 || off < 0) return -EINVAL;
    return (int)(fds[fd].off = (u32)off);
}
int sys_fstat(int fd, struct ustat *s) { return bad_fd(fd) ? -EBADF : zfs_req(FS_FSTAT, fds[fd].ino, (u32)s, 0, 0); }
int sys_procinfo(u32 i, struct uproc *p)                /* one process: this zup (its lock is its own) */
{
    if (i != 1) return -EINVAL;
    memset(p, 0, sizeof *p);
    p->pid = (u32)getpid(); p->ppid = (u32)getppid(); p->state = 1;
    strcpy(p->name, "zup");
    return 0;
}
int sys_spawn(const char *path, char *const *argv, const int *stdio)
{
    (void)path; (void)stdio;
    pid_t pid = fork();
    if (pid == 0) {
        execv(getenv("ZUP_SELF"), argv);
        _exit(127);
    }
    return pid < 0 ? -EAGAIN : pid;
}
int sys_wait(int pid)
{
    int st;
    if (waitpid(pid, &st, 0) < 0) return -ECHILD;
    int r = WIFEXITED(st) ? WEXITSTATUS(st) : 139;
    if (r == 99) _exit(99);                         /* the power went while the new zup ran */
    zfs_remount();                                  /* it changed the image behind this one's cache */
    return r;
}
int sys_port_find(const char *n) { return strcmp(n, "fs") ? -ENOENT : 3; }
int sys_call(int port, struct umsg *m)
{
    if (port != 3 || m->type != FS_MARK) return -ENOSYS;
    return zfs_req(m->type, m->a[0], m->a[1], m->a[2], m->a[3]);
}
static void *bundle;
int sys_update(u32 op, u32 a, u32 b2)
{
    const char *kv = getenv("ZUP_KVNUM");
    if (!kv) return -ENOSYS;
    if (op != UPD_INFO) return 0;
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
        u->addr = (u32)bundle; u->size = (u32)n;
    }
    return 0;
}
int zerrno;
int dir_list(const char *path, char ***names)
{
    struct ustat st;
    struct udirent e;
    *names = 0;
    int ino = zfs_req(FS_OPEN, (u32)path, 2, O_RDONLY, (u32)&st);
    if (ino < 0) return -1;
    int n = 0, cap = 16;
    char **v = malloc(sizeof *v * (size_t)cap);
    u32 cookie = 0;
    while (zfs_req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&cookie) > 0) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if (n + 1 >= cap) v = realloc(v, sizeof *v * (size_t)(cap *= 2));
        v[n++] = strdup(e.name);
    }
    zfs_req(FS_CLOSE, (u32)ino, 0, 0, 0);
    v[n] = 0;
    *names = v;
    return n;
}
void dir_free(char **names, int n) { for (int i = 0; i < n; i++) free(names[i]); free(names); }
