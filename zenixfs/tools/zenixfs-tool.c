/* SPDX-License-Identifier: GPL-2.0-only */
/* zenixfs-tool: make, check and use ZenixFS images from Linux.
 *   zenixfs-tool mkfs IMAGE SIZE_MB [-b 10|11|12] [-l LABEL] [-c latin1|utf8]
 *   zenixfs-tool info|fsck IMAGE
 *   zenixfs-tool ls IMAGE [-l] [PATH]      stat IMAGE PATH      cat IMAGE PATH
 *   zenixfs-tool put IMAGE HOSTFILE PATH      put IMAGE -r HOSTDIR PATH   (copy a directory tree in)
 *   zenixfs-tool get IMAGE PATH [HOSTFILE]    get IMAGE -r PATH HOSTDIR  (copy a tree out; without HOSTFILE a file goes to stdout)
 *   zenixfs-tool mkdir|rm|rmdir IMAGE PATH    mv IMAGE FROM TO
 *   zenixfs-tool chmod IMAGE OCTALMODE PATH   chown IMAGE UID GID PATH
 *   zenixfs-tool ln IMAGE TARGET LINKPATH     (symbolic link)       ln IMAGE -h EXISTING NEWPATH   (hard link)
 *   zenixfs-tool salvage IMAGE OUTDIR [--commit K] [--no-carve] [--old-copies]   copy out everything that can still be read, skipping damage
 *   zenixfs-tool rescue IMAGE NEWIMAGE SIZE_MB [--old-copies]   salvage into a temporary folder, then make a fresh clean image from it
 *   zenixfs-tool repair IMAGE [--rollback K]   rebuild a damaged superblock and free-space map; --rollback K goes back K commits
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
#include "../zenixfs.h"
#include "../host/zdev.h"
#include "zenixfs-carve.h"

static struct zfile img;
static struct zenixfs *fs;

static void die(const char *what, int err) { fprintf(stderr, "zenixfs-tool: %s: error %d%s\n", what, err, err == ZENIXFS_ENOENT ? " (no such file or directory)" : err == ZENIXFS_EEXIST ? " (exists)" : err == ZENIXFS_ENOSPC ? " (no space)" : err == ZENIXFS_ENOTEMPTY ? " (directory not empty)" : err == ZENIXFS_ECORRUPT ? " (damaged)" : ""); exit(1); }
static void usage(void) { fputs("usage: zenixfs-tool mkfs|info|fsck|ls|stat|cat|put|get|mkdir|rm|rmdir|mv|ln IMAGE ...   (see the top of tools/zenixfs-tool.c)\n", stderr); exit(2); }
static void fsck_log(void *c, const char *m) { (void)c; printf("  %s\n", m); }

static int open_image(const char *path)
{
    /* the block size is stored in the superblock; try the three sizes until the superblock agrees */
    for (uint32_t bs = 10; bs <= 12; bs++) {
        if (zfile_open(&img, path, bs, 0) < 0) die("open image", -1);
        if (zenixfs_mount(&fs, &img.dev, 512) == 0) return 0;
        zfile_close(&img);
    }
    return -1;
}
/* resolve a path to an inode; if want_parent, stop at the parent and return the last name */
static int resolve(const char *path, int want_parent, uint32_t *ino, const char **last, int *lastlen)
{
    uint32_t cur = ZENIXFS_ROOT_INO; const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) { if (want_parent) return ZENIXFS_EINVAL; *ino = cur; return 0; }
        const char *e = p; while (*e && *e != '/') e++;
        const char *q = e; while (*q == '/') q++;
        if (want_parent && !*q) { *ino = cur; *last = p; *lastlen = (int)(e - p); return 0; }
        int r = zenixfs_lookup(fs, cur, p, (int)(e - p), &cur);
        if (r) return r;
        p = e;
    }
}
static void must_resolve(const char *path, uint32_t *ino) { int r = resolve(path, 0, ino, NULL, NULL); if (r) die(path, r); }

