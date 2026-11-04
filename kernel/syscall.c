/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "fsproto.h"
#include "version.h"

/* The version record (DESIGN D15): "ZVER", the number, the string. tools/mkzup.py finds it in the kernel image and
 * refuses a bundle whose number is not the kernel's; SYS_update INFO reports it. Frozen layout, grown at its end
 * (DESIGN D19 step 1): "ZSRV" and the root filesystems this kernel has a server built in for (ZVER_ROOT_*), which
 * zenixboot reads to refuse an update whose kernel cannot serve the machine's root. A record without "ZSRV" is
 * older: every kernel before it served both ext2 and ZenixFS. */
#define ZVER_ROOT_EXT2    1u
#define ZVER_ROOT_ZENIXFS 2u
#ifdef KERNEL_ZFSD
#define ZVER_ROOTS (ZVER_ROOT_EXT2 | ZVER_ROOT_ZENIXFS)
#else
#define ZVER_ROOTS ZVER_ROOT_EXT2
#endif
static const struct { char magic[4]; u32 vnum; char vstr[16]; char smagic[4]; u32 roots; } zver
    __attribute__((used, aligned(4))) = { { 'Z', 'V', 'E', 'R' }, ZENIX_VNUM, ZENIX_VSTR, { 'Z', 'S', 'R', 'V' }, ZVER_ROOTS };

/* copy a NUL-terminated string from user memory, bounds-checked: 0, -EFAULT (outside what the caller may read: its
 * own memory or its shared code, D13) or -ENAMETOOLONG (longer than max - 1: a path given to a system call is at most
 * FS_PATHMAX - 1 bytes, fsproto.h) */
static __attribute__((noinline)) int copy_str(char *dst, u32 uptr, u32 max)
{
    u32 ok = user_rd_extent(current, uptr);     /* readable bytes from uptr on, in the one area it lies in */
    for (u32 i = 0; i < max; i++) {
        if (i >= ok)
            return -EFAULT;
        dst[i] = ((const char *)uptr)[i];
        if (!dst[i])
            return 0;
    }
    return -ENAMETOOLONG;
}

/* Frozen interfaces (CLAUDE.md): a struct the kernel or a server writes into a program's memory never overruns the
 * caller's buffer. The caller passes its buffer's size in an argument every libz before D15 passed as 0; 0 means the
 * struct's first size (it only ever grows at its end), a size below that is refused, a bigger one gets at most the
 * kernel's own size. sizes = first << 16 | now. The bytes the caller gets, or -EINVAL / -EFAULT */
_Static_assert(__builtin_offsetof(struct ustat, links) + 4 == USTAT_FIRST, "ustat's first size is frozen");
_Static_assert(__builtin_offsetof(struct udirent, name) + 64 == UDIRENT_FIRST, "udirent's first size is frozen");
_Static_assert(__builtin_offsetof(struct umountinfo, type) + 8 == UMOUNTINFO_FIRST, "umountinfo's first size is frozen");
_Static_assert(__builtin_offsetof(struct uupdate, kver) + 16 == UUPDATE_FIRST, "uupdate's first size is frozen");
_Static_assert(__builtin_offsetof(struct uproc, name) + 16 == UPROC_FIRST, "uproc's first size is frozen");
_Static_assert(__builtin_offsetof(struct uconread, pad) + 2 == UCONREAD_FIRST, "uconread's first size is frozen");
_Static_assert(__builtin_offsetof(struct uconline, palette) + 32 == UCONLINE_FIRST, "uconline's first size is frozen");
static __attribute__((noinline)) s32 usize(u32 p, u32 given, u32 sizes)
{
    u32 n = usize_n(given, sizes >> 16, sizes & 0xFFFF);
    return !n ? -EINVAL : user_ok_wr(current, p, n) ? (s32)n : -EFAULT;
}

static struct file *getfd(u32 fd)
{
    return fd < NFD ? current->fd[fd] : 0;
}

