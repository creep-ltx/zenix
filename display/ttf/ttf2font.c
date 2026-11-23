/* SPDX-License-Identifier: GPL-2.0-only */
/* ttf2font: a TrueType font drawn once at a size into an AmigaOS bitmap font (DESIGN D51, docs/d51-truetype.md;
 * man ttf2font). displayd runs it when the desktop's font is a .ttf whose cache file is missing or stale; by hand it
 * makes fonts for AmigaOS too, or the console's Unicode font (-c).
 *
 *   ttf2font [-q] [-o DIR] [-n NAME] FONT.ttf SIZE...   DIR/NAME/SIZE and DIR/NAME.font (DIR /var/cache/fonts)
 *   ttf2font [-q] -c FONT.ttf [OUT.zcf]                  the console's 8 x 8 font past U+00FF (ZCF1)
 *
 * Every file is written beside its place and renamed over it, so a cut leaves the old file or the new one. */
#include "libz.h"
#include "ttfont.h"

static int quiet;

static void *zalloc(unsigned n) { return malloc(n); }
static void zfree(void *p) { free(p); }

static int fail(const char *what, const char *why)
{
    dprintf(2, "ttf2font: %s: %s\n", what, why);
    return 1;
}

/* the whole file, at most max bytes (zslurpfd, libz) -> its bytes (free them) and *n, *st; or 0 and *why */
static unsigned char *slurp(const char *path, u32 max, u32 *n, struct ustat *st, const char **why)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        *why = fd == -ENOENT ? "not there" : "cannot be opened";
        return 0;
    }
    unsigned char *b = sys_fstat(fd, st) < 0 ? 0 : (unsigned char *)zslurpfd(fd, max, n);
    if (!b)
        *why = zerrno == EFBIG ? "too large" : zerrno == ENOMEM ? "no memory to read it" : "cannot be read";
    sys_close(fd);
    return b;
}

/* path written whole through path.new -> 0, or -errno */
static int put(const char *path, const void *d, u32 n)
{
    char *t = malloc(strlen(path) + 5);
    if (!t)
        return -ENOMEM;
    strcpy(t, path);
    strcat(t, ".new");
    sys_unlink(t);
    int fd = sys_open(t, O_WRONLY | O_CREAT | O_EXCL), r = fd;
    if (fd >= 0) {
        r = sys_write(fd, d, n);
        sys_close(fd);
        r = r == (int)n ? sys_rename(t, path) : r < 0 ? r : -ENOSPC;
        if (r < 0)
            sys_unlink(t);
    }
    free(t);
    return r < 0 ? r : 0;
}

static const char *errtext(int e)
{
    return e == -ENOSPC ? "the disk is full" : e == -EACCES || e == -EPERM ? "not allowed to write there"
         : e == -ENOENT ? "no such directory" : e == -EROFS ? "a read-only disk" : "cannot be written";
}

/* the folders of path made, as mkdir -p (an existing one is fine) */
static void mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            sys_mkdir(path);
            *p = '/';
        }
    sys_mkdir(path);
}

static const char USAGE[] = "usage: ttf2font [-q] [-o DIR] [-n NAME] FONT.ttf SIZE...\n"
                            "       ttf2font [-q] -c FONT.ttf [OUT.zcf]\n";
