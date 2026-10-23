// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"

/* tar c|x|t [v] [f archive] [-C dir] [file...]   (ustar; archive defaults to standard output/input; use a pipe
 * through gzip for .tgz). Handles files, directories and symbolic links. */
static int afd, verbose, add_rc;              /* add_rc: 1 when tar c could not add something */
static int rd_full(void *b, int n) { int g = 0; while (g < n) { int r = sys_read(afd, (char *)b + g, n - g); if (r <= 0) break; g += r; } return g; }

static void put_oct(char *f, int w, u32 v) { for (int i = w - 2; i >= 0; i--) { f[i] = '0' + (v & 7); v >>= 3; } f[w - 1] = 0; }
static u32 get_oct(const char *f, int w) { u32 v = 0; for (int i = 0; i < w && f[i] >= '0' && f[i] <= '7'; i++) v = v * 8 + (f[i] - '0'); return v; }

static void write_header(const char *name, const struct ustat *st, char type, const char *link, u32 size)
{
    unsigned char h[512]; memset(h, 0, 512);
    u32 nl = strlen(name);
    if (nl > 100) { u32 cut = nl - 100; while (cut < nl && name[cut] != '/') cut++; if (cut >= nl || cut > 155) { fprintf(2, "tar: %s: name too long\n", name); return; }
                    memcpy(h + 345, name, cut); memcpy(h, name + cut + 1, nl - cut - 1); }
    else memcpy(h, name, nl);
    put_oct((char *)h + 100, 8, st->mode & 07777); put_oct((char *)h + 108, 8, st->uid); put_oct((char *)h + 116, 8, st->gid);
    put_oct((char *)h + 124, 12, size); put_oct((char *)h + 136, 12, st->mtime);
    memset(h + 148, ' ', 8); h[156] = type;
    if (link) { strncpy((char *)h + 157, link, 99); }
    memcpy(h + 257, "ustar", 5); h[262] = 0; h[263] = '0'; h[264] = '0';
    u32 sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    put_oct((char *)h + 148, 7, sum); h[154] = 0; h[155] = ' ';
    sys_write(1, h, 512);
}

/* the parts of add() that need buffers are separate functions, so the recursion (one add() per directory level)
 * carries none of them */
static void __attribute__((noinline)) add_link(const char *path, const struct ustat *st)
{
    char l[100]; int n = sys_readlink(path, l, sizeof l - 1); if (n < 0) return; l[n] = 0;
    write_header(path, st, '2', l, 0);
}
static void __attribute__((noinline)) add_file(const char *path, const struct ustat *st)
{
    int fd = sys_open(path, O_RDONLY); if (fd < 0) { fprintf(2, "tar: %s: %s\n", path, zwhy("cannot open")); add_rc = 1; return; }
    write_header(path, st, '0', 0, st->size);
    char b[512]; u32 left = st->size;
    while (left) { int want = left > 512 ? 512 : (int)left; memset(b, 0, 512); int r = sys_read(fd, b, want); if (r <= 0) { r = want; memset(b, 0, 512); } sys_write(1, b, 512); left -= (u32)r > left ? left : (u32)r; }
    sys_close(fd);
}
static void add(const char *path)
{
    struct ustat st;
    if (sys_lstat(path, &st) < 0) { fprintf(2, "tar: %s: cannot stat\n", path); add_rc = 1; return; }
    u32 t = st.mode & S_IFMT;
    if (verbose) fprintf(2, "%s\n", path);
    if (t == S_IFDIR) {
        u32 pl = strlen(path);
        char *nm = malloc(pl + 2);
        if (!nm) { fprintf(2, "tar: %s: out of memory\n", path); add_rc = 1; return; }
        memcpy(nm, path, pl); nm[pl] = '/'; nm[pl + 1] = 0;
        write_header(nm, &st, '5', 0, 0);
        free(nm);
        char **names; int nn = dir_list(path, &names);        /* heap, not stack: one level per directory */
        if (nn < 0) { fprintf(2, "tar: %s: %s\n", path, zwhy("cannot read")); add_rc = 1; return; }
        for (int i = 0; i < nn; i++) {
            char *c = path_join(path, names[i]);
            if (c) add(c); else { fprintf(2, "tar: %s/%s: %s\n", path, names[i], zwhy("cannot add")); add_rc = 1; }
            free(c);
        }
        dir_free(names, nn);
    } else if (t == S_IFLNK) add_link(path, &st);
    else if (t == S_IFREG) add_file(path, &st);
}

