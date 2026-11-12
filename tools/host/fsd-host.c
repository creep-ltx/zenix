/* SPDX-License-Identifier: GPL-2.0-only */
/* The ext2 filesystem server (user/fsd.c + kernel/ext2.c) on the host, driven by commands on stdin, one per line:
 *   mount [root]            try_mount of the image (root: it must be writable, as the root picker demands)
 *   open PATH FLAGS         FS_OPEN (flags: r w rw c t a, combined: "wct"); prints the inode number or the error
 *   close INO               FS_CLOSE
 *   write INO OFF TEXT      FS_WRITE ("TEXT" may be @N: N bytes of a pattern)
 *   read INO OFF N          FS_READ, prints what came back
 *   fstat INO | stat PATH | lstat PATH
 *   chdir P | unlink P | rmdir P | mkdir P | symlink TARGET P | link OLD NEW | rename OLD NEW | readlink P | chmod P MODE
 *   ls PATH                 every entry (FS_READDIR)
 *   sync | quit             FS_SYNC | FS_QUIT
 *   writes                  the sectors written to the image so far (hostdisk_writes)
 *   lsat DIR COOKIE         one FS_READDIR of DIR at COOKIE (a place a program's lseek could give)
 *   reads                   the read requests and the sectors read so far (FAT-family servers: DESIGN D47's speed check)
 *   cut N                   a power cut: only N more sectors reach the image, every later write fails
 *   as UID GID              later requests come from this user
 *   raw on|off              later requests come as a user's own SYS_call (no UMSG_KERNEL), not from the kernel
 *   links on|off            later requests carry UMSG_LINKS (DESIGN D26): an FS_LINKED answer prints
 *                           "CMD -> -102N a0|a1 TEXT" with the rewritten argument; the paths are copied into
 *                           FS_PATHMAX buffers of their own first (as the kernel's), always
 *   utimes P T | chown P UID GID     FS_UTIMES (T "now" = FS_UTIME_NOW) | FS_CHOWN
 *   state                   the clean mark (DESIGN D17): the state's bit 0 on the image, and fsd's marked, need, hold,
 *                           orphans and the block writes it made outside the mark
 *   idle                    what fsd does 1 s after the last change (the quiet moment)
 * At the end a line "unmarked-writes N" if fsd wrote a block while the volume was not marked (never right).
 * open FLAGS also take x (O_EXCL).
 * An inode argument "%N" is the inode number the N-th successful open returned.
 * Every command prints one line "CMD -> RESULT ...". Used by tools/ext2-host-test.sh.
 * Built with -DZFSD it drives the ZenixFS server (user/zenixfsd.c, zenixfs/zenixfs.c) on a ZenixFS image instead: the
 * same commands but cut, unclean, mark, idle and state (ext2's clean mark), "mount" ignores root (tools/zfsd-host-test.sh).
 * With -DFATD as well the FAT server (user/fatd.c, tools/fat-host-test.py): "mount [OPTIONS]", and commit (the timer's
 * commit), umount (FS_QUIT and what fatd does at it), statfs; a + in a path is a space.
 * With -DFFSD instead the OFS/FFS server (user/ffsd.c, tools/ffs-host-test.py): the same commands as FAT's.
 * With -DPFS3D the PFS3 server (user/pfs3d.c, tools/pfs3-host-test.py): the same, and it can only read.
 * With -DISO9660D the ISO 9660 server (user/iso9660d.c, tools/iso9660-host-test.py): as PFS3's (DESIGN D44).
 * With -DEXFATD the exFAT server (user/exfatd.c, tools/exfat-host-test.py): as PFS3's (DESIGN D47). */
