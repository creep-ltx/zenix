/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* iso - ISO 9660 CD images, read-only, browsed one directory at a time.
 *
 * Long names come from Rock Ridge (NM entries, also in a continuation
 * area) when the image has them,
 * else from the Joliet tree, else the plain ISO names without ";1".
 * No member cache: a CD image can be bigger than the Amiga's memory,
 * and each listing reads only its own directory's sectors.
 */
#ifndef CF_ISO_H
#define CF_ISO_H

#include "arc.h"

struct isovol {
    struct arcsrc *src;
    unsigned long root, rootsz;     /* the root directory's extent and bytes */
    int joliet;                     /* names are UCS-2 (the Joliet tree) */
    int rr;                         /* Rock Ridge: names from NM entries */
    unsigned char *buf;             /* one sector */
    unsigned char *cebuf;           /* a Rock Ridge continuation area's sector */
    char volname[34];
};

/* 0 ok; ARC_EUNSUP when it is not an ISO 9660 image */
int iso_open(struct arcsrc *s, struct isovol *v);
void iso_close(struct isovol *v);

/* one add() per entry of dirpath ("" = root, "a/b" deeper); names only.
 * dataoff = the extent (sector), size = bytes; dirs report size 0. */
int iso_list(struct isovol *v, const char *dirpath, arc_addfn add, void *ctx);

/* a file's bytes, from an entry iso_list gave */
int iso_extract(struct isovol *v, const struct arcent *e, struct arcsink *out);

/* the entry at path (for a file named on the command line or a deep dir) */
int iso_find(struct isovol *v, const char *path, struct arcent *e);

#endif
