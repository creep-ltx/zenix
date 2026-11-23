/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsproto.h"
#include "rdb.h"
#include "volid.h"

/* mount                      list the mounted filesystems
 * mount [-t TYPE] [-o OPTIONS] DEVICE DIR  serve DEVICE with the filesystem server for TYPE (a service, fs.DEVICE) and put it on DIR
 * mount -t nfs [-o OPTIONS] HOST:/EXPORT DIR   an NFS export (DESIGN D23): the service fs.nfsN, served by /sbin/nfscd
 * mount -a                   mount everything listed in /etc/fstab (init does this at boot)
 * TYPE is ext2, zenixfs, fat, exfat, ffs, pfs3, iso9660 or nfs; without -t it is read from the partition's type in the RDB or
 * MBR (a HOST:/EXPORT is nfs; a device whose boot sector says EXFAT - DESIGN D47 - exfat; one with CD001 at its byte
 * 32769 - a CD, DESIGN D44 - iso9660).
 * /etc/fstab: one line per mount, "DEVICE DIR TYPE OPTIONS", TYPE = ext2, zenixfs, fat, exfat, ffs, pfs3, iso9660 or auto (from the partition type), OPTIONS =
 * "defaults" or a comma list; "noauto" leaves the line out of mount -a. Lines starting with # and blank lines are ignored.
 * An ext2 volume not marked clean (a power cut while it was written, DESIGN D17) is checked first by mount -a
 * (fsck.ext2 -y, as Linux's fsck pass at boot); a plain mount only warns. */

static int disk_info(const char *dev, struct diskinfo *di)
{
    int dp = sys_port_find("disk");
    struct umsg m;
    if (dp < 0) return -1;
    for (u32 i = 0; i < 16; i++) {
        m.type = DISK_LISTSZ; m.a[0] = i; m.a[1] = (u32)di; m.a[2] = sizeof *di;
        if (sys_call(dp, &m) < 0) return -1;
        if (!strcmp(di->name, dev)) return 0;
    }
    return -1;
}

static void list(void)
{
    struct umountinfo mi;
    struct ustatfs sf;
    dprintf(1, "%-8s %-14s %-8s %9s %9s\n", "DEVICE", "MOUNTED ON", "TYPE", "SIZE-KB", "FREE-KB");
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++) {
        unsigned size = 0, fr = 0;
        if (sys_statfs_mount(&sf, i) == 0) { size = sf.blocks * (sf.bsize / 1024); fr = sf.bfree * (sf.bsize / 1024); }
        dprintf(1, "%-8s %-14s %-8s %9u %9u\n", mi.dev[0] ? mi.dev : "?", mi.path, mi.type[0] ? mi.type : "?", size, fr);
    }
}

/* mount DEV on DIR; type 0 = from the partition. The server is a service (DESIGN D11 6.4): fs.DEV is registered with the
 * server for the type (ext2: the kernel's built-in fsd; ZenixFS: the file /sbin/zfsd, DESIGN D19) and the volume id, at
 * nice -10; the kernel's first call to it (the root inode, in SYS_mount) starts it, and a call after it died starts it
 * again. Returns 0, or 1 after printing why not. */
/* an MBR partition of a FAT type (diskd's 'MBR' + type byte): 01 04 06 0B 0C 0E, 11 14 16 1B 1C 1E (hidden), EF (D30) */
static int fat_part(u32 dt)
{
    return (dt & 0xFFFFFF00u) == 0x4D425200u && ((dt & 0xFF) == 0xEF || ((dt & 0xE0) == 0 && (0x58525852u >> (dt & 0x1F) & 1)));
}
/* an RDB partition of AmigaDOS's own types, DOS\\0 .. DOS\\7 (D31) */
static int ffs_part(u32 dt) { return (dt & 0xFFFFFFF8u) == 0x444F5300u; }
/* an RDB partition of PFS3's types: PFS\\1 .. PFS\\3, PDS\\1 .. PDS\\3 (D32; the owner's card has PFS\\3) */
static int pfs3_part(u32 dt)
{
    return ((dt & 0xFFFFFF00u) == 0x50465300u || (dt & 0xFFFFFF00u) == 0x50445300u) && (dt & 0xFF) >= 1 && (dt & 0xFF) <= 3;
}
/* does DEV's sector LBA have the N bytes MAGIC at byte AT? An ISO 9660 volume (D44): CD001 in the descriptor at its
 * block 16 (sector 64, byte 1); exFAT (D47): "EXFAT   " in its boot sector (sector 0, byte 3; on a partition of MBR
 * type 07, which NTFS shares, or on a whole stick) */