/* May the caller kill, stop, continue or make foreground process p? One rule for SYS_kill, SYS_jobctl and SYS_setfg
 * (audit D): root may (any process but the idle one); others only a process whose real or effective user id is their
 * real or effective user id, or their own child - never a built-in server (diskd, fsd, zfsd, boot:
 * started from the kernel image, ppid 0) or a privileged process. 0, -3 (ESRCH: no such process) or -13 (EACCES). */
int may_signal(const struct proc *p)
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

/* SYS_groups (kernel.h): the caller's supplementary groups. GET: count 0 only counts, a smaller room than the list
 * is -EINVAL (as Linux's getgroups). SET: root only, at most NGROUPS ids; a new grp_id for every set (vfs.c) */
KCOLD static long do_groups(u32 op, u32 buf, u32 n)
{
    u16 *g = (u16 *)buf;
    if (op == GROUPS_GET) {
        if (!n)
            return current->ngroups;
        if (n < current->ngroups)
            return -EINVAL;
        if (current->ngroups && !user_ok_wr(current, buf, (u32)current->ngroups * 2))
            return -EFAULT;
        for (int i = 0; i < current->ngroups; i++) g[i] = current->groups[i];
        return current->ngroups;
    }
    if (op != GROUPS_SET)
        return -EINVAL;
    if (current->uid != 0)
        return -EPERM;
    if (n > NGROUPS)
        return -EINVAL;
    if (n && !user_ok_rd(current, buf, n * 2))
        return -EFAULT;
    for (u32 i = 0; i < n; i++) current->groups[i] = g[i];
    current->ngroups = (u8)n;
    current->grp_id = n ? vfs_new_grp_id() : 0;
    return 0;
}

/* getpriority / setpriority (DESIGN D10). PRIO_PROCESS: who is a pid, 0 the caller; PRIO_USER: every process of user
 * id who (0: the caller's real id); PRIO_PGRP: there are no process groups (-EINVAL). Changing another process needs
 * the kill rule (may_signal); raising a base priority (a lower nice) needs root: -EACCES. getpriority answers 20 - nice
 * (1 .. 40, the best of the matching processes), so a result never looks like an error number; libz turns it back. */
KCOLD static long do_priority(int set, u32 which, u32 who, s32 nice)
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