static void mode_str(uint32_t m, char *s)
{
    s[0] = (m & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR ? 'd' : (m & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK ? 'l' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++) s[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    s[10] = 0;
}
static void ls_dir(uint32_t dir, int lng)
{
    uint64_t cookie = 0; char name[300]; uint32_t ino; uint8_t type;
    int n = 0, cap = 64; char **names = malloc(cap * sizeof *names); uint32_t *inos = malloc(cap * sizeof *inos);
    while (zenixfs_readdir(fs, dir, &cookie, name, &ino, &type) > 0) {
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (n == cap) { cap *= 2; names = realloc(names, cap * sizeof *names); inos = realloc(inos, cap * sizeof *inos); }
        names[n] = strdup(name); inos[n++] = ino;
    }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcmp(names[i], names[j]) > 0) { char *t = names[i]; names[i] = names[j]; names[j] = t; uint32_t u = inos[i]; inos[i] = inos[j]; inos[j] = u; }
    for (int i = 0; i < n; i++) {
        if (lng) {
            struct zenixfs_inode in; char ms[11]; char tb[32] = "";
            zenixfs_getattr(fs, inos[i], &in); mode_str(in.mode, ms);
            time_t t = (time_t)in.mtime; struct tm *tm = gmtime(&t); if (tm) strftime(tb, sizeof tb, "%Y-%m-%d %H:%M", tm);
            printf("%s %3u %5u %5u %10llu %s %s", ms, in.nlink, in.uid, in.gid, (unsigned long long)in.size, tb, names[i]);
            if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK) { char tg[300]; int l = zenixfs_readlink(fs, inos[i], tg, 299); if (l > 0) { tg[l] = 0; printf(" -> %s", tg); } }
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
    r = zenixfs_lookup(fs, dir, nm, nl, &ino);
    if (r == 0) { r = zenixfs_truncate(fs, ino, 0); if (r) die(dest, r); }
    else { r = zenixfs_create(fs, dir, nm, nl, st.st_mode & 07777, 0, 0, &ino); if (r) die(dest, r); }
    uint8_t *buf = malloc(1 << 17); size_t n; uint64_t off = 0;
    zenixfs_set_time(fs, (uint64_t)st.st_mtime);
    while ((n = fread(buf, 1, 1 << 17, f)) > 0) { r = zenixfs_write(fs, ino, off, buf, (uint32_t)n); if (r < 0) die(dest, r); off += n; }
    free(buf); fclose(f);
}
static void put_tree(const char *host, const char *dest)
{
    struct stat st; if (lstat(host, &st) < 0) { perror(host); exit(1); }
    if (S_ISDIR(st.st_mode)) {
        uint32_t dir, ino; const char *nm; int nl;
        if (strcmp(dest, "/")) {
            int r = resolve(dest, 1, &dir, &nm, &nl); if (r) die(dest, r);
            r = zenixfs_mkdir(fs, dir, nm, nl, st.st_mode & 07777, 0, 0, &ino); if (r && r != ZENIXFS_EEXIST) die(dest, r);
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
        r = zenixfs_symlink(fs, dir, nm, nl, tg, 0, 0, &ino); if (r) die(dest, r);
    } else if (S_ISREG(st.st_mode)) put_file(host, dest);
}
static void get_file(uint32_t ino, FILE *out)
{
    struct zenixfs_inode in; int r = zenixfs_getattr(fs, ino, &in); if (r) die("stat", r);
    uint8_t *buf = malloc(1 << 17); uint64_t off = 0;
    while (off < in.size) { int n = zenixfs_read(fs, ino, off, buf, 1 << 17); if (n < 0) die("read", n); if (!n) break; fwrite(buf, 1, n, out); off += n; }
    free(buf);
}
static void get_tree(uint32_t ino, const char *host)
{
    struct zenixfs_inode in; zenixfs_getattr(fs, ino, &in);
    if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) {
        mkdir(host, 0755);
        uint64_t cookie = 0; char name[300]; uint32_t c; uint8_t type;
        while (zenixfs_readdir(fs, ino, &cookie, name, &c, &type) > 0) {
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            char h[4096]; snprintf(h, sizeof h, "%s/%s", host, name); get_tree(c, h);
        }
    } else if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK) {
        char tg[4096]; int l = zenixfs_readlink(fs, ino, tg, sizeof tg - 1); if (l >= 0) { tg[l] = 0; symlink(tg, host); }
    } else { FILE *f = fopen(host, "wb"); if (!f) { perror(host); return; } get_file(ino, f); fclose(f); }
}


/* ---------- recovery ---------- */
struct salv { unsigned files, partial, dirs, links, unreadable_dirs, unreadable_entries; };

static void salv_note(const char *what, const char *path, int err)
{
    fprintf(stderr, "salvage: %s: %s (error %d)\n", what, path, err);
}
static void salv_dir(uint32_t dir, const char *host, struct salv *sv, int depth)
{
    uint64_t cookie = 0; char name[300]; uint32_t ino; uint8_t type;
    if (depth > 100) return;
    for (;;) {
        int r = zenixfs_readdir(fs, dir, &cookie, name, &ino, &type);
        if (r < 0) { salv_note("directory cut short", host, r); sv->unreadable_dirs++; return; }
        if (r == 0) return;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char path[4096]; snprintf(path, sizeof path, "%s/%s", host, name);
        struct zenixfs_inode in;
        if ((r = zenixfs_getattr(fs, ino, &in)) < 0) { salv_note("entry has no readable inode", path, r); sv->unreadable_entries++; continue; }
        if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) {
            mkdir(path, 0755);
            sv->dirs++;
            salv_dir(ino, path, sv, depth + 1);
        } else if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFLNK) {
            char tg[4096]; int l = zenixfs_readlink(fs, ino, tg, sizeof tg - 1);
            if (l >= 0) { tg[l] = 0; symlink(tg, path); sv->links++; } else { salv_note("link target unreadable", path, l); sv->unreadable_entries++; }
        } else {
            FILE *f = fopen(path, "wb");
            if (!f) continue;
            uint8_t *buf = malloc(1 << 16); uint64_t off = 0; int bad = 0;
            while (off < in.size) {
                int n = zenixfs_read(fs, ino, off, buf, 1 << 16);
                if (n <= 0) { bad = n < 0 ? n : -1; break; }
                fwrite(buf, 1, n, f); off += n;
            }
            free(buf);
            fclose(f);
            if (!bad) {                                      /* no read error, but were all the blocks there? (a lost extent reads as a hole of zeros) */
                uint64_t mapped = 0;
                struct zenixfs_statfs sf; zenixfs_statfs(fs, &sf);
                if (zenixfs_mapped_blocks(fs, ino, &mapped) == 0 && in.size && mapped < (in.size + sf.bsize - 1) / sf.bsize)
                    bad = -2;                                  /* (a genuinely sparse file is flagged too: the name says "partial", the data is not lost) */
            }
            if (bad) {                                       /* keep what was read, but not under the real name: the carve pass may have the whole file */
                char part[4200]; snprintf(part, sizeof part, "%s.partial", path);
                rename(path, part);
                salv_note("file data unreadable after some bytes", path, bad);
                sv->partial++;
            } else sv->files++;
            chmod(path, in.mode & 0777);
        }
    }
}

