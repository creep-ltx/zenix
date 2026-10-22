#include "fsenv.h"
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"

/* The filesystem server: kernel/ext2.c in user mode behind the "fs" port.
 * Embedded in the kernel image (the kernel needs it before any disk is
 * readable); the kernel's VFS layer talks to it with messages. */

static int disk_port, fs_dev = -1;

static int disk_rw(u32 op, u32 lba, u32 count, const void *buf)
{
    struct umsg m;
    m.type = op; m.a[0] = (u32)fs_dev; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return sys_call(disk_port, &m);
}
int fs_blk_read(u32 lba, u32 count, void *buf) { return disk_rw(DISK_READ, lba, count, buf); }
int fs_blk_write(u32 lba, u32 count, const void *buf) { return disk_rw(DISK_WRITE, lba, count, buf); }

static int disk_open(const char *name, u32 flags)
{
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = flags;
    return sys_call(disk_port, &m);
}
static __attribute__((unused)) void disk_unmount(int h)
{
    struct umsg m;
    m.type = DISK_UNMOUNT; m.a[0] = (u32)h;
    sys_call(disk_port, &m);
}

/* the root filesystem: a root image the boot floppy carried (the dev/test image) wins; otherwise the
 * first IDE partition that holds ext2, else the bare disk.
 * A device is opened plain to probe it and only mounted (made ours alone) once ext2 is confirmed. */
static int try_mount(const char *name)
{
    fs_dev = disk_open(name, 0);
    if (fs_dev < 0 || !ext2_probe()) return -1;
    int h = disk_open(name, DISK_MOUNT);
    if (h < 0) return -1;
    fs_dev = h;
    return 0;
}

static int find_root(void)
{
    struct diskinfo di;
    if (!try_mount("ram0")) return 0;                 /* only exists when the floppy carried a root image */
    for (u32 i = 1; i < 16; i++) {                    /* 0 is the whole disk */
        struct umsg m;
        m.type = DISK_LIST; m.a[0] = i; m.a[1] = (u32)&di;
        if (sys_call(disk_port, &m) < 0) break;
        if (di.dostype == 0) continue;                /* ram0 */
        if (!try_mount(di.name)) return 0;
    }
    return try_mount("ide0");
}

static void fill_stat(const struct inode *in, struct ustat *st)
{
    st->ino = in->ino; st->mode = in->mode; st->size = in->size;
    st->mtime = in->mtime; st->uid = in->uid; st->gid = in->gid; st->links = in->links;
}

/* parent directory + last name of a path (kernel-copied, so trusted) */
static int parent(const char *path, u32 cwd, struct inode *dir, char *name)
{
    return ext2_namei_parent(path, cwd, dir, name);
}

static int handle(struct umsg *m)
{
    struct inode in, dir, dir2;
    char name[64], name2[64];
    switch (m->type) {
    case FS_OPEN: {
        const char *path = (const char *)m->a[0];
        u32 flags = m->a[2];
        int writing = (flags & (O_WRONLY | O_RDWR)) != 0;
        if (ext2_namei(path, m->a[1], &in, 0)) {
            if (!(flags & O_CREAT) || parent(path, m->a[1], &dir, name) ||
                ext2_create(&dir, name, &in))
                return -1;
        } else if (writing) {
            if (!S_ISREG(in.mode)) return -1;
            if ((flags & O_TRUNC) && ext2_truncate(&in)) return -1;
        }
        fill_stat(&in, (struct ustat *)m->a[3]);
        return (int)in.ino;
    }
    case FS_READ:
        if (ext2_iget(m->a[0], &in)) return -1;
        return ext2_read(&in, m->a[1], (void *)m->a[2], m->a[3]);
    case FS_WRITE:
        if (ext2_iget(m->a[0], &in)) return -1;
        return ext2_write(&in, m->a[1], (const void *)m->a[2], m->a[3]);
    case FS_STAT:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0)) return -1;
        fill_stat(&in, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if (ext2_iget(m->a[0], &in)) return -1;
        fill_stat(&in, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: {
        u32 cookie = m->a[1];
        if (ext2_iget(m->a[0], &in) || !S_ISDIR(in.mode)) return -1;
        int r = ext2_readdir(&in, &cookie, (struct udirent *)m->a[2]);
        *(u32 *)m->a[3] = cookie;
        return r;
    }
    case FS_CHDIR:
        if (ext2_namei((const char *)m->a[0], m->a[1], &in, 0) || !S_ISDIR(in.mode)) return -1;
        return (int)in.ino;
    case FS_MKDIR:
        if (parent((const char *)m->a[0], m->a[1], &dir, name)) return -1;
        return ext2_mkdir(&dir, name);
    case FS_UNLINK:
        if (parent((const char *)m->a[0], m->a[1], &dir, name)) return -1;
        return ext2_unlink(&dir, name);
    case FS_RMDIR:
        if (parent((const char *)m->a[0], m->a[1], &dir, name)) return -1;
        return ext2_rmdir(&dir, name);
    case FS_RENAME:
        if (parent((const char *)m->a[0], m->a[2], &dir, name) ||
            parent((const char *)m->a[1], m->a[2], &dir2, name2)) return -1;
        return ext2_rename(&dir, name, &dir2, name2);
    }
    return -1;
}

int main(void)
{
    int tries = 0;
    while ((disk_port = sys_port_find("disk")) < 0) {
        if (++tries > 600) { printf("fsd: no disk server\n"); return 1; }
        sys_sleep(5);
    }
    if (find_root() || ext2_mount()) { printf("fsd: no ext2 filesystem\n"); return 1; }
    int port = sys_port_create("fs");
    if (port < 0) { printf("fsd: cannot create the fs port\n"); return 1; }
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1)
        m = sys_replyrecv(m, handle(m), port);     /* reply and wait in one trap */
    return 1;
}
