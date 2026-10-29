// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"

/* tar c|x|t [v] [f archive] [-C dir] [file...]   (ustar; archive defaults to standard output/input; use a pipe
 * through gzip for .tgz). Handles files, directories and symbolic links. */
static int afd, verbose, add_rc;              /* add_rc: 1 when tar c could not add something */
static void out_block(const void *b)                /* one 512-byte block of the archive being written */
{
    if (sys_write(1, b, 512) != 512) { dprintf(2, "tar: cannot write the archive: %s\n", zwhy("write error")); _zexit(2); }
}
/* the header's checksum: the sum of its bytes with the checksum field counted as spaces */
static u32 hsum(const unsigned char *h) { u32 sum = 0; for (int i = 0; i < 512; i++) sum += (i >= 148 && i < 156) ? ' ' : h[i]; return sum; }
static int rd_full(void *b, int n) { int g = 0; while (g < n) { int r = sys_read(afd, (char *)b + g, n - g); if (r <= 0) break; g += r; } return g; }

static void put_oct(char *f, int w, u32 v) { for (int i = w - 2; i >= 0; i--) { f[i] = '0' + (v & 7); v >>= 3; } f[w - 1] = 0; }
static u32 get_oct(const char *f, int w) { u32 v = 0; for (int i = 0; i < w && f[i] >= '0' && f[i] <= '7'; i++) v = v * 8 + (f[i] - '0'); return v; }

static int write_header(const char *name, const struct ustat *st, char type, const char *link, u32 size)
{
    unsigned char h[512]; memset(h, 0, 512);
    u32 nl = strlen(name);
    if (nl > 100) { u32 cut = nl - 100; while (cut < nl && name[cut] != '/') cut++; if (cut >= nl || cut > 155) { dprintf(2, "tar: %s: name too long\n", name); add_rc = 1; return -1; }
                    memcpy(h + 345, name, cut); memcpy(h, name + cut + 1, nl - cut - 1); }
    else memcpy(h, name, nl);
    if (link && strlen(link) > 100) { dprintf(2, "tar: %s: link target too long\n", name); add_rc = 1; return -1; }
    put_oct((char *)h + 100, 8, st->mode & 07777); put_oct((char *)h + 108, 8, st->uid); put_oct((char *)h + 116, 8, st->gid);
    put_oct((char *)h + 124, 12, size); put_oct((char *)h + 136, 12, st->mtime);
    memset(h + 148, ' ', 8); h[156] = type;
    if (link) memcpy(h + 157, link, strlen(link));          /* up to 100 bytes, as ustar allows */
    memcpy(h + 257, "ustar", 5); h[262] = 0; h[263] = '0'; h[264] = '0';
    u32 sum = 0; for (int i = 0; i < 512; i++) sum += h[i];
    put_oct((char *)h + 148, 7, sum); h[154] = 0; h[155] = ' ';
    out_block(h);
    return 0;
}

/* the parts of add() that need buffers are separate functions, so the recursion (one add() per directory level)
 * carries none of them */