static void mkpath(char *p)
{
    for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; sys_mkdir(p); *s = '/'; }
}

int main(int argc, char **argv)
{
    char mode = 0; const char *file = 0, *dir = 0; int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i]; if (a[0] != '-' && i > 1) break;
        if (a[0] == '-') a++;
        if (!strcmp(argv[i], "-C")) { dir = argv[++i]; continue; }
        for (; *a; a++) { if (*a == 'c' || *a == 'x' || *a == 't') mode = *a; else if (*a == 'v') verbose = 1; else if (*a == 'f') file = argv[++i]; else if (*a == 'C') dir = argv[++i]; else { fprintf(2, "tar: unknown option %c\n", *a); return 2; } }
    }
    if (!mode) { fprintf(2, "usage: tar c|x|t [v] [f archive] [-C dir] [file...]\n"); return 2; }
    if (mode == 'c') {
        if (file && strcmp(file, "-")) { int fd = sys_open(file, O_WRONLY | O_CREAT | O_TRUNC); if (fd < 0) { fprintf(2, "tar: %s: %s\n", file, zwhy("cannot write")); return 1; } sys_dup2(fd, 1); sys_close(fd); }
        if (dir && sys_chdir(dir) < 0) { fprintf(2, "tar: %s: cannot enter\n", dir); return 1; }
        if (i >= argc) { fprintf(2, "tar: nothing to archive\n"); return 2; }
        for (; i < argc; i++) add(argv[i]);
        char z[1024]; memset(z, 0, sizeof z); sys_write(1, z, sizeof z);
        return add_rc;
    }
    afd = 0;
    if (file && strcmp(file, "-")) { afd = sys_open(file, O_RDONLY); if (afd < 0) { fprintf(2, "tar: %s: %s\n", file, zwhy("cannot open")); return 1; } }
    if (mode == 'x' && dir && sys_chdir(dir) < 0) { fprintf(2, "tar: %s: cannot enter\n", dir); return 1; }
    unsigned char h[512]; int rc = 0;
    while (rd_full(h, 512) == 512) {
        if (!h[0]) break;
        char name[300]; name[0] = 0;
        if (h[345]) { strncpy(name, (char *)h + 345, 155); name[155] = 0; strcat(name, "/"); }
        u32 nl = strlen(name); strncpy(name + nl, (char *)h, 100); name[nl + 100] = 0;
        u32 size = get_oct((char *)h + 124, 12), mode_bits = get_oct((char *)h + 100, 8), mt = get_oct((char *)h + 136, 12); char type = h[156] ? h[156] : '0';
        u32 l = strlen(name); while (l > 1 && name[l - 1] == '/') name[--l] = 0;
        if (mode == 't' || verbose) printf("%s%s\n", name, type == '5' ? "/" : "");
        u32 blocks = (size + 511) / 512;
        if (mode == 't') { char skip[512]; for (u32 b = 0; b < blocks; b++) rd_full(skip, 512); if (type != '0' && type != '5' && type != '2') ; continue; }
        if (type == '5') { mkpath(name); sys_mkdir(name); sys_chmod(name, mode_bits); }
        else if (type == '2') { char lk[101]; strncpy(lk, (char *)h + 157, 100); lk[100] = 0; mkpath(name); sys_unlink(name); if (sys_symlink(lk, name) < 0) { fprintf(2, "tar: %s: cannot link\n", name); rc = 1; } }
        else if (type == '0') {
            mkpath(name); int fd = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) { fprintf(2, "tar: %s: %s\n", name, zwhy("cannot create")); rc = 1; }
            char b[512]; u32 left = size;
            for (u32 k = 0; k < blocks; k++) { rd_full(b, 512); u32 n = left > 512 ? 512 : left; if (fd >= 0) sys_write(fd, b, n); left -= n; }
            if (fd >= 0) { sys_close(fd); sys_chmod(name, mode_bits); sys_utimes(name, mt); }
        } else { char skip[512]; for (u32 b = 0; b < blocks; b++) rd_full(skip, 512); }
    }
    return rc;
}