static int cmd_salvage(const char *image, const char *outdir, int skip, int carve, int old)
{
    struct salv sv; memset(&sv, 0, sizeof sv);
    mkdir(outdir, 0755);
    int mounted = 0;
    for (uint32_t bs = 10; bs <= 12 && !mounted; bs++) {
        if (zfile_open(&img, image, bs, 0) < 0) { perror(image); return 1; }
        struct zenixfs_mount_opts mo = { 512, 0, ZENIXFS_MNT_NOBITMAPCHECK | ZENIXFS_MNT_READONLYISH, (uint32_t)skip };
        if (zenixfs_mount_opts(&fs, &img.dev, &mo) == 0) mounted = 1; else zfile_close(&img);
    }
    int problems = 0;
    if (mounted) {
        struct zenixfs_stats ms; zenixfs_get_stats(fs, &ms);
        if (ms.mount_damage) printf("salvage: %u commit record(s) were unreadable; reading the last good commit\n", ms.mount_damage);
        salv_dir(ZENIXFS_ROOT_INO, outdir, &sv, 0);
        problems = sv.partial + sv.unreadable_dirs + sv.unreadable_entries;
        printf("salvage: walked the tree: %u files, %u directories, %u links copied; %u partial files, %u directories cut short, %u entries unreadable\n",
               sv.files, sv.dirs, sv.links, sv.partial, sv.unreadable_dirs, sv.unreadable_entries);
        zenixfs_unmount(fs); zfile_close(&img);                /* nothing was changed: READONLYISH mount, and the unmount commits nothing */
    } else printf("salvage: the volume cannot be mounted (superblock, commit records or free-space map damaged)\n");
    if (carve && (!mounted || problems)) {
        struct carve_stats cs;
        int r = zenixfs_carve(image, outdir, &cs, 0, old);
        if (r < 0) { printf("salvage: carving found nothing usable (%d)\n", r); return mounted ? 0 : 1; }
        printf("salvage: carved from the raw blocks (block size %d, %u valid leaves%s): %u files, %u directories, %u links added; %u in lost+found; %u files marked .partial, %u marked .nosize\n",
               cs.bshift, cs.leaves, cs.live_filter ? (old ? ", old copies included" : ", only blocks in use") : ", no usable free-space map: old copies included", cs.files, cs.dirs, cs.links, cs.lost_dirs + cs.lost_files, cs.partial, cs.no_inode);
    }
    printf("salvage: done\n");
    return 0;
}