static int dev_has(const char *dev, u32 lba, u32 at, const char *magic, u32 n)
{
    static unsigned char sec[512];
    struct umsg m;
    int dp = sys_port_find("disk");
    m.type = DISK_OPEN; m.a[0] = (u32)dev; m.a[1] = 0;
    int h = sys_call(dp, &m);
    m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = 1; m.a[3] = (u32)sec;
    return h >= 0 && sys_call(dp, &m) == 0 && !memcmp(sec + at, magic, n);
}
/* is the word W among the comma-separated OPTS? */
static int opt_has(const char *opts, const char *w)
{
    unsigned l = strlen(w);
    for (const char *p = opts; p && *p; ) {
        if (!strncmp(p, w, l) && (p[l] == ',' || !p[l])) return 1;
        while (*p && *p != ',') p++;
        if (*p) p++;
    }
    return 0;
}
static int mount_one(const char *dev, const char *dir, const char *type, const char *opts, int fsck)
{
    struct diskinfo di;
    struct umountinfo mi;
    char pname[16], id[64], prog[SVC_TEXT_MAX];
    int vt = vol_id(sys_port_find("disk"), dev, id);       /* its DISK_OPEN also probes a floppy drive past df0 */
    if (disk_info(dev, &di) < 0) { dprintf(1, "mount: no such device: %s\n", dev); return 1; }
    if (di.mounted) { dprintf(1, "mount: %s is already in use\n", dev); return 1; }
    if (!type) {
        type = di.dostype == DT_EXT2 ? "ext2" : di.dostype == DT_ZENIXFS ? "zenixfs" : fat_part(di.dostype) ? "fat" : ffs_part(di.dostype) ? "ffs" : pfs3_part(di.dostype) ? "pfs3" :
               dev_has(dev, 0, 3, "EXFAT   ", 8) ? "exfat" : dev_has(dev, 64, 1, "CD001", 5) ? "iso9660" : 0;
        if (!type) {
            if ((di.caps & DISKCAP_RO) && !di.sectors) dprintf(1, "mount: no disc in %s\n", dev);
            else dprintf(1, "mount: cannot tell the type of %s; use -t ext2, -t zenixfs, -t fat, -t ffs, -t pfs3, -t iso9660 or -t exfat\n", dev);
            return 1;
        }
    }
    int z = !strcmp(type, "zenixfs"), a = !strcmp(type, "ffs"), p3 = !strcmp(type, "pfs3"), iso = !strcmp(type, "iso9660");
    int ex = !strcmp(type, "exfat");
    int f = a || p3 || iso || ex || !strcmp(type, "fat");           /* f: a server with options */
    if (opts && !f && !fsck) { dprintf(1, "mount: -o is for FAT, exFAT, FFS, PFS3, ISO 9660 and NFS mounts only\n"); return 1; }
    if (!z && !f && strcmp(type, "ext2")) { dprintf(1, "mount: unknown type %s (ext2, zenixfs, fat, exfat, ffs, pfs3, iso9660 or nfs)\n", type); return 1; }
    if ((p3 || iso || ex) && opt_has(opts, "rw")) { dprintf(1, "mount: %s volumes are read-only on Zenix: -o rw is refused\n", p3 ? "PFS3" : iso ? "ISO 9660" : "exFAT"); return 1; }
    if ((di.caps & DISKCAP_RO) && !iso && !ex && strcmp(type, "fat")) { dprintf(1, "mount: %s is read-only: only -t iso9660, -t exfat or -t fat for it\n", dev); return 1; }
    if ((di.caps & DISKCAP_RO) && !strcmp(type, "fat") && opt_has(opts, "rw")) { dprintf(1, "mount: %s is a read-only device: -o rw is refused (FAT is mounted read-only there)\n", dev); return 1; }
    strcpy(pname, "fs."); strncpy(pname + 3, dev, 12); pname[15] = 0;
    for (u32 i = 0; sys_mountinfo(i, &mi) == 0; i++)
        if (!strcmp(mi.port, pname)) { dprintf(1, "mount: %s is already mounted on %s\n", dev, mi.path); return 1; }
    if (!z && !f && vt == 1 && !(vol_state & 1)) {               /* not marked clean: a cut while it was written */
        if (!fsck) dprintf(1, "mount: %s was not unmounted cleanly: check it (umount it, then fsck.ext2 -y %s)\n", dev, dev);
        else {
            dprintf(1, "mount: %s was not unmounted cleanly: fsck.ext2 -y %s\n", dev, dev);
            char *fa[] = { "/bin/fsck.ext2", "-y", (char *)dev, 0 };
            int pid = sys_spawn(fa[0], fa, 0), st = pid < 0 ? pid : sys_wait(pid);
            if (pid < 0 || st > 1)
                dprintf(1, "mount: %s could not be repaired (fsck.ext2: %d): mounted as it is, still marked \"needs a check\"\n", dev, st);
        }
    }
    /* ext2: the built-in fsd. ZenixFS: the file /sbin/zfsd (DESIGN D19 step 1; D11 6.4's file service), without the
     * port word (zfsd names it fs.DEVICE itself): the path and the volume id leave no room for it in SVC_TEXT */
    /* FAT (D30): the file /sbin/fatd, the id's place only (a volume serial is not read here), the options as they are:
     * fatd checks them */
    /* FFS (D31), PFS3 (D32), ISO 9660 (D44), exFAT (D47): /sbin/ffsd, /sbin/pfs3d, /sbin/iso9660d, /sbin/exfatd, the same way */
    strcpy(prog, z ? "/sbin/zfsd " : a ? "/sbin/ffsd " : p3 ? "/sbin/pfs3d " : iso ? "/sbin/iso9660d " : ex ? "/sbin/exfatd " : f ? "/sbin/fatd " : "@fsd ");
    strcat(prog, dev);
    if (!z && !f) { strcat(prog, " "); strcat(prog, pname); }
    if (f) id[17] = 0;
    strcat(prog, " -id "); strcat(prog, id);
    if (f && opts) {
        if (strlen(prog) + strlen(opts) + 5 > SVC_TEXT_MAX) { dprintf(1, "mount: too many options\n"); return 1; }
        strcat(prog, " -o "); strcat(prog, opts);
    }
    int r = sys_svc_register(pname, prog, -10);
    if (r < 0) {
        dprintf(1, r == -EBUSY ? "mount: too many filesystems mounted\n" : "mount: cannot register the server of %s (%d)\n", dev, r);
        return 1;
    }
    struct umountreq rq = { dir, pname, dev, type };
    if ((r = sys_mount(&rq)) < 0) {
        if (r == -EIO) dprintf(1, "mount: cannot mount %s: its filesystem server did not start (see dmesg)\n", dev);
        else dprintf(1, "mount: cannot mount %s on %s: %s\n", dev, dir, zwhy("refused (is it an existing directory, not already a mount point? are you root?)"));
        sys_svc_remove(pname);                             /* a failed mount leaves no service behind */
        return 1;
    }
    return 0;
}