static int usage(void) { zusage(USAGE); }
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *dir = "/var/cache/fonts", *name = 0;
    int console = 0, i = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        if (!strcmp(argv[i], "-q"))
            quiet = 1;
        else if (!strcmp(argv[i], "-c"))
            console = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            dir = argv[++i];
        else if (!strcmp(argv[i], "-n") && i + 1 < argc)
            name = argv[++i];
        else
            zbadopt(argv[i], USAGE);
    }
    if (i >= argc || (!console && i + 1 >= argc) || (console && i + 2 < argc))
        return usage();
    const char *src = argv[i++];
    /* the font's name: -n, else the file's name without its folder and ".ttf" */
    char nm[32];
    const char *b = strrchr(src, '/');
    b = name ? name : b ? b + 1 : src;
    u32 nl = strlen(b);
    if (!name && nl > 4 && (!strcmp(b + nl - 4, ".ttf") || !strcmp(b + nl - 4, ".TTF")))
        nl -= 4;
    if (!nl || nl > 25)
        return fail(src, "its name must be 1-25 bytes (-n NAME)");
    memcpy(nm, b, nl);
    nm[nl] = 0;
    for (u32 k = 0; k < nl; k++)
        if ((unsigned char)nm[k] <= ' ' || (unsigned char)nm[k] > 0x7E || nm[k] == '/' || nm[k] == ':')
            return fail(nm, "a font's name has no spaces, slashes or colons (-n NAME)");
    if (!strcmp(nm, ".") || !strcmp(nm, ".."))
        return fail(nm, "not a font's name");

    u32 n;
    struct ustat st;
    const char *why;
    unsigned char *d = slurp(src, TTF_MAXFILE, &n, &st, &why);
    if (!d)
        return fail(src, why);
    struct ttf t;
    if ((why = ttf_open(&t, d, n)))
        return fail(src, why);

    if (console) {
        char out[96];
        if (i < argc)
            strncpy(out, argv[i], sizeof out - 1), out[sizeof out - 1] = 0;
        else
            snprintf(out, sizeof out, "/usr/share/consolefonts/%s.zcf", nm);
        unsigned char *z;
        u32 zn;
        int ng;
        u32 t0 = sys_ticks();
        if ((why = ttf_zcf(&t, zalloc, zfree, &z, &zn, &ng)))
            return fail(src, why);
        int r = put(out, z, zn);
        if (r < 0)
            return fail(out, errtext(r));
        if (!quiet)
            dprintf(1, "ttf2font: %s: %d glyphs, %u bytes, %u.%02u s\n", out, ng, zn, (sys_ticks() - t0) / 100,
                    (sys_ticks() - t0) % 100);
        return 0;
    }

    int bad = 0;
    for (; i < argc; i++) {
        char *e;
        unsigned long size = strtoul(argv[i], &e, 10);
        if (*e || !argv[i][0] || size < 4 || size > TTF_MAXH) {
            bad = fail(argv[i], "a size of 4-64 lines is needed");
            continue;
        }
        /* the place first: a disk that cannot take the file costs no drawing (the review: a read-only root would
         * have cost the whole drawing at every start of the desktop) */
        char *path = malloc(strlen(dir) + 40);
        if (!path)
            return fail(src, "no memory");
        snprintf(path, strlen(dir) + 40, "%s/%s", dir, nm);
        mkdirs(path);
        snprintf(path, strlen(dir) + 40, "%s/%s/.w", dir, nm);
        sys_unlink(path);
        int pr = sys_open(path, O_WRONLY | O_CREAT | O_EXCL);
        if (pr < 0) {
            path[strlen(path) - 3] = 0;
            bad = fail(path, errtext(pr));
            free(path);
            continue;
        }
        sys_close(pr);
        sys_unlink(path);
        u32 t0 = sys_ticks();
        unsigned char *f;
        u32 fl;
        struct ttf_size how;
        if ((why = ttf_amifont(&t, (int)size, TTF_XR, nm, st.size, st.mtime, zalloc, zfree, &f, &fl, &how))) {
            bad = fail(src, why);
            free(path);
            continue;
        }
        u32 ticks = sys_ticks() - t0;
        snprintf(path, strlen(dir) + 40, "%s/%s/%lu", dir, nm, size);
        int r = put(path, f, fl);
        free(f);
        if (r < 0) {
            bad = fail(path, errtext(r));
            free(path);
            continue;
        }
        /* NAME.font: the sizes there and this one */
        snprintf(path, strlen(dir) + 40, "%s/%s.font", dir, nm);
        u32 on = 0;
        struct ustat cs;
        const char *cw;
        unsigned char *old = slurp(path, 4 + 260 * 64, &on, &cs, &cw);
        unsigned char *c = malloc(4 + 260 * 64);
        u32 cn = c ? ttf_contents(old, old ? on : 0, nm, (int)size, how.prop, c) : 0;
        free(old);
        r = !c ? -ENOMEM : cn ? put(path, c, cn) : 0;
        free(c);
        if (r < 0 || !cn)
            bad = fail(path, r < 0 ? errtext(r) : "it lists 64 sizes already: this one is not added");
        else if (!quiet)
            dprintf(1, "ttf2font: %s/%s/%lu: %d glyphs, %d lines (baseline %d), %s, %u.%02u s\n", dir, nm, size,
                    how.nglyphs, how.h, how.base, how.prop ? "proportional" : "fixed width", ticks / 100,
                    ticks % 100);
        if (how.wide)                           /* said even with -q: a limit hit is never silent */
            dprintf(2, "ttf2font: %s %lu: %d glyphs wider than 64 pixels or 255 across left out (the 8x8 stands in)\n", nm, size,
                    how.wide);
        free(path);
    }
    free(d);
    return bad;
}
