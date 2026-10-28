/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The package formats of DESIGN D15 (frozen, they only grow), read in one place (DESIGN D22 4.2, "shared parsers"):
 * the manifest ("zpkg 1" text: name, version, group, depends, one line per file, link and directory) and the bundle
 * (ZUP1: a 96-byte header, a table of contents with a SHA-256 per part, the parts). zup (user/zup.c), zpkg and the
 * host tests (tools/zup-host-test.sh, tools/host/zpkgfmt-fuzz.c) link user/zpkgfmt.c. Plain C types only: it builds
 * for Zenix (libz) and for the PC (-DZHOST).
 * A refusal says its reason through zf_say(), which the program provides (zup: "zup: REASON" on its console). */

#define ZF_MAXPKG   16                  /* packages in one bundle */
#define ZF_MAXDEP   8                   /* dependencies of one package */
#define ZF_MAXTAKES 8                   /* takes lines of one package (DESIGN D22 8.4.4) */
#define ZF_PMAX     64                  /* every path given to a system call is at most 63 bytes */
#define ZF_HDR      96                  /* the bundle header */
#define ZF_TOCE     80                  /* one table-of-contents entry: name[40], offset, size, SHA-256 */
#define ZF_MAXTOC   64
#define ZF_MAXENT   2048                /* entries of one manifest (a limit that is hit is said) */
#define ZF_MANMAX   (256u << 10)        /* a manifest's bytes */

struct zf_ent {                         /* one f, c, d or l line */
    char kind;
    unsigned short mode, uid, gid;
    unsigned char sha[32];
    unsigned size;
    char *path, *target;
};
struct zf_pkg {
    char *name, *vstr, *group, *text;
    unsigned vnum, textlen;
    int ndep; char *dep[ZF_MAXDEP]; unsigned depv[ZF_MAXDEP];
    int ntakes; char *takes[ZF_MAXTAKES];  /* takes OTHER (D22 8.4.4: its files become this package's) */
    int nent; struct zf_ent *ent;
    const unsigned char *tgz; unsigned tgzlen;      /* the parts, in the bundle (zf_bundle_parse) */
    const unsigned char *man; unsigned manlen;
};
struct zf_bundle {
    const unsigned char *p;             /* the header and table of contents (and, from zf_bundle_parse, every part) */
    unsigned len, vnum, minv, ntoc;
    char vstr[17], channel[9];
    int npkg; struct zf_pkg pk[ZF_MAXPKG];
    char pname[ZF_MAXPKG][25];          /* each package's name from its part names */
};

void zf_say(const char *fmt, ...);      /* the program's own */

int zf_path_ok(const char *p);          /* absolute, no //, no . or .. part, [A-Za-z0-9._+/-], at most 55 bytes */
int zf_name_ok(const char *p);          /* a package name: [a-z0-9][a-z0-9+.-]*, at most 24 */
int zf_parse_sha(const char *s, unsigned char out[32]);     /* 64 lowercase hex characters and the end: 0, else -1 */
void zf_put_sha(char *out, const unsigned char h[32]);      /* 64 characters and a NUL */
unsigned zf_num(const char *s, int base, int *bad);
int zf_target_ok(const char *t);        /* a link target: [A-Za-z0-9._+/-], 1-54 bytes, not "-" */
int zf_reserved(const char *p);         /* ends in .zup-new, .new or .save: zup's own names, never an entry */
/* text: a manifest (malloc'd, NUL-terminated; kept by the pkg, which points into it). 0, or -EINVAL / -ENOMEM with
 * the reason said; where names it in the messages */
int zf_manifest_parse(struct zf_pkg *p, char *text, unsigned len, const char *where);
struct zf_ent *zf_find_ent(struct zf_pkg *p, const char *path);
void zf_pkg_free(struct zf_pkg *p);     /* its entries and its text */

/* the header and table of contents, the first ZF_HDR + ntoc * ZF_TOCE bytes (avail of them at p) of a bundle of len
 * bytes: magic, format, sizes, the table's own SHA-256. 0, or -EINVAL with the reason said. Fills b->vnum, minv,
 * vstr, channel, ntoc (b->p = p, b->len = len). Returns the bytes it needs in *need when avail is short (-EAGAIN) */
int zf_bundle_head(struct zf_bundle *b, const unsigned char *p, unsigned avail, unsigned len, unsigned *need);
/* entry i of the table (after zf_bundle_head): its name (NUL-terminated copy, 41 bytes), offset, size and SHA-256;
 * 0, or -EINVAL (not terminated, outside the bundle) with the reason said */
int zf_bundle_entry(const struct zf_bundle *b, unsigned i, char name[41], unsigned *off, unsigned *size,
                    const unsigned char **sha);
/* a pkg/ part's name: "pkg/NAME.manifest" -> 1, ".tgz" -> 2, any other pkg/ name -EINVAL (said), not pkg/ 0;
 * the package name into pname (25 bytes) */
int zf_part_kind(const char *name, char pname[25]);
/* a whole bundle in memory (zenixboot's hand-over): head, every part's SHA-256, every package's manifest parsed. The
 * parts are read in place: never pass them to a system call (they are no process's memory) */
int zf_bundle_parse(struct zf_bundle *b, const unsigned char *p, unsigned len);
void zf_bundle_free(struct zf_bundle *b);       /* every package's entries and text (also after a refusal) */
