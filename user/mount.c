/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsproto.h"
#include "rdb.h"
#include "volid.h"

/* mount                      list the mounted filesystems
 * mount [-t TYPE] DEVICE DIR  serve DEVICE with the filesystem server for TYPE (a service, fs.DEVICE) and put it on DIR
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

/* mount DEV on DIR; type 0 = from the partition. The server is a service (DESIGN D11 6.4): fs.DEV is registered with the
 * kernel's built-in server for the type and the volume id, at nice -10; the kernel's first call to it (the root inode,
 * in SYS_mount) starts it, and a call after it died starts it again. Returns 0, or 1 after printing why not. */
static int mount_one(const char *dev, const char *dir, const char *type)
{
    struct diskinfo di;
    struct umountinfo mi;
    char pname[16], id[64], prog[SVC_TEXT_MAX];
    vol_id(sys_port_find("disk"), dev, id);                /* its DISK_OPEN also probes a floppy drive past df0 */
    if (disk_info(dev, &di) < 0) { printf("mount: no such device: %s\n", dev); return 1; }
    if (di.mounted) { printf("mount: %s is already in use\n", dev); return 1; }
    if (!type) {
        type = di.dostype == DT_EXT2 ? "ext2" : di.dostype == DT_ZENIXFS ? "zenixfs" : 0;
        if (!type) { printf("mount: cannot tell the type of %s; use -t ext2 or -t zenixfs\n", dev); return 1; }
    }
    int z = !strcmp(type, "zenixfs");
    if (!z && strcmp(type, "ext2")) { printf("mount: unknown type %s (ext2 or zenixfs)\n", type); return 1; }
    strcpy(pname, "fs."); strncpy(pname + 3, dev, 12); pname[15] = 0;
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++)
        if (!strcmp(mi.port, pname)) { printf("mount: %s is already mounted on %s\n", dev, mi.path); return 1; }
    strcpy(prog, z ? "@zfsd " : "@fsd ");
    strcat(prog, dev); strcat(prog, " "); strcat(prog, pname); strcat(prog, " -id "); strcat(prog, id);
    int r = sys_svc_register(pname, prog, -10);
    if (r < 0) {
        printf(r == -EBUSY ? "mount: too many filesystems mounted\n" : "mount: cannot register the server of %s (%d)\n", dev, r);
        return 1;
    }
    struct umountreq rq = { dir, pname, dev, type };
    if ((r = sys_mount(&rq)) < 0) {
        if (r == -EIO) printf("mount: cannot mount %s: its filesystem server did not start (see dmesg)\n", dev);
        else printf("mount: cannot mount %s on %s: %s\n", dev, dir, zwhy("refused (is it an existing directory, not already a mount point? are you root?)"));
        sys_svc_remove(pname);                             /* a failed mount leaves no service behind */
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