#ifdef ZFSD                                               /* -DZFSD: the ZenixFS server (user/zenixfsd.c) instead */
#include "zhost.h"
struct udirent { u32 ino; u32 type; char name[64]; };    /* usys.h's (zhost.h has none; ext2.h is not included) */
struct ustat { u32 ino, mode, size, mtime; u32 uid, gid, links; };
static inline long sys3(u32 nr, u32 a, u32 b, u32 c) { (void)nr; (void)a; (void)b; (void)c; return 0; }   /* main only */
#define main fsd_main
#if defined(FATD) || defined(FFSD) || defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
#undef main
#define S_IFDIR 0040000
#define S_IFREG 0100000
#ifdef FATD                                               /* -DZFSD -DFATD: the FAT server (user/fatd.c) */
#include "../../user/fatd.c"
#define ROOT_INO ROOT
#elif defined(PFS3D)                                      /* -DZFSD -DPFS3D: the PFS3 server (user/pfs3d.c) */
#include "../../user/pfs3d.c"
#define ROOT_INO ROOT_ANODE
#elif defined(ISO9660D)                                   /* -DZFSD -DISO9660D: the ISO 9660 server (user/iso9660d.c) */
#include "../../user/iso9660d.c"
#define ROOT_INO (root_ext << 10)
#elif defined(EXFATD)                                     /* -DZFSD -DEXFATD: the exFAT server (user/exfatd.c, D47) */
#include "../../user/exfatd.c"
#define ROOT_INO ROOT
#else                                                     /* -DZFSD -DFFSD: the OFS/FFS server (user/ffsd.c) */
#include "../../user/ffsd.c"
#define ROOT_INO root_blk
#endif
#else
#include "../../user/zenixfsd.c"
#define ROOT_INO ROOT
#endif
#undef main
int sys_svc_self(void) { return 1; }
#else
#define main fsd_main
#include "../../user/fsd.c"
#undef main
int sys_svc_self(void) { return 1; }
#endif                      /* a first start: no restart rules */
int sys_svc_opens(u32 *ino, u32 bytes) { (void)ino; (void)bytes; return 0; }

#ifdef ZFSD
#define CWD ROOT_INO                                          /* the working directory of relative paths: the root */
#else
#define CWD 2
#endif
static unsigned cur_uid, cur_gid, raw, links;
static struct umsg_groups cur_groups;                    /* "groups g1,g2,...": the caller's supplementary groups */
static int opened[1000], nopened;
static u32 num(const char *s) { return s && s[0] == '%' && atoi(s + 1) > 0 && atoi(s + 1) <= nopened ? (u32)opened[atoi(s + 1) - 1] : (u32)atoi(s ? s : "0"); }

static int req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct { struct umsg m; struct umsg_groups g; } km; /* as the kernel's (vfs.c): the groups follow the message */
#define m km.m
    memset(&km, 0, sizeof km);
    km.g = cur_groups;
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    m.uid = (unsigned short)cur_uid; m.gid = (unsigned short)cur_gid; m.umask = 022;
    m.from = 7;
    m.kflags = raw ? 0 : UMSG_KERNEL | UMSG_GROUPS | (links ? UMSG_LINKS : 0);
    return handle(&m);
#undef m
}

static u32 flags_of(const char *s)
{
    u32 f = 0; int r = 0, w = 0;
    for (; *s; s++) {
        if (*s == 'r') r = 1;
        if (*s == 'w') w = 1;
        if (*s == 'c') f |= O_CREAT;
        if (*s == 't') f |= O_TRUNC;
        if (*s == 'a') f |= O_APPEND;
        if (*s == 'x') f |= O_EXCL;
    }
    return f | (r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY);
}

#if defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
/* tree DIR: every name under DIR, one line each: kind, mode, size, links, mtime, crc32 of all its data read in pieces,
 * the path, the listed inode and -> a soft link's text. A line "! CODE PATH" for any error. The judge of a real volume */
