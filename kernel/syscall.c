/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "fsproto.h"

/* copy a NUL-terminated string from user memory, bounds-checked: 0, -EFAULT (outside the caller's memory) or
 * -ENAMETOOLONG (longer than max - 1: a path given to a system call is at most 63 bytes) */
static __attribute__((noinline)) int copy_str(char *dst, u32 uptr, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        if (!user_ok(current, uptr + i, 1))
            return -EFAULT;
        dst[i] = ((const char *)uptr)[i];
        if (!dst[i])
            return 0;
    }
    return -ENAMETOOLONG;
}

static struct file *getfd(u32 fd)
{
    return fd < NFD ? current->fd[fd] : 0;
}

/* May the caller kill, stop, continue or make foreground process p? One rule for SYS_kill, SYS_jobctl and SYS_setfg
 * (audit D): root may (any process but the idle one); others only a process whose real or effective user id is their
 * real or effective user id, or their own child - never a built-in server (diskd, fsd, zfsd, boot:
 * started from the kernel image, ppid 0) or a privileged process. 0, -3 (ESRCH: no such process) or -13 (EACCES). */
static int may_signal(const struct proc *p)
{
    if (!p || p->pid == 0 || p->state == P_FREE)
        return -ESRCH;
    if (current->uid == 0)
        return 0;
    if (p->ppid == 0 || p->priv)
        return -EACCES;
    if (p->ppid == current->pid || p->ruid == current->ruid || p->ruid == current->uid ||
        p->uid == current->ruid || p->uid == current->uid)
        return 0;
    return -EACCES;
}

/* getpriority / setpriority (DESIGN D10). PRIO_PROCESS: who is a pid, 0 the caller; PRIO_USER: every process of user
 * id who (0: the caller's real id); PRIO_PGRP: there are no process groups (-EINVAL). Changing another process needs
 * the kill rule (may_signal); raising a base priority (a lower nice) needs root: -EACCES. getpriority answers 20 - nice
 * (1 .. 40, the best of the matching processes), so a result never looks like an error number; libz turns it back. */
static long do_priority(int set, u32 which, u32 who, s32 nice)
{
    if (which == PRIO_PROCESS) {
        struct proc *p = who ? proc_by_pid((int)who) : current;
        if (!p || p->pid == 0 || p->state == P_FREE || p->state == P_ZOMBIE)
            return -ESRCH;
        if (!set)
            return 20 - p->nice;
        long r = p == current ? 0 : may_signal(p);
        if (r)
            return r;
        if (nice < p->nice && current->uid != 0)
            return -EACCES;
        proc_renice(p, nice);
        return 0;
    }
    if (which != PRIO_USER)
        return -EINVAL;
    u32 uid = who ? who : current->ruid;
    int found = 0, best = NICE_MAX + 1;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &procs[i];
        if (p->state == P_FREE || p->state == P_ZOMBIE || p->ruid != uid)
            continue;
        found = 1;
        if (!set) {
            if (p->nice < best)
                best = p->nice;
            continue;
        }
        if (p != current && may_signal(p))
            return -EACCES;
        if (nice < p->nice && current->uid != 0)
            return -EACCES;
    }
    if (!found)
        return -ESRCH;
    if (!set)
        return 20 - best;
    for (int i = 1; i < NPROC; i++)                 /* every check passed: all or nothing */
        if (procs[i].state != P_FREE && procs[i].state != P_ZOMBIE && procs[i].ruid == uid)
            proc_renice(&procs[i], nice);
    return 0;
}

/* spawn's arguments: up to SPAWN_ARGC arguments and SPAWN_ARGBYTES bytes of text (copied to the kernel heap, then to
 * the top of the new program's stack); more is -E2BIG ("argument list too long"), never a silent cut */
#define SPAWN_ARGC     256
#define SPAWN_ARGBYTES 8192

