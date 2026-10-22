/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsproto.h"
#include "rdb.h"

/* mount                      list the mounted filesystems
 * mount [-t TYPE] DEVICE DIR  start the filesystem server /bin/mount.TYPE on DEVICE and put it on DIR
 * TYPE is ext2 or zenixfs; without -t it is read from the partition's type in the RDB. */

static int disk_info(const char *dev, struct diskinfo *di)
{
    int dp = sys_port_find("disk");
    struct umsg m;
    if (dp < 0) return -1;
    for (u32 i = 0; i < 16; i++) {
        m.type = DISK_LIST; m.a[0] = i; m.a[1] = (u32)di;
        if (sys_call(dp, &m) < 0) return -1;
        if (!strcmp(di->name, dev)) return 0;
    }
    return -1;
}

static void list(void)
{
    struct umountinfo mi;
    struct ustatfs sf;
    printf("%-8s %-14s %-8s %9s %9s\n", "DEVICE", "MOUNTED ON", "TYPE", "SIZE-KB", "FREE-KB");
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        unsigned size = 0, fr = 0;
        if (sys_statfs_mount(&sf, i) == 0) { size = sf.blocks * (sf.bsize / 1024); fr = sf.bfree * (sf.bsize / 1024); }
        printf("%-8s %-14s %-8s %9u %9u\n", mi.dev[0] ? mi.dev : "?", mi.path, mi.type[0] ? mi.type : "?", size, fr);
    }
}

int main(int argc, char **argv)
{
    const char *type = 0, *dev = 0, *dir = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!dev) dev = argv[i];
        else if (!dir) dir = argv[i];
        else dev = 0;
    }
    if (argc == 1) { list(); return 0; }
    if (!dev || !dir) { printf("usage: mount [-t ext2|zenixfs] DEVICE DIR     mount (list)\n"); return 2; }
    struct diskinfo di;
    if (disk_info(dev, &di) < 0) { printf("mount: no such device: %s\n", dev); return 1; }
    if (di.mounted) { printf("mount: %s is already in use\n", dev); return 1; }
    if (!type) {
        type = di.dostype == DT_EXT2 ? "ext2" : di.dostype == DT_ZENIXFS ? "zenixfs" : 0;
        if (!type) { printf("mount: cannot tell the type of %s; use -t ext2 or -t zenixfs\n", dev); return 1; }
    }
    char prog[40], pname[16];
    if (strlen(type) > 12) { printf("mount: bad type\n"); return 1; }
    strcpy(prog, "/bin/mount."); strcat(prog, type);
    strcpy(pname, "fs."); strncpy(pname + 3, dev, 12); pname[15] = 0;
    char *args[4] = { prog, (char *)dev, pname, 0 };
    int pid = sys_spawn(prog, args, 0);
    if (pid < 0) { printf("mount: cannot start %s\n", prog); return 1; }
    int port = -1;
    for (int t = 0; t < 1500; t++) {                       /* up to 15 s: a big filesystem takes a while to open */
        if ((port = sys_port_find(pname)) >= 0) break;
        if (sys_waitnb(pid) != -2) { printf("mount: %s failed\n", prog); return 1; }
        sys_sleep(1);
    }
    if (port < 0) { printf("mount: %s did not start\n", prog); sys_kill(pid); return 1; }
    struct umountreq rq = { dir, pname, dev, type };
    if (sys_mount(&rq) < 0) {
        printf("mount: cannot mount %s on %s (is it an existing directory, not already a mount point?)\n", dev, dir);
        struct umsg m; m.type = FS_QUIT;
        sys_call(port, &m);
        return 1;
    }
    return 0;
}
