#include "fsenv.h"
#include "../kernel/fsproto.h"

/* The filesystem server: kernel/ext2.c in user mode behind the "fs" port.
 * Embedded in the kernel image (the kernel needs it before any disk is
 * readable); the kernel's VFS layer talks to it with messages. */

int fs_blk_read(u32 lba, u32 count, void *buf) { return sys_blk_read(lba, count, buf); }
int fs_blk_write(u32 lba, u32 count, const void *buf) { return sys_blk_write(lba, count, buf); }

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
    if (ext2_mount()) { printf("fsd: no ext2 filesystem\n"); return 1; }
    int port = sys_port_create("fs");
    if (port < 0) { printf("fsd: cannot create the fs port\n"); return 1; }
    for (;;) {
        struct umsg *m = sys_recv(port, 1);
        if ((int)m == -1) return 1;
        sys_reply(m, handle(m));
    }
}
