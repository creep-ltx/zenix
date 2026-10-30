/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* arcw - changing lha, lzx and zip archives through the Amiga's own
 * archivers (C:lha, C:lzx, C:zip), every change a checked commit, and
 * the u (unpack) and p (pack) commands. */
#ifndef CF_ARCW_H
#define CF_ARCW_H

#include "place.h"
#include "ops.h"

int arcw_canwrite(struct place *p);       /* pane p shows an lha, lzx or zip archive */
/* in a place, the names list: 0 none, 1 a file, 2 a directory */
int arcw_has(struct place *p, const char *name);
int arcw_putfile(struct place *q, const char *src, const char *name);   /* one file in, checked */

/* copy (or move) entries idx[] of place p into the archive place q under
 * the names tn[]; returns 1 when the archive took them all */
int arcw_add(struct place *p, struct place *q, const int *idx, char (*tn)[NAMEMAX + 4], int n, int ismove);
void arcw_delete(struct place *p, int cur);     /* the marked set, or entry cur */
int arcw_delents(struct place *p, const int *idx, int n); /* no question: a move's second half */
void arcw_rename(struct place *p, const char *from, const char *to);
void arcw_mkdir(struct place *p, const char *name);

/* u and p: the marked set of p (else entry cur) unpacked into, or
 * packed into a new archive in, the folder q */
void arcw_unpack(struct place *p, struct place *q, int cur);
void arcw_pack(struct place *p, struct place *q, int cur);

#endif