static u32 crc_of(u32 c, const u8 *p, u32 n)
{
    static u32 t[256];
    if (!t[1]) for (u32 i = 0; i < 256; i++) { u32 x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ x >> 1 : x >> 1; t[i] = x; }
    c = ~c;
    while (n--) c = t[(c ^ *p++) & 255] ^ c >> 8;
    return ~c;
}
static u32 tree_left = 0xFFFFFFFFu, tree_bytes = 0xFFFFFFFFu;     /* "tree DIR N MB": where a damaged volume's walk stops */
static void tree(const char *dir)
{
    static u8 data[1 << 17];
    struct ustat ds;
    int ino = req(FS_OPEN, (u32)dir, CWD, O_RDONLY, (u32)&ds);
    if (ino < 0) { printf("! %d %s\n", ino, dir); return; }
    u32 cookie = 0;
    struct udirent e;
    int r;
    while ((r = req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&cookie)) > 0) {
        if (!tree_left || !--tree_left) break;
        char *full = malloc(FS_PATHMAX);
        struct ustat st;
        if ((u32)snprintf(full, FS_PATHMAX, "%s/%s", strcmp(dir, "/") ? dir : "", e.name) >= FS_PATHMAX) {
            printf("toolong %s/%s\n", dir, e.name);     /* over the system's path limit: reached only with chdir */
            free(full);
            continue;
        }
        char *ask = malloc(FS_PATHMAX);
        strcpy(ask, full);
        int rs = req(FS_LSTAT, (u32)ask, CWD, (u32)&st, 0);
        if (rs < 0) printf("! %d %s\n", rs, full);
        else if ((st.mode & 0170000) == S_IFDIR) {
            printf("d %o %u %u %u 0 %s #%u\n", st.mode & 07777, 0u, st.links, st.mtime, full, st.ino);
            tree(full);
        } else if ((st.mode & 0170000) == 0120000) {
            static char t[256];
            strcpy(ask, full);
            rs = req(FS_READLINK, (u32)ask, CWD, (u32)t, 255);
            if (rs < 0) printf("! %d %s\n", rs, full);
            else { t[rs] = 0; printf("l %o %u %u %u 0 %s #%u -> %s\n", st.mode & 07777, st.size, st.links, st.mtime, full, st.ino, t); }
        } else {
            struct ustat os;
            strcpy(ask, full);
            int fi = req(FS_OPEN, (u32)ask, CWD, O_RDONLY, (u32)&os);
            if (fi < 0) { printf("! %d %s\n", fi, full); free(ask); free(full); continue; }
            u32 off = 0, crc = 0;
            static const u32 piece[] = { 65536, 100, 777, 4096, 1, 131072 };
            for (u32 i = 0; ; i++) {
                rs = req(FS_READ, (u32)fi, off, (u32)data, piece[i % 6]);
                if (rs <= 0 || (u32)rs > tree_bytes) break;
                tree_bytes -= (u32)rs;
                crc = crc_of(crc, data, (u32)rs);
                off += (u32)rs;
            }
            req(FS_CLOSE, (u32)fi, 0, 0, 0);
            if (rs < 0) printf("! %d %s\n", rs, full);
            if (tree_bytes && off != st.size) printf("! short %u %s\n", off, full);
            printf("f %o %u %u %u %08x %s #%u\n", st.mode & 07777, st.size, st.links, st.mtime, crc, full, st.ino);
        }
        free(ask); free(full);
    }
    req(FS_CLOSE, (u32)ino, 0, 0, 0);
    if (r < 0) printf("! %d %s (listing)\n", r, dir);
}
#endif