static void __attribute__((noinline)) add_link(const char *path, const struct ustat *st)
{
    char l[102]; int n = sys_readlink(path, l, sizeof l - 1);
    if (n < 0) { dprintf(2, "tar: %s: %s\n", path, zwhy("cannot read the link")); add_rc = 1; return; }
    if (n > 100) { dprintf(2, "tar: %s: link target longer than 100 bytes (the tar header's limit): skipped\n", path); add_rc = 1; return; }
    l[n] = 0;
    write_header(path, st, '2', l, 0);
}
static void __attribute__((noinline)) add_file(const char *path, const struct ustat *st)
{
    int fd = sys_open(path, O_RDONLY); if (fd < 0) { dprintf(2, "tar: %s: %s\n", path, zwhy("cannot open")); add_rc = 1; return; }
    if (write_header(path, st, '0', 0, st->size)) { sys_close(fd); return; }
    char b[512]; u32 left = st->size; int shrank = 0;
    while (left) {                                     /* exactly the size in the header: a short read is padded and said */
        u32 want = left > 512 ? 512 : left, got = 0;
        memset(b, 0, 512);
        while (got < want) { int r = sys_read(fd, b + got, want - got); if (r <= 0) break; got += (u32)r; }
        if (got < want && !shrank) { dprintf(2, "tar: %s: %s: the rest is padded with zeros\n", path, got ? "file shrank" : zwhy("read error or file shrank")); shrank = add_rc = 1; }
        out_block(b);
        left -= want;
    }
    sys_close(fd);
}
static void add(const char *path)
{
    struct ustat st;
    if (sys_lstat(path, &st) < 0) { dprintf(2, "tar: %s: cannot stat\n", path); add_rc = 1; return; }
    u32 t = st.mode & S_IFMT;
    if (verbose) dprintf(2, "%s\n", path);
    if (t == S_IFDIR) {
        u32 pl = strlen(path);
        char *nm = malloc(pl + 2);
        if (!nm) { dprintf(2, "tar: %s: out of memory\n", path); add_rc = 1; return; }
        memcpy(nm, path, pl); nm[pl] = '/'; nm[pl + 1] = 0;
        write_header(nm, &st, '5', 0, 0);
        free(nm);
        char **names; int nn = dir_list(path, &names);        /* heap, not stack: one level per directory */
        if (nn < 0) { dprintf(2, "tar: %s: %s\n", path, zwhy("cannot read")); add_rc = 1; return; }
        for (int i = 0; i < nn; i++) {
            char *c = path_join(path, names[i]);
            if (c) add(c); else { dprintf(2, "tar: %s/%s: %s\n", path, names[i], zwhy("cannot add")); add_rc = 1; }
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

/* A member name made safe to extract (audit D), in place: leading slashes are dropped (the archive lands under the
 * current directory, as GNU tar does), "." parts and doubled slashes go. 0, or -1 for a name with a ".." part, which is
 * refused: it could reach outside the directory. */
static int safe_name(char *name)
{
    char *o = name, *p = name;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        char *e = p;
        while (*e && *e != '/') e++;
        u32 len = (u32)(e - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') return -1;
        if (!(len == 1 && p[0] == '.')) {
            if (o != name) *o++ = '/';
            memmove(o, p, len); o += len;
        }
        p = e;
    }
    *o = 0;
    return 0;
}

/* the symbolic links this archive created: nothing later in it may be written through one of them (a member
 * "x -> /etc" followed by "x/passwd" would otherwise write /etc/passwd) */
static char **links; static u32 nlinks;
static void remember_link(const char *name)
{
    char **n = realloc(links, (nlinks + 1) * sizeof *n);
    char *c = strdup(name);
    if (!n || !c) { free(c); if (n) links = n; return; }
    links = n; links[nlinks++] = c;
}
/* does NAME go through (or is it) a link this archive made? */
static const char *through_link(const char *name)
{
    for (u32 i = 0; i < nlinks; i++) {
        u32 l = strlen(links[i]);
        if (!strncmp(name, links[i], l) && (name[l] == '/' || name[l] == 0)) return links[i];
    }
    return 0;
}

int main(int argc, char **argv)
{
    char mode = 0; const char *file = 0, *dir = 0; int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i]; if (a[0] != '-' && i > 1) break;
        if (a[0] == '-') a++;
        if (!strcmp(argv[i], "-C")) { dir = argv[++i]; continue; }
        for (; *a; a++) { if (*a == 'c' || *a == 'x' || *a == 't') mode = *a; else if (*a == 'v') verbose = 1; else if (*a == 'f') file = argv[++i]; else if (*a == 'C') dir = argv[++i]; else { dprintf(2, "tar: unknown option %c\n", *a); return 2; } }
    }
    if (!mode) { dprintf(2, "usage: tar c|x|t [v] [f archive] [-C dir] [file...]\n"); return 2; }
    if (mode == 'c') {
        if (file && strcmp(file, "-")) { int fd = sys_open(file, O_WRONLY | O_CREAT | O_TRUNC); if (fd < 0) { dprintf(2, "tar: %s: %s\n", file, zwhy("cannot write")); return 1; } sys_dup2(fd, 1); sys_close(fd); }
        if (dir && sys_chdir(dir) < 0) { dprintf(2, "tar: %s: cannot enter\n", dir); return 1; }
        if (i >= argc) { dprintf(2, "tar: nothing to archive\n"); return 2; }
        for (; i < argc; i++) add(argv[i]);
        char z[512]; memset(z, 0, sizeof z); out_block(z); out_block(z);
        return add_rc;
    }
    afd = 0;
    if (file && strcmp(file, "-")) { afd = sys_open(file, O_RDONLY); if (afd < 0) { dprintf(2, "tar: %s: %s\n", file, zwhy("cannot open")); return 1; } }
    if (mode == 'x' && dir && sys_chdir(dir) < 0) { dprintf(2, "tar: %s: cannot enter\n", dir); return 1; }
    unsigned char h[512]; int rc = 0; u32 blockno = 0;
    #define SKIP(nb) do { char sk_[512]; for (u32 b_ = 0; b_ < (nb); b_++) if (rd_full(sk_, 512) != 512) { dprintf(2, "tar: unexpected end of the archive\n"); return 2; } blockno += (nb); } while (0)
    for (;;) {
        int got = rd_full(h, 512);
        if (got == 0) break;                                      /* no end blocks: accepted, as GNU tar does */
        if (got != 512) { dprintf(2, "tar: unexpected end of the archive\n"); return 2; }
        if (!h[0]) break;
        if (get_oct((char *)h + 148, 8) != hsum(h)) {             /* not a header: a damaged archive or not a tar file */
            dprintf(2, "tar: damaged archive: wrong header checksum at block %u\n", blockno);
            return 2;
        }
        blockno++;
        char name[300]; name[0] = 0;
        if (h[345]) { strncpy(name, (char *)h + 345, 155); name[155] = 0; strcat(name, "/"); }
        u32 nl = strlen(name); strncpy(name + nl, (char *)h, 100); name[nl + 100] = 0;
        u32 size = get_oct((char *)h + 124, 12), mode_bits = get_oct((char *)h + 100, 8), mt = get_oct((char *)h + 136, 12); char type = h[156] ? h[156] : '0';
        u32 l = strlen(name); while (l > 1 && name[l - 1] == '/') name[--l] = 0;
        if (mode == 't' || verbose) dprintf(1, "%s%s\n", name, type == '5' ? "/" : "");
        u32 blocks = (size + 511) / 512;
        if (mode == 'x') {
            const char *lk = 0;
            char *orig = strdup(name);
            int skip = 1;
            if (safe_name(name)) { dprintf(2, "tar: %s: refused: a \"..\" part could reach outside the directory\n", orig ? orig : name); rc = 1; }
            else if (!name[0]) ;                                        /* "/" or "." alone: nothing to make */
            else if ((lk = through_link(name))) { dprintf(2, "tar: %s: refused: it goes through the symbolic link %s from this archive\n", name, lk); rc = 1; }
            else skip = 0;
            free(orig);
            if (skip) { SKIP(blocks); continue; }
        }
        if (mode == 't') { SKIP(blocks); continue; }
        if (type == '5') { mkpath(name); sys_mkdir(name); sys_chmod(name, mode_bits); }
        else if (type == '2') { char lk[101]; strncpy(lk, (char *)h + 157, 100); lk[100] = 0; mkpath(name); sys_unlink(name); if (sys_symlink(lk, name) < 0) { dprintf(2, "tar: %s: cannot link\n", name); rc = 1; } else remember_link(name); }
        else if (type == '0') {
            mkpath(name); int fd = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) { dprintf(2, "tar: %s: %s\n", name, zwhy("cannot create")); rc = 1; }
            char b[512]; u32 left = size;
            for (u32 k = 0; k < blocks; k++) {
                if (rd_full(b, 512) != 512) { dprintf(2, "tar: %s: unexpected end of the archive\n", name); if (fd >= 0) sys_close(fd); return 2; }
                blockno++;
                u32 n = left > 512 ? 512 : left;
                if (fd >= 0 && sys_write(fd, b, n) != (int)n) { dprintf(2, "tar: %s: %s\n", name, zwhy("write error")); sys_close(fd); fd = -1; rc = 1; }
                left -= n;
            }
            if (fd >= 0) { sys_close(fd); sys_chmod(name, mode_bits); sys_utimes(name, mt); }
        } else SKIP(blocks);
    }
    return rc;
}
