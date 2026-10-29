/* SPDX-License-Identifier: GPL-2.0-only */
/* Processes (DESIGN D27 5 and 5.5): posix_spawn and posix_spawnp over SYS_spawn, which gives the child the parent's
 * environment and descriptors 0-2 only (there is no fork or exec), so:
 *  - file actions are done in the parent around the spawn and can only set the child's descriptors 0-2 (others EINVAL);
 *  - an envp other than environ is set with SYS_envset around the spawn and put back;
 *  - POSIX_SPAWN_SETPGROUP is done by the parent right after the spawn (setpgid); the other attributes answer EINVAL.
 * system and popen run /bin/sh -c through it. waitpid and wait over SYS_wait, which answers a child's status as the
 * shell's $? (0-255, 128 + the signal for one that a signal ended): it is given as an exit status (WEXITSTATUS), a
 * stopped child (WUNTRACED) as stopped by SIGTSTP. SYS_wait for any child does not say which one: wait looks at the
 * process table before and after (SYS_procinfo). getppid from the table too. */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zenix-consts.h>
#include "zshare.h"

extern char **environ;

/* ---- file actions: kept in order, done at the spawn */
enum { FA_OPEN, FA_DUP2, FA_CLOSE };
struct fa { int kind, fd, src, oflag; mode_t mode; char *path; };
struct __posix_spawn_file_actions { int n, cap; struct fa *a; };
int posix_spawn_file_actions_init(posix_spawn_file_actions_t *fa)
{
    if (!(*fa = calloc(1, sizeof **fa))) return ENOMEM;
    return 0;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t *fa)
{
    if (!*fa) return EINVAL;
    for (int i = 0; i < (*fa)->n; i++) free((*fa)->a[i].path);
    free((*fa)->a);
    free(*fa);
    *fa = 0;
    return 0;
}
static int fa_add(posix_spawn_file_actions_t *fa, int kind, int fd, int src, const char *path, int oflag, mode_t mode)
{
    struct __posix_spawn_file_actions *f = *fa;
    if (!f) return EINVAL;
    if (fd < 0 || fd > 2) return EINVAL;            /* the child gets descriptors 0-2 only */
    if (f->n == f->cap) {
        int nc = f->cap ? f->cap * 2 : 4;
        struct fa *na = realloc(f->a, (size_t)nc * sizeof *na);
        if (!na) return ENOMEM;
        f->a = na; f->cap = nc;
    }
    struct fa *a = &f->a[f->n];
    a->kind = kind; a->fd = fd; a->src = src; a->oflag = oflag; a->mode = mode; a->path = 0;
    if (path && !(a->path = strdup(path))) return ENOMEM;
    f->n++;
    return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t *fa, int fd, const char *path, int oflag, mode_t mode)
{ return fa_add(fa, FA_OPEN, fd, 0, path, oflag, mode); }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t *fa, int src, int fd)
{ return src < 0 ? EBADF : fa_add(fa, FA_DUP2, fd, src, 0, 0, 0); }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t *fa, int fd)
{ return fa_add(fa, FA_CLOSE, fd, 0, 0, 0, 0); }

/* ---- attributes: the flags and the process group */
struct __posix_spawnattr { short flags; pid_t pgroup; };
int posix_spawnattr_init(posix_spawnattr_t *at)
{
    if (!(*at = calloc(1, sizeof **at))) return ENOMEM;
    return 0;
}
int posix_spawnattr_destroy(posix_spawnattr_t *at) { free(*at); *at = 0; return 0; }
int posix_spawnattr_setflags(posix_spawnattr_t *at, short flags)
{
    if (flags & ~POSIX_SPAWN_SETPGROUP) return EINVAL;  /* signal masks and defaults, ids, scheduling: none here */
    (*at)->flags = flags;
    return 0;
}
int posix_spawnattr_getflags(const posix_spawnattr_t *at, short *flags) { *flags = (*at)->flags; return 0; }
int posix_spawnattr_setpgroup(posix_spawnattr_t *at, pid_t pg) { (*at)->pgroup = pg; return 0; }
int posix_spawnattr_getpgroup(const posix_spawnattr_t *at, pid_t *pg) { *pg = (*at)->pgroup; return 0; }
int posix_spawnattr_setsigmask(posix_spawnattr_t *at, const sigset_t *s) { (void)at; (void)s; return EINVAL; }
int posix_spawnattr_setsigdefault(posix_spawnattr_t *at, const sigset_t *s) { (void)at; (void)s; return EINVAL; }

