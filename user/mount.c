/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsproto.h"
#include "rdb.h"

/* mount                      list the mounted filesystems
 * mount [-t TYPE] DEVICE DIR  start the filesystem server /bin/mount.TYPE on DEVICE and put it on DIR
 * mount -a                   mount everything listed in /etc/fstab (init does this at boot)
 * TYPE is ext2 or zenixfs; without -t it is read from the partition's type in the RDB.
 * /etc/fstab: one line per mount, "DEVICE DIR TYPE OPTIONS", TYPE = ext2, zenixfs or auto (from the partition type), OPTIONS =
 * "defaults" or a comma list; "noauto" leaves the line out of mount -a. Lines starting with # and blank lines are ignored. */

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

/* mount DEV on DIR; type 0 = from the partition. Returns 0, or 1 after printing why not. */
static int mount_one(const char *dev, const char *dir, const char *type)
{
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
        printf("mount: cannot mount %s on %s: %s\n", dev, dir, zwhy("refused (is it an existing directory, not already a mount point? are you root?)"));
        struct umsg m; m.type = FS_QUIT;
        sys_call(port, &m);
        return 1;
    }
    return 0;
}

/* mount -a: the lines of /etc/fstab */
static int mount_all(void)
{
    static char buf[2048];
    int fd = sys_open("/etc/fstab", O_RDONLY);
    if (fd < 0) return 0;                                  /* no fstab: nothing to do */
    int n = sys_read(fd, buf, sizeof buf - 1);
    sys_close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    int bad = 0;
    for (char *line = buf; *line; ) {
        char *eol = line;
        while (*eol && *eol != '\n') eol++;
        char save = *eol; *eol = 0;
        char *f[4] = { 0, 0, 0, 0 }; int nf = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p && *p != '#') {
            while (*p && nf < 4) {
                f[nf++] = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                if (*p) *p++ = 0;
                while (*p == ' ' || *p == '\t') p++;
            }
            if (nf >= 2 && !(f[3] && strstr(f[3], "noauto"))) {
                const char *type = (nf >= 3 && strcmp(f[2], "auto")) ? f[2] : 0;
                if (mount_one(f[0], f[1], type)) bad = 1;
            } else if (nf < 2) { printf("mount: bad line in /etc/fstab: %s\n", line); bad = 1; }
        }
        *eol = save;
        line = *eol ? eol + 1 : eol;
    }
    return bad;
}

int main(int argc, char **argv)
{
    const char *type = 0, *dev = 0, *dir = 0;
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!dev) dev = argv[i];
        else if (!dir) dir = argv[i];
        else dev = 0;
    }
    if (all) return mount_all();
    if (argc == 1) { list(); return 0; }
    if (!dev || !dir) { printf("usage: mount [-t ext2|zenixfs] DEVICE DIR     mount -a     mount (list)\n"); return 2; }
    return mount_one(dev, dir, type);
}
