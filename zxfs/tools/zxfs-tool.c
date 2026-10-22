/* SPDX-License-Identifier: GPL-2.0-only */
/* zxfs-tool: make, check and use ZXFS images from Linux.
 *   zxfs-tool mkfs IMAGE SIZE_MB [-b 10|11|12] [-l LABEL] [-c latin1|utf8]
 *   zxfs-tool info|fsck IMAGE
 *   zxfs-tool ls IMAGE [-l] [PATH]      stat IMAGE PATH      cat IMAGE PATH
 *   zxfs-tool put IMAGE HOSTFILE PATH      put IMAGE -r HOSTDIR PATH   (copy a directory tree in)
 *   zxfs-tool get IMAGE PATH [HOSTFILE]    get IMAGE -r PATH HOSTDIR  (copy a tree out; without HOSTFILE a file goes to stdout)
 *   zxfs-tool mkdir|rm|rmdir IMAGE PATH    mv IMAGE FROM TO
 *   zxfs-tool ln IMAGE TARGET LINKPATH     (symbolic link)       ln IMAGE -h EXISTING NEWPATH   (hard link)
 * Options always come after IMAGE.
 * Paths inside the image start with / . Block size by default: under 16 MB 1 KB, under 256 MB 2 KB, else 4 KB. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "../zxfs.h"
#include "../host/zdev.h"

static struct zfile img;
static struct zxfs *fs;

static void die(const char *what, int err) { fprintf(stderr, "zxfs-tool: %s: error %d%s\n", what, err, err == ZXFS_ENOENT ? " (no such file or directory)" : err == ZXFS_EEXIST ? " (exists)" : err == ZXFS_ENOSPC ? " (no space)" : err == ZXFS_ENOTEMPTY ? " (directory not empty)" : err == ZXFS_ECORRUPT ? " (damaged)" : ""); exit(1); }
static void usage(void) { fputs("usage: zxfs-tool mkfs|info|fsck|ls|stat|cat|put|get|mkdir|rm|rmdir|mv|ln IMAGE ...   (see the top of tools/zxfs-tool.c)\n", stderr); exit(2); }
static void fsck_log(void *c, const char *m) { (void)c; printf("  %s\n", m); }

static int open_image(const char *path)
{
    /* the block size is stored in the superblock; try the three sizes until the superblock agrees */
    for (uint32_t bs = 10; bs <= 12; bs++) {
        if (zfile_open(&img, path, bs, 0) < 0) die("open image", -1);
        if (zxfs_mount(&fs, &img.dev, 512) == 0) return 0;
        zfile_close(&img);
    }
    return -1;
}
/* resolve a path to an inode; if want_parent, stop at the parent and return the last name */
static int resolve(const char *path, int want_parent, uint32_t *ino, const char **last, int *lastlen)
{
    uint32_t cur = ZXFS_ROOT_INO; const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { if (want_parent) return ZXFS_EINVAL; *ino = cur; return 0; }
        const char *e = p; while (*e && *e != '/') e++;
        const char *q = e; while (*q == '/') q++;
        if (want_parent && !*q) { *ino = cur; *last = p; *lastlen = (int)(e - p); return 0; }
        int r = zxfs_lookup(fs, cur, p, (int)(e - p), &cur);
        if (r) return r;
        p = e;
    }
}
static void must_resolve(const char *path, uint32_t *ino) { int r = resolve(path, 0, ino, NULL, NULL); if (r) die(path, r); }

