/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* core - the parts of UP-File that know nothing about the Amiga.
 *
 * Name sorting, file kinds from names, size formatting, header
 * sniffing, the ANSI art parser and the LhA header walker. All of it
 * builds on the host and under vamos too, so tests/coretest.c can
 * prove it both ways before anything boots (the harness-first rule
 * from cdiff: bebbo's -O2 has miscompiled plain C loops before).
 */
#ifndef UP_CORE_H
#define UP_CORE_H

/* file kinds, picked from the name alone (no disk access) */
enum { K_PLAIN, K_ARCH, K_IMG, K_AUD, K_INFO, K_SRC, K_DOC, K_NKINDS };

/* content types, from the first bytes of a file */
enum { TY_OTHER, TY_EXEC, TY_TEXT, TY_ANSI, TY_LHA, TY_LZX, TY_ZIP,
       TY_DMS, TY_IFF, TY_MOD, TY_EMPTY };

/* sprintf without stdio (see core.c) */
int fmt(char *dst, const char *f, ...);

/* natural, case-insensitive: "file9" sorts before "file10" */
int natcmp(const char *a, const char *b);
/* plain case-insensitive compare, Latin-1 aware */
int nocase(const char *a, const char *b);
/* an index (entry numbers) sorted by name, no case, and a name found in it */
void name_sort(int *ix, int n, const char *(*nm)(void *ctx, int i), void *ctx);
int name_find(const int *ix, int n, const char *(*nm)(void *ctx, int i), void *ctx, const char *name);
/* case-insensitive substring test; the empty needle matches */
int nchas(const char *hay, const char *needle);
/* the text after the last '.', or "" */
const char *extof(const char *name);
int kindof(const char *name);
/* "123", "1.2K", "45K", "3.4M", "1.9G" - at most 5 characters */
void fmtsize(char *dst, unsigned long n);
int sniffmem(const unsigned char *buf, long n);

unsigned long be32(const unsigned char *p);
unsigned int be16(const unsigned char *p);
unsigned long le32(const unsigned char *p);
unsigned int le16(const unsigned char *p);

/* ---- ANSI art --------------------------------------------------
 * An attribute byte packs fg (bits 0-2), bg (bits 3-5) and bold
 * (bit 6). ansi_render parses from `off` with attribute `attr` and
 * fills `rows` rows of `w` cells (chars + attributes), wrapping at
 * `artw` columns the way an 80-column terminal does. It returns the
 * number of rows it actually filled with content. ansi_index records
 * where every displayed line starts, and the attribute in force
 * there, so a pager can start drawing at any line. */
#define ANSI_DEFATTR 7
int ansi_render(const unsigned char *buf, long len, long off, int attr,
                unsigned char *cells, unsigned char *attrs,
                int w, int rows, int artw);
/* calls add(ctx, off, attr) for each line start; returns line count */
long ansi_index(const unsigned char *buf, long len, int artw,
                void (*add)(void *ctx, long off, int attr), void *ctx);

/* ---- plain text ------------------------------------------------
 * One displayed row from `off`: tabs to 8-column stops, control
 * characters to '.', CR ignored. With wrap the row ends at w columns
 * and the rest continues on the next row; without, the line is cut.
 * Fills `cells` (padded with spaces to w) and returns the offset of
 * the next row. */
long text_row(const unsigned char *buf, long len, long off,
              unsigned char *cells, int w, int wrap);

/* one hex dump row of `bpr` bytes from `off`, into cells, width w */
void hex_row(const unsigned char *buf, long len, long off, int bpr,
             unsigned char *cells, int w);
int hex_bpr(int w);

/* ---- LhA ---------------------------------------------------------
 * Parse one header at the start of `h` (n bytes valid). Fills name
 * (dir/name, at most namemax-1 chars), sizes, and *next = the
 * offset of the next header relative to this one. Returns 1 for a
 * member, 0 at the end marker or anything that is not a header. */
int lha_header(const unsigned char *h, long n, char *name, int namemax,
               unsigned long *packed, unsigned long *orig,
               unsigned long *next);

#endif