#define FD_SHUT 0x7FFF                  /* a descriptor number no process has: the child gets none there */

/* the environment block of envp (NAME=value strings, a 0 after each and one more at the end), malloc'd */
static char *env_block(char *const *envp, u32 *len)
{
    u32 n = 1;
    for (int i = 0; envp[i]; i++) n += (u32)strlen(envp[i]) + 1;
    if (n > ZX_ENVMAX) { errno = E2BIG; return 0; }
    char *b = malloc(n), *p = b;
    if (!b) { errno = ENOMEM; return 0; }
    for (int i = 0; envp[i]; i++) { size_t l = strlen(envp[i]) + 1; memcpy(p, envp[i], l); p += l; }
    *p = 0;
    *len = n;
    return b;
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                char *const argv[], char *const envp[])
{
    int io[3] = { 0, 1, 2 }, opened[8], nopen = 0, err = 0, saved_len = 0;
    char *saved = 0;
    int e0 = errno;
    if (fa && *fa)                                  /* the file actions, in order, in the parent */
        for (int i = 0; i < (*fa)->n && !err; i++) {
            struct fa *a = &(*fa)->a[i];
            if (a->kind == FA_OPEN) {
                int fd = nopen < 8 ? open(a->path, a->oflag, a->mode) : (errno = EMFILE, -1);
                if (fd < 0) { err = errno; break; }
                opened[nopen++] = io[a->fd] = fd;
            } else if (a->kind == FA_DUP2) {
                int s = a->src <= 2 ? io[a->src] : a->src;
                if (s == FD_SHUT) { err = EBADF; break; }
                io[a->fd] = s;
            } else
                io[a->fd] = FD_SHUT;
        }
    if (!err && envp && envp != environ) {          /* the child's environment: ours for the moment of the spawn */
        u32 len;
        char *blk = env_block(envp, &len);
        saved = malloc(ZX_ENVMAX);
        if (!blk || !saved) err = blk ? ENOMEM : errno;
        else if ((saved_len = sys_envget(saved, ZX_ENVMAX)) < 0 || sys_envset(blk, len) < 0) err = errno;
        free(blk);
        if (err) { free(saved); saved = 0; }
    }
    int r = err ? 0 : sys_spawn(path, argv, io);
    if (!err && r < 0) err = errno;
    if (saved) {
        sys_envset(saved, saved_len > ZX_ENVMAX ? ZX_ENVMAX : (u32)saved_len);
        free(saved);
    }
    for (int i = 0; i < nopen; i++) close(opened[i]);
    if (err) { errno = e0; return err; }
    if (at && *at && ((*at)->flags & POSIX_SPAWN_SETPGROUP))
        setpgid(r, (*at)->pgroup ? (*at)->pgroup : r);  /* right after the spawn (DESIGN D27 5.5) */
    if (pid) *pid = r;
    errno = e0;
    return 0;
}
int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at,
                 char *const argv[], char *const envp[])
{
    char path[ZPATH_MAX];
    if (!path_find(file, path, sizeof path)) return ENOENT;        /* through $PATH, as sh finds it */
    return posix_spawn(pid, path, fa, at, argv, envp);
}