int main(void)
{
    char line[1024];
#ifndef ZFSD
    disk_port = 1;
    ext2_busy = open_busy;
    cache_by_memory();                                    /* HOSTDISK_NOFAST: as on a machine without fast RAM */
#endif
    static char big[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *cmd = strtok(line, " "), *a = strtok(0, " "), *b = strtok(0, " "), *c = strtok(0, "");
        if (!cmd || cmd[0] == '#') continue;
        int r = 0;
        struct ustat st;
        if (!strcmp(cmd, "mount")) {
#ifdef ZFSD
#if defined(FATD) || defined(FFSD) || defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
            if (a && options(a)) { printf("mount -> bad option\n"); continue; }
            r = try_mount("hd0", 0);
            printf("mount -> %d%s\n", r, readonly ? " readonly" : "");
            if (r) return 0;                              /* as fatd: a refused volume has no server */
            continue;
#else
            r = try_mount("hd0", 0, 0);
            printf("mount -> %d\n", r);
            continue;
#endif
#else
            r = try_mount("hd0", a && !strcmp(a, "root") ? EXT2_RW : EXT2_ANY, 0);
            printf("mount -> %d%s\n", r, ext2_readonly ? " readonly" : "");
            continue;
#endif
        }
        if (!strcmp(cmd, "raw")) { raw = a && !strcmp(a, "on"); printf("raw -> %u\n", raw); continue; }
        if (!strcmp(cmd, "links")) { links = a && !strcmp(a, "on"); printf("links -> %u\n", links); continue; }
        static char pa[FS_PATHMAX], pb[FS_PATHMAX];      /* the kernel's buffers: a server may write TEXT there */
        char *a0 = a, *b0 = b;
#if defined(FATD) || defined(FFSD) || defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
        for (char *t = a; t && *t; t++) if (*t == '+') *t = ' ';  /* a name with spaces in a command line */
        if (b && strcmp(cmd, "write")) for (char *t = b; *t; t++) if (*t == '+') *t = ' ';
#endif
        if (a && strlen(a) < sizeof pa) { strcpy(pa, a); a = pa; }
        if (b && strlen(b) < sizeof pb && strcmp(cmd, "chmod") && strcmp(cmd, "utimes") && strcmp(cmd, "chown") &&
            strcmp(cmd, "open") && strcmp(cmd, "write") && strcmp(cmd, "read") && strcmp(cmd, "as")) { strcpy(pb, b); b = pb; }
        if (!strcmp(cmd, "groups")) {                 /* groups [g1,g2,...]: none without an argument */
            cur_groups.n = 0;
            for (char *s = a; s && *s && cur_groups.n < NGROUPS; ) { cur_groups.g[cur_groups.n++] = (unsigned short)atoi(s); s = strchr(s, ','); if (s) s++; }
            printf("groups -> %u\n", cur_groups.n);
            continue;
        }
        if (!strcmp(cmd, "as")) { cur_uid = (unsigned)atoi(a); cur_gid = (unsigned)atoi(b); printf("as -> %u %u\n", cur_uid, cur_gid); continue; }
        if (!strcmp(cmd, "open")) { r = req(FS_OPEN, (u32)a, CWD, flags_of(b ? b : "r"), (u32)&st); if (r > 0 && nopened < 1000) opened[nopened++] = r; }
        else if (!strcmp(cmd, "close")) r = req(FS_CLOSE, num(a), 0, 0, 0);
        else if (!strcmp(cmd, "write")) {
            const char *t = c ? c : "";
            u32 n = (u32)strlen(t);
            if (t[0] == '@') { n = (u32)atoi(t + 1); if (n > sizeof big) n = sizeof big; for (u32 i = 0; i < n; i++) big[i] = (char)('a' + i % 23); t = big; }
            r = req(FS_WRITE, num(a), (u32)atoi(b), (u32)t, n);
        } else if (!strcmp(cmd, "read")) {
            u32 n = (u32)atoi(c);
            if (n > sizeof big - 1) n = sizeof big - 1;
            r = req(FS_READ, num(a), (u32)strtoul(b, 0, 10), (u32)big, n);   /* (an offset past 2 GB: D47) */
            if (r > 0) { big[r] = 0; printf("read -> %d %s\n", r, r <= 200 ? big : "(long)"); continue; }
        } else if (!strcmp(cmd, "fstat") || !strcmp(cmd, "stat") || !strcmp(cmd, "lstat")) {
            r = !strcmp(cmd, "fstat") ? req(FS_FSTAT, num(a), (u32)&st, 0, 0)
                                      : req(!strcmp(cmd, "stat") ? FS_STAT : FS_LSTAT, (u32)a, CWD, (u32)&st, 0);
            if (r == 0) { printf("%s -> 0 ino %u mode %o size %u links %u uid %u gid %u mtime %u\n", cmd, st.ino, st.mode, st.size, st.links, st.uid, st.gid, st.mtime); continue; }
        }
        else if (!strcmp(cmd, "chdir")) r = req(FS_CHDIR, (u32)a, CWD, 0, 0);
        else if (!strcmp(cmd, "unlink")) r = req(FS_UNLINK, (u32)a, CWD, 0, 0);
        else if (!strcmp(cmd, "rmdir")) r = req(FS_RMDIR, (u32)a, CWD, 0, 0);
        else if (!strcmp(cmd, "mkdir")) r = req(FS_MKDIR, (u32)a, CWD, 0, 0);
        else if (!strcmp(cmd, "symlink")) r = req(FS_SYMLINK, (u32)a, (u32)b, CWD, 0);
        else if (!strcmp(cmd, "link")) r = req(FS_LINK, (u32)a, (u32)b, CWD, 0);
        else if (!strcmp(cmd, "rename")) r = req(FS_RENAME, (u32)a, (u32)b, CWD, 0);
        else if (!strcmp(cmd, "chmod")) r = req(FS_CHMOD, (u32)a, CWD, (u32)strtol(b, 0, 8), 0);
        else if (!strcmp(cmd, "utimes")) r = req(FS_UTIMES, (u32)a, CWD, !strcmp(b, "now") ? FS_UTIME_NOW : (u32)atoi(b), 0);
        else if (!strcmp(cmd, "chown")) { char *g = strtok(c, " "); r = req(FS_CHOWN, (u32)a, CWD, (u32)atoi(b), g ? (u32)atoi(g) : 0xFFFFFFFFu); }
        else if (!strcmp(cmd, "readlink")) {
            r = req(FS_READLINK, (u32)a, CWD, (u32)big, 255);
            if (r > 0) { big[r] = 0; printf("readlink -> %d %s\n", r, big); continue; }
        } else if (!strcmp(cmd, "ls")) {
            struct ustat ds;
            int ino = req(FS_OPEN, (u32)a, CWD, O_RDONLY, (u32)&ds);
            if (ino < 0) { printf("ls -> %d\n", ino); continue; }
            u32 cookie = 0, n = 0;
            struct udirent e;
            printf("ls ->");
            while ((r = req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&cookie)) > 0) { printf(" %s", e.name); n++; }
            req(FS_CLOSE, (u32)ino, 0, 0, 0);
            printf(" (%u, %d)\n", n, r);
            continue;
        }
        else if (!strcmp(cmd, "cut")) { extern unsigned long hostdisk_writes, hostdisk_limit; hostdisk_limit = hostdisk_writes + (unsigned long)atol(a); printf("cut -> %lu\n", hostdisk_limit); continue; }
        else if (!strcmp(cmd, "writes")) { extern unsigned long hostdisk_writes; printf("writes -> %lu\n", hostdisk_writes); continue; }
        else if (!strcmp(cmd, "sync")) r = req(FS_SYNC, 0, 0, 0, 0);
        else if (!strcmp(cmd, "quit")) r = req(FS_QUIT, 0, 0, 0, 0);
#ifdef ZFSD
        else if (!strcmp(cmd, "dirty")) { printf("dirty -> %d\n", dirty); continue; }   /* changes not yet committed */
#if defined(FATD) || defined(FFSD) || defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
        else if (!strcmp(cmd, "commit")) r = sync_all();        /* the timer's commit */
        else if (!strcmp(cmd, "umount")) { r = req(FS_QUIT, 0, 0, 0, 0); if (!r) r = go_clean(); }
#if defined(PFS3D) || defined(ISO9660D) || defined(EXFATD)
        else if (!strcmp(cmd, "tree")) {
            if (b0) { tree_left = (u32)atoi(b0); tree_bytes = c ? (u32)atoi(c) << 20 : 0xFFFFFFFFu; }
            tree(a0 && *a0 ? a0 : "/");
            printf("tree -> done\n");
            continue;
        }
#endif
#ifdef FFSD
        else if (!strcmp(cmd, "rmall")) {                 /* rmall DIR: every name removed while it is listed, as some programs do */
            struct ustat ds;
            struct udirent e;
            int ino = req(FS_OPEN, (u32)a, CWD, O_RDONLY, (u32)&ds), n = 0;
            u32 cookie = 0;
            while (ino > 0 && req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&cookie) > 0) {
                static char full[FS_PATHMAX];
                snprintf(full, sizeof full, "%s/%s", a0, e.name);
                if (!req(FS_UNLINK, (u32)full, CWD, 0, 0)) n++;
            }
            printf("rmall -> %d\n", n);
            continue;
        }
