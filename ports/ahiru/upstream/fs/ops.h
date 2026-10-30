/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ops - copy, move, delete, rename, new, measure, info. Every rule
 * the E CFile's October audit made (0.5b52) holds here:
 *  - an existing file is never emptied first: the new bytes go to a
 *    scratch name beside it and replace it only when complete
 *  - a link is a link: delete removes the link itself, walks never
 *    enter one, a soft link inside a copied tree is left out (counted)
 *  - a move deletes its source only after the copy is whole
 *  - a path that does not fit refuses; it never acts on the parent
 *  - Esc stops copy, move, delete and measure between chunks
 */
#ifndef CF_OPS_H
#define CF_OPS_H

#include "place.h"
#include "shared.h"

#define NAMEMAX 107


/* p and q are places; cur is the entry under the cursor (-1 none): the
 * marked set when there is one, else cur. Each asks what it must,
 * reports through fe.h and has the places read again. */
void fs_copy(struct place *p, struct place *q, int cur, int ismove, int force);
void fs_delete(struct place *p, int cur);
void fs_rename(struct place *p, int cur);
/* n: asks the name; makes a folder or a blank .adf itself. A new file
 * is the program's to edit: FS_NEWFILE (path filled) or, inside an
 * archive, FS_NEWMEMBER (name filled). 0 = done or refused */
#define FS_NEWFILE 1
#define FS_NEWMEMBER 2
int fs_new(struct place *p, char *name, char *path);
int fs_mkdir(struct place *p, const char *name);
int fs_renameone(struct place *p, int i, const char *to);   /* a folder on a disk: no question */   /* a folder, in an archive too; 1 = made */
int fs_measure(struct place *p, int i);         /* '=': 1 = ent[i].size is the tree's */
void faultmsg(const char *what);                /* what: the DOS error, as a message */

/* shared with the viewer and the editor */
#include <dos/dos.h>
int buildfull(char *dst, const char *dir, const char *name);
void sidename(char *dst, const char *path, int tag);
int swapin(const char *tmp, const char *path, int keep);
BPTR safeopen(const char *path, char *tmp, int *swap);
const char *linktarget(const char *path, char *buf);   /* through a soft link (buf CPATH) */
int safeclose(BPTR fh, const char *path, const char *tmp, int swap, int ok, int keep);
int sniffpath(const char *path);
struct verinfo;
void fileversion(const char *path, struct verinfo *v);   /* $VER, or a RomTag's */
LONG filedays(const char *path);

/* the copy and delete machinery, for the archive writer (arcw.c) */
int isinfo(const char *name);
int sidecarof(const char *dir, const char *name, char *dst);
int hasinfo(struct place *p, const char *name);
int cbuf_get(void);
void cbuf_free(void);
int copyfile(const char *src, const char *dst);
int copytree(const char *src, const char *dst, int depth);
void treestat(const char *path, int depth);
void op_begin(void);
int zap(const char *path, int ask);
int deltree(const char *path, int depth, int tick);
void wipetree(const char *path);

int insidedir(const char *a, const char *b);   /* b is a, or inside it */

#endif
