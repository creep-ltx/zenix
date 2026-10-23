#include "kernel.h"
#include "fsproto.h"

/* copy a NUL-terminated string from user memory, bounds-checked */
static int copy_str(char *dst, u32 uptr, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        if (!user_ok(current, uptr + i, 1))
            return -1;
        dst[i] = ((const char *)uptr)[i];
        if (!dst[i])
            return 0;
    }
    return -1;
}

static struct file *getfd(u32 fd)
{
    return fd < NFD ? current->fd[fd] : 0;
}

/* the logged-in session (set by login): workspace shells started by the session manager run as this user */
static u16 sess_uid, sess_gid, sess_envlen;
static char sess_env[ENVMAX];

static long do_spawn(u32 upath, u32 uargv, u32 ustdio, int tty)
{
    char path[64], strs[512], *argv[17];
    int argc = 0, fds[3], *stdio = 0;
    u32 used = 0;
    if (copy_str(path, upath, sizeof path))
        return -1;
    if (uargv) {
        for (;;) {
            u32 ap;
            if (!user_ok(current, uargv + (u32)argc * 4, 4))
                return -1;
            ap = ((const u32 *)uargv)[argc];
            if (!ap)
                break;
            if (argc == 16 || used >= sizeof strs - 1)
                return -1;
            argv[argc] = strs + used;
            if (copy_str(strs + used, ap, sizeof strs - used))
                return -1;
            while (strs[used])
                used++;
            used++;
            argc++;
        }
    }
    if (argc == 0) {
        argv[0] = path;
        argc = 1;
    }
    argv[argc] = 0;
    if (ustdio) {
        if (!user_ok(current, ustdio, 12))
            return -1;
        for (int i = 0; i < 3; i++)
            fds[i] = (int)((const u32 *)ustdio)[i];
        stdio = fds;
    }
    return proc_spawn(path, argv, argc, current, stdio, tty);
}

/* TRAP #0 handler. rb[0..7] = d0-d7, rb[8..14] = a0-a6. d0 = number,
 * d1-d3 = arguments; the result goes back into rb[0]. */