/* ---- waiting */
#define ST_STOP_STATUS (0x7f | SIGTSTP << 8)
/* the children's pids into kids (at most max), their count; the stopped ones marked by a negative pid */
static int children(int *kids, int max)
{
    struct uproc p;
    int n = 0, me = getpid(), e = errno;
    for (u32 i = 0; n < max && sys_procinfo(i, &p) >= 0; i++)
        if (p.state && (int)p.ppid == me) kids[n++] = p.state == 7 ? -(int)p.pid : (int)p.pid;   /* 7: P_STOP */
    errno = e;
    return n;
}
pid_t waitpid(pid_t pid, int *status, int options)
{
    int kids[32], nk = 0, e = errno;
    if (options & ~(WNOHANG | WUNTRACED) || pid == 0 || pid < -1) { errno = EINVAL; return -1; }
    if (pid == -1) nk = children(kids, 32);
    int r = sys3(6, (u32)pid, (u32)((options & WNOHANG ? 1 : 0) | (options & WUNTRACED ? 2 : 0)), 0);
    if (r == -2) { errno = e; return 0; }           /* WNOHANG: children, none ended yet */
    if (r == -1) { errno = ECHILD; return -1; }     /* (SYS_wait's -1: no such child) */
    if (r < 0) return -1;                           /* EINTR (a handler ran), ECHILD */
    if (pid == -1) {                                /* which one: the child that is gone, or the one now stopped */
        int now[32], nn = children(now, 32);
        pid = 0;
        for (int i = 0; i < nk && !pid; i++) {
            int k = kids[i] < 0 ? -kids[i] : kids[i], j = 0;
            while (j < nn && (now[j] < 0 ? -now[j] : now[j]) != k) j++;
            if (r != ST_STOPPED ? j == nn : j < nn && now[j] < 0 && kids[i] > 0) pid = k;
        }
    }
    if (status) *status = r == ST_STOPPED ? ST_STOP_STATUS : (r & 0xff) << 8;
    errno = e;
    return pid;
}
pid_t wait(int *status) { return waitpid(-1, status, 0); }
pid_t getppid(void)
{
    struct uproc p;
    int me = getpid(), e = errno;
    for (u32 i = 0; sys_procinfo(i, &p) >= 0; i++)
        if (p.state && (int)p.pid == me) { errno = e; return (pid_t)p.ppid; }
    errno = e;
    return 0;
}

/* ---- system and popen: /bin/sh -c */
int system(const char *cmd)
{
    if (!cmd) return access("/bin/sh", X_OK) == 0;
    char *argv[] = { "sh", "-c", (char *)cmd, 0 };
    pid_t pid;
    int st, r = posix_spawn(&pid, "/bin/sh", 0, 0, argv, 0);
    if (r) { errno = r; return -1; }
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return st;
}

struct popen_file { FILE *f; pid_t pid; struct popen_file *next; };
static struct popen_file *popens;
FILE *popen(const char *cmd, const char *mode)
{
    int rd = mode[0] == 'r', fds[2];
    if ((mode[0] != 'r' && mode[0] != 'w') || (mode[1] && mode[1] != 'e')) { errno = EINVAL; return 0; }
    struct popen_file *pf = malloc(sizeof *pf);
    if (!pf) { errno = ENOMEM; return 0; }
    if (pipe(fds) < 0) { free(pf); return 0; }
    posix_spawn_file_actions_t fa;
    int r = posix_spawn_file_actions_init(&fa);
    if (!r) r = posix_spawn_file_actions_adddup2(&fa, fds[rd ? 1 : 0], rd ? 1 : 0);
    char *argv[] = { "sh", "-c", (char *)cmd, 0 };
    if (!r) r = posix_spawn(&pf->pid, "/bin/sh", &fa, 0, argv, 0);
    if (fa) posix_spawn_file_actions_destroy(&fa);
    close(fds[rd ? 1 : 0]);                         /* the child's end: only the child holds it now */
    if (r || !(pf->f = fdopen(fds[rd ? 0 : 1], rd ? "r" : "w"))) {
        if (!r) r = errno;
        close(fds[rd ? 0 : 1]);
        free(pf);
        errno = r;
        return 0;
    }
    pf->next = popens;
    popens = pf;
    return pf->f;
}
int pclose(FILE *f)
{
    struct popen_file **pp = &popens, *pf;
    while (*pp && (*pp)->f != f) pp = &(*pp)->next;
    if (!(pf = *pp)) { errno = ECHILD; return -1; }
    *pp = pf->next;
    fclose(f);                                      /* the end of the pipe: the command sees its input end */
    int st;
    pid_t r;
    while ((r = waitpid(pf->pid, &st, 0)) < 0 && errno == EINTR)
        ;
    free(pf);
    return r < 0 ? -1 : st;
}