static void mode_str(uint32_t m, char *s)
{
    s[0] = (m & ZXFS_S_IFMT) == ZXFS_S_IFDIR ? 'd' : (m & ZXFS_S_IFMT) == ZXFS_S_IFLNK ? 'l' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) s[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    s[10] = 0;
}
static void ls_dir(uint32_t dir, int lng)
{
    uint64_t cookie = 0; char name[300]; uint32_t ino; uint8_t type;
    int n = 0, cap = 64; char **names = malloc(cap * sizeof *names); uint32_t *inos = malloc(cap * sizeof *inos);
    while (zxfs_readdir(fs, dir, &cookie, name, &ino, &type) > 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (n == cap) { cap *= 2; names = realloc(names, cap * sizeof *names); inos = realloc(inos, cap * sizeof *inos); }
        names[n] = strdup(name); inos[n++] = ino;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; uint32_t u = inos[i]; inos[i] = inos[j]; inos[j] = u; }
    for (int i = 0; i < n; i++) {
        if (lng) {
            struct zxfs_inode in; char ms[11]; char tb[32] = "";
            zxfs_getattr(fs, inos[i], &in); mode_str(in.mode, ms);
            time_t t = (time_t)in.mtime; struct tm *tm = gmtime(&t); if (tm) strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", tm);
            printf("%s %3u %5u %5u %10llu %s %s", ms, in.nlink, in.uid, in.gid, (unsigned long long)in.size, tb, names[i]);
            if ((in.mode & ZXFS_S_IFMT) == ZXFS_S_IFLNK) { char tg[300]; int l = zxfs_readlink(fs, inos[i], tg, 299); if (l > 0) { tg[l] = 0; printf(" -> %s", tg); } }
            putchar('\n');
        } else puts(names[i]);
        free(names[i]);
    }
    free(names); free(inos);
}
static void put_file(const char *host, const char *dest)
{
    FILE *f = fopen(host, "rb"); if (!f) { perror(host); exit(1); }
    struct stat st; fstat(fileno(f), &st);
    uint32_t dir, ino; const char *nm; int nl;
    int r = resolve(dest, 1, &dir, &nm, &nl); if (r) die(dest, r);
    r = zxfs_lookup(fs, dir, nm, nl, &ino);
    if (r == 0) { r = zxfs_truncate(fs, ino, 0); if (r) die(dest, r); }
    else { r = zxfs_create(fs, dir, nm, nl, st.st_mode & 07777, 0, 0, &ino); if (r) die(dest, r); }
    uint8_t *buf = malloc(1 << 17); size_t n; uint64_t off = 0;
    zxfs_set_time(fs, (uint64_t)st.st_mtime);
    while ((n = fread(buf, 1, 1 << 17, f)) > 0) { r = zxfs_write(fs, ino, off, buf, (uint32_t)n); if (r < 0) die(dest, r); off += n; }
    free(buf); fclose(f);
}
static void put_tree(const char *host, const char *dest)
{
    struct stat st; if (lstat(host, &st) < 0) { perror(host); exit(1); }
    if (S_ISDIR(st.st_mode)) {
        uint32_t dir, ino; const char *nm; int nl;
        if (strcmp(dest, "/")) {
            int r = resolve(dest, 1, &dir, &nm, &nl); if (r) die(dest, r);
            r = zxfs_mkdir(fs, dir, nm, nl, st.st_mode & 07777, 0, 0, &ino); if (r && r != ZXFS_EEXIST) die(dest, r);
        }
        DIR *d = opendir(host); struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char h[4096], t[4096]; snprintf(h, sizeof h, "%s/%s", host, e->d_name); snprintf(t, sizeof t, "%s/%s", strcmp(dest, "/") ? dest : "", e->d_name);
            put_tree(h, t);
        }
        closedir(d);
    } else if (S_ISLNK(st.st_mode)) {
        char tg[4096]; ssize_t l = readlink(host, tg, sizeof tg - 1); if (l < 0) return; tg[l] = 0;
        uint32_t dir, ino; const char *nm; int nl; int r = resolve(dest, 1, &dir, &nm, &nl); if (r) die(dest, r);
        r = zxfs_symlink(fs, dir, nm, nl, tg, 0, 0, &ino); if (r) die(dest, r);
    } else if (S_ISREG(st.st_mode)) put_file(host, dest);
}
static void get_file(uint32_t ino, FILE *out)
{
    struct zxfs_inode in; int r = zxfs_getattr(fs, ino, &in); if (r) die("stat", r);
    uint8_t *buf = malloc(1 << 17); uint64_t off = 0;
    while (off < in.size) { int n = zxfs_read(fs, ino, off, buf, 1 << 17); if (n < 0) die("read", n); if (!n) break; fwrite(buf, 1, n, out); off += n; }
    free(buf);
}
static void get_tree(uint32_t ino, const char *host)
{
    struct zxfs_inode in; zxfs_getattr(fs, ino, &in);
    if ((in.mode & ZXFS_S_IFMT) == ZXFS_S_IFDIR) {
        mkdir(host, 0755);
        uint64_t cookie = 0; char name[300]; uint32_t c; uint8_t type;
        while (zxfs_readdir(fs, ino, &cookie, name, &c, &type) > 0) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            char h[4096]; snprintf(h, sizeof h, "%s/%s", host, name); get_tree(c, h);
        }
    } else if ((in.mode & ZXFS_S_IFMT) == ZXFS_S_IFLNK) {
        char tg[4096]; int l = zxfs_readlink(fs, ino, tg, sizeof tg - 1); if (l >= 0) { tg[l] = 0; symlink(tg, host); }
    } else { FILE *f = fopen(host, "wb"); if (!f) { perror(host); return; } get_file(ino, f); fclose(f); }
}