static int cmd_repair(const char *image, int rollback)
{
    int bs;
    for (bs = 10; bs <= 12; bs++) {                           /* is the superblock fine? */
        if (zfile_open(&img, image, (uint32_t)bs, 0) < 0) { perror(image); return 1; }
        struct zenixfs_mount_opts mo = { 128, 0, ZENIXFS_MNT_NOBITMAPCHECK | ZENIXFS_MNT_READONLYISH, (uint32_t)rollback };
        if (zenixfs_mount_opts(&fs, &img.dev, &mo) == 0) break;
        zfile_close(&img);
    }
    if (bs > 12) {                                            /* no: rewrite it from what the volume itself says about its size */
        int g = zenixfs_guess_bshift(image);
        if (g < 0) { printf("repair: nothing in %s looks like ZenixFS\n", image); return 1; }
        if (zfile_open(&img, image, (uint32_t)g, 0) < 0) { perror(image); return 1; }
        int r = zenixfs_rewrite_super(&img.dev, "recovered", 0, ZENIXFS_CHARSET_LATIN1, (uint64_t)time(NULL));
        printf("repair: superblock rewritten (block size %d bytes, label \"recovered\", reserve default)%s\n", 1 << g, r ? " FAILED" : "");
        zfile_close(&img);
        bs = g;
        if (zfile_open(&img, image, (uint32_t)bs, 0) < 0) return 1;
        struct zenixfs_mount_opts mo = { 128, 0, ZENIXFS_MNT_NOBITMAPCHECK | ZENIXFS_MNT_READONLYISH, (uint32_t)rollback };
        if (zenixfs_mount_opts(&fs, &img.dev, &mo) != 0) {
            printf("repair: still cannot mount: no usable commit record or root block. Use: zenixfs-tool salvage %s OUTDIR\n", image);
            return 1;
        }
    }
    struct zenixfs_stats ms; zenixfs_get_stats(fs, &ms);
    if (ms.mount_damage) printf("repair: %u commit record(s) or root(s) were unreadable; working from the last good commit\n", ms.mount_damage);
    if (rollback) printf("repair: rolled back %d commit(s)\n", rollback);
    int changed = zenixfs_rebuild_bitmap(fs);
    if (changed < 0) { printf("repair: rebuilding the free-space map failed (%d)\n", changed); return 1; }
    printf("repair: free-space map rebuilt from the tree (%d block(s) changed)\n", changed);
    int n = zenixfs_fsck(fs, fsck_log, NULL);
    zenixfs_unmount(fs); zfile_close(&img);
    printf(n ? "repair: %d problem(s) remain (the tree itself is damaged; use salvage to copy out what can be read)\n" : "repair: clean\n", n);
    return n ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) usage();
    const char *cmd = argv[1], *path = argv[2];
    if (!strcmp(cmd, "mkfs")) {
        if (argc < 4) usage();
        unsigned mb = (unsigned)atoi(argv[3]); unsigned bs = mb < 16 ? 10 : mb < 256 ? 11 : 12; const char *label = NULL; uint32_t cs = ZENIXFS_CHARSET_LATIN1;
        for (int i = 4; i + 1 < argc; i += 2) {
            if (!strcmp(argv[i], "-b")) bs = (unsigned)atoi(argv[i + 1]);
            else if (!strcmp(argv[i], "-l")) label = argv[i + 1];
            else if (!strcmp(argv[i], "-c")) cs = !strcmp(argv[i + 1], "utf8") ? ZENIXFS_CHARSET_UTF8 : ZENIXFS_CHARSET_LATIN1;
            else usage();
        }
        if (!mb || bs < 10 || bs > 12) usage();
        uint32_t nblocks = (uint32_t)(((uint64_t)mb << 20) >> bs);
        if (zfile_open(&img, path, bs, (int)nblocks) < 0) die("create image", -1);
        int r = zenixfs_mkfs(&img.dev, label, 0, (uint64_t)time(NULL)); if (r) die("mkfs", r);
        {   /* a volume id (Zenix D11: a filesystem server finds its volume by place and uuid): random */
            uint8_t u[16]; FILE *rf = fopen("/dev/urandom", "rb");
            if (!rf || fread(u, 1, 16, rf) != 16) die("read /dev/urandom", -1);
            fclose(rf);
            if ((r = zenixfs_set_uuid(&img.dev, u))) die("uuid", r);
        }
        r = zenixfs_mount(&fs, &img.dev, 64); if (r) die("mount", r);
        if (cs != ZENIXFS_CHARSET_LATIN1) zenixfs_set_charset(fs, cs);
        struct zenixfs_statfs st; zenixfs_statfs(fs, &st);
        printf("%s: ZenixFS, %u blocks of %u bytes, %.1f MB usable, names %s\n", path, st.blocks, st.bsize, (double)st.bfree * st.bsize / 1048576.0, cs == ZENIXFS_CHARSET_UTF8 ? "UTF-8" : "Latin-1");
        return zenixfs_unmount(fs) ? 1 : 0;
    }
    if (!strcmp(cmd, "salvage") && argc > 3) {
        int skip = 0, carve = 1, old = 0;
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--commit") && i + 1 < argc) skip = atoi(argv[++i]);
            else if (!strcmp(argv[i], "--no-carve")) carve = 0;
            else if (!strcmp(argv[i], "--old-copies")) old = 1;
        }
        return cmd_salvage(path, argv[3], skip, carve, old);
    }
    if (!strcmp(cmd, "rescue") && argc > 4) {
        char tmpl[] = "/tmp/zenixfs-rescue-XXXXXX";
        if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
        char cmdline[8192];
        int old = argc > 5 && !strcmp(argv[5], "--old-copies");
        int r = cmd_salvage(path, tmpl, 0, 1, old);
        if (r) return r;
        snprintf(cmdline, sizeof cmdline, "\"%s\" mkfs \"%s\" %s -l rescued && \"%s\" put \"%s\" -r \"%s\" / && \"%s\" fsck \"%s\"",
                 argv[0], argv[3], argv[4], argv[0], argv[3], tmpl, argv[0], argv[3]);
        r = system(cmdline);
        printf(r == 0 ? "rescue: %s is a fresh, clean image with everything that could be recovered (the folder %s has the same files)\n"
                      : "rescue: making the new image failed; the recovered files are in %s\n", r == 0 ? argv[3] : tmpl, tmpl);
        return r != 0;
    }
    if (!strcmp(cmd, "repair")) {
        int rb = 0;
        for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--rollback") && i + 1 < argc) rb = atoi(argv[++i]);
        return cmd_repair(path, rb);
    }
    if (open_image(path) < 0) { fprintf(stderr, "zenixfs-tool: %s is not a ZenixFS image (or its superblock and commit records are damaged)\n", path); return 1; }
    zenixfs_set_time(fs, (uint64_t)time(NULL));
    int rc = 0;
    struct zenixfs_stats ms; zenixfs_get_stats(fs, &ms);
    if (ms.mount_damage) fprintf(stderr, "zenixfs-tool: WARNING: %u commit record(s) or root(s) were unreadable at mount (damage, or a write that was interrupted); the volume was mounted from the last good commit, the newest changes may be missing\n", ms.mount_damage);
    if (!strcmp(cmd, "info")) {
        struct zenixfs_statfs st; zenixfs_statfs(fs, &st);
        printf("ZenixFS: block size %u, %u blocks (%.1f MB), %u free blocks (%.1f MB usable), %u inodes used, names %s\n", st.bsize, st.blocks, (double)st.blocks * st.bsize / 1048576.0,
               st.bfree, (double)st.bfree * st.bsize / 1048576.0, st.files, zenixfs_charset(fs) == ZENIXFS_CHARSET_UTF8 ? "UTF-8" : "Latin-1");
    } else if (!strcmp(cmd, "fsck")) {
        int n = zenixfs_fsck(fs, fsck_log, NULL);
        printf(n ? "%d problem(s) found\n" : "clean\n", n); rc = n ? 1 : 0;
    } else if (!strcmp(cmd, "ls")) {
        int lng = 0, a = 3; if (argc > 3 && !strcmp(argv[3], "-l")) { lng = 1; a = 4; }
        const char *p = argc > a ? argv[a] : "/"; uint32_t ino; must_resolve(p, &ino);
        struct zenixfs_inode in; zenixfs_getattr(fs, ino, &in);
        if ((in.mode & ZENIXFS_S_IFMT) == ZENIXFS_S_IFDIR) ls_dir(ino, lng); else puts(p);
    } else if (!strcmp(cmd, "stat") && argc > 3) {
        uint32_t ino; must_resolve(argv[3], &ino); struct zenixfs_inode in; zenixfs_getattr(fs, ino, &in); char ms2[11]; mode_str(in.mode, ms2);
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
        if (!strcmp(cmd, "mkdir")) r = zenixfs_mkdir(fs, dir, nm, nl, 0755, 0, 0, &ino);
        else if (!strcmp(cmd, "rm")) r = zenixfs_unlink(fs, dir, nm, nl);
        else r = zenixfs_rmdir(fs, dir, nm, nl);
        if (r) die(argv[3], r);
    } else if (!strcmp(cmd, "chmod") && argc > 4) {
        uint32_t ino; must_resolve(argv[4], &ino);
        struct zenixfs_inode in; int r = zenixfs_getattr(fs, ino, &in); if (r) die("chmod", r);
        in.mode = (in.mode & ZENIXFS_S_IFMT) | ((uint32_t)strtoul(argv[3], NULL, 8) & 07777);
        r = zenixfs_setattr(fs, ino, &in); if (r) die("chmod", r);
    } else if (!strcmp(cmd, "chown") && argc > 5) {
        uint32_t ino; must_resolve(argv[5], &ino);
        struct zenixfs_inode in; int r = zenixfs_getattr(fs, ino, &in); if (r) die("chown", r);
        in.uid = (uint32_t)strtoul(argv[3], NULL, 10); in.gid = (uint32_t)strtoul(argv[4], NULL, 10);
        r = zenixfs_setattr(fs, ino, &in); if (r) die("chown", r);
    } else if (!strcmp(cmd, "mv") && argc > 4) {
        uint32_t d1, d2; const char *n1, *n2; int l1, l2; int r = resolve(argv[3], 1, &d1, &n1, &l1); if (r) die(argv[3], r);
        r = resolve(argv[4], 1, &d2, &n2, &l2); if (r) die(argv[4], r);
        r = zenixfs_rename(fs, d1, n1, l1, d2, n2, l2); if (r) die("mv", r);
    } else if (!strcmp(cmd, "ln") && argc > 4) {
        int hard = !strcmp(argv[3], "-h"), a = hard ? 4 : 3;
        if (argc <= a + 1) usage();
        uint32_t dir, ino; const char *nm; int nl; int r = resolve(argv[a + 1], 1, &dir, &nm, &nl); if (r) die(argv[a + 1], r);
        if (hard) { uint32_t t; must_resolve(argv[a], &t); r = zenixfs_link(fs, dir, nm, nl, t); } else r = zenixfs_symlink(fs, dir, nm, nl, argv[a], 0, 0, &ino);
        if (r) die("ln", r);
    } else usage();
    int e = zenixfs_unmount(fs); zfile_close(&img);
    if (e) die("commit", e);
    return rc;
}
