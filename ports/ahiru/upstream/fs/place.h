/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* place - what a pane (or a column) shows: a folder, the volume list,
 * or a folder inside an archive or disk image. The core reads it,
 * sorts it and works on its entries; the program only draws it.
 * (docs/core.md) */
#ifndef CF_PLACE_H
#define CF_PLACE_H

#include <exec/types.h>

#define CPATH 300               /* longest path a place holds */

/* the sort tier is the kind: higher sorts first */
#define KD_FILE 0
#define KD_ASSIGN 1
#define KD_DIR 255              /* directories and volumes */

struct ent {
    char *name;
    ULONG size;
    LONG date;                  /* days * 1440 + minutes, 0 = none */
    UBYTE kind;
    UBYTE mark;
    UBYTE link;                 /* a hard or soft link: deleted as a link, never walked */
    ULONG prot;                 /* the protection bits (hsparwed), 0 when not known */
};

struct pool;                    /* name storage, freed per read */
struct ast;                     /* an archive's state (arcfs.c) */

struct place {
    char path[CPATH];           /* "" = the volume list */
    struct ent *ent;            /* every entry, sorted */
    int n, cap;
    int fail;                   /* the directory could not be read */
    int arc;                    /* inside an archive: its file's path length in path */
    struct ast *as;             /* that archive, while arc */
    LONG freeb;                 /* free bytes, asked once per read */
    int freeok;
    struct pool *names;
};

#define pl_involume(P) ((P)->path[0] == 0)
#define pl_inarc(P) ((P)->arc != 0)

/* read it again (a folder, the volume list, or inside an archive);
 * the entries come sorted by mode: PS_NAME, PS_SIZE, PS_DATE or
 * PS_EXT, | PS_NATURAL for "file9" before "file10" */
#define PS_NAME 0
#define PS_SIZE 1
#define PS_DATE 2
#define PS_EXT 3
#define PS_NATURAL 16
void place_read(struct place *P, int mode, int rev);
void place_sort(struct place *P, int mode, int rev);
/* show path: a folder, "" (the volume list), or a folder inside an
 * archive or disk image, which is opened on the way. Not read yet.
 * 0 = no such place */
int place_go(struct place *P, const char *path);
void place_free(struct place *P);       /* its entries and its archive */
int place_add(struct place *P, const char *name, int kind, ULONG size, LONG date);
int place_find(struct place *P, const char *name);     /* the entry, -1 none */
/* for many lookups: an index by name (xfree it; NULL no memory), and
 * place_find through it - halving, not every entry */
int *place_index(struct place *P);
int place_findix(struct place *P, const int *ix, const char *name);
int place_marks(struct place *P);
/* the entries an operation works on: the marked ones (an icon riding
 * along with its marked file is left out: it travels with it), else
 * cur. n returned; idx is xalloc'ed (NULL when n is 0) */
int place_pick(struct place *P, int cur, int **idx);

char *pool_str(struct pool **pp, const char *s);
void pool_free(struct pool **pp);

#endif