static __attribute__((noinline)) long do_spawn(u32 upath, u32 uargv, u32 ustdio)
{
    char path[FS_PATHMAX];
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
            if (!user_ok_rd(current, uargv + (u32)argc * 4, 4)) { r = -EFAULT; goto out; }
            ap = ((const u32 *)uargv)[argc];
            if (!ap)
                break;
            if (argc == SPAWN_ARGC || used >= SPAWN_ARGBYTES - 1) { r = -E2BIG; goto out; }
            argv[argc] = strs + used;
            int c = copy_str(strs + used, ap, SPAWN_ARGBYTES - used);
            if (c) {                    /* bad pointer, or the text does not fit */
                r = user_ok_rd(current, ap, 1) ? -E2BIG : -EFAULT;
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
        if (!user_ok_rd(current, ustdio, 12)) { r = -EFAULT; goto out; }
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
/* The system calls that take paths (docs/architecture.md section 7, "Paths"). Each path comes in as FS_PATHMAX bytes on
 * the kernel stack, in these functions and not in syscall_c: its frame is on the stack of every system call, read and
 * write too, and a server's reply and an interrupt come on top of it. */
static __attribute__((noinline)) s32 sys_path1(u32 nr, u32 a1, u32 a2, u32 a3)
{
    char path[FS_PATHMAX];
    int e = copy_str(path, a1, sizeof path);
    if (e)
        return e;
    switch (nr) {
    case SYS_open:   return (s32)vfs_open(path, (int)a2);
    case SYS_mkdir:  return (s32)vfs_mkdir(path);
    case SYS_unlink: return (s32)vfs_unlink(path);
    case SYS_rmdir:  return (s32)vfs_rmdir(path);
    case SYS_umount: return (s32)vfs_umount(path);
    case SYS_chdir:  return (s32)vfs_chdir(path);
    case SYS_chmod:  return (s32)vfs_path1(FS_CHMOD, path, a2, 0);
    case SYS_utimes: return (s32)vfs_path1(FS_UTIMES, path, a2, 0);
    case SYS_chown:  return (s32)vfs_path1(FS_CHOWN, path, a2, a3);
    case SYS_readlink:
        return user_ok_wr(current, a2, a3) ? (s32)vfs_path1(FS_READLINK, path, a2, a3) : -EFAULT;
    case SYS_stat: case SYS_lstat: {                /* the buffer checked after the path, as always */
        s32 n = usize(a2, a3, USTAT_FIRST << 16 | sizeof(struct ustat));
        if (n <= 0)
            return n;
        return (s32)(nr == SYS_stat ? vfs_stat(path, (struct ustat *)a2) : vfs_path1(FS_LSTAT, path, a2, 0));
    }
    }
    return -ENOSYS;
}

/* rename, link, symlink (a symlink's first argument is its target's text: at most 255 bytes, as a path) */
static __attribute__((noinline)) s32 sys_path2(u32 nr, u32 a1, u32 a2)
{
    char p1[FS_PATHMAX], p2[FS_PATHMAX];
    int e = copy_str(p1, a1, sizeof p1);
    if (!e)
        e = copy_str(p2, a2, sizeof p2);
    if (e)
        return e;
    return (s32)(nr == SYS_rename ? vfs_rename(p1, p2) : vfs_link(nr == SYS_link ? FS_LINK : FS_SYMLINK, p1, p2));
}

/* mount's request: the strings copied in; 0 or -errno */
static __attribute__((noinline)) s32 sys_mount_copy(u32 a1)
{
    struct umountreq rq;
    char path[FS_PATHMAX], port[16], dev[16], type[8];
    int e;
    rq = *(const struct umountreq *)a1;
    if ((e = copy_str(path, (u32)rq.path, sizeof path)) || (e = copy_str(port, (u32)rq.port, sizeof port)) ||
        (e = copy_str(dev, (u32)rq.dev, sizeof dev)) || (e = copy_str(type, (u32)rq.type, sizeof type)))
        return e;
    rq.path = path; rq.port = port; rq.dev = dev; rq.type = type;
    return (s32)vfs_mount(&rq);
}

void syscall_c(u32 *rb)
{
    u32 nr = rb[0], a1 = rb[1], a2 = rb[2], a3 = rb[3];
    s32 r = -ENOSYS;                    /* an unknown call number; every failing check below sets its own errno */
    struct file *f;
    /* the checks, each setting r when it fails (so a condition reads as "all of these hold") */
#define CSTR(dst, u)  ((r = copy_str(dst, u, sizeof dst)) == 0)
    /* user pointers (D13): UOKW where the kernel (or a server answering for it) writes into the caller's memory - its
     * own only; UOKR where it only reads - its shared code too (string literals live there) */
#define UOKW(p, n)    (user_ok_wr(current, p, n) || ((r = -EFAULT), 0))
#define UOKR(p, n)    (user_ok_rd(current, p, n) || ((r = -EFAULT), 0))
#define GETFD(f_, fd) (((f_) = getfd(fd)) || ((r = -EBADF), 0))
    /* a struct written into the caller's buffer p of size given (usize): n_ the bytes it gets */
#define UOKS(n_, p, given, first, now) (((n_) = usize(p, given, (first) << 16 | (now))) > 0 || ((r = (n_)), 0))
    s32 n;

    {   /* the stack guard (proc.c) at every system call: the quick check, the full one if it fails (the register block
         * is the kernel stack pointer) */
        u32 usp = get_usp();
        int bad = stack_user_suspect(current, usp) ? stack_bad(current, (u32)rb - 64, usp) : 0;
        if (bad) {
            stack_fail(current, bad);
            proc_exit(139);
        }
    }
    current->sig_intr = 0;              /* set only by a blocking site of this call that gave up for a handler (D16) */
    switch (nr) {
    case SYS_exit:                      /* a2 (D18): a child handed to the parent first (login stepping aside) */
        if (a2)
            proc_handover((int)a2);
        proc_exit((int)a1);
    case SYS_write:
        if (GETFD(f, a1) && UOKR(a2, a3))
            r = (s32)vfs_write(f, (const void *)a2, a3);
        break;
    case SYS_read:
        if (GETFD(f, a1) && UOKW(a2, a3))
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
    case SYS_sleep: {                   /* a stop and continue in between wakes it early: it sleeps on; a handler ends it
                                         * (-EINTR, never restarted: D16 rule 4) */
        u32 until = ticks + a1;
        r = 0;
        while ((s32)(ticks - until) < 0 && !(r = (s32)ev_block(0, 1, until, 0)))
            ;
        break;
    }
    case SYS_open: case SYS_mkdir: case SYS_unlink: case SYS_rmdir: case SYS_chdir: case SYS_chmod: case SYS_utimes:
    case SYS_stat: case SYS_lstat: case SYS_readlink:
        r = sys_path1(nr, a1, a2, a3);
        break;
    case SYS_rename: case SYS_link: case SYS_symlink:
        r = sys_path2(nr, a1, a2);
        break;
    case SYS_umount: case SYS_chown:                /* root only */
        r = current->uid != 0 ? -EACCES : sys_path1(nr, a1, a2, a3);
        break;
    case SYS_mount:                                 /* root only */
        r = current->uid != 0 ? -EACCES : !user_ok_rd(current, a1, sizeof(struct umountreq)) ? -EFAULT : sys_mount_copy(a1);
        break;
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
        if (UOKW(a2, sizeof(struct umsg)))     /* the server answers into it */
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
        if (UOKS(n, a1, a3, USTATFS_FIRST, 20))      /* the server writes struct ustatfs (frozen in fsproto.h) */
            r = (s32)vfs_statfs((void *)a1, a2);       /* a2 = the n-th mount, as SYS_mountinfo counts (0 = the root) */
        break;
    case SYS_mountinfo:
        if (UOKS(n, a2, a3, UMOUNTINFO_FIRST, sizeof(struct umountinfo)))
            r = (s32)vfs_mountinfo(a1, (struct umountinfo *)a2);
        break;
    case SYS_blk_flush:
        r = current->priv ? blk_flush() : -EPERM;
        break;
    case SYS_sync:
        r = (s32)vfs_sync(0);
        break;
    case SYS_blk_ram:
        r = -EPERM;
        if (current->priv && UOKW(a1, 8)) {
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
    case SYS_envset:                    /* a new block of the caller's (proc.c), then the old one goes */
        r = -E2BIG;
        if (a2 <= ENVMAX && UOKR(a1, a2)) {
            char *e = a2 ? mem_alloc(a2, 0, (u32)current->pid) : 0;
            r = -ENOMEM;
            if (e)
                memcpy(e, (const void *)a1, a2);
            if (e && e[a2 - 1]) {       /* a block without its last 0 is refused, checked on the kernel's own copy:
                                         * every child's strlen would run past it (audit B3-12) */
                mem_free(e);
                r = -EINVAL;
            } else if (e || !a2) {
                mem_free(current->env);
                current->env = e;
                current->envlen = (u16)a2;
                r = 0;
            }
        }
        break;
    case SYS_envget:
        if (UOKW(a1, a2)) {
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
    case SYS_time:                      /* unsigned seconds since 1970, the 32 bits as they are (clock.h, audit F8) */
        r = (s32)wclock_now(&wclock);
        break;
    case SYS_settime: {                 /* root only */
        if (current->uid != 0) { r = -EACCES; break; }
        if (a2 > 1) { r = -EINVAL; break; }     /* a2 (0 for every older caller): 1 = from a checked source (D21 8.3) */
        u16 s = irq_save();             /* the tick may not count a second between the read and the write */
        wclock_set(&wclock, a1);
        wclock.trust = a2 ? (u8)(wclock.trust | CLOCK_NET) : 0;    /* a hand-set time (date -s) is trusted by no one */
        irq_restore(s);
        r = 0;
        break;
    }
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
    case SYS_groups: r = do_groups(a1, a2, a3); break;
    case SYS_umask: r = current->umask; current->umask = (u16)(a1 & 0777); break;
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
        if (GETFD(f, a1) && UOKS(n, a2, a3, UDIRENT_FIRST, sizeof(struct udirent)))
            r = (s32)vfs_readdir(f, (struct udirent *)a2);
        break;
    case SYS_fstat:
        if (GETFD(f, a1) && UOKS(n, a2, a3, USTAT_FIRST, sizeof(struct ustat)))
            r = (s32)vfs_fstat(f, (struct ustat *)a2);
        break;
    case SYS_blk_read: case SYS_blk_write:
        /* raw disk: only the disk server (trusted; the buffer is a client's, so
         * it is not checked against this process's own memory) */
        r = !current->priv ? -EPERM : nr == SYS_blk_read ? blk_read(a1, a2, (void *)a3)
                                                              : blk_write(a1, a2, (const void *)a3);
        break;
    case SYS_input:
        if (UOKW(a1, 16)) {
            input_read((s32 *)a1);
            r = 0;
        }
        break;
    case SYS_klog:
        r = 0;
        if (a3 && UOKW(a2, a3))
            r = (s32)klog_read(a1, (char *)a2, a3);
        break;
    case SYS_getcwd: {
        u32 n = 0;
        while (current->cwd[n])
            n++;
        if ((n + 1 <= a2 || ((r = -ERANGE), 0)) && UOKW(a1, n + 1)) {
            memcpy((void *)a1, current->cwd, n + 1);
            r = (s32)n;
        }
        break;
    }
    case SYS_pipe:
        if (UOKW(a1, 8)) {
            int fds[2];
            if ((r = (s32)vfs_pipe(fds, a2)) == 0) {
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
    case SYS_ticks:                     /* a1 0: the 100 Hz tick count (wraps after 497 days); 1: whole seconds since boot
                                         * (clock.h's own count, wraps after 136 years; audit 2026-10-26 F9); 2: who set
                                         * the time this boot, the CLOCK_* bits (D21 8.3) */
        r = a1 == 0 ? (s32)ticks : a1 == 1 ? (s32)wclock.secs : a1 == 2 ? (s32)wclock.trust : -EINVAL;
        break;
    case SYS_procinfo:
        if ((a1 < NPROC || ((r = -EINVAL), 0)) && UOKS(n, a2, a3, UPROC_FIRST, sizeof(struct uproc))) {
            struct proc *p = &procs[a1];
            struct uproc uu, *u = &uu;
            u->pid = (u32)p->pid;
            u->ppid = (u32)p->ppid;
            u->state = p->state != P_EV ? (u32)p->state : (p->ev_wait & EV_CHILD) ? P_WAIT : P_IO;
            u->pgid = (u32)p->pgid;
            u->mem = p->span + (p->seg ? 0 : p->csize);     /* its own memory: a private copy of code counts, */
            for (int i = 0; i < NREG; i++)                  /* and the regions it asked for (malloc, sh's arena) */
                if (p->reg[i].base)
                    u->mem += p->reg[i].size;
            u->shared = p->seg ? p->csize : 0;
            memcpy(u->name, p->name, 16);
            u->nice = p->nice;
            u->pri = p->prio;
            memcpy((void *)a2, u, (u32)n);
            r = 0;
        }
        break;
    case SYS_meminfo:
        if (UOKS(n, a1, a2, UMEMINFO_FIRST, 16)) {      /* chip free, fast free, shared code in memory, what sharing saves */
            mem_stats(&((u32 *)a1)[0], &((u32 *)a1)[1]);
            seg_stats(&((u32 *)a1)[2], &((u32 *)a1)[3]);
            r = 0;
        }
        break;
    case SYS_setfg:                     /* on the caller's own tty; 0 = nobody, else a process the caller may signal, or
                                         * (a2 1, D16) a process group with a member the caller may signal */
        if (a1 == 0 || !(r = a2 ? (s32)sig_group((int)a1, 0, 1) : may_signal(proc_by_pid((int)a1)))) {
            tty_setfg(tty_of(current), (int)a1, a2 != 0);
            r = 0;
        }
        break;
    case SYS_jobctl: {                  /* D16 rule 9: SIGSTOP and SIGCONT (so a SIGCONT handler runs) */
        struct proc *p = proc_by_pid((int)a1);
        if (!(r = may_signal(p)))
            sig_send(p, a2 ? SIGCONT : SIGSTOP);
        break;
    }
    case SYS_evwait:
        r = (s32)ev_wait_items(a1, a2, (s32)a3);
        break;
    case SYS_sigaction:
        r = (s32)sig_action(a1, a2);
        break;
    case SYS_sigprocmask:
        r = (s32)sig_procmask(a1, a2, a3);
        break;
    case SYS_sigreturn:
        r = (s32)sig_return(rb, a1);
        break;
    case SYS_alarm:
        r = (s32)sig_alarm(a1);
        break;
    case SYS_setpgid:
        r = (s32)sig_setpgid((int)a1, (int)a2);
        break;
    case SYS_console:                   /* the 2 MB diet: /etc/console's colours, set by init (root); D20: a */
        if (a1 == CON_COLOURS)          /* console's font and rules, named by a descriptor in the op word */
            r = a2 && current->uid != 0 ? -EPERM : gfx_set_colours((int)a2);
        else if (a1 == CON_UFONT)       /* D28: the Unicode font, every console's: no descriptor (with one: */
            r = (s32)term_ufont(a2, a3);        /* term_conop's -EINVAL, as for any op it does not know) */
        else
            r = (s32)term_conop(a1, a2, a3);
        break;
    case SYS_sock:                      /* D19: every BSD socket call (sock.c) */
        r = (s32)sock_sys(a1, a2, a3);
        break;
    case SYS_netsrv:                    /* D19: the network server's own calls */
        r = (s32)netsrv_sys(a1, a2, a3);
        break;
    case SYS_drv:                       /* D19 step 7: the network's drivers (drv.c) */
        r = (s32)drv_sys(a1, a2, a3);
        break;
    case SYS_zine:                      /* D29: an Amiga program under ZINE (zine.c) */
        r = (s32)zine_sys(a1, a2, a3);
        break;
    case SYS_getpriority:
        r = (s32)do_priority(0, a1, a2, 0);
        break;
    case SYS_setpriority:
        r = (s32)do_priority(1, a1, a2, (s32)a3);
        break;
    case SYS_reboot:                    /* like the reset warning: sync, stop disk writes, then the reset */
        if (current->uid != 0)
            r = -EPERM;
        else if (a1 > BOOTNOTE_ZENIX && a1 != REBOOT_HALT)
            r = -EINVAL;
        else if (a1 && a1 != REBOOT_HALT && (r = (s32)bootnote_set(a1)) < 0)
            ;
        else {
            svc_hurry();                /* loaders in a back-off start at once (D11 6.3) */
            long e = vfs_sync(VS_REBOOT | VS_BOUNDED);   /* each server has 5 s (A7); a network one 2 s (D23) */
            disk_frozen = 1;
            klog_set_reason(KLOG_END_REBOOT);
            if (a1 == REBOOT_HALT) {    /* halt(8): an Amiga cannot switch itself off; it stops and says so */
                kprintf("halt: filesystems synced%s, disk writes stopped\n", e < 0 ? " (with an error)" : "");
                kprintf("\nThe system is halted. You can switch the Amiga off now (or press Ctrl-Amiga-Amiga to restart).\n");
                for (;;)
                    __asm__ volatile("stop #0x2700");
            }
            kprintf("reboot: filesystems synced%s, disk writes stopped; resetting%s\n", e < 0 ? " (with an error)" : "",
                    a1 == BOOTNOTE_AMIGAOS ? " into AmigaOS" : a1 == BOOTNOTE_ZENIX ? " into Zenix" : "");
            machine_reset();
        }
        break;
    case SYS_svc: {                     /* D11: op | nice << 8, name, program text */
        char name[16], text[SVC_TEXT];
        u32 op = a1 & 0xFF;
        if (op >= SVC_SAVE) {               /* SVC_SAVE: the kernel reads the buffer; SVC_LOAD, SVC_OPENS write it */
            if (op == SVC_SAVE ? UOKR(a2, a3) : UOKW(a2, a3))
                r = (s32)svc_data(op, (void *)a2, a3);
            break;
        }
        if (op != SVC_SELF && !CSTR(name, a2))
            break;
        if (op == SVC_REGISTER && !CSTR(text, a3))
            break;
        r = (s32)svc_sys(a1, op == SVC_SELF ? 0 : name, text);
        break;
    }
    case SYS_update:                    /* D15: INFO (anyone), RELEASE and NOTE (root) */
        if (a1 == UPD_INFO) {
            if (UOKS(n, a2, a3, UUPDATE_FIRST, sizeof(struct uupdate))) {
                struct uupdate u = { upd_pa, upd_size, upd_flags, zver.vnum, { 0 } };
                memcpy(u.kver, zver.vstr, 16);
                memcpy((void *)a2, &u, (u32)n);
                r = 0;
            }
        } else if (current->uid != 0)
            r = -EPERM;
        else if (a1 == UPD_RELEASE) {
            upd_release();
            r = 0;
        } else if (a1 == UPD_NOTE) {
#ifdef DIAG_TRIAL       /* test builds (tools/update-test.sh): a trial kernel that fails where it would confirm */
            if (a2 == UPDNOTE_CONFIRMED) {
                kprintf("diag: DIAG_TRIAL=%u: not confirming\n", DIAG_TRIAL);
                if (DIAG_TRIAL == 1)
                    panic("test panic (DIAG_TRIAL)");           /* the panic resets the machine in this build */
                if (DIAG_TRIAL == 2) {
                    serial_flush();
                    machine_reset();                            /* a hang the owner reset: the log says RUNNING */
                }
                r = 0;
                break;
            }
#endif
            r = a2 >= 1 && a2 <= UPDNOTE_ROOTFAILED ? (s32)updnote_set(a2, a3) : -EINVAL;
        }
        else
            r = -EINVAL;
        break;
    case SYS_kill:                      /* the old form (a3 0): end it, status 130, D11's rules. The new one (a3 1, D16
                                         * section 7): a2 the signal (0: does it exist?), a1 > 0 a pid, 0 the caller's
                                         * group, < -1 a group; rule 15's results */
        if (!a3) {
            struct proc *p = proc_by_pid((int)a1);
            if (!(r = may_signal(p)))
                proc_kill(p);
        } else if (a3 != 1 || a2 >= NSIG || (s32)a1 == -1)
            r = -EINVAL;
        else if ((s32)a1 <= 0)
            r = (s32)sig_group(a1 ? -(s32)a1 : current->pgid, (int)a2, 1);
        else {
            struct proc *p = proc_by_pid((int)a1);
            r = may_signal(p);
            if (r == -EACCES)
                r = -EPERM;
            if (!r && a2)
                sig_send(p, (int)a2);
        }
        break;
    }
    rb[0] = (u32)r;
    if (kwork_pending)
        kwork();                        /* queued input: done here, on the way back to user mode (main.c) */
    if (need_resched)
        resched();                      /* a better process became ready (a reply, a wake, input): it runs now */
    if (proc_kill_now())
        proc_exit(kill_status(current));
    if (SIG_WORK(current))
        sig_deliver(rb, nr);            /* D16: a stop, then a handler; the very last step before user mode */
}
