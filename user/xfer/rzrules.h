/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rzrules - where a download goes (DESIGN D20 5, "Where files go"): Zenix's side of the Zmodem engine's I/O table,
 * kept out of the engine. Plain C without system calls, so the PC tests run the same rules: the caller gives a
 * look() that says whether a path exists and its size, and does the opening, writing and renaming itself.
 *  - The name is cleaned: the engine already took off any drawers (`/`, `:`, `\`); a control character becomes `_`;
 *    a name that begins with `.` (`.profile`, `..`) or is empty is refused: a board never writes a hidden file.
 *  - A file is written as `NAME.part` and renamed to NAME when whole.
 *  - An existing NAME is never overwritten: the same size is skipped (ZSKIP); otherwise NAME.1, NAME.2 ... are tried.
 *  - A `NAME.part` shorter than the offered file is a resume candidate: gone on from only after ZCRC says it is the
 *    same file (XferIO wresume); else the part is kept and the file starts under the next free name.
 */
#ifndef RZRULES_H
#define RZRULES_H

/* The longest cleaned name: room left for ".999.part" in a 255-byte file name. */
#define RZ_NAMEMAX 246
#define RZ_MAXN    999                  /* NAME.1 .. NAME.999 */

/* look(u, path, &size): 1 the path exists (*size its size), 0 it does not. */
typedef int (*rz_look)(void *u, const char *path, unsigned long *size);

/* Clean an offered name into out (outlen >= RZ_NAMEMAX + 1). 0: done; -1: refused (empty, or begins with '.'). */
int rz_clean(const char *offered, char *out, int outlen);

/* rz_choose's answers */
#define RZ_SKIP   1     /* target exists with the offered size: the same file is here (ZSKIP) */
#define RZ_FRESH  2     /* write target + ".part" from the start (an empty part there may be replaced) */
#define RZ_RESUME 3     /* target + ".part" holds *partlen bytes, fewer than offered: ask ZCRC before going on */
#define RZ_FULL   4     /* no free name up to NAME.999: refuse */
/* flags */
#define RZF_NORESUME 1  /* a part is never a candidate (after a ZCRC that said no): only free names */
#define RZF_NOSKIP   2  /* an existing file of the same size is not "the same file" (the rename at the end) */

/* Choose where the cleaned name goes, trying NAME (index 0), then NAME.1 ... from index `from`. target gets the
 * chosen name (tlen >= RZ_NAMEMAX + 5), *index its number, so a later call can go on from *index + 1. */
int rz_choose(const char *name, unsigned long size, int from, int flags, rz_look look, void *u,
              char *target, int tlen, unsigned long *partlen, int *index);

/* target + ".part" into out (outlen >= RZ_NAMEMAX + 10). */
void rz_partname(const char *target, char *out, int outlen);

/* ---- the file side of the engine's XferIO for downloads, over the system's file calls (rz on Zenix, stdio files
 * in the PC tests): rz's wopen, wresume, wwrite and wclose call these with their own state ---- */

#define RZ_OPEN_NEW    1                /* create, or empty an existing (part) file */
#define RZ_OPEN_APPEND 2                /* write after what is there */
#define RZ_OPEN_READ   3

/* what is said to the person (say's what) */
#define RZ_SAY_REFUSED 1                /* name: not taken (empty, or begins with '.') */
#define RZ_SAY_SAME    2                /* other: the same file (name, size) is here already: skipped */
#define RZ_SAY_OTHER   3                /* name exists and is another file: saved as other */
#define RZ_SAY_RESUME  4                /* other (the part) is the same file: gone on from */
#define RZ_SAY_NOTSAME 5                /* other (the part) is not the same file, or the sender could not say: kept, the
                                         * download starts again as name */
#define RZ_SAY_FULL    6                /* name: no free name up to .999 */
#define RZ_SAY_KEPT    7                /* other: the download ended early; what came is kept there */
#define RZ_SAY_RENAMED 8                /* name appeared while it came: the file is other */
#define RZ_SAY_NOOPEN  9                /* other: could not be opened (or renamed) */

struct rz_fs {
    void *u;
    rz_look look;
    void *(*open)(void *u, const char *path, int how);     /* NULL: failed */
    long (*read)(void *u, void *h, unsigned char *b, long n);
    long (*write)(void *u, void *h, const unsigned char *b, long n);
    void (*close)(void *u, void *h);
    int (*rename)(void *u, const char *from, const char *to);    /* 0: done (never over an existing file) */
    void (*remove)(void *u, const char *path);
    void (*say)(void *u, int what, const char *name, const char *other);    /* may be NULL */
};

/* One download at a time. */
struct rz_dl {
    const struct rz_fs *fs;
    char name[RZ_NAMEMAX + 1];          /* the cleaned name */
    char target[RZ_NAMEMAX + 8];        /* name or name.N */
    char part[RZ_NAMEMAX + 16];         /* target.part */
    unsigned long size, written;
    int index;
};

void *rz_wopen(struct rz_dl *d, const char *name, unsigned long size, char *made, int madelen, unsigned long *have);
void *rz_wresume(struct rz_dl *d, void *f, int answered, unsigned long crc, char *made, int madelen,
                 unsigned long *have);
long rz_wwrite(struct rz_dl *d, void *f, const unsigned char *b, long n);
void rz_wclose(struct rz_dl *d, void *f, int ok);

#endif