void syscall_c(u32 *rb)
{
    u32 a1 = rb[1], a2 = rb[2], a3 = rb[3];
    s32 r = -1;
    struct file *f;

    switch (rb[0]) {
    case SYS_exit:
        proc_exit((int)a1);
    case SYS_write:
        if ((f = getfd(a1)) && user_ok(current, a2, a3))
            r = (s32)vfs_write(f, (const void *)a2, a3);
        break;
    case SYS_read:
        if ((f = getfd(a1)) && user_ok(current, a2, a3))
            r = (s32)vfs_read(f, (void *)a2, a3);
        break;
    case SYS_yield:
        schedule();
        r = 0;
        break;
    case SYS_getpid:
        r = current->pid;
        break;
    case SYS_spawn:
        r = (s32)do_spawn(a1, a2, a3, 0);
        break;
    case SYS_wait:
        r = proc_wait((int)a1, (int)a2);
        break;
    case SYS_sleep: {
        u32 until = ticks + a1;
        while ((s32)(ticks - until) < 0 && !current->killed) {    /* a stop and continue in between wakes it early */
            current->wake = until;
            current->state = P_SLEEP;
            nsleep++;
            schedule();
        }
        r = 0;
        break;
    }
    case SYS_open: {
        char path[64];
        if (!copy_str(path, a1, sizeof path))
            r = (s32)vfs_open(path, (int)a2);
        break;
    }
    case SYS_mkdir: case SYS_unlink: case SYS_rmdir: {
        char path[64];
        if (!copy_str(path, a1, sizeof path))
            r = (s32)(rb[0] == SYS_mkdir ? vfs_mkdir(path) :
                      rb[0] == SYS_unlink ? vfs_unlink(path) : vfs_rmdir(path));
        break;
    }
    case SYS_rename: {
        char from[64], to[64];
        if (!copy_str(from, a1, sizeof from) && !copy_str(to, a2, sizeof to))
            r = (s32)vfs_rename(from, to);
        break;
    }
    case SYS_port_create: {
        char name[16];
        if (!copy_str(name, a1, sizeof name))
            r = port_create(name, current->pid);
        break;
    }
    case SYS_port_find: {
        char name[16];
        if (!copy_str(name, a1, sizeof name))
            r = port_find(name);
        break;
    }
    case SYS_call:
        if (user_ok(current, a2, sizeof(struct umsg)))
            r = (s32)ipc_call((int)a1, (struct umsg *)a2);
        break;
    case SYS_recv:
        r = (s32)ipc_recv((int)a1, (int)a2);
        break;
    case SYS_reply:
        r = (s32)ipc_reply((struct umsg *)a1, (s32)a2);
        break;
    case SYS_replyrecv:
        r = (s32)ipc_replyrecv((struct umsg *)a1, (s32)a2, (int)a3);
        break;
    case SYS_spawn_tty:                 /* only the session manager (priv): the new shell belongs to the logged-in user */
        if (current->priv) {
            u16 u = current->uid, ru = current->ruid, g = current->gid, um = current->umask, el = current->envlen;
            static char saved_env[ENVMAX];
            memcpy(saved_env, current->env, ENVMAX);
            current->uid = current->ruid = sess_uid; current->gid = sess_gid; current->umask = 022;
            memcpy(current->env, sess_env, ENVMAX); current->envlen = sess_envlen;
            r = (s32)do_spawn(a1, a2, 0, (int)a3);
            current->uid = u; current->ruid = ru; current->gid = g; current->umask = um;
            memcpy(current->env, saved_env, ENVMAX); current->envlen = el;
        }
        break;
    case SYS_session:                   /* login: remember who is logged in (ids + environment) for workspace shells */
        if (current->uid == 0) {
            sess_uid = (u16)a1; sess_gid = (u16)a2;
            memcpy(sess_env, current->env, ENVMAX); sess_envlen = current->envlen;
            r = 0;
        } else r = -1;
        break;
    case SYS_wsreq:
        if (current->priv) {
            for (;;) {
                if (current->killed)
                    break;
                for (int w = 1; w <= 9; w++)
                    if (ws_spawn_pending[w]) {
                        ws_spawn_pending[w] = 0;
                        r = w;
                        break;
                    }
                if (r > 0)
                    break;
                block_io();             /* race-free: ws_spawn_pending is only set by kwork, which never runs between
                                         * the check above and this block (the points in main.c, audit B5) */
            }
        }
        break;
    case SYS_statfs:
        if (user_ok(current, a1, 20))
            r = (s32)vfs_statfs((void *)a1, a2);       /* a2 = mount index, 0 = the root */
        break;
    case SYS_mount: {                   /* root only */
        struct umountreq rq;
        char path[64], port[16], dev[16], type[8];
        if (current->uid != 0) { r = -13; break; }
        if (user_ok(current, a1, sizeof rq)) {
            rq = *(const struct umountreq *)a1;
            if (!copy_str(path, (u32)rq.path, sizeof path) && !copy_str(port, (u32)rq.port, sizeof port) &&
                !copy_str(dev, (u32)rq.dev, sizeof dev) && !copy_str(type, (u32)rq.type, sizeof type)) {
                rq.path = path; rq.port = port; rq.dev = dev; rq.type = type;
                r = (s32)vfs_mount(&rq);
            }
        }
        break;
    }
    case SYS_umount: {
        char path[64];
        if (current->uid != 0) { r = -13; break; }
        if (!copy_str(path, a1, sizeof path))
            r = (s32)vfs_umount(path);
        break;
    }
    case SYS_mountinfo:
        if (user_ok(current, a2, sizeof(struct umountinfo)))
            r = (s32)vfs_mountinfo(a1, (struct umountinfo *)a2);
        break;
    case SYS_blk_flush:
        if (current->priv)
            r = blk_flush();
        break;
    case SYS_sync:
        r = (s32)vfs_sync();
        break;
    case SYS_blk_ram:
        if (current->priv && user_ok(current, a1, 8)) {
            ((u32 *)a1)[0] = boot_ram_pa;
            ((u32 *)a1)[1] = boot_ram_size;
            r = 0;
        }
        break;
    case SYS_fd_info:
        if (current->priv)
            r = fd_info(a1);
        break;
    case SYS_fd_rw:
        if (current->priv)
            r = fd_rw(a1 >> 28, a1 & 0x07FFFFFF, a2, (void *)a3, (a1 >> 27) & 1);
        break;
    case SYS_blk_size:
        if (current->priv)
            r = (s32)blk_sectors();
        break;
    case SYS_ttymode:
        tty_setmode(tty_of(current), (int)a1, current->pid);
        r = 0;
        break;
    case SYS_ttyinfo:
        r = term_size();
        break;
    case SYS_envset:
        if (a2 <= ENVMAX && user_ok(current, a1, a2)) {
            memcpy(current->env, (const void *)a1, a2);
            current->envlen = (u16)a2;
            r = 0;
        }
        break;
    case SYS_envget:
        if (user_ok(current, a1, a2)) {
            u32 n = current->envlen < a2 ? current->envlen : a2;
            memcpy((void *)a1, current->env, n);
            r = (s32)current->envlen;
        }
        break;
    case SYS_dup:
        if ((f = getfd(a1))) {
            for (int i = 3; i < NFD; i++)
                if (!current->fd[i]) {
                    current->fd[i] = file_dup(f);
                    r = i;
                    break;
                }
        }
        break;
    case SYS_dup2:
        if ((f = getfd(a1)) && a2 < NFD) {
            if (a1 == a2) {
                r = (s32)a2;
            } else {
                if (current->fd[a2])
                    file_close(current->fd[a2]);
                current->fd[a2] = file_dup(f);
                r = (s32)a2;
            }
        }
        break;
    case SYS_ttyintr:
        r = tty_take_intr(tty_of(current));
        break;
    case SYS_chmod: case SYS_utimes: {
        char path[64];
        if (!copy_str(path, a1, sizeof path))
            r = (s32)vfs_path1(rb[0] == SYS_chmod ? FS_CHMOD : FS_UTIMES, path, a2, 0);
        break;
    }
    case SYS_lstat: {
        char path[64];
        if (!copy_str(path, a1, sizeof path) && user_ok(current, a2, sizeof(struct ustat)))
            r = (s32)vfs_path1(FS_LSTAT, path, a2, 0);
        break;
    }
    case SYS_readlink: {
        char path[64];
        if (!copy_str(path, a1, sizeof path) && user_ok(current, a2, a3))
            r = (s32)vfs_path1(FS_READLINK, path, a2, a3);
        break;
    }
    case SYS_link: case SYS_symlink: {
        char p1[64], p2[64];
        if (!copy_str(p1, a1, sizeof p1) && !copy_str(p2, a2, sizeof p2))
            r = (s32)vfs_link(rb[0] == SYS_link ? FS_LINK : FS_SYMLINK, p1, p2);
        break;
    }
    case SYS_time:
        r = (s32)(time_base + ticks / 100);
        break;
    case SYS_settime:
        time_base = a1 - ticks / 100;
        r = 0;
        break;
    case SYS_getuid: r = current->ruid; break;
    case SYS_geteuid: r = current->uid; break;
    case SYS_getgid: r = current->gid; break;
    case SYS_setuid:                    /* effective root: becomes anyone (real and effective); others: only back to their real id */
        if (current->uid == 0) { current->uid = current->ruid = (u16)a1; r = 0; }
        else if (a1 == current->ruid) { current->uid = (u16)a1; r = 0; }
        else r = -1;
        break;
    case SYS_setgid:
        if (current->uid == 0 || a1 == current->gid) { current->gid = (u16)a1; r = 0; }
        else r = -1;
        break;
    case SYS_umask: r = current->umask; current->umask = (u16)(a1 & 0777); break;
    case SYS_chown: {
        char path[64];
        if (current->uid != 0) { r = -13; break; }
        if (!copy_str(path, a1, sizeof path)) r = (s32)vfs_path1(FS_CHOWN, path, a2, a3);
        break;
    }
    case SYS_isatty:
        r = (f = getfd(a1)) && f->type == FT_TTY;
        break;
    case SYS_hrtime:
        r = (s32)hr_time();
        break;
    case SYS_ttymirror:
        tty_mirror = (int)a1;
        r = 0;
        break;
    case SYS_close:
        if ((f = getfd(a1))) {
            current->fd[a1] = 0;
            file_close(f);
            r = 0;
        }
        break;
    case SYS_lseek:
        if ((f = getfd(a1)) && f->type == FT_INODE)
            r = (s32)vfs_lseek(f, (s32)a2, a3);
        break;
    case SYS_readdir:
        if ((f = getfd(a1)) && user_ok(current, a2, sizeof(struct udirent)))
            r = (s32)vfs_readdir(f, (struct udirent *)a2);
        break;
    case SYS_stat: {
        char path[64];
        if (!copy_str(path, a1, sizeof path) &&
            user_ok(current, a2, sizeof(struct ustat)))
            r = (s32)vfs_stat(path, (struct ustat *)a2);
        break;
    }
    case SYS_fstat:
        if ((f = getfd(a1)) && user_ok(current, a2, sizeof(struct ustat)))
            r = (s32)vfs_fstat(f, (struct ustat *)a2);
        break;
    case SYS_chdir: {
        char path[64];
        if (!copy_str(path, a1, sizeof path))
            r = (s32)vfs_chdir(path);
        break;
    }
    case SYS_blk_read: case SYS_blk_write:
        /* raw disk: only the disk server (trusted; the buffer is a client's, so
         * it is not checked against this process's own memory) */
        if (current->priv)
            r = rb[0] == SYS_blk_read ? blk_read(a1, a2, (void *)a3)
                                      : blk_write(a1, a2, (const void *)a3);
        break;
    case SYS_input:
        if (user_ok(current, a1, 16)) {
            input_read((s32 *)a1);
            r = 0;
        }
        break;
    case SYS_klog:
        if (a3 && user_ok(current, a2, a3))
            r = (s32)klog_read(a1, (char *)a2, a3);
        break;
    case SYS_getcwd: {
        u32 n = 0;
        while (current->cwd[n])
            n++;
        if (n + 1 <= a2 && user_ok(current, a1, n + 1)) {
            memcpy((void *)a1, current->cwd, n + 1);
            r = (s32)n;
        }
        break;
    }
    case SYS_pipe:
        if (user_ok(current, a1, 8)) {
            int fds[2];
            if (vfs_pipe(fds) == 0) {
                ((u32 *)a1)[0] = (u32)fds[0];
                ((u32 *)a1)[1] = (u32)fds[1];
                r = 0;
            }
        }
        break;
    case SYS_memalloc:                  /* the region's address, or 0 (NULL) when it cannot be given: never -1 */
        r = 0;
        if (a1 == 0 || a1 > MEMALLOC_MAX)
            break;
        for (int i = 0; i < NREG; i++)
            if (!current->reg[i].base) {
                void *m = mem_alloc(a1, 0, (u32)current->pid);
                if (m) {
                    current->reg[i].base = (u32)m;
                    current->reg[i].size = a1;
                    r = (s32)m;
                }
                break;
            }
        break;
    case SYS_memfree:
        for (int i = 0; i < NREG; i++)
            if (current->reg[i].base == a1 && a1) {
                mem_free((void *)a1);
                current->reg[i].base = 0;
                r = 0;
                break;
            }
        break;
    case SYS_ticks:
        r = (s32)ticks;
        break;
    case SYS_procinfo:
        if (a1 < NPROC && user_ok(current, a2, sizeof(struct uproc))) {
            struct proc *p = &procs[a1];
            struct uproc *u = (struct uproc *)a2;
            u->pid = (u32)p->pid;
            u->ppid = (u32)p->ppid;
            u->state = (u32)p->state;
            u->mem = p->span;
            memcpy(u->name, p->name, 16);
            r = 0;
        }
        break;
    case SYS_meminfo:
        if (user_ok(current, a1, 16)) {
            mem_stats(&((u32 *)a1)[0], &((u32 *)a1)[1]);
            ((u32 *)a1)[2] = ((u32 *)a1)[3] = 0;
            r = 0;
        }
        break;
    case SYS_setfg:
        tty_setfg(tty_of(current), (int)a1);
        r = 0;
        break;
    case SYS_jobctl: {
        struct proc *p = proc_by_pid((int)a1);
        if (p && p->pid != 0 && (current->uid == 0 || p->uid == current->uid || p->ppid == current->pid)) {
            if (a2 == 0) proc_stop(p); else proc_cont(p);
            r = 0;
        }
        break;
    }
    case SYS_kill: {
        struct proc *p = proc_by_pid((int)a1);
        if (p && p->pid != 0) {
            proc_kill(p);
            r = 0;
        }
        break;
    }
    }
    rb[0] = (u32)r;
    if (kwork_pending)
        kwork();                        /* queued input: done here, on the way back to user mode (main.c) */
    if (current->killed)
        proc_exit(130);
}