int main(int argc, char **argv)
{
    if (argc < 3) usage();
    const char *cmd = argv[1], *path = argv[2];
    if (!strcmp(cmd, "mkfs")) {
        if (argc < 4) usage();
        unsigned mb = (unsigned)atoi(argv[3]); unsigned bs = mb < 16 ? 10 : mb < 256 ? 11 : 12; const char *label = NULL; uint32_t cs = ZXFS_CHARSET_LATIN1;
        for (int i = 4; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "-b")) bs = (unsigned)atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-l")) label = argv[i + 1];
            else if (!strcmp(argv[i], "-c")) cs = !strcmp(argv[i + 1], "utf8") ? ZXFS_CHARSET_UTF8 : ZXFS_CHARSET_LATIN1;
            else usage();
        }
        if (!mb || bs < 10 || bs > 12) usage();
        uint32_t nblocks = (uint32_t)(((uint64_t)mb << 20) >> bs);
        if (zfile_open(&img, path, bs, (int)nblocks) < 0) die("create image", -1);
        int r = zxfs_mkfs(&img.dev, label, 0, (uint64_t)time(NULL)); if (r) die("mkfs", r);
        r = zxfs_mount(&fs, &img.dev, 64); if (r) die("mount", r);
        if (cs != ZXFS_CHARSET_LATIN1) zxfs_set_charset(fs, cs);
        struct zxfs_statfs st; zxfs_statfs(fs, &st);
        printf("%s: ZXFS, %u blocks of %u bytes, %.1f MB usable, names %s\n", path, st.blocks, st.bsize, (double)st.bfree * st.bsize / 1048576.0, cs == ZXFS_CHARSET_UTF8 ? "UTF-8" : "Latin-1");
        return zxfs_unmount(fs) ? 1 : 0;
    }
    if (open_image(path) < 0) { fprintf(stderr, "zxfs-tool: %s is not a ZXFS image (or its superblock and commit records are damaged)\n", path); return 1; }
    zxfs_set_time(fs, (uint64_t)time(NULL));
    int rc = 0;
    struct zxfs_stats ms; zxfs_get_stats(fs, &ms);
    if (ms.mount_damage) fprintf(stderr, "zxfs-tool: WARNING: %u damaged commit record(s) or root(s) at mount; the volume was mounted from an OLDER commit, recent changes may be lost\n", ms.mount_damage);
    if (!strcmp(cmd, "info")) {
        struct zxfs_statfs st; zxfs_statfs(fs, &st);
        printf("ZXFS: block size %u, %u blocks (%.1f MB), %u free blocks (%.1f MB usable), %u inodes used, names %s\n", st.bsize, st.blocks, (double)st.blocks * st.bsize / 1048576.0,
               st.bfree, (double)st.bfree * st.bsize / 1048576.0, st.files, zxfs_charset(fs) == ZXFS_CHARSET_UTF8 ? "UTF-8" : "Latin-1");
    } else if (!strcmp(cmd, "fsck")) {
        int n = zxfs_fsck(fs, fsck_log, NULL);
        printf(n ? "%d problem(s) found\n" : "clean\n", n); rc = n ? 1 : 0;
    } else if (!strcmp(cmd, "ls")) {
        int lng = 0, a = 3; if (argc > 3 && !strcmp(argv[3], "-l")) { lng = 1; a = 4; }
        const char *p = argc > a ? argv[a] : "/"; uint32_t ino; must_resolve(p, &ino);
        struct zxfs_inode in; zxfs_getattr(fs, ino, &in);
        if ((in.mode & ZXFS_S_IFMT) == ZXFS_S_IFDIR) ls_dir(ino, lng); else puts(p);
    } else if (!strcmp(cmd, "stat") && argc > 3) {
        uint32_t ino; must_resolve(argv[3], &ino); struct zxfs_inode in; zxfs_getattr(fs, ino, &in); char ms2[11]; mode_str(in.mode, ms2);
        printf("inode %u  %s (%o)  links %u  uid %u gid %u  size %llu  mtime %llu  flags %x  %s\n", ino, ms2, in.mode, in.nlink, in.uid, in.gid, (unsigned long long)in.size, (unsigned long long)in.mtime, in.flags, (in.flags & 1) ? "stored inside the tree" : "in extents");
    } else if (!strcmp(cmd, "cat") && argc > 3) {
        uint32_t ino; must_resolve(argv[3], &ino); get_file(ino, stdout);
    } else if (!strcmp(cmd, "put") && argc > 4) {
        int a = 3, rec = 0; if (!strcmp(argv[a], "-r")) { rec = 1; a++; }
        if (argc <= a + 1) usage();
        if (rec) put_tree(argv[a], argv[a + 1]); else put_file(argv[a], argv[a + 1]);
    } else if (!strcmp(cmd, "get") && argc > 3) {
        int a = 3, rec = 0; if (!strcmp(argv[a], "-r")) { rec = 1; a++; }
        if (argc <= a) usage();
        uint32_t ino; must_resolve(argv[a], &ino);
        if (rec) { if (argc <= a + 1) usage(); get_tree(ino, argv[a + 1]); }
        else if (argc > a + 1) { FILE *f = fopen(argv[a + 1], "wb"); if (!f) { perror(argv[a + 1]); return 1; } get_file(ino, f); fclose(f); }
        else get_file(ino, stdout);
    } else if ((!strcmp(cmd, "mkdir") || !strcmp(cmd, "rm") || !strcmp(cmd, "rmdir")) && argc > 3) {
        uint32_t dir, ino; const char *nm; int nl; int r = resolve(argv[3], 1, &dir, &nm, &nl); if (r) die(argv[3], r);
        if (!strcmp(cmd, "mkdir")) r = zxfs_mkdir(fs, dir, nm, nl, 0755, 0, 0, &ino);
        else if (!strcmp(cmd, "rm")) r = zxfs_unlink(fs, dir, nm, nl);
        else r = zxfs_rmdir(fs, dir, nm, nl);
        if (r) die(argv[3], r);
    } else if (!strcmp(cmd, "mv") && argc > 4) {
        uint32_t d1, d2; const char *n1, *n2; int l1, l2; int r = resolve(argv[3], 1, &d1, &n1, &l1); if (r) die(argv[3], r);
        r = resolve(argv[4], 1, &d2, &n2, &l2); if (r) die(argv[4], r);
        r = zxfs_rename(fs, d1, n1, l1, d2, n2, l2); if (r) die("mv", r);
    } else if (!strcmp(cmd, "ln") && argc > 4) {
        int hard = !strcmp(argv[3], "-h"), a = hard ? 4 : 3;
        if (argc <= a + 1) usage();
        uint32_t dir, ino; const char *nm; int nl; int r = resolve(argv[a + 1], 1, &dir, &nm, &nl); if (r) die(argv[a + 1], r);
        if (hard) { uint32_t t; must_resolve(argv[a], &t); r = zxfs_link(fs, dir, nm, nl, t); } else r = zxfs_symlink(fs, dir, nm, nl, argv[a], 0, 0, &ino);
        if (r) die("ln", r);
    } else usage();
    int e = zxfs_unmount(fs); zfile_close(&img);
    if (e) die("commit", e);
    return rc;
}