/* ---- NFS (DESIGN D23 4.1) ---- */
/* the options as nfscd's codes: h/s hard or soft, tN timeo seconds, rN retrans, o read-only, y sync, aN actimeo, l
 * lazy; the defaults (hard, timeo=30, retrans=3, rw) are left out. 0, or 1 after printing why not */
static int nfs_codes(const char *opts, int lazy, char *codes)
{
    char buf[96], *p = buf;
    int n = 0;
    codes[0] = 0;
    strncpy(buf, opts ? opts : "", sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    while (*p) {
        char *e = p;
        while (*e && *e != ',') e++;
        char save = *e; *e = 0;
        const char *v = strchr(p, '=');
        u32 num = v ? (u32)atoi(v + 1) : 0;
        int okv = v && v[1] >= '0' && v[1] <= '9';
        char c[12]; c[0] = 0;
        if (!strcmp(p, "soft")) strcpy(c, "s");
        else if (!strcmp(p, "hard") || !strcmp(p, "rw") || !strcmp(p, "defaults") || !strcmp(p, "noauto") ||
                 !strcmp(p, "async") || !*p) ;
        else if (!strcmp(p, "ro")) strcpy(c, "o");
        else if (!strcmp(p, "sync")) strcpy(c, "y");
        else if (!strncmp(p, "timeo=", 6) && okv && num >= 1 && num <= 600) { if (num != 30) sprintf(c, "t%u", num); }
        else if (!strncmp(p, "retrans=", 8) && okv && num >= 1 && num <= 100) { if (num != 3) sprintf(c, "r%u", num); }
        else if (!strncmp(p, "actimeo=", 8) && okv && num <= 3600) sprintf(c, "a%u", num);
        else { dprintf(1, "mount: unknown NFS option %s (hard, soft, timeo=S, retrans=N, ro, rw, sync, actimeo=S)\n", p); return 1; }
        if (n + strlen(c) >= 32) { dprintf(1, "mount: too many NFS options\n"); return 1; }
        strcpy(codes + n, c); n += strlen(c);
        *e = save;
        p = *e ? e + 1 : e;
    }
    if (lazy) { codes[n++] = 'l'; codes[n] = 0; }
    return 0;
}

/* a network mount never goes where netd and nfscd read their own files (4.2: no cycle netd -> nfscd -> netd) */
static int nfs_refused(const char *dir)
{
    static const char *const no[] = { "/bin", "/sbin", "/etc" };
    if (!strcmp(dir, "/")) return 1;
    for (u32 i = 0; i < 3; i++) {
        u32 l = strlen(no[i]);
        if (!strncmp(dir, no[i], l) && (dir[l] == 0 || dir[l] == '/')) return 1;
    }
    return 0;
}

static void nfs_why(const char *src, int r)
{
    const char *w = r == -ETIMEDOUT ? "the NAS did not answer" :
                    r == -EACCES || r == -EPERM ? "the NAS refused the mount (its export rules)" :
                    r == -ENOENT ? "the NAS has no such export" :
                    r == -ECONNREFUSED ? "the NAS refused the connection (is NFS on?)" :
                    r == -EHOSTUNREACH || r == -ENETUNREACH ? "the NAS cannot be reached" :
                    r == -ENETDOWN ? "the network is down" :
                    r == -EIO ? "its server did not start" : 0;
    if (w) dprintf(1, "mount: %s: %s (see dmesg)\n", src, w);
    else dprintf(1, "mount: %s: %s (see dmesg)\n", src, zstrerror(r) ? zstrerror(r) : "refused");
}

/* mount -t nfs HOST:/EXPORT DIR: fs.nfsN -> "/sbin/nfscd nfsN HOST:/EXPORT [CODES]" at nice -10 (D11 6.4). A hand mount
 * reaches the NAS before it answers (a typo or a firewall is said at once); mount -a's are lazy (l): the boot never
 * waits for a NAS, a cable or a lease */
static int mount_nfs(const char *src, const char *dir, const char *opts, int lazy)
{
    char codes[40], prog[SVC_TEXT_MAX], pname[16], dev[8], real[ZPATH_MAX];
    const char *c = strchr(src, ':');
    if (!c || c == src || c[1] != '/') { dprintf(1, "mount: %s: not HOST:/EXPORT\n", src); return 1; }
    if (nfs_codes(opts, lazy, codes)) return 1;
    if (zrealpath(dir, real, sizeof real) < 0) strncpy(real, dir, sizeof real - 1), real[sizeof real - 1] = 0;
    if (nfs_refused(real)) {
        dprintf(1, "mount: %s: an NFS mount may not be on /, /bin, /sbin, /etc or below them\n", dir);
        return 1;
    }
    int k = 0;
    for (; k < 8; k++) {
        strcpy(dev, "nfs"); dev[3] = (char)('0' + k); dev[4] = 0;
        strcpy(pname, "fs."); strcat(pname, dev);
        if (sys_port_find(pname) < 0) break;
    }
    if (k == 8) { dprintf(1, "mount: too many NFS mounts (nfs0 .. nfs7 are in use)\n"); return 1; }
    u32 room = SVC_TEXT_MAX - 1 - strlen("/sbin/nfscd nfs0 ") - (codes[0] ? strlen(codes) + 1 : 0);
    if (strlen(src) > room) {
        dprintf(1, "mount: %s: too long for the service table: at most %u characters\n", src, room);
        return 1;
    }
    strcpy(prog, "/sbin/nfscd "); strcat(prog, dev); strcat(prog, " "); strcat(prog, src);
    if (codes[0]) { strcat(prog, " "); strcat(prog, codes); }
    int r = sys_svc_register(pname, prog, -10);
    if (r < 0) {
        dprintf(1, r == -EBUSY ? "mount: too many filesystems mounted\n" : "mount: cannot register the server of %s (%d)\n", src, r);
        return 1;
    }
    struct umountreq rq = { dir, pname, dev, "nfs" };
    if ((r = sys_mount(&rq)) < 0) {
        if (r == -ENOENT || r == -ENOTDIR || r == -EBUSY || r == -EACCES) {
            /* the kernel's own refusals come before the server is asked; the server's come from its FS_ROOT */
            struct ustat st;
            if (sys_stat(dir, &st) < 0 || (st.mode & 0170000) != 0040000 || r == -EBUSY)
                dprintf(1, "mount: cannot mount %s on %s: %s\n", src, dir, zwhy("refused"));
            else nfs_why(src, r);
        } else nfs_why(src, r);
        sys_svc_remove(pname);
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
                if (type && !strcmp(type, "nfs") ? mount_nfs(f[0], f[1], f[3], 1) : mount_one(f[0], f[1], type, f[3], 1)) bad = 1;
            } else if (nf < 2) { dprintf(1, "mount: bad line in /etc/fstab: %s\n", line); bad = 1; }
        }
        *eol = save;
        line = *eol ? eol + 1 : eol;
    }
    return bad;
}

static const char USAGE[] = "usage: mount [-t ext2|zenixfs|fat|exfat|ffs|pfs3|iso9660] [-o OPTIONS] DEVICE DIR     mount -t nfs [-o OPTIONS] HOST:/EXPORT DIR\n"
                            "       mount -a     mount every line of /etc/fstab;   mount   list the mounts\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *type = 0, *dev = 0, *dir = 0, *opts = 0;
    int all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = 1;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) opts = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) zbadopt(argv[i], USAGE);
        else if (!dev) dev = argv[i];
        else if (!dir) dir = argv[i];
        else dev = 0;
    }
    if (all) return mount_all();
    if (argc == 1) { list(); return 0; }
    if (!dev || !dir) zusage(USAGE);
    if ((type && !strcmp(type, "nfs")) || (!type && strstr(dev, ":/"))) return mount_nfs(dev, dir, opts, 0);
    return mount_one(dev, dir, type, opts, 0);
}