static long do_spawn(u32 upath, u32 uargv, u32 ustdio)
{
    char path[64];
    int argc = 0, fds[3], *stdio = 0;
    u32 used = 0;
    int c0 = copy_str(path, upath, sizeof path);
    if (c0)
        return c0;
    char *strs = kmalloc(SPAWN_ARGBYTES);
    char **argv = kmalloc((SPAWN_ARGC + 1) * sizeof(char *));
    long r = 0;
    if (!strs || !argv) {
        r = -ENOMEM;
        goto out;
    }
    if (uargv) {
        for (;;) {
            u32 ap;
            if (!user_ok(current, uargv + (u32)argc * 4, 4)) { r = -EFAULT; goto out; }
            ap = ((const u32 *)uargv)[argc];
            if (!ap)
                break;
            if (argc == SPAWN_ARGC || used >= SPAWN_ARGBYTES - 1) { r = -E2BIG; goto out; }
            argv[argc] = strs + used;
            int c = copy_str(strs + used, ap, SPAWN_ARGBYTES - used);
            if (c) {                    /* bad pointer, or the text does not fit */
                r = user_ok(current, ap, 1) ? -E2BIG : -EFAULT;
                goto out;
            }
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
        if (!user_ok(current, ustdio, 12)) { r = -EFAULT; goto out; }
        for (int i = 0; i < 3; i++)
            fds[i] = (int)((const u32 *)ustdio)[i];
        stdio = fds;
    }
    r = proc_spawn(path, argv, argc, current, stdio, 0);
out:
    kfree(strs);
    kfree(argv);
    return r;
}

/* TRAP #0 handler. rb[0..7] = d0-d7, rb[8..14] = a0-a6. d0 = number,
 * d1-d3 = arguments; the result goes back into rb[0]. */
void syscall_c(u32 *rb)
{
    u32 a1 = rb[1], a2 = rb[2], a3 = rb[3];
    s32 r = -ENOSYS;                    /* an unknown call number; every failing check below sets its own errno */
    struct file *f;
    /* the checks, each setting r when it fails (so a condition reads as "all of these hold") */
#define CSTR(dst, u)  ((r = copy_str(dst, u, sizeof dst)) == 0)
#define UOK(p, n)     (user_ok(current, p, n) || ((r = -EFAULT), 0))
#define GETFD(f_, fd) (((f_) = getfd(fd)) || ((r = -EBADF), 0))

    {   /* the stack guard (proc.c) at every system call: the quick check, the full one if it fails (the register block
         * is the kernel stack pointer) */
        u32 usp = get_usp();
        int bad = stack_user_suspect(current, usp) ? stack_bad(current, (u32)rb - 64, usp) : 0;
        if (bad) {
            stack_fail(current, bad);
            proc_exit(139);
        }
    }
    switch (rb[0]) {
    case SYS_exit:
        proc_exit((int)a1);
    case SYS_write:
        if (GETFD(f, a1) && UOK(a2, a3))
            r = (s32)vfs_write(f, (const void *)a2, a3);
        break;
    case SYS_read:
        if (GETFD(f, a1) && UOK(a2, a3))
            r = (s32)vfs_read(f, (void *)a2, a3);
        break;
    case SYS_yield:
#ifdef STACK_TEST
        /* test builds only (make DIAG=-DSTACK_TEST, tools/diag-test.sh stack): `crash kstack` / `crash idle` damage
         * the guard of their own kernel stack / of pid 0's, as an overflow would, to test the check */
        if (a1 == 0x4B53544Bu)          /* "KSTK" */
            *(u32 *)current->kstk = 0;
        if (a1 == 0x49444C45u)          /* "IDLE" */
            *(u32 *)procs[0].kstk = 0;
#endif
        schedule();
        r = 0;
        break;
    case SYS_getpid:
        r = current->pid;
        break;
    case SYS_spawn:
        r = (s32)do_spawn(a1, a2, a3);
        break;
    case SYS_wait:
        r = proc_wait((int)a1, (int)a2);
        break;
    case SYS_sleep: {
        u32 until = ticks + a1;
        while ((s32)(ticks - until) < 0 && !current->killed) {    /* a stop and continue in between wakes it early */
            sleep_until(until);
            schedule();
        }
        r = 0;
        break;
    }
    case SYS_open: {
        char path[64];
        if (CSTR(path, a1))
            r = (s32)vfs_open(path, (int)a2);
        break;
    }
    case SYS_mkdir: case SYS_unlink: case SYS_rmdir: {
        char path[64];
        if (CSTR(path, a1))
            r = (s32)(rb[0] == SYS_mkdir ? vfs_mkdir(path) :
                      rb[0] == SYS_unlink ? vfs_unlink(path) : vfs_rmdir(path));
        break;
    }
    case SYS_rename: {
        char from[64], to[64];
        if (CSTR(from, a1) && CSTR(to, a2))
            r = (s32)vfs_rename(from, to);
        break;
    }
    case SYS_port_create: {
        char name[16];
        if (CSTR(name, a1))
            r = port_create(name, current->pid);
        break;
    }
    case SYS_port_find: {
        char name[16];
        if (CSTR(name, a1))
            r = port_find(name);
        break;
    }
    case SYS_call:
        if (UOK(a2, sizeof(struct umsg)))
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
    case SYS_statfs:
        if (UOK(a1, 20))
            r = (s32)vfs_statfs((void *)a1, a2);       /* a2 = the n-th mount, as SYS_mountinfo counts (0 = the root) */
        break;
    case SYS_mount: {                   /* root only */
        struct umountreq rq;
        char path[64], port[16], dev[16], type[8];
        if (current->uid != 0) { r = -EACCES; break; }
        if (UOK(a1, sizeof rq)) {
            rq = *(const struct umountreq *)a1;
            if (CSTR(path, (u32)rq.path) && CSTR(port, (u32)rq.port) &&
                CSTR(dev, (u32)rq.dev) && CSTR(type, (u32)rq.type)) {
                rq.path = path; rq.port = port; rq.dev = dev; rq.type = type;
                r = (s32)vfs_mount(&rq);
            }
        }
        break;
    }
    case SYS_umount: {
        char path[64];
        if (current->uid != 0) { r = -EACCES; break; }
        if (CSTR(path, a1))
            r = (s32)vfs_umount(path);
        break;
    }
    case SYS_mountinfo:
        if (UOK(a2, sizeof(struct umountinfo)))
            r = (s32)vfs_mountinfo(a1, (struct umountinfo *)a2);
        break;
    case SYS_blk_flush:
        r = current->priv ? blk_flush() : -EPERM;
        break;
    case SYS_sync:
        r = (s32)vfs_sync();
        break;
    case SYS_blk_ram:
        r = -EPERM;
        if (current->priv && UOK(a1, 8)) {
            ((u32 *)a1)[0] = boot_ram_pa;
            ((u32 *)a1)[1] = boot_ram_size;
            r = 0;
        }
        break;
    case SYS_fd_info:
        r = current->priv ? fd_info(a1) : -EPERM;
        break;
    case SYS_fd_rw:
        r = current->priv ? fd_rw(a1 >> 28, a1 & 0x07FFFFFF, a2, (void *)a3, (a1 >> 27) & 1) : -EPERM;
        break;
    case SYS_blk_size:
        r = current->priv ? (s32)blk_sectors() : -EPERM;
        break;
    case SYS_ttymode:
        tty_setmode(tty_of(current), (int)a1, current->pid);
        r = 0;
        break;
    case SYS_ttyinfo:
        r = term_size();
        break;
    case SYS_envset:
        r = -E2BIG;
        if (a2 <= ENVMAX && UOK(a1, a2)) {
            memcpy(current->env, (const void *)a1, a2);
            current->envlen = (u16)a2;
            r = 0;
        }
        break;
    case SYS_envget:
        if (UOK(a1, a2)) {
            u32 n = current->envlen < a2 ? current->envlen : a2;
            memcpy((void *)a1, current->env, n);
            r = (s32)current->envlen;
        }
        break;
    case SYS_dup:
        if (GETFD(f, a1)) {
            r = -EMFILE;
            for (int i = 3; i < NFD; i++)
                if (!current->fd[i]) {
                    current->fd[i] = file_dup(f);
                    r = i;
                    break;
                }
        }
        break;
    case SYS_dup2:
        if (GETFD(f, a1) && (a2 < NFD || ((r = -EBADF), 0))) {
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
        if (CSTR(path, a1))
            r = (s32)vfs_path1(rb[0] == SYS_chmod ? FS_CHMOD : FS_UTIMES, path, a2, 0);
        break;
    }
    case SYS_lstat: {
        char path[64];
        if (CSTR(path, a1) && UOK(a2, sizeof(struct ustat)))
            r = (s32)vfs_path1(FS_LSTAT, path, a2, 0);
        break;
    }
    case SYS_readlink: {
        char path[64];
        if (CSTR(path, a1) && UOK(a2, a3))
            r = (s32)vfs_path1(FS_READLINK, path, a2, a3);
        break;
    }
    case SYS_link: case SYS_symlink: {
        char p1[64], p2[64];
        if (CSTR(p1, a1) && CSTR(p2, a2))
            r = (s32)vfs_link(rb[0] == SYS_link ? FS_LINK : FS_SYMLINK, p1, p2);
        break;
    }
    case SYS_time:
        r = (s32)(time_base + ticks / 100);
        break;
    case SYS_settime:                   /* root only */
        if (current->uid != 0) { r = -EACCES; break; }
        time_base = a1 - ticks / 100;
        r = 0;
        break;
    case SYS_getuid: r = current->ruid; break;
    case SYS_geteuid: r = current->uid; break;
    case SYS_getgid: r = current->gid; break;
    case SYS_setuid:                    /* effective root: becomes anyone (real and effective); others: only back to their real id */
        if (current->uid == 0) { current->uid = current->ruid = (u16)a1; r = 0; }
        else if (a1 == current->ruid) { current->uid = (u16)a1; r = 0; }
        else r = -EPERM;
        break;
    case SYS_setgid:
        if (current->uid == 0 || a1 == current->gid) { current->gid = (u16)a1; r = 0; }
        else r = -EPERM;
        break;
    case SYS_umask: r = current->umask; current->umask = (u16)(a1 & 0777); break;
    case SYS_chown: {
        char path[64];
        if (current->uid != 0) { r = -EACCES; break; }
        if (CSTR(path, a1)) r = (s32)vfs_path1(FS_CHOWN, path, a2, a3);
        break;
    }
    case SYS_isatty:
        r = GETFD(f, a1) && f->type == FT_TTY;
        break;
    case SYS_hrtime:
        r = (s32)hr_time();
        break;
    case SYS_ttymirror:                 /* root only: the serial copy is the test harness's and the cable's */
        if (current->uid != 0) { r = -EACCES; break; }
        tty_mirror = (int)a1;
        r = 0;
        break;
    case SYS_close:
        if (GETFD(f, a1)) {
            current->fd[a1] = 0;
            file_close(f);
            r = 0;
        }
        break;
    case SYS_lseek:
        if (GETFD(f, a1) && (f->type == FT_INODE || ((r = -ESPIPE), 0)))
            r = (s32)vfs_lseek(f, (s32)a2, a3);
        break;
    case SYS_readdir:
        if (GETFD(f, a1) && UOK(a2, sizeof(struct udirent)))
            r = (s32)vfs_readdir(f, (struct udirent *)a2);
        break;
    case SYS_stat: {
        char path[64];
        if (CSTR(path, a1) &&
            UOK(a2, sizeof(struct ustat)))
            r = (s32)vfs_stat(path, (struct ustat *)a2);
        break;
    }
    case SYS_fstat:
        if (GETFD(f, a1) && UOK(a2, sizeof(struct ustat)))
            r = (s32)vfs_fstat(f, (struct ustat *)a2);
        break;
    case SYS_chdir: {
        char path[64];
        if (CSTR(path, a1))
            r = (s32)vfs_chdir(path);
        break;
    }
    case SYS_blk_read: case SYS_blk_write:
        /* raw disk: only the disk server (trusted; the buffer is a client's, so
         * it is not checked against this process's own memory) */
        r = !current->priv ? -EPERM : rb[0] == SYS_blk_read ? blk_read(a1, a2, (void *)a3)
                                                              : blk_write(a1, a2, (const void *)a3);
        break;
    case SYS_input:
        if (UOK(a1, 16)) {
            input_read((s32 *)a1);
            r = 0;
        }
        break;
    case SYS_klog:
        r = 0;
        if (a3 && UOK(a2, a3))
            r = (s32)klog_read(a1, (char *)a2, a3);
        break;
    case SYS_getcwd: {
        u32 n = 0;
        while (current->cwd[n])
            n++;
        if ((n + 1 <= a2 || ((r = -ERANGE), 0)) && UOK(a1, n + 1)) {
            memcpy((void *)a1, current->cwd, n + 1);
            r = (s32)n;
        }
        break;
    }
    case SYS_pipe:
        if (UOK(a1, 8)) {
            int fds[2];
            if ((r = (s32)vfs_pipe(fds)) == 0) {
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
        r = -EINVAL;                    /* not a region memalloc gave this process */
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
        if ((a1 < NPROC || ((r = -EINVAL), 0)) && UOK(a2, sizeof(struct uproc))) {
            struct proc *p = &procs[a1];
            struct uproc *u = (struct uproc *)a2;
            u->pid = (u32)p->pid;
            u->ppid = (u32)p->ppid;
            u->state = (u32)p->state;
            u->mem = p->span;
            memcpy(u->name, p->name, 16);
            u->nice = p->nice;
            u->pri = p->prio;
            r = 0;
        }
        break;
    case SYS_meminfo:
        if (UOK(a1, 16)) {
            mem_stats(&((u32 *)a1)[0], &((u32 *)a1)[1]);
            ((u32 *)a1)[2] = ((u32 *)a1)[3] = 0;
            r = 0;
        }
        break;
    case SYS_setfg:                     /* on the caller's own tty; 0 = nobody, else a process the caller may signal */
        if (a1 == 0 || !(r = may_signal(proc_by_pid((int)a1)))) {
            tty_setfg(tty_of(current), (int)a1);
            r = 0;
        }
        break;
    case SYS_jobctl: {
        struct proc *p = proc_by_pid((int)a1);
        if (!(r = may_signal(p))) {
            if (a2 == 0) proc_stop(p); else proc_cont(p);
        }
        break;
    }
    case SYS_getpriority:
        r = (s32)do_priority(0, a1, a2, 0);
        break;
    case SYS_setpriority:
        r = (s32)do_priority(1, a1, a2, (s32)a3);
        break;
    case SYS_reboot:                    /* like the reset warning: sync, stop disk writes, then the reset */
        if (current->uid != 0)
            r = -EPERM;
        else if (a1 > BOOTNOTE_ZENIX)
            r = -EINVAL;
        else if (a1 && (r = (s32)bootnote_set(a1)) < 0)
            ;
        else {
            long e = vfs_sync();
            disk_frozen = 1;
            klog_set_reason(KLOG_END_REBOOT);
            kprintf("reboot: filesystems synced%s, disk writes stopped; resetting%s\n", e < 0 ? " (with an error)" : "",
                    a1 == BOOTNOTE_AMIGAOS ? " into AmigaOS" : a1 == BOOTNOTE_ZENIX ? " into Zenix" : "");
            machine_reset();
        }
        break;
    case SYS_svc: {                     /* D11: op | nice << 8, name, program text */
        char name[16], text[SVC_TEXT];
        u32 op = a1 & 0xFF;
        if (op != SVC_SELF && !CSTR(name, a2))
            break;
        if (op == SVC_REGISTER && !CSTR(text, a3))
            break;
        r = (s32)svc_sys(a1, op == SVC_SELF ? 0 : name, text);
        break;
    }
    case SYS_kill: {
        struct proc *p = proc_by_pid((int)a1);
        if (!(r = may_signal(p)))
            proc_kill(p);
        break;
    }
    }
    rb[0] = (u32)r;
    if (kwork_pending)
        kwork();                        /* queued input: done here, on the way back to user mode (main.c) */
    if (need_resched)
        resched();                      /* a better process became ready (a reply, a wake, input): it runs now */
    if (proc_kill_now())
        proc_exit(current->killed == 2 ? 139 : 130);
}