#endif
        else if (!strcmp(cmd, "dump") || !strcmp(cmd, "dumpp")) {   /* dump INO HOSTFILE: the file through FS_READ, in odd
                                                                     * pieces; dumpp PATH HOSTFILE: opened by path first */
            u32 ino = 0;
            if (cmd[4]) {
                struct ustat ps;
                int pi = req(FS_OPEN, (u32)a, CWD, O_RDONLY, (u32)&ps);
                if (pi < 0) { printf("dumpp -> %d\n", pi); continue; }
                ino = (u32)pi;
            } else ino = num(a);
            FILE *out = fopen(b0, "wb");
            u32 off = 0;
            static const u32 piece[] = { 4096, 100, 777, 65536, 1, 512 };
            for (u32 i = 0; out; i++) {
                r = req(FS_READ, ino, off, (u32)big, piece[i % 6]);
                if (r <= 0) break;
                fwrite(big, 1, (size_t)r, out);
                off += (u32)r;
            }
            if (out) fclose(out);
            if (cmd[4]) req(FS_CLOSE, ino, 0, 0, 0);
            printf("%s -> %d %u\n", cmd, r, off);
            continue;
        }
        else if (!strcmp(cmd, "lsat")) {                  /* lsat DIR COOKIE: one FS_READDIR at a cookie a program could seek to */
            struct ustat ds;
            struct udirent e;
            u32 cookie = (u32)strtoul(b0 ? b0 : "0", 0, 10), nc = 0;
            int ino = req(FS_OPEN, (u32)a, CWD, O_RDONLY, (u32)&ds);
            r = ino < 0 ? ino : req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&nc);
            if (ino >= 0) req(FS_CLOSE, (u32)ino, 0, 0, 0);
            printf("lsat -> %d %s\n", r, r > 0 ? e.name : "");
            continue;
        }
        else if (!strcmp(cmd, "reads")) {                 /* the disk server's reads so far: requests and sectors (D47) */
            extern unsigned long hostdisk_reads, hostdisk_calls;
            printf("reads -> %lu %lu\n", hostdisk_calls, hostdisk_reads);
            continue;
        }
        else if (!strcmp(cmd, "statfs")) {
            struct ustatfs f;
            r = req(FS_STATFS, (u32)&f, 0, 0, 0);
            if (!r) { printf("statfs -> 0 bsize %u blocks %u bfree %u\n", f.bsize, f.blocks, f.bfree); continue; }
        }
