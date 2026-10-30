/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* find - the walk behind "find": names, or text inside the files, from
 * a folder down (see find.c). The program shows the hits its own way:
 * CFile in a list, Ahiru as a folder of its own. */
#ifndef CF_FIND_H_CORE
#define CF_FIND_H_CORE

#include "os.h"

#define FIND_NAMES 0            /* names holding what, or matching it (#? *) */
#define FIND_LINES 1            /* every line of a text file holding what */
#define FIND_FILES 2            /* every text file holding what, once */

struct findhit {
    const char *full;           /* the whole path */
    const char *rel;            /* from the root: "sub/name" */
    const struct dent *e;       /* its directory entry */
    unsigned long line;         /* FIND_LINES: the line number, and */
    const char *text;           /* the line (spaces in front taken off) */
};

/* 0 = stop the walk */
typedef int (*find_fn)(void *ctx, const struct findhit *h);

/* walks root (folders on disks; soft links are not followed). Esc stops
 * it (cancelok is set while it runs) and progress goes to showmsg.
 * 1 = walked to the end, 0 = stopped (Esc, or fn said so), -1 = what
 * is a pattern AmigaDOS does not understand, -2 = no memory.
 * *seen counts what was looked at. */
int fs_find(const char *root, const char *what, int mode, find_fn fn, void *ctx, unsigned long *seen);

#endif
