#include "kernel.h"

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
        r = proc_wait((int)a1);
        break;
    case SYS_sleep:
        current->wake = ticks + a1;
        current->state = P_SLEEP;
        nsleep++;
        schedule();
        r = 0;
        break;
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
    case SYS_spawn_tty:                 /* only the session manager (priv) */
        if (current->priv)
            r = (s32)do_spawn(a1, a2, 0, (int)a3);
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
                block_io();
            }
        }
        break;
    case SYS_statfs:
        if (user_ok(current, a1, 20))
            r = (s32)vfs_statfs((void *)a1);
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
    case SYS_blk_size:
        if (current->priv)
            r = (s32)ide_sectors();
        break;
    case SYS_ttymode:
        tty_setmode(tty_of(current), (int)a1, current->pid);
        r = 0;
        break;
    case SYS_ttyinfo:
        r = term_size();
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
            r = rb[0] == SYS_blk_read ? ide_read(a1, a2, (void *)a3)
                                      : ide_write(a1, a2, (const void *)a3);
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
    case SYS_memalloc:
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
    if (current->killed)
        proc_exit(130);
}