#endif
#else
        else if (!strcmp(cmd, "unclean")) {                             /* what a restarted fsd does after a crash */
            fsd_marking = 1; r = ext2_mark_unclean(); fsd_marking = 0;
            if (!r) need = marked = 1;
        }
        else if (!strcmp(cmd, "mark")) r = req(FS_MARK, (u32)atoi(a), 0, 0, 0);   /* FS_MARK (D15): 1 = needs a check, 0 = clean */
        else if (!strcmp(cmd, "idle")) { quiet(); r = marked; }
        else if (!strcmp(cmd, "state")) {
            unsigned char sec[512];
            r = fs_blk_read(2, 1, sec);
            printf("state -> %d marked %d need %d hold %d orphans %d unmarked %lu\n", r ? -1 : sec[58] & 1, marked, need, hold,
                   norphans, fsd_unmarked_writes);
            continue;
        }
#endif
        else { printf("%s -> unknown command\n", cmd); continue; }
        if (fsl_is(r)) {                                  /* D26: which argument was rewritten, and to what */
            int one = (-r - FS_LINKED) & FSL_A1;
            const char *now = one ? b : a;
            printf("%s -> %d %s %s%s\n", cmd, r, one ? "a1" : "a0", now, (one ? a : b) && strcmp(one ? a : b, one ? a0 : b0) ? " (other changed)" : "");
            continue;
        }
        printf("%s -> %d\n", cmd, r);
    }
#ifndef ZFSD
    if (fsd_unmarked_writes) printf("unmarked-writes %lu\n", fsd_unmarked_writes);
#endif
    return 0;
}
