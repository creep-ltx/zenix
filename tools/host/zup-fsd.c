/* SPDX-License-Identifier: GPL-2.0-only */
/* The ext2 server (user/fsd.c + kernel/ext2.c) on an image file, as a library for zup's ext2 host build
 * (tools/host/zup-ext2.c, tools/zup-host-test.sh section 3). Built with -Dsys_call=hd_sys_call
 * -Dsys_port_find=hd_port_find (and hostdisk.c the same), so that the disk server's calls do not meet zup's own
 * sys_call and sys_port_find. The image comes from HOSTDISK; ZUP_DISKCUT=N is a power cut: only N more sectors reach
 * the image, and the program ends (exit 99) at the write that would pass them. */
#define main fsd_main
#include "../../user/fsd.c"
#undef main
int sys_svc_self(void) { return 1; }
int sys_svc_opens(u32 *ino, u32 bytes) { (void)ino; (void)bytes; return 0; }

extern unsigned long hostdisk_writes, hostdisk_limit;
extern int hostdisk_die;

int zfs_mount(void)
{
    static int done;
    if (done) return 0;
    done = 1;
    disk_port = 1;
    ext2_busy = open_busy;
    const char *c = getenv("ZUP_DISKCUT");
    if (c && *c) { hostdisk_limit = (unsigned long)atol(c); hostdisk_die = 1; }
    if (try_mount("hd0", EXT2_RW)) { fprintf(stderr, "zup-fsd: the image is not a writable ext2 volume\n"); exit(2); }
    return 0;
}
/* after another process (a spawned zup) changed the image: forget every cached block, mount again */
void zfs_remount(void)
{
    if (try_mount("hd0", EXT2_RW)) { fprintf(stderr, "zup-fsd: the image cannot be mounted again\n"); exit(2); }
}
unsigned long zfs_writes(void) { return hostdisk_writes; }

/* one request from root, as the kernel sends it. flags of FS_OPEN: bit 0/1 the access mode, 4 create, 8 exclusive,
 * 16 truncate (the values of this build's O_* are fsd's own) */
int zfs_req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    zfs_mount();
    struct umsg m;
    memset(&m, 0, sizeof m);
    if (type == FS_OPEN) {
        u32 f = a2, o = f & 3;
        if (f & 4) o |= O_CREAT;
        if (f & 8) o |= O_EXCL;
        if (f & 16) o |= O_TRUNC;
        a2 = o;
    }
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    m.uid = 0; m.gid = 0; m.umask = 022; m.from = 7; m.kflags = UMSG_KERNEL;
    return handle(&m);
}
