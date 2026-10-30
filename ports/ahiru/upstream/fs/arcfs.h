/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* arcfs - archives and disk images browsed like directories.
 *
 * A place inside an archive keeps the archive in its path
 * ("Work:x/foo.lha/docs"); p->arc is the length of the archive file's
 * own path in it, p->as its state. Here p and q are places. ADF images
 * are written in place; lha/lzx/zip writing is arcw.c.
 */
#ifndef CF_ARCFS_H
#define CF_ARCFS_H

#include <exec/types.h>
#include "place.h"

#define AK_NONE 0
#define AK_LHA 1
#define AK_LZX 2
#define AK_ZIP 3
#define AK_ADF 4
#define AK_ISO 5
#define AK_DMS 6

int arcfs_kind(const char *path);              /* AK_*, from the bytes */
const char *arcfs_kindname(struct place *p);              /* "lha", "ADF" ... for the border */

/* Enter on a file: 1 = handled (entered, or a DMS unpacked), 0 = not an archive */
int arcfs_open(struct place *p, const char *file);
void arcfs_leave(struct place *p);
int arcfs_read(struct place *p);                          /* fill the pane: 0 ok, else ARC_E* */

/* copy one entry of pane p (a file or a whole directory) to dst */
int arcfs_copyout(struct place *p, const char *name, const char *dst, int isdir);
ULONG arcfs_treesize(struct place *p, const char *name);  /* the bytes under a directory */
/* a member in a scratch file to view: 1 = made (tmp filled) */
int arcfs_temp(struct place *p, const char *name, char *tmp);

/* for the writer and for u (a place of its own, not shown) */
int arcfs_load(struct place *p, const char *file);       /* 0 ok, else ARC_E* */
int arcfs_reload(struct place *p);                        /* after the archive changed */
void arcfs_reloadsame(struct place *p);                   /* p, and the other pane on the same archive */
int arcfs_damaged(struct place *p);
int arcfs_hold(struct place *p);                          /* one session for many copyouts */
void arcfs_release(struct place *p);                       /* listed only in part: not written to */
const char *arcfs_file(struct place *p);
int arcfs_kindof(struct place *p);
const char *arcfs_sub(struct place *p);                   /* where the pane is inside ("" = top) */
int arcfs_extractall(struct place *p, const char *dst);   /* 1 ok; opmsg says why not */
int arcfs_nmem(struct place *p);
const char *arcfs_mem(struct place *p, int i, ULONG *size, int *isdir);
struct arcent;
int arcfs_listfile(const char *file, int kind, int (*add)(void *ctx, const struct arcent *e), void *ctx);

/* ADF images are written in place by core/adfw.c */
int arcfs_isadf(struct place *p);
LONG arcfs_free(struct place *p);                         /* free bytes in an image pane, -1 unknown */
int arcfs_adfput(struct place *p, struct place *q, const int *idx, char (*tn)[107 + 4], int n, int ismove);
int arcfs_adfdelone(struct place *p, const char *name, int isdir);
void arcfs_adfdelete(struct place *p, int cur);   /* the marked, else entry cur */
void arcfs_adfrename(struct place *p, const char *from, const char *to);
void arcfs_adfmkdir(struct place *p, const char *name);
int arcfs_newadf(const char *path);             /* a blank disk image; opmsg says why not */

/* refuse a write inside an archive pane (1 = refused, message shown) */
int arcfs_guard(struct place *p);
int arcfs_kind_rel(struct place *p, const char *name);    /* by name: an archive inside one */
int arcfs_adfputfile(struct place *q, const char *src, const char *name);
int arcfs_scratch(char *dst);                    /* T:CFileE<task>, or RAM: */

#endif
